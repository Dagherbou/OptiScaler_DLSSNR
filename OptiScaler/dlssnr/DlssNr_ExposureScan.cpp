#include "pch.h"

#include "DlssNr_ExposureScan.h"

#include <Config.h>
#include <Util.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace DlssNr
{
namespace ExposureScan
{
namespace
{

// How many candidates are worth keeping. The shape being looked for is rare -- in a frame's worth of
// A cap on how many candidates are tracked. Kept low originally as a statement that a tight filter
// should find only a handful -- but buffer-heavy engines crowd the real exposure out of a low cap:
// Cyberpunk's REDengine creates dozens of tiny UAV buffers the same 4/12 bytes as an exposure, and its
// real one can land past slot 24. Now that the scan is crash-safe (references dropped at feature
// teardown), the real discriminator is MOVEMENT, not scarcity, so a larger cap costs only a few tiny
// copies a frame and stops the answer being crowded out.
constexpr size_t kMaxCandidates = 64;

// Ring depth for the readbacks. Four, so the slot being read is four frames behind the slot being
// written and the read never waits on the GPU. Same depth and the same reason as the meter's.
constexpr unsigned int kSlots = 4;

// Every candidate's value lands in one buffer, at its own offset, so there is one copy per candidate
// but only one buffer per slot. 16 bytes each is enough for the widest format worth reading.
// D3D12 requires a placed-footprint offset to be a multiple of
// D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512). A texture candidate at index i copies to i*kStride,
// so the stride is that alignment; buffer copies have no such rule and are unaffected by the waste.
constexpr unsigned int kStride = 512;

// What an exposure could plausibly be. Outside these it is a flag, a counter, a sentinel or a
// zeroed buffer nobody has written yet -- Nioh 3 offers all four, including one holding 1000000.
constexpr float kFloor = 1e-6f;
constexpr float kCeiling = 1e4f;

struct Tracked
{
    ID3D12Resource* resource = nullptr;
    std::string shape;
    bool isBuffer = false;
    unsigned int bytes = 4;
    DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN;  // the source texture's format, for CopyTextureRegion

    float latest = 0.0f;
    float lowest = 0.0f;
    float highest = 0.0f;
    unsigned int reads = 0;
    unsigned int inRange = 0;   // reads that could plausibly be an exposure
    bool moves = false;
};

struct ScanState
{
    unsigned int examined = 0;
    std::vector<Tracked> tracked;
    ID3D12Resource* readback[kSlots] = {};
    unsigned long long frames = 0;
    const char* status = "not started";
    bool complained = false;
    unsigned int nearMissLogged = 0;   // bounded diagnostic; see NoteResource
};

ScanState g_scan;
std::mutex g_scanMutex;

// Formats an exposure could plausibly be in: floating point, one or two channels.
//
// Two channels because eye adaptation commonly carries the value and something alongside it -- the
// previous frame's value, or a target it is easing toward. Anything wider is a picture rather than a
// number. Integer formats are excluded because an exposure is a scale and a normalised integer
// cannot hold one.
// outBytes is the size of the FIRST channel only -- that is the one texel this reads back, and a
// two-channel format that stored 4 or 8 there would be decoded as the wrong type. The copy uses the
// real format (outFormat) so it matches the source texture; the read uses outBytes.
bool PlausibleFormat(DXGI_FORMAT f, unsigned int* outBytes, const char** outName, DXGI_FORMAT* outFormat)
{
    *outFormat = f;
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
        *outBytes = 4;
        *outName = "R32G32_FLOAT";
        return true;
    case DXGI_FORMAT_R16G16_FLOAT:
        *outBytes = 2;
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

// Whether the scan should be running at all.
//
// Choosing it as the white point's source is the whole of the answer for anybody using this.
// The separate setting survives as a developer override, for the one case a user has no reason
// to want: watching the scan in a game that supplies a REAL exposure, so the two can be
// compared in the log. That is validation work, not a control, and it does not belong in a
// panel.
bool Wanted()
{
    return Config::Instance()->DlssNrWhitePointSource.value_or_default() == 2 ||
           Config::Instance()->DlssNrScanExposure.value_or_default();
}

} // namespace

// Everything the two entry points share: does this description look like a number rather than a
// picture, and if so what is it.
//
// Widened from the first attempt, which asked for at most 4x4 and one or two channels and found
// nothing anywhere. That was tuned on what an exposure buffer ought to look like rather than on what
// engines actually allocate: some keep a small histogram beside the value, some keep a few frames of
// history, and some put the whole thing in a four-channel texture and use one channel. The filter
// only has to be tight enough that the list stays readable.
bool LooksLikeANumber(const D3D12_RESOURCE_DESC& rd, std::string* outShape, unsigned int* outBytes,
                      bool* outIsBuffer, DXGI_FORMAT* outFormat)
{
    // An exposure is computed, so it is written by a shader. This is the one condition worth being
    // strict about: it removes almost everything without removing anything that could be the answer.
    if ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
        return false;

    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        // 256 texels rather than 16. A 16x16 texture is still a number by any reasonable measure and
        // a 256-bin histogram is exactly how a lot of engines compute one.
        if (rd.Width * rd.Height > 256 || rd.Width == 0 || rd.Height == 0)
            return false;

        const char* name = nullptr;

        if (!PlausibleFormat(rd.Format, outBytes, &name, outFormat))
            return false;

        *outIsBuffer = false;
        *outShape = std::to_string((unsigned int) rd.Width) + "x" + std::to_string(rd.Height) + " " + name;
        return true;
    }

    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        *outFormat = DXGI_FORMAT_UNKNOWN;

        // Unreal moved eye adaptation off a texture and onto a buffer, so buffers have to be in
        // scope or a whole engine's worth of games is invisible.
        //
        // 128 bytes, down from 4kB. The wider bound filled all twelve slots in Nioh 3 with 256, 512
        // and 768 byte buffers that never held anything but zero, and the real answer -- eight bytes
        // -- only made the list because it happened to be created early. A cap that can be filled by
        // junk is a cap that can hide the answer.
        if (rd.Width == 0 || rd.Width > 128)
            return false;

        *outIsBuffer = true;
        *outBytes = 4;
        *outShape = "buffer, " + std::to_string((unsigned int) rd.Width) + " bytes";
        return true;
    }

    return false;
}

void Adopt(ID3D12Resource* resource, const std::string& shape, unsigned int bytes, bool isBuffer,
           DXGI_FORMAT texFormat)
{
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
            LOG_WARN("DLSS-NR exposure scan: more than {} candidates, so the filter is too loose here "
                     "rather than the game having {} exposures",
                     kMaxCandidates, kMaxCandidates);
        }

        return;
    }

    Tracked t;
    t.resource = resource;
    t.shape = shape;
    t.isBuffer = isBuffer;
    t.bytes = bytes;
    t.texFormat = texFormat;
    resource->AddRef();

    g_scan.tracked.push_back(t);

    LOG_INFO("DLSS-NR exposure scan: candidate {} -- {}", g_scan.tracked.size(), shape);
}

void NoteResource(const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource)
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return;

    if (desc == nullptr || resource == nullptr)
        return;

    std::string shape;
    unsigned int bytes = 4;
    bool isBuffer = false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_scan.examined++;

    if (!LooksLikeANumber(*desc, &shape, &bytes, &isBuffer, &fmt))
    {
        // Near-miss diagnostic. A resource a shader writes (UAV) that the filter rejected: logging its
        // shape -- bounded to the first 40 so it cannot flood -- reveals whether a game the scan finds
        // nothing in (Cyberpunk 2077) has an exposure the filter narrowly misses (a small UAV texture
        // in an unlisted format, or a UAV buffer just over 128 bytes -> widen precisely to match) or
        // nothing scannable at all (only large buffers/textures -> the exposure is baked in a bigger
        // buffer and no filter change can help). Read these against Examined() in the log.
        if ((desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && g_scan.nearMissLogged < 40)
        {
            g_scan.nearMissLogged++;
            LOG_INFO("DLSS-NR scan near-miss #{}: UAV dim {} {}x{}x{} fmt {} (filter rejected)",
                     g_scan.nearMissLogged, (int) desc->Dimension, (unsigned int) desc->Width,
                     desc->Height, desc->DepthOrArraySize, (int) desc->Format);
        }

        return;
    }

    Adopt(resource, shape, bytes, isBuffer, fmt);
}

unsigned int Examined()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_scan.examined;
}

void NoteUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)
{
    // Deliberately NOT gated on the scan setting, and that was a real bug rather than a nicety.
    //
    // An engine creates its eye adaptation view once, when it builds its render targets, which is
    // long before anybody opens a menu and ticks a box. Gating the recording meant every candidate
    // was thrown away before the scan could want it, and the readout then said "nothing matched
    // yet -- play for a few seconds", which is advice that could never come true no matter how long
    // anyone played.
    //
    // Recording is a resource description and a pointer. What is genuinely risky -- reading a buffer
    // the game owns, on an assumption about its state -- lives in Tick, and that is still gated.
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return;

    if (resource == nullptr)
        return;

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();

    std::string shape;
    unsigned int bytes = 4;
    bool isBuffer = false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_scan.examined++;

    if (!LooksLikeANumber(rd, &shape, &bytes, &isBuffer, &fmt))
        return;

    Adopt(resource, shape, bytes, isBuffer, fmt);
}

void Tick(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
    if (!Wanted())
        return;

    if (device == nullptr || cmdList == nullptr)
        return;

    // Before the lock, deliberately. EnsureReadback creates a committed resource, and that call is
    // detoured to hkCreateCommittedResource -> NoteResource, which takes g_scanMutex. Holding it
    // here would be a self-deadlock on a non-recursive mutex the first frame the scan runs. The
    // readback ring is only ever touched on this (render) thread, so it needs no lock of its own.
    if (!EnsureReadback(device))
        return;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_scan.tracked.empty())
    {
        g_scan.status = "no buffer in this game is shaped like an exposure";
        return;
    }

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

                // Only values that could BE an exposure are allowed into the range, and this is
                // the whole of "8 watching, none moving" never changing.
                //
                // A buffer is usually zero the first time it is read -- created but not yet
                // written, or read a frame before the game fills it. That zero became `lowest`,
                // and since movement is a ratio guarded by `lowest > kFloor`, one early zero
                // disqualified that candidate for the rest of the session however the light
                // changed. The range has to be built from plausible samples, not from whichever
                // sample happened to be first.
                if (value <= kFloor || value >= kCeiling)
                {
                    t.latest = value;
                    t.reads++;
                    continue;
                }

                if (t.inRange == 0)
                {
                    t.lowest = value;
                    t.highest = value;
                }
                else
                {
                    t.lowest = std::min(t.lowest, value);
                    t.highest = std::max(t.highest, value);
                }

                t.inRange++;

                // "Moves" is the whole point of the readout, and the first version of this test
                // was wrong in a way that mattered: a spread of ten percent of the highest value
                // seen is a threshold of zero when the highest value seen is zero, so three buffers
                // sitting at 0.00000 with float noise around them all reported MOVES.
                //
                // Ratios, not differences, and only over values that could be an exposure at all.
                // An exposure is positive, is not a thousandth of a thousandth, and does not sit at
                // a million. Nioh 3's real one runs 0.0019 to 0.616 -- a factor of three hundred --
                // so a quarter is a low bar that noise cannot reach.
                if (t.inRange > 1 && t.highest > t.lowest * 1.25f)
                    t.moves = true;

                t.latest = value;
                t.reads++;
            }

            D3D12_RANGE nothingWritten { 0, 0 };
            old->Unmap(0, &nothingWritten);
        }
    }

    // Periodic movement readout. The menu's Advanced panel shows which candidate tracks the light, but
    // the log did not -- so a game the scan is being taught (Cyberpunk) could not be cracked from a log
    // alone. Every ~300 frames, name the candidates that MOVE and their travel: the exposure is the one
    // that swings widely between bright and dark. Throttled, and only while the scan is wanted.
    if (g_scan.frames > 0 && g_scan.frames % 300 == 0)
    {
        unsigned int movers = 0;

        for (size_t i = 0; i < g_scan.tracked.size(); ++i)
        {
            const Tracked& t = g_scan.tracked[i];

            if (!t.moves)
                continue;

            movers++;
            LOG_INFO("DLSS-NR scan mover: candidate {} ({}) range {:.5f}..{:.5f} (x{:.1f}), latest {:.5f}",
                     (unsigned int) (i + 1), t.shape, t.lowest, t.highest,
                     t.lowest > kFloor ? t.highest / t.lowest : 0.0f, t.latest);
        }

        if (movers == 0)
            LOG_INFO("DLSS-NR scan: {} candidates tracked, none moving yet -- go between bright and dark",
                     (unsigned int) g_scan.tracked.size());
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
            to.PlacedFootprint.Footprint.Format = t.texFormat;
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

// How many frames of watching without movement before saying so. At sixty frames a second this is
// about half a minute, which is long enough to have walked somewhere with different light in it and
// short enough that nobody waits on it wondering.
constexpr unsigned int kPatience = 1800;

Verdict Where()
{
    if (!Wanted())
        return Verdict::Off;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_scan.tracked.empty())
        return Verdict::Waiting;

    unsigned int mostReads = 0;

    for (const Tracked& t : g_scan.tracked)
    {
        if (t.moves)
            return Verdict::Found;

        mostReads = std::max(mostReads, t.reads);
    }

    return mostReads >= kPatience ? Verdict::Barren : Verdict::Watching;
}

const char* Headline()
{
    static std::string line;

    switch (Where())
    {
    case Verdict::Off:
        line = "";
        break;

    case Verdict::Waiting:
    {
        // The examined count is the whole diagnosis. Zero means the hook is not running and no
        // amount of playing will change that; a large number means the game genuinely has nothing
        // shaped like an exposure, which is an answer rather than a failure.
        const unsigned int seen = Examined();
        line = seen == 0 ? "DLSS-NR exposure scan: NOT RUNNING -- no resources seen at all"
                         : "DLSS-NR exposure scan: examined " + std::to_string(seen) +
                               " resources, none shaped like an exposure";
        break;
    }

    case Verdict::Watching:
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);
        unsigned int mostReads = 0;

        for (const Tracked& t : g_scan.tracked)
            mostReads = std::max(mostReads, t.reads);

        line = "DLSS-NR exposure scan: watching " + std::to_string(g_scan.tracked.size()) +
               ", none moving yet -- walk between light and shade  (" +
               std::to_string(mostReads * 100 / kPatience) + "%)";
        break;
    }

    case Verdict::Found:
    {
        std::lock_guard<std::mutex> lock(g_scanMutex);

        // The widest travel wins where several move. An exposure swings by orders of magnitude
        // between a dark interior and open daylight; anything that merely wobbles is something else.
        size_t best = 0;
        float bestRatio = 0.0f;

        for (size_t i = 0; i < g_scan.tracked.size(); ++i)
        {
            const Tracked& t = g_scan.tracked[i];

            if (!t.moves || t.lowest <= kFloor)
                continue;

            const float ratio = t.highest / t.lowest;

            if (ratio > bestRatio)
            {
                bestRatio = ratio;
                best = i;
            }
        }

        char buf[192];
        // The live value is in here so the line visibly ticks. Without it the indicator looks stuck
        // the moment the range settles, which is exactly when it has succeeded.
        snprintf(buf, sizeof(buf),
                 "DLSS-NR exposure scan: FOUND -- candidate %zu = %.5f  (%.5f..%.5f, x%.0f)  done",
                 best + 1, g_scan.tracked[best].latest, g_scan.tracked[best].lowest,
                 g_scan.tracked[best].highest, bestRatio);
        line = buf;
        break;
    }

    case Verdict::Barren:
        line = "DLSS-NR exposure scan: nothing moved. No exposure to find here.";
        break;
    }

    return line.c_str();
}

float BestValue(int* outIndex, float* outLowest, float* outHighest)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    int best = -1;
    float bestRatio = 0.0f;

    for (size_t i = 0; i < g_scan.tracked.size(); ++i)
    {
        const Tracked& t = g_scan.tracked[i];

        if (!t.moves || t.lowest <= kFloor)
            continue;

        const float ratio = t.highest / t.lowest;

        if (ratio > bestRatio)
        {
            bestRatio = ratio;
            best = (int) i;
        }
    }

    if (best < 0)
        return 0.0f;

    if (outIndex != nullptr)
        *outIndex = best + 1;

    if (outLowest != nullptr)
        *outLowest = g_scan.tracked[best].lowest;

    if (outHighest != nullptr)
        *outHighest = g_scan.tracked[best].highest;

    return g_scan.tracked[best].latest;
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

bool Scanning() { return Wanted(); }

// ---------------------------------------------------------------------------------------------
// Multi-point anchoring
// ---------------------------------------------------------------------------------------------

namespace
{
std::vector<AnchorPoint> g_anchors;   // guarded by g_scanMutex, kept sorted by scan ascending

void SortAnchorsLocked()
{
    std::sort(g_anchors.begin(), g_anchors.end(),
              [](const AnchorPoint& a, const AnchorPoint& b) { return a.scan < b.scan; });
}
}  // namespace

// Load the persisted table (or migrate a pre-existing single anchor) exactly once, before any lock
// is taken -- LoadAnchors/AnchorAdd take g_scanMutex themselves, so this must not hold it.
void EnsureAnchorsLoaded()
{
    static std::once_flag once;
    std::call_once(once, [] {
        auto& cfg = *Config::Instance();
        const std::string ser = cfg.DlssNrScanAnchors.value_or_default();

        if (!ser.empty())
        {
            LoadAnchors(ser);
            return;
        }

        // Migration: fold a single-anchor ini from before this feature into a one-row table so the
        // user does not lose the calibration they already set.
        const float v = cfg.DlssNrScanAnchorValue.value_or_default();
        const float w = cfg.DlssNrScanAnchorWhitePoint.value_or_default();

        if (v > kFloor && w > 1e-6f)
            AnchorAdd(v, w);
    });
}

std::vector<AnchorPoint> Anchors()
{
    EnsureAnchorsLoaded();
    std::lock_guard<std::mutex> lock(g_scanMutex);
    return g_anchors;
}

bool AnchorAdd(float scan, float white)
{
    if (!(scan > kFloor && scan < kCeiling) || !(white > 1e-6f))
        return false;

    std::lock_guard<std::mutex> lock(g_scanMutex);

    // A near-duplicate scan value would make log(v_{k+1}) - log(v_k) ~ 0 and divide the interpolation
    // by zero. Replace the existing point's white instead of adding a second at the same place.
    for (auto& p : g_anchors)
    {
        if (scan > p.scan * 0.98f && scan < p.scan * 1.02f)
        {
            p.white = white;
            return true;
        }
    }

    if (g_anchors.size() >= 8)
        return false;

    g_anchors.push_back({ scan, white });
    SortAnchorsLocked();
    return true;
}

void AnchorSetWhite(int index, float white)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (index >= 0 && index < (int) g_anchors.size() && white > 1e-6f)
        g_anchors[index].white = white;
}

void AnchorRemove(int index)
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (index >= 0 && index < (int) g_anchors.size())
        g_anchors.erase(g_anchors.begin() + index);
}

void AnchorClear()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_anchors.clear();
}

float AnchoredWhitePoint(float scanNow, bool inverted, float trim)
{
    EnsureAnchorsLoaded();
    trim = std::clamp(trim, 0.25f, 4.0f);

    std::lock_guard<std::mutex> lock(g_scanMutex);

    if (g_anchors.empty() || !(scanNow > kFloor))
        return 0.0f;

    // One point is the original ratio law, and it needs the direction the two-point case reads off
    // the data instead.
    if (g_anchors.size() == 1)
    {
        const AnchorPoint& p = g_anchors[0];

        if (!(p.scan > kFloor) || !(p.white > 1e-6f))
            return 0.0f;

        const float ratio = inverted ? scanNow / p.scan : p.scan / scanNow;
        return std::clamp(p.white * ratio * trim, 0.01f, 4096.0f);
    }

    // Clamp at the ends: holding the outermost calibrated white point beats extrapolating a ratio
    // into a scene darker or brighter than anything the user calibrated.
    if (scanNow <= g_anchors.front().scan)
        return std::clamp(g_anchors.front().white * trim, 0.01f, 4096.0f);

    if (scanNow >= g_anchors.back().scan)
        return std::clamp(g_anchors.back().white * trim, 0.01f, 4096.0f);

    // Between two points: interpolate log(white) against log(scan). A white point is a divisor, so
    // log is the space in which it is linear -- and it makes the two-point case reproduce the exact
    // ratio law of the single-point case, so adding a second point never causes a jump.
    for (size_t k = 0; k + 1 < g_anchors.size(); ++k)
    {
        const AnchorPoint& a = g_anchors[k];
        const AnchorPoint& b = g_anchors[k + 1];

        if (scanNow >= a.scan && scanNow <= b.scan && b.scan > a.scan * 1.0001f)
        {
            const float t = (std::log(scanNow) - std::log(a.scan)) / (std::log(b.scan) - std::log(a.scan));
            const float w = std::exp(std::log(a.white) + t * (std::log(b.white) - std::log(a.white)));
            return std::clamp(w * trim, 0.01f, 4096.0f);
        }
    }

    return std::clamp(g_anchors.back().white * trim, 0.01f, 4096.0f);
}

void LoadAnchors(const std::string& serialized)
{
    std::vector<AnchorPoint> parsed;
    size_t i = 0;

    while (i < serialized.size() && parsed.size() < 8)
    {
        size_t semi = serialized.find(';', i);
        std::string tok = serialized.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
        i = semi == std::string::npos ? serialized.size() : semi + 1;

        const size_t colon = tok.find(':');
        if (colon == std::string::npos)
            continue;

        try
        {
            const float v = std::stof(tok.substr(0, colon));
            const float w = std::stof(tok.substr(colon + 1));

            if (v > kFloor && v < kCeiling && w > 1e-6f)
                parsed.push_back({ v, w });
        }
        catch (...)
        {
            // A malformed token is skipped rather than aborting the whole table.
        }
    }

    std::lock_guard<std::mutex> lock(g_scanMutex);
    g_anchors = std::move(parsed);
    SortAnchorsLocked();
}

std::string SerializeAnchors()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    std::string out;
    char buf[64];

    for (const AnchorPoint& p : g_anchors)
    {
        snprintf(buf, sizeof(buf), "%.6g:%.6g;", p.scan, p.white);
        out += buf;
    }

    return out;
}

// Drop the scan's references to the resources it captured, WITHOUT touching our own readback buffers.
//
// The scan AddRef's every candidate it adopts (Adopt) but nothing ever released them -- Shutdown() has
// no callers -- so a Streamline/DLSS-D-owned resource that passes the filter is pinned by our stray
// AddRef, and when the driver frees its (placed) heap at feature teardown the surviving wrapper points
// at freed memory: the use-after-free that removed the device in Cyberpunk (a lock on a freed object in
// nvwgf2umx). Calling this at feature release drops our references first, so nothing we hold outlives
// the heap. Only the candidates (foreign resources) are released here -- NOT the readback ring, which
// is ours and may have GPU copies in flight; freeing that here would be a new hazard. Capture is gated
// on NR being ENABLED (not on the scan source), so this releases whatever was captured whenever NR is
// on -- scan selected or not; it is a no-op only when NR is off (nothing captured), so FSR/XeSS users
// with NR off pay nothing. The scan re-adopts candidates next frame.
void ReleaseTrackedResources()
{
    std::lock_guard<std::mutex> lock(g_scanMutex);

    for (Tracked& t : g_scan.tracked)
    {
        if (t.resource != nullptr)
            t.resource->Release();
    }

    g_scan.tracked.clear();
    g_scan.complained = false;
}

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
