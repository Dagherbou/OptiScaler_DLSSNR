import io

# --- A: OS_Dx12::Dispatch sizes its work from the resources it is handed, not from the global
# current feature. For the conventional Output Scaling chain the two are identical (the OS buffer is
# target-sized, the game output display-sized), but any other caller -- the split's supersample
# downscale, whose current feature is a 1:1 denoiser -- got a third of the source read and a third of
# the destination written: the corrupted band across the bottom of the frame.
p = 'OptiScaler/shaders/output_scaling/OS_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

old = """    FsrEasuCon(fsr1Constants.const0, fsr1Constants.const1, fsr1Constants.const2, fsr1Constants.const3,
               State::Instance().currentFeature->TargetWidth(), State::Instance().currentFeature->TargetHeight(),
               State::Instance().currentFeature->TargetWidth(), State::Instance().currentFeature->TargetHeight(),
               State::Instance().currentFeature->DisplayWidth(), State::Instance().currentFeature->DisplayHeight());

    constants.srcWidth = State::Instance().currentFeature->TargetWidth();
    constants.srcHeight = State::Instance().currentFeature->TargetHeight();
    constants.destWidth = State::Instance().currentFeature->DisplayWidth();
    constants.destHeight = State::Instance().currentFeature->DisplayHeight();"""
assert old in t
new = """    // The work is sized by the resources actually passed in. For the usual Output Scaling chain these
    // match the current feature's target/display sizes; for any other caller only the resources are
    // the truth.
    const auto srcDesc = InResource->GetDesc();
    const auto dstDesc = OutResource->GetDesc();
    const auto srcW = (uint32_t) srcDesc.Width;
    const auto srcH = (uint32_t) srcDesc.Height;
    const auto dstW = (uint32_t) dstDesc.Width;
    const auto dstH = (uint32_t) dstDesc.Height;

    FsrEasuCon(fsr1Constants.const0, fsr1Constants.const1, fsr1Constants.const2, fsr1Constants.const3, srcW, srcH,
               srcW, srcH, dstW, dstH);

    constants.srcWidth = srcW;
    constants.srcHeight = srcH;
    constants.destWidth = dstW;
    constants.destHeight = dstH;"""
t = t.replace(old, new, 1)

old = """    dispatchWidth =
        static_cast<UINT>((State::Instance().currentFeature->DisplayWidth() + InNumThreadsX - 1) / InNumThreadsX);
    dispatchHeight = (State::Instance().currentFeature->DisplayHeight() + InNumThreadsY - 1) / InNumThreadsY;"""
assert old in t
new = """    dispatchWidth = (dstW + InNumThreadsX - 1) / InNumThreadsX;
    dispatchHeight = (dstH + InNumThreadsY - 1) / InNumThreadsY;"""
t = t.replace(old, new, 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('OS dispatch dims fixed')

# --- B: seam hardening ---
p = 'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

# B1: the downscaler's shader is chosen at construction, but the Downscaler dropdown can change at any
# time -- and Dispatch picks its constants layout from the live dropdown. A stale instance then runs a
# bicubic pipeline on FSR1 constants (or the reverse): garbage. Rebuild ours when the choice changes.
old = """    std::unique_ptr<OS_Dx12> downscaler;    // oversized -> display, OptiScaler's own filtering"""
assert old in t
t = t.replace(old, """    std::unique_ptr<OS_Dx12> downscaler;    // oversized -> display, OptiScaler's own filtering
    int downscalerKind = -1;                // the Downscaler choice it was built with""", 1)

old = """    if (SplitDx12.downscaler == nullptr)
        SplitDx12.downscaler = std::make_unique<OS_Dx12>("DLSS-NR Split Downscale", D3D12Device, false);"""
assert old in t
new = """    // The downscaler's pipeline is baked at construction, but its dispatch reads the Downscaler
    // dropdown live -- a stale instance runs one scaler's shader on another's constants. Rebuild when
    // the user's choice changes; the old instance is parked, since its last dispatch may be in flight.
    const int downscalerKind = (int) Config::Instance()->OutputScalingDownscaler.value_or_default();

    if (SplitDx12.downscaler != nullptr && downscalerKind != SplitDx12.downscalerKind)
    {
        SplitRetired r;
        r.shader = std::move(SplitDx12.downscaler);
        SplitParkedList.push_back(std::move(r));
    }

    if (SplitDx12.downscaler == nullptr)
    {
        SplitDx12.downscaler = std::make_unique<OS_Dx12>("DLSS-NR Split Downscale", D3D12Device, false);
        SplitDx12.downscalerKind = downscalerKind;
    }"""
t = t.replace(old, new, 1)

# B2: parked things live twice as long -- rapid toggling stacks transitions, and 16 evaluates at 138
# fps is close to the wire for work still referenced by in-flight frames.
old = "    int framesLeft = 16;"
assert old in t
t = t.replace(old, "    int framesLeft = 32;", 1)

# B3: a longer settle for the same reason, and the leftover no-op block goes away.
old = """    if (++SplitDx12.stableFrames < 20 && SplitDx12.stableFrames > 0)
    {
        // Not yet settled; unless nothing would change anyway, wait.
    }"""
assert old in t
t = t.replace(old, "    ++SplitDx12.stableFrames;", 1)

old = "    const bool settled = SplitDx12.stableFrames >= 20;"
assert old in t
t = t.replace(old, "    const bool settled = SplitDx12.stableFrames >= 30;", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('seam hardening in')
