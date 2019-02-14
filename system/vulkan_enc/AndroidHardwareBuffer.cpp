/// Copyright (C) 2019 The Android Open Source Project
// Copyright (C) 2019 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "AndroidHardwareBuffer.h"

#include "Resources.h"

#include "gralloc_cb.h"
#include "vk_format_info.h"

namespace goldfish_vk {

// From Intel ANV implementation.
/* Construct ahw usage mask from image usage bits, see
 * 'AHardwareBuffer Usage Equivalence' in Vulkan spec.
 */
static uint64_t
goldfish_ahw_usage_from_vk_usage(const VkImageCreateFlags vk_create,
                                 const VkImageUsageFlags vk_usage)
{
   uint64_t ahw_usage = 0;

   if (vk_usage & VK_IMAGE_USAGE_SAMPLED_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   if (vk_usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   if (vk_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;

   if (vk_create & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP;

   if (vk_create & VK_IMAGE_CREATE_PROTECTED_BIT)
      ahw_usage |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;

   /* No usage bits set - set at least one GPU usage. */
   if (ahw_usage == 0)
      ahw_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

   return ahw_usage;
}

VkResult getAndroidHardwareBufferPropertiesANDROID(
    const HostVisibleMemoryVirtualizationInfo* hostMemVirtInfo,
    VkDevice,
    const AHardwareBuffer* buffer,
    VkAndroidHardwareBufferPropertiesANDROID* pProperties) {

    VkAndroidHardwareBufferFormatPropertiesANDROID* ahbFormatProps =
        (VkAndroidHardwareBufferFormatPropertiesANDROID*)vk_find_struct(
            (vk_struct*)pProperties->pNext,
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID);

    if (ahbFormatProps) {
        AHardwareBuffer_Desc desc;
        AHardwareBuffer_describe(buffer, &desc);

       uint64_t gpu_usage =
          AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
          AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
          AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;

        if (!(desc.usage & (gpu_usage))) {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }

        ahbFormatProps->format =
            vk_format_from_android(desc.format);

        // Just don't, for now
        ahbFormatProps->externalFormat = 0;

        // The formatFeatures member must include
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT and at least one of
        // VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT or
        // VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT, and should include
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT and
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT.

        ahbFormatProps->formatFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;

        // "Implementations may not always be able to determine the color model,
        // numerical range, or chroma offsets of the image contents, so the values in
        // VkAndroidHardwareBufferFormatPropertiesANDROID are only suggestions.
        // Applications should treat these values as sensible defaults to use in the
        // absence of more reliable information obtained through some other means."

        ahbFormatProps->samplerYcbcrConversionComponents.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.a = VK_COMPONENT_SWIZZLE_IDENTITY;

        ahbFormatProps->suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
        ahbFormatProps->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;

        ahbFormatProps->suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ahbFormatProps->suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    }

    const native_handle_t *handle =
       AHardwareBuffer_getNativeHandle(buffer);
    const cb_handle_t* cb_handle =
        reinterpret_cast<const cb_handle_t*>(handle);
    uint32_t colorBufferHandle = cb_handle->hostHandle;

    if (!colorBufferHandle) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    // Disallow host visible heaps for now
    // (hard to make actual dedicated allocs)
    uint32_t memoryTypeBits = 0;
    for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; ++i) {
        memoryTypeBits |=
            (!isHostVisibleMemoryTypeIndexForGuest(
             hostMemVirtInfo, i)) << i;
    }

    pProperties->memoryTypeBits = memoryTypeBits;
    pProperties->allocationSize =
        cb_handle->ashmemBase ? cb_handle->ashmemSize : 0;

    return VK_SUCCESS;
}

// Based on Intel ANV implementation.
VkResult getMemoryAndroidHardwareBufferANDROID(struct AHardwareBuffer **pBuffer) {

   /* Some quotes from Vulkan spec:
    *
    * "If the device memory was created by importing an Android hardware
    * buffer, vkGetMemoryAndroidHardwareBufferANDROID must return that same
    * Android hardware buffer object."
    *
    * "VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID must
    * have been included in VkExportMemoryAllocateInfo::handleTypes when
    * memory was created."
    */

    if (!pBuffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!(*pBuffer)) return VK_ERROR_OUT_OF_HOST_MEMORY;

    AHardwareBuffer_acquire(*pBuffer);
    return VK_SUCCESS;
}

VkResult importAndroidHardwareBuffer(
    const VkImportAndroidHardwareBufferInfoANDROID* info,
    struct AHardwareBuffer **importOut) {

    if (!info || !info->buffer) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    const native_handle_t *handle =
       AHardwareBuffer_getNativeHandle(info->buffer);
    const cb_handle_t* cb_handle =
        reinterpret_cast<const cb_handle_t*>(handle);
    uint32_t colorBufferHandle = cb_handle->hostHandle;

    if (!colorBufferHandle) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    auto ahb = info->buffer;

    AHardwareBuffer_acquire(ahb);
    *importOut = ahb;
    return VK_SUCCESS;
}

VkResult createAndroidHardwareBuffer(
    bool hasDedicatedImage,
    bool hasDedicatedBuffer,
    const VkExtent3D& imageExtent,
    uint32_t imageLayers,
    VkFormat imageFormat,
    VkImageUsageFlags imageUsage,
    VkImageCreateFlags imageCreateFlags,
    VkDeviceSize bufferSize,
    VkDeviceSize allocationInfoAllocSize,
    struct AHardwareBuffer **out) {

    uint32_t w = 0;
    uint32_t h = 1;
    uint32_t layers = 1;
    uint32_t format = 0;
    uint64_t usage = 0;

    /* If caller passed dedicated information. */
    if (hasDedicatedImage) {
       w = imageExtent.width;
       h = imageExtent.height;
       layers = imageLayers;
       format = android_format_from_vk(imageFormat);
       usage = goldfish_ahw_usage_from_vk_usage(imageCreateFlags, imageUsage);
    } else if (hasDedicatedBuffer) {
       w = bufferSize;
       format = AHARDWAREBUFFER_FORMAT_BLOB;
       usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    } else {
       w = allocationInfoAllocSize;
       format = AHARDWAREBUFFER_FORMAT_BLOB;
       usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    }

    struct AHardwareBuffer *ahw = NULL;
    struct AHardwareBuffer_Desc desc = {
        .width = w,
        .height = h,
        .layers = layers,
        .format = format,
        .usage = usage,
    };

    if (AHardwareBuffer_allocate(&desc, &ahw) != 0) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *out = ahw;

    return VK_SUCCESS;
}

} // namespace goldfish_vk