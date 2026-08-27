"""Rebuilds the model when its tuning changes, so the controls work while the game is running.

The tuning is read once, when the feature is built, so changing it afterwards needs a new feature. That
is the operation with the worst history in this project -- releasing a model feature mid-session has
crashed the game every way it has been tried.

What makes it safe here is that the finished-frame path submits its own command lists and now owns a
fence over them. Waiting for that fence means every command referencing the feature has completed before
it is released. Nothing else in this project ever had that guarantee.

Dragging a slider changes the value on every frame, so a change has to settle before anything is rebuilt.
A quarter of a second of stability is long enough to be one rebuild per adjustment and short enough to
feel immediate.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

# --- what the live feature was built with -----------------------------------------------------------

old = """    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;
    bool guidesReady = false;"""
new = """    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;
    bool guidesReady = false;

    // The values the live feature was created with, and when a difference from them was first seen.
    unsigned int builtPreset = 0;
    float builtIntensity = 0.0f;
    unsigned int builtStyle = 0;
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    unsigned long long settledAt = 0;"""
assert old in text
text = text.replace(old, new, 1)

# --- the comparison and the rebuild -----------------------------------------------------------------

HELPERS = '''
// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

bool TuningMatchesFeature(const Config& cfg)
{
    return g_nr.builtPreset == cfg.DlssNrPreset.value_or_default() &&
           g_nr.builtIntensity == cfg.DlssNrIntensity.value_or_default() &&
           g_nr.builtStyle == cfg.DlssNrStyle.value_or_default() &&
           g_nr.builtLocalStructure == cfg.DlssNrLocalStructure.value_or_default() &&
           g_nr.builtLocalTone == cfg.DlssNrLocalTone.value_or_default() &&
           g_nr.builtSkinStructure == cfg.DlssNrSkinStructure.value_or_default() &&
           g_nr.builtAutoMask == cfg.DlssNrAutoMask.value_or_default();
}

void RecordBuiltTuning(const Config& cfg)
{
    g_nr.builtPreset = cfg.DlssNrPreset.value_or_default();
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle = cfg.DlssNrStyle.value_or_default();
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
}

// Waits for every list this has submitted. Releasing the feature before that is what took the game down
// each of the previous times, and this is the first place with the means to avoid it.
void WaitForAllSubmitted()
{
    if (g_nr.presentFence == nullptr || g_nr.presentFenceNext == 0)
        return;

    if (g_nr.presentFence->GetCompletedValue() >= g_nr.presentFenceNext)
        return;

    if (SUCCEEDED(g_nr.presentFence->SetEventOnCompletion(g_nr.presentFenceNext, g_nr.presentFenceEvent)))
        WaitForSingleObject(g_nr.presentFenceEvent, 1000);
}
'''

anchor = "bool EnsurePresentList(ID3D12Device* device)"
assert anchor in text
text = text.replace(anchor, HELPERS.strip() + "\n\n" + anchor, 1)

# --- drive it from the present path -------------------------------------------------------------------

old = """    const unsigned int slot = backBufferIndex % kPresentAllocators;"""
new = """    // A tuning change means a new model, since the values are only read when one is built.
    if (g_nr.feature != nullptr && !TuningMatchesFeature(cfg))
    {
        if (g_nr.settledAt == 0)
            g_nr.settledAt = g_frames;

        if (g_frames - g_nr.settledAt >= kSettleFrames)
        {
            WaitForAllSubmitted();
            g_nr.release(g_nr.feature);
            g_nr.feature = nullptr;
            g_nr.settledAt = 0;
            LOG_INFO("DLSS-NR rebuilding for changed tuning");
        }
    }
    else
    {
        g_nr.settledAt = 0;
    }

    ++g_frames;

    const unsigned int slot = backBufferIndex % kPresentAllocators;"""
assert old in text
text = text.replace(old, new, 1)

old = """        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        LOG_INFO("DLSS-NR running on the finished frame at {}x{}, guides {}x{}", width, height,
                 g_nr.guideWidth, g_nr.guideHeight);"""
new = """        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running on the finished frame at {}x{}, guides {}x{} (intensity {}, style {}, "
                 "local structure {}, local tone {}, skin {})",
                 width, height, g_nr.guideWidth, g_nr.guideHeight, g_nr.builtIntensity, g_nr.builtStyle,
                 g_nr.builtLocalStructure, g_nr.builtLocalTone, g_nr.builtSkinStructure);"""
assert old in text
text = text.replace(old, new, 1)

# The other path records what it built with too, so the overlay can tell the truth about it.
old = """        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        LOG_INFO("DLSS-NR running at {}x{}, guides {}x{}", width, height, guideWidth, guideHeight);"""
new = """        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running at {}x{}, guides {}x{}", width, height, guideWidth, guideHeight);"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module patched")

# --- say so in the overlay --------------------------------------------------------------------------

NL = "\\n"
PATH = "C:/Games_Temp/OptiScaler/OptiScaler/menu/menu_common.cpp"
text = io.open(PATH, encoding="utf-8").read()

old = ('''        ShowHelpMarker("0 leaves the choice to the model."
                       "@@Undocumented, and not the same scale as the super resolution or ray"
                       "@reconstruction presets -- the same number means something different here."
                       "@@Baked in when the model is created, so it takes effect on the next restart,"
                       "@unlike everything else in this panel.");''').replace("@", NL)
new = ('''        ShowHelpMarker("0 leaves the choice to the model."
                       "@@Undocumented, and not the same scale as the super resolution or ray"
                       "@reconstruction presets -- the same number means something different here.");''').replace("@", NL)
assert old in text
text = text.replace(old, new, 1)

old = ('''        ImGui::SeparatorText("Model");''')
new = ('''        ImGui::SeparatorText("Model");

        ImGui::TextUnformatted("Read when the model is built, so a change rebuilds it after a moment.");''')
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("menu patched")
