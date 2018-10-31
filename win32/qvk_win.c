/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2018 Krzysztof Kondrak

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
/*
** QVK_WIN.C
**
** This file implements the operating system binding of Vk to QVk function
** pointers.  When doing a port of Quake2 you must implement the following
** two functions:
**
** QVk_Init() - loads libraries, assigns function pointers, etc.
** QVk_Shutdown() - unloads libraries, NULLs function pointers
*/
#include <float.h>
#include "../ref_vk/vk_local.h"
#include "vk_win.h"

FILE *vk_logfp = NULL;

VkInstance	 vk_instance = VK_NULL_HANDLE;
VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
VmaAllocator vk_malloc  = VK_NULL_HANDLE;
qvkdevice_t	 vk_device = {
	.physical = VK_NULL_HANDLE,
	.logical = VK_NULL_HANDLE,
	.gfxQueue = VK_NULL_HANDLE,
	.presentQueue = VK_NULL_HANDLE,
	.transferQueue = VK_NULL_HANDLE,
	.gfxFamilyIndex = -1,
	.presentFamilyIndex = -1,
	.transferFamilyIndex = -1
};
qvkswapchain_t vk_swapchain = {
	.sc = VK_NULL_HANDLE,
	.format = VK_FORMAT_UNDEFINED,
	.presentMode = VK_PRESENT_MODE_MAILBOX_KHR,
	.extent = { 0, 0 },
	.images = NULL,
	.imageCount = 0
};

// Vulkan renderpasses (standard and MSAA)
qvkrenderpass_t vk_renderpasses[RT_COUNT] = { 
	{
		.rp = VK_NULL_HANDLE,
		.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.sampleCount = VK_SAMPLE_COUNT_1_BIT
	},
	{
		.rp = VK_NULL_HANDLE,
		.colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.sampleCount = VK_SAMPLE_COUNT_8_BIT
	} 
};

VkCommandPool vk_commandPool = VK_NULL_HANDLE;
VkCommandPool vk_transferCommandPool = VK_NULL_HANDLE;
VkDescriptorPool vk_descriptorPool = VK_NULL_HANDLE;

// Vulkan image views
VkImageView *vk_imageviews = NULL;
// Vulkan framebuffers
VkFramebuffer *vk_framebuffers[RT_COUNT];
// depth buffer
qvktexture_t vk_depthbuffer = QVVKTEXTURE_INIT;
// render targets for MSAA
qvktexture_t vk_msaaColorbuffer = QVVKTEXTURE_INIT;
qvktexture_t vk_msaaDepthbuffer = QVVKTEXTURE_INIT;
// viewport and scissor
VkViewport vk_viewport = { .0f, .0f, .0f, .0f, .0f, .0f };
VkRect2D vk_scissor = { { 0, 0 }, { 0, 0 } };

#define NUM_CMDBUFFERS 2
// Vulkan command buffers
VkCommandBuffer *vk_commandbuffers = NULL;
// command buffer double buffering fences
VkFence vk_fences[NUM_CMDBUFFERS];
// semaphore: signal when next image is available for rendering
VkSemaphore vk_imageAvailableSemaphores[NUM_CMDBUFFERS];
// semaphore: signal when rendering to current command buffer is complete
VkSemaphore vk_renderFinishedSemaphores[NUM_CMDBUFFERS];
// tracker variables
qvkrenderpass_t vk_activeRenderpass;
VkFramebuffer   vk_activeFramebuffer = VK_NULL_HANDLE;
VkCommandBuffer vk_activeCmdbuffer = VK_NULL_HANDLE;
// index of active command buffer
int vk_activeBufferIdx = 0;
// index of currently acquired image
int vk_imageIndex = 0;

PFN_vkCreateDebugUtilsMessengerEXT qvkCreateDebugUtilsMessengerEXT;
PFN_vkDestroyDebugUtilsMessengerEXT qvkDestroyDebugUtilsMessengerEXT;

qvkpipeline_t vk_console_pipeline = QVKPIPELINE_INIT;

#define VK_INPUTBIND_DESC(s) { \
	.binding = 0, \
	.stride = s, \
	.inputRate = VK_VERTEX_INPUT_RATE_VERTEX \
};

#define VK_INPUTATTR_DESC(l, f, o) { \
	.binding = 0, \
	.location = l, \
	.format = f, \
	.offset = o \
}
qvkbuffer_t vertexBuffer;
qvkbuffer_t indexBuffer;
qvkbuffer_t uniformBuffer;

// we will need multiple of these
VkDescriptorSetLayout descriptorSetLayout;

qvkshader_t shaders[2];
const VkDeviceSize consoleUboSize = sizeof(float) * 8;

VkFormat QVk_FindDepthFormat()
{
	VkFormat depthFormats[] = {
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D16_UNORM_S8_UINT,
		VK_FORMAT_D16_UNORM
	};

	for (int i = 0; i < 5; ++i)
	{
		VkFormatProperties formatProps;
		vkGetPhysicalDeviceFormatProperties(vk_device.physical, depthFormats[i], &formatProps);

		if (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			return depthFormats[i];
	}

	return VK_FORMAT_D16_UNORM;
}

// internal helpers
void DestroyImageViews()
{
	for (int i = 0; i < vk_swapchain.imageCount; i++)
		vkDestroyImageView(vk_device.logical, vk_imageviews[i], NULL);
	free(vk_imageviews);
	vk_imageviews = NULL;
}

VkResult CreateImageViews()
{
	VkResult res = VK_SUCCESS;
	vk_imageviews = (VkImageView *)malloc(vk_swapchain.imageCount * sizeof(VkImageView));

	for (size_t i = 0; i < vk_swapchain.imageCount; ++i)
	{
		VkResult res = QVk_CreateImageView(&vk_swapchain.images[i], VK_IMAGE_ASPECT_COLOR_BIT, &vk_imageviews[i], vk_swapchain.format, 1);

		if (res != VK_SUCCESS)
		{
			DestroyImageViews();
			return res;
		}
	}

	return res;
}

void DestroyFramebuffers()
{
	for (int f = 0; f < RT_COUNT; f++)
	{
		if (vk_framebuffers[f])
		{
			for (int i = 0; i < vk_swapchain.imageCount; ++i)
			{
				vkDestroyFramebuffer(vk_device.logical, vk_framebuffers[f][i], NULL);
			}

			free(vk_framebuffers[f]);
			vk_framebuffers[f] = NULL;
		}
	}
}

VkResult CreateFramebuffers(const qvkrenderpass_t *renderpass, qvkrendertype_t framebufferType)
{
	VkResult res = VK_SUCCESS;
	vk_framebuffers[framebufferType] = (VkFramebuffer *)malloc(vk_swapchain.imageCount * sizeof(VkFramebuffer));

	VkFramebufferCreateInfo fbCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = renderpass->rp,
		.attachmentCount = (renderpass->sampleCount != VK_SAMPLE_COUNT_1_BIT) ? 3 : 2,
		.width = vk_swapchain.extent.width,
		.height = vk_swapchain.extent.height,
		.layers = 1
	};

	for (size_t i = 0; i < vk_swapchain.imageCount; ++i)
	{
		VkImageView attachments[] = { vk_imageviews[i], vk_depthbuffer.imageView };
		VkImageView attachmentsMSAA[] = { vk_msaaColorbuffer.imageView, vk_msaaDepthbuffer.imageView, vk_imageviews[i] };

		fbCreateInfo.pAttachments = (renderpass->sampleCount != VK_SAMPLE_COUNT_1_BIT) ? attachmentsMSAA : attachments;
		VkResult result = vkCreateFramebuffer(vk_device.logical, &fbCreateInfo, NULL, &vk_framebuffers[framebufferType][i]);

		if (result != VK_SUCCESS)
		{
			DestroyFramebuffers();
			return res;
		}
	}

	return res;
}

void CreateDrawBuffers()
{
	QVk_CreateDepthBuffer(vk_renderpasses[RT_STANDARD].sampleCount, &vk_depthbuffer);
	ri.Con_Printf(PRINT_ALL, "...created depth buffer\n");
	QVk_CreateDepthBuffer(vk_renderpasses[RT_MSAA].sampleCount, &vk_msaaDepthbuffer);
	ri.Con_Printf(PRINT_ALL, "...created MSAA depth buffer\n");
	QVk_CreateColorBuffer(vk_renderpasses[RT_MSAA].sampleCount, &vk_msaaColorbuffer);
	ri.Con_Printf(PRINT_ALL, "...created MSAA color buffer\n");
}

void DestroyDrawBuffer(qvktexture_t *drawBuffer)
{
	if (drawBuffer->image != VK_NULL_HANDLE)
	{
		vmaDestroyImage(vk_malloc, drawBuffer->image, drawBuffer->allocation);
		vkDestroyImageView(vk_device.logical, drawBuffer->imageView, NULL);
		drawBuffer->image = VK_NULL_HANDLE;
		drawBuffer->imageView = VK_NULL_HANDLE;
	}
}

void DestroyDrawBuffers()
{
	DestroyDrawBuffer(&vk_depthbuffer);
	DestroyDrawBuffer(&vk_msaaDepthbuffer);
	DestroyDrawBuffer(&vk_msaaColorbuffer);
}

/*
** QVk_Shutdown
**
** Destroy all Vulkan related resources.
*/
void QVk_Shutdown( void )
{
	if (vk_instance != VK_NULL_HANDLE)
	{
		ri.Con_Printf(PRINT_ALL, "Shutting down Vulkan\n");
		vkDeviceWaitIdle(vk_device.logical);

		QVk_DestroyPipeline(&vk_console_pipeline);
		QVk_FreeBuffer(&vertexBuffer);
		QVk_FreeBuffer(&indexBuffer);
		QVk_FreeBuffer(&uniformBuffer);
		vkDestroyDescriptorPool(vk_device.logical, vk_descriptorPool, NULL);
		vkDestroyDescriptorSetLayout(vk_device.logical, descriptorSetLayout, NULL);

		for (int i = 0; i < RT_COUNT; i++)
		{
			if (vk_renderpasses[i].rp != VK_NULL_HANDLE)
				vkDestroyRenderPass(vk_device.logical, vk_renderpasses[i].rp, NULL);
			vk_renderpasses[i].rp = VK_NULL_HANDLE;
		}
		if (vk_commandbuffers)
		{
			vkFreeCommandBuffers(vk_device.logical, vk_commandPool, NUM_CMDBUFFERS, vk_commandbuffers);
			free(vk_commandbuffers);
			vk_commandbuffers = NULL;
		}
		if (vk_commandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(vk_device.logical, vk_commandPool, NULL);
		if (vk_transferCommandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(vk_device.logical, vk_transferCommandPool, NULL);
		DestroyFramebuffers();
		DestroyImageViews();
		DestroyDrawBuffers();
		if (vk_swapchain.sc != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(vk_device.logical, vk_swapchain.sc, NULL);
			free(vk_swapchain.images);
			vk_swapchain.images = NULL;
			vk_swapchain.imageCount = 0;
		}
		for (int i = 0; i < NUM_CMDBUFFERS; ++i)
		{
			vkDestroySemaphore(vk_device.logical, vk_imageAvailableSemaphores[i], NULL);
			vkDestroySemaphore(vk_device.logical, vk_renderFinishedSemaphores[i], NULL);
			vkDestroyFence(vk_device.logical, vk_fences[i], NULL);
		}
		if (vk_malloc != VK_NULL_HANDLE)
			vmaDestroyAllocator(vk_malloc);
		if (vk_device.logical != VK_NULL_HANDLE)
			vkDestroyDevice(vk_device.logical, NULL);
		if(vk_surface != VK_NULL_HANDLE)
			vkDestroySurfaceKHR(vk_instance, vk_surface, NULL);
		QVk_DestroyValidationLayers();

		vkDestroyInstance(vk_instance, NULL);
		vk_instance = VK_NULL_HANDLE;
		vk_activeFramebuffer = VK_NULL_HANDLE;
		vk_activeCmdbuffer = VK_NULL_HANDLE;
		vk_activeBufferIdx = 0;
		vk_imageIndex = 0;
	}
}

#	pragma warning (disable : 4113 4133 4047 )

/*
** QVk_Init
**
** This is responsible for initializing Vulkan.
** 
*/
qboolean QVk_Init()
{
	VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "Quake 2",
		.applicationVersion = VK_MAKE_VERSION(3, 21, 0),
		.pEngineName = "id Tech 2",
		.engineVersion = VK_MAKE_VERSION(2, 0, 0),
		.apiVersion = VK_API_VERSION_1_1
	};

	uint32_t extCount;
	char **wantedExtensions;

	Vkimp_GetSurfaceExtensions(NULL, &extCount);

	if(vk_validation->value)
		extCount++;

	wantedExtensions = (char **)malloc(extCount * sizeof(const char *));
	Vkimp_GetSurfaceExtensions(wantedExtensions, NULL);

	if (vk_validation->value)
		wantedExtensions[extCount-1] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

	ri.Con_Printf(PRINT_ALL, "Vulkan extensions: ");
	for (int i = 0; i < extCount; i++)
	{
		ri.Con_Printf(PRINT_ALL, "%s ", wantedExtensions[i]);
	}
	ri.Con_Printf(PRINT_ALL, "\n");

	VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledExtensionCount = extCount,
		.enabledLayerCount = 0,
		.ppEnabledExtensionNames = wantedExtensions
	};

	if (vk_validation->value)
	{
		const char *validationLayers[] = { "VK_LAYER_LUNARG_standard_validation" };
		createInfo.enabledLayerCount = sizeof(validationLayers)/sizeof(validationLayers[0]);
		createInfo.ppEnabledLayerNames = validationLayers;
	}

	VkResult res = vkCreateInstance(&createInfo, NULL, &vk_instance);
	free(wantedExtensions);

	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan instance: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created Vulkan instance\n");

	// initialize function pointers
	qvkCreateDebugUtilsMessengerEXT  = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk_instance, "vkCreateDebugUtilsMessengerEXT");
	qvkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk_instance, "vkDestroyDebugUtilsMessengerEXT");

	if (vk_validation->value)
		QVk_CreateValidationLayers();

	res = Vkimp_CreateSurface();
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan surface: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created Vulkan surface\n");

	// create Vulkan device
	QVk_CreateDevice();
	// create memory allocator
	VmaAllocatorCreateInfo allocInfo = {
		.physicalDevice = vk_device.physical,
		.device = vk_device.logical
	};

	res = vmaCreateAllocator(&allocInfo, &vk_malloc);
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan memory allocator: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created Vulkan memory allocator\n");

	// setup swapchain
	res = QVk_CreateSwapchain();
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan swapchain: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created Vulkan swapchain\n");

	// set viewport and scissor
	vk_viewport.x = 0.f;
	vk_viewport.y = 0.f;
	vk_viewport.minDepth = 0.f;
	vk_viewport.maxDepth = 1.f;
	vk_viewport.width = (float)vk_swapchain.extent.width;
	vk_viewport.height = (float)vk_swapchain.extent.height;
	vk_scissor.offset.x = 0;
	vk_scissor.offset.y = 0;
	vk_scissor.extent = vk_swapchain.extent;

	// setup fences and semaphores
	VkFenceCreateInfo fCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT
	};
	VkSemaphoreCreateInfo sCreateInfo = { 
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO 
	};
	for (int i = 0; i < NUM_CMDBUFFERS; ++i)
	{
		VK_VERIFY(vkCreateFence(vk_device.logical, &fCreateInfo, NULL, &vk_fences[i]));
		VK_VERIFY(vkCreateSemaphore(vk_device.logical, &sCreateInfo, NULL, &vk_imageAvailableSemaphores[i]));
		VK_VERIFY(vkCreateSemaphore(vk_device.logical, &sCreateInfo, NULL, &vk_renderFinishedSemaphores[i]));
	}
	ri.Con_Printf(PRINT_ALL, "...created synchronization objects\n");

	// setup render passes
	res = QVk_CreateRenderpass(&vk_renderpasses[0]);
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan renderpass: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created Vulkan renderpass\n");
	res = QVk_CreateRenderpass(&vk_renderpasses[1]);
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan MSAA renderpass: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created Vulkan MSAA renderpass\n");

	// setup command pools
	res = QVk_CreateCommandPool(&vk_commandPool, vk_device.gfxFamilyIndex);
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan command pool for graphics: %s\n", QVk_GetError(res));
		return false;
	}
	res = QVk_CreateCommandPool(&vk_transferCommandPool, vk_device.transferFamilyIndex);
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan command pool for transfer: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created Vulkan command pools\n");

	// setup draw buffers
	CreateDrawBuffers();

	// setup image views
	res = CreateImageViews();
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan image views: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created %d Vulkan image view(s)\n", vk_swapchain.imageCount);

	// setup framebuffers
	res = CreateFramebuffers(&vk_renderpasses[RT_STANDARD], RT_STANDARD);
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan framebuffers: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created %d Vulkan framebuffers\n", vk_swapchain.imageCount);
	res = CreateFramebuffers(&vk_renderpasses[RT_MSAA], RT_MSAA);
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan MSAA framebuffers: %s\n", QVk_GetError(res));
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created %d Vulkan MSAA framebuffers\n", vk_swapchain.imageCount);

	// setup command buffers (double buffering)
	vk_commandbuffers = (VkCommandBuffer *)malloc(NUM_CMDBUFFERS * sizeof(VkCommandBuffer));

	VkCommandBufferAllocateInfo cbInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk_commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = NUM_CMDBUFFERS
	};

	res = vkAllocateCommandBuffers(vk_device.logical, &cbInfo, vk_commandbuffers);
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "QVk_Init(): Could not create Vulkan commandbuffers: %s\n", QVk_GetError(res));
		free(vk_commandbuffers);
		vk_commandbuffers = NULL;
		return false;
	}
	ri.Con_Printf(PRINT_ALL, "...created %d Vulkan commandbuffers\n", NUM_CMDBUFFERS);

	// initialize tracker variables
	vk_activeRenderpass = vk_renderpasses[RT_STANDARD];
	vk_activeFramebuffer = vk_framebuffers[RT_STANDARD];
	vk_activeCmdbuffer = vk_commandbuffers[vk_activeBufferIdx];

	// init console pipeline
	VkVertexInputBindingDescription bindingDesc = VK_INPUTBIND_DESC(sizeof(float) * 4);
	VkVertexInputAttributeDescription attributeDescriptions[] = {
		VK_INPUTATTR_DESC(0, VK_FORMAT_R32G32_SFLOAT, 0),
		VK_INPUTATTR_DESC(1, VK_FORMAT_R32G32_SFLOAT, sizeof(float)*2),
	};

	const float verts[] = { -1., -1., 0., 0.,
							 1.,  1., 1., 1.,
							-1.,  1., 0., 1.,
							 1., -1., 1., 0. };

	const uint32_t indices[] = { 0, 1, 2, 0, 3, 1 };

	QVk_CreateVertexBuffer(verts, sizeof(verts), &vertexBuffer, NULL);
	QVk_CreateIndexBuffer(indices, sizeof(indices), &indexBuffer, NULL);
	QVk_CreateUniformBuffer(consoleUboSize, &uniformBuffer);

	// descriptor set layout for the uniform and a sampler
	VkDescriptorSetLayoutBinding layoutBindings[] = {
		// UBO
		{
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			.pImmutableSamplers = NULL
		},
		// sampler
		{
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.pImmutableSamplers = NULL
		}
	};

	VkDescriptorSetLayoutCreateInfo layoutInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.bindingCount = sizeof(layoutBindings)/sizeof(layoutBindings[0]),
		.pBindings = layoutBindings
	};

	VK_VERIFY(vkCreateDescriptorSetLayout(vk_device.logical, &layoutInfo, NULL, &descriptorSetLayout));

	// create descriptor pool
	VkDescriptorPoolSize poolSizes[] = {
		// UBO
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = MAX_VKTEXTURES + 1
		},
		// sampler
		{
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = MAX_VKTEXTURES + 1
		}
	};

	VkDescriptorPoolCreateInfo poolInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.maxSets = MAX_VKTEXTURES + 32,
		.poolSizeCount = sizeof(poolSizes)/sizeof(poolSizes[0]),
		.pPoolSizes = poolSizes,
	};

	VK_VERIFY(vkCreateDescriptorPool(vk_device.logical, &poolInfo, NULL, &vk_descriptorPool));

	// create pipeline object
	VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.pNext = NULL,
		.flags = 0,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &bindingDesc,
		.vertexAttributeDescriptionCount = sizeof(attributeDescriptions)/sizeof(attributeDescriptions[0]),
		.pVertexAttributeDescriptions = attributeDescriptions
	};

	shaders[0] = QVk_CreateShader(basic_vert_spv, basic_vert_size, VK_SHADER_STAGE_VERTEX_BIT);
	shaders[1] = QVk_CreateShader(basic_frag_spv, basic_frag_size, VK_SHADER_STAGE_FRAGMENT_BIT);

	vk_console_pipeline.depthTestEnable = VK_FALSE;
	QVk_CreatePipeline(&descriptorSetLayout, 1, &vertexInputInfo, &vk_console_pipeline, shaders, 2);
	
	vkDestroyShaderModule(vk_device.logical, shaders[0].module, NULL);
	vkDestroyShaderModule(vk_device.logical, shaders[1].module, NULL);

	return true;
}

VkResult QVk_BeginFrame()
{
	VkResult result = vkAcquireNextImageKHR(vk_device.logical, vk_swapchain.sc, UINT32_MAX, vk_imageAvailableSemaphores[vk_activeBufferIdx], VK_NULL_HANDLE, &vk_imageIndex);
	vk_activeCmdbuffer = vk_commandbuffers[vk_activeBufferIdx];
	vk_activeFramebuffer = (vk_activeRenderpass.sampleCount == VK_SAMPLE_COUNT_1_BIT) ? vk_framebuffers[RT_STANDARD][vk_imageIndex] : vk_framebuffers[RT_MSAA][vk_imageIndex];

	// swapchain has become incompatible - need to recreate it
	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		ri.Con_Printf(PRINT_ALL, "Vulkan swapchain incompatible after vkAcquireNextImageKHR - rebuilding!\n");
		QVk_RecreateSwapchain();
		return result;
	}

	VK_VERIFY(vkWaitForFences(vk_device.logical, 1, &vk_fences[vk_activeBufferIdx], VK_TRUE, UINT32_MAX));
	VK_VERIFY(vkResetFences(vk_device.logical, 1, &vk_fences[vk_activeBufferIdx]));

	assert((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && "Could not acquire swapchain image!");

	// setup command buffers and render pass for drawing
	VkCommandBufferBeginInfo beginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		.pInheritanceInfo = NULL
	};

	VK_VERIFY(vkBeginCommandBuffer(vk_commandbuffers[vk_activeBufferIdx], &beginInfo));

	VkClearValue clearColors[2] = {
		{ .color = { .0f, .0f, .0f, 1.f } },
		{ .depthStencil = { 1.f, 0 } }
	};
	VkRenderPassBeginInfo renderBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = vk_activeRenderpass.rp,
		.framebuffer = vk_activeFramebuffer,
		.renderArea.offset = { 0, 0 },
		.renderArea.extent = vk_swapchain.extent,
		.clearValueCount = 2,
		.pClearValues = clearColors
	};

#if 0
	vkCmdBeginRenderPass(vk_commandbuffers[vk_activeBufferIdx], &renderBeginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
#else
	vkCmdBeginRenderPass(vk_commandbuffers[vk_activeBufferIdx], &renderBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdSetViewport(vk_commandbuffers[vk_activeBufferIdx], 0, 1, &vk_viewport);
	vkCmdSetScissor(vk_commandbuffers[vk_activeBufferIdx], 0, 1, &vk_scissor);
#endif

	return VK_SUCCESS;
}

VkResult QVk_EndFrame()
{
	// submit
	vkCmdEndRenderPass(vk_commandbuffers[vk_activeBufferIdx]);
	VK_VERIFY(vkEndCommandBuffer(vk_commandbuffers[vk_activeBufferIdx]));

	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &vk_imageAvailableSemaphores[vk_activeBufferIdx],
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &vk_renderFinishedSemaphores[vk_activeBufferIdx],
		.pWaitDstStageMask = waitStages,
		.commandBufferCount = 1,
		.pCommandBuffers = &vk_commandbuffers[vk_activeBufferIdx]
	};

	VK_VERIFY(vkQueueSubmit(vk_device.gfxQueue, 1, &submitInfo, vk_fences[vk_activeBufferIdx]));

	// present
	VkSwapchainKHR swapChains[] = { vk_swapchain.sc };
	VkPresentInfoKHR presentInfo = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &vk_renderFinishedSemaphores[vk_activeBufferIdx],
		.swapchainCount = 1,
		.pSwapchains = swapChains,
		.pImageIndices = &vk_imageIndex,
		.pResults = NULL
	};

	VkResult renderResult = vkQueuePresentKHR(vk_device.presentQueue, &presentInfo);

	// recreate swapchain if it's out of date
	if (renderResult == VK_ERROR_OUT_OF_DATE_KHR || renderResult == VK_SUBOPTIMAL_KHR)
	{
		ri.Con_Printf(PRINT_ALL, "Vulkan swapchain out of date/suboptimal after vkQueuePresentKHR - rebuilding!\n");
		QVk_RecreateSwapchain();
	}

	vk_activeBufferIdx = (vk_activeBufferIdx + 1) % NUM_CMDBUFFERS;

	return renderResult;
}

void QVk_RecreateSwapchain()
{
	vkDeviceWaitIdle( vk_device.logical );
	DestroyFramebuffers();
	DestroyImageViews();
	VK_VERIFY( QVk_CreateSwapchain() );
	vk_viewport.width = vk_swapchain.extent.width;
	vk_viewport.height = vk_swapchain.extent.height;
	vk_scissor.extent = vk_swapchain.extent;
	DestroyDrawBuffers();
	CreateDrawBuffers();
	VK_VERIFY( CreateImageViews() );
	VK_VERIFY( CreateFramebuffers( &vk_renderpasses[RT_STANDARD], RT_STANDARD ) );
	VK_VERIFY( CreateFramebuffers( &vk_renderpasses[RT_MSAA], RT_MSAA ) );
}

const char *QVk_GetError(VkResult errorCode)
{
#define ERRSTR(r) case VK_ ##r: return "VK_"#r
	switch (errorCode)
	{
		ERRSTR(NOT_READY);
		ERRSTR(TIMEOUT);
		ERRSTR(EVENT_SET);
		ERRSTR(EVENT_RESET);
		ERRSTR(INCOMPLETE);
		ERRSTR(ERROR_OUT_OF_HOST_MEMORY);
		ERRSTR(ERROR_OUT_OF_DEVICE_MEMORY);
		ERRSTR(ERROR_INITIALIZATION_FAILED);
		ERRSTR(ERROR_DEVICE_LOST);
		ERRSTR(ERROR_MEMORY_MAP_FAILED);
		ERRSTR(ERROR_LAYER_NOT_PRESENT);
		ERRSTR(ERROR_EXTENSION_NOT_PRESENT);
		ERRSTR(ERROR_FEATURE_NOT_PRESENT);
		ERRSTR(ERROR_INCOMPATIBLE_DRIVER);
		ERRSTR(ERROR_TOO_MANY_OBJECTS);
		ERRSTR(ERROR_FORMAT_NOT_SUPPORTED);
		ERRSTR(ERROR_SURFACE_LOST_KHR);
		ERRSTR(ERROR_NATIVE_WINDOW_IN_USE_KHR);
		ERRSTR(SUBOPTIMAL_KHR);
		ERRSTR(ERROR_OUT_OF_DATE_KHR);
		ERRSTR(ERROR_INCOMPATIBLE_DISPLAY_KHR);
		ERRSTR(ERROR_VALIDATION_FAILED_EXT);
		ERRSTR(ERROR_INVALID_SHADER_NV);
	}
#undef ERRSTR
	return "UNKNOWN ERROR";
}

void Vkimp_EnableLogging(qboolean enable)
{
	if (enable)
	{
		if (!vkw_state.log_fp)
		{
			struct tm *newtime;
			time_t aclock;
			char buffer[1024];

			time(&aclock);
			newtime = localtime(&aclock);

			asctime(newtime);

			Com_sprintf(buffer, sizeof(buffer), "%s/vk.log", ri.FS_Gamedir());
			vkw_state.log_fp = fopen(buffer, "wt");

			fprintf(vkw_state.log_fp, "%s\n", asctime(newtime));
		}
		vk_logfp = vkw_state.log_fp;
	}
	else
	{
		vk_logfp = NULL;
	}
}

void Vkimp_LogNewFrame( void )
{
	fprintf( vkw_state.log_fp, "*** R_BeginFrame ***\n" );
}

#pragma warning (default : 4113 4133 4047 )



