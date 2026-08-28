#include "pch.h"
#include "Util.h"
#include "Config.h"

#include "NVNGX_DLSS.h"
#include "NVNGX_Parameter.h"
#include "proxies/NVNGX_Proxy.h"
#include "dlssnr/DlssNr_Dx12.h"
#include <upscalers/dlss/DLSSFeature_Dx12.h>
#include <dlssnr/DlssNr_Codec.h>
#include <shaders/output_scaling/OS_Dx12.h>

#include <upscalers/FeatureProvider_Dx12.h>
#include "upscalers/dlss/DLSSFeature_Dx12.h"

#include <framegen/nvngx/Nvngx_FG.h>
#include "FG/FSR3_Dx12_FG.h"
#include "FG/Upscaler_Inputs_Dx12.h"

#include <imgui/ImGuiNotify.hpp>

#include <hooks/D3D12_Hooks.h>

#include <dxgi1_4.h>
#include <shared_mutex>
#include "detours/detours.h"
#include <ankerl/unordered_dense.h>
#include <misc/IdentifyGpu.h>

static ankerl::unordered_dense::map<unsigned int, ContextData<IFeature_Dx12>> Dx12Contexts;
static std::unordered_map<unsigned int, NVSDK_NGX_Feature> HandleToFeature;

static ID3D12Device* D3D12Device = nullptr;
static int evalCounter = 0;
static bool shutdown = false;
static bool _skipInit = false;
static wchar_t const** paths;

class ScopedInitDx12
{
  private:
    bool previousState;

  public:
    ScopedInitDx12()
    {
        previousState = _skipInit;
        _skipInit = true;
    }

    ~ScopedInitDx12() { _skipInit = previousState; }
};

static void UpdateInitPaths(NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    State::Instance().NVNGX_FeatureInfo_Paths.clear();

    if (InFeatureInfo != nullptr)
    {
        auto exePath = Util::ExePath().remove_filename();

        std::optional<std::filesystem::path> nvngxDlssPath = std::nullopt;
        std::optional<std::filesystem::path> nvngxDlssDPath = std::nullopt;
        std::optional<std::filesystem::path> nvngxDlssGPath = std::nullopt;

        // Check DLSS path
        if (State::Instance().NVNGX_DLSS_Path.has_value())
        {
            nvngxDlssPath = std::filesystem::path(State::Instance().NVNGX_DLSS_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlss.dll");

            if (path.has_value())
                nvngxDlssPath = path.value();
        }

        // Check DLSS-D path
        if (State::Instance().NVNGX_DLSSD_Path.has_value())
        {
            nvngxDlssDPath = std::filesystem::path(State::Instance().NVNGX_DLSSD_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlssd.dll");

            if (path.has_value())
                nvngxDlssDPath = path.value();
        }

        // Check DLSS-G path
        if (State::Instance().NVNGX_DLSSG_Path.has_value())
        {
            nvngxDlssGPath = std::filesystem::path(State::Instance().NVNGX_DLSSG_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlssg.dll");

            if (path.has_value())
                nvngxDlssGPath = path.value();
        }

        // Override locations
        if (Config::Instance()->DLSSFeaturePath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(Config::Instance()->DLSSFeaturePath.value());

        // If DLSS path is overriden
        if (Config::Instance()->NVNGX_DLSS_Library.has_value() && nvngxDlssPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssPath.value().parent_path().wstring());

        // OptiDll Path
        State::Instance().NVNGX_FeatureInfo_Paths.push_back(Config::Instance()->MainDllPath.value());

        // Original paths from NVNGX
        for (size_t i = 0; i < InFeatureInfo->PathListInfo.Length; i++)
        {
            const wchar_t* path = InFeatureInfo->PathListInfo.Path[i];
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(std::wstring(path));
        }

        // Exe path
        State::Instance().NVNGX_FeatureInfo_Paths.push_back(exePath.wstring());

        // If DLSS path is not overriden
        if (!Config::Instance()->NVNGX_DLSS_Library.has_value() && nvngxDlssPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssPath.value().parent_path().wstring());

        // Add found locations
        if (nvngxDlssDPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssDPath.value().parent_path().wstring());

        if (nvngxDlssGPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssGPath.value().parent_path().wstring());

        // Build pointer array
        paths = new const wchar_t*[State::Instance().NVNGX_FeatureInfo_Paths.size()];
        for (size_t i = 0; i < State::Instance().NVNGX_FeatureInfo_Paths.size(); ++i)
        {
            paths[i] = State::Instance().NVNGX_FeatureInfo_Paths[i].c_str();
            LOG_DEBUG("Feature Path [{}]: {}", i, wstring_to_string(State::Instance().NVNGX_FeatureInfo_Paths[i]));
        }

        InFeatureInfo->PathListInfo.Path = paths;
        InFeatureInfo->PathListInfo.Length = (int) State::Instance().NVNGX_FeatureInfo_Paths.size();
    }
}

#pragma region DLSS Init Calls

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_Ext(unsigned long long InApplicationId,
                                                        const wchar_t* InApplicationDataPath, ID3D12Device* InDevice,
                                                        NVSDK_NGX_Version InSDKVersion,
                                                        const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    State::Instance().NVNGX_ApplicationId = InApplicationId;
    State::Instance().NVNGX_ApplicationDataPath = std::wstring(InApplicationDataPath);
    State::Instance().NVNGX_Version = InSDKVersion;
    State::Instance().NVNGX_FeatureInfo = &localFeatureInfo;
    State::Instance().NVNGX_Version = InSDKVersion;

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init_Ext() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init_Ext");

            auto result = NVNGXProxy::D3D12_Init_Ext()(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion,
                                                       &localFeatureInfo);
            LOG_INFO("calling NVNGXProxy::D3D12_Init_Ext result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
        else
        {
            LOG_WARN("NVNGXProxy::NVNGXModule or NVNGXProxy::D3D12_Init_Ext is nullptr!");
        }
    }

    if (InFeatureInfo != nullptr && InSDKVersion > 0x0000013)
        State::Instance().NVNGX_Logger = InFeatureInfo->LoggingInfo;

    if (State::Instance().nvngxDx12Inited && InDevice == D3D12Device)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        Nvngx_FG::D3D12_Init_Ext(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);
    }

    LOG_INFO("AppId: {0}", InApplicationId);
    LOG_INFO("SDK: {0:x}", (unsigned int) InSDKVersion);
    LOG_INFO(L"InApplicationDataPath {0}", std::wstring(InApplicationDataPath));

    D3D12Device = InDevice;
    State::Instance().currentD3D12Device = InDevice;
    D3D12Hooks::HookDevice(InDevice);

    State::Instance().nvngxDx12Inited = true;

    UpscalerInputsDx12::Init(InDevice);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init(unsigned long long InApplicationId,
                                                    const wchar_t* InApplicationDataPath, ID3D12Device* InDevice,
                                                    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
                                                    NVSDK_NGX_Version InSDKVersion)
{
    LOG_FUNC();

    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init");

            auto result = NVNGXProxy::D3D12_Init()(InApplicationId, InApplicationDataPath, InDevice, &localFeatureInfo,
                                                   InSDKVersion);

            LOG_INFO("calling NVNGXProxy::D3D12_Init result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
    }

    if (State::Instance().nvngxDx12Inited && InDevice == D3D12Device)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    // if (State::Instance().activeFgInput == FGInput::NvngxFG)
    //{
    //     Nvngx_FG::D3D12_Init(InApplicationId, InApplicationDataPath, InDevice, InFeatureInfo, InSDKVersion);
    // }

    ScopedInitDx12 scopedInit {};
    auto result =
        NVSDK_NGX_D3D12_Init_Ext(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);

    LOG_DEBUG("was called NVSDK_NGX_D3D12_Init_Ext");
    return result;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_ProjectID(const char* InProjectId,
                                                              NVSDK_NGX_EngineType InEngineType,
                                                              const char* InEngineVersion,
                                                              const wchar_t* InApplicationDataPath,
                                                              ID3D12Device* InDevice, NVSDK_NGX_Version InSDKVersion,
                                                              const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InProjectId = project_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init_ProjectID() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init_ProjectID");

            auto result =
                NVNGXProxy::D3D12_Init_ProjectID()(InProjectId, InEngineType, InEngineVersion, InApplicationDataPath,
                                                   InDevice, InSDKVersion, &localFeatureInfo);

            LOG_INFO("calling NVNGXProxy::D3D12_Init_ProjectID result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
    }

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    if (State::Instance().nvngxDx12Inited && InDevice == D3D12Device)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    ScopedInitDx12 scopedInit {};
    auto result = NVSDK_NGX_D3D12_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);
    return result;
}

// Not sure about this one, original nvngx does not export this method
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType, const char* InEngineVersion,
    const wchar_t* InApplicationDataPath, ID3D12Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    LOG_FUNC();

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    if (State::Instance().nvngxDx12Inited)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    auto result = NVSDK_NGX_D3D12_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo);

    return result;
}

#pragma endregion

#pragma region DLSS Shutdown Calls

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Shutdown(void)
{
    shutdown = true;
    State::Instance().nvngxDx12Inited = false;

    D3D12Device = nullptr;

    State::Instance().currentFeature = nullptr;

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // UnhookAll();
    DLSSFeatureDx12::Shutdown(D3D12Device);

    // Added `&& !State::Instance().isShuttingDown` hack for crash on exit
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx12Inited() &&
        NVNGXProxy::D3D12_Shutdown() != nullptr && !State::Instance().isShuttingDown)
    {
        auto result = NVNGXProxy::D3D12_Shutdown()();
        NVNGXProxy::SetDx12Inited(false);
    }

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // HooksDx::UnHook();

    // Disabled to prevent crash
    if (State::Instance().currentFG != nullptr && State::Instance().activeFgInput == FGInput::Upscaler)
    {
        if (State::Instance().isShuttingDown)
            State::Instance().currentFG->Shutdown();
        else
            State::Instance().currentFG->DestroyFGContext();

        State::Instance().clearCapturedHudlesses = true;
    }

    shutdown = false;

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        Nvngx_FG::D3D12_Shutdown();
    }

    State::Instance().nvngxDx12Inited = false;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* InDevice)
{
    shutdown = true;
    State::Instance().nvngxDx12Inited = false;

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        Nvngx_FG::D3D12_Shutdown1(InDevice);
    }

    // Added `&& !State::Instance().isShuttingDown` hack for crash on exit
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx12Inited() &&
        NVNGXProxy::D3D12_Shutdown1() != nullptr && !State::Instance().isShuttingDown)
    {
        auto result = NVNGXProxy::D3D12_Shutdown1()(InDevice);
        NVNGXProxy::SetDx12Inited(false);
    }

    return NVSDK_NGX_D3D12_Shutdown();
}

#pragma endregion

#pragma region DLSS Parameter Calls

/**
 * @brief [Deprecated NGX API] Superceeded by NVSDK_NGX_AllocateParameters and NVSDK_NGX_GetCapabilityParameters.
 *
 * Retrieves a common NVSDK parameter map for providing params to the SDK. The lifetime of this
 * map is NOT managed by the application. It is expected to be managed internally by the SDK.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    // If DLSS is enabled and the real DLSS module is loaded, get native NGX table
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::D3D12_GetParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_GetParameters");
        auto result = NVNGXProxy::D3D12_GetParameters()(OutParameters);
        LOG_INFO("Calling NVNGXProxy::D3D12_GetParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        // Copy OptiScaler config to real NGX param table
        if (result == NVSDK_NGX_Result_Success)
        {
            InitNGXParameters(*OutParameters, API::DX12);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVPersistent);
            return NVSDK_NGX_Result_Success;
        }
    }

    // Get custom parameters if using custom backend
    static NVNGX_Parameters oldParams = NVNGX_Parameters(API::DX12, true);
    *OutParameters = &oldParams;
    InitNGXParameters(*OutParameters, API::DX12);

    LOG_DEBUG("Returning custom Opti parameters");

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new NVSDK parameter map pre-populated with NGX capabilities and information about available
 * features. The output parameter map may also be used in the same ways as a parameter map allocated with
 * AllocateParameters(). The lifetime of this map is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetCapabilityParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    // Get native DLSS params if DLSS is enabled and the module is loaded
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::IsDx12Inited() && NVNGXProxy::D3D12_GetCapabilityParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_GetCapabilityParameters");
        auto result = NVNGXProxy::D3D12_GetCapabilityParameters()(OutParameters);
        LOG_INFO("Calling NVNGXProxy::D3D12_GetCapabilityParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        if (result == NVSDK_NGX_Result_Success)
        {
            // Init external NGX table with current configuration and mark as dynamic+external
            InitNGXParameters(*OutParameters, API::DX12);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return NVSDK_NGX_Result_Success;
        }
    }

    // Get custom parameters if using custom backend
    auto& params = *(new NVNGX_Parameters(API::DX12, false));
    InitNGXParameters(&params, API::DX12);
    *OutParameters = &params;

    LOG_DEBUG("Returning custom Opti parameters");

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new parameter map used to provide parameters needed by the DLSS API. The lifetime of this map
 * is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::D3D12_AllocateParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_AllocateParameters");
        auto result = NVNGXProxy::D3D12_AllocateParameters()(OutParameters);
        LOG_INFO("Calling NVNGXProxy::D3D12_AllocateParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        if (result == NVSDK_NGX_Result_Success)
        {
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    auto* params = new NVNGX_Parameters(API::DX12, false);
    *OutParameters = params;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_PopulateParameters_Impl(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    InitNGXParameters(InParameters, API::DX12);

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        Nvngx_FG::D3D12_PopulateParameters_Impl(InParameters);
    }

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Destroys a given input parameter map created with AllocateParameters or GetCapabilityParameters.
 Must not be called on maps returned by GetParameters(). Unsupported tables will not be freed.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    const bool isUsingDlss = Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule();
    const bool success = TryDestroyNGXParameters(InParameters, NVNGXProxy::D3D12_DestroyParameters());

    if (isUsingDlss)
        UpscalerInputsDx12::Reset();

    return success ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

#pragma endregion

#pragma region DLSS Feature Calls

static Upscaler GetUpscalerBackend()
{
    Upscaler upscaler = Upscaler::XeSS; // Default

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    if (NVNGXProxy::IsDx12Inited() && primaryGpu.dlssCapable)
        upscaler = Upscaler::DLSS;

    if (primaryGpu.fsr4Support != FSR4Support::None)
        upscaler = Upscaler::FFX;

    if (Config::Instance()->Dx12Upscaler.has_value())
        upscaler = Config::Instance()->Dx12Upscaler.value();

    return upscaler;
}

static bool EnsureD3D12Device(ID3D12GraphicsCommandList* cmdList)
{
    if (D3D12Device)
        return true;

    LOG_DEBUG("Get D3D12 device from InCmdList!");

    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&D3D12Device))) || !D3D12Device)
    {
        LOG_ERROR("Can't get Dx12Device from InCmdList!");
        return false;
    }

    return true;
}

static NVSDK_NGX_Result TryEvaluateOptiFeature(ID3D12GraphicsCommandList* InCmdList,
                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                               NVSDK_NGX_Parameter* InParameters,
                                               PFN_NVSDK_NGX_ProgressCallback InCallback);


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
    int downscalerKind = -1;                // the Downscaler choice it was built with
    unsigned int srTargetWidth = 0;         // what the enlargement was built to produce
    bool failed = false;


    // Geometry-change control, so nothing can ever loop recreations. The pair doubles as the steering
    // target while a recreation is mid-flight and the feature does not exist to ask.
    unsigned int lastDesiredWidth = 0;
    unsigned int lastDesiredHeight = 0;
    int armTries = 0;

    // The split only ever restores geometry it changed itself, exactly once. Without this, a feature
    // legitimately resized by something else -- conventional Output Scaling above all -- read as
    // "wrong" forever, and the restore fought it in an endless recreation loop that shredded the frame.
    bool geometryOwned = false;
    bool restorePending = false;

    // Rapid toggling must not thrash recreations: the wanted-state has to hold still briefly before the
    // machinery moves. Steering of in-flight transitions is unaffected.
    bool lastWant = false;
    int stableFrames = 0;

    // The game's own quality-mode declaration, captured with the display size. A feature built at a
    // ratio that contradicts the block's declared quality mode is created fine and then refused at
    // evaluate (0xbad00000) -- so every geometry the split asks for carries a matching declaration,
    // and the game's own is restored with its geometry.
    unsigned int origPerfQuality = 0xffffffff;

    // The driver refused the supersampled enlargement once this session: run at display size instead
    // of latching the whole split off. Cleared on a toggle edge or Retry.
    bool supersampleRefused = false;
};

static SplitState SplitDx12;

namespace DlssNr
{
extern void (*g_splitRetryHook)();
}

static void SplitClearFailure()
{
    SplitDx12.failed = false;
    SplitDx12.armTries = 0;
    SplitDx12.supersampleRefused = false;
    DlssNr::SetSplitStatus("");
}

static const bool g_splitRetryRegistered = [] {
    DlssNr::g_splitRetryHook = &SplitClearFailure;
    return true;
}();

// Retired on a live change: still referenced by command lists submitted over the last frames, so each
// entry is released a number of evaluates later. A list, so rapid changes queue rather than forcing an
// early free -- releasing under the GPU is the mistake this project has paid for repeatedly.
struct SplitRetired
{
    ID3D12Resource* resource = nullptr;
    std::unique_ptr<IFeature_Dx12> feature;
    std::unique_ptr<OS_Dx12> shader;
    int framesLeft = 32;
};

static std::vector<SplitRetired> SplitParkedList;

static void SplitParkResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    SplitRetired r;
    r.resource = res;
    res = nullptr;
    SplitParkedList.push_back(std::move(r));
}

static bool SplitWanted()
{
    const Config& cfg = *Config::Instance();
    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrSplitPipeline.value_or_default() &&
           !SplitDx12.failed;
}

// Whether the user wants supersampling: Output Scaling's own Enable, as saved -- the runtime value is
// forced off while the split runs, because the split does the supersampling itself.
static bool SplitOsIntent()
{
    return Config::Instance()->OutputScalingEnabled.value_for_config().value_or(false);
}

// The split absorbs Output Scaling while it runs: the runtime flag goes off, so no feature geometry can
// be steered by two owners, and the split supersamples at the Ratio itself. Given back the moment the
// split stands down, so conventional Output Scaling resumes.
static void SplitAbsorbOs()
{
    if (SplitOsIntent() && Config::Instance()->OutputScalingEnabled.value_or_default())
    {
        Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
        LOG_INFO("DLSS-NR split: absorbing Output Scaling -- the split supersamples in its place");
    }
}

static void SplitRestoreOs()
{
    if (SplitOsIntent() && !Config::Instance()->OutputScalingEnabled.value_or_default())
    {
        Config::Instance()->OutputScalingEnabled.set_volatile_value(true);
        LOG_INFO("DLSS-NR split: returning Output Scaling to its own machinery");
    }
}

// The supersample ratio in force: the Output Scaling Ratio, when the user has Output Scaling on.
static float SplitRatio()
{
    if (!SplitOsIntent() || SplitDx12.supersampleRefused)
        return 1.0f;

    float mult = Config::Instance()->OutputScalingMultiplier.value_or_default();

    if (mult > 3.0f)
        mult = 3.0f;

    return mult > 1.05f ? mult : 1.0f;
}

// The quality mode that honestly describes an upscale from renderW to targetW. The thresholds sit
// between the modes' nominal ratios (Quality 1.5x, Balanced 1.72x, Performance 2x, Ultra
// Performance 3x).
static unsigned int SplitPerfQuality(unsigned int renderW, unsigned int targetW)
{
    const float r = renderW == 0 ? 1.0f : (float) targetW / (float) renderW;

    if (r >= 2.5f)
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;

    if (r >= 1.85f)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;

    if (r >= 1.6f)
        return NVSDK_NGX_PerfQuality_Value_Balanced;

    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
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

// Frees what live changes parked, entry by entry, once enough evaluates have passed that nothing in
// flight can still reference each one.
static void SplitTickParked()
{
    for (size_t i = 0; i < SplitParkedList.size();)
    {
        if (--SplitParkedList[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        if (SplitParkedList[i].resource != nullptr)
            SplitParkedList[i].resource->Release();

        SplitParkedList.erase(SplitParkedList.begin() + i);
    }
}

// Parks the enlargement stage for deferred release.
static void SplitParkEnlargement()
{
    if (SplitDx12.sr != nullptr)
    {
        SplitRetired r;
        r.feature = std::move(SplitDx12.sr);
        SplitParkedList.push_back(std::move(r));
    }

    if (SplitDx12.downscaler != nullptr)
    {
        SplitRetired r;
        r.shader = std::move(SplitDx12.downscaler);
        SplitParkedList.push_back(std::move(r));
    }

    SplitParkResource(SplitDx12.oversized);
    SplitDx12.srTargetWidth = 0;
}

// Applies the toggles while the game runs, by re-creating the Ray Reconstruction feature at whatever
// geometry the settings currently call for. The split itself never trusts this function: it operates
// only when the feature's observed geometry matches the desired one, so every transition frame falls
// through to the conventional path.
static void SplitManageTransition(uint32_t handleId, NVSDK_NGX_Parameter* params)
{
    SplitTickParked();

    auto it = Dx12Contexts.find(handleId);

    if (it == Dx12Contexts.end())
        return;

    const bool want = SplitWanted();
    State& state = State::Instance();

    if (want)
        SplitAbsorbOs();
    else
        SplitRestoreOs();

    // A recreation is mid-flight -- the old feature may already be destroyed, and ChangeFeature's first
    // phase stamps the block's output size back to the old feature's. Keep the block aimed at the
    // destination every frame until the new feature exists, or the parse reads the stamped size and the
    // recreation reproduces exactly what it was meant to replace.
    if (it->second.changeBackendCounter != 0 || it->second.feature == nullptr)
    {
        // Steer only transitions that are ours: the split's own arm, or its one restore. Anything else
        // in flight -- Output Scaling's recreations included -- is none of our business.
        if (want && SplitDx12.geometryOwned && SplitDx12.lastDesiredWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.lastDesiredWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.lastDesiredHeight);

            unsigned int rw = 0;
            params->Get(NVSDK_NGX_Parameter_Width, &rw);

            if (rw != 0 && SplitDx12.displayWidth != 0 &&
                SplitDx12.lastDesiredWidth > SplitDx12.displayWidth)
                params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                            SplitPerfQuality(rw, SplitDx12.lastDesiredWidth));
        }
        else if (SplitDx12.restorePending && SplitDx12.displayWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

            if (SplitDx12.origPerfQuality != 0xffffffff)
                params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);
        }

        return;
    }

    // A completed transition ends any pending restore, whatever geometry resulted -- one attempt only.
    SplitDx12.restorePending = false;

    // Debounce: act on a changed wanted-state only once it has held still. A fast enable/disable run
    // otherwise burns the whole retry budget on transitions that were each individually succeeding.
    if (want != SplitDx12.lastWant)
    {
        SplitDx12.lastWant = want;
        SplitDx12.stableFrames = 0;
        SplitDx12.supersampleRefused = false;
        return;
    }

    ++SplitDx12.stableFrames;

    IFeature_Dx12* f = it->second.feature.get();

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &w);
    params->Get(NVSDK_NGX_Parameter_Height, &h);
    params->Get(NVSDK_NGX_Parameter_OutWidth, &ow);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &oh);

    // The display size the game originally asked for, captured exactly once. This block is also
    // written by us -- the internal SR's oversized target goes through it -- and treating later, larger
    // values as the game's own compounded the supersample every frame until the device hung.
    if (SplitDx12.displayWidth == 0 && w != 0 && ow > w)
    {
        SplitDx12.displayWidth = ow;
        SplitDx12.displayHeight = oh;
        params->Get(NVSDK_NGX_Parameter_PerfQualityValue, &SplitDx12.origPerfQuality);
    }

    unsigned int desiredW = 0, desiredH = 0;
    SplitDesiredTarget(f->RenderWidth(), f->RenderHeight(), &desiredW, &desiredH);

    const bool matches = f->TargetWidth() == desiredW && f->TargetHeight() == desiredH;

    // A different geometry is now desired: the retry budget starts over.
    if (desiredW != SplitDx12.lastDesiredWidth || desiredH != SplitDx12.lastDesiredHeight)
    {
        SplitDx12.lastDesiredWidth = desiredW;
        SplitDx12.lastDesiredHeight = desiredH;
        SplitDx12.armTries = 0;
    }

    // The feature is where the settings want it: the retry budget is refunded, so only consecutive
    // failures ever exhaust it.
    if (want && matches)
        SplitDx12.armTries = 0;

    const bool settled = SplitDx12.stableFrames >= 30;

    if (want && !matches && settled)
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

        // A feature built oversized must declare the quality mode it actually is, or the driver
        // creates it and then refuses every evaluate. The 1:1 arrangement keeps the game's own value.
        if (desiredW > SplitDx12.displayWidth && SplitDx12.displayWidth != 0)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                        SplitPerfQuality(f->RenderWidth(), desiredW));
        else if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);
        state.newBackend = Upscaler::DLSSD;
        state.changeBackend[handleId] = true;
        SplitDx12.geometryOwned = true;

        LOG_INFO("DLSS-NR split: re-creating Ray Reconstruction {}x{} -> {}x{} in place",
                 f->RenderWidth(), f->RenderHeight(), desiredW, desiredH);
        DlssNr::SetSplitStatus("re-creating Ray Reconstruction...");
        return;
    }

    if (!want && settled && SplitDx12.geometryOwned && SplitDx12.displayWidth != 0 &&
        (f->TargetWidth() != SplitDx12.displayWidth || f->TargetHeight() != SplitDx12.displayHeight))
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

        if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);

        state.newBackend = Upscaler::DLSSD;
        state.changeBackend[handleId] = true;
        SplitDx12.geometryOwned = false;
        SplitDx12.restorePending = true;

        SplitParkResource(SplitDx12.intermediate);
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
    SplitDx12.lastDesiredWidth = desiredW;
    SplitDx12.lastDesiredHeight = desiredH;
    SplitDx12.geometryOwned = true;

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

static void SplitBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res,
                         D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

// The final downscale, through OptiScaler's own filter so the look matches Output Scaling's.
static bool SplitDownscale(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* from, ID3D12Resource* to)
{
    // The downscaler's pipeline is baked at construction, but its dispatch reads the Downscaler
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
    }

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
        // DLSS refuses ratios beyond 4x its input, so the target is capped there.
        auto targetW = (unsigned int) (SplitDx12.displayWidth * mult + 0.5f);
        auto targetH = (unsigned int) (SplitDx12.displayHeight * mult + 0.5f);
        targetW = targetW > renderW * 4 ? renderW * 4 : targetW;
        targetH = targetH > renderH * 4 ? renderH * 4 : targetH;

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
            // The feature matched and was stable, so this is a genuine refusal of the supersampled
            // arrangement. Drop Include RR (runtime only -- the saved checkbox survives) and let the
            // manager re-arm the plain split, rather than failing every frame from here on.
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            LOG_ERROR("DLSS-NR split: Ray Reconstruction refused to run at {}x{} -> {}x{}; dropping "
                      "Include RR",
                      renderW, renderH, targetW, targetH);
            Config::Instance()->DlssNrSplitIncludeRR.set_volatile_value(false);
            DlssNr::SetSplitStatus("include-RR refused at this ratio; running the split without it");
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

        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

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
    auto targetW =
        supersample ? (unsigned int) (SplitDx12.displayWidth * mult + 0.5f) : SplitDx12.displayWidth;
    auto targetH =
        supersample ? (unsigned int) (SplitDx12.displayHeight * mult + 0.5f) : SplitDx12.displayHeight;

    // DLSS refuses ratios beyond 4x its input, so the target is capped there.
    targetW = targetW > renderW * 4 ? renderW * 4 : targetW;
    targetH = targetH > renderH * 4 ? renderH * 4 : targetH;

    if (SplitDx12.sr != nullptr && SplitDx12.srTargetWidth != targetW)
    {
        LOG_INFO("DLSS-NR split: rebuilding the enlargement for {}x{}", targetW, targetH);
        SplitParkEnlargement();
    }

    if (SplitDx12.sr == nullptr)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);

        // The enlargement parses the game's block, whose quality mode describes the game's own ratio.
        // Built supersampled, it must declare what it actually is or the driver refuses it at evaluate.
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitPerfQuality(renderW, targetW));

        auto sr = std::make_unique<DLSSFeatureDx12>(IFeature::GetNextHandleId(), params);
        const bool srInited = sr->Init(D3D12Device, cmdList, params) && sr->IsInited();

        if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);

        if (!srInited)
        {
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

            if (supersample && !SplitDx12.supersampleRefused)
            {
                // The supersampled enlargement was refused; run at display size instead of dying.
                SplitDx12.supersampleRefused = true;
                LOG_ERROR("DLSS-NR split: the supersampled enlargement would not initialise; dropping "
                          "to display size");
                DlssNr::SetSplitStatus("supersample refused here; enlargement at display size");
                *outResult = rrResult;
                return true;
            }

            LOG_ERROR("DLSS-NR split: the internal Super Resolution feature would not initialise; "
                      "falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
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

    // The internal SR consumes the intermediate as an input, and NGX requires inputs shader-readable.
    // The model's pass leaves it in UNORDERED_ACCESS, and an input in the wrong state is undefined
    // reads -- which upscales to garbage without a single error anywhere.
    SplitBarrier(cmdList, SplitDx12.intermediate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // The feature was created with IsHDR and no AutoExposure, so it requires an exposure texture. The
    // game supplies one under its Ray Reconstruction name; hand it over under the name SR reads, or the
    // result is black -- the community guide's exposure trap, in its two-feature form.
    ID3D12Resource* rrExposure = nullptr;
    params->Get("DLSSD.ExposureTexture", &rrExposure);

    if (rrExposure != nullptr)
        params->Set("ExposureTexture", rrExposure);

    // And the game's sharpness would switch on RCAS inside our SR, against motion vectors it does not
    // understand at this geometry. The enlargement is an enlargement, nothing more.
    float gameSharpness = 0.0f;
    params->Get(NVSDK_NGX_Parameter_Sharpness, &gameSharpness);
    params->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);

    bool srOk = SplitDx12.sr->Evaluate(cmdList, params);

    SplitBarrier(cmdList, SplitDx12.intermediate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    params->Set(NVSDK_NGX_Parameter_Sharpness, gameSharpness);

    if (srOk && useOversized)
        srOk = SplitDownscale(cmdList, SplitDx12.oversized, gameOutput);

    // The block goes back exactly as the game filled it -- including the output size, which we borrowed
    // for the oversized target and whose pollution once compounded the supersample until the device hung.
    if (gameColor != nullptr)
        params->Set(NVSDK_NGX_Parameter_Color, gameColor);

    params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
    params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
    params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

    if (!srOk && supersample && !SplitDx12.supersampleRefused)
    {
        // The driver refused the supersampled enlargement at evaluate. Run at display size from the
        // next frame instead of latching the whole split off; the enlargement is rebuilt for the new
        // target by the srTargetWidth check above.
        SplitDx12.supersampleRefused = true;
        LOG_ERROR("DLSS-NR split: the supersampled enlargement refused to run; dropping to display "
                  "size");
        DlssNr::SetSplitStatus("supersample refused here; enlargement at display size");
    }
    else if (!srOk)
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

static NVSDK_NGX_Result TryCreateOptiFeature(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID,
                                             NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle)
{
    State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    state.api = DX12;

    const uint32_t handleId = IFeature::GetNextHandleId();
    LOG_INFO("Creating OptiScaler feature, HandleId: {}", handleId);

    // Determine backend name
    Upscaler upscalerBackend;
    if (InFeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        upscalerBackend = GetUpscalerBackend();
        LOG_INFO("Creating {} upscaler feature", UpscalerDisplayName(upscalerBackend));
    }
    else
    {
        upscalerBackend = Upscaler::DLSSD;
        LOG_INFO("Creating DLSSD (Ray Reconstruction) feature");
    }

    // Root signature restoration setup
    const bool restoreCompute = cfg.RestoreComputeSignature.value_or_default();
    const bool restoreGraphics = cfg.RestoreGraphicSignature.value_or_default();
    const bool shouldRestoreSigs = restoreCompute || restoreGraphics;

    // To avoid capturing the upscaler creation
    D3D12Hooks::SetRootSignatureTracking(false);

    if (shouldRestoreSigs)
        D3D12Hooks::HookToCommandListLate(InCmdList);

    // Create context entry
    Dx12Contexts[handleId] = {};

    // Retrieve feature implementation
    if (!FeatureProvider_Dx12::GetFeature(upscalerBackend, handleId, InParameters, &Dx12Contexts[handleId].feature))
    {
        LOG_ERROR("Failed to retrieve feature implementation for '{}'", UpscalerDisplayName(upscalerBackend));

        D3D12Hooks::SetRootSignatureTracking(true);

        Dx12Contexts.erase(handleId);
        return NVSDK_NGX_Result_Fail;
    }

    // Ensure D3D12 device
    if (!EnsureD3D12Device(InCmdList))
    {
        LOG_ERROR("Failed to acquire D3D12 device");

        D3D12Hooks::SetRootSignatureTracking(true);

        // Partial cleanup � handle is allocated but context is incomplete
        Dx12Contexts.erase(handleId);
        return NVSDK_NGX_Result_Fail;
    }

    // Assign handle
    if (*OutHandle == nullptr)
        *OutHandle = new NVSDK_NGX_Handle { handleId };
    else
        (*OutHandle)->Id = handleId;

    state.autoExposure.reset();

    IFeature_Dx12* feature = Dx12Contexts[handleId].feature.get();

    // Initialize feature
    if (feature->Init(D3D12Device, InCmdList, InParameters))
    {
        state.currentFeature = feature;
        evalCounter = 0;
        UpscalerInputsDx12::Reset();
    }
    else
    {
        LOG_ERROR("Feature '{}' initialization failed falling back to FSR 2.1.2", UpscalerDisplayName(upscalerBackend));
        state.newBackend = Upscaler::FSR21;
        state.changeBackend[handleId] = true;
    }

    // Restore root signatures
    if (shouldRestoreSigs)
        D3D12Hooks::RestoreRoot(InCmdList);

    D3D12Hooks::SetRootSignatureTracking(true);

    if (state.activeFgInput == FGInput::Upscaler)
        state.fgChanged = true;

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Instantiates a new feature based on the given unique feature ID and param table and
 * provides a handle used to reference the feature elsewhere in the API. Currently supports
 * various TSR and Frame Generation algorithms, including a special case for DLSS-RR passthrough.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* InCmdList,
                                                             NVSDK_NGX_Feature InFeatureID,
                                                             NVSDK_NGX_Parameter* InParameters,
                                                             NVSDK_NGX_Handle** OutHandle)
{
    LOG_FUNC();

    if (!InCmdList)
    {
        LOG_ERROR("InCmdList is null");
        return NVSDK_NGX_Result_Fail;
    }

    if (!OutHandle)
    {
        LOG_ERROR("OutHandle is null");
        return NVSDK_NGX_Result_Fail;
    }

    const State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    // DLSSG replacements passthrough
    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None && Nvngx_FG::isDx12Available() &&
        InFeatureID == NVSDK_NGX_Feature_FrameGeneration)
    {
        LOG_INFO("Passthrough to DLSSG Replacement's CreateFeature for FrameGeneration");

        NVSDK_NGX_Result res = Nvngx_FG::D3D12_CreateFeature(InCmdList, InFeatureID, InParameters, OutHandle);

        if (*OutHandle)
        {
            LOG_INFO("Created modded DLSSG feature with HandleId: {}", (*OutHandle)->Id);
            HandleToFeature[(*OutHandle)->Id] = InFeatureID;
        }

        return res;
    }

    // Native DLSS passthrough (exclude SuperSampling and RayReconstruction)
    if (InFeatureID != NVSDK_NGX_Feature_SuperSampling && InFeatureID != NVSDK_NGX_Feature_RayReconstruction)
    {
        if (cfg.DLSSEnabled.value_or_default() && NVNGXProxy::InitDx12(D3D12Device) &&
            NVNGXProxy::D3D12_CreateFeature() != nullptr)
        {
            LOG_INFO("Passthrough to native NGX CreateFeature for feature {}", (int) InFeatureID);

            NVSDK_NGX_Result res = NVNGXProxy::D3D12_CreateFeature()(InCmdList, InFeatureID, InParameters, OutHandle);

            if (*OutHandle)
            {
                LOG_INFO("Native CreateFeature success, HandleId: {}", (*OutHandle)->Id);
                HandleToFeature[(*OutHandle)->Id] = InFeatureID;
            }
            else
            {
                LOG_INFO("Native CreateFeature failed: 0x{:X}", (uint32_t) res);
            }

            return res;
        }

        LOG_WARN("Native DLSS passthrough not available for feature {}", (int) InFeatureID);
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }

    // OptiScaler internal handling (SuperSampling or RayReconstruction)
    SplitOnCreate(InFeatureID, InParameters);

    auto tryResult = TryCreateOptiFeature(InCmdList, InFeatureID, InParameters, OutHandle);

    if (tryResult == NVSDK_NGX_Result_Success)
        HandleToFeature[(*OutHandle)->Id] = InFeatureID;

    return tryResult;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* InHandle)
{
    LOG_FUNC();

    if (!InHandle)
        return NVSDK_NGX_Result_Success;

    auto handleId = InHandle->Id;

    // Clean up framegen
    if (State::Instance().currentFG != nullptr && State::Instance().activeFgInput == FGInput::Upscaler)
    {
        State::Instance().fgChanged = true;
        State::Instance().currentFG->DestroyFGContext();
        State::Instance().clearCapturedHudlesses = true;
        UpscalerInputsDx12::Reset();
    }

    if (!shutdown)
        LOG_INFO("releasing feature with id {0}", handleId);

    // OptiScaler handles start after this offset. If it's outside this range, it doesn't belong to OptiScaler.
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
        {
            if (!shutdown)
                LOG_INFO("calling D3D12_ReleaseFeature for ({0})", handleId);

            // Clean up real DLSS feature
            auto result = NVNGXProxy::D3D12_ReleaseFeature()(InHandle);

            if (!shutdown)
                LOG_INFO("D3D12_ReleaseFeature result for ({0}): {1:X}", handleId, (UINT) result);

            return result;
        }
        else
        {
            if (!shutdown)
                LOG_INFO("D3D12_ReleaseFeature not available for ({0})", handleId);

            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }
    // Clean up OptiScaler feature with framegen
    else if (State::Instance().activeFgNvngx != FGNvngxReplacement::None && handleId >= NVNGX_PROVIDER_ID_OFFSET)
    {
        LOG_INFO("D3D12_ReleaseFeature modded DLSSG with HandleId: {0}", handleId);
        return Nvngx_FG::D3D12_ReleaseFeature(InHandle);
    }

    // Remove feature from context map
    if (auto it = Dx12Contexts.find(handleId); it != Dx12Contexts.end())
    {
        auto& entry = it->second;

        if (auto* deviceContext = entry.feature.get())
        {
            // Clear global reference if it matches
            if (deviceContext == State::Instance().currentFeature)
                State::Instance().currentFeature = nullptr;

            // Erase from map (smart pointer reset is implicit on erase)
            Dx12Contexts.erase(it);
        }
    }
    else
    {
        // Fallback Error Handling
        if (!shutdown)
            LOG_ERROR("can't release feature with id {0}!", handleId);
    }

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Used by the client application to check for feature support.
 * @param Adapter Device the feature is for.
 * @param FeatureDiscoveryInfo Specifies the feature being queried.
 * @param OutSupported Used to indicate whether a feature is supported and its requirements.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter* Adapter, const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo,
    NVSDK_NGX_FeatureRequirement* OutSupported)
{
    LOG_DEBUG("for ({0})", (int) FeatureDiscoveryInfo->FeatureID);

    const bool isUpscaling = FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling;
    const bool isFG = FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_FrameGeneration;
    const bool dlssgAdjacent = Nvngx_FG::isDx12Available() || State::Instance().activeFgInput == FGInput::DLSSG;

    if (isUpscaling || (isFG && dlssgAdjacent))
    {
        if (OutSupported == nullptr)
        {
            static auto tmp = NVSDK_NGX_FeatureRequirement();
            OutSupported = &tmp;
        }

        OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        OutSupported->MinHWArchitecture = 0;

        // Some old windows 10 os version
        strcpy_s(OutSupported->MinOSVersion, "10.0.10240.16384");
        return NVSDK_NGX_Result_Success;
    }

    if (Config::Instance()->DLSSEnabled.value_or_default() && IdentifyGpu::getPrimaryGpu().dlssCapable &&
        NVNGXProxy::NVNGXModule() == nullptr)
    {
        NVNGXProxy::InitNVNGX();
    }

    if (Config::Instance()->DLSSEnabled.value_or_default() && IdentifyGpu::getPrimaryGpu().dlssCapable &&
        NVNGXProxy::D3D12_GetFeatureRequirements() != nullptr)
    {
        LOG_DEBUG("D3D12_GetFeatureRequirements for ({0})", (int) FeatureDiscoveryInfo->FeatureID);
        auto result = NVNGXProxy::D3D12_GetFeatureRequirements()(Adapter, FeatureDiscoveryInfo, OutSupported);
        LOG_DEBUG("D3D12_GetFeatureRequirements result for ({0}): {1:X}", (int) FeatureDiscoveryInfo->FeatureID,
                  (UINT) result);

        return result;
    }
    else
    {
        LOG_DEBUG("D3D12_GetFeatureRequirements not available for ({0})", (int) FeatureDiscoveryInfo->FeatureID);
    }

    OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

static NVSDK_NGX_Result TryEvaluateOptiFeature(ID3D12GraphicsCommandList* InCmdList,
                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                               NVSDK_NGX_Parameter* InParameters,
                                               PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    State& state = State::Instance();
    const Config& cfg = *Config::Instance();
    const uint32_t handleId = InFeatureHandle->Id;

    auto ctxIt = Dx12Contexts.find(handleId);

    if (ctxIt == Dx12Contexts.end())
    {
        LOG_WARN("No context found for handle {}", handleId);
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    ContextData<IFeature_Dx12>& ctxData = ctxIt->second;
    IFeature_Dx12* feature = ctxData.feature.get();

    if (feature == nullptr) // Prevent source api name flicker when dlssg is active
        state.setInputApiName = state.currentInputApiName;

    const auto targetApiName =
        !state.setInputApiName.has_value() ? ApiUpscalerInput::DLSS_DX12 : state.setInputApiName.value();

    if (state.currentInputApiName != targetApiName)
        state.currentInputApiName = targetApiName;

    state.setInputApiName.reset();
    evalCounter++;

    // Skip evaluation for the first N frames if configured
    if (cfg.SkipFirstFrames.has_value() && evalCounter < cfg.SkipFirstFrames.value())
        return NVSDK_NGX_Result_Success;

    // Root signature restoration setup
    const bool restoreCompute = cfg.RestoreComputeSignature.value_or_default();
    const bool restoreGraphics = cfg.RestoreGraphicSignature.value_or_default();
    const bool shouldRestoreSigs = restoreCompute || restoreGraphics;

    if (shouldRestoreSigs)
    {
        D3D12Hooks::HookToCommandListLate(InCmdList);

        if (!D3D12Hooks::CanRestoreRootSignature(InCmdList))
        {
            LOG_DEBUG("Skipping upscaling because can't restore root signature");
            return NVSDK_NGX_Result_Success;
        }
    }

    if (InCallback)
        LOG_INFO("Progress callback provided but unused in synchronous OptiScaler path");

    // Resolution change detection (only for upscalers that may require recreation)
    if (feature != nullptr)
    {
        const bool isFFX =
            feature->GetUpscalerType() == Upscaler::FFX || feature->GetUpscalerType() == Upscaler::FFX_on12;
        const bool isFSR31OrLater = isFFX && feature->Version() >= feature_version { 3, 1, 0 };

        // FSR 3.1 supports upscaleSize that doesn't need reinit to change output resolution
        if (!isFSR31OrLater && feature->UpdateOutputResolution(InParameters))
            state.changeBackend[handleId] = true;
    }

    // To avoid capturing potential upscaler change (creation) and then upscaling itself
    D3D12Hooks::SetRootSignatureTracking(false);

    // Backend change or recreation requested
    if (state.changeBackend[handleId])
    {
        UpscalerInputsDx12::Reset();

        auto successfulPhase = FeatureProvider_Dx12::ChangeFeature(state.newBackend, D3D12Device, InCmdList, handleId,
                                                                   InParameters, &ctxData);
        feature = ctxData.feature.get();

        evalCounter = 0;

        if (ctxData.changeBackendCounter != 0 || !successfulPhase)
        {
            D3D12Hooks::SetRootSignatureTracking(true);
            return NVSDK_NGX_Result_Success;
        }
    }

    // Fallback to FSR 2.1.2 if feature failed to initialize and user didn't explicitly request it
    if (!feature->IsInited() && cfg.Dx12Upscaler.value_or_default() != Upscaler::FSR21)
    {
        LOG_WARN("Feature '{}' failed to initialize. Falling back to FSR 2.1.2", feature->Name());
        ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to FSR 2.1.2" });

        state.newBackend = Upscaler::FSR21;
        state.changeBackend[handleId] = true;

        D3D12Hooks::SetRootSignatureTracking(true);

        return NVSDK_NGX_Result_Success;
    }

    state.currentFeature = feature;

    // Prepare upscaling inputs
    UpscalerInputsDx12::UpscaleStart(InCmdList, InParameters, feature);
    FSR3FG::SetUpscalerInputs(InCmdList, InParameters, feature);

    // Evaluate the feature
    bool evalSuccess = false;
    {
        // Resource tracking
        UpscalerInputsDx12::UpscaleEnd(InCmdList, InParameters, feature);

        ScopedSkipHeapCapture skip {};
        evalSuccess = feature->Evaluate(InCmdList, InParameters);
    }

    if (!evalSuccess)
    {
        LOG_ERROR("Feature evaluation failed for '{}'", feature->Name());
        ImGui::InsertNotification({ ImGuiToastType::Error, 10000, "Upscaler failed to run!" });
    }

    // Restore root signatures
    if (shouldRestoreSigs)
        D3D12Hooks::RestoreRoot(InCmdList);

    D3D12Hooks::SetRootSignatureTracking(true);

    return evalSuccess ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

/**
 * @brief Per-frame feature execution. Runs a feature (upscaler, framegen, etc.) on a given command list using a
 * preexisting feature instance referenced by a unique handle.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* InCmdList,
                                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                                               NVSDK_NGX_Parameter* InParameters,
                                                               PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    if (!InFeatureHandle)
    {
        LOG_DEBUG("InFeatureHandle is null");
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    if (!InCmdList)
    {
        LOG_ERROR("InCmdList is null");
        return NVSDK_NGX_Result_Fail;
    }

    const uint32_t handleId = InFeatureHandle->Id;
    LOG_DEBUG("EvaluateFeature - Handle: {}, CmdList: {:p}", handleId, (void*) InCmdList);

    const State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    auto feature = HandleToFeature[handleId];
    static size_t evalWithoutFG = 0;
    bool fgCreated = std::any_of(HandleToFeature.begin(), HandleToFeature.end(),
                                 [](const auto& pair) { return pair.second == NVSDK_NGX_Feature_FrameGeneration; });

    static std::optional<float> lastDlssgCameraNear {};
    static std::optional<float> lastDlssgCameraFar {};

    if (feature == NVSDK_NGX_Feature_FrameGeneration)
    {
        evalWithoutFG = 0;

        int frameCount = 0;
        InParameters->Get("DLSSG.MultiFrameCount", &frameCount);
        State::Instance().dlssgDetectedInterpolationCount = frameCount;
        ReflexHooks::setDlssgFrameCount(frameCount);

        float dlssgCameraNear = 0.0f;
        float dlssgCameraFar = 0.0f;

        if (InParameters->Get("DLSSG.CameraNear", &dlssgCameraNear) == NVSDK_NGX_Result_Success)
            lastDlssgCameraNear = dlssgCameraNear;

        if (InParameters->Get("DLSSG.CameraFar", &dlssgCameraFar) == NVSDK_NGX_Result_Success)
            lastDlssgCameraFar = dlssgCameraFar;
    }
    else if (fgCreated)
    {
        evalWithoutFG++;

        if (evalWithoutFG == 6)
        {
            // Report FG as disabled
            State::Instance().dlssgDetectedInterpolationCount = 0;
            ReflexHooks::setDlssgFrameCount(0);
        }
    }

    // Native DLSS passthrough
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (cfg.DLSSEnabled.value_or_default() && NVNGXProxy::D3D12_EvaluateFeature() != nullptr)
        {
            LOG_DEBUG("Passthrough to native DLSS EvaluateFeature for handle {}", handleId);

            NVSDK_NGX_Result result =
                NVNGXProxy::D3D12_EvaluateFeature()(InCmdList, InFeatureHandle, InParameters, InCallback);
            LOG_DEBUG("Native DLSS EvaluateFeature result: 0x{:X}", (uint32_t) result);

            // Neural Rendering runs over what the upscaler just wrote, on the same list, so frame
            // generation interpolates from enhanced frames and the model still costs one run per
            // rendered frame. The feature check is the point: frame generation is handed depth and
            // motion vectors too, and its handle can reach here because the branch above does not
            // return, so filtering on the parameter block alone would run the model twice a frame.
            if (result == NVSDK_NGX_Result_Success && feature != NVSDK_NGX_Feature_FrameGeneration)
                DlssNr::EvaluateAfterUpscale(InCmdList, InParameters);

            return result;
        }

        LOG_DEBUG("Native DLSS EvaluateFeature not available for handle {}", handleId);
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    // DLSSG replacements passthrough
    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None && handleId >= NVNGX_PROVIDER_ID_OFFSET)
    {
        LOG_DEBUG("Passthrough to DLSSG Replacement's EvaluateFeature for handle {}", handleId);
        return Nvngx_FG::D3D12_EvaluateFeature(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    if (lastDlssgCameraNear.has_value())
        InParameters->Set("DLSSG.CameraNear", lastDlssgCameraNear.value());

    if (lastDlssgCameraFar.has_value())
        InParameters->Set("DLSSG.CameraFar", lastDlssgCameraFar.value());

    // The split pipeline: denoise 1:1, enhance at render resolution, enlarge once. The toggle applies
    // live -- the feature is re-created in place at the other geometry.
    if (feature == NVSDK_NGX_Feature_RayReconstruction)
    {
        SplitManageTransition(handleId, InParameters);

        NVSDK_NGX_Result splitResult = NVSDK_NGX_Result_Success;

        if (SplitEvaluateRR(InCmdList, InFeatureHandle, InParameters, InCallback, &splitResult))
            return splitResult;
    }

    // OptiScaler internal handling
    const NVSDK_NGX_Result optiResult = TryEvaluateOptiFeature(InCmdList, InFeatureHandle, InParameters, InCallback);

    // Same pass, for OptiScaler's own upscalers rather than native DLSS.
    if (optiResult == NVSDK_NGX_Result_Success && feature != NVSDK_NGX_Feature_FrameGeneration)
        DlssNr::EvaluateAfterUpscale(InCmdList, InParameters);

    return optiResult;
}

#pragma endregion

#pragma region DLSS Buffer Size Call

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature InFeatureId,
                                                                    const NVSDK_NGX_Parameter* InParameters,
                                                                    size_t* OutSizeInBytes)
{
    if (OutSizeInBytes == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::None && InFeatureId == NVSDK_NGX_Feature_FrameGeneration)
    {
        return Nvngx_FG::D3D12_GetScratchBufferSize(InFeatureId, InParameters, OutSizeInBytes);
    }

    LOG_WARN("-> 52428800");
    *OutSizeInBytes = 52428800;
    return NVSDK_NGX_Result_Success;
}

#pragma endregion
