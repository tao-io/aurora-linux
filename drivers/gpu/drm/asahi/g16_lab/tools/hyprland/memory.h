/* SPDX-License-Identifier: MIT */
static VkResult m4_allocate(VkPhysicalDevice physical, VkDevice device,
                           VkMemoryRequirements requirements, VkDeviceMemory *memory)
{
   VkPhysicalDeviceMemoryProperties props;
   vkGetPhysicalDeviceMemoryProperties(physical, &props);
   for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if ((requirements.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
         VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = i};
         return vkAllocateMemory(device, &info, NULL, memory);
      }
   }
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

