"""Lets the model run at a lower resolution than the frame, and upsamples only its edit.

The published guidance ties the model's resolution to the render scale, by moving the pass ahead of the
upscaler. At native resolution that buys nothing, because render and display are the same size.

Isolating the edit makes a better arrangement possible. The frame stays exactly as it was at full
resolution; only the model's *contribution* is computed small and enlarged. Cost falls with the square of
the scale regardless of what the upscaler is doing, and the underlying image is untouched by
construction.

What it trades: the model contributes local shading, which is low frequency and survives enlargement,
and fine structure, which does not. Half resolution keeps most of the former and softens the latter. A
real trade rather than a free win, so it is a control rather than a default -- full resolution stays the
default and behaves exactly as before.
"""

import io

# --- codec: a downsample pass, and sampling the edit at whatever size it was computed -------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Codec.h"
text = io.open(PATH, encoding="utf-8").read()

text = text.replace("""constexpr int MODE_ENCODE = 0;
constexpr int MODE_RESOLVE = 1;""",
"""constexpr int MODE_ENCODE = 0;
constexpr int MODE_RESOLVE = 1;
// Shrinks the frame so the model can work on fewer pixels. Filtered, not point sampled: the guidance is
// explicit that a nearest-neighbour enlargement of this pass turns into harsh aliasing.
constexpr int MODE_DOWNSAMPLE = 2;""", 1)

old = """Texture2D<float4>   gSource   : register(t0);  // encode: the frame. resolve: the proxy.
Texture2D<float4>   gModel    : register(t1);  // resolve: what the model returned.
Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy."""
new = """Texture2D<float4>   gSource   : register(t0);  // encode: the frame. resolve: the proxy.
Texture2D<float4>   gModel    : register(t1);  // resolve: what the model returned.
Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy.
SamplerState        gLinear   : register(s0);  // so the edit can be read at a different size"""
assert old in text
text = text.replace(old, new, 1)

old = """[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    if (gMode == 0)"""
new = """[numthreads(8, 8, 1)]
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

    if (gMode == 0)"""
assert old in text
text = text.replace(old, new, 1)

# The proxy and the model may be smaller than the frame; the frame itself never is.
old = """    float4 proxySample = gSource.Load(int3(id.xy, 0));
    float4 modelSample = gModel.Load(int3(id.xy, 0));"""
new = """    // Sampled rather than loaded: when the model ran at a reduced resolution these are smaller than the
    // frame, and its edit is enlarged here while the frame underneath stays untouched.
    float4 proxySample = gSource.SampleLevel(gLinear, uv, 0);
    float4 modelSample = gModel.SampleLevel(gLinear, uv, 0);"""
assert old in text
text = text.replace(old, new, 1)

# A static sampler on the root signature.
old = """        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;"""
new = """        D3D12_STATIC_SAMPLER_DESC sampler = {};
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
        rootDesc.pStaticSamplers = &sampler;"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("codec can downsample and enlarge the edit")

# --- config ------------------------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/Config.h"
text = io.open(PATH, encoding="utf-8").read()
old = "    CustomOptional<uint32_t> DlssNrDebugView { 0 };"
new = """    CustomOptional<uint32_t> DlssNrDebugView { 0 };

    // The fraction of the frame's resolution the model works at. The frame itself is never reduced --
    // only the model's contribution is computed small and enlarged, so the picture underneath is
    // untouched whatever this is set to. 1.0 is full resolution and behaves exactly as before.
    CustomOptional<float> DlssNrWorkingScale { 1.0f };"""
assert old in text
text = text.replace(old, new, 1)
io.open(PATH, "w", encoding="utf-8").write(text)

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/Config.cpp"
text = io.open(PATH, encoding="utf-8").read()
old = '            DlssNrDebugView.set_from_config(readUInt("DlssNr", "DebugView"));'
assert old in text
text = text.replace(old, old + '\n            DlssNrWorkingScale.set_from_config(readFloat("DlssNr", "WorkingScale"));', 1)
old = '    ini.SetValue("DlssNr", "DebugView", GetIntValue(Instance()->DlssNrDebugView.value_for_config()).c_str());'
assert old in text
text = text.replace(old, old + '\n    ini.SetValue("DlssNr", "WorkingScale", GetFloatValue(Instance()->DlssNrWorkingScale.value_for_config()).c_str());', 1)
io.open(PATH, "w", encoding="utf-8").write(text)
print("config option added")
