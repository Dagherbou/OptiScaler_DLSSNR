import io

# ==================== MENU: three honest styles; two controls leave ====================
p = 'OptiScaler/dlssnr/DlssNr_Menu.cpp'
m = io.open(p, encoding='utf-8').read()

old = """        static const char* nrStyleNames[] = { "Natural (default)", "Cinematic" };
        int style = (int) config->DlssNrStyle.value_or_default();
        if (ImGui::Combo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
            config->DlssNrStyle = (uint32_t) style;

        HelpMarker("The model has two, and only two. A slider offering more was a guess, and it"
                       "\\nmatched the observation that most of its positions did nothing.");"""
assert old in m
m = m.replace(old, """        static const char* nrStyleNames[] = { "Default (standard)", "Natural", "Cinematic" };
        int style = (int) config->DlssNrStyle.value_or_default();

        if (style > 2)
            style = 2;

        if (ImGui::Combo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
            config->DlssNrStyle = (uint32_t) style;

        HelpMarker("The model's own processing profiles."
                   "\\n\\nDefault (standard): the strongest. Boosts local contrast and deepens"
                   "\\nlighting, and can oversaturate or look stylised -- most of what reads as"
                   "\\n'the model changed my game's look' is this profile."
                   "\\n\\nNatural: the same detail work with a gentler hand. Keeps skin tones and"
                   "\\ntonal balance closer to what the game rendered."
                   "\\n\\nCinematic: tones down the shine and over-processing for a film-like look."
                   "\\n\\nRead when the model is built, so a change rebuilds it after a moment. The"
                   "\\nnames come from community testing; NVIDIA ships no names in the binaries.");""", 1)

# Global tone goes
start = m.find("        float globalTone = config->DlssNrGlobalTone.value_or_default();")
assert start > 0
end = m.find('global tone and lets the model add detail only. Read when the model is built.");', start)
assert end > 0
end += len('global tone and lets the model add detail only. Read when the model is built.");') + 1
m = m[:start] + m[end:]

# The skin exclusion goes
start = m.find("        bool skipSkin = config->DlssNrRestoreSkipSkin.value_or_default();")
assert start > 0
end = m.find('only means a little less restore there.");', start)
assert end > 0
end += len('only means a little less restore there.");') + 1
m = m[:start] + m[end:]
assert 'skipSkin' not in m and 'globalTone' not in m
io.open(p, 'w', encoding='utf-8').write(m)
print('menu in')

# ==================== CODEC: the skin classifier goes ====================
p = 'OptiScaler/dlssnr/DlssNr_Codec.h'
t = io.open(p, encoding='utf-8').read()

old = """    float gRestoreSkipSkin; // 1: highlight restore leaves skin alone; 0: applies everywhere
    uint  gPad2;"""
assert old in t
t = t.replace(old, """    uint  gPad1;
    uint  gPad2;""", 1)

start = t.find("// A soft skin-tone classifier on gamma-space colour")
end = t.find("float3x3 CurveMatrix()")
assert 0 < start < end
t = t[:start] + t[end:]

old = """        // Highlight restore: the model's darkening of bright regions is pulled back -- except on skin,
        // when asked: the model's softening of a lit face is its skin work, not a muted highlight.
        float skinHold = 1.0;

        if (gRestoreSkipSkin > 0.0)
        {
            float3 gammaRgb = gPassthrough != 0 ? proxy : LinearToSrgb(saturate(proxy));
            skinHold = 1.0 - SkinWeight(gammaRgb) * gRestoreSkipSkin;
        }

        if (appliedLuma < 0.0)
            applied -= appliedLuma * gProtectHighlights * smoothstep(0.25, 0.9, relLuma) * skinHold;"""
assert old in t
t = t.replace(old, """        // Highlight restore: the model's darkening of bright regions is pulled back. A colour test for
        // skin was tried here and removed -- wood, sand and brick sit in the same chromaticity region,
        // so it withheld the restore from half the environment. The model's own Style and Skin
        // structure controls are where skin belongs.
        if (appliedLuma < 0.0)
            applied -= appliedLuma * gProtectHighlights * smoothstep(0.25, 0.9, relLuma);""", 1)

old = """    float restoreSkipSkin;
    unsigned int pad2;"""
assert old in t
t = t.replace(old, """    unsigned int pad1;
    unsigned int pad2;""", 1)
assert 'SkinWeight' not in t and 'gRestoreSkipSkin' not in t and 'restoreSkipSkin' not in t
io.open(p, 'w', encoding='utf-8').write(t)
print('codec in')

# ==================== MODULE ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
d = io.open(p, encoding='utf-8').read()

old = "\n        resolveParams.restoreSkipSkin = cfg.DlssNrRestoreSkipSkin.value_or_default() ? 1.0f : 0.0f;"
assert d.count(old) == 3
d = d.replace(old, "")

old = "    float builtGlobalTone = 1.0f;\n"
assert old in d
d = d.replace(old, "", 1)
old = """ &&
           g_nr.builtGlobalTone == cfg.DlssNrGlobalTone.value_or_default();"""
assert old in d
d = d.replace(old, ";", 1)
old = "    g_nr.builtGlobalTone = cfg.DlssNrGlobalTone.value_or_default();\n"
assert old in d
d = d.replace(old, "", 1)

old = "    g_nr.setExtras(g_nr.capabilityParams, cfg.DlssNrGlobalTone.value_or_default(), ui, ui, backbuffer,"
assert old in d
d = d.replace(old, """    // Global tone is written at the model's own default: the control that exposed it changed nothing
    // that could be seen, and the block persists, so a value still has to be put there.
    g_nr.setExtras(g_nr.capabilityParams, 1.0f, ui, ui, backbuffer,""", 1)

start = d.find("        {\n            float globalToneBack = -1.0f;")
assert start > 0
end = d.find("        }", start)
assert end > 0
end += len("        }") + 1
d = d[:start] + d[end:]
assert 'GlobalTone' not in d and 'restoreSkipSkin' not in d

# the extras helper no longer needs the config for its tone argument
old = "void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,"
assert old in d
io.open(p, 'w', encoding='utf-8').write(d)
print('module in')

# ==================== CONFIG ====================
p = 'OptiScaler/Config.h'
h = io.open(p, encoding='utf-8').read()
old = """
    // The model's global tone strength -- its overall re-exposure of the scene, as opposed to the local
    // one. NVIDIA's own integration sets it; 1 is the model's default behaviour, 0 keeps the game's tone.
    CustomOptional<float> DlssNrGlobalTone { 1.0f };"""
assert old in h
h = h.replace(old, "", 1)
old = """

    // Highlight restore leaves skin alone: the model's softening of a lit face is its skin work.
    CustomOptional<bool> DlssNrRestoreSkipSkin { false };"""
assert old in h
h = h.replace(old, "", 1)
old = "    CustomOptional<uint32_t> DlssNrStyle { 0 };"
if old in h:
    h = h.replace(old, "    // 0 default (standard), 1 natural, 2 cinematic -- the model's own processing profiles.\n" + old, 1)
io.open(p, 'w', encoding='utf-8').write(h)

p = 'OptiScaler/Config.cpp'
c = io.open(p, encoding='utf-8').read()
old = '\n            DlssNrGlobalTone.set_from_config(readFloat("DlssNr", "GlobalTone"));'
assert old in c
c = c.replace(old, "", 1)
old = '\n            DlssNrRestoreSkipSkin.set_from_config(readBool("DlssNr", "RestoreSkipSkin"));'
assert old in c
c = c.replace(old, "", 1)
old = '\n    ini.SetValue("DlssNr", "GlobalTone", GetFloatValue(Instance()->DlssNrGlobalTone.value_for_config()).c_str());'
assert old in c
c = c.replace(old, "", 1)
old = '\n    ini.SetValue("DlssNr", "RestoreSkipSkin", GetBoolValue(Instance()->DlssNrRestoreSkipSkin.value_for_config()).c_str());'
assert old in c
c = c.replace(old, "", 1)
assert 'GlobalTone' not in c and 'RestoreSkipSkin' not in c
io.open(p, 'w', encoding='utf-8').write(c)
print('config in')
