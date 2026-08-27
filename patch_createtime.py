"""Sets the model's tuning when the feature is created, not only when it is evaluated.

Intensity, style, local structure, local tone, skin structure and the auto mask have never done anything
in either integration. The reason is visible in what the forwarder sets and when: everything written at
create time -- enabled, width, height, the preset -- works, and everything written only at evaluate time
does not. The split is exact.

So the model reads its tuning once, when the feature is built, exactly as it does the preset. Writing
those values every frame afterwards was never going to matter.

They are still written at evaluate as well. It costs nothing, and if some of them do turn out to be read
live then both behaviours are correct.
"""

import io

# --- the forwarder ---------------------------------------------------------------------------------

for path in ("C:/Games_Temp/dlssnr-forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp"):
    text = io.open(path, encoding="utf-8").read()

    old = """__declspec(dllexport) void *dlssnr_call_create(const wchar_t *snippetPath, const wchar_t *dataPath,
                                               ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                                               void *capabilityParams, unsigned int width,
                                               unsigned int height, int preset) {"""
    new = """__declspec(dllexport) void *dlssnr_call_create(const wchar_t *snippetPath, const wchar_t *dataPath,
                                               ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                                               void *capabilityParams, unsigned int width,
                                               unsigned int height, int preset, float intensity,
                                               int style, float localStructure, float localTone,
                                               float skinStructure, int useAutoMask) {"""
    assert old in text, "create signature not found in " + path
    text = text.replace(old, new, 1)

    old = """    if (preset != 0) {
        setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);
    }"""
    new = """    if (preset != 0) {
        setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);
    }

    // The tuning has to be here rather than at evaluate. Everything this sets before create takes
    // effect; everything set only at evaluate is ignored, which is why none of these controls did
    // anything for a long time. The model reads them once, when it builds the feature.
    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);"""
    assert old in text, "preset block not found in " + path
    text = text.replace(old, new, 1)

    io.open(path, "w", encoding="utf-8").write(text)
    print("forwarder patched:", path)

# --- the module ------------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

old = """using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*,
                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int);"""
new = """using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*,
                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int,
                                      float, int, float, float, float, int);"""
assert old in text
text = text.replace(old, new, 1)

CREATE_ARGS = """, (int) cfg.DlssNrPreset.value_or_default(),
                        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
                        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                        cfg.DlssNrSkinStructure.value_or_default(),
                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0)"""

old = """                        (int) cfg.DlssNrPreset.value_or_default());"""
assert text.count(old) == 2, "expected both create call sites, found %d" % text.count(old)
text = text.replace(old, CREATE_ARGS + ";")

io.open(PATH, "w", encoding="utf-8").write(text)
print("module patched")
