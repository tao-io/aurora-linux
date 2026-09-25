/* SPDX-License-Identifier: MIT
 * Attachmentless maximum-size framebuffer with a small, observable viewport.
 * Compile beside generated vertex.h/fragment.h. Hardware requires gpu-experiment.
 */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vertex.h"
#include "fragment.h"
#define CHECK(x) do { VkResult r=(x); if(r) { fprintf(stderr,"%s: %d\n",#x,r); exit(1); } } while(0)
int main(void) {
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
 VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
 VkInstance inst; CHECK(vkCreateInstance(&ici,0,&inst));
 VkPhysicalDevice phy; uint32_t n=1; CHECK(vkEnumeratePhysicalDevices(inst,&n,&phy));
 VkPhysicalDeviceProperties prop; vkGetPhysicalDeviceProperties(phy,&prop);
 printf("Device: %s\n",prop.deviceName); fflush(stdout);
 if(prop.deviceID!=0x8132 && !(getenv("PROBE_SOFTWARE") && prop.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)) return 2;
 vkGetPhysicalDeviceQueueFamilyProperties(phy,&n,0);
 VkQueueFamilyProperties *families=calloc(n,sizeof(*families)); vkGetPhysicalDeviceQueueFamilyProperties(phy,&n,families);
 uint32_t q=0; while(q<n && !(families[q].queueFlags&VK_QUEUE_GRAPHICS_BIT)) q++; if(q==n)return 2; free(families);
 float priority=1;
 VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=q,.queueCount=1,.pQueuePriorities=&priority};
 VkPhysicalDeviceDynamicRenderingFeatures dr={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,.dynamicRendering=VK_TRUE};
 VkPhysicalDeviceFeatures feat={.fragmentStoresAndAtomics=VK_TRUE};
 VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&dr,.pEnabledFeatures=&feat,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
 VkDevice dev; CHECK(vkCreateDevice(phy,&di,0,&dev)); VkQueue queue; vkGetDeviceQueue(dev,q,0,&queue);
 VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64*64*4,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
 VkBuffer buffer; CHECK(vkCreateBuffer(dev,&bi,0,&buffer));
 VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,buffer,&req);
 VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phy,&mp);
 uint32_t mt=0; for(;mt<mp.memoryTypeCount;mt++) if((req.memoryTypeBits&(1u<<mt)) && (mp.memoryTypes[mt].propertyFlags&6)==6)break;
 if(mt==mp.memoryTypeCount)return 2;
 VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=mt};
 VkDeviceMemory mem; CHECK(vkAllocateMemory(dev,&ai,0,&mem)); CHECK(vkBindBufferMemory(dev,buffer,mem,0));
 uint32_t *data; CHECK(vkMapMemory(dev,mem,0,VK_WHOLE_SIZE,0,(void**)&data)); memset(data,0,64*64*4);
 VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
 VkDescriptorSetLayoutCreateInfo sli={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
 VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(dev,&sli,0,&sl));
 VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
 VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
 VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev,&dpi,0,&dp));
 VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&sl};
 VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(dev,&dai,&ds));
 VkDescriptorBufferInfo db={buffer,0,64*64*4};
 VkWriteDescriptorSet wr={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
 vkUpdateDescriptorSets(dev,1,&wr,0,0);
 VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&sl};
 VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev,&pli,0,&pl));
 VkShaderModule modules[2]; const uint32_t *code[]={vertex,fragment}; size_t size[]={sizeof(vertex),sizeof(fragment)};
 VkPipelineShaderStageCreateInfo stages[2];
 for(unsigned i=0;i<2;i++) { VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size[i],.pCode=code[i]}; CHECK(vkCreateShaderModule(dev,&si,0,&modules[i])); stages[i]=(VkPipelineShaderStageCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT,.module=modules[i],.pName="main"}; }
 VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
 VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
 VkViewport vp={0,0,64,64,0,1}; VkRect2D sc={{0,0},{64,64}};
 VkPipelineViewportStateCreateInfo vs={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&sc};
 VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
 VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
 VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
 VkPipelineRenderingCreateInfo pr={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
 VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&pr,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vs,.pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&cb,.layout=pl};
 VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(dev,0,1,&pi,0,&pipeline));
 VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=q};
 VkCommandPool cp; CHECK(vkCreateCommandPool(dev,&cpi,0,&cp));
 VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY};
 VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev,&cai,&cmd));
 VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
 VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{16384,16384}},.layerCount=32};
 vkCmdBeginRendering(cmd,&ri); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&ds,0,0); vkCmdDraw(cmd,3,1,0,0); vkCmdEndRendering(cmd);
 VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,0,0,0);
 CHECK(vkEndCommandBuffer(cmd));
 VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(vkCreateFence(dev,&fi,0,&fence));
 VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd}; CHECK(vkQueueSubmit(queue,1,&submit,fence));
 CHECK(vkWaitForFences(dev,1,&fence,VK_TRUE,20000000000ull));
 unsigned bad=0; for(unsigned i=0;i<4096;i++) if(data[i]!=i+1)bad++;
 printf("%s: 16K x 16K x 32 attachmentless fragment writes bad=%u/4096\n",bad?"FAIL":"PASS",bad);
 CHECK(vkDeviceWaitIdle(dev));
 vkDestroyFence(dev,fence,0); vkDestroyCommandPool(dev,cp,0); vkDestroyPipeline(dev,pipeline,0);
 for(unsigned i=0;i<2;i++)vkDestroyShaderModule(dev,modules[i],0);
 vkDestroyPipelineLayout(dev,pl,0);vkDestroyDescriptorPool(dev,dp,0);vkDestroyDescriptorSetLayout(dev,sl,0);
 vkUnmapMemory(dev,mem);vkDestroyBuffer(dev,buffer,0);vkFreeMemory(dev,mem,0);vkDestroyDevice(dev,0);vkDestroyInstance(inst,0);
 return bad?1:0;
}
