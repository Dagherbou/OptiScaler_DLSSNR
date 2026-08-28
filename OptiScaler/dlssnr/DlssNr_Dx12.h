#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

// DLSS 5 Neural Rendering, run over the upscaler's output.
//
// Neural Rendering is a post-process, not an upscaler and not a denoiser: it takes a finished frame plus
// depth and motion vectors and synthesises detail. NVIDIA ships no public integration for it, so it is
// driven directly through nvngx_dlssnr.dll as feature 18.
//
// OptiScaler is the right host for it because of one thing it knows that an external hook cannot: which
// NGX evaluate belongs to the upscaler and which to frame generation. Both are handed depth and motion
// vectors, so anything guessing from the parameter block alone attaches to both and runs the model twice
// per rendered frame. Here it is a lookup on the feature handle.
namespace DlssNr
{
// Where the model runs. Two genuine trade-offs rather than one compromise: before frame generation it
// costs one run per rendered frame and generated frames inherit the result, but the game's tonemapper
// has not run yet so it works on a proxy; on the finished frame it sees exactly the sort of picture it
// was trained on, at a run per presented frame.
constexpr unsigned int INJECT_BEFORE_FG = 0;
constexpr unsigned int INJECT_PRESENT = 1;
// Runs the model over Output on the same command list, immediately after the upscaler has written it.
// Called only for upscaler evaluates -- never for frame generation, which is the whole point.
//
// Safe to call every frame; it builds what it needs on first use and disables itself for the session if
// anything fails, rather than retrying into a crash.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          bool forceInPlace = false);

// The split pipeline runs the model itself; the present-time pass stands down while it is active.
void SetSplitActive(bool active);

// Runs the model over the finished frame, on a command list of its own, and submits it. Called every
// present; does nothing unless that inject point is selected.
void EvaluateAtPresent(ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, unsigned int backBufferIndex);

// Whether the model is loaded and running, for the overlay.
bool IsRunning();

// Why it is not, if it is not. Empty while it is running or has not been tried yet.
const char* FailureReason();

// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.
float CurrentWhitePoint();

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();

// Writes a run of consecutive frames, each as the upscaler produced it and again after the model's edit.
// The pair is a control: same frames, same run, one variable.
void RequestCapture(unsigned int frames);
bool CaptureInProgress();

void Shutdown();
} // namespace DlssNr
