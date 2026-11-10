// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// The smallest RADV title: an instance and a device on RADV (linked into the eboot, no loader), a surface on
// VideoOut's display (VK_KHR_display), a swapchain, and frames cleared to a colour and presented. It proves the link
// of RADV + ps5-native-runtime-mit + its platform layer, with no GPL input. What each step returned goes to
// /data/ps5rt-radv-title.txt (or the path in argv[1]), so a run on a console says how far it got.
//
// Nothing here has run on a PS5 yet: this build is only linked and checked on a PC.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "ps5rt/platform.h"

// RADV's PS5 winsys (radv_ps5_winsys_public.h): the direct-memory pool, what it has left, and what the driver holds
void radv_ps5_winsys_heap_usage(uint64_t *pool_bytes, uint64_t *available_bytes, uint64_t *driver_bytes);

static void say(const char *fmt, ...);

static void log_memory(const char *when)
{
	uint64_t pool = 0, available = 0, driver = 0;
	radv_ps5_winsys_heap_usage(&pool, &available, &driver);
	say("mem  %s: direct memory pool %llu MiB, %llu MiB left, RADV holds %llu MiB", when,
		(unsigned long long)(pool >> 20), (unsigned long long)(available >> 20), (unsigned long long)(driver >> 20));
}

#define FRAMES 180

static FILE *g_log;

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (g_log) {
		vfprintf(g_log, fmt, ap);
		fputc('\n', g_log);
		fflush(g_log);
	}
	va_end(ap);
}

#define CHECK(call)                                                                                                   \
	do {                                                                                                              \
		VkResult r_ = (call);                                                                                         \
		if (r_ != VK_SUCCESS) {                                                                                       \
			say("FAIL %s: VkResult %d", #call, (int)r_);                                                              \
			return 1;                                                                                                 \
		}                                                                                                             \
		say("ok   %s", #call);                                                                                        \
	} while (0)

// RADV is the only driver, linked in: its ICD entry point gives every other function
static PFN_vkGetInstanceProcAddr gipa;
#define IFN(inst, name) PFN_##name name = (PFN_##name)gipa((inst), #name)
#define DFN(dev, name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr_((dev), #name)

static int run(void)
{
	gipa = (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr(NULL, "vkGetInstanceProcAddr");
	if (!gipa) {
		say("FAIL vk_icdGetInstanceProcAddr(vkGetInstanceProcAddr)");
		return 1;
	}
	IFN(NULL, vkCreateInstance);
	const char *inst_ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "ps5rt-radv-title",
		.apiVersion = VK_API_VERSION_1_3,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = inst_ext,
	};
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));

	IFN(inst, vkEnumeratePhysicalDevices);
	IFN(inst, vkGetPhysicalDeviceProperties);
	IFN(inst, vkGetPhysicalDeviceQueueFamilyProperties);
	IFN(inst, vkGetPhysicalDeviceDisplayPropertiesKHR);
	IFN(inst, vkGetDisplayModePropertiesKHR);
	IFN(inst, vkCreateDisplayPlaneSurfaceKHR);
	IFN(inst, vkGetPhysicalDeviceSurfaceSupportKHR);
	IFN(inst, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
	IFN(inst, vkGetPhysicalDeviceSurfaceFormatsKHR);
	IFN(inst, vkCreateDevice);
	IFN(inst, vkGetDeviceProcAddr);
	IFN(inst, vkDestroySurfaceKHR);
	IFN(inst, vkDestroyInstance);
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr_ = vkGetDeviceProcAddr;

	uint32_t n = 1;
	VkPhysicalDevice phys;
	VkResult er = vkEnumeratePhysicalDevices(inst, &n, &phys);
	if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) {
		say("FAIL vkEnumeratePhysicalDevices: VkResult %d, %u devices", (int)er, n);
		return 1;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(phys, &props);
	say("ok   device: %s, Vulkan %u.%u.%u, driver 0x%x", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
		VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);

	// VideoOut's display, its first (largest) mode, its one plane
	VkDisplayPropertiesKHR disp;
	n = 1;
	er = vkGetPhysicalDeviceDisplayPropertiesKHR(phys, &n, &disp);
	if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) {
		say("FAIL vkGetPhysicalDeviceDisplayPropertiesKHR: VkResult %d, %u displays", (int)er, n);
		return 1;
	}
	VkDisplayModePropertiesKHR mode;
	n = 1;
	er = vkGetDisplayModePropertiesKHR(phys, disp.display, &n, &mode);
	if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) {
		say("FAIL vkGetDisplayModePropertiesKHR: VkResult %d, %u modes", (int)er, n);
		return 1;
	}
	say("ok   display %s, mode %ux%u at %u mHz", disp.displayName ? disp.displayName : "(unnamed)",
		mode.parameters.visibleRegion.width, mode.parameters.visibleRegion.height, mode.parameters.refreshRate);
	VkDisplaySurfaceCreateInfoKHR dsci = {
		.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
		.displayMode = mode.displayMode,
		.planeIndex = 0,
		.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.globalAlpha = 1.0f,
		.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
		.imageExtent = mode.parameters.visibleRegion,
	};
	VkSurfaceKHR surface;
	CHECK(vkCreateDisplayPlaneSurfaceKHR(inst, &dsci, NULL, &surface));

	// a queue that draws and presents
	VkQueueFamilyProperties qf[8];
	n = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qf);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < n && family == UINT32_MAX; i++) {
		VkBool32 present = VK_FALSE;
		if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
			vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present) == VK_SUCCESS && present)
			family = i;
	}
	if (family == UINT32_MAX) {
		say("FAIL no graphics queue family that presents to the display");
		return 1;
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	VkPhysicalDeviceVulkan13Features f13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.dynamicRendering = VK_TRUE,
	};
	const char *dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &f13,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = dev_ext,
	};
	VkDevice dev;
	CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
	log_memory("after vkCreateDevice");
	DFN(dev, vkGetDeviceQueue);
	DFN(dev, vkCreateSwapchainKHR);
	DFN(dev, vkGetSwapchainImagesKHR);
	DFN(dev, vkAcquireNextImageKHR);
	DFN(dev, vkQueuePresentKHR);
	DFN(dev, vkCreateImageView);
	DFN(dev, vkCreateCommandPool);
	DFN(dev, vkAllocateCommandBuffers);
	DFN(dev, vkBeginCommandBuffer);
	DFN(dev, vkEndCommandBuffer);
	DFN(dev, vkResetCommandBuffer);
	DFN(dev, vkCmdPipelineBarrier);
	DFN(dev, vkCmdBeginRendering);
	DFN(dev, vkCmdEndRendering);
	DFN(dev, vkCreateSemaphore);
	DFN(dev, vkCreateFence);
	DFN(dev, vkWaitForFences);
	DFN(dev, vkResetFences);
	DFN(dev, vkQueueSubmit);
	DFN(dev, vkDeviceWaitIdle);
	DFN(dev, vkDestroySemaphore);
	DFN(dev, vkDestroyFence);
	DFN(dev, vkDestroyCommandPool);
	DFN(dev, vkDestroyImageView);
	DFN(dev, vkDestroySwapchainKHR);
	DFN(dev, vkDestroyDevice);
	VkQueue queue;
	vkGetDeviceQueue(dev, family, 0, &queue);

	// the swapchain: the surface's first format, FIFO (the only mode VideoOut presents with)
	VkSurfaceCapabilitiesKHR caps;
	CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
	VkSurfaceFormatKHR fmt;
	n = 1;
	er = vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, &fmt);
	if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) {
		say("FAIL vkGetPhysicalDeviceSurfaceFormatsKHR: VkResult %d, %u formats", (int)er, n);
		return 1;
	}
	uint32_t want = caps.minImageCount + 1;
	if (caps.maxImageCount && want > caps.maxImageCount)
		want = caps.maxImageCount;
	VkSwapchainCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = surface,
		.minImageCount = want,
		.imageFormat = fmt.format,
		.imageColorSpace = fmt.colorSpace,
		.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : mode.parameters.visibleRegion,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = caps.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};
	VkSwapchainKHR swapchain;
	CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &swapchain));
	VkImage images[8];
	uint32_t count = 8;
	CHECK(vkGetSwapchainImagesKHR(dev, swapchain, &count, images));
	VkImageView views[8];
	for (uint32_t i = 0; i < count; i++) {
		VkImageViewCreateInfo vci = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = fmt.format,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		CHECK(vkCreateImageView(dev, &vci, NULL, &views[i]));
	}
	say("ok   swapchain: %u images of %ux%u, format %d", count, sci.imageExtent.width, sci.imageExtent.height,
		(int)fmt.format);

	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = family,
	};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
	VkSemaphoreCreateInfo semci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
	VkSemaphore acquired, rendered;
	VkFence done;
	CHECK(vkCreateSemaphore(dev, &semci, NULL, &acquired));
	CHECK(vkCreateSemaphore(dev, &semci, NULL, &rendered));
	CHECK(vkCreateFence(dev, &fci, NULL, &done));

	// frames: each image cleared (dynamic rendering, load op CLEAR) to a colour that cycles, then presented
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int frame = 0; frame < FRAMES; frame++) {
		VkResult r = vkWaitForFences(dev, 1, &done, VK_TRUE, UINT64_MAX);
		if (r == VK_SUCCESS)
			r = vkResetFences(dev, 1, &done);
		uint32_t index = 0;
		if (r == VK_SUCCESS)
			r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
			say("FAIL frame %d: wait / acquire: VkResult %d", frame, (int)r);
			return 1;
		}
		VkCommandBufferBeginInfo bi = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkResetCommandBuffer(cmd, 0);
		vkBeginCommandBuffer(cmd, &bi);
		VkImageMemoryBarrier to_draw = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = images[index],
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &to_draw);
		float phase = (float)(frame % 60) / 60.0f;
		VkRenderingAttachmentInfo color = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = views[index],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue.color.float32 = {0.1f, 0.2f + 0.6f * phase, 0.8f - 0.6f * phase, 1.0f},
		};
		VkRenderingInfo ri = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = {{0, 0}, sci.imageExtent},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color,
		};
		vkCmdBeginRendering(cmd, &ri);
		vkCmdEndRendering(cmd);
		VkImageMemoryBarrier to_present = to_draw;
		to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		to_present.dstAccessMask = 0;
		to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, NULL, 0, NULL, 1, &to_present);
		vkEndCommandBuffer(cmd);
		VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo si = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &acquired,
			.pWaitDstStageMask = &wait_stage,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &rendered,
		};
		r = vkQueueSubmit(queue, 1, &si, done);
		VkPresentInfoKHR pi = {
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &rendered,
			.swapchainCount = 1,
			.pSwapchains = &swapchain,
			.pImageIndices = &index,
		};
		if (r == VK_SUCCESS)
			r = vkQueuePresentKHR(queue, &pi);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
			say("FAIL frame %d: submit / present: VkResult %d", frame, (int)r);
			return 1;
		}
		if (frame == 0)
			say("ok   first frame cleared and presented");
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
	say("ok   %d frames in %.2f s (%.1f fps)", FRAMES, secs, secs > 0 ? FRAMES / secs : 0.0);
	log_memory("after the frames");

	CHECK(vkDeviceWaitIdle(dev));
	vkDestroyFence(dev, done, NULL);
	vkDestroySemaphore(dev, rendered, NULL);
	vkDestroySemaphore(dev, acquired, NULL);
	vkDestroyCommandPool(dev, pool, NULL);
	for (uint32_t i = 0; i < count; i++)
		vkDestroyImageView(dev, views[i], NULL);
	vkDestroySwapchainKHR(dev, swapchain, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroySurfaceKHR(inst, surface, NULL);
	vkDestroyInstance(inst, NULL);
	say("ok   everything destroyed");
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 && argv[1] && argv[1][0] ? argv[1] : "/data/ps5rt-radv-title.txt";
	g_log = fopen(path, "w");
	say("ps5rt-radv-title: RADV + ps5-native-runtime-mit + its platform layer (MIT)");
	say("info threads made without attributes get %zu KiB stacks", ps5rt_thread_stack_size() >> 10);
	int rc = run();
	say(rc == 0 ? "PASS" : "FAILED");
	if (g_log)
		fclose(g_log);
	return rc;
}
