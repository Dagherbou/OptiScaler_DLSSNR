"""Reads the model's tuning parameters back out of the block after setting them.

Intensity, local structure, local tone and skin structure appear to do nothing in either integration.
Those four are the only floats; everything that demonstrably works -- width, height, enabled, the
resources -- is a uint or a pointer. That split is too clean to be a coincidence, and it leaves two
candidates: the float setter is not landing, or the model latches these when the feature is created and
ignores later changes the way the preset does.

Reading them back separates the two. If they come back as what was written, the values are landing and
the model is not reading them live. If they come back missing or wrong, the setter path is the problem.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

old = """    g_nr.reset = false;
"""
new = """    g_nr.reset = false;

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        auto report = [](const char* name)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = g_nr.capabilityParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (result 0x{:X})", name, value, (uint32_t) r);
        };

        report("DLSSNR.Intensity");
        report("DLSSNR.LocalStructureStrength");
        report("DLSSNR.LocalToneStrength");
        report("DLSSNR.SkinStructureStrength");

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult = g_nr.capabilityParams->Get("DLSSNR.Style", &style);
        LOG_INFO("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        LOG_INFO("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                 cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                 cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                 cfg.DlssNrStyle.value_or_default());
    }
"""
assert old in text, "evaluate tail not found"
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("readback added")
