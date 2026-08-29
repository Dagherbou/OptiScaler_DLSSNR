import io

# =========== CODEC: lighting-only accumulation, DLAA-mode removal, highlight restore ===========
p = 'OptiScaler/dlssnr/DlssNr_Codec.h'
t = io.open(p, encoding='utf-8').read()

# mode 4 out
old = """
    if (gMode == 4)
    {
        // The edit as a picture: grey is zero, negatives below it, positives above -- so a trained
        // video model can be handed the edit and asked to stabilise it like footage.
        float3 e = EditAt(uv);
        gTarget[id.xy] = float4(saturate(0.5 + e * 0.5), 1.0);
        return;
    }"""
assert old in t
t = t.replace(old, '', 1)

# accumulate==3 decode out
old = """
    // The stabilised edit, when a trained temporal model did the accumulating: decoded from the
    // grey-centred picture it was given. The hand-made accumulator below is bypassed.
    if (gAccumulate == 3)
        edit = (gPrevEdit.SampleLevel(gLinear, uv, 0).rgb - 0.5) * 2.0;
"""
assert old in t
t = t.replace(old, '', 1)

# Catmull-Rom helper out (nothing uses it any more)
start = t.find('// Catmull-Rom resampling of the edit history.')
end = t.find('[numthreads(8, 8, 1)]')
assert 0 < start < end
t = t[:start] + t[end:]

# the accumulate block becomes lighting-only
start = t.find('    if (gAccumulate == 1 || gAccumulate == 2)')
marker = '        edit = accumulatedHigh + accumulatedLow;\n    }'
end = t.find(marker)
assert 0 < start < end
new_block = """    if (gAccumulate == 1 || gAccumulate == 2)
    {
        // Only the lighting is accumulated. Two unrelated temporal filters -- the hand-made variance
        // clip and a trained DLAA pass -- both under-stabilised the detail band and both ghosted on
        // it, which settles the question: the edit's detail is re-decided with the framing rather
        // than attached to surfaces, so reprojecting it mixes genuinely different answers. History
        // cannot fix that band; routing the pass through a real upscaler accumulator (the split)
        // can. The lighting band is smooth and forgiving, and its accumulation measurably kills the
        // pumping without ghosts -- so that is all this does. History: alpha = lighting, rgb unused.
        float2 px = 1.0 / float2(gWidth, gHeight);
        float lowNow = dot(edit, kLuma);

        const float2 kWide[8] = { float2(-0.7, -0.2), float2(0.6, -0.6), float2(0.2, 0.7),
                                  float2(-0.5, 0.5),  float2(0.9, 0.1),  float2(-0.9, -0.6),
                                  float2(0.1, -0.9),  float2(0.5, 0.3) };

        {
            [unroll]
            for (int i = 0; i < 8; ++i)
                lowNow += dot(EditAt(uv + kWide[i] * 12.0 * px), kLuma);

            lowNow /= 9.0;
        }

        float accumulatedLow = lowNow;

        if (gAccumulate == 1)
        {
            float2 mv = gMotion.Load(int3(uv * float2(gGuideWidth, gGuideHeight), 0)).xy *
                        float2(gMvScaleX, gMvScaleY);
            float2 uvPrev = uv + mv / float2(gWidth, gHeight);

            if (all(uvPrev >= 0.0) && all(uvPrev <= 1.0))
            {
                // Where the motion field disagrees with itself across this band's footprint -- a car
                // against a streaming road -- history dies outright: hold and clamp both collapse, so
                // nothing trails. Where the field is coherent, the hold is strong and the pumping
                // cannot survive it.
                float divergence = 0.0;

                [unroll]
                for (int k = 0; k < 4; ++k)
                {
                    float2 tapUv = uv + kWide[k * 2] * 12.0 * px;
                    float2 mvTap = gMotion.Load(int3(tapUv * float2(gGuideWidth, gGuideHeight), 0)).xy *
                                   float2(gMvScaleX, gMvScaleY);
                    divergence = max(divergence, length(mvTap - mv));
                }

                float coherent = 1.0 - saturate(divergence / 2.0);
                float lowHold = coherent * max(gStability, 0.9);
                float clampWidth = lerp(0.015, 0.10, coherent);

                float prevLow = gPrevEdit.SampleLevel(gLinear, uvPrev, 0).a;
                prevLow = clamp(prevLow, lowNow - clampWidth, lowNow + clampWidth);
                accumulatedLow = lerp(lowNow, prevLow, lowHold);
            }
        }

        gKeep[id.xy] = float4(0.0, 0.0, 0.0, accumulatedLow);
        edit += accumulatedLow - lowNow;
    }"""
t = t[:start] + new_block + t[end + len(marker):]

# highlight restore replaces the near-white fade
old = """    // Protect highlights. The model was trained to produce finished, tone-mapped pictures, so its
    // instinct at an extreme highlight -- a lamp, a neon sign -- is to calm it toward its trained
    // statistics. Structure detail lives everywhere else; the punch of a light lives exactly there.
    // The edit fades out over the top fraction of the range, so the model keeps its say everywhere
    // except the peaks. 0 is off.
    if (gProtectHighlights > 0.0)
    {
        float relLuma = saturate(dot(original, kLuma) / max(gWhitePoint, 1e-4));
        applied *= 1.0 - smoothstep(1.0 - gProtectHighlights, 1.0, relLuma);
    }"""
assert old in t
t = t.replace(old, """    // Highlight restore. The model's trained instinct is to calm bright things -- not only the
    // near-clipped peak but the whole glow around a lamp -- and that reads as muted, in SDR too.
    // The achromatic darkening of bright regions is pulled back, scaled by how bright the original
    // is; colour shifts, brightening and structure pass untouched, so the model's detail stays.
    // 0 is off; 1 removes all darkening from the brightest regions.
    if (gProtectHighlights > 0.0)
    {
        float relLuma = saturate(dot(original, kLuma) / max(gWhitePoint, 1e-4));
        float appliedLuma = dot(applied, kLuma);

        if (appliedLuma < 0.0)
            applied -= appliedLuma * gProtectHighlights * smoothstep(0.25, 0.9, relLuma);
    }""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('codec reworked')

# =========== MODULE: remove the DLAA experiment entirely ===========
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

old = """
#include <upscalers/dlss/DLSSFeature_Dx12.h>"""
assert old in t
t = t.replace(old, '', 1)

old = """

    // DLAA-on-the-edit: NVIDIA's own temporal model stabilising the edit, as an experiment toggle.
    std::unique_ptr<DLSSFeatureDx12> dlaa;
    ID3D12Resource* editImage = nullptr;  // the edit, grey-centred, as a picture
    ID3D12Resource* editStable = nullptr; // what the model made of it
    unsigned int dlaaWidth = 0;
    bool dlaaJustCreated = false;"""
assert old in t
t = t.replace(old, '', 1)

start = t.find('// DLAA-on-the-edit support: a session latch')
end = t.find('bool EnsureDlaaKit')
end2 = t.find('\n}', t.find('return g_dlaaEvent != nullptr;'))
assert 0 < start < end < end2
t = t[:start] + t[end2 + 2:]

start = t.find('// The wildcard: the edit is encoded as a grey-centred picture')
end = t.find('void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,')
assert 0 < start < end
t = t[:start] + t[end:]

# render call site
old = """        // The wildcard first, when asked: a trained temporal model stabilises the edit, and the
        // hand-made accumulator below stands down for the frame.
        bool dlaaEdit = false;

        if (cfg.DlssNrDlaaEdit.value_or_default())
            dlaaEdit = EvaluateDlaaOnEdit(cmdList, device, params, modelInput, depthIn, motionIn, width,
                                          height, guideWidth, guideHeight, g_nr.guideMvScaleX,
                                          g_nr.guideMvScaleY, isHdrBuffer ? 0u : 1u);

        // Resolve takes"""
assert old in t
t = t.replace(old, """        // Resolve takes""", 1)

old = """
        if (dlaaEdit)
            resolveParams.accumulate = 3u;

        if (!dlaaEdit && stability > 0.0f && motionIn != nullptr)"""
assert old in t
t = t.replace(old, """
        if (stability > 0.0f && motionIn != nullptr)""", 1)

old = """        g_codec.dispatch(cmdList, resolveParams, modelInput, g_nr.output, g_nr.hdrCopy, target,
                         historyOut, motionIn, dlaaEdit ? g_nr.editStable : historyIn);"""
assert old in t
t = t.replace(old, """        g_codec.dispatch(cmdList, resolveParams, modelInput, g_nr.output, g_nr.hdrCopy, target,
                         historyOut, motionIn, historyIn);""", 1)

old = """        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (dlaaEdit)
            Barrier(cmdList, g_nr.editStable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (historyIn != nullptr)
        {
            Barrier(cmdList, historyIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_nr.editIndex = 1 - g_nr.editIndex;
            g_nr.editWarm = true;
        }
"""
assert t.count(old) >= 1
t = t.replace(old, """        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (historyIn != nullptr)
        {
            Barrier(cmdList, historyIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_nr.editIndex = 1 - g_nr.editIndex;
            g_nr.editWarm = true;
        }
""")

# present call site
old = """        // The wildcard works here too -- and the finished frame at DLAA is its best case, since the
        // guides sit exactly at the frame's size.
        bool dlaaEdit = false;

        if (cfg.DlssNrDlaaEdit.value_or_default() && g_nr.capabilityParams != nullptr)
        {
            // No game parameter block exists at present time; the forwarder's capability block serves
            // -- parameter blocks are generic driver containers, as the float-slot probing proved.
            // The depth clone is already parked for the next frame's copy; borrow it briefly.
            Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            dlaaEdit = EvaluateDlaaOnEdit(cmdList, device, g_nr.capabilityParams, modelInput,
                                          g_nr.depthClone, g_nr.motionClone, width, height,
                                          g_nr.guideWidth, g_nr.guideHeight, g_nr.guideMvScaleX,
                                          g_nr.guideMvScaleY, hdrEncoded ? 0u : 1u);
            Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        }

        // For SDR the frame"""
assert old in t
t = t.replace(old, """        // For SDR the frame""", 1)

old = """
        if (dlaaEdit)
            resolveParams.accumulate = 3u;

        if (!dlaaEdit && stability > 0.0f && g_nr.motionClone != nullptr)"""
assert old in t
t = t.replace(old, """
        if (stability > 0.0f && g_nr.motionClone != nullptr)""", 1)

old = """        g_codec.dispatch(cmdList, resolveParams, modelInput, g_nr.output, g_nr.colorCopy, g_nr.hdrCopy,
                         historyOut, g_nr.motionClone, dlaaEdit ? g_nr.editStable : historyIn);"""
assert old in t
t = t.replace(old, """        g_codec.dispatch(cmdList, resolveParams, modelInput, g_nr.output, g_nr.colorCopy, g_nr.hdrCopy,
                         historyOut, g_nr.motionClone, historyIn);""", 1)

# shutdown
old = """
    g_nr.dlaa.reset();

    if (g_nr.editImage != nullptr)
    {
        g_nr.editImage->Release();
        g_nr.editImage = nullptr;
    }

    if (g_nr.editStable != nullptr)
    {
        g_nr.editStable->Release();
        g_nr.editStable = nullptr;
    }
"""
assert old in t
t = t.replace(old, '', 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('module dlaa removed')

# =========== CONFIG ===========
p = 'OptiScaler/Config.h'
t = io.open(p, encoding='utf-8').read()
old = """

    // The wildcard: an internal DLAA-mode DLSS feature stabilises the edit (encoded as a grey-centred
    // picture) instead of the hand-made accumulator. Experimental; split pipeline or DLAA games only.
    CustomOptional<bool> DlssNrDlaaEdit { false };"""
assert old in t
t = t.replace(old, '', 1)
io.open(p, 'w', encoding='utf-8').write(t)

p = 'OptiScaler/Config.cpp'
t = io.open(p, encoding='utf-8').read()
old = """
            DlssNrDlaaEdit.set_from_config(readBool("DlssNr", "DlaaEdit"));"""
assert old in t
t = t.replace(old, '', 1)
old = """
    ini.SetValue("DlssNr", "DlaaEdit", GetBoolValue(Instance()->DlssNrDlaaEdit.value_for_config()).c_str());"""
assert old in t
t = t.replace(old, '', 1)
io.open(p, 'w', encoding='utf-8').write(t)
print('config cleaned')
