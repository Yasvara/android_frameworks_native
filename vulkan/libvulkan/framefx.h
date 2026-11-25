/*
 * SPDX-FileCopyrightText: The Yasvara Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <vulkan/vulkan.h>

struct framefx_api;

namespace vulkan {
namespace driver {

struct InstanceData;

// framefx present hook (Luwes frame generation, Kelir) for processes Pentas armed.
//
// Arming is decided once, at the first instance an app process creates: an unarmed process never
// loads libframefx and keeps every loader entry point. In an armed one, the device-level calls
// that frame a swapchain's life resolve to the engine, which presents through the loader's own
// swapchain below it.
namespace framefx_vk {

extern const framefx_api* gEngine;

inline bool Armed() {
    return gEngine != nullptr;
}

void Init();

VkResult CreateDevice(const InstanceData& instance, VkPhysicalDevice physical,
                      const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator,
                      VkDevice* device);

// The engine's entry point for a device-level `name`, or nullptr for the loader's own.
PFN_vkVoidFunction Override(const char* name);

// The engine's wrapper around `next`, the entry point the device would otherwise get, or nullptr.
// `device` is VK_NULL_HANDLE for a device command looked up through the instance.
PFN_vkVoidFunction Intercept(VkDevice device, const char* name, PFN_vkVoidFunction next);

}  // namespace framefx_vk
}  // namespace driver
}  // namespace vulkan
