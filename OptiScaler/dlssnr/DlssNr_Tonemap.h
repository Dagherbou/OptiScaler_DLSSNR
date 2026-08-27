// Display-referred round trip for the pre-frame-generation path.
//
// Neural Rendering is trained on finished, display-referred frames. DLSS's output is linear HDR with an
// open-ended range, and feeding that to the model does not merely shift colour -- it reads ordinary
// values as enormously bright and returns something unusable.
//
// So the frame is encoded into a display-referred [0,1] signal, the model runs on that, and the result
// is decoded back to linear HDR before frame generation ever sees it. The transform pair must be an
// exact inverse, otherwise every pixel the model leaves untouched would still come back altered.
//
// Reinhard plus a gamma is used rather than a filmic curve precisely because it inverts exactly and
// cheaply: x/(1+x) undone by y/(1-y). It is not the game's own tonemapper, so what the model sees is
// not what the player will finally see, but it is in the right domain, which is what the model needs.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

namespace tonemap {

// Encode and decode share a shader; the constant selects direction. Values above the knee would clip in
// the encoded domain, so the curve is scaled by a white point that keeps highlights recoverable.
inline const char *kShaderSource = R"(
cbuffer Params : register(b0)
{
    uint  gDecode;      // 0 = linear HDR -> display referred, 1 = the inverse
    float gWhitePoint;  // linear value that maps to 1.0 before the gamma
    uint  gWidth;
    uint  gHeight;
};

Texture2D<float4>   gSource : register(t0);
RWTexture2D<float4> gTarget : register(u0);

static const float kGamma = 2.2;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float4 c = gSource.Load(int3(id.xy, 0));
    float3 v = c.rgb;

    if (gDecode == 0)
    {
        // Guard against the negatives and NaNs an HDR buffer can legitimately contain: they would come
        // back as black or garbage through the inverse.
        v = max(v, float3(0.0, 0.0, 0.0));
        v = v / gWhitePoint;
        v = v / (1.0 + v);
        v = pow(saturate(v), 1.0 / kGamma);
    }
    else
    {
        v = pow(saturate(v), kGamma);
        // Clamped just below one: the inverse diverges at exactly one, which would produce infinities
        // in the buffer frame generation is about to read.
        v = min(v, float3(0.999, 0.999, 0.999));
        v = v / (1.0 - v);
        v = v * gWhitePoint;
    }

    gTarget[id.xy] = float4(v, c.a);
}
)";

// A typeless resource cannot be viewed. DLSS's output is usually typed already, but the render target it
// writes into is sometimes declared typeless, so the typed member of the same family is substituted.
inline DXGI_FORMAT typedFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    // The sRGB views cannot be bound as a typed UAV, and the shader works in linear anyway, so the
    // plain member is used and the encode curve stands in for the transfer function.
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

struct Params {
    unsigned int decode;
    float whitePoint;
    unsigned int width;
    unsigned int height;
};

// Owns the compute pipeline and the descriptor heap the round trip needs. One instance is enough: the
// dispatches are recorded onto the caller's command list, so there is no queue or fence to manage.
class ToneTransform {
public:
    bool ensure(ID3D12Device *device) {
        if (pipeline_) {
            return true;
        }
        ID3DBlob *code = nullptr;
        ID3DBlob *errors = nullptr;
        if (FAILED(D3DCompile(kShaderSource, strlen(kShaderSource), nullptr, nullptr, nullptr, "main",
                              "cs_5_1", 0, 0, &code, &errors))) {
            if (errors) {
                errors->Release();
            }
            return false;
        }
        if (errors) {
            errors->Release();
        }

        // A descriptor table for the pair of textures, and the small constant block inline as root
        // constants so no upload buffer or per-frame allocation is needed.
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = sizeof(Params) / 4;
        params[1].Constants.ShaderRegister = 0;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;

        ID3DBlob *serialized = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                               nullptr))) {
            code->Release();
            return false;
        }
        HRESULT hr = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                 serialized->GetBufferSize(), IID_PPV_ARGS(&root_));
        serialized->Release();
        if (FAILED(hr)) {
            code->Release();
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = root_;
        pso.CS.pShaderBytecode = code->GetBufferPointer();
        pso.CS.BytecodeLength = code->GetBufferSize();
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_));
        code->Release();
        if (FAILED(hr)) {
            return false;
        }

        // Two descriptors per dispatch, and two dispatches per frame, so a ring of four keeps a frame's
        // descriptors from being overwritten while it is still in flight.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kRingSlots * 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_)))) {
            return false;
        }
        stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device_ = device;
        return true;
    }

    // Records source -> target through the curve. Both must already be in the states the pipeline needs:
    // the source shader-readable, the target writable.
    void dispatch(ID3D12GraphicsCommandList *cmd, ID3D12Resource *source, ID3D12Resource *target,
                  unsigned int width, unsigned int height, bool decode, float whitePoint) {
        if (!pipeline_) {
            return;
        }
        const unsigned int slot = ring_;
        ring_ = (ring_ + 1) % kRingSlots;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T) slot * 2 * stride_;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += (UINT64) slot * 2 * stride_;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = typedFormat(source->GetDesc().Format);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(source, &srv, cpu);

        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpu;
        uavHandle.ptr += stride_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = typedFormat(target->GetDesc().Format);
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(target, nullptr, &uav, uavHandle);

        Params constants = { decode ? 1u : 0u, whitePoint, width, height };

        ID3D12DescriptorHeap *heaps[] = { heap_ };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root_);
        cmd->SetPipelineState(pipeline_);
        cmd->SetComputeRootDescriptorTable(0, gpu);
        cmd->SetComputeRoot32BitConstants(1, sizeof(Params) / 4, &constants, 0);
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }

    void destroy() {
        if (pipeline_) {
            pipeline_->Release();
            pipeline_ = nullptr;
        }
        if (root_) {
            root_->Release();
            root_ = nullptr;
        }
        if (heap_) {
            heap_->Release();
            heap_ = nullptr;
        }
        device_ = nullptr;
    }

private:
    static const unsigned int kRingSlots = 4;

    ID3D12Device *device_ = nullptr;
    ID3D12RootSignature *root_ = nullptr;
    ID3D12PipelineState *pipeline_ = nullptr;
    ID3D12DescriptorHeap *heap_ = nullptr;
    unsigned int stride_ = 0;
    unsigned int ring_ = 0;
};

} // namespace tonemap
