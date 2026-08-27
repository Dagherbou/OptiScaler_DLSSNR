"""Rewrites the DLSS-NR module's apply stage around the codec, and adds the auto white point.

A file rather than an inline heredoc: the replacement text is full of backslashes and quotes, and a
shell heredoc mangles those into real newlines inside string literals.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"

text = io.open(PATH, encoding="utf-8").read()

# --- includes and state -------------------------------------------------------------------------

text = text.replace(
    '#include "DlssNr_Tonemap.h"',
    '#include "DlssNr_Codec.h"\n#include "DlssNr_Probe.h"',
    1,
)

text = text.replace(
    "NrState g_nr;\ntonemap::ToneTransform g_tone;",
    """NrState g_nr;
codec::Codec g_codec;

// Exposure measurement, and the white point derived from it.
probe::FrameReducer g_reducer;
probe::BlockReader g_reader;
float g_autoWhitePoint = 2.0f;
bool g_autoWhitePointSettled = false;
unsigned long long g_frames = 0;

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
// that lunges at every cut is worse than one that arrives a moment late.
constexpr float kWhitePointBlend = 0.25f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}""",
    1,
)

# --- the apply stage ----------------------------------------------------------------------------

OLD_START = "    // The upscaler has just written this, so it is a UAV. The model wants it readable."
OLD_END = "    // Leave the staging copy as the next frame expects to find it."

start = text.index(OLD_START)
end = text.index(OLD_END)

NEW = '''    // The upscaler has just written this, so it is a UAV. The model needs it readable.
    const bool haveCodec = g_codec.ensure(device);

    if (!haveCodec)
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    const bool autoWhite = cfg.DlssNrAutoWhitePoint.value_or_default();

    if (autoWhite)
    {
        const probe::Stats stats = g_reader.collect();

        if (stats.valid && stats.meanLuma > 0.0f)
        {
            const float target = WhitePointForMean(stats.meanLuma);

            if (!g_autoWhitePointSettled)
            {
                // Nothing to ease away from on the first reading, and easing in from a wrong default is
                // just a slow wrong answer.
                g_autoWhitePoint = target;
                g_autoWhitePointSettled = true;
                LOG_INFO("DLSS-NR white point settled at {:.3f} (frame mean {:.4f})", g_autoWhitePoint,
                         stats.meanLuma);
            }
            else
            {
                g_autoWhitePoint += (target - g_autoWhitePoint) * kWhitePointBlend;
            }
        }
    }

    const float whitePoint = autoWhite && g_autoWhitePointSettled
                                 ? g_autoWhitePoint
                                 : cfg.DlssNrWhitePoint.value_or_default();

    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;
    encodeParams.whitePoint = whitePoint;
    encodeParams.width = width;
    encodeParams.height = height;

    Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_codec.dispatch(cmdList, encodeParams, target, nullptr, g_nr.colorCopy);

    // Measuring here, while the frame is already readable, costs one dispatch every so often and no
    // extra barriers. Twice a second is far more often than an exposure meaningfully moves.
    if (autoWhite && (g_frames % 30 == 0) && g_reducer.ensure(device))
    {
        ID3D12Resource* reduced = g_reducer.dispatch(cmdList, target, width, height);
        g_reader.capture(cmdList, reduced, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The transition doubles as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, g_nr.colorCopy, depth, motion, g_nr.output, width,
        height, guideWidth, guideHeight, 1, g_nr.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
        (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
        cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0);

    g_nr.reset = false;

    if (result == NVSDK_NGX_Result_Success)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        codec::Params resolveParams {};
        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.whitePoint = whitePoint;
        resolveParams.width = width;
        resolveParams.height = height;
        resolveParams.transferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.colourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.debugView = cfg.DlssNrDebugView.value_or_default();

        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_codec.dispatch(cmdList, resolveParams, g_nr.colorCopy, g_nr.output, target);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X}, disabling for this session", (uint32_t) result);
    }

'''

text = text[:start] + NEW + text[end:]

# --- teardown and the reported white point --------------------------------------------------------

text = text.replace("    g_tone.destroy();", "    g_codec.destroy();\n    g_reducer.destroy();\n    g_reader.destroy();", 1)

text = text.replace(
    "const char* FailureReason() { return g_nr.failed ? g_nr.reason : \"\"; }",
    """const char* FailureReason() { return g_nr.failed ? g_nr.reason : ""; }

float CurrentWhitePoint() { return g_autoWhitePointSettled ? g_autoWhitePoint : 0.0f; }""",
    1,
)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module patched")

# --- header ---------------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.h"
text = io.open(PATH, encoding="utf-8").read()
text = text.replace(
    "void Shutdown();",
    """// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.
float CurrentWhitePoint();

void Shutdown();""",
    1,
)
io.open(PATH, "w", encoding="utf-8").write(text)
print("header patched")
