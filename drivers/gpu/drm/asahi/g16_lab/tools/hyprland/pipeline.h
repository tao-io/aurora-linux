/* SPDX-License-Identifier: MIT */
#include "triangle_spirv.h"

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
#define TRY(call) do { result = (call); if (result != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #call, result); goto out; } } while (0)
   VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {256,256,1}, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
   TRY(vkCreateImage(device, &image_info, NULL, &image));
   vkGetImageMemoryRequirements(device, image, &requirements);
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
out:
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
