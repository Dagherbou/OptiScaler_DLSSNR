#include "pch.h"

#include "DlssNr_ExposureScan.h"

#include <Config.h>
#include <Util.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace DlssNr
{
namespace ExposureScan
{
namespace
{

// How many candidates are worth keeping. The shape being looked for is rare -- in a frame's worth of
// unordered access views a game creates hundreds, and a handful are this small -- so a low cap is
// not a compromise, it is a statement that finding twenty means the filter is wrong.
constexpr size_t kMaxCandidates = 12;

// Ring depth for the readbacks. Four, so the slot being read is four frames behind the slot being
// written and the read never waits on the GPU. Same depth and the same reason as the meter's.
constexpr unsigned int kSlots = 4;

// Every candidate's value lands in one buffer, at its own offset, so there is one copy per candidate
// but only one buffer per slot. 16 bytes each is enough for the widest format worth reading.
constexpr unsigned int kStride = 16;

struct Tracked
{
    ID3D12Resource* resource = nullptr;
    std::string shape;
    bool isBuffer = false;
    unsigned int bytes = 4;

    float latest = 0.0f;
    float lowest = 0.0f;
    float highest = 0.0f;
    unsigned int reads = 0;
    bool moves = false;
};

struct ScanState
{
    std::vector<Tracked> tracked;
    ID3D12Resource* readback[kSlots] = {};
    unsigned long long frames = 0;
    const char* status = "not started";
    bool complained = false;
};

ScanState g_scan;
std::mutex g_scanMutex;

// Formats an exposure could plausibly be in: floating point, one or two channels.
//
// Two channels because eye adaptation commonly carries the value and something alongside it -- the
// previous frame's value, or a target it is easing toward. Anything wider is a picture rather than a
// number. Integer formats are excluded because an exposure is a scale and a normalised integer
// cannot hold one.
bool PlausibleFormat(DXGI_FORMAT f, unsigned int* outBytes, const char** outName)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_FLOAT:
        *outBytes = 4;
        *outName = "R32_FLOAT";
        return true;
    case DXGI_FORMAT_R16_FLOAT:
        *outBytes = 2;
        *outName = "R16_FLOAT";
        return true;
    case DXGI_FORMAT_R32G32_FLOAT:
        *outBytes = 8;
        *outName = "R32G32_FLOAT";
        return true;
    case DXGI_FORMAT_R16G16_FLOAT:
        *outBytes = 4;
        *outName = "R16G16_FLOAT";
        return true;
    default:
        return false;
    }
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1Fu;
    uint32_t mantissa = h & 0x3FFu;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            const uint32_t bits = sign;
            float out;
            std::memcpy(&out, &bits, sizeof(out));
            return out;
        }

        // Subnormal: normalise it by hand.
        exponent = 1;

        while ((mantissa & 0x400u) == 0)
        {
            mantissa <<= 1;
            --exponent;
        }

        mantissa &= 0x3FFu;
    }
    else if (exponent == 31)
    {
        const uint32_t bits = sign | 0x7F800000u | (mantissa << 13);
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    const uint32_t bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (res == nullptr || from == to)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = res;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &barrier);
}

bool EnsureReadback(ID3D12Device* device)
{
    for (unsigned int i = 0; i < kSlots; ++i)
    {
        if (g_scan.readback[i] != nullptr)
            continue;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kStride * kMaxCandidates;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&g_scan.readback[i]))))
        {
            g_scan.status = "could not allocate the readback buffers";
            return false;
        }
    }

    return true;
}

} // namespace

void NoteUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)
{
    if (!Config::Instance()->DlssNrScanExposure.value_or_default())
        return;

    if (resource == nullptr)
        return;

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();

    unsigned int bytes = 4;
    const char* formatName = "";
    std::string shape;
    bool isBuffer = false;

    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        // Small enough to be a number rather than a picture. 4x4 rather than strictly 1x1 because
        // some engines keep a couple of extra values beside the exposure -- the previous frame's, or
        // a target being eased toward -- and a few keep a tiny histogram next to it.
        if (rd.Width > 4 || rd.Height > 4)
            return;

        if (!PlausibleFormat(rd.Format, &bytes, &formatName))
            return;

        shape = std::to_string((unsigned int) rd.Width) + "x" + std::to_string(rd.Height) + " " + formatName;
    }
    else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        // Unreal moved eye adaptation from a texture to a buffer, so buffers have to be in scope or
        // a whole engine's worth of games is invisible to this. Same size argument: a buffer holding
        // an exposure is a handful of floats, not a screenful.
        if (rd.Width == 0 || rd.Width > 64)
            return;

        if (desc == nullptr || desc->ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
            return;

        isBuffer = true;
        bytes = 4;
        shape = "buffer, " + std::to_string((unsigned int) rd.Width) + " bytes";
    }
    else
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_scanMutex);

    for (const Tracked& t : g_scan.tracked)
    {
        if (t.resource == resource)
            return;
    }

    if (g_scan.tracked.size() >= kMaxCandidates)
    {
        if (!g_scan.complained)
        {
            g_scan.complained = true;
            LOG_WARN("DLSS-NR exposure scan: more than {} candidates, which means the filter is too "
                     "loose to be useful here rather than that the game has {} exposures",
                     kMaxCandidates, kMaxCandidates);
        }

        return;
    }

    Tracked t;
    t.resource = resource;
    t.shape = shape;
    t.isBuffer = isBuffer;
    t.bytes = bytes;
    resource->AddRef();

    g_scan.tracked.push_back(t);

    LOG_INFO("DLSS-NR exposure scan: candidate {} -- {}", g_scan.tracked.size(), shape);
}

void Tick(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
    if (!Config::Instance()->DlssNrScanExposure.value_or_default())
        return;

    if (device == nullptr || cmdList == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_scan.tracked.empty())
    {
        g_scan.status = "nothing matched yet -- play for a few seconds, the buffer is created lazily "
                        "by some engines";
        return;
    }

    if (!EnsureReadback(device))
        return;

    // Read the slot written four frames ago before overwriting it. Retired by now, so this reads
    // mapped memory rather than waiting on the GPU.
    if (g_scan.frames >= kSlots)
    {
        ID3D12Resource* old = g_scan.readback[g_scan.frames % kSlots];
        void* mapped = nullptr;
        D3D12_RANGE range { 0, kStride * kMaxCandidates };

        if (old != nullptr && SUCCEEDED(old->Map(0, &range, &mapped)) && mapped != nullptr)
        {
            const unsigned char* base = (const unsigned char*) mapped;

            for (size_t i = 0; i < g_scan.tracked.size(); ++i)
            {
                Tracked& t = g_scan.tracked[i];
                const unsigned char* at = base + i * kStride;

                float value = 0.0f;

                if (t.bytes == 2)
                {
                    uint16_t half = 0;
                    std::memcpy(&half, at, sizeof(half));
                    value = HalfToFloat(half);
                }
                else
                {
                    std::memcpy(&value, at, sizeof(value));
                }

                if (!std::isfinite(value))
                    continue;

                if (t.reads == 0)
                {
                    t.lowest = value;
                    t.highest = value;
                }
                else
                {
                    t.lowest = std::min(t.lowest, value);
                    t.highest = std::max(t.highest, value);
                }

                // "Moves" is the whole point of the readout. An exposure changes with the lighting;
                // a constant somebody happened to put in a 1x1 texture does not. Ten percent of
                // travel is well above readback noise and well below what walking outdoors does.
                if (t.reads > 0 && t.highest > 0.0f && (t.highest - t.lowest) > 0.10f * std::abs(t.highest))
                    t.moves = true;

                t.latest = value;
                t.reads++;
            }

            D3D12_RANGE nothingWritten { 0, 0 };
            old->Unmap(0, &nothingWritten);
        }
    }

    ID3D12Resource* dst = g_scan.readback[g_scan.frames % kSlots];

    if (dst == nullptr)
        return;

    // The state a candidate is in is the game's business and nothing here has a contract about it.
    //
    // UNORDERED_ACCESS is the assumption, and it is the reasonable one: every candidate got here by
    // having an unordered access view created on it, which is what a compute shader writes through,
    // and an eye adaptation buffer is written every frame and read by the next pass. It is still an
    // assumption, which is why the whole scan is behind a setting that is off by default -- getting
    // this wrong on someone's machine costs them a frame or a device, and nobody who has not asked
    // for the scan should be exposed to that.
    for (size_t i = 0; i < g_scan.tracked.size(); ++i)
    {
        Tracked& t = g_scan.tracked[i];

        if (t.resource == nullptr)
            continue;

        Barrier(cmdList, t.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);

        if (t.isBuffer)
        {
            cmdList->CopyBufferRegion(dst, i * kStride, t.resource, 0, t.bytes);
        }
        else
        {
            D3D12_TEXTURE_COPY_LOCATION src {};
            src.pResource = t.resource;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION to {};
            to.pResource = dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Offset = i * kStride;
            to.PlacedFootprint.Footprint.Format = t.bytes == 2 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
            to.PlacedFootprint.Footprint.Width = 1;
            to.PlacedFootprint.Footprint.Height = 1;
            to.PlacedFootprint.Footprint.Depth = 1;
            to.PlacedFootprint.Footprint.RowPitch = 256;

            D3D12_BOX one { 0, 0, 0, 1, 1, 1 };
            cmdList->CopyTextureRegion(&to, 0, 0, 0, &src, &one);
        }

        Barrier(cmdList, t.resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    g_scan.frames++;
    g_scan.status = "";
}

std::vector<Candidate> Report()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    std::vector<Candidate> out;
    out.reserve(g_scan.tracked.size());

    for (const Tracked& t : g_scan.tracked)
    {
        Candidate c;
        c.shape = t.shape;
        c.latest = t.latest;
        c.lowest = t.lowest;
        c.highest = t.highest;
        c.reads = t.reads;
        c.moves = t.moves;
        out.push_back(c);
    }

    return out;
}

const char* Status()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_scan.status;
}

bool Scanning() { return Config::Instance()->DlssNrScanExposure.value_or_default(); }

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    for (Tracked& t : g_scan.tracked)
    {
        if (t.resource != nullptr)
            t.resource->Release();
    }

    g_scan.tracked.clear();

    for (unsigned int i = 0; i < kSlots; ++i)
    {
        if (g_scan.readback[i] != nullptr)
        {
            g_scan.readback[i]->Release();
            g_scan.readback[i] = nullptr;
        }
    }

    g_scan.frames = 0;
    g_scan.status = "not started";
}

} // namespace ExposureScan
} // namespace DlssNr
