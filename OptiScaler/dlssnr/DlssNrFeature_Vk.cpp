#include "pch.h"

#include "DlssNrFeature_Vk.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <NVNGX_Parameter.h>

#include <shaders/dlssnr/DlssNr_Vk.h>
#include <shaders/output_scaling/OS_Vk.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace DlssNr
{

namespace
{

// The forwarder's Vulkan surface. The model checks its caller's module path and requires nvngx.dll in
// it, whichever API is being used, so these calls go through the same shim the D3D12 path does.
using PFN_VkProbe = int(__cdecl*)(const wchar_t*);
using PFN_VkInit = int(__cdecl*)(const wchar_t*, const wchar_t*, void*, void*, void*, int);
using PFN_VkCreate = void*(__cdecl*)(void*, void*, unsigned int, unsigned int, int, float, int, float, float, float,
                                     int, int);
using PFN_VkEvaluate = int(__cdecl*)(void*, void*, void*, void*, void*, void*, void*, unsigned int, unsigned int,
                                     unsigned int, unsigned int, int, int, float, int, float, float, float, int, float,
                                     float);
using PFN_VkRelease = void(__cdecl*)(void*);

// One image this pass owns: the storage, the view, and the NGX wrapper that describes it. Kept
// together because they are created, resized and destroyed as one thing.
struct OwnedImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    NVSDK_NGX_Resource_VK ngx {};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    bool Valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
};

struct VkState
{
    bool failed = false;
    const char* reason = "";

    HMODULE forwarder = nullptr;
    PFN_VkProbe probe = nullptr;
    PFN_VkInit init = nullptr;
    PFN_VkCreate create = nullptr;
    PFN_VkEvaluate evaluate = nullptr;
    PFN_VkRelease release = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    bool ngxInitialised = false;
    void* feature = nullptr;
    NVSDK_NGX_Parameter* capabilityParams = nullptr;

    // What the model writes, the proxy it is shown, and the frame as the upscaler left it.
    OwnedImage output;
    OwnedImage proxy;
    OwnedImage keep;

    // The proxy at the model's working size, when that is below the frame. The model -- 98% of the
    // cost -- then runs on this instead of the full proxy, which is the whole point of the working
    // scale slider. Unused (and never created) at scale 1, so the default path is unchanged.
    OwnedImage proxySmall;

    // Supersampling (working scale > 1): the model runs above native, superUp enlarges the proxy to
    // that size and superDown averages the answer (output) back into outputNative at native for a 1:1
    // composite. nrScaler is the filter both were built with, so a changed DlssNrScalingDownscaler
    // rebuilds them. Unused and never created at scale <= 1.
    OwnedImage outputNative;
    std::unique_ptr<OS_Vk> superUp;
    std::unique_ptr<OS_Vk> superDown;
    Scaler nrScaler = Scaler::Count;

    std::unique_ptr<DlssNr_Vk> pass;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    bool reset = true;
    unsigned long long frames = 0;

    // Timing. A pair of timestamps per frame across a ring, read back three frames later: a query
    // read the frame it was written stalls the CPU on the GPU, which would cost more than the pass
    // it is measuring. Vulkan reports ticks, and timestampPeriod is how many nanoseconds a tick is.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    float timestampPeriod = 0.0f;
    unsigned long long timedFrames = 0;
    std::optional<double> lastGpuTime;

    // Whether the game hands over an exposure texture, and what it said when it did.
    bool exposureOffered = false;

    // The game's own exposure, read off its 1x1 texture, and the scale it multiplied its buffer by.
    //
    // gameExposure holds its last good value rather than resetting when a frame arrives without a
    // texture: GTA V dropped it three times in one session on the D3D12 path, and falling back to a
    // default on those frames is a flicker, not a fallback.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // The exposure's courier: an 8x8 R32_FLOAT image the meter writes, and a ring of host-visible
    // buffers it is copied into. Only texel (0,0) is ever read -- the rest of the grid belongs to the
    // frame-statistics meter that was removed from the shared shader, and 8x8 is here only so that a
    // single 8x8 thread group lands entirely inside the image.
    OwnedImage meter;
    VkBuffer meterReadback[4] = {};
    VkDeviceMemory meterReadbackMemory[4] = {};
    void* meterMapped[4] = {};
    unsigned long long meterFrames = 0;
};

// The grid the meter writes, and the size of one readback. 8 * 8 * sizeof(float).
constexpr uint32_t kMeterSide = 8;
constexpr VkDeviceSize kMeterBytes = kMeterSide * kMeterSide * sizeof(float);

// Four, so the slot being read is four frames behind the slot being written and the read never waits
// on the GPU. Same depth as the D3D12 meter's ring, for the same reason.
constexpr unsigned long long kMeterSlots = 4;

// Four frames of pairs. Three would do, four keeps the modulo cheap and the slot being written well
// clear of the slot being read.
constexpr uint32_t kTimingSlots = 4;

VkState g_vk;
std::mutex g_vkMutex;

void Fail(const char* why)
{
    if (g_vk.failed)
        return;

    g_vk.failed = true;
    g_vk.reason = why;
    LOG_ERROR("DLSS-NR Vulkan unavailable: {}", why);
}

// ---------------------------------------------------------------------------------------------
// Images this pass owns
// ---------------------------------------------------------------------------------------------

void DestroyImage(OwnedImage& img)
{
    if (g_vk.device == VK_NULL_HANDLE)
        return;

    if (img.view != VK_NULL_HANDLE)
        vkDestroyImageView(g_vk.device, img.view, nullptr);

    if (img.image != VK_NULL_HANDLE)
        vkDestroyImage(g_vk.device, img.image, nullptr);

    if (img.memory != VK_NULL_HANDLE)
        vkFreeMemory(g_vk.device, img.memory, nullptr);

    img = OwnedImage {};
}

uint32_t FindMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps {};
    vkGetPhysicalDeviceMemoryProperties(g_vk.physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    return UINT32_MAX;
}

// STORAGE and SAMPLED both, because every one of these is written by one dispatch and read by the
// next; TRANSFER_SRC so a capture can copy it out without a second surface.
// Build the OS_Vk resample descriptor for one of our own images. OS_Vk reads Width/Height/Format from
// this (the NR override makes it size from the images, not the current feature).
static VkImageInfo ImageInfoOf(const OwnedImage& img)
{
    VkImageInfo info {};
    info.ImageView = img.view;
    info.Image = img.image;
    info.SubresourceRange = VkImageSubresourceRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    info.Format = img.format;
    info.Width = img.width;
    info.Height = img.height;
    return info;
}

bool CreateImage(OwnedImage& img, uint32_t width, uint32_t height, VkFormat format, bool readWrite)
{
    DestroyImage(img);

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(g_vk.device, &info, nullptr, &img.image) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not create a {}x{} image", width, height);
        return false;
    }

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(g_vk.device, img.image, &req);

    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(g_vk.device, &alloc, nullptr, &img.memory) != VK_SUCCESS ||
        vkBindImageMemory(g_vk.device, img.image, img.memory, 0) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not back a {}x{} image", width, height);
        DestroyImage(img);
        return false;
    }

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(g_vk.device, &view, nullptr, &img.view) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not view a {}x{} image", width, height);
        DestroyImage(img);
        return false;
    }

    img.width = width;
    img.height = height;
    img.format = format;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The NGX wrapper. Filled once, because none of it changes until the image is recreated.
    img.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    img.ngx.Resource.ImageViewInfo.ImageView = img.view;
    img.ngx.Resource.ImageViewInfo.Image = img.image;
    img.ngx.Resource.ImageViewInfo.SubresourceRange = view.subresourceRange;
    img.ngx.Resource.ImageViewInfo.Format = format;
    img.ngx.Resource.ImageViewInfo.Width = width;
    img.ngx.Resource.ImageViewInfo.Height = height;
    img.ngx.ReadWrite = readWrite;

    return true;
}

// The ring of host-visible buffers the meter's grid is copied into, created once and mapped for
// good. HOST_COHERENT so the read needs no invalidate; it is universally available for a buffer this
// small and the alternative is a vkInvalidateMappedMemoryRanges on a path that runs every frame.
bool CreateMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (g_vk.meterReadback[i] != VK_NULL_HANDLE)
            continue;

        VkBufferCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = kMeterBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(g_vk.device, &info, nullptr, &g_vk.meterReadback[i]) != VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not create the exposure readback buffer");
            return false;
        }

        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(g_vk.device, g_vk.meterReadback[i], &req);

        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryTypeIndex(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(g_vk.device, &alloc, nullptr, &g_vk.meterReadbackMemory[i]) != VK_SUCCESS ||
            vkBindBufferMemory(g_vk.device, g_vk.meterReadback[i], g_vk.meterReadbackMemory[i], 0) != VK_SUCCESS ||
            vkMapMemory(g_vk.device, g_vk.meterReadbackMemory[i], 0, kMeterBytes, 0, &g_vk.meterMapped[i]) !=
                VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not back the exposure readback buffer");
            return false;
        }
    }

    return true;
}

void DestroyMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (g_vk.meterReadbackMemory[i] != VK_NULL_HANDLE)
        {
            if (g_vk.meterMapped[i] != nullptr)
                vkUnmapMemory(g_vk.device, g_vk.meterReadbackMemory[i]);

            vkFreeMemory(g_vk.device, g_vk.meterReadbackMemory[i], nullptr);
        }

        if (g_vk.meterReadback[i] != VK_NULL_HANDLE)
            vkDestroyBuffer(g_vk.device, g_vk.meterReadback[i], nullptr);

        g_vk.meterMapped[i] = nullptr;
        g_vk.meterReadbackMemory[i] = VK_NULL_HANDLE;
        g_vk.meterReadback[i] = VK_NULL_HANDLE;
    }

    g_vk.meterFrames = 0;
}

// A layout transition with the access masks that go with it. Vulkan has no equivalent of D3D12's
// state promotion, so every read and every write says which layout it needs and this is how it gets
// there. Tracked per image so a no-op transition is not recorded.
void Transition(VkCommandBuffer cmd, OwnedImage& img, VkImageLayout to)
{
    if (img.image == VK_NULL_HANDLE || img.layout == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = img.layout;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    img.layout = to;
}

// A resource the game owns. Its layout is the game's business, so this records the transition and
// puts it back exactly as it was rather than tracking it.
void TransitionForeign(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange range, VkImageLayout from,
                       VkImageLayout to)
{
    if (image == VK_NULL_HANDLE || from == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------------------------

bool LoadForwarder()
{
    if (g_vk.forwarder != nullptr)
        return g_vk.create != nullptr;

    auto path = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!path.has_value())
        path = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!path.has_value())
    {
        Fail("nvngx.dll_dlssnr.dll was not found beside OptiScaler or the game");
        return false;
    }

    g_vk.forwarder = LoadLibraryW(path->wstring().c_str());

    if (g_vk.forwarder == nullptr)
    {
        Fail("the forwarder would not load");
        return false;
    }

    g_vk.probe = (PFN_VkProbe) GetProcAddress(g_vk.forwarder, "dlssnr_vk_probe");
    g_vk.init = (PFN_VkInit) GetProcAddress(g_vk.forwarder, "dlssnr_vk_init");
    g_vk.create = (PFN_VkCreate) GetProcAddress(g_vk.forwarder, "dlssnr_vk_create");
    g_vk.evaluate = (PFN_VkEvaluate) GetProcAddress(g_vk.forwarder, "dlssnr_vk_evaluate");
    g_vk.release = (PFN_VkRelease) GetProcAddress(g_vk.forwarder, "dlssnr_vk_release");

    if (g_vk.init == nullptr || g_vk.create == nullptr || g_vk.evaluate == nullptr)
    {
        Fail("the forwarder is missing its Vulkan entry points");
        return false;
    }

    return true;
}

// Whether a format can hold linear, open-ended light. A frame the game already tone mapped has white
// at 1 and must not be encoded a second time; an 8-bit or normalised format cannot be scene-referred
// whatever the game says. The D3D12 path asks the same question of DXGI formats.
bool FormatCanHoldLinearHdr(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return true;
    default:
        return false;
    }
}

// The create flags the game gave its own upscaler, which is where HDR and inverted depth are stated.
// Read from the parameter block rather than configured, because they describe the game's buffers and
// getting either wrong is silent: an encoded frame encoded twice, or depth read backwards.
unsigned int GameCreateFlags(NVSDK_NGX_Parameter* params)
{
    unsigned int flags = 0;

    if (params != nullptr)
        params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);

    return flags;
}

std::optional<std::filesystem::path> FindSnippet()
{
    auto snippet = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    return snippet;
}

} // namespace

// ---------------------------------------------------------------------------------------------

bool IsRunningVk() { return g_vk.feature != nullptr && !g_vk.failed; }

const char* FailureReasonVk() { return g_vk.failed ? g_vk.reason : ""; }

unsigned long long FramesVk() { return g_vk.frames; }

bool ExposureOfferedVk() { return g_vk.exposureOffered; }

std::optional<double> LastGpuTimeVk() { return g_vk.lastGpuTime; }

void EvaluateAfterUpscaleVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params, VkInstance instance,
                            VkPhysicalDevice physicalDevice, VkDevice device)
{
    auto& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
        return;

    if (cmdBuffer == VK_NULL_HANDLE || params == nullptr || device == VK_NULL_HANDLE ||
        physicalDevice == VK_NULL_HANDLE)
        return;

    std::lock_guard<std::mutex> lock(g_vkMutex);

    if (g_vk.failed)
        return;

    // The game's own resources, already wrapped: NGX hands Vulkan resources over as
    // NVSDK_NGX_Resource_VK, so only this pass's own images need building.
    NVSDK_NGX_Resource_VK* colour = nullptr;
    NVSDK_NGX_Resource_VK* depth = nullptr;
    NVSDK_NGX_Resource_VK* motion = nullptr;

    params->Get(NVSDK_NGX_Parameter_Output, (void**) &colour);
    params->Get(NVSDK_NGX_Parameter_Depth, (void**) &depth);
    params->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &motion);

    // The game's exposure, now read rather than only counted.
    //
    // What blocked this was the layout: a descriptor names the layout its image will be in when the
    // shader runs, NVIDIA's Vulkan header does not use the word "layout" once, and a barrier is no
    // safer because it needs the layout it is coming from. Three things in this tree answer it, and
    // they agree. FSR2Feature_Vk hands this same texture to FidelityFX as COMPUTE_READ, which its
    // Vulkan backend maps to SHADER_READ_ONLY_OPTIMAL, on a path that works in these games. The
    // D3D12-on-Vulkan bridge transitions the game's exposure image out of SHADER_READ_ONLY_OPTIMAL,
    // on a path that works. And the header stating nothing means there is no contract to break --
    // the convention is the contract.
    //
    // So it is bound in SHADER_READ_ONLY_OPTIMAL and no barrier is recorded: this never transitions a
    // resource it does not own. If a game turns out to leave it somewhere else the cost is a wrong
    // number, not a lost device, and the gate on the readback throws a wrong number away.
    NVSDK_NGX_Resource_VK* exposure = nullptr;
    float preExposure = 1.0f;
    const bool havePre =
        params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

    params->Get(NVSDK_NGX_Parameter_ExposureTexture, (void**) &exposure);

    static bool saidExposure = false;

    if (!saidExposure)
    {
        saidExposure = true;
        LOG_INFO("DLSS-NR Vulkan: exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}",
                 havePre ? std::to_string(preExposure) : std::string("not supplied"),
                 exposure != nullptr ? "supplied" : "not supplied");
    }

    g_vk.exposureOffered = exposure != nullptr;

    if (havePre && std::isfinite(preExposure) && preExposure > 0.0f)
        g_vk.gamePreExposure = preExposure;

    // Take the grid written four frames ago. Retired by now, so this reads mapped memory rather than
    // waiting on the GPU -- which is the whole reason for the ring.
    if (g_vk.meterFrames >= kMeterSlots)
    {
        const void* mapped = g_vk.meterMapped[g_vk.meterFrames % kMeterSlots];

        if (mapped != nullptr)
        {
            float measured = 0.0f;
            std::memcpy(&measured, mapped, sizeof(float));

            // Believed only if it could be an exposure. A texel read through a layout the game did
            // not leave it in, or a slot the game stopped filling, fails here and the last good
            // value stands.
            if (std::isfinite(measured) && measured > 0.0f)
                g_vk.gameExposure = measured;
        }
    }

    // Said when it moves by more than a fiftieth, not every frame. Enough to see in a log that the
    // number is the game's and that it tracks the scene, without a line per frame.
    static float loggedExposure = -1.0f;

    if (g_vk.gameExposure > 1e-6f &&
        std::abs(loggedExposure - g_vk.gameExposure) > std::max(0.02f * g_vk.gameExposure, 1e-5f))
    {
        loggedExposure = g_vk.gameExposure;
        LOG_INFO("DLSS-NR Vulkan: the game's exposure is {}, pre-exposure {}, so white point {}",
                 g_vk.gameExposure, g_vk.gamePreExposure, g_vk.gamePreExposure / g_vk.gameExposure);
    }

    if (colour == nullptr || depth == nullptr || motion == nullptr)
    {
        static bool said = false;

        if (!said)
        {
            said = true;
            LOG_INFO("DLSS-NR Vulkan: the parameter block carried no {}",
                     colour == nullptr ? "output" : (depth == nullptr ? "depth" : "motion vectors"));
        }

        return;
    }

    const uint32_t width = colour->Resource.ImageViewInfo.Width;
    const uint32_t height = colour->Resource.ImageViewInfo.Height;
    const uint32_t guideWidth = depth->Resource.ImageViewInfo.Width;
    const uint32_t guideHeight = depth->Resource.ImageViewInfo.Height;

    if (width == 0 || height == 0)
        return;

    // The model's working size. The slider is a fraction of the frame; at 1 it is the frame, and the
    // reduced path below never runs, so the default is byte-for-byte what it was.
    // Above 1 the model supersamples (up to 2x): the proxy is enlarged, the model runs above native,
    // and superDown averages the answer back. Vulkan matches the D3D12 cap.
    const float workScale = std::clamp(cfg.DlssNrWorkingScale.value_or_default(), 0.25f, 2.0f);
    const uint32_t workWidth = (uint32_t) (width * workScale + 0.5f);
    const uint32_t workHeight = (uint32_t) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    g_vk.instance = instance;
    g_vk.physicalDevice = physicalDevice;

    // A device change invalidates everything. The OLD device is presumed dead here -- the game
    // destroyed it, which already freed every resource made on it -- so abandon those handles rather
    // than call vkDestroy*/wait-idle on a dead device (that would be use-after-free). Rebuild fresh.
    if (g_vk.device != device)
    {
        ShutdownVk(false);
        g_vk.device = device;
        g_vk.instance = instance;
        g_vk.physicalDevice = physicalDevice;
    }

    if (!LoadForwarder())
        return;

    // Initialise NGX on this device, once. The snippet path is the model itself; the forwarder loads
    // it so the caller gate sees a module named nvngx.dll.
    if (!g_vk.ngxInitialised)
    {
        auto snippet = FindSnippet();

        if (!snippet.has_value())
        {
            Fail("nvngx_dlssnr.dll was not found beside OptiScaler or the game");
            return;
        }

        const int probe = g_vk.probe != nullptr ? g_vk.probe(snippet->wstring().c_str()) : 0;

        // Four bits, one per entry point. Anything short of fifteen means the model's Vulkan surface
        // is not entirely reachable and there is no point going further.
        if (probe != 15)
        {
            LOG_ERROR("DLSS-NR Vulkan: the model's Vulkan surface is incomplete (probe {})", probe);
            Fail("the model does not expose a complete Vulkan surface");
            return;
        }

        const int result =
            g_vk.init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                      (void*) instance, (void*) physicalDevice, (void*) device, 0x0000015);

        if (result != 1)
        {
            LOG_ERROR("DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned {}", result);
            Fail("the model would not initialise on this Vulkan device");
            return;
        }

        g_vk.ngxInitialised = true;
        LOG_INFO("DLSS-NR Vulkan: the model initialised on this device");
    }

    if (g_vk.capabilityParams == nullptr)
    {
        if (NVSDK_NGX_VULKAN_AllocateParameters(&g_vk.capabilityParams) != NVSDK_NGX_Result_Success ||
            g_vk.capabilityParams == nullptr)
        {
            Fail("a parameter block could not be allocated");
            return;
        }
    }

    if (g_vk.queryPool == VK_NULL_HANDLE)
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        // A period of zero means the device does not support timestamps on this queue. The pass runs
        // regardless; it simply reports no cost, which is what the D3D12 path does when its heap is
        // unavailable.
        g_vk.timestampPeriod = props.limits.timestampPeriod;

        if (g_vk.timestampPeriod > 0.0f)
        {
            VkQueryPoolCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kTimingSlots * 2;

            if (vkCreateQueryPool(device, &info, nullptr, &g_vk.queryPool) != VK_SUCCESS)
            {
                g_vk.queryPool = VK_NULL_HANDLE;
                LOG_INFO("DLSS-NR Vulkan: no timestamp pool, the pass will not report its cost");
            }
        }
    }

    if (g_vk.pass == nullptr)
    {
        g_vk.pass = std::make_unique<DlssNr_Vk>("Neural Rendering", device, physicalDevice);

        if (!g_vk.pass->IsInit())
        {
            g_vk.pass.reset();
            Fail("the composition pass could not be created");
            return;
        }
    }

    // Resize. The feature is built for a size and has to be rebuilt when the frame OR the working
    // size changes -- moving the slider is a rebuild, which is why it is compared here.
    if (g_vk.width != width || g_vk.height != height || g_vk.workWidth != workWidth ||
        g_vk.workHeight != workHeight)
    {
        // This block releases the feature and frees the surfaces below IMMEDIATELY. A frame-size
        // change is already fenced by the game -- it recreates the swapchain around it -- but moving
        // the working-scale slider is not: the game is mid-flight and previous frames' command
        // buffers still reference the feature and images about to be destroyed. Freeing a Vulkan
        // resource that in-flight GPU work still touches is device removal (ERR_GFX_STATE, reproduced
        // on RDR2 and Enshrouded by dragging the model-resolution slider). Drain the device first.
        // Only the rare resize path reaches here, so the CPU stall is a one-off hitch, not per-frame.
        if (g_vk.device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(g_vk.device);

        if (g_vk.feature != nullptr && g_vk.release != nullptr)
        {
            g_vk.release(g_vk.feature);
            g_vk.feature = nullptr;
        }

        const VkFormat working = VK_FORMAT_R16G16B16A16_SFLOAT;

        // The meter is a fixed 8x8 whatever the frame is, so it is only built the once -- but it is
        // built alongside the rest so that a failure here is caught by the same check.
        const bool meterReady = (g_vk.meter.Valid() || CreateImage(g_vk.meter, kMeterSide, kMeterSide,
                                                                   VK_FORMAT_R32_SFLOAT, true)) &&
                                CreateMeterReadback();

        if (!meterReady)
            LOG_WARN("DLSS-NR Vulkan: no exposure meter; the white point stays on the slider");

        DestroyImage(g_vk.proxySmall);
        DestroyImage(g_vk.outputNative);

        // output is the model's target, so it is the working size. proxy and keep are full: proxy is
        // the source the downsample reads, keep is the untouched frame the resolve composites onto.
        // outputNative is the native buffer the supersample down-leg averages the answer into.
        const bool ok = CreateImage(g_vk.output, workWidth, workHeight, working, true) &&
                        CreateImage(g_vk.proxy, width, height, working, true) &&
                        CreateImage(g_vk.keep, width, height, working, true) &&
                        (!reduced || CreateImage(g_vk.proxySmall, workWidth, workHeight, working, true)) &&
                        (workScale <= 1.0f || CreateImage(g_vk.outputNative, width, height, working, true));

        if (!ok)
        {
            Fail("the pass could not allocate its own surfaces");
            return;
        }

        g_vk.width = width;
        g_vk.height = height;
        g_vk.workWidth = workWidth;
        g_vk.workHeight = workHeight;
        g_vk.reset = true;
    }

    if (g_vk.feature == nullptr)
    {
        g_vk.feature = g_vk.create(
            (void*) cmdBuffer, g_vk.capabilityParams, workWidth, workHeight, (int) cfg.DlssNrPreset.value_or_default(),
            cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
            cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
            cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

        if (g_vk.feature == nullptr)
        {
            Fail("the model would not build a feature on this device");
            return;
        }

        LOG_INFO("DLSS-NR Vulkan: feature up at {}x{} (frame {}x{})", workWidth, workHeight, width, height);
        g_vk.reset = true;
    }

    // -----------------------------------------------------------------------------------------
    // Encode: the frame the upscaler wrote -> a display-referred proxy, plus an untouched copy
    // -----------------------------------------------------------------------------------------

    const unsigned int createFlags = GameCreateFlags(params);
    const bool gameSaysHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    const bool depthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;

    // The game asking the upscaler to forget its history -- a cut, a teleport, a load. Same omission
    // as the D3D12 path had: the model's history was only ever reset by things that happened to us,
    // never by anything that happened in the game.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success &&
            gameReset != 0)
        {
            g_vk.reset = true;

            static unsigned long long resets = 0;
            ++resets;

            if (resets <= 3 || resets % 100 == 0)
                LOG_INFO("DLSS-NR Vulkan: the game asked for a history reset ({} so far)", resets);
        }
    }

    // Both have to agree. A game can set the HDR flag on a buffer that cannot hold open-ended light,
    // and encoding an already tone-mapped frame a second time looks washed out and banded.
    const bool linearHdr = gameSaysHdr && FormatCanHoldLinearHdr(colour->Resource.ImageViewInfo.Format);

    // The same rule as the D3D12 path, deliberately spelled the same way: the game divides its frame
    // by preExposure and multiplies by exposure, so undoing that is the divisor this pass wants, and
    // the slider becomes a trim on top rather than the answer.
    //
    // The trim is bounded here, at the point of use, rather than at the slider. Someone who found 64
    // by hand on the manual path and then switches the exposure source on keeps that 64 in their ini;
    // bounding it in the menu would leave the picture wrong for a reason the menu no longer showed.
    // Their value stays in the config untouched, so switching back to manual restores it.
    float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && g_vk.gameExposure > 1e-6f)
    {
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
        whitePoint = std::clamp(g_vk.gamePreExposure / g_vk.gameExposure * trim, 0.01f, 4096.0f);
    }

    static bool saidEncoding = false;

    if (!saidEncoding)
    {
        saidEncoding = true;
        LOG_INFO("DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}",
                 linearHdr ? "linear HDR" : "already tone-mapped", gameSaysHdr ? "set" : "clear",
                 (int) colour->Resource.ImageViewInfo.Format, depthInverted ? "inverted" : "normal");
    }

    DlssNrConstants encode {};
    encode.Mode = DlssNrMode_Encode;
    encode.Width = width;
    encode.Height = height;
    encode.WhitePoint = whitePoint;
    encode.Passthrough = linearHdr ? 0u : 1u;
    encode.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    encode.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
    encode.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
    encode.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
    encode.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
    encode.Transfer = cfg.DlssNrTransfer.value_or_default();
    encode.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
    encode.GuideWidth = guideWidth;
    encode.GuideHeight = guideHeight;

    const VkImageSubresourceRange colourRange = colour->Resource.ImageViewInfo.SubresourceRange;

    // Open the measurement. Reset immediately before writing: a query pool slot must be reset before
    // it is written again, and doing it here rather than at the end keeps the two in one place.
    const uint32_t timingSlot = (uint32_t) (g_vk.timedFrames % kTimingSlots);

    if (g_vk.queryPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cmdBuffer, g_vk.queryPool, timingSlot * 2, 2);
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_vk.queryPool, timingSlot * 2);
    }

    // The game's colour is read here and written at the end. Its layout on arrival is GENERAL, which
    // is what NGX requires of a resource it is handed, so it is left alone.
    Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmdBuffer, g_vk.keep, VK_IMAGE_LAYOUT_GENERAL);

    // Read in GENERAL, which is the layout it is actually in.
    //
    // This slot used to take the default and declare SHADER_READ_ONLY_OPTIMAL, which disagreed with
    // the comment four lines up and with the resolve below -- the resolve writes this same image as a
    // storage image, which is only legal in GENERAL, and nothing transitions it in between. It is the
    // upscaler's output, a storage image the upscaler has just written, so GENERAL is what it is.
    // Inert on the only hardware this model runs on, wrong everywhere it is read.
    if (!g_vk.pass->Dispatch(cmdBuffer, encode, width, height, colour->Resource.ImageViewInfo.ImageView,
                             VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, g_vk.proxy.view, g_vk.keep.view,
                             VK_IMAGE_LAYOUT_GENERAL))
    {
        Fail("the encode dispatch failed");
        return;
    }

    // The model's input: the full proxy, or a downsampled copy of it when the working scale is below
    // the frame. Mirrors the D3D12 path -- the encode always writes a full proxy, and a separate
    // downsample makes the small one the model actually reads.
    OwnedImage* modelInput = &g_vk.proxy;

    if (reduced && g_vk.proxySmall.Valid())
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: upscale the proxy to the super-native working size with the chosen filter so
            // the model sees a clean input. Rebuild both scalers when the NR downscaler changed (baked
            // at construction). proxy -> SHADER_READ_ONLY (sampled), proxySmall -> GENERAL (storage).
            const Scaler wantScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_vk.nrScaler != wantScaler)
            {
                // Rebuilding frees the old scalers' pipelines/descriptors. The filter dropdown changes
                // no size, so this does NOT go through the resize block's drain -- and prior frames'
                // submitted command buffers still bind these pipelines. Freeing them under in-flight GPU
                // work is device removal (the same hazard the resize path drains for). Drain first. A
                // filter change is rare, so the one-off stall is a hitch, not a per-frame cost.
                if (g_vk.device != VK_NULL_HANDLE)
                    vkDeviceWaitIdle(g_vk.device);
                g_vk.superUp.reset();
                g_vk.superDown.reset();
                g_vk.nrScaler = wantScaler;
            }
            if (!g_vk.superUp)
                g_vk.superUp = std::make_unique<OS_Vk>("DLSS-NR VK supersample up", device, physicalDevice,
                                                       true, wantScaler);
            if (!g_vk.superDown)
                g_vk.superDown = std::make_unique<OS_Vk>("DLSS-NR VK supersample down", device,
                                                         physicalDevice, false, wantScaler);

            Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            VkImageInfo upin = ImageInfoOf(g_vk.proxy);
            VkImageInfo upout = ImageInfoOf(g_vk.proxySmall);

            if (g_vk.superUp && g_vk.superUp->IsInit() && g_vk.superUp->Dispatch(cmdBuffer, upin, upout))
                built = true;
            else
            {
                static bool warnedVkSuper = false;
                if (!warnedVkSuper)
                {
                    warnedVkSuper = true;
                    LOG_WARN("DLSS-NR Vulkan supersample: upscaler unavailable, falling back to box enlarge.");
                }
            }
        }

        if (!built)
        {
            DlssNrConstants down = encode;
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;

            Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            if (!g_vk.pass->Dispatch(cmdBuffer, down, workWidth, workHeight, g_vk.proxy.view, VK_NULL_HANDLE,
                                     VK_NULL_HANDLE, VK_NULL_HANDLE, g_vk.proxySmall.view, VK_NULL_HANDLE,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                Fail("the downsample dispatch failed");
                return;
            }
        }

        modelInput = &g_vk.proxySmall;
    }

    // -----------------------------------------------------------------------------------------
    // The meter: the game's 1x1 exposure -> texel (0,0) of the grid -> a buffer the CPU can read
    // -----------------------------------------------------------------------------------------

    // The motion slot carries it, because the meter has no use for motion vectors and the shader is
    // one shader with a fixed set of bindings. The source slot is left empty and gets the dummy.
    //
    // Gated on the setting that consumes the answer, which is not merely tidy. This is the only place
    // the pass binds a resource it does not own on a guess about its layout, and the guess is good
    // but it is still a guess. A user who has not asked for the exposure source never has the game's
    // image touched at all, so if some engine does leave it somewhere unexpected, the blast radius is
    // people who turned the thing on rather than everyone on Vulkan.
    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && exposure != nullptr &&
        exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
        exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE && g_vk.meter.Valid())
    {
        const unsigned long long slot = g_vk.meterFrames % kMeterSlots;

        if (g_vk.meterReadback[slot] != VK_NULL_HANDLE)
        {
            DlssNrConstants meter = encode;
            meter.Mode = DlssNrMode_Meter;
            meter.Width = kMeterSide;
            meter.Height = kMeterSide;

            Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_GENERAL);

            if (g_vk.pass->Dispatch(cmdBuffer, meter, kMeterSide, kMeterSide, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, exposure->Resource.ImageViewInfo.ImageView, g_vk.meter.view,
                                    VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                VkBufferImageCopy region {};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageOffset = { 0, 0, 0 };
                region.imageExtent = { kMeterSide, kMeterSide, 1 };

                vkCmdCopyImageToBuffer(cmdBuffer, g_vk.meter.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       g_vk.meterReadback[slot], 1, &region);

                // The copy has to be visible to a host read, and only the host will read it.
                VkBufferMemoryBarrier toHost {};
                toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toHost.buffer = g_vk.meterReadback[slot];
                toHost.offset = 0;
                toHost.size = kMeterBytes;

                vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                     nullptr, 1, &toHost, 0, nullptr);

                g_vk.meterFrames++;
            }
        }
    }

    // -----------------------------------------------------------------------------------------
    // The model
    // -----------------------------------------------------------------------------------------

    Transition(cmdBuffer, g_vk.output, VK_IMAGE_LAYOUT_GENERAL);

    const int evaluated = g_vk.evaluate(
        (void*) cmdBuffer, g_vk.feature, g_vk.capabilityParams, &modelInput->ngx, depth, motion, &g_vk.output.ngx,
        workWidth, workHeight, guideWidth, guideHeight, depthInverted ? 1 : 0, g_vk.reset ? 1 : 0,
        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1.0f, 1.0f);

    g_vk.reset = false;
    g_vk.frames++;

    if (evaluated != 1)
    {
        LOG_ERROR("DLSS-NR Vulkan: evaluate returned {}", evaluated);
        Fail("the model refused to evaluate");
        return;
    }

    // -----------------------------------------------------------------------------------------
    // Resolve: proxy + the model's answer + the untouched copy -> the frame
    // -----------------------------------------------------------------------------------------

    DlssNrConstants resolve = encode;
    resolve.Mode = DlssNrMode_Resolve;

    // Supersampling down-leg (Vulkan). Average the Nx model answer back to native with the chosen
    // filter so the resolve composites a native answer against the native proxy 1:1 -- not the single
    // bilinear tap the Nx answer would otherwise get, which aliases the model's detail into noise. On
    // failure it falls back to the Nx pair (modelInput + output), the old behaviour.
    OwnedImage* resolveProxy = modelInput;
    OwnedImage* resolveAnswer = &g_vk.output;

    if (workScale > 1.0f && g_vk.superDown && g_vk.superDown->IsInit() && g_vk.outputNative.Valid())
    {
        Transition(cmdBuffer, g_vk.output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, g_vk.outputNative, VK_IMAGE_LAYOUT_GENERAL);

        VkImageInfo dsin = ImageInfoOf(g_vk.output);
        VkImageInfo dsout = ImageInfoOf(g_vk.outputNative);

        if (g_vk.superDown->Dispatch(cmdBuffer, dsin, dsout))
        {
            resolveProxy = &g_vk.proxy;
            resolveAnswer = &g_vk.outputNative;
        }
    }

    Transition(cmdBuffer, *resolveProxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, *resolveAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, g_vk.keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (!g_vk.pass->Dispatch(cmdBuffer, resolve, width, height, resolveProxy->view, resolveAnswer->view,
                             g_vk.keep.view, VK_NULL_HANDLE, colour->Resource.ImageViewInfo.ImageView,
                             VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
    {
        Fail("the resolve dispatch failed");
        return;
    }

    // Close it, and read the pair from three frames ago -- retired by now, so the read does not wait.
    if (g_vk.queryPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vk.queryPool, timingSlot * 2 + 1);
        g_vk.timedFrames++;

        if (g_vk.timedFrames > kTimingSlots)
        {
            const uint32_t readSlot = (uint32_t) (g_vk.timedFrames % kTimingSlots);
            uint64_t ticks[2] = {};

            // Without WAIT: a slot this old is retired, and if it somehow is not, NOT_READY is the
            // right answer rather than a stall.
            if (vkGetQueryPoolResults(device, g_vk.queryPool, readSlot * 2, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ticks[1] > ticks[0])
            {
                const double ms = (double) (ticks[1] - ticks[0]) * (double) g_vk.timestampPeriod / 1e6;

                // A pass that appears to have taken over a second did not; the queue was reset under
                // it or the pair straddled a device change.
                if (ms > 0.0 && ms < 1000.0)
                    g_vk.lastGpuTime = ms;
            }
        }
    }

    static bool reported = false;

    if (!reported && g_vk.frames > 2)
    {
        reported = true;
        LOG_INFO("DLSS-NR Vulkan: running natively at {}x{}, guides {}x{}", width, height, guideWidth, guideHeight);
    }
}

void ShutdownVk(bool deviceAlive)
{
    if (!deviceAlive)
    {
        // The device these handles belong to is gone (a device change was detected). Destroying a
        // VkDevice already frees every resource created on it, so touch NOTHING on the old device --
        // no wait-idle, no vkDestroy*, no NGX release, and crucially no DlssNr_Vk destructor (it would
        // vkDestroy its pipelines on the dead device). Abandon the handles; the driver reclaimed them
        // when the device died. The one-off CPU-side leak of the pass object is the rare cost of a
        // device recreation, and far cheaper than the use-after-free it replaces. Zeroing the
        // OwnedImage/meter handles matters: the resize path gates on `.Valid()`, so a stale non-null
        // handle from the dead device would be reused on the NEW device and crash.
        g_vk.pass.release();
        g_vk.superUp.release();
        g_vk.superDown.release();
        g_vk.nrScaler = Scaler::Count;
        g_vk.feature = nullptr;
        g_vk.capabilityParams = nullptr;
        g_vk.queryPool = VK_NULL_HANDLE;
        g_vk.output = OwnedImage {};
        g_vk.proxy = OwnedImage {};
        g_vk.proxySmall = OwnedImage {};
        g_vk.outputNative = OwnedImage {};
        g_vk.keep = OwnedImage {};
        g_vk.meter = OwnedImage {};

        for (int i = 0; i < 4; ++i)
        {
            g_vk.meterReadback[i] = VK_NULL_HANDLE;
            g_vk.meterReadbackMemory[i] = VK_NULL_HANDLE;
            g_vk.meterMapped[i] = nullptr;
        }

        g_vk.device = VK_NULL_HANDLE;
        g_vk.width = 0;
        g_vk.height = 0;
        g_vk.workWidth = 0;
        g_vk.workHeight = 0;
        g_vk.timedFrames = 0;
        g_vk.meterFrames = 0;
        g_vk.lastGpuTime.reset();
        g_vk.ngxInitialised = false;
        g_vk.reset = true;
        return;
    }

    // The device is alive (real teardown): drain before freeing so nothing the GPU is still using is
    // destroyed under it, the same rule as the resize path.
    if (g_vk.device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(g_vk.device);

    if (g_vk.feature != nullptr && g_vk.release != nullptr)
        g_vk.release(g_vk.feature);

    g_vk.feature = nullptr;

    DestroyImage(g_vk.output);
    DestroyImage(g_vk.proxy);
    DestroyImage(g_vk.proxySmall);
    DestroyImage(g_vk.outputNative);
    DestroyImage(g_vk.keep);
    DestroyImage(g_vk.meter);
    DestroyMeterReadback();

    g_vk.pass.reset();
    g_vk.superUp.reset();
    g_vk.superDown.reset();
    g_vk.nrScaler = Scaler::Count;

    if (g_vk.capabilityParams != nullptr)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(g_vk.capabilityParams);
        g_vk.capabilityParams = nullptr;
    }

    if (g_vk.queryPool != VK_NULL_HANDLE && g_vk.device != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(g_vk.device, g_vk.queryPool, nullptr);
        g_vk.queryPool = VK_NULL_HANDLE;
    }

    g_vk.timedFrames = 0;
    g_vk.lastGpuTime.reset();

    g_vk.device = VK_NULL_HANDLE;
    g_vk.width = 0;
    g_vk.height = 0;
    g_vk.ngxInitialised = false;
    g_vk.reset = true;
}

} // namespace DlssNr
