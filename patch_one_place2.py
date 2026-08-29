import io

# ==================== HEADER ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.h'
h = io.open(p, encoding='utf-8').read()

old = """// Where the model runs. Two genuine trade-offs rather than one compromise: before frame generation it
// costs one run per rendered frame and generated frames inherit the result, but the game's tonemapper
// has not run yet so it works on a proxy; on the finished frame it sees exactly the sort of picture it
// was trained on, at a run per presented frame.
// Where the pass runs. 0 was "before frame generation" (the linear frame straight after the
// upscaler) and is retired: the finished frame is the model's trained distribution, and the hudless
// point gives the same picture without the interface. A stored 0 reads as the finished frame.
constexpr unsigned int INJECT_PRESENT = 1;
constexpr unsigned int INJECT_HUDLESS = 2;
"""
assert old in h
h = h.replace(old, """// The model runs on the finished frame -- the picture after the game's own tonemapper, which is the
// sort of picture it was trained on. The split pipeline is the other arrangement: it runs the model
// before the interface exists, and takes over when it is on.
""", 1)

old = """// The hudless inject point: the finished-look image before the interface is drawn, tagged by
// Streamline FG games. The model's trained distribution AND structural UI immunity at once.
void EvaluateHudless(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* hudless,
                     D3D12_RESOURCE_STATES state);

"""
assert old in h
h = h.replace(old, "", 1)
assert 'EvaluateHudless' not in h and 'INJECT_' not in h
io.open(p, 'w', encoding='utf-8').write(h)
print('header in')

# ==================== MODULE ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

start = t.find("// The best seat in the house, when a game offers it")
assert start > 0
end = t.find("// Reads the finished frame's luminance samples for the proxy curve")
assert end > start
t = t[:start] + t[end:]
assert 'EvaluateHudless' not in t

old = """    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT)
        return;

    // The split pipeline already ran the model this frame, on its own intermediate."""
assert old in t
t = t.replace(old, """    // The split pipeline already ran the model this frame, on its own intermediate.""", 1)

old = """    if (cfg.DlssNrProxyCurve.value_or_default() == 1 &&
        (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT || g_splitActive))"""
assert old in t
t = t.replace(old, """    if (cfg.DlssNrProxyCurve.value_or_default() == 1 && g_splitActive)""", 1)

old = """    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT)
        return;

"""
assert old in t
t = t.replace(old, "", 1)
assert 'InjectPoint' not in t and 'INJECT_' not in t
io.open(p, 'w', encoding='utf-8').write(t)
print('module in')

# ==================== HOOKS ====================
p = 'OptiScaler/hooks/Streamline_Hooks.cpp'
s = io.open(p, encoding='utf-8').read()
for marker in ("        if (tags[i].type == sl::kBufferTypeHUDLessColor && cmdBuffer != nullptr)",
               "        if (resources[i].type == sl::kBufferTypeHUDLessColor && cmdBuffer != nullptr)"):
    i = s.find(marker)
    assert i > 0, marker
    end = s.find(";", s.find("DlssNr::EvaluateHudless", i)) + 1

    while s[end:end + 1] == "\n":
        end += 1

    s = s[:i] + s[end:]
assert 'EvaluateHudless' not in s
io.open(p, 'w', encoding='utf-8').write(s)
print('hooks in')

# ==================== CONFIG ====================
p = 'OptiScaler/Config.h'
c = io.open(p, encoding='utf-8').read()
line_start = c.find("    CustomOptional<uint32_t> DlssNrInjectPoint { 1 };")
assert line_start > 0
line_end = c.find("\n", line_start) + 1
ls = line_start

while True:
    prev = c.rfind("\n", 0, ls - 1) + 1
    if c[prev:ls].lstrip().startswith("//"):
        ls = prev
    else:
        break

c = c[:ls] + c[line_end:]
assert 'InjectPoint' not in c
io.open(p, 'w', encoding='utf-8').write(c)

p = 'OptiScaler/Config.cpp'
c = io.open(p, encoding='utf-8').read()
for old in ('            DlssNrInjectPoint.set_from_config(readUInt("DlssNr", "InjectPoint"));\n',
            '    ini.SetValue("DlssNr", "InjectPoint", GetIntValue(Instance()->DlssNrInjectPoint.value_for_config()).c_str());\n'):
    assert old in c
    c = c.replace(old, "", 1)
assert 'InjectPoint' not in c
io.open(p, 'w', encoding='utf-8').write(c)
print('config in')

# ==================== MENU ====================
p = 'OptiScaler/dlssnr/DlssNr_Menu.cpp'
m = io.open(p, encoding='utf-8').read()

start = m.find("        static const char* injectNames[]")
assert start > 0
end = m.find('frame simply looks better, and the split covers the pre-tonemapper case.");', start)
assert end > start
end += len('frame simply looks better, and the split covers the pre-tonemapper case.");') + 1

while m[end:end + 1] == "\n":
    end += 1

m = m[:start] + m[end:]

old = """        if (config->DlssNrInjectPoint.value_or_default() == DlssNr::INJECT_PRESENT)
            ImGui::TextDisabled("%s", DlssNr::UiLayerStatus());"""
assert old in m
m = m.replace(old, """        ImGui::TextDisabled("%s", DlssNr::UiLayerStatus());""", 1)

old = """        if (config->DlssNrInjectPoint.value_or_default() == DlssNr::INJECT_PRESENT)
            ImGui::TextDisabled("SDR finished frames go over unconverted; scRGB HDR frames are\\n"
                                "encoded with their own measured white point. Everything below\\n"
                                "still applies.");"""
assert old in m
m = m.replace(old, """        ImGui::TextDisabled("On the finished frame an SDR picture goes over unconverted and an scRGB\\n"
                            "one is encoded with its measured white point. In the split the frame is\\n"
                            "compressed through the proxy curve instead -- and everything the resolve\\n"
                            "does there is attenuated by the game's own tonemapper afterwards.");""", 1)
assert 'InjectPoint' not in m and 'INJECT_' not in m
io.open(p, 'w', encoding='utf-8').write(m)
print('menu in')
