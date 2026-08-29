This adds DLSS 5 Neural Rendering to OptiScaler as an optional pass. It runs NVIDIA's `nvngx_dlssnr.dll` over whatever upscaler you're already using and synthesises detail into the result. It's off by default, and the model itself isn't included — you supply it from a driver package.

The pass runs right after the upscaler, before the game draws its UI. That was deliberate: the model never sees the HUD, so it can't invent detail in your text and icons, and there's no masking or detection involved to go wrong.

**The colour handling is not mine.** It's taken from [RenoDX](https://github.com/clshortfuse/renodx)'s DLSS 5 addon by clshortfuse, used under its MIT licence. I'd written my own version first and it could turn skin green in some scenes; theirs simply can't, because of how it's built. Their copyright and licence are in `Licenses/RenoDX_ATTRIBUTION.txt` and the credit is in the shader and the README too. Full credit to them for that part.

There are two sliders that matter. **Detail strength** is how far the frame moves toward the model's picture — at 0 you get exactly what the upscaler produced. **Colour strength** decides whether the model's colour comes along: at 0 you keep the game's own colours with the added detail, at 1 you get the model's look.

### For maintainers

If you don't want this in the build, set `OPTI_DLSSNR` to `0` in `dlssnr/DlssNr_Switch.h`. That's the whole procedure — it compiles out completely, and I checked by building it both ways: with the switch off the resulting DLL has zero mentions of the feature anywhere in it.

Everything lives in `OptiScaler/dlssnr/`. Outside that folder it's eight small `#if` blocks across four files, and deleting them plus the folder removes it entirely.

**The first commit is separate from all of this.** `OS_Dx12::Dispatch` was reading its sizes from the global current feature instead of from the resources actually passed to it. Those happen to match in the normal Output Scaling path so nothing was visibly broken, but the function takes them as arguments. It has nothing to do with Neural Rendering and you can take or drop it on its own.

### Things worth knowing

It needs an RTX 50 card and a driver that ships the model, on DX12 (DX11 works through the existing bridge, Vulkan doesn't yet). None of this is documented or supported by NVIDIA — the model is driven directly, and some of its own settings are educated guesses, which the tooltips say honestly. HDR10 output currently passes through unconverted; scRGB and SDR are handled.

There's a small forwarder DLL in the package. The model refuses to run unless the calling module is named `nvngx.dll`, so that file exists purely to satisfy the check. It's built here and contains no NVIDIA code.

`OptiScaler/dlssnr/README.md` has the details, including a few things that cost me device hangs to learn.
