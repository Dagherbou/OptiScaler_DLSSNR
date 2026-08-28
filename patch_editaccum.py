"""Accumulates the model's edit across frames, reprojected by the game's own motion vectors.

The matched-frame capture measured the wobble at its root: on a completely static scene, the model
re-decides about a fifth of its edit every frame. Routing the pass through DLSS's temporal accumulator
proved that averaging that noise over time removes it -- but that arrangement cannot coexist with Ray
Reconstruction, whose input is pre-denoise noise.

So the same mechanism is applied to the only quantity this integration isolates and nobody else does:
the edit itself. Last frame's accumulated edit is reprojected by the game's motion vectors and blended
with this frame's, and the blend is what lands on the picture. The consistent part of the edit -- the
actual detail -- survives the average; the re-randomised part cancels.

Ghosting in the edit domain is far more forgiving than in the image domain: a slightly misplaced
enhancement reads as softness, not as a smeared object. That is what makes a hand-rolled accumulator
safe here when it would not be for whole frames. Where the reprojection leaves the screen the blend
falls back to the current edit, and a feature reset restarts the history.

Off by default. Zero means bit-identical to before.
"""

import io

ROOT = "C:/Games_Temp/OptiScaler/OptiScaler/"

# =====================================================================================================
# Codec: two more SRVs (motion vectors, previous edit), an accumulation output, and the blend.
# =====================================================================================================

p = ROOT + "dlssnr/DlssNr_Codec.h"
t = io.open(p, encoding="utf-8").read()

old = """    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    uint  gPad;
};"""
new = """    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    uint  gAccumulate;   // 0 off, 1 blend with the reprojected history, 2 restart the history
    float gMvScaleX;     // motion vector units -> pixels of this dispatch
    float gMvScaleY;
    uint  gGuideWidth;   // the motion texture's valid region
    uint  gGuideHeight;
    float gStability;    // how much of the history survives each frame; 0 is off
    uint  gPad;
};"""
assert old in t
t = t.replace(old, new, 1)

old = """Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy."""
new = """Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
Texture2D<float4>   gMotion   : register(t3);  // resolve, accumulating: the game's motion vectors.
Texture2D<float4>   gPrevEdit : register(t4);  // resolve, accumulating: last frame's accumulated edit.
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy. resolve: the edit history."""
assert old in t
t = t.replace(old, new, 1)

old = """    float3 edit = model - proxy;

    // Split so the detail the model synthesised and any colour it shifted can be dialled apart."""
new = """    float3 edit = model - proxy;

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
                accumulated = lerp(edit, prev, gStability);
            }
        }

        gKeep[id.xy] = float4(accumulated, 1.0);
        edit = accumulated;
    }

    // Split so the detail the model synthesised and any colour it shifted can be dialled apart."""
assert old in t
t = t.replace(old, new, 1)

old = """        ranges[0].NumDescriptors = 3; // proxy, model, original"""
new = """        ranges[0].NumDescriptors = 5; // proxy, model, original, motion, previous edit"""
assert old in t
t = t.replace(old, new, 1)

old = """    static const unsigned int kPerDispatch = 5;"""
new = """    static const unsigned int kPerDispatch = 7;"""
assert old in t
t = t.replace(old, new, 1)

old = """    void dispatch(ID3D12GraphicsCommandList* cmd, const Params& constants, ID3D12Resource* source,
                  ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* target,
                  ID3D12Resource* keep)"""
new = """    void dispatch(ID3D12GraphicsCommandList* cmd, const Params& constants, ID3D12Resource* source,
                  ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* target,
                  ID3D12Resource* keep, ID3D12Resource* motion = nullptr,
                  ID3D12Resource* prevEdit = nullptr)"""
assert old in t
t = t.replace(old, new, 1)

old = """        ID3D12Resource* srvs[3] = { source, model != nullptr ? model : source,
                                    original != nullptr ? original : source };

        for (int i = 0; i < 3; ++i)"""
new = """        ID3D12Resource* srvs[5] = { source, model != nullptr ? model : source,
                                    original != nullptr ? original : source,
                                    motion != nullptr ? motion : source,
                                    prevEdit != nullptr ? prevEdit : source };

        for (int i = 0; i < 5; ++i)"""
assert old in t
t = t.replace(old, new, 1)

old = """            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
            handle.ptr += (SIZE_T) (3 + i) * stride_;"""
new = """            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
            handle.ptr += (SIZE_T) (5 + i) * stride_;"""
assert old in t
t = t.replace(old, new, 1)

io.open(p, "w", encoding="utf-8").write(t)
print("codec accumulates")

# =====================================================================================================
# Module: a ping-pong pair of edit-history textures, wired into the before-frame-generation resolve.
# =====================================================================================================

p = ROOT + "dlssnr/DlssNr_Dx12.cpp"
t = io.open(p, encoding="utf-8").read()

old = """    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;"""
new = """    // The accumulated edit, double-buffered: one read while the other is written. Always float, since
    // an edit is signed and the frame's own format generally is not.
    ID3D12Resource* editHistory[2] = {};
    unsigned int editIndex = 0;
    bool editWarm = false;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;"""
assert old in t
t = t.replace(old, new, 1)

# Release with everything else in Shutdown.
old = """    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }

    g_capture.release();"""
new = """    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }

    for (auto& h : g_nr.editHistory)
    {
        if (h != nullptr)
        {
            h->Release();
            h = nullptr;
        }
    }

    g_capture.release();"""
assert old in t
t = t.replace(old, new, 1)

# Wire into the before-frame-generation resolve (line ~1232 site: proxy, output, hdrCopy, target).
old = """        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.passthrough = isHdrBuffer ? 0u : 1u;

        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_codec.dispatch(cmdList, resolveParams, g_nr.colorCopy, g_nr.output, g_nr.hdrCopy,
                         target, nullptr);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);"""
new = """        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.passthrough = isHdrBuffer ? 0u : 1u;

        // The accumulator: this frame's edit blended with its own history, carried to where the surface
        // is now by the same motion vectors the model was given.
        const float stability = cfg.DlssNrEditStability.value_or_default();
        ID3D12Resource* historyIn = nullptr;
        ID3D12Resource* historyOut = nullptr;

        if (stability > 0.0f && depthIn != nullptr && motionIn != nullptr)
        {
            if (g_nr.editHistory[0] == nullptr)
            {
                g_nr.editHistory[0] = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
                g_nr.editHistory[1] = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
                g_nr.editWarm = false;
            }

            if (g_nr.editHistory[0] != nullptr && g_nr.editHistory[1] != nullptr)
            {
                historyIn = g_nr.editHistory[g_nr.editIndex];
                historyOut = g_nr.editHistory[1 - g_nr.editIndex];

                // 2 restarts the history: the first frame, and any frame after a reset, has nothing
                // valid behind it.
                resolveParams.accumulate = g_nr.editWarm ? 1u : 2u;
                resolveParams.stability = stability > 0.95f ? 0.95f : stability;
                resolveParams.mvScaleX = g_nr.guideMvScaleX;
                resolveParams.mvScaleY = g_nr.guideMvScaleY;
                resolveParams.guideWidth = guideWidth;
                resolveParams.guideHeight = guideHeight;

                Barrier(cmdList, historyIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }

        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_codec.dispatch(cmdList, resolveParams, g_nr.colorCopy, g_nr.output, g_nr.hdrCopy,
                         target, historyOut, motionIn, historyIn);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (historyIn != nullptr)
        {
            Barrier(cmdList, historyIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_nr.editIndex = 1 - g_nr.editIndex;
            g_nr.editWarm = true;
        }"""
assert old in t, "before-FG resolve site not found"
t = t.replace(old, new, 1)

# A reset invalidates the history along with everything else.
old = """    g_nr.width = 0;
    g_nr.height = 0;
    g_nr.rebuild = false;
    g_nr.reset = true;
}"""
if old in t:
    new = """    g_nr.width = 0;
    g_nr.height = 0;
    g_nr.rebuild = false;
    g_nr.reset = true;
    g_nr.editWarm = false;
}"""
    t = t.replace(old, new, 1)

io.open(p, "w", encoding="utf-8").write(t)
print("module accumulates on the before-FG path")

# =====================================================================================================
# Config and menu.
# =====================================================================================================

p = ROOT + "Config.h"
t = io.open(p, encoding="utf-8").read()
old = "    CustomOptional<bool> DlssNrAutoCapture { true };"
new = """    CustomOptional<bool> DlssNrAutoCapture { true };

    // How much of the edit's history survives each frame, 0 to 0.95. The model re-decides a measured
    // fraction of its edit every frame even on a static scene; accumulating the edit keeps the
    // consistent detail and cancels the re-randomised part. 0 is off and bit-identical to before.
    CustomOptional<float> DlssNrEditStability { 0.0f };"""
assert old in t
t = t.replace(old, new, 1)
io.open(p, "w", encoding="utf-8").write(t)

p = ROOT + "Config.cpp"
t = io.open(p, encoding="utf-8").read()
old = '            DlssNrAutoCapture.set_from_config(readBool("DlssNr", "AutoCapture"));'
assert old in t
t = t.replace(old, old + '\n            DlssNrEditStability.set_from_config(readFloat("DlssNr", "EditStability"));', 1)
old = '    ini.SetValue("DlssNr", "AutoCapture", GetBoolValue(Instance()->DlssNrAutoCapture.value_for_config()).c_str());'
assert old in t
t = t.replace(old, old + '\n    ini.SetValue("DlssNr", "EditStability", GetFloatValue(Instance()->DlssNrEditStability.value_for_config()).c_str());', 1)
io.open(p, "w", encoding="utf-8").write(t)
print("config added")

NL = chr(92) + "n"
p = ROOT + "menu/menu_common.cpp"
t = io.open(p, encoding="utf-8").read()
old = '''        ShowHelpMarker("The same, for the colour part of the edit, which is separated out because it is"'''
assert old in t
insert = ('''        float editStability = config->DlssNrEditStability.value_or_default();
        if (ImGui::SliderFloat("Temporal stability", &editStability, 0.0f, 0.95f, "%.2f"))
            config->DlssNrEditStability = editStability;

        ShowHelpMarker("Averages the model's edit across frames, carried to where each surface is now by"
                       "@the game's own motion vectors. The model re-decides about a fifth of its edit"
                       "@every frame even on a static scene -- measured directly -- and that is the"
                       "@wobble. Averaging keeps the consistent detail and cancels the noise."
                       "@@0 is off and bit-identical. 0.6 to 0.8 is the useful range. Too high and the"
                       "@detail lags a step behind fast motion, which reads as softness rather than"
                       "@smearing -- the edit is small, so its ghosts are too."
                       "@@Before-frame-generation inject point only; before the upscaler, DLSS's own"
                       "@accumulator already does this.");

''').replace("@", NL)
# Place it before the colour-strength help marker's slider block: find that slider instead.
anchor = '''        float colour = config->DlssNrColourStrength.value_or_default();'''
assert anchor in t
t = t.replace(anchor, insert + anchor, 1)
io.open(p, "w", encoding="utf-8").write(t)
print("menu added")
