// Turns the upscaler's linear HDR output into the kind of picture Neural Rendering was trained on, and
// folds the model's answer back into the frame.
//
// Two passes:
//
//   ENCODE   linear HDR -> an sRGB-encoded picture in [0,1], plus an untouched copy of the frame. The
//            picture is what the model is shown; the copy is what the answer is folded back into.
//
//   RESOLVE  proxy + model output + that copy -> the finished frame.
//
// The first version of this decoded the model's output back through the inverse of the tone curve, and
// that is what turned every strip light in Cyberpunk into a string of coloured cells. Two reasons, both
// fatal on highlights:
//
//   * The curve was applied per channel, so a saturated bright light had its channels compressed by
//     different amounts and came back a different hue.
//
//   * x/(1-x) diverges as x approaches one. A light sitting at 0.99 in the encoded picture reconstructs
//     to a hundred times the white point, and the model nudging one channel by a thousandth moves that
//     by tens of percent. Highlights are exactly where the model has least to say and where the inverse
//     amplifies most, which is the worst possible combination.
//
// So nothing is reconstructed by inversion any more. The encode maps luminance and carries chroma along
// unchanged, so hue survives. The resolve keeps the original frame and adds the model's edit to it,
// scaled by the local slope of the curve -- which for Reinhard against a white point works out to the
// tidy (whitePoint + luminance), so a one percent edit on a bright light stays a one percent edit. At
// zero edit the frame is bit-for-bit what the upscaler produced.
//
// On top of that the edit is rolled off as the proxy approaches white, and the total change is clamped
// to a ratio of the original. Neither should be load-bearing given the above; they are there because a
// detail pass has no business restyling a light source, whatever the model returns.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

namespace codec
{
constexpr int MODE_ENCODE = 0;
constexpr int MODE_RESOLVE = 1;
// Shrinks the frame so the model can work on fewer pixels. Filtered, not point sampled: the guidance is
// explicit that a nearest-neighbour enlargement of this pass turns into harsh aliasing.
constexpr int MODE_DOWNSAMPLE = 2;

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
    float gMaxRatio;
    uint  gPassthrough;
    uint  gAccumulate;   // 0 off, 1 blend with the reprojected history, 2 restart the history
    float gMvScaleX;     // motion vector units -> pixels of this dispatch
    float gMvScaleY;
    uint  gGuideWidth;   // the motion texture's valid region
    uint  gGuideHeight;
    float gStability;    // how much of the history survives each frame; 0 is off
    float gNoiseFloor;   // edits below this are squashed toward zero; 0 is off
    float gProtectHighlights; // the top fraction of the range where the edit fades out; 0 is off
};

Texture2D<float4>   gSource   : register(t0);  // encode: the frame. resolve: the proxy.
Texture2D<float4>   gModel    : register(t1);  // resolve: what the model returned.
Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
Texture2D<float4>   gMotion   : register(t3);  // resolve, accumulating: the game's motion vectors.
Texture2D<float4>   gPrevEdit : register(t4);  // resolve, accumulating: last frame's accumulated edit.
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy. resolve: the edit history.
SamplerState        gLinear   : register(s0);  // so the edit can be read at a different size

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

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    // Normalised, so the source may be any size relative to this dispatch.
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    if (gMode == 2)
    {
        gTarget[id.xy] = gSource.SampleLevel(gLinear, uv, 0);
        return;
    }

    if (gMode == 0)
    {
        float4 source = gSource.Load(int3(id.xy, 0));
        float3 frame = max(source.rgb, float3(0.0, 0.0, 0.0));

        // Kept so the resolve has the frame as it was, rather than having to reconstruct it.
        gKeep[id.xy] = float4(frame, source.a);

        // Some games hand DLSS a frame that has already been through their tonemapper. The game says
        // which in its own DLSS creation flags, and converting one that needs no conversion is pure
        // damage, so it goes through untouched.
        if (gPassthrough != 0)
        {
            gTarget[id.xy] = float4(frame, source.a);
            return;
        }

        // Reinhard on luminance alone, with chroma carried along untouched. Compressing each channel
        // separately is what shifted the hue of every saturated highlight.
        float luma = dot(frame, kLuma);
        float toned = (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;

        gTarget[id.xy] = float4(LinearToSrgb(frame * scale), source.a);
        return;
    }

    // Sampled rather than loaded: when the model ran at a reduced resolution these are smaller than the
    // frame, and its edit is enlarged here while the frame underneath stays untouched.
    float4 proxySample = gSource.SampleLevel(gLinear, uv, 0);
    float4 modelSample = gModel.SampleLevel(gLinear, uv, 0);

    // Nothing was encoded on the way in, so nothing is decoded here either.
    float3 proxy = gPassthrough != 0 ? proxySample.rgb : SrgbToLinear(proxySample.rgb);
    float3 model = gPassthrough != 0 ? modelSample.rgb : SrgbToLinear(modelSample.rgb);
    float4 originalSample = gOriginal.Load(int3(id.xy, 0));
    float3 original = originalSample.rgb;

    // The slope of the encode at this pixel, so an edit made in the compressed picture lands as the
    // equivalent edit in the original. For Reinhard against a white point this is exactly
    // whitePoint + luminance -- bounded everywhere, unlike the inverse of the curve.
    float originalLuma = dot(original, kLuma);
    // With no curve applied there is no slope to undo: the edit lands as it is.
    float slope = gPassthrough != 0 ? 1.0 : gWhitePoint + originalLuma;

    if (gDebugView == 1)
    {
        gTarget[id.xy] = float4(proxy * gWhitePoint, originalSample.a);
        return;
    }

    if (gDebugView == 2)
    {
        gTarget[id.xy] = float4(model * gWhitePoint, originalSample.a);
        return;
    }

    float3 edit = model - proxy;

    // Coring. The churn the model re-decides every frame is small-amplitude and unstructured, while
    // the detail worth keeping -- occlusion, contact shadows, synthesised texture -- is larger and
    // structured. Edits below the floor are squashed toward zero, edits above it pass untouched, and
    // the ramp between the two keeps the transition invisible. At 0 this does nothing at all.
    if (gNoiseFloor > 0.0)
    {
        float editSize = max(abs(edit.r), max(abs(edit.g), abs(edit.b)));
        edit *= smoothstep(gNoiseFloor * 0.5, gNoiseFloor * 1.5, editSize);
    }

    if (gDebugView == 3)
    {
        // Amplified and centred on grey, so both directions of the edit are visible at once. Shows the
        // edit as it will land -- after coring -- so the Noise floor slider is judged here too.
        float3 shown = saturate(0.5 + edit * 20.0);
        gTarget[id.xy] = float4(SrgbToLinear(shown) * gWhitePoint, originalSample.a);
        return;
    }

    // The edit, averaged over time. The model re-decides a measurable fraction of its answer every
    // frame even on a static scene; blending each frame's edit with its own reprojected history keeps
    // the consistent part -- the detail -- and cancels the part that re-randomises. NVIDIA's own
    // motion vectors carry the history to where the surface is now.
    if (gAccumulate != 0)
    {
        float3 accumulated = edit;

        if (gAccumulate == 1)
        {
            float2 mv = gMotion.Load(int3(uv * float2(gGuideWidth, gGuideHeight), 0)).xy *
                        float2(gMvScaleX, gMvScaleY);
            float2 uvPrev = uv + mv / float2(gWidth, gHeight);

            if (all(uvPrev >= 0.0) && all(uvPrev <= 1.0))
            {
                float3 prev = gPrevEdit.SampleLevel(gLinear, uvPrev, 0).rgb;

                // Rectified before blending: the history may not stray far from what the model says
                // now, so a stale edit at a disocclusion is pulled in within a frame or two instead of
                // being carried -- and then amplified -- indefinitely. The margin scales with the edit
                // so strong legitimate detail is not clipped, with a floor so the noise floor itself
                // can still cancel.
                float3 bound = abs(edit) * 1.5 + 0.015;
                prev = clamp(prev, edit - bound, edit + bound);

                accumulated = lerp(edit, prev, gStability);
            }
        }

        gKeep[id.xy] = float4(accumulated, 1.0);
        edit = accumulated;
    }

    // Split so the detail the model synthesised and any colour it shifted can be dialled apart.
    float lumaEdit = dot(edit, kLuma);
    float3 colourEdit = edit - lumaEdit;
    float3 applied = lumaEdit * gTransferStrength + colourEdit * gColourStrength;

    // Protect highlights. The model was trained to produce finished, tone-mapped pictures, so its
    // instinct at an extreme highlight -- a lamp, a neon sign -- is to calm it toward its trained
    // statistics. Structure detail lives everywhere else; the punch of a light lives exactly there.
    // The edit fades out over the top fraction of the range, so the model keeps its say everywhere
    // except the peaks. 0 is off.
    if (gProtectHighlights > 0.0)
    {
        float relLuma = saturate(dot(original, kLuma) / max(gWhitePoint, 1e-4));
        applied *= 1.0 - smoothstep(1.0 - gProtectHighlights, 1.0, relLuma);
    }

    // No highlight rolloff. It was a second belt after the clamp below, and it discarded the model's
    // contribution exactly where a lit scene carries its punch -- the two inject points now apply the
    // edit identically, with the clamp as the one safety in both.

    float3 result = original + applied * slope;

    // A detail pass should not be able to restyle anything, whatever comes back. The small constant
    // keeps this meaningful where the original is near black and a ratio alone would not be.
    float3 ceiling = original * gMaxRatio + 0.01;
    float3 floorValue = max(original / gMaxRatio - 0.01, float3(0.0, 0.0, 0.0));

    gTarget[id.xy] = float4(clamp(result, floorValue, ceiling), originalSample.a);
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
    float maxRatio;
    // Set when the game's own buffer is already tone-mapped, in which case there is nothing to convert.
    unsigned int passthrough;
    // 0 off, 1 blend with the reprojected history, 2 restart the history.
    unsigned int accumulate;
    float mvScaleX;
    float mvScaleY;
    unsigned int guideWidth;
    unsigned int guideHeight;
    float stability;
    float noiseFloor;
    float protectHighlights;
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
        ranges[0].NumDescriptors = 5; // proxy, model, original, motion, previous edit
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2; // result, kept copy
        ranges[1].OffsetInDescriptorsFromTableStart = 5;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = sizeof(Params) / 4;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &sampler;

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

        // Five descriptors per dispatch, two dispatches a frame; a ring of eight keeps a frame's
        // descriptors from being overwritten while it is still in flight.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kRingSlots * kPerDispatch;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_))))
            return false;

        stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device_ = device;
        return true;
    }

    // Every texture must already be in the state its slot needs: sources shader-readable, targets
    // writable. Slots a pass does not read still have to be populated, or the descriptor is undefined.
    void dispatch(ID3D12GraphicsCommandList* cmd, const Params& constants, ID3D12Resource* source,
                  ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* target,
                  ID3D12Resource* keep, ID3D12Resource* motion = nullptr,
                  ID3D12Resource* prevEdit = nullptr)
    {
        if (pipeline_ == nullptr)
            return;

        const unsigned int slot = ring_;
        ring_ = (ring_ + 1) % kRingSlots;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T) slot * kPerDispatch * stride_;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += (UINT64) slot * kPerDispatch * stride_;

        ID3D12Resource* srvs[5] = { source, model != nullptr ? model : source,
                                    original != nullptr ? original : source,
                                    motion != nullptr ? motion : source,
                                    prevEdit != nullptr ? prevEdit : source };

        for (int i = 0; i < 5; ++i)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            srv.Format = TypedFormat(srvs[i]->GetDesc().Format);

            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
            handle.ptr += (SIZE_T) i * stride_;
            device_->CreateShaderResourceView(srvs[i], &srv, handle);
        }

        ID3D12Resource* uavs[2] = { target, keep != nullptr ? keep : target };

        for (int i = 0; i < 2; ++i)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = TypedFormat(uavs[i]->GetDesc().Format);

            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
            handle.ptr += (SIZE_T) (5 + i) * stride_;
            device_->CreateUnorderedAccessView(uavs[i], nullptr, &uav, handle);
        }

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
    static const unsigned int kPerDispatch = 7;

    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    unsigned int stride_ = 0;
    unsigned int ring_ = 0;
};
} // namespace codec
