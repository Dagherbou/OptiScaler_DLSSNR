// --- The split pipeline -----------------------------------------------------------------------------
//
// Ray Reconstruction as a pure denoiser, Neural Rendering at a controllable resolution, and one
// enlargement at the end. The stream stays linear HDR throughout, so every feature keeps the flags the
// game asked for.
//
// Supersampling is not a switch of its own: the split reads the Output Scaling Ratio the user already
// tuned, and supersamples its enlargement whenever that ratio is above one. Output Scaling's own Enable
// must stay off -- it owns the same feature geometry and the two cannot both steer it.
//
// Two arrangements, chosen by one checkbox:
//
//   RR 1:1 (default)   RR denoises at render size, the model runs there too, and an internal Super
//                      Resolution feature does the enlargement -- to the supersampled size when the
//                      ratio asks, with OptiScaler's own downscaler carrying it back. Cheapest: the
//                      expensive models both run at their smallest size.
//
//   RR included        RR itself upscales to the supersampled size, the model works on that image (its
//                      cost governed by the Model resolution dropdown), and only the downscale remains.
//                      The conventional Output Scaling look with the model in the chain -- and RR's
//                      cost rising with the square of the ratio, which is the price of it.

struct SplitState
{
    unsigned int displayWidth = 0;  // what the game originally asked its RR feature to output
    unsigned int displayHeight = 0;
    ID3D12Resource* intermediate = nullptr; // render-sized: denoised, then enhanced (RR 1:1 mode)
    ID3D12Resource* oversized = nullptr;    // above display size: the supersampled working image
    std::unique_ptr<IFeature_Dx12> sr;      // the enlargement (RR 1:1 mode only)
    std::unique_ptr<OS_Dx12> downscaler;    // oversized -> display, OptiScaler's own filtering
    unsigned int srTargetWidth = 0;         // what the enlargement was built to produce
    bool failed = false;

    // Retired on a live change: still referenced by command lists submitted over the last frames, so
    // they are released a number of evaluates later rather than on the spot.
    ID3D12Resource* parkedIntermediate = nullptr;
    ID3D12Resource* parkedOversized = nullptr;
    std::unique_ptr<IFeature_Dx12> parkedSr;
    std::unique_ptr<OS_Dx12> parkedDownscaler;
    int parkedCountdown = 0;

    // Geometry-change control, so nothing can ever loop recreations.
    unsigned int lastDesiredWidth = 0;
    int armTries = 0;
};

static SplitState SplitDx12;

static bool SplitWanted()
{
    const Config& cfg = *Config::Instance();

    // Output Scaling's Enable owns the same feature geometry; the split reads its Ratio instead and
    // does the supersampling itself.
    if (cfg.OutputScalingEnabled.value_or_default())
        return false;

    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrSplitPipeline.value_or_default() &&
           !SplitDx12.failed;
}

// The supersample ratio in force: the user's Output Scaling Ratio, when above one.
static float SplitRatio()
{
    float mult = Config::Instance()->OutputScalingMultiplier.value_or_default();

    if (mult > 3.0f)
        mult = 3.0f;

    return mult > 1.05f ? mult : 1.0f;
}

// What the Ray Reconstruction feature's output size should be under the current settings.
static void SplitDesiredTarget(unsigned int renderW, unsigned int renderH, unsigned int* outW,
                               unsigned int* outH)
{
    const float mult = SplitRatio();

    if (Config::Instance()->DlssNrSplitIncludeRR.value_or_default() && mult > 1.0f &&
        SplitDx12.displayWidth != 0)
    {
        *outW = (unsigned int) (SplitDx12.displayWidth * mult + 0.5f);
        *outH = (unsigned int) (SplitDx12.displayHeight * mult + 0.5f);
        return;
    }

    *outW = renderW;
    *outH = renderH;
}

// Frees what a live change parked, once enough evaluates have passed that nothing in flight can still
// reference it.
static void SplitTickParked()
{
    if (SplitDx12.parkedCountdown <= 0)
        return;

    if (--SplitDx12.parkedCountdown > 0)
        return;

    if (SplitDx12.parkedIntermediate != nullptr)
    {
        SplitDx12.parkedIntermediate->Release();
        SplitDx12.parkedIntermediate = nullptr;
    }

    if (SplitDx12.parkedOversized != nullptr)
    {
        SplitDx12.parkedOversized->Release();
        SplitDx12.parkedOversized = nullptr;
    }

    SplitDx12.parkedSr.reset();
    SplitDx12.parkedDownscaler.reset();
}

// Parks the enlargement stage for deferred release. If the parking lot is still occupied from a very
// recent change, the older tenants have had their frames and are let go now.
static void SplitParkEnlargement()
{
    if (SplitDx12.parkedOversized != nullptr)
        SplitDx12.parkedOversized->Release();

    SplitDx12.parkedSr.reset();
    SplitDx12.parkedDownscaler.reset();
    SplitDx12.parkedOversized = nullptr;

    if (SplitDx12.sr != nullptr)
        SplitDx12.parkedSr = std::move(SplitDx12.sr);

    if (SplitDx12.oversized != nullptr)
    {
        SplitDx12.parkedOversized = SplitDx12.oversized;
        SplitDx12.oversized = nullptr;
    }

    if (SplitDx12.downscaler != nullptr)
        SplitDx12.parkedDownscaler = std::move(SplitDx12.downscaler);

    SplitDx12.srTargetWidth = 0;
    SplitDx12.parkedCountdown = 16;
}

// Applies the toggles while the game runs, by re-creating the Ray Reconstruction feature at whatever
// geometry the settings currently call for. The split itself never trusts this function: it operates
// only when the feature's observed geometry matches the desired one, so every transition frame falls
// through to the conventional path.
static void SplitManageTransition(uint32_t handleId, NVSDK_NGX_Parameter* params)
{
    SplitTickParked();

    auto it = Dx12Contexts.find(handleId);

    if (it == Dx12Contexts.end() || it->second.feature == nullptr)
        return;

    IFeature_Dx12* f = it->second.feature.get();
    const bool want = SplitWanted();
    State& state = State::Instance();

    if (!want && Config::Instance()->DlssNrSplitPipeline.value_or_default() &&
        Config::Instance()->OutputScalingEnabled.value_or_default())
        DlssNr::SetSplitStatus("waiting: turn Output Scaling's Enable off -- the split reads its Ratio "
                               "and supersamples itself");

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &w);
    params->Get(NVSDK_NGX_Parameter_Height, &h);
    params->Get(NVSDK_NGX_Parameter_OutWidth, &ow);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &oh);

    // The display size the game originally asked for, captured whenever the block shows it.
    if (w != 0 && ow > w && ow != SplitDx12.displayWidth &&
        (SplitDx12.displayWidth == 0 || ow > SplitDx12.displayWidth))
    {
        SplitDx12.displayWidth = ow;
        SplitDx12.displayHeight = oh;
    }

    unsigned int desiredW = 0, desiredH = 0;
    SplitDesiredTarget(f->RenderWidth(), f->RenderHeight(), &desiredW, &desiredH);

    const bool matches = f->TargetWidth() == desiredW && f->TargetHeight() == desiredH;

    // A different geometry is now desired: the retry budget starts over.
    if (desiredW != SplitDx12.lastDesiredWidth)
    {
        SplitDx12.lastDesiredWidth = desiredW;
        SplitDx12.armTries = 0;
    }

    // A recreation is mid-flight: keep the block's output size aimed where it is going, in case the
    // game rewrites it every frame, and otherwise stay out of the way.
    if (it->second.changeBackendCounter != 0)
    {
        if (want && desiredW != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, desiredW);
            params->Set(NVSDK_NGX_Parameter_OutHeight, desiredH);
        }
        else if (!want && SplitDx12.displayWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
        }

        return;
    }

    if (want && !matches)
    {
        // If recreations complete and the feature still does not match, something else owns its
        // geometry. Give up loudly rather than re-creating every frame, which is a device-killing storm.
        if (SplitDx12.armTries >= 3)
        {
            if (!SplitDx12.failed)
            {
                SplitDx12.failed = true;
                DlssNr::SetSplitStatus("failed: the feature will not hold the requested size (see the log)");
                LOG_ERROR("DLSS-NR split: three recreations did not reach {}x{}; giving up", desiredW,
                          desiredH);
            }

            return;
        }

        if (w == 0 || (SplitDx12.displayWidth == 0 && (ow == 0 || ow <= w)))
        {
            static bool saidNothing = false;

            if (!saidNothing)
            {
                saidNothing = true;
                LOG_INFO("DLSS-NR split: nothing to split at {}x{} -> {}x{}; needs a render scale below "
                         "native",
                         w, h, ow, oh);
            }

            DlssNr::SetSplitStatus("waiting: needs a render scale below native (set DLSS Quality or lower)");
            return;
        }

        ++SplitDx12.armTries;
        params->Set(NVSDK_NGX_Parameter_OutWidth, desiredW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, desiredH);
        state.newBackend = Upscaler::DLSSD;
        state.changeBackend[handleId] = true;

        LOG_INFO("DLSS-NR split: re-creating Ray Reconstruction {}x{} -> {}x{} in place",
                 f->RenderWidth(), f->RenderHeight(), desiredW, desiredH);
        DlssNr::SetSplitStatus("re-creating Ray Reconstruction...");
        return;
    }

    if (!want && SplitDx12.displayWidth != 0 &&
        (f->TargetWidth() != SplitDx12.displayWidth || f->TargetHeight() != SplitDx12.displayHeight))
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
        state.newBackend = Upscaler::DLSSD;
        state.changeBackend[handleId] = true;

        if (SplitDx12.intermediate != nullptr)
        {
            SplitDx12.parkedIntermediate = SplitDx12.intermediate;
            SplitDx12.intermediate = nullptr;
        }

        SplitParkEnlargement();
        DlssNr::SetSplitActive(false);
        DlssNr::SetSplitStatus("");

        LOG_INFO("DLSS-NR split: returning Ray Reconstruction to {}x{} -> {}x{} in place",
                 f->RenderWidth(), f->RenderHeight(), SplitDx12.displayWidth, SplitDx12.displayHeight);
        return;
    }
}

// Clamps the game's Ray Reconstruction feature at creation, for launches with the split already on.
static void SplitOnCreate(NVSDK_NGX_Feature featureId, NVSDK_NGX_Parameter* params)
{
    if (featureId != NVSDK_NGX_Feature_RayReconstruction || !SplitWanted() || params == nullptr)
        return;

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &w);
    params->Get(NVSDK_NGX_Parameter_Height, &h);
    params->Get(NVSDK_NGX_Parameter_OutWidth, &ow);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &oh);

    if (w == 0 || h == 0 || ow <= w || oh <= h)
    {
        LOG_INFO("DLSS-NR split: nothing to split ({}x{} -> {}x{}); running conventionally", w, h, ow, oh);
        return;
    }

    SplitDx12.displayWidth = ow;
    SplitDx12.displayHeight = oh;

    unsigned int desiredW = 0, desiredH = 0;
    SplitDesiredTarget(w, h, &desiredW, &desiredH);
    params->Set(NVSDK_NGX_Parameter_OutWidth, desiredW);
    params->Set(NVSDK_NGX_Parameter_OutHeight, desiredH);

    LOG_INFO("DLSS-NR split: Ray Reconstruction created {}x{} -> {}x{}; display {}x{} will be reached "
             "at the end of the chain",
             w, h, desiredW, desiredH, ow, oh);
}

static ID3D12Resource* SplitScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int w,
                                    unsigned int h)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    return res;
}

// Makes sure the oversized working image exists at the given size, parking a mismatched one.
static bool SplitEnsureOversized(unsigned int w, unsigned int h, DXGI_FORMAT format)
{
    if (SplitDx12.oversized != nullptr &&
        ((unsigned int) SplitDx12.oversized->GetDesc().Width != w ||
         SplitDx12.oversized->GetDesc().Height != h))
        SplitParkEnlargement();

    if (SplitDx12.oversized == nullptr)
        SplitDx12.oversized = SplitScratch(D3D12Device, format, w, h);

    return SplitDx12.oversized != nullptr;
}

// The final downscale, through OptiScaler's own filter so the look matches Output Scaling's.
static bool SplitDownscale(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* from, ID3D12Resource* to)
{
    if (SplitDx12.downscaler == nullptr)
        SplitDx12.downscaler = std::make_unique<OS_Dx12>("DLSS-NR Split Downscale", D3D12Device, false);

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = from;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &b);

    const bool ok = SplitDx12.downscaler->Dispatch(cmdList, from, to);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmdList->ResourceBarrier(1, &b);

    return ok;
}

// The per-frame orchestration. Returns true when it handled the evaluate, with the result in outResult.
static bool SplitEvaluateRR(ID3D12GraphicsCommandList* cmdList, const NVSDK_NGX_Handle* handle,
                            NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback,
                            NVSDK_NGX_Result* outResult)
{
    if (!SplitWanted() || SplitDx12.displayWidth == 0)
        return false;

    unsigned int renderW = 0, renderH = 0;

    // Observable state only: operate when the feature's geometry matches what the settings call for and
    // no recreation is in flight. Every transition frame falls through to the conventional path.
    {
        auto it = Dx12Contexts.find(handle->Id);

        if (it == Dx12Contexts.end() || it->second.feature == nullptr ||
            it->second.changeBackendCounter != 0)
            return false;

        IFeature_Dx12* f = it->second.feature.get();
        renderW = f->RenderWidth();
        renderH = f->RenderHeight();

        unsigned int desiredW = 0, desiredH = 0;
        SplitDesiredTarget(renderW, renderH, &desiredW, &desiredH);

        if (f->TargetWidth() != desiredW || f->TargetHeight() != desiredH)
            return false;
    }

    ID3D12Resource* gameOutput = nullptr;
    ID3D12Resource* gameColor = nullptr;
    params->Get(NVSDK_NGX_Parameter_Output, &gameOutput);
    params->Get(NVSDK_NGX_Parameter_Color, &gameColor);

    if (gameOutput == nullptr || D3D12Device == nullptr)
        return false;

    const DXGI_FORMAT workFormat = codec::TypedFormat(gameOutput->GetDesc().Format);
    const float mult = SplitRatio();
    const bool includeRR =
        Config::Instance()->DlssNrSplitIncludeRR.value_or_default() && mult > 1.0f;

    char status[160];

    if (includeRR)
    {
        // RR itself upscales to the supersampled size; the model works on that image; only the
        // downscale remains. The conventional Output Scaling look with the model in the chain.
        const auto targetW = (unsigned int) (SplitDx12.displayWidth * mult + 0.5f);
        const auto targetH = (unsigned int) (SplitDx12.displayHeight * mult + 0.5f);

        if (!SplitEnsureOversized(targetW, targetH, workFormat))
        {
            LOG_ERROR("DLSS-NR split: the supersample target could not be created; falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            return false;
        }

        params->Set(NVSDK_NGX_Parameter_Output, SplitDx12.oversized);
        const NVSDK_NGX_Result rrResult = TryEvaluateOptiFeature(cmdList, handle, params, callback);

        if (rrResult != NVSDK_NGX_Result_Success)
        {
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            *outResult = rrResult;
            return true;
        }

        DlssNr::SetSplitActive(true);
        DlssNr::EvaluateAfterUpscale(cmdList, params, true);

        const bool ok = SplitDownscale(cmdList, SplitDx12.oversized, gameOutput);
        params->Set(NVSDK_NGX_Parameter_Output, gameOutput);

        if (!ok)
        {
            LOG_ERROR("DLSS-NR split: the downscale failed; falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            *outResult = NVSDK_NGX_Result_Fail;
            return true;
        }

        std::snprintf(status, sizeof(status),
                      "running: RR supersampled x%.2f -> NR -> downscale (RR included)", mult);
        DlssNr::SetSplitStatus(status);
        *outResult = NVSDK_NGX_Result_Success;
        return true;
    }

    // RR 1:1: denoise at render size, enhance there, enlarge once.
    if (SplitDx12.intermediate == nullptr)
    {
        SplitDx12.intermediate = SplitScratch(D3D12Device, workFormat, renderW, renderH);

        if (SplitDx12.intermediate == nullptr)
        {
            LOG_ERROR("DLSS-NR split: the intermediate could not be created; falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            return false;
        }
    }

    params->Set(NVSDK_NGX_Parameter_Output, SplitDx12.intermediate);
    const NVSDK_NGX_Result rrResult = TryEvaluateOptiFeature(cmdList, handle, params, callback);

    if (rrResult != NVSDK_NGX_Result_Success)
    {
        params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
        *outResult = rrResult;
        return true;
    }

    DlssNr::SetSplitActive(true);
    DlssNr::EvaluateAfterUpscale(cmdList, params, true);

    // The enlargement: to the supersampled size when the ratio asks, straight to display otherwise.
    const bool supersample = mult > 1.0f;
    const auto targetW =
        supersample ? (unsigned int) (SplitDx12.displayWidth * mult + 0.5f) : SplitDx12.displayWidth;
    const auto targetH =
        supersample ? (unsigned int) (SplitDx12.displayHeight * mult + 0.5f) : SplitDx12.displayHeight;

    if (SplitDx12.sr != nullptr && SplitDx12.srTargetWidth != targetW)
    {
        LOG_INFO("DLSS-NR split: rebuilding the enlargement for {}x{}", targetW, targetH);
        SplitParkEnlargement();
    }

    if (SplitDx12.sr == nullptr)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);

        auto sr = std::make_unique<DLSSFeatureDx12>(IFeature::GetNextHandleId(), params);

        if (!sr->Init(D3D12Device, cmdList, params) || !sr->IsInited())
        {
            LOG_ERROR("DLSS-NR split: the internal Super Resolution feature would not initialise; "
                      "falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            params->Set(NVSDK_NGX_Parameter_OutWidth, renderW);
            params->Set(NVSDK_NGX_Parameter_OutHeight, renderH);
            *outResult = rrResult;
            return true;
        }

        SplitDx12.sr = std::move(sr);
        SplitDx12.srTargetWidth = targetW;
        LOG_INFO("DLSS-NR split: internal Super Resolution running {}x{} -> {}x{}{}", renderW, renderH,
                 targetW, targetH, supersample ? " (supersampled)" : "");
    }

    const bool useOversized = supersample && SplitEnsureOversized(targetW, targetH, workFormat);

    params->Set(NVSDK_NGX_Parameter_Color, SplitDx12.intermediate);
    params->Set(NVSDK_NGX_Parameter_Output, useOversized ? SplitDx12.oversized : gameOutput);
    params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
    params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);

    bool srOk = SplitDx12.sr->Evaluate(cmdList, params);

    if (srOk && useOversized)
        srOk = SplitDownscale(cmdList, SplitDx12.oversized, gameOutput);

    if (gameColor != nullptr)
        params->Set(NVSDK_NGX_Parameter_Color, gameColor);

    params->Set(NVSDK_NGX_Parameter_Output, gameOutput);

    if (!srOk)
    {
        LOG_ERROR("DLSS-NR split: the enlargement failed; falling back");
        SplitDx12.failed = true;
        DlssNr::SetSplitActive(false);
        DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
    }
    else if (useOversized)
    {
        std::snprintf(status, sizeof(status),
                      "running: RR 1:1 -> NR -> SR x%.2f supersampled -> downscale", mult);
        DlssNr::SetSplitStatus(status);
    }
    else
    {
        DlssNr::SetSplitStatus("running: RR 1:1 -> NR -> internal SR");
    }

    *outResult = srOk ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
    return true;
}

