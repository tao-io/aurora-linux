/* SPDX-License-Identifier: MIT */
/* Device/queue creation, optionally graphics command recording.
 * Neither mode submits commands or GPU jobs. */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#ifdef M4_PIPELINE_PROBE
#include "pipeline.h"
#endif

int main(void)
{
   VkInstance instance;
   VkInstanceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   VkResult ret = vkCreateInstance(&create, NULL, &instance);
   if (ret != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance: %d\n", ret);
      return 1;
   }
   VkPhysicalDevice devices[8];
   uint32_t count = 8;
   ret = vkEnumeratePhysicalDevices(instance, &count, devices);
   if (ret != VK_SUCCESS || count != 1) {
      fprintf(stderr, "vkEnumeratePhysicalDevices: %d, devices=%u\n", ret, count);
      vkDestroyInstance(instance, NULL);
      return 1;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(devices[0], &props);
   if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
       props.deviceID != 0x8132 || !strstr(props.deviceName, "M4")) {
      fprintf(stderr, "unexpected device: %s %#x type=%u\n", props.deviceName, props.deviceID, props.deviceType);
      vkDestroyInstance(instance, NULL);
      return 1;
   }
   fprintf(stderr, "physical device: %s, deviceID=%#x\n", props.deviceName, props.deviceID);
   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
   VkDeviceCreateInfo device_create = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
   VkDevice device;
   ret = vkCreateDevice(devices[0], &device_create, NULL, &device);
   if (ret != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice: %d\n", ret);
      vkDestroyInstance(instance, NULL);
      return 1;
   }
   VkQueue graphics_queue;
   vkGetDeviceQueue(device, 0, 0, &graphics_queue);
   if (graphics_queue == VK_NULL_HANDLE) {
      fprintf(stderr, "vkGetDeviceQueue returned no queue\n");
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      return 1;
   }
#ifdef M4_PIPELINE_PROBE
   ret = m4_record_triangle(devices[0], device);
   if (ret != VK_SUCCESS) {
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      return 1;
   }
#endif
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   puts("{\"passed\":true,\"device_id\":33074,\"physical_device_initialized\":true,\"logical_device_initialized\":true,"
#ifdef M4_PIPELINE_PROBE
        "\"graphics_commands_recorded\":true,"
#endif
        "\"shader_submitted\":false}");
   return 0;
}
