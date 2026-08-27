// Turns the upscaler's linear HDR output into the kind of picture Neural Rendering was trained on, and
// folds the model's answer back into the frame.
//
// Two passes:
//
//   ENCODE   linear HDR -> an sRGB-encoded picture in [0,1]. This is what the model is shown. Feeding it
//            linear light instead does not shift colour slightly; it makes the frame unusable.
//
//   RESOLVE  proxy + model output -> linear HDR, written over the frame.
//
// Resolve does not simply decode the model's output. It takes the *difference* between what the model
// returned and what it was shown, and adds that back, which is what NVIDIA's own integration does. Three
// things follow from it. At strength zero the frame is bit-for-bit what the upscaler produced, because
// the encode and decode are exact inverses. Anything the model left alone stays exactly as it was, rather
// than making a round trip through the curve. And the edit can be scaled -- including past 1.0, which is
// the only honest way to answer "is this doing anything at all".
//
// Luminance and colour are scaled separately because the model does both, and they are worth judging
// apart: detail synthesis is a luminance edit, and any colour shift is usually the part you do not want.
//
// The curve is Reinhard plus sRGB rather than anything filmic, chosen because it inverts exactly and
// cheaply. It is not the game's own tonemapper, so the picture the model sees is not the one the player
// finally sees -- but it is in the right domain, which is what the model actually needs.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

namespace codec
{
constexpr int MODE_ENCODE = 0;
constexpr int MODE_RESOLVE = 1;

// Debug views, so the model's contribution can be looked at rather than guessed at.
constexpr int DEBUG_OFF = 0;
constexpr int DEBUG_PROXY = 1;      // the picture the model was shown
constexpr int DEBUG_MODEL = 2;      // the model's raw answer
constexpr int DEBUG_DIFFERENCE = 3; // what it changed, amplified

inline const char* kShaderSource = R"(
cbuffer Params : register(b0)
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    uint  gPad;
};

Texture2D<float4>   gSource : register(t0);   // encode: the frame. resolve: the proxy.
Texture2D<float4>   gModel  : register(t1);   // resolve: what the model returned.
RWTexture2D<float4> gTarget : register(u0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// sRGB rather than a plain 2.2 power: it is what an SDR game buffer actually carries, and the model was
// trained on those.
float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

// Reinhard against the white point, so an open-ended range lands in [0,1] and comes back exactly.
float3 ToneDown(float3 v, float whitePoint)
{
    v = max(v, float3(0.0, 0.0, 0.0)) / whitePoint;
    return v / (1.0 + v);
}

float3 ToneUp(float3 v, float whitePoint)
{
    // Clamped just below one: the inverse diverges there, and infinities in this buffer would reach
    // frame generation.
    v = min(saturate(v), float3(0.999, 0.999, 0.999));
    return (v / (1.0 - v)) * whitePoint;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float4 source = gSource.Load(int3(id.xy, 0));

    if (gMode == 0)
    {
        gTarget[id.xy] = float4(LinearToSrgb(ToneDown(source.rgb, gWhitePoint)), source.a);
        return;
    }

    float3 proxy = source.rgb;
    float3 model = gModel.Load(int3(id.xy, 0)).rgb;
    float3 edit = model - proxy;

    if (gDebugView == 1)
    {
        gTarget[id.xy] = float4(ToneUp(SrgbToLinear(proxy), gWhitePoint), source.a);
        return;
    }

    if (gDebugView == 2)
    {
        gTarget[id.xy] = float4(ToneUp(SrgbToLinear(model), gWhitePoint), source.a);
        return;
    }

    if (gDebugView == 3)
    {
        // Amplified and centred on grey, so both directions of the edit are visible at once.
        float3 shown = saturate(0.5 + edit * 20.0);
        gTarget[id.xy] = float4(ToneUp(SrgbToLinear(shown), gWhitePoint), source.a);
        return;
    }

    // Split so the detail the model synthesised and any colour it shifted can be dialled apart.
    float lumaEdit = dot(edit, kLuma);
    float3 colourEdit = edit - lumaEdit;
    float3 applied = proxy + lumaEdit * gTransferStrength + colourEdit * gColourStrength;

    gTarget[id.xy] = float4(ToneUp(SrgbToLinear(applied), gWhitePoint), source.a);
}
)";

struct Params
{
    unsigned int mode;
    float whitePoint;
    unsigned int width;
    unsigned int height;
    float transferStrength;
    float colourStrength;
    unsigned int debugView;
    unsigned int pad;
};

// A typeless resource cannot be viewed, and the buffer the upscaler writes is occasionally declared that
// way, so the typed member of the same family is substituted.
inline DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        // The sRGB view cannot be bound as a typed UAV, and the shader does its own transfer function.
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

// Owns the compute pipeline and the descriptors both passes need. The dispatches are recorded onto the
// caller's command list, so there is no queue or fence to manage here.
class Codec
{
  public:
    bool ensure(ID3D12Device* device)
    {
        if (pipeline_ != nullptr)
            return true;

        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;

        if (FAILED(D3DCompile(kShaderSource, strlen(kShaderSource), nullptr, nullptr, nullptr, "main",
                              "cs_5_1", 0, 0, &code, &errors)))
        {
            if (errors != nullptr)
                errors->Release();

            return false;
        }

        if (errors != nullptr)
            errors->Release();

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 2; // proxy and model
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = sizeof(Params) / 4;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;

        ID3DBlob* serialized = nullptr;

        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)))
        {
            code->Release();
            return false;
        }

        HRESULT hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                 IID_PPV_ARGS(&root_));
        serialized->Release();

        if (FAILED(hr))
        {
            code->Release();
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = root_;
        pso.CS.pShaderBytecode = code->GetBufferPointer();
        pso.CS.BytecodeLength = code->GetBufferSize();
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_));
        code->Release();

        if (FAILED(hr))
            return false;

        // Three descriptors per dispatch, two dispatches a frame; a ring of eight keeps a frame's
        // descriptors from being overwritten while it is still in flight.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kRingSlots * 3;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_))))
            return false;

        stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device_ = device;
        return true;
    }

    // Both source textures must already be shader-readable and the target writable.
    void dispatch(ID3D12GraphicsCommandList* cmd, const Params& constants, ID3D12Resource* source,
                  ID3D12Resource* model, ID3D12Resource* target)
    {
        if (pipeline_ == nullptr)
            return;

        const unsigned int slot = ring_;
        ring_ = (ring_ + 1) % kRingSlots;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T) slot * 3 * stride_;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += (UINT64) slot * 3 * stride_;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;

        srv.Format = TypedFormat(source->GetDesc().Format);
        device_->CreateShaderResourceView(source, &srv, cpu);

        // Encode never reads it, but the table must be fully populated or the descriptor is undefined.
        D3D12_CPU_DESCRIPTOR_HANDLE modelHandle = cpu;
        modelHandle.ptr += stride_;
        ID3D12Resource* modelSource = model != nullptr ? model : source;
        srv.Format = TypedFormat(modelSource->GetDesc().Format);
        device_->CreateShaderResourceView(modelSource, &srv, modelHandle);

        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpu;
        uavHandle.ptr += (SIZE_T) 2 * stride_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = TypedFormat(target->GetDesc().Format);
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(target, nullptr, &uav, uavHandle);

        ID3D12DescriptorHeap* heaps[] = { heap_ };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root_);
        cmd->SetPipelineState(pipeline_);
        cmd->SetComputeRootDescriptorTable(0, gpu);
        cmd->SetComputeRoot32BitConstants(1, sizeof(Params) / 4, &constants, 0);
        cmd->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);
    }

    void destroy()
    {
        if (pipeline_ != nullptr)
        {
            pipeline_->Release();
            pipeline_ = nullptr;
        }

        if (root_ != nullptr)
        {
            root_->Release();
            root_ = nullptr;
        }

        if (heap_ != nullptr)
        {
            heap_->Release();
            heap_ = nullptr;
        }

        device_ = nullptr;
    }

  private:
    static const unsigned int kRingSlots = 8;

    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    unsigned int stride_ = 0;
    unsigned int ring_ = 0;
};
} // namespace codec
