#pragma once

#include "SysUtils.h"

#include <shaders/Shader_Vk.h>

// Forward declaration (scoped enum with a fixed underlying type -> complete type) so this header need
// not include Config.h.
enum class Scaler : uint32_t;

class OS_Vk : public Shader_Vk
{
    bool _upsample = false;

    // Scaler::Count means "no override -- read the global OutputScalingDownscaler and size from the
    // current feature", i.e. exactly the Output Scaling / Magnifier behaviour. Neural Rendering passes
    // its own DlssNrScalingDownscaler, which also switches Dispatch to sizing from the passed images.
    Scaler _scalerOverride;
    Scaler ActiveScaler() const;

  public:
    OS_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice, bool InUpsample);
    OS_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice, bool InUpsample,
          Scaler InScalerOverride);
    ~OS_Vk() = default;

    // Wrappers to maintain the original public API while using the generalized base methods
    bool CreateImageResource(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height,
                             VkFormat format, VkImageUsageFlags usage)
    {
        return Shader_Vk::CreateImageResource(width, height, format, usage);
    }
    void SetImageLayout(VkCommandBuffer cmdBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                        VkImageSubresourceRange subresourceRange)
    {
        Shader_Vk::SetImageLayout(cmdBuffer, image, oldLayout, newLayout, subresourceRange);
    }

    bool Dispatch(VkCommandBuffer InCmdList, const VkImageInfo& InResourceView, const VkImageInfo& OutResourceView);
};
