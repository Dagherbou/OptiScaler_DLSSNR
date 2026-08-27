// Reads a small block of pixels back off the GPU so the add-on can report the range of values it is
// actually working with.
//
// The tone transform's white point has to match the game's exposure, and the first attempt at this
// injection point failed precisely because that range was guessed. Guessing twice would be careless when
// the number is sitting in the buffer.
//
// The copy is issued on one frame and mapped several frames later rather than fenced. A diagnostic that
// blocked the render thread would change the thing it is measuring, and a torn read costs nothing here:
// the statistic is a rough range, and it is sampled continuously.

#pragma once

#include <windows.h>
#include <d3d12.h>

namespace probe {

// Half-precision to float. Only the cases a colour buffer produces are handled exactly; denormals are
// close enough to zero for a range readout.
inline float halfToFloat(unsigned short h) {
    const unsigned int sign = (h & 0x8000u) << 16;
    const unsigned int exponent = (h >> 10) & 0x1Fu;
    const unsigned int mantissa = h & 0x3FFu;
    unsigned int bits;
    if (exponent == 0) {
        bits = sign;
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

struct Stats {
    float minLuma = 0.0f;
    float maxLuma = 0.0f;
    float meanLuma = 0.0f;
    bool valid = false;
};

// One texture's worth of sampling state. Kept per call site so the two ends of the round trip can be
// compared against each other.
class BlockReader {
public:
    static const unsigned int kSide = 64;

    // Records a copy of a kSide-square block from the middle of the texture. Safe to call every frame;
    // it only issues a copy when the previous one has been collected.
    void capture(ID3D12GraphicsCommandList *cmd, ID3D12Resource *tex, D3D12_RESOURCE_STATES state) {
        if (countdown_ >= 0) {
            return;
        }
        D3D12_RESOURCE_DESC desc = tex->GetDesc();
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Width < kSide || desc.Height < kSide) {
            return;
        }
        ID3D12Device *device = nullptr;
        if (FAILED(tex->GetDevice(IID_PPV_ARGS(&device))) || !device) {
            return;
        }
        if (!readback_) {
            D3D12_RESOURCE_DESC block = desc;
            block.Width = kSide;
            block.Height = kSide;
            unsigned long long total = 0;
            device->GetCopyableFootprints(&block, 0, 1, 0, &layout_, nullptr, nullptr, &total);

            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buf = {};
            buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buf.Width = total;
            buf.Height = 1;
            buf.DepthOrArraySize = 1;
            buf.MipLevels = 1;
            buf.Format = DXGI_FORMAT_UNKNOWN;
            buf.SampleDesc.Count = 1;
            buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback_)))) {
                device->Release();
                return;
            }
        }
        device->Release();

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback_;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = layout_;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_BOX box = {};
        box.left = (unsigned int) (desc.Width / 2) - kSide / 2;
        box.top = desc.Height / 2 - kSide / 2;
        box.right = box.left + kSide;
        box.bottom = box.top + kSide;
        box.back = 1;

        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->ResourceBarrier(1, &b);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = state;
        cmd->ResourceBarrier(1, &b);

        // Deep enough that the copy has certainly executed; the render thread is never made to wait.
        countdown_ = 6;
    }

    // Returns a reading once one is ready, and nothing on every other frame.
    Stats collect() {
        Stats out;
        if (countdown_ < 0) {
            return out;
        }
        if (--countdown_ >= 0) {
            return out;
        }
        if (!readback_) {
            return out;
        }
        void *mapped = nullptr;
        D3D12_RANGE range = { 0, (SIZE_T) layout_.Footprint.RowPitch * kSide };
        if (FAILED(readback_->Map(0, &range, &mapped)) || !mapped) {
            return out;
        }
        float lo = 1e30f;
        float hi = -1e30f;
        double sum = 0.0;
        for (unsigned int y = 0; y < kSide; ++y) {
            const unsigned short *row =
                (const unsigned short *) ((const unsigned char *) mapped +
                                          (size_t) y * layout_.Footprint.RowPitch);
            for (unsigned int x = 0; x < kSide; ++x) {
                const float r = halfToFloat(row[x * 4 + 0]);
                const float g = halfToFloat(row[x * 4 + 1]);
                const float b = halfToFloat(row[x * 4 + 2]);
                const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                lo = luma < lo ? luma : lo;
                hi = luma > hi ? luma : hi;
                sum += luma;
            }
        }
        D3D12_RANGE written = { 0, 0 };
        readback_->Unmap(0, &written);

        out.minLuma = lo;
        out.maxLuma = hi;
        out.meanLuma = (float) (sum / (kSide * kSide));
        out.valid = true;
        return out;
    }

    void destroy() {
        if (readback_) {
            readback_->Release();
            readback_ = nullptr;
        }
        countdown_ = -1;
    }

private:
    ID3D12Resource *readback_ = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout_ = {};
    int countdown_ = -1;
};

} // namespace probe
