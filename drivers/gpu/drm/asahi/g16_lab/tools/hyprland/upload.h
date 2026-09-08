/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include "memory.h"

/* Exercise the graphics meta-copy path with varying image sizes and padded
 * source rows. Read the raw optimal image only for this known M4 tile layout. */
static VkResult m4_upload_images(VkPhysicalDevice physical, VkDevice device, VkQueue queue)
{
   const unsigned sizes[8] = {2, 4, 16, 32, 64, 128, 256, 2};
   VkResult result = VK_SUCCESS;
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory source_memory = VK_NULL_HANDLE, image_memory = VK_NULL_HANDLE;
   VkImage image = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   void *source = NULL, *target = NULL;
   VkMemoryRequirements requirements;
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #call, result); goto out; } } while (0)
   VkBufferCreateInfo binfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 264 * 256 * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
   TRY(vkCreateBuffer(device, &binfo, NULL, &buffer));
   vkGetBufferMemoryRequirements(device, buffer, &requirements);
   TRY(m4_allocate(physical, device, requirements, &source_memory));
   TRY(vkBindBufferMemory(device, buffer, source_memory, 0));
   TRY(vkMapMemory(device, source_memory, 0, VK_WHOLE_SIZE, 0, &source));
   VkCommandPoolCreateInfo pinfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
   TRY(vkCreateCommandPool(device, &pinfo, NULL, &pool));
   VkCommandBufferAllocateInfo ainfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cmd;
   TRY(vkAllocateCommandBuffers(device, &ainfo, &cmd));
   for (unsigned frame = 0; frame < 8; ++frame) {
      unsigned side = sizes[frame], stride = side + 8;
      VkImageCreateInfo iinfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {side, side, 1}, .mipLevels = 1, .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
      TRY(vkCreateImage(device, &iinfo, NULL, &image));
      vkGetImageMemoryRequirements(device, image, &requirements);
      TRY(m4_allocate(physical, device, requirements, &image_memory));
      TRY(vkBindImageMemory(device, image, image_memory, 0));
      TRY(vkMapMemory(device, image_memory, 0, VK_WHOLE_SIZE, 0, &target));
      memset(target, 0xcd, requirements.size);
      memset(source, 0xa5, binfo.size);
      for (unsigned y = 0; y < side; ++y)
         for (unsigned x = 0; x < side; ++x) {
            unsigned char *p = (unsigned char *)source + (y * stride + x) * 4;
            p[0] = x; p[1] = y; p[2] = frame * 29; p[3] = 255;
         }
      TRY(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      TRY(vkBeginCommandBuffer(cmd, &begin));
      VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0, NULL, 0, NULL, 1, &barrier);
      VkBufferImageCopy region = {.bufferRowLength = stride,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {side, side, 1}};
      vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      TRY(vkEndCommandBuffer(cmd));
      VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
      TRY(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      TRY(vkQueueWaitIdle(queue));
      unsigned tile = side < 64 ? side : 64;
      unsigned failures = 0;
      for (unsigned y = 0; y < side; ++y)
         for (unsigned x = 0; x < side; ++x) {
            unsigned index = ((y / tile) * (side / tile) + x / tile) * tile * tile;
            for (unsigned bit = 0; (1u << bit) < tile; ++bit)
               index |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1);
            const unsigned char *actual = (unsigned char *)target + index * 4;
            const unsigned char *expected = (unsigned char *)source + (y * stride + x) * 4;
            if (memcmp(actual, expected, 4) && failures++ < 8)
               fprintf(stderr, "upload %u pixel %u,%u: %u,%u,%u,%u expected %u,%u,%u,%u\n",
                  frame, x, y, actual[0], actual[1], actual[2], actual[3],
                  expected[0], expected[1], expected[2], expected[3]);
         }
      const char *directory = getenv("M4_RENDER_OUTPUT");
      if (directory) {
         char path[4096];
         snprintf(path, sizeof(path), "%s/upload-%02u.raw", directory, frame);
         FILE *file = fopen(path, "wb");
         if (!file) { result = VK_ERROR_UNKNOWN; goto out; }
         size_t written = fwrite(target, 1, requirements.size, file);
         int closed = fclose(file);
         if (written != requirements.size || closed) { result = VK_ERROR_UNKNOWN; goto out; }
      }
      fprintf(stderr, "upload %u: %ux%u, %u/%u pixels match\n", frame, side, side, side * side - failures, side * side);
      if (failures) { result = VK_ERROR_UNKNOWN; goto out; }
      vkUnmapMemory(device, image_memory); target = NULL;
      vkDestroyImage(device, image, NULL); image = VK_NULL_HANDLE;
      vkFreeMemory(device, image_memory, NULL); image_memory = VK_NULL_HANDLE;
   }
out:
   if (target) vkUnmapMemory(device, image_memory);
   if (source) vkUnmapMemory(device, source_memory);
   if (pool) vkDestroyCommandPool(device, pool, NULL);
   if (image) vkDestroyImage(device, image, NULL);
   if (buffer) vkDestroyBuffer(device, buffer, NULL);
   if (image_memory) vkFreeMemory(device, image_memory, NULL);
   if (source_memory) vkFreeMemory(device, source_memory, NULL);
#undef TRY
   return result;
}
