/*
 * SPDX-FileCopyrightText: The Yasvara Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "framefx.h"

#include <framefx/hook.h>
#include <log/log.h>

#include <cstring>
#include <mutex>

#include "driver.h"
#include "swapchain.h"

namespace vulkan {
namespace driver {
namespace framefx_vk {

const framefx_api* gEngine = nullptr;

namespace {

VkResult Acquire(VkDevice device, VkSwapchainKHR swapchain, VkSemaphore semaphore,
                 uint32_t* index) {
    return AcquireNextImageKHR(device, swapchain, UINT64_MAX, semaphore, VK_NULL_HANDLE, index);
}

int64_t RefreshNs(VkSwapchainKHR swapchain) {
    VkRefreshCycleDurationGOOGLE refresh = {};
    return GetRefreshCycleDurationGOOGLE(VK_NULL_HANDLE, swapchain, &refresh) == VK_SUCCESS
            ? static_cast<int64_t>(refresh.refreshDuration)
            : 0;
}

VKAPI_ATTR void DestroyDeviceHook(VkDevice device, const VkAllocationCallbacks* allocator) {
    gEngine->vk_destroy_device(device);
    DestroyDevice(device, allocator);
}

VKAPI_ATTR void GetDeviceQueueHook(VkDevice device, uint32_t family, uint32_t index,
                                   VkQueue* queue) {
    GetDeviceQueue(device, family, index, queue);
    gEngine->vk_get_device_queue(device, family, *queue);
}

VKAPI_ATTR void GetDeviceQueue2Hook(VkDevice device, const VkDeviceQueueInfo2* info,
                                    VkQueue* queue) {
    GetDeviceQueue2(device, info, queue);
    if (*queue != VK_NULL_HANDLE) {
        gEngine->vk_get_device_queue(device, info->queueFamilyIndex, *queue);
    }
}

template <typename T>
T either(T core, T khr) {
    return core != nullptr ? core : khr;
}

}  // namespace

void Init() {
    static std::mutex lock;
    static bool decided = false;
    std::lock_guard guard(lock);
    // The zygote and system processes are never armed, and must not decide for their forks.
    if (decided || !framefx::IsAppUid(getuid())) return;
    decided = true;
    const char* error = nullptr;
    const framefx_api* engine = framefx::LoadEngine(&error);
    if (engine == nullptr) {
        if (error != nullptr) ALOGE("framefx: armed but the engine did not load: %s", error);
        return;
    }
    const framefx_vk_host host = {
            .create_swapchain = CreateSwapchainKHR,
            .destroy_swapchain = DestroySwapchainKHR,
            .get_swapchain_images = GetSwapchainImagesKHR,
            .queue_present = QueuePresentKHR,
            .acquire = Acquire,
            .release = ReleaseUnpresentedImage,
            .refresh_ns = RefreshNs,
            .window = SwapchainWindow,
            .allocate_command_buffers = AllocateCommandBuffers,
    };
    engine->vk_attach(&host);
    gEngine = engine;
}

VkResult CreateDevice(const InstanceData& data, VkPhysicalDevice physical,
                      const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator,
                      VkDevice* device) {
    const InstanceDriverTable& d = data.driver;
    const framefx_vk_instance instance = {
            .get_properties2 =
                    either(d.GetPhysicalDeviceProperties2, d.GetPhysicalDeviceProperties2KHR),
            .get_features2 = either(d.GetPhysicalDeviceFeatures2, d.GetPhysicalDeviceFeatures2KHR),
            .get_format_properties2 = either(d.GetPhysicalDeviceFormatProperties2,
                                             d.GetPhysicalDeviceFormatProperties2KHR),
            .get_memory_properties2 = either(d.GetPhysicalDeviceMemoryProperties2,
                                             d.GetPhysicalDeviceMemoryProperties2KHR),
            .enumerate_extensions = d.EnumerateDeviceExtensionProperties,
            .create_device = d.CreateDevice,
            .get_device_proc_addr = data.get_device_proc_addr,
            .instance = data.handle,
    };
    return gEngine->vk_create_device(&instance, physical, info, allocator, device);
}

PFN_vkVoidFunction Override(const char* name) {
    if (strcmp(name, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(gEngine->vk_queue_present);
    }
    if (strcmp(name, "vkCreateSwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(gEngine->vk_create_swapchain);
    }
    if (strcmp(name, "vkDestroySwapchainKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(gEngine->vk_destroy_swapchain);
    }
    if (strcmp(name, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyDeviceHook);
    }
    if (strcmp(name, "vkGetDeviceQueue") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceQueueHook);
    }
    if (strcmp(name, "vkGetDeviceQueue2") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(GetDeviceQueue2Hook);
    }
    return nullptr;
}

PFN_vkVoidFunction Intercept(VkDevice device, const char* name, PFN_vkVoidFunction next) {
    return gEngine->vk_intercept(device, name, next);
}

}  // namespace framefx_vk
}  // namespace driver
}  // namespace vulkan
