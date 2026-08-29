import io

# ==================== HEADER ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.h'
h = io.open(p, encoding='utf-8').read()

start = h.find("// Where the pass runs.")
assert start > 0
end = h.find("constexpr unsigned int INJECT_HUDLESS = 2;")
assert end > start
end += len("constexpr unsigned int INJECT_HUDLESS = 2;") + 1

while h[end:end + 1] == "\n":
    end += 1

h = h[:start] + h[end:]

start = h.find("// Frame generation titles tag their UI layer")
assert start > 0
end = h.find("void EvaluateHudless(")
assert end > start
end2 = h.find(";", end) + 2
# keep the UI-layer note, drop only the hudless declaration
note_end = h.find("// Called at tag time.")
assert note_end > 0

start_h = h.find("// The hudless inject point:")
if start_h > 0:
    e = h.find(";", start_h) + 2
    h = h[:start_h] + h[e:]

start_h = h.find("void EvaluateHudless(")
assert start_h > 0
e = h.find(";", start_h) + 2

while h[e:e + 1] == "\n":
    e += 1

h = h[:start_h] + h[e:]
assert 'EvaluateHudless' not in h and 'INJECT_' not in h
io.open(p, 'w', encoding='utf-8').write(h)
print('header in')

# ==================== MODULE ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

# the hudless evaluate goes entirely
start = t.find("// The best seat in the house, when a game offers it")
assert start > 0
end = t.find("// Reads the finished frame's luminance samples for the proxy curve")
assert end > start
t = t[:start] + t[end:]
assert 'EvaluateHudless' not in t

# the model always runs at the finished frame now
old = """    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT)
        return;

    // The split pipeline already ran the model this frame, on its own intermediate."""
assert old in t
t = t.replace(old, """    // The split pipeline already ran the model this frame, on its own intermediate.""", 1)

old = """    if (cfg.DlssNrProxyCurve.value_or_default() == 1 &&
        (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT || g_splitActive))"""
assert old in t
t = t.replace(old, """    if (cfg.DlssNrProxyCurve.value_or_default() == 1 && g_splitActive)""", 1)

old = """    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT || cfg.DlssNrHudDetect.value_or_default() <= 0.0f)
        return;"""
if old in t:
    t = t.replace(old, "", 1)
else:
    old2 = """    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT)
        return;"""
    assert old2 in t
    t = t.replace(old2, "", 1)

assert 'InjectPoint' not in t and 'INJECT_' not in t
io.open(p, 'w', encoding='utf-8').write(t)
print('module in')

# ==================== HOOKS ====================
p = 'OptiScaler/hooks/Streamline_Hooks.cpp'
s = io.open(p, encoding='utf-8').read()
for _ in range(2):
    i = s.find("        if (tags[i].type == sl::kBufferTypeHUDLessColor && cmdBuffer != nullptr)")
    if i < 0:
        i = s.find("        if (resources[i].type == sl::kBufferTypeHUDLessColor && cmdBuffer != nullptr)")
    assert i > 0
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
start = c.rfind("\n", 0, c.find("    CustomOptional<uint32_t> DlssNrInjectPoint { 1 };"))
# drop the declaration and any comment lines directly above it
line_start = c.find("    CustomOptional<uint32_t> DlssNrInjectPoint { 1 };")
assert line_start > 0
line_end = c.find("\n", line_start) + 1
# walk back over contiguous comment lines
ls = line_start
while True:
    prev = c.rfind("\n", 0, ls - 1) + 1
    if c[prev:ls].lstrip().startswith("//"):
        ls = prev
    else:
        break
c = c[:ls] + c[line_end:]
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
m = m.replace(old, """        ImGui::TextDisabled("SDR finished frames go over unconverted; scRGB HDR frames are\\n"
                            "encoded with their own measured white point. In the split the frame is\\n"
                            "compressed through the proxy curve instead, and everything the resolve\\n"
                            "does is attenuated by the game's own tonemapper afterwards.");""", 1)
assert 'InjectPoint' not in m and 'INJECT_' not in m
io.open(p, 'w', encoding='utf-8').write(m)
print('menu in')
