"""Finds the parameter block's float setter instead of assuming it.

The readback settled a long-running question. Every uint parameter lands -- style, the preset, the masks
-- and every float one comes back as FAIL_UnsupportedParameter, meaning the block holds no such value.
Intensity, local structure, local tone and skin structure have never reached the model at all, which is
exactly what was reported from playing and what was repeatedly explained away as something else.

The floats are written through vtable slot 1, which is where the public header declares the float
overload. This block is not the header's implementation though -- it is the driver's own -- and it
plainly does not keep a float there.

So this stops guessing. It writes a known value through each candidate slot, asks the block for it back
through the header's typed getter, and keeps whichever slot round-trips. One probe per session, on a
private key, and the answer is logged. If none works the floats are left alone rather than scribbling
into a live block.
"""

import io

# --- the forwarder takes the slot rather than hardcoding one -------------------------------------------

for path in ("C:/Games_Temp/dlssnr-forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/cp2077-nr/dlssnr_ngx.cpp"):
    t = io.open(path, encoding="utf-8").read()

    old = """constexpr int VT_SET_FLOAT = 1;"""
    new = """// Where the float setter actually lives. The public header declares it at slot 1, and this block --
// the driver's own, not the header's implementation -- does not keep a float there: every float written
// to slot 1 reads back as FAIL_UnsupportedParameter while every uint lands. The host discovers the real
// slot by round-tripping a value and sets it here before anything else is written.
int g_floatSlot = 1;"""
    assert old in t, "float slot constant not found in " + path
    t = t.replace(old, new, 1)

    t = t.replace("reinterpret_cast<PFN_SetFloat>(vt[VT_SET_FLOAT])",
                  "reinterpret_cast<PFN_SetFloat>(vt[g_floatSlot])", 1)

    old = """extern "C" {"""
    new = """extern "C" {

// Called once, after the host has worked out which slot this block keeps floats in.
__declspec(dllexport) void dlssnr_call_set_float_slot(int slot) {
    if (slot >= 0 && slot < 8) {
        g_floatSlot = slot;
    }
}

// Writes a float through an arbitrary slot, so the host can find the right one by testing.
__declspec(dllexport) void dlssnr_call_probe_float(void *params, const char *name, float value,
                                                   int slot) {
    if (!params || slot < 0 || slot >= 8) {
        return;
    }
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[slot])(params, name, value);
}"""
    assert old in t, "extern block not found in " + path
    t = t.replace(old, new, 1)

    io.open(path, "w", encoding="utf-8").write(t)
    print("forwarder patched:", path)

# --- the module discovers it ------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

old = """using PFN_NrRelease = void(__cdecl*) (void*);"""
new = """using PFN_NrRelease = void(__cdecl*) (void*);
using PFN_NrSetFloatSlot = void(__cdecl*) (int);
using PFN_NrProbeFloat = void(__cdecl*) (void*, const char*, float, int);"""
assert old in text
text = text.replace(old, new, 1)

old = """    PFN_NrRelease release = nullptr;"""
new = """    PFN_NrRelease release = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;"""
assert old in text
text = text.replace(old, new, 1)

old = """    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");"""
new = """    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");"""
assert old in text
text = text.replace(old, new, 1)

HELPER = '''
// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and will simply
// win wherever the assumption was right. The others are the neighbours a differently ordered vtable
// would put it in.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr ||
        g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.3750f; // exact in binary, so a round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. Every uint parameter still applies.");
}
'''

anchor = "ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,"
assert anchor in text
text = text.replace(anchor, HELPER.strip() + "\n\n" + anchor, 1)

# Probe as soon as the block exists, before anything is written to it.
old = """    return true;
}

ID3D12Resource* CreateScratch"""
new = """    DiscoverFloatSlot(g_nr.capabilityParams);
    return true;
}

ID3D12Resource* CreateScratch"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module discovers the slot")
