/* SPDX-License-Identifier: MIT */
#include "triangle_spirv.h"
#include <stdlib.h>

#ifdef M4_SUBMIT_PROBE
static int m4_check_pixels(const void *image, unsigned frame)
{
   unsigned char linear[256 * 256 * 4];
   unsigned failures = 0;
   const unsigned char expected[4] = {
      (frame * 255 + 4) / 8, ((7 - frame) * 255 + 4) / 8, 32, 255,
   };
   for (unsigned y = 0; y < 256; ++y) {
      for (unsigned x = 0; x < 256; ++x) {
         unsigned index = ((y / 64) * 4 + x / 64) * 4096;
         for (unsigned bit = 0; bit < 6; ++bit)
            index |= ((x >> bit) & 1) << (2 * bit) |
                     ((y >> bit) & 1) << (2 * bit + 1);
         const unsigned char *p = (const unsigned char *)image + index * 4;
         memcpy(linear + (y * 256 + x) * 4, p, 4);
         bool triangle = true, clear = true;
         for (unsigned c = 0; c < 4; ++c) {
            triangle &= abs((int)p[c] - expected[c]) <= 1;
            clear &= p[c] == 0;
         }
         /* Pixel centers on the diagonal depend on the top-left edge rule.
          * Require either the exact triangle color or the clear there. */
         bool valid = x + y < 255 ? triangle : x + y > 255 ? clear : triangle || clear;
         if (!valid && failures++ < 8)
            fprintf(stderr, "frame %u pixel %u,%u: %u,%u,%u,%u\n",
                    frame, x, y, p[0], p[1], p[2], p[3]);
      }
   }
   const char *directory = getenv("M4_RENDER_OUTPUT");
   if (directory) {
      char path[4096];
      int count = snprintf(path, sizeof(path), "%s/vulkan-frame-%02u.rgba", directory, frame);
      if (count < 0 || (size_t)count >= sizeof(path))
         return -1;
      FILE *file = fopen(path, "wb");
      if (!file)
         return -1;
      size_t written = fwrite(linear, 1, sizeof(linear), file);
      int closed = fclose(file);
      if (written != sizeof(linear) || closed)
         return -1;
   }
   fprintf(stderr, "frame %u: %u/65536 pixels match\n", frame, 65536 - failures);
   return failures ? -1 : 0;
}
#endif

#include "memory.h"

static VkResult m4_record_triangle(VkPhysicalDevice physical, VkDevice device)
{
   VkResult result = VK_SUCCESS;
   VkImage image = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
   VkBuffer vertex_buffer = VK_NULL_HANDLE;
   VkDeviceMemory image_memory = VK_NULL_HANDLE, vertex_memory = VK_NULL_HANDLE;
   VkRenderPass pass = VK_NULL_HANDLE;
   VkFramebuffer framebuffer = VK_NULL_HANDLE;
   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkMemoryRequirements requirements;
#ifdef M4_SUBMIT_PROBE
   void *image_map = NULL;
   VkDeviceSize image_bytes = 0;
#endif
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #call, result); goto out; } } while (0)
   VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {256,256,1}, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
   TRY(vkCreateImage(device, &image_info, NULL, &image));
   vkGetImageMemoryRequirements(device, image, &requirements);
#ifdef M4_SUBMIT_PROBE
   image_bytes = requirements.size;
   fprintf(stderr, "M4 uncompressed RGBA8 image allocation: %llu bytes\n", (unsigned long long)image_bytes);
   if (image_bytes != 256 * 256 * 4) {
      result = VK_ERROR_FORMAT_NOT_SUPPORTED;
      goto out;
   }
#endif
   TRY(m4_allocate(physical, device, requirements, &image_memory));
   TRY(vkBindImageMemory(device, image, image_memory, 0));
   VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image_info.format,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
   TRY(vkCreateImageView(device, &view_info, NULL, &view));
   const float vertices[3][4] = {{-1,-1,1,0}, {1,-1,0,1}, {-1,1,0,0}};
   VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = sizeof(vertices), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   TRY(vkCreateBuffer(device, &buffer_info, NULL, &vertex_buffer));
   vkGetBufferMemoryRequirements(device, vertex_buffer, &requirements);
   TRY(m4_allocate(physical, device, requirements, &vertex_memory));
   TRY(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0));
   void *mapped;
   TRY(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
   memcpy(mapped, vertices, sizeof(vertices));
   vkUnmapMemory(device, vertex_memory);
   VkAttachmentDescription attachment = {.format = image_info.format, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference color = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1, .pColorAttachments = &color};
   VkRenderPassCreateInfo pass_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
   TRY(vkCreateRenderPass(device, &pass_info, NULL, &pass));
   VkFramebufferCreateInfo framebuffer_info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = pass, .attachmentCount = 1, .pAttachments = &view, .width = 256, .height = 256, .layers = 1};
   TRY(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));
   VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(triangle_vert_spv), .pCode = triangle_vert_spv};
   TRY(vkCreateShaderModule(device, &module_info, NULL, &vs));
   module_info.codeSize = sizeof(triangle_frag_spv); module_info.pCode = triangle_frag_spv;
   TRY(vkCreateShaderModule(device, &module_info, NULL, &fs));
   VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   TRY(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
   VkPipelineShaderStageCreateInfo stages[] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
   };
   VkVertexInputBindingDescription binding = {.binding = 0, .stride = sizeof(vertices[0])};
   VkVertexInputAttributeDescription attribute = {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT};
   VkPipelineVertexInputStateCreateInfo vertex_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
      .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute};
   VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport viewport = {.width = 256, .height = 256, .maxDepth = 1};
   VkRect2D scissor = {.extent = {256,256}};
   VkPipelineViewportStateCreateInfo viewport_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
   VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
   VkPipelineMultisampleStateCreateInfo msaa = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 15};
   VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &blend_attachment};
   VkGraphicsPipelineCreateInfo pipeline_info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex_info, .pInputAssemblyState = &assembly,
      .pViewportState = &viewport_info, .pRasterizationState = &raster, .pMultisampleState = &msaa,
      .pColorBlendState = &blend, .layout = layout, .renderPass = pass};
   TRY(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
   fprintf(stderr, "M4 vertex/fragment graphics pipeline compiled\n");
   VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
   TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
   VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer commands;
   TRY(vkAllocateCommandBuffers(device, &allocation, &commands));
   VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   TRY(vkBeginCommandBuffer(commands, &begin));
   VkClearValue clear = {.color = {.float32 = {0,0,0,0}}};
   VkRenderPassBeginInfo render = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = pass, .framebuffer = framebuffer, .renderArea = scissor, .clearValueCount = 1, .pClearValues = &clear};
   vkCmdBeginRenderPass(commands, &render, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   VkDeviceSize offset = 0;
   vkCmdBindVertexBuffers(commands, 0, 1, &vertex_buffer, &offset);
   vkCmdDraw(commands, 3, 1, 0, 0);
   vkCmdEndRenderPass(commands);
   TRY(vkEndCommandBuffer(commands));
   fprintf(stderr, "M4 triangle commands recorded; no GPU submission\n");
#ifdef M4_SUBMIT_PROBE
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);
   VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &commands};
   TRY(vkMapMemory(device, image_memory, 0, VK_WHOLE_SIZE, 0, &image_map));
   for (unsigned frame = 0; frame < 8; ++frame) {
      TRY(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
      float (*vertex)[4] = mapped;
      for (unsigned i = 0; i < 3; ++i) {
         vertex[i][2] = frame / 8.0f;
         vertex[i][3] = (7 - frame) / 8.0f;
      }
      vkUnmapMemory(device, vertex_memory);
      memset(image_map, 0xcd, image_bytes);
      TRY(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      TRY(vkQueueWaitIdle(queue));
      if (m4_check_pixels(image_map, frame)) {
         result = VK_ERROR_UNKNOWN;
         goto out;
      }
   }
   fprintf(stderr, "M4 Vulkan triangle: 8 submissions, 524288 pixels checked\n");
#endif
out:
#ifdef M4_SUBMIT_PROBE
   if (image_map)
      vkUnmapMemory(device, image_memory);
#endif
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyShaderModule(device, fs, NULL);
   vkDestroyShaderModule(device, vs, NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyImageView(device, view, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkFreeMemory(device, image_memory, NULL);
#undef TRY
   return result;
}
