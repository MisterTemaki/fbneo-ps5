// ps5-slang-gpu: the GPU renderer (ps5gpu/gpu.h).
//
// Vulkan 1.0 only, so that RADV on the PS5 and any desktop driver (lavapipe in the host tests) run the same code:
//   - on the PS5: VK_KHR_display (PS5_Mesa's VideoOut WSI): the display's 60 Hz mode, a plane surface, a FIFO
//     swapchain; the entry points come from the linked driver (vk_icdGetInstanceProcAddr), no loader;
//   - on the host: an off-screen target, read back after each frame and handed to Config::host_present.
// One frame is in flight: Present waits for the previous frame's fence before it touches the staging buffers.
//
// The slang runtime follows RetroArch's spec (docs: libretro's slang_shaders.md): each pass renders into its own
// image (8-bit, sRGB or 16-bit float), sized from the source, the viewport or absolutely; the last pass renders
// into the game's rectangle on the target. Textures by name (Original, Source, OriginalHistory#, PassOutput#,
// PassFeedback#, aliases, LUTs) and the uniform semantics (MVP, OutputSize, SourceSize, OriginalSize,
// FinalViewportSize, FrameCount, FrameDirection, Rotation, <texture>Size, the parameters) are bound from the
// reflection tools/slang_compile.py (or src/slang_loader.cpp) made.
//
// Taken from Genesis Plus GX PS5's ProsperoGpu.cpp (rs1 95cf4b4, our own MIT code), made generic: the frame comes in
// any of four pixel formats with a pitch, a crop and a rotation; paths, log and host output are the program's Config.
//
// SPDX-License-Identifier: MIT

#include "ps5gpu/gpu.h"
#include "internal.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <map>
#include <mutex>
#include <new>
#include <chrono>
#include <string>
#include <vector>

// the presets' look-up textures are PNGs: a private copy of stb_image's PNG decoder (static: a program's own copy of
// stb_image can't clash with it)
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

#ifdef __PROSPERO__
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
#define GPU_GET_INSTANCE_PROC vk_icdGetInstanceProcAddr
#else
extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name);
#define GPU_GET_INSTANCE_PROC vkGetInstanceProcAddr
#endif

namespace ps5gpu
{
namespace
{
// ---- entry points ------------------------------------------------------------------------------------------
#define GPU_GLOBAL_FNS(X) X(CreateInstance) X(EnumerateInstanceExtensionProperties)
#define GPU_INSTANCE_FNS(X)                                                                                          \
	X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties)                                      \
	X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(CreateDevice)                     \
	X(GetDeviceProcAddr) X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceFormatProperties)
#define GPU_DISPLAY_FNS(X)                                                                                           \
	X(GetPhysicalDeviceDisplayPropertiesKHR) X(GetDisplayModePropertiesKHR) X(CreateDisplayPlaneSurfaceKHR)            \
	X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR)              \
	X(GetPhysicalDeviceSurfaceFormatsKHR)
#define GPU_DEVICE_FNS(X)                                                                                            \
	X(DestroyDevice) X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)             \
	X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle)  \
	X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore)              \
	X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateImage) X(DestroyImage) \
	X(GetImageMemoryRequirements) X(BindImageMemory) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)       \
	X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) X(CreateRenderPass)                      \
	X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateShaderModule) X(DestroyShaderModule)        \
	X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout)        \
	X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool)                     \
	X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdPipelineBarrier) X(CmdCopyBufferToImage)                   \
	X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline)               \
	X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor)           \
	X(CmdDraw) X(CmdClearColorImage) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool)                       \
	X(CmdWriteTimestamp) X(GetQueryPoolResults) X(FreeDescriptorSets) X(FreeCommandBuffers) X(CreatePipelineCache)          \
	X(DestroyPipelineCache) X(GetPipelineCacheData) X(CmdBlitImage) X(GetFenceStatus)
// 1.1 (Config::video): the host-pointer import's own call (VK_EXT_external_memory_host; null without it)
#define GPU_VIDEO_DEVICE_FNS(X) X(GetMemoryHostPointerPropertiesEXT)
#define GPU_SWAPCHAIN_FNS(X)                                                                                         \
	X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR)

#define GPU_DECLARE(name) PFN_vk##name vk##name = nullptr;
GPU_GLOBAL_FNS(GPU_DECLARE)
GPU_INSTANCE_FNS(GPU_DECLARE)
GPU_DISPLAY_FNS(GPU_DECLARE)
GPU_DEVICE_FNS(GPU_DECLARE)
GPU_SWAPCHAIN_FNS(GPU_DECLARE)
GPU_VIDEO_DEVICE_FNS(GPU_DECLARE)
// (Config::video) VK_KHR_get_physical_device_properties2's, on a Vulkan 1.0 instance -- never a 1.1 instance: the PS5's
// RADV took one and crashed in the physical-device step (MPC 1.6 test2 / test10), the ports' 1.0 instances never did
PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
#undef GPU_DECLARE

// ---- resources ---------------------------------------------------------------------------------------------
struct Buffer
{
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	void* map = nullptr;
	VkDeviceSize size = 0;
};

struct Image
{
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	int w = 0, h = 0;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkFramebuffer fb = VK_NULL_HANDLE; // when it is a pass's render target
};

struct PassRt
{
	const ps5slang::Pass* def = nullptr;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkRenderPass render_pass = VK_NULL_HANDLE; // its own (intermediate passes); the last pass uses the final one
	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	Image out[2]; // out[cur]: this frame's output; out[cur ^ 1]: last frame's (PassFeedback)
	int cur = 0;
	bool feedback = false;
	Buffer ubo;
	VkDescriptorSet set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	int out_w = 0, out_h = 0;
};

struct Chain
{
	int preset = -1;
	std::vector<PassRt> passes;
	std::vector<Image> luts;
	std::vector<float> params; // the preset's parameter values
	int history = 0; // OriginalHistory# used (the deepest)
	std::vector<Image> hist; // hist[0] = OriginalHistory1 ...
	bool built = false;
};

struct State
{
	bool ok = false;
	bool offscreen = false;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memprops = {};
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	char device_name[256] = {};
	float ts_period = 0;
	bool timestamps = false;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	bool in_flight = false;
	bool frame_drew_game = false; // 1.1 review: this Present recorded the game's chain (FrameSubmitFailed)
	bool deferred_host_present = false; // host tests: PS5GPU_TEST_OFFSCREEN_IN_FLIGHT (the read-back shown a Present late)
	VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
	VkQueryPool queries = VK_NULL_HANDLE;
	bool queries_written = false;
	// the target
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat target_format = VK_FORMAT_B8G8R8A8_UNORM;
	std::vector<VkImage> swap_images;
	std::vector<VkImageView> swap_views;
	std::vector<VkFramebuffer> swap_fbs;
	Image offscreen_target;
	Buffer readback;
	int out_w = 0, out_h = 0;
	VkRenderPass final_pass = VK_NULL_HANDLE;
	// the surface and the game frame
	int ui_w = 0, ui_h = 0;
	Image ui;
	Buffer ui_staging;
	bool ui_full = true;
	Image original;
	Buffer game_staging;
	Image dummy;
	// draw state
	Buffer vertices;
	VkSampler samplers[2][4] = {};
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	Chain ui_chain;
	// MPC HDR extension: render PQ into a real 10:10:10:2 image, then copy its bits to VideoOut's retyped buffers.
	Image hdr_target;
	VkRenderPass hdr_pass = VK_NULL_HANDLE;
	VkPipeline hdr_video_pipe = VK_NULL_HANDLE;
	Chain hdr_ui_chain;
	bool hdr_active = false, hdr_wish = false, hdr_refused = false, hdr_readback = false, hdr_broken = false;
	double hdr_wish_since = 0;
	int hdr_check_frames = 0;
	std::vector<uint8_t> hdr_host_pixels;
	Chain game_chain;
	// shaders built off the frame loop: the old one stays on screen meanwhile
	VkPipelineCache pcache = VK_NULL_HANDLE; // saved to <root>/cache/gpu-pipelines.bin: a shader built once loads fast
	std::mutex queue_mx; // the queue: the frame's submit and present, the builder's uploads
	std::mutex upload_mx; // the upload command pool
	std::mutex dpool_mx; // the descriptor pool
	VkCommandPool upload_pool = VK_NULL_HANDLE;
	size_t pcache_saved = 0; // the size of the cache file last written (or read)
	internal::Thread builder; // a big stack: RADV's shader compiler (ACO) recurses deep, and the SDK's default stack is small
	std::atomic<bool> building{false};
	std::atomic<bool> built_ready{false};
	int building_preset = -1;
	Chain* built = nullptr; // the builder's result (nullptr: it failed)
	// the shader parameters set in the menu: preset name -> parameter -> value
	std::mutex param_mx; // param_overrides: the menu writes, the builder reads
	std::map<std::string, std::map<std::string, float>> param_overrides;
	// the shelf's scene (SetScene): its quad pipeline, its textures (mip-mapped), the quads
	VkDescriptorSetLayout scene_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout scene_layout = VK_NULL_HANDLE;
	VkPipeline scene_pipe = VK_NULL_HANDLE;
	VkSampler scene_sampler = VK_NULL_HANDLE;
	VkDescriptorSet scene_dummy_set = VK_NULL_HANDLE;
	struct SceneTex
	{
		VkImage img = VK_NULL_HANDLE;
		VkDeviceMemory mem = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		uint64_t used = 0; // the present it was last drawn in
		size_t bytes = 0;
	};
	std::map<uint64_t, SceneTex> scene_tex;
	std::vector<SceneQuad> scene;
	Buffer scene_vb; // the quads' vertices (written each frame, after the last one's fence)
	bool scene_on = false;
	uint64_t presents = 0;
	// the next frame's game picture
	bool game = false;
	int presents_without_game = 0; // the shelf draws no game: after a moment the GPU stops drawing the last one
	bool game_new = false;
	int game_w = 0, game_h = 0;
	Rect game_dst = {0, 0, 0, 0};
	int shader = 0;
	bool smooth = false, scanlines = false;
	int rotation = 0; // the last pass turns the picture (Rotation: counter-clockwise quarter turns)
	bool ui_straight = false; // the UI surface's alpha is straight, not premultiplied
	VkFormat ui_format = VK_FORMAT_R8G8B8A8_UNORM;
	uint64_t frame_count = 0;
	double gpu_ms = 0;
	int gpu_frames = 0;
};
State g;

const float kMvp[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1}; // [0,1] -> [-1,1], column-major

// kMvp, then turned `quarters` quarter turns counter-clockwise on screen (y points down in Vulkan's clip space):
// x' = x cos t + y sin t, y' = -x sin t + y cos t
void RotatedMvp(int quarters, float* m)
{
	static const int kCos[4] = {1, 0, -1, 0}, kSin[4] = {0, 1, 0, -1};
	const float c = float(kCos[quarters & 3]), sn = float(kSin[quarters & 3]);
	for (int i = 0; i < 16; i++)
		m[i] = 0;
	m[0] = 2 * c;
	m[1] = -2 * sn;
	m[4] = 2 * sn;
	m[5] = 2 * c;
	m[10] = 1;
	m[12] = -(c + sn);
	m[13] = sn - c;
	m[15] = 1;
}

bool Check(VkResult r, const char* what)
{
	if (r != VK_SUCCESS)
	{
		internal::Log("[gpu] %s -> %d", what, int(r));
		return false;
	}
	return true;
}

uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice = 0)
{
	for (int pass = 0; pass < 2; pass++)
		for (uint32_t i = 0; i < g.memprops.memoryTypeCount; i++)
		{
			const VkMemoryPropertyFlags f = g.memprops.memoryTypes[i].propertyFlags;
			if ((bits & (1u << i)) && (f & want) == want && (pass == 1 || (f & nice) == nice))
				return i;
		}
	return UINT32_MAX;
}

bool MakeBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = usage;
	if (!Check(vkCreateBuffer(g.device, &bi, nullptr, &b.buf), "vkCreateBuffer"))
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(g.device, b.buf, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = MemoryType(req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX || !Check(vkAllocateMemory(g.device, &ai, nullptr, &b.mem), "vkAllocateMemory"))
		return false;
	vkBindBufferMemory(g.device, b.buf, b.mem, 0);
	if (!Check(vkMapMemory(g.device, b.mem, 0, size, 0, &b.map), "vkMapMemory"))
		return false;
	b.size = size;
	return true;
}

void FreeBuffer(Buffer& b)
{
	if (b.buf)
		vkDestroyBuffer(g.device, b.buf, nullptr);
	if (b.mem)
		vkFreeMemory(g.device, b.mem, nullptr);
	b = Buffer();
}

bool MakeImage(Image& im, int w, int h, VkFormat format, VkImageUsageFlags usage)
{
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = {uint32_t(w), uint32_t(h), 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!Check(vkCreateImage(g.device, &ii, nullptr, &im.img), "vkCreateImage"))
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g.device, im.img, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX)
		ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, 0);
	if (!Check(vkAllocateMemory(g.device, &ai, nullptr, &im.mem), "vkAllocateMemory (image)"))
		return false;
	vkBindImageMemory(g.device, im.img, im.mem, 0);
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = im.img;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (!Check(vkCreateImageView(g.device, &vi, nullptr, &im.view), "vkCreateImageView"))
		return false;
	im.format = format;
	im.w = w;
	im.h = h;
	im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

void FreeImage(Image& im)
{
	if (im.fb)
		vkDestroyFramebuffer(g.device, im.fb, nullptr);
	if (im.view)
		vkDestroyImageView(g.device, im.view, nullptr);
	if (im.img)
		vkDestroyImage(g.device, im.img, nullptr);
	if (im.mem)
		vkFreeMemory(g.device, im.mem, nullptr);
	im = Image();
}

void Barrier(VkCommandBuffer c, Image& im, VkImageLayout to)
{
	if (im.layout == to)
		return;
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = im.layout;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = im.img;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
		nullptr, 1, &b);
	im.layout = to;
}

VkRenderPass MakeRenderPass(VkFormat format, VkImageLayout final_layout, bool clear)
{
	VkAttachmentDescription a = {};
	a.format = format;
	a.samples = VK_SAMPLE_COUNT_1_BIT;
	a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	a.finalLayout = final_layout;
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sp = {};
	sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sp.colorAttachmentCount = 1;
	sp.pColorAttachments = &ref;
	VkSubpassDependency dep = {};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	dep.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	dep.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	VkRenderPassCreateInfo ri = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	ri.attachmentCount = 1;
	ri.pAttachments = &a;
	ri.subpassCount = 1;
	ri.pSubpasses = &sp;
	ri.dependencyCount = 1;
	ri.pDependencies = &dep;
	VkRenderPass rp = VK_NULL_HANDLE;
	Check(vkCreateRenderPass(g.device, &ri, nullptr, &rp), "vkCreateRenderPass");
	return rp;
}

VkFramebuffer MakeFramebuffer(VkRenderPass rp, VkImageView view, int w, int h)
{
	VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fi.renderPass = rp;
	fi.attachmentCount = 1;
	fi.pAttachments = &view;
	fi.width = uint32_t(w);
	fi.height = uint32_t(h);
	fi.layers = 1;
	VkFramebuffer fb = VK_NULL_HANDLE;
	Check(vkCreateFramebuffer(g.device, &fi, nullptr, &fb), "vkCreateFramebuffer");
	return fb;
}

VkShaderModule Module(const uint32_t* code, unsigned bytes)
{
	VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	si.codeSize = bytes;
	si.pCode = code;
	VkShaderModule m = VK_NULL_HANDLE;
	Check(vkCreateShaderModule(g.device, &si, nullptr, &m), "vkCreateShaderModule");
	return m;
}

VkFormat PassFormat(int fb)
{
	return fb == 2 ? VK_FORMAT_R16G16B16A16_SFLOAT : fb == 1 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

// ---- a chain: its passes' pipelines ------------------------------------------------------------------------
bool BuildPass(PassRt& p, VkRenderPass rp, bool blend)
{
	const ps5slang::Pass& d = *p.def;
	std::vector<VkDescriptorSetLayoutBinding> binds;
	if (d.ubo_binding >= 0)
		binds.push_back({uint32_t(d.ubo_binding), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	for (int i = 0; i < d.sampler_count; i++)
		binds.push_back({uint32_t(d.samplers[i].binding), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = uint32_t(binds.size());
	li.pBindings = binds.data();
	if (!Check(vkCreateDescriptorSetLayout(g.device, &li, nullptr, &p.set_layout), "vkCreateDescriptorSetLayout"))
		return false;
	VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, uint32_t(d.push_size)};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &p.set_layout;
	pli.pushConstantRangeCount = d.push_size > 0 ? 1 : 0;
	pli.pPushConstantRanges = &pc;
	if (!Check(vkCreatePipelineLayout(g.device, &pli, nullptr, &p.layout), "vkCreatePipelineLayout"))
		return false;

	VkShaderModule vs = Module(d.vs, d.vs_bytes), fs = Module(d.fs, d.fs_bytes);
	if (!vs || !fs)
		return false;
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = vs;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = fs;
	st[1].pName = "main";
	VkVertexInputBindingDescription vb = {0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
		{1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)}};
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vb;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = va;
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba = {};
	ba.colorWriteMask = 0xf;
	if (blend)
	{
		// the surface over the game: premultiplied alpha (or straight, Config::ui_premultiplied)
		ba.blendEnable = VK_TRUE;
		ba.srcColorBlendFactor = g.ui_straight ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
		ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ba.colorBlendOp = VK_BLEND_OP_ADD;
		ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ba.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.stageCount = 2;
	pi.pStages = st;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &ds;
	pi.layout = p.layout;
	pi.renderPass = rp;
	const bool ok = Check(vkCreateGraphicsPipelines(g.device, g.pcache, 1, &pi, nullptr, &p.pipeline),
		"vkCreateGraphicsPipelines");
	vkDestroyShaderModule(g.device, vs, nullptr);
	vkDestroyShaderModule(g.device, fs, nullptr);
	if (!ok)
		return false;
	if (d.ubo_binding >= 0 && !MakeBuffer(p.ubo, std::max(d.ubo_size, 16), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
		return false;
	for (int k = 0; k < 2; k++)
	{
		VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = g.dpool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &p.set_layout;
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		if (!Check(vkAllocateDescriptorSets(g.device, &ai, &p.set[k]), "vkAllocateDescriptorSets"))
			return false;
	}
	return true;
}

void FreeChain(Chain& c)
{
	for (PassRt& p : c.passes)
	{
		if (p.pipeline)
			vkDestroyPipeline(g.device, p.pipeline, nullptr);
		if (p.layout)
			vkDestroyPipelineLayout(g.device, p.layout, nullptr);
		if (p.set_layout)
			vkDestroyDescriptorSetLayout(g.device, p.set_layout, nullptr);
		if (p.render_pass)
			vkDestroyRenderPass(g.device, p.render_pass, nullptr);
		{
			std::lock_guard<std::mutex> lock(g.dpool_mx);
			for (VkDescriptorSet& set : p.set)
				if (set)
					vkFreeDescriptorSets(g.device, g.dpool, 1, &set);
		}
		FreeImage(p.out[0]);
		FreeImage(p.out[1]);
		FreeBuffer(p.ubo);
	}
	for (Image& im : c.luts)
		FreeImage(im);
	for (Image& im : c.hist)
		FreeImage(im);
	c = Chain();
}

bool UploadImage(Image& im, const uint8_t* rgba, int w, int h);

// the values set in the menu (Shader parameters) over the preset's own; the builder thread calls this too
void ApplyOverrides(Chain& c)
{
	if (c.preset < 0 || c.preset >= ps5slang::PresetCount())
		return;
	const ps5slang::Preset& pr = ps5slang::PresetAt(c.preset);
	std::lock_guard<std::mutex> lk(g.param_mx);
	const auto ov = g.param_overrides.find(pr.name);
	for (int i = 0; i < pr.param_count && size_t(i) < c.params.size(); i++)
	{
		float v = pr.params[i].value;
		if (ov != g.param_overrides.end())
		{
			const auto it = ov->second.find(pr.params[i].name);
			if (it != ov->second.end())
				v = it->second;
		}
		c.params[size_t(i)] = v;
	}
}

bool BuildChain(Chain& c, int preset, bool ui, VkRenderPass output_pass = VK_NULL_HANDLE)
{
	FreeChain(c);
	if (preset < 0 || preset >= ps5slang::PresetCount())
		return false;
	// a preset of the user's own: compiled the first time (here, on the build thread)
	if (ps5slang::IsExternal(preset) && !ps5slang::EnsureCompiled(preset, nullptr))
		return false;
	const ps5slang::Preset& pr = ps5slang::PresetAt(preset);
	c.preset = preset;
	c.passes.resize(size_t(pr.pass_count));
	for (int i = 0; i < pr.param_count; i++)
		c.params.push_back(pr.params[i].value);
	// what the passes read: feedback and history
	for (int i = 0; i < pr.pass_count; i++)
	{
		const ps5slang::Pass& d = pr.passes[i];
		for (int s = 0; s < d.sampler_count; s++)
		{
			const std::string n = d.samplers[s].name;
			if (n.compare(0, 15, "OriginalHistory") == 0)
				c.history = std::max(c.history, atoi(n.c_str() + 15));
			if (n.compare(0, 12, "PassFeedback") == 0)
			{
				const int k = atoi(n.c_str() + 12);
				if (k >= 0 && k < pr.pass_count)
					c.passes[size_t(k)].feedback = true;
			}
			for (int j = 0; j < pr.pass_count; j++)
				if (pr.passes[j].alias[0] && n == std::string(pr.passes[j].alias) + "Feedback")
					c.passes[size_t(j)].feedback = true;
		}
	}
	for (int i = 0; i < pr.pass_count; i++)
	{
		PassRt& p = c.passes[size_t(i)];
		p.def = &pr.passes[i];
		const bool last = i == pr.pass_count - 1;
		p.format = last ? g.target_format : PassFormat(p.def->framebuffer);
		if (!last)
			p.render_pass = MakeRenderPass(p.format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
		if (!BuildPass(p, last ? (output_pass ? output_pass : g.final_pass) : p.render_pass, ui))
		{
			internal::Log("[gpu] preset %s: pass %d could not be built", pr.name, i);
			return false;
		}
	}
	for (int i = 0; i < pr.lut_count; i++)
	{
		int w = 0, h = 0, n = 0;
		stbi_uc* px = stbi_load_from_memory(pr.luts[i].png, int(pr.luts[i].png_bytes), &w, &h, &n, 4);
		Image im;
		if (!px || !MakeImage(im, w, h, VK_FORMAT_R8G8B8A8_UNORM,
						 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
			!UploadImage(im, px, w, h))
		{
			internal::Log("[gpu] preset %s: texture %s could not be loaded", pr.name, pr.luts[i].name);
			if (px)
				stbi_image_free(px);
			return false;
		}
		stbi_image_free(px);
		c.luts.push_back(im);
	}
	ApplyOverrides(c);
	c.built = true;
	return true;
}

// a one-off upload (LUTs): through a temporary staging buffer, waited for
bool UploadImage(Image& im, const uint8_t* rgba, int w, int h)
{
	Buffer st;
	if (!MakeBuffer(st, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
		return false;
	memcpy(st.map, rgba, size_t(w) * h * 4);
	std::lock_guard<std::mutex> upload_lock(g.upload_mx);
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = g.upload_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer c;
	if (!Check(vkAllocateCommandBuffers(g.device, &ai, &c), "vkAllocateCommandBuffers"))
	{
		FreeBuffer(st);
		return false;
	}
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(c, &bi);
	Barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = {};
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageExtent = {uint32_t(w), uint32_t(h), 1};
	vkCmdCopyBufferToImage(c, st.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	Barrier(c, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkEndCommandBuffer(c);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c;
	VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence done = VK_NULL_HANDLE;
	vkCreateFence(g.device, &fi, nullptr, &done);
	bool ok;
	{
		std::lock_guard<std::mutex> q(g.queue_mx);
		ok = Check(vkQueueSubmit(g.queue, 1, &si, done), "vkQueueSubmit (upload)");
	}
	if (ok)
		vkWaitForFences(g.device, 1, &done, VK_TRUE, UINT64_MAX);
	vkDestroyFence(g.device, done, nullptr);
	vkFreeCommandBuffers(g.device, g.upload_pool, 1, &c);
	FreeBuffer(st);
	return ok;
}

// ---- per frame: sizes, textures, uniforms ------------------------------------------------------------------
struct Tex
{
	Image* im;
	bool linear;
	int wrap;
};

void SizeVec(float* v, int w, int h)
{
	v[0] = float(w);
	v[1] = float(h);
	v[2] = w ? 1.0f / float(w) : 0.0f;
	v[3] = h ? 1.0f / float(h) : 0.0f;
}

// a texture by its slang name, for pass `i` (Source is the previous pass's output)
bool Resolve(Chain& c, int i, const std::string& n, Tex& t, int& w, int& h)
{
	const ps5slang::Pass& d = *c.passes[size_t(i)].def;
	t = {&g.dummy, d.linear, d.wrap};
	Image* src = i == 0 ? &g.original : &c.passes[size_t(i - 1)].out[c.passes[size_t(i - 1)].cur];
	auto out_of = [&](int k, bool fb) -> Image* {
		PassRt& p = c.passes[size_t(k)];
		return fb ? (p.feedback ? &p.out[p.cur ^ 1] : &g.dummy) : &p.out[p.cur];
	};
	if (n == "Source")
		t.im = src;
	else if (n == "Original" || n == "OriginalHistory0")
		t.im = &g.original;
	else if (n.compare(0, 15, "OriginalHistory") == 0)
	{
		const int k = atoi(n.c_str() + 15);
		t.im = k >= 1 && k <= int(c.hist.size()) ? &c.hist[size_t(k - 1)] : &g.original;
	}
	else if (n.compare(0, 10, "PassOutput") == 0)
	{
		const int k = atoi(n.c_str() + 10);
		t.im = k >= 0 && k < i ? out_of(k, false) : &g.dummy;
	}
	else if (n.compare(0, 12, "PassFeedback") == 0)
	{
		const int k = atoi(n.c_str() + 12);
		t.im = k >= 0 && k < int(c.passes.size()) ? out_of(k, true) : &g.dummy;
	}
	else
	{
		bool found = false;
		for (int k = 0; k < int(c.passes.size()) && !found; k++)
		{
			const char* a = c.passes[size_t(k)].def->alias;
			if (!a[0])
				continue;
			if (n == a)
			{
				t.im = out_of(k, false);
				found = true;
			}
			else if (n == std::string(a) + "Feedback")
			{
				t.im = out_of(k, true);
				found = true;
			}
		}
		const ps5slang::Preset& pr = ps5slang::PresetAt(c.preset);
		for (int k = 0; k < pr.lut_count && !found; k++)
			if (n == pr.luts[k].name)
			{
				t = {&c.luts[size_t(k)], pr.luts[k].linear, pr.luts[k].wrap};
				found = true;
			}
		if (!found)
			return false;
	}
	w = t.im->w;
	h = t.im->h;
	return true;
}

// mvp: the pass's (the last one turns the picture); rot: the Rotation semantic
void FillUniforms(Chain& c, int i, uint8_t* block, const ps5slang::Member* m, int count, int out_w, int out_h,
	int vp_w, int vp_h, const float* mvp, int rot)
{
	const ps5slang::Preset& pr = ps5slang::PresetAt(c.preset);
	const ps5slang::Pass& d = *c.passes[size_t(i)].def;
	for (int k = 0; k < count; k++)
	{
		const std::string n = m[k].name;
		uint8_t* at = block + m[k].offset;
		float v4[4];
		auto put4 = [&](int w, int h) {
			SizeVec(v4, w, h);
			memcpy(at, v4, 16);
		};
		if (n == "MVP")
			memcpy(at, mvp, 64);
		else if (n == "OutputSize")
			put4(out_w, out_h);
		else if (n == "FinalViewportSize")
			put4(vp_w, vp_h);
		else if (n == "FrameCount")
		{
			const uint32_t f = d.frame_count_mod ? uint32_t(g.frame_count % uint64_t(d.frame_count_mod)) :
												   uint32_t(g.frame_count);
			memcpy(at, &f, 4);
		}
		else if (n == "FrameDirection")
		{
			const int32_t one = 1;
			memcpy(at, &one, 4);
		}
		else if (n == "Rotation")
		{
			const uint32_t r = uint32_t(rot & 3);
			memcpy(at, &r, 4);
		}
		else if (n == "TotalSubFrames" || n == "CurrentSubFrame")
		{
			const uint32_t one = 1;
			memcpy(at, &one, 4);
		}
		else if (n.size() > 4 && n.compare(n.size() - 4, 4, "Size") == 0)
		{
			Tex t;
			int w = 0, h = 0;
			if (Resolve(c, i, n.substr(0, n.size() - 4), t, w, h))
				put4(w, h);
			else
				put4(0, 0);
		}
		else
		{
			float v = 0;
			for (int p = 0; p < pr.param_count; p++)
				if (n == pr.params[p].name)
				{
					v = c.params[size_t(p)];
					break;
				}
			if (m[k].kind == 0)
				memcpy(at, &v, 4);
		}
	}
}

// the passes' output sizes for this frame; (re)creates the images that changed size
bool SizeChain(Chain& c, int vp_w, int vp_h)
{
	int sw = g.original.w, sh = g.original.h;
	for (size_t i = 0; i < c.passes.size(); i++)
	{
		PassRt& p = c.passes[i];
		const ps5slang::Pass& d = *p.def;
		const bool last = i + 1 == c.passes.size();
		int w, h;
		if (last)
		{
			w = vp_w;
			h = vp_h;
		}
		else
		{
			auto dim = [&](int type, float scale, int src, int vp) {
				const float v = type == 0 ? src * scale : type == 1 ? vp * scale : scale;
				return std::max(1, std::min(8192, int(std::lround(v))));
			};
			w = dim(d.scale_type_x, d.scale_x, sw, vp_w);
			h = dim(d.scale_type_y, d.scale_y, sh, vp_h);
		}
		p.out_w = w;
		p.out_h = h;
		if (!last && (p.out[0].w != w || p.out[0].h != h))
		{
			for (int k = 0; k < (p.feedback ? 2 : 1); k++)
			{
				FreeImage(p.out[k]);
				if (!MakeImage(p.out[k], w, h, p.format,
						VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
							VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
					return false;
				p.out[k].fb = MakeFramebuffer(p.render_pass, p.out[k].view, w, h);
			}
			if (!p.feedback)
				p.out[1] = Image(); // never read
		}
		sw = w;
		sh = h;
	}
	return true;
}

// One pass: its textures and uniforms, then its draw. An intermediate pass opens and closes its own render pass;
// the last one draws into the final render pass (already begun) in the game's rectangle `vp`, turned `rot` quarter
// turns (the chain's viewport, vpw x vph, is the rectangle in the picture's own orientation: vp's sides swapped for an
// odd `rot`).
void RecordPass(VkCommandBuffer cmd, Chain& c, size_t i, Rect vp, int vpw, int vph, int rot, bool plain_smooth)
{
	PassRt& p = c.passes[i];
	const ps5slang::Pass& d = *p.def;
	const bool last = i + 1 == c.passes.size();
	const int set_idx = int(g.frame_count & 1);
	if (p.feedback)
		p.cur ^= 1;
	std::vector<VkDescriptorImageInfo> infos(size_t(d.sampler_count));
	std::vector<VkWriteDescriptorSet> writes;
	VkDescriptorBufferInfo ubi = {p.ubo.buf, 0, VK_WHOLE_SIZE};
	if (d.ubo_binding >= 0)
	{
		VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = p.set[set_idx];
		w.dstBinding = uint32_t(d.ubo_binding);
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		w.pBufferInfo = &ubi;
		writes.push_back(w);
	}
	for (int s = 0; s < d.sampler_count; s++)
	{
		Tex t;
		int tw, th;
		if (!Resolve(c, int(i), d.samplers[s].name, t, tw, th) || !t.im->img)
			t = {&g.dummy, false, 1};
		if (!last)
			Barrier(cmd, *t.im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); // (inside a render pass: done before it)
		const bool linear = (c.preset == 0 && t.im == &g.original) ? plain_smooth : t.linear;
		infos[size_t(s)] = {g.samplers[linear ? 1 : 0][t.wrap & 3], t.im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = p.set[set_idx];
		w.dstBinding = uint32_t(d.samplers[s].binding);
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &infos[size_t(s)];
		writes.push_back(w);
	}
	vkUpdateDescriptorSets(g.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
	const int ow = p.out_w, oh = p.out_h;
	float mvp[16];
	RotatedMvp(last ? rot : 0, mvp);
	if (d.ubo_binding >= 0)
		FillUniforms(c, int(i), static_cast<uint8_t*>(p.ubo.map), d.ubo, d.ubo_count, ow, oh, vpw, vph, mvp, rot);
	uint8_t push[256] = {};
	if (d.push_size > 0)
		FillUniforms(c, int(i), push, d.push, d.push_count, ow, oh, vpw, vph, mvp, rot);
	if (!last)
	{
		Image& out = p.out[p.cur];
		VkClearValue cv = {};
		VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rb.renderPass = p.render_pass;
		rb.framebuffer = out.fb;
		rb.renderArea = {{0, 0}, {uint32_t(ow), uint32_t(oh)}};
		rb.clearValueCount = 1;
		rb.pClearValues = &cv;
		vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
		out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // the render pass's final layout
	}
	const VkViewport view = last ? VkViewport{float(vp.x), float(vp.y), float(vp.w), float(vp.h), 0, 1} :
								   VkViewport{0, 0, float(ow), float(oh), 0, 1};
	const VkRect2D sc = last ? VkRect2D{{vp.x, vp.y}, {uint32_t(vp.w), uint32_t(vp.h)}} :
							   VkRect2D{{0, 0}, {uint32_t(ow), uint32_t(oh)}};
	vkCmdSetViewport(cmd, 0, 1, &view);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &p.set[set_idx], 0, nullptr);
	if (d.push_size > 0)
		vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			uint32_t(std::min(d.push_size, 256)), push);
	const VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &g.vertices.buf, &zero);
	vkCmdDraw(cmd, 4, 1, 0, 0);
	if (!last)
		vkCmdEndRenderPass(cmd);
}

// ---- the target --------------------------------------------------------------------------------------------
bool MakeTarget(int want_w, int want_h)
{
	if (g.offscreen)
	{
		g.out_w = want_w;
		g.out_h = want_h;
		g.target_format = VK_FORMAT_B8G8R8A8_UNORM;
		if (!MakeImage(g.offscreen_target, want_w, want_h, g.target_format,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
			return false;
		if (!MakeBuffer(g.readback, VkDeviceSize(want_w) * want_h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
			return false;
		g.final_pass = MakeRenderPass(g.target_format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true);
		g.offscreen_target.fb = MakeFramebuffer(g.final_pass, g.offscreen_target.view, want_w, want_h);
		return g.final_pass && g.offscreen_target.fb;
	}
	// the display: its mode closest to 60 Hz (the largest of those), a plane surface, a FIFO swapchain
	VkDisplayPropertiesKHR disp;
	uint32_t n = 1;
	if (vkGetPhysicalDeviceDisplayPropertiesKHR(g.phys, &n, &disp) < 0 || n < 1)
	{
		internal::Log("[gpu] no display");
		return false;
	}
	VkDisplayModePropertiesKHR modes[16];
	uint32_t mc = 16;
	if (vkGetDisplayModePropertiesKHR(g.phys, disp.display, &mc, modes) < 0 || mc == 0)
	{
		internal::Log("[gpu] the display has no modes");
		return false;
	}
	int best = 0;
	for (uint32_t i = 0; i < mc; i++)
	{
		const VkDisplayModeParametersKHR& a = modes[i].parameters;
		const VkDisplayModeParametersKHR& b = modes[best].parameters;
		internal::Log("[gpu] display mode %u: %ux%u at %.3f Hz", i, a.visibleRegion.width, a.visibleRegion.height,
			a.refreshRate / 1000.0);
		const int da = std::abs(int(a.refreshRate) - 60000), db = std::abs(int(b.refreshRate) - 60000);
		if (da < db - 500 ||
			(std::abs(da - db) <= 500 && a.visibleRegion.width * a.visibleRegion.height >
											 b.visibleRegion.width * b.visibleRegion.height))
			best = int(i);
	}
	const VkDisplayModeParametersKHR& mp = modes[best].parameters;
	VkDisplaySurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
	si.displayMode = modes[best].displayMode;
	si.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	si.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	si.imageExtent = mp.visibleRegion;
	if (!Check(vkCreateDisplayPlaneSurfaceKHR(g.instance, &si, nullptr, &g.surface), "vkCreateDisplayPlaneSurfaceKHR"))
		return false;
	VkBool32 sup = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.family, g.surface, &sup);
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
	VkSurfaceFormatKHR fmts[16];
	uint32_t fc = 16;
	vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &fc, fmts);
	g.target_format = fc ? fmts[0].format : VK_FORMAT_B8G8R8A8_UNORM;
	VkColorSpaceKHR cs = fc ? fmts[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	for (uint32_t i = 0; i < fc; i++)
		if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM)
		{
			g.target_format = fmts[i].format;
			cs = fmts[i].colorSpace;
			break;
		}
	g.out_w = int(caps.currentExtent.width ? caps.currentExtent.width : mp.visibleRegion.width);
	g.out_h = int(caps.currentExtent.height ? caps.currentExtent.height : mp.visibleRegion.height);
	VkSwapchainCreateInfoKHR sc = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	sc.surface = g.surface;
	sc.minImageCount = std::max(caps.minImageCount, 2u);
	sc.imageFormat = g.target_format;
	sc.imageColorSpace = cs;
	sc.imageExtent = {uint32_t(g.out_w), uint32_t(g.out_h)};
	sc.imageArrayLayers = 1;
	sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	sc.clipped = VK_TRUE;
	if (!sup || !Check(vkCreateSwapchainKHR(g.device, &sc, nullptr, &g.swapchain), "vkCreateSwapchainKHR"))
		return false;
	uint32_t ic = 0;
	vkGetSwapchainImagesKHR(g.device, g.swapchain, &ic, nullptr);
	g.swap_images.resize(ic);
	vkGetSwapchainImagesKHR(g.device, g.swapchain, &ic, g.swap_images.data());
	g.final_pass = MakeRenderPass(g.target_format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
	for (VkImage im : g.swap_images)
	{
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = im;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = g.target_format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView v = VK_NULL_HANDLE;
		if (!Check(vkCreateImageView(g.device, &vi, nullptr, &v), "vkCreateImageView (swapchain)"))
			return false;
		g.swap_views.push_back(v);
		g.swap_fbs.push_back(MakeFramebuffer(g.final_pass, v, g.out_w, g.out_h));
	}
	internal::Log("[gpu] display %s: %dx%d at %.3f Hz, %u images, format %d", disp.displayName ? disp.displayName : "?",
		g.out_w, g.out_h, mp.refreshRate / 1000.0, ic, int(g.target_format));
	return true;
}

bool HasExtension(const std::vector<VkExtensionProperties>& v, const char* name)
{
	for (const auto& e : v)
		if (!strcmp(e.extensionName, name))
			return true;
	return false;
}
std::string PipelineCachePath()
{
	return CurrentConfig().cache_file;
}

// the pipeline cache to its file (written whole to a temporary file first)
void SavePipelineCache()
{
	if (!g.pcache || PipelineCachePath().empty())
		return;
	size_t n = 0;
	if (vkGetPipelineCacheData(g.device, g.pcache, &n, nullptr) != VK_SUCCESS || n == 0 || n == g.pcache_saved)
		return; // nothing new since the last save
	internal::MakeParentDirs(PipelineCachePath());
	std::vector<uint8_t> data(n);
	if (vkGetPipelineCacheData(g.device, g.pcache, &n, data.data()) != VK_SUCCESS)
		return;
	const std::string path = PipelineCachePath(), tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return;
	const bool ok = fwrite(data.data(), 1, n, f) == n;
	if (fclose(f) == 0 && ok && rename(tmp.c_str(), path.c_str()) == 0)
	{
		g.pcache_saved = n;
		internal::Log("[gpu] pipeline cache saved: %zu KiB", n >> 10);
	}
	else
		remove(tmp.c_str());
}

// Builds `preset` on a thread of its own; Present takes it when it is done.
void StartBuild(int preset)
{
	if (g.builder.joinable())
		g.builder.join();
	g.building_preset = preset;
	g.built_ready = false;
	g.building = true;
	auto build = [preset] {
		Chain* c = new Chain;
		const auto t0 = std::chrono::steady_clock::now();
		if (!BuildChain(*c, preset, false))
		{
			FreeChain(*c);
			delete c;
			c = nullptr;
		}
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (c)
			internal::Log("[gpu] shader %s built in the background in %.2f s", ps5slang::PresetAt(preset).name, secs);
		else
			internal::Log("[gpu] shader %s: the background build failed after %.2f s", ps5slang::PresetAt(preset).name, secs);
		g.built = c;
		g.built_ready = true;
		g.building = false;
	};
	g.builder = internal::Thread(build, size_t(8) << 20);
	if (!g.builder.joinable())
	{
		// the system refused the thread: built here, the picture waits for it this once
		internal::Log("[gpu] no thread for the shader's build: built on the frame's thread");
		build();
	}
}

// ---- the shelf's scene: textured quads in perspective, under the surface -------------------------------------
// The quads' shaders read everything from a vertex buffer, as the slang passes do. (2.0 read them from push
// constants with dynamic indexing, q.x[i]: the host's driver drew it, RADV on the PS5 drew nothing at all.)
const char* const kSceneVs = R"(#version 450
layout(location = 0) in vec4 Pos;    // clip space, w the corner's distance (perspective)
layout(location = 1) in vec2 Uv;
layout(location = 2) in float K;     // 0 at the top edge, 1 at the bottom (the reflection's fade)
layout(location = 3) in vec4 P;      // bright, alpha, mode
layout(location = 4) in vec4 Rect;   // glow: the rectangle, in target pixels
layout(location = 5) in vec4 Col;    // glow: colour, radius in target pixels
layout(location = 6) in vec4 UvRect; // the quad's texture rectangle (its edges are smoothed)
layout(location = 0) out vec2 vUv;
layout(location = 1) noperspective out float vK;
layout(location = 2) flat out vec4 vP;
layout(location = 3) flat out vec4 vRect;
layout(location = 4) flat out vec4 vCol;
layout(location = 5) flat out vec4 vUvRect;
void main()
{
	gl_Position = Pos;
	vUv = Uv;
	vK = K;
	vP = P;
	vRect = Rect;
	vCol = Col;
	vUvRect = UvRect;
}
)";
const char* const kSceneFs = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D T;
layout(location = 0) in vec2 vUv;
layout(location = 1) noperspective in float vK;
layout(location = 2) flat in vec4 vP;
layout(location = 3) flat in vec4 vRect;
layout(location = 4) flat in vec4 vCol;
layout(location = 5) flat in vec4 vUvRect;
layout(location = 0) out vec4 FragColor;
void main()
{
	if (vP.z > 1.5)
	{
		// the glow: by the distance to the rectangle; added (alpha 0)
		vec2 f = gl_FragCoord.xy;
		float dx = max(max(vRect.x - f.x, 0.0), f.x - vRect.z);
		float dy = max(max(vRect.y - f.y, 0.0), f.y - vRect.w);
		float d = length(vec2(dx, dy));
		if (d >= vCol.w || d <= 0.0)
			discard;
		float k = 1.0 - d / vCol.w;
		float a = k * k * vP.y * 0.85;
		FragColor = vec4(vCol.rgb * a, 0.0);
		return;
	}
	vec4 c = texture(T, vUv);
	float a = vP.y;
	// the quad's own edges, smoothed over a pixel (no multisampling)
	vec2 lo = min(vUv - min(vUvRect.xy, vUvRect.zw), max(vUvRect.xy, vUvRect.zw) - vUv);
	vec2 fw = max(fwidth(vUv), vec2(1e-6));
	a *= clamp(min(lo.x / fw.x, lo.y / fw.y) + 0.5, 0.0, 1.0);
	if (vP.z > 0.5)
		a *= 0.30 * (1.0 - vK) * (1.0 - vK);
	FragColor = vec4(c.rgb * vP.x * a, a);
}
)";
constexpr int kSceneVertexFloats = 28; // pos 4, uv 2, k 1, pad 1, p 4, rect 4, col 4, uvrect 4, pad 4

bool AllocSceneSet(VkDescriptorSet& set, VkImageView view)
{
	VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	ai.descriptorPool = g.dpool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &g.scene_set_layout;
	{
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		if (!Check(vkAllocateDescriptorSets(g.device, &ai, &set), "vkAllocateDescriptorSets (scene)"))
			return false;
	}
	VkDescriptorImageInfo info = {g.scene_sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = set;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &info;
	vkUpdateDescriptorSets(g.device, 1, &w, 0, nullptr);
	return true;
}

// The quad pipeline (its shaders compiled here by glslang). False: the shelf stays on the CPU.
bool InitScene()
{
	std::vector<uint32_t> vs, fs;
	std::string err;
	bool compiled = false;
	{
		// on a thread with a big stack (glslang's parser recurses; the caller's stack may be the SDK's small one)
		auto compile = [&] {
			compiled = ps5slang::CompileGlsl(kSceneVs, true, vs, err) && ps5slang::CompileGlsl(kSceneFs, false, fs, err);
		};
		internal::Thread t(compile, size_t(4) << 20);
		if (t.joinable())
			t.join();
		else
			compile(); // no thread: on the caller's stack
	}
	if (!compiled)
	{
		internal::Log("[gpu] the shelf's quad shader did not compile (%s): the CPU draws the shelf", err.c_str());
		return false;
	}
	VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = 1;
	li.pBindings = &b;
	if (!Check(vkCreateDescriptorSetLayout(g.device, &li, nullptr, &g.scene_set_layout), "vkCreateDescriptorSetLayout"))
		return false;
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &g.scene_set_layout;
	if (!Check(vkCreatePipelineLayout(g.device, &pli, nullptr, &g.scene_layout), "vkCreatePipelineLayout"))
		return false;
	VkShaderModule vm = Module(vs.data(), unsigned(vs.size() * 4)), fm = Module(fs.data(), unsigned(fs.size() * 4));
	if (!vm || !fm)
		return false;
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = vm;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = fm;
	st[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkVertexInputBindingDescription vb = {0, kSceneVertexFloats * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
	const VkVertexInputAttributeDescription va[7] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
		{1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)}, {2, 0, VK_FORMAT_R32_SFLOAT, 6 * sizeof(float)},
		{3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 * sizeof(float)}, {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12 * sizeof(float)},
		{5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * sizeof(float)}, {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20 * sizeof(float)}};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vb;
	vi.vertexAttributeDescriptionCount = 7;
	vi.pVertexAttributeDescriptions = va;
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba = {};
	ba.colorWriteMask = 0xf;
	ba.blendEnable = VK_TRUE;
	ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.colorBlendOp = VK_BLEND_OP_ADD;
	ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.stageCount = 2;
	pi.pStages = st;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &ds;
	pi.layout = g.scene_layout;
	pi.renderPass = g.final_pass;
	const bool ok = Check(vkCreateGraphicsPipelines(g.device, g.pcache, 1, &pi, nullptr, &g.scene_pipe),
		"vkCreateGraphicsPipelines (scene)");
	vkDestroyShaderModule(g.device, vm, nullptr);
	vkDestroyShaderModule(g.device, fm, nullptr);
	if (!ok)
		return false;
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.maxLod = 16.0f;
	if (!Check(vkCreateSampler(g.device, &si, nullptr, &g.scene_sampler), "vkCreateSampler (scene)"))
		return false;
	return AllocSceneSet(g.scene_dummy_set, g.dummy.view);
}

void FreeSceneTex(State::SceneTex& t)
{
	if (t.set)
	{
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		vkFreeDescriptorSets(g.device, g.dpool, 1, &t.set);
	}
	if (t.view)
		vkDestroyImageView(g.device, t.view, nullptr);
	if (t.img)
		vkDestroyImage(g.device, t.img, nullptr);
	if (t.mem)
		vkFreeMemory(g.device, t.mem, nullptr);
	t = State::SceneTex();
}

void FreeScene()
{
	for (auto& kv : g.scene_tex)
		FreeSceneTex(kv.second);
	g.scene_tex.clear();
	g.scene.clear();
	g.scene_on = false;
	FreeBuffer(g.scene_vb);
	if (g.scene_dummy_set)
	{
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		vkFreeDescriptorSets(g.device, g.dpool, 1, &g.scene_dummy_set);
	}
	if (g.scene_sampler)
		vkDestroySampler(g.device, g.scene_sampler, nullptr);
	if (g.scene_pipe)
		vkDestroyPipeline(g.device, g.scene_pipe, nullptr);
	if (g.scene_layout)
		vkDestroyPipelineLayout(g.device, g.scene_layout, nullptr);
	if (g.scene_set_layout)
		vkDestroyDescriptorSetLayout(g.device, g.scene_set_layout, nullptr);
	g.scene_dummy_set = VK_NULL_HANDLE;
	g.scene_sampler = VK_NULL_HANDLE;
	g.scene_pipe = VK_NULL_HANDLE;
	g.scene_layout = VK_NULL_HANDLE;
	g.scene_set_layout = VK_NULL_HANDLE;
}

// a texture with its whole mip chain: level 0 copied in, the others blitted down from it
bool MakeSceneTex(State::SceneTex& t, const uint32_t* px, int w, int h, bool column_major)
{
	int levels = 1;
	while ((std::max(w, h) >> levels) > 0)
		levels++;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_UNORM; // the surface's (and the covers') byte order
	ii.extent = {uint32_t(w), uint32_t(h), 1};
	ii.mipLevels = uint32_t(levels);
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (!Check(vkCreateImage(g.device, &ii, nullptr, &t.img), "vkCreateImage (scene)"))
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g.device, t.img, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX)
		ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, 0);
	if (!Check(vkAllocateMemory(g.device, &ai, nullptr, &t.mem), "vkAllocateMemory (scene)"))
		return false;
	vkBindImageMemory(g.device, t.img, t.mem, 0);
	t.bytes = size_t(req.size);
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = t.img;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = ii.format;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(levels), 0, 1};
	if (!Check(vkCreateImageView(g.device, &vi, nullptr, &t.view), "vkCreateImageView (scene)"))
		return false;

	Buffer st;
	if (!MakeBuffer(st, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
		return false;
	uint32_t* dst = static_cast<uint32_t*>(st.map);
	if (column_major)
	{
		for (int x = 0; x < w; x++)
			for (int y = 0; y < h; y++)
				dst[size_t(y) * w + x] = px[size_t(x) * h + y] | 0xff000000u;
	}
	else
		for (size_t i = 0; i < size_t(w) * h; i++)
			dst[i] = px[i] | 0xff000000u;

	std::lock_guard<std::mutex> upload_lock(g.upload_mx);
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = g.upload_pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer c;
	if (!Check(vkAllocateCommandBuffers(g.device, &cai, &c), "vkAllocateCommandBuffers"))
	{
		FreeBuffer(st);
		return false;
	}
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(c, &bi);
	auto barrier = [&](int level, VkImageLayout from, VkImageLayout to) {
		VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = t.img;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 1, 0, 1};
		vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
			0, nullptr, 1, &b);
	};
	for (int l = 0; l < levels; l++)
		barrier(l, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = {};
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageExtent = {uint32_t(w), uint32_t(h), 1};
	vkCmdCopyBufferToImage(c, st.buf, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	for (int l = 1; l < levels; l++)
	{
		barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkImageBlit bl = {};
		bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(l - 1), 0, 1};
		bl.srcOffsets[1] = {std::max(1, w >> (l - 1)), std::max(1, h >> (l - 1)), 1};
		bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(l), 0, 1};
		bl.dstOffsets[1] = {std::max(1, w >> l), std::max(1, h >> l), 1};
		vkCmdBlitImage(c, t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
			VK_FILTER_LINEAR);
		barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	barrier(levels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkEndCommandBuffer(c);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c;
	VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence done = VK_NULL_HANDLE;
	vkCreateFence(g.device, &fi, nullptr, &done);
	bool ok;
	{
		std::lock_guard<std::mutex> q(g.queue_mx);
		ok = Check(vkQueueSubmit(g.queue, 1, &si, done), "vkQueueSubmit (scene texture)");
	}
	if (ok)
		vkWaitForFences(g.device, 1, &done, VK_TRUE, UINT64_MAX);
	vkDestroyFence(g.device, done, nullptr);
	vkFreeCommandBuffers(g.device, g.upload_pool, 1, &c);
	FreeBuffer(st);
	return ok && AllocSceneSet(t.set, t.view);
}

// the quads' vertices into vb (grown when needed: the caller has waited for the frame that used it)
bool FillQuads(Buffer& vb, const std::vector<SceneQuad>& quads, int sw, int sh, int ow, int oh)
{
	const VkDeviceSize need = VkDeviceSize(std::max<size_t>(quads.size(), 1)) * 4 * kSceneVertexFloats * sizeof(float);
	if (vb.size < need)
	{
		FreeBuffer(vb);
		if (!MakeBuffer(vb, std::max<VkDeviceSize>(need * 2, 64 * 1024), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
			return false;
	}
	float* v = static_cast<float*>(vb.map);
	const float kx = float(ow) / float(sw), ky = float(oh) / float(sh);
	for (const SceneQuad& q : quads)
		for (int i = 0; i < 4; i++, v += kSceneVertexFloats)
		{
			const float w = q.w[i] > 1e-3f ? q.w[i] : 1e-3f;
			const float nx = q.x[i] / float(sw) * 2.0f - 1.0f, ny = q.y[i] / float(sh) * 2.0f - 1.0f;
			const float vert[kSceneVertexFloats] = {nx * w, ny * w, 0.0f, w, (i & 1) ? q.u1 : q.u0, (i & 2) ? q.v1 : q.v0,
				(i & 2) ? 1.0f : 0.0f, 0.0f, q.bright, q.alpha, float(q.mode), 0.0f, q.rect[0] * kx, q.rect[1] * ky,
				q.rect[2] * kx, q.rect[3] * ky, q.color[0], q.color[1], q.color[2], q.glow_r * kx, q.u0, q.v0, q.u1, q.v1,
				0.0f, 0.0f, 0.0f, 0.0f};
			memcpy(v, vert, sizeof(vert));
		}
	return true;
}

// the quads, in a render pass already begun: sw x sh the coordinates' space (the surface's), ow x oh the target's
void RecordQuads(VkCommandBuffer cmd, const std::vector<SceneQuad>& quads, int sw, int sh, int ow, int oh, Buffer& vb)
{
	if (quads.empty() || !FillQuads(vb, quads, sw, sh, ow, oh))
		return;
	const VkViewport view = {0, 0, float(ow), float(oh), 0, 1};
	const VkRect2D sc = {{0, 0}, {uint32_t(ow), uint32_t(oh)}};
	vkCmdSetViewport(cmd, 0, 1, &view);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.scene_pipe);
	const VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buf, &zero);
	for (size_t n = 0; n < quads.size(); n++)
	{
		const SceneQuad& q = quads[n];
		VkDescriptorSet set = g.scene_dummy_set;
		if (q.mode != kQuadGlow)
		{
			auto it = g.scene_tex.find(q.tex);
			if (it == g.scene_tex.end())
				continue;
			it->second.used = g.presents;
			set = it->second.set;
		}
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.scene_layout, 0, 1, &set, 0, nullptr);
		vkCmdDraw(cmd, 4, 1, uint32_t(n * 4), 0);
	}
}

void RecordScene(VkCommandBuffer cmd)
{
	static int logs = 0;
	if (logs < 3 && !g.scene.empty())
	{
		logs++;
		size_t found = 0;
		for (const SceneQuad& q : g.scene)
			found += q.mode == kQuadGlow || g.scene_tex.count(q.tex);
		internal::Log("[gpu] shelf scene: %zu quad(s) drawn (%zu with their texture), %zu texture(s) kept, %dx%d output",
			g.scene.size(), found, g.scene_tex.size(), g.out_w, g.out_h);
	}
	RecordQuads(cmd, g.scene, g.ui_w, g.ui_h, g.out_w, g.out_h, g.scene_vb);
}

// The scene's self-test, at start: two quads drawn into a small image of the target's format with the target's
// render pass -- one flat (w 1, a blue texture), one in perspective as a cover is (w about 1000, a red texture,
// column-major as the covers are) -- and the pixels read back. The 2.0 shelf drew nothing on a PS5 while the
// host's driver drew it right: this says what the console's driver makes of it, and keeps the shelf on the CPU
// when the picture is not what it should be.
bool SceneSelfTest()
{
	constexpr int S = 64, SW = 96; // three 32-pixel columns: flat, perspective, glow
	const uint64_t kBlue = 0xfff0000000000001ull, kRed = 0xfff0000000000002ull;
	uint32_t blue[16], red[16];
	for (int i = 0; i < 16; i++)
	{
		blue[i] = 0xffff0000u; // R8G8B8A8 in memory: R G B A -> as a uint32, A B G R
		red[i] = 0xff0000ffu;
	}
	if (!SceneTexture(kBlue, blue, 4, 4, false) || !SceneTexture(kRed, red, 4, 4, true))
	{
		internal::Log("[gpu] shelf self-test: the test textures could not be made");
		return false;
	}
	Image im;
	bool ok = MakeImage(im, SW, S, g.target_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	if (ok)
		im.fb = MakeFramebuffer(g.final_pass, im.view, SW, S);
	Buffer rb, test_vb;
	ok = ok && im.fb && MakeBuffer(rb, VkDeviceSize(SW) * S * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	std::vector<SceneQuad> quads(3);
	if (ok)
	{
		// left half: flat, blue
		SceneQuad& a = quads[0];
		a.tex = kBlue;
		const float ax[4] = {2, 30, 2, 30}, ay[4] = {2, 2, 62, 62};
		for (int i = 0; i < 4; i++)
		{
			a.x[i] = ax[i];
			a.y[i] = ay[i];
		}
		// right half: in perspective (its two ends at different distances, as a side cover), red
		SceneQuad& b = quads[1];
		b.tex = kRed;
		const float bx[4] = {34, 62, 34, 62}, by[4] = {2, 10, 62, 54}, bw[4] = {1000, 1300, 1000, 1300};
		for (int i = 0; i < 4; i++)
		{
			b.x[i] = bx[i];
			b.y[i] = by[i];
			b.w[i] = bw[i];
		}
		// third column: a green glow (no texture) around a point at (80, 32), 40 pixels out
		SceneQuad& c = quads[2];
		c.mode = kQuadGlow;
		const float cx[4] = {64, 96, 64, 96}, cy[4] = {0, 0, 64, 64};
		for (int i = 0; i < 4; i++)
		{
			c.x[i] = cx[i];
			c.y[i] = cy[i];
		}
		c.rect[0] = c.rect[2] = 80;
		c.rect[1] = c.rect[3] = 32;
		c.glow_r = 40;
		c.color[1] = 1;
	}
	VkCommandBuffer c = VK_NULL_HANDLE;
	if (ok)
	{
		std::lock_guard<std::mutex> upload_lock(g.upload_mx);
		VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = g.upload_pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		ok = Check(vkAllocateCommandBuffers(g.device, &cai, &c), "vkAllocateCommandBuffers (self-test)");
		if (ok)
		{
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vkBeginCommandBuffer(c, &bi);
			VkClearValue cv = {};
			cv.color.float32[3] = 1.0f; // black, as the frame's
			VkRenderPassBeginInfo rb2 = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			rb2.renderPass = g.final_pass;
			rb2.framebuffer = im.fb;
			rb2.renderArea = {{0, 0}, {uint32_t(SW), uint32_t(S)}};
			rb2.clearValueCount = 1;
			rb2.pClearValues = &cv;
			vkCmdBeginRenderPass(c, &rb2, VK_SUBPASS_CONTENTS_INLINE);
			RecordQuads(c, quads, SW, S, SW, S, test_vb);
			vkCmdEndRenderPass(c);
			// the render pass leaves it in the target's final layout (present, or transfer source off screen)
			im.layout = g.offscreen ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			Barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkBufferImageCopy r = {};
			r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			r.imageExtent = {uint32_t(SW), uint32_t(S), 1};
			vkCmdCopyImageToBuffer(c, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
			vkEndCommandBuffer(c);
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &c;
			VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			VkFence done = VK_NULL_HANDLE;
			vkCreateFence(g.device, &fi, nullptr, &done);
			{
				std::lock_guard<std::mutex> q(g.queue_mx);
				ok = Check(vkQueueSubmit(g.queue, 1, &si, done), "vkQueueSubmit (self-test)");
			}
			if (ok)
				ok = vkWaitForFences(g.device, 1, &done, VK_TRUE, 5000000000ull) == VK_SUCCESS;
			vkDestroyFence(g.device, done, nullptr);
			vkFreeCommandBuffers(g.device, g.upload_pool, 1, &c);
		}
	}
	bool good = false;
	if (ok)
	{
		// the target is B8G8R8A8 (or R8G8B8A8): find each pixel's red and blue whatever the order
		const bool bgra = g.target_format == VK_FORMAT_B8G8R8A8_UNORM || g.target_format == VK_FORMAT_B8G8R8A8_SRGB;
		auto px = [&](int x, int y, int& r, int& gg, int& b) {
			const uint8_t* p = static_cast<const uint8_t*>(rb.map) + (size_t(y) * SW + x) * 4;
			r = bgra ? p[2] : p[0];
			gg = p[1];
			b = bgra ? p[0] : p[2];
		};
		int r1, g1, b1, r2, g2, b2, r0, g0, b0, r3, g3, b3;
		px(16, 32, r1, g1, b1); // flat quad: blue
		px(48, 32, r2, g2, b2); // perspective quad: red
		px(32, 1, r0, g0, b0);  // between: the black clear
		px(84, 32, r3, g3, b3); // the glow, 4 pixels from its point: green
		internal::Log("[gpu] shelf self-test (format %d): flat quad %d,%d,%d (want blue), perspective quad %d,%d,%d (want red), "
				 "glow %d,%d,%d (want green), background %d,%d,%d (want black)",
			int(g.target_format), r1, g1, b1, r2, g2, b2, r3, g3, b3, r0, g0, b0);
		good = b1 > 200 && r1 < 60 && r2 > 200 && b2 < 60 && g3 > 100 && r3 < 60 && r0 < 30 && b0 < 30;
	}
	else
		internal::Log("[gpu] shelf self-test: could not run");
	FreeBuffer(rb);
	FreeBuffer(test_vb);
	FreeImage(im);
	// the test textures go
	for (uint64_t k : {kBlue, kRed})
	{
		auto it = g.scene_tex.find(k);
		if (it != g.scene_tex.end())
		{
			FreeSceneTex(it->second);
			g.scene_tex.erase(it);
		}
	}
	return good;
}
} // namespace

bool Building()
{
	return g.building.load();
}

int ShaderParamCount(int shader)
{
	const int p = shader <= 0 ? -1 : shader + 1;
	return p > 1 && p < ps5slang::PresetCount() ? ps5slang::PresetAt(p).param_count : 0;
}

bool ShaderParam(int shader, int i, ParamInfo* out)
{
	const int p = shader <= 0 ? -1 : shader + 1;
	if (p <= 1 || p >= ps5slang::PresetCount() || i < 0 || i >= ps5slang::PresetAt(p).param_count)
		return false;
	const ps5slang::Preset& pr = ps5slang::PresetAt(p);
	const ps5slang::Param& d = pr.params[i];
	out->name = d.name;
	out->desc = d.desc;
	out->def = d.value;
	out->min = d.min;
	out->max = d.max;
	out->step = d.step;
	out->value = d.value;
	std::lock_guard<std::mutex> lk(g.param_mx);
	const auto ov = g.param_overrides.find(pr.name);
	if (ov != g.param_overrides.end())
	{
		const auto v = ov->second.find(d.name);
		if (v != ov->second.end())
			out->value = v->second;
	}
	return true;
}

void SetShaderParam(int shader, const char* name, float value)
{
	const int p = shader <= 0 ? -1 : shader + 1;
	if (p <= 1 || p >= ps5slang::PresetCount())
		return;
	const ps5slang::Preset& pr = ps5slang::PresetAt(p);
	for (int i = 0; i < pr.param_count; i++)
		if (!strcmp(pr.params[i].name, name))
		{
			std::lock_guard<std::mutex> lk(g.param_mx);
			if (value == pr.params[i].value)
				g.param_overrides[pr.name].erase(name);
			else
				g.param_overrides[pr.name][name] = value;
			if (g.game_chain.preset == p && size_t(i) < g.game_chain.params.size())
				g.game_chain.params[size_t(i)] = value;
		}
}

void ResetShaderParams(int shader)
{
	const int p = shader <= 0 ? -1 : shader + 1;
	if (p <= 1 || p >= ps5slang::PresetCount())
		return;
	{
		std::lock_guard<std::mutex> lk(g.param_mx);
		g.param_overrides.erase(ps5slang::PresetAt(p).name);
	}
	if (g.game_chain.preset == p)
		ApplyOverrides(g.game_chain);
}

namespace
{
std::string ShaderParamsPath()
{
	return CurrentConfig().params_file;
}

// "<preset>\t<parameter>\t<value>" a line (preset names have spaces); a preset or parameter this build doesn't have is
// left out
void LoadShaderParams()
{
	if (ShaderParamsPath().empty())
		return;
	FILE* f = fopen(ShaderParamsPath().c_str(), "r");
	if (!f)
		return;
	char line[512];
	int n = 0;
	std::lock_guard<std::mutex> lk(g.param_mx);
	while (fgets(line, sizeof(line), f))
	{
		char* t1 = strchr(line, '\t');
		char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
		if (!t2)
			continue;
		*t1 = *t2 = 0;
		const char* preset = line;
		const char* name = t1 + 1;
		char* end = nullptr;
		const float v = strtof(t2 + 1, &end);
		if (end == t2 + 1 || !std::isfinite(v))
			continue;
		for (int p = 2; p < ps5slang::PresetCount(); p++)
		{
			const ps5slang::Preset& pr = ps5slang::PresetAt(p);
			if (strcmp(pr.name, preset))
				continue;
			if (ps5slang::IsExternal(p))
			{
				// not compiled yet: its parameters are not known, the value is kept as it is (applied by name)
				g.param_overrides[pr.name][name] = v;
				n++;
				continue;
			}
			for (int i = 0; i < pr.param_count; i++)
			{
				const ps5slang::Param& d = pr.params[i];
				if (!strcmp(d.name, name))
				{
					g.param_overrides[pr.name][d.name] = std::max(d.min, std::min(d.max, v));
					n++;
				}
			}
		}
	}
	fclose(f);
	if (n)
		internal::Log("[gpu] %d shader parameter(s) set in the menu, from %s", n, ShaderParamsPath().c_str());
}
} // namespace

bool SaveShaderParams()
{
	std::string text;
	{
		std::lock_guard<std::mutex> lk(g.param_mx);
		for (const auto& pr : g.param_overrides)
			for (const auto& kv : pr.second)
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "\t%.6g\n", double(kv.second));
				text += pr.first + "\t" + kv.first + buf;
			}
	}
	const std::string path = ShaderParamsPath(), tmp = path + ".part";
	if (path.empty())
		return true;
	if (text.empty())
	{
		remove(path.c_str());
		return true;
	}
	internal::MakeParentDirs(path);
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
		return false;
	const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		remove(tmp.c_str());
		internal::Log("[gpu] could not write %s", path.c_str());
		return false;
	}
	return true;
}

// ---- 1.1: a media player's pictures (SetVideo) ------------------------------------------------------------------------
// One pipeline (shaders/video.vert / video.frag, compiled at build time into video_spv.h) draws the picture into the
// final render pass, under the UI surface. Its planes are images of their own (R8 / R8G8 / R8G8B8A8, R16 / R16G16),
// filled each new picture by vkCmdCopyBufferToImage from either
//   - a staging buffer of a ring of three, the planes copied in by SetVideo (one memcpy per plane): the slot the GPU
//     may still be reading (the frame in flight) is never the one written, so SetVideo never waits for the GPU; or
//   - the caller's own memory, imported (VK_EXT_external_memory_host): no CPU copy at all; the caller's release is
//     called once the frame that read it is done.
// One frame is in flight (Present waits for the previous one first), so the plane images, the descriptor set and the
// uniform block are written only when the GPU doesn't use them.
#include "video_spv.h"

struct HostMemory
{
	Buffer b; // b.map: the caller's base
	size_t bytes = 0;
};

namespace
{
constexpr int kVideoRing = 3;

struct VideoUbo
{
	float lsize[4], csize[4], ratio[4], norm[4], m0[4], m1[4], m2[4], tm[4], pq[4], coff[4];
	int32_t mode[4], swz[4];
};

struct VideoPending
{
	bool set = false;
	int slot = -1; // the ring's slot, or -1: imported
	HostMemory* mem = nullptr;
	VkDeviceSize off[3] = {};
	uint32_t row_texels[3] = {};
	void (*release)(void*) = nullptr;
	void* ctx = nullptr;
};

struct VideoGpu
{
	bool ok = false;
	bool host_import = false;
	size_t host_align = 0;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	Buffer ubo;
	bool linear_ok[5] = {}; // R8, R8G8, R8G8B8A8, R16, R16G16: the hardware's bilinear filter
	bool sampled_ok[5] = {};
	// the plane images and what they hold
	Image planes[3];
	int nplanes = 0;
	VkFormat fmt[3] = {};
	int pw[3] = {}, ph[3] = {};
	bool set_dirty = true;
	// 1.1 review: the pending picture's plane images when its sizes or formats aren't the shown one's, made by SetVideo
	// (so a picture it takes always has them) and swapped in at its upload, when the GPU no longer reads the old ones
	Image next_planes[3];
	int next_n = 0;
	VkFormat next_fmt[3] = {};
	int next_w[3] = {}, next_h[3] = {};
	bool pend_next = false;
	// 1.1 review: this Present recorded the pending picture's upload; it stays pending (its slot or its reference kept)
	// until the submit went through (CommitVideoUpload), so a failed submit leaves it to the next Present
	bool upload_recorded = false;
	VideoFrame meta;  // the pending picture's description (pointers unused)
	VideoFrame shown; // the picture the plane images hold
	bool picture = false; // the images hold a picture
	bool show = false;
	VideoView view;
	// the ring, and what the frame in flight reads
	Buffer ring[kVideoRing];
	int ring_next = 0;
	int ring_busy = -1; // read by the frame in flight
	void (*busy_release)(void*) = nullptr; // an imported picture read by the frame in flight
	void* busy_ctx = nullptr;
	HostMemory* busy_mem = nullptr;
	VideoPending pend;
	// statistics (TakeVideoStats)
	std::mutex stats_mx;
	VideoStats st;
	uint64_t present_count = 0;
	double gpu_ms_sum = 0;
	int gpu_frames = 0;
	bool drawn_this_frame = false;
};
VideoGpu gv;

double NowMs()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// the format slot of a plane: texel bytes and components
int FormatSlot(bool wide, int comps)
{
	return wide ? (comps == 2 ? 4 : 3) : (comps == 1 ? 0 : comps == 2 ? 1 : 2);
}
VkFormat SlotFormat(int s)
{
	static const VkFormat f[5] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16_UNORM,
		VK_FORMAT_R16G16_UNORM};
	return f[s];
}
int SlotOf(VkFormat f)
{
	for (int s = 0; s < 5; s++)
		if (SlotFormat(s) == f)
			return s;
	return 0;
}
int SlotBytes(int s)
{
	static const int b[5] = {1, 2, 4, 2, 4};
	return b[s];
}

// A frame's planes: count, size, components (Packed: 4 bytes a pixel; 16-bit samples: 2 bytes each)
struct PlaneGeom
{
	int n = 0;
	int w[3] = {}, h[3] = {};
	int slot[3] = {};
};
bool Geometry(const VideoFrame& f, PlaneGeom& p)
{
	if (f.width <= 0 || f.height <= 0 || f.width > 8192 || f.height > 8192 || f.bits < 8 || f.bits > 16)
		return false;
	if (f.chroma_shift_x < 0 || f.chroma_shift_x > 2 || f.chroma_shift_y < 0 || f.chroma_shift_y > 2)
		return false;
	const bool wide = f.bits > 8;
	const int cw = (f.width + (1 << f.chroma_shift_x) - 1) >> f.chroma_shift_x;
	const int ch = (f.height + (1 << f.chroma_shift_y) - 1) >> f.chroma_shift_y;
	p.w[0] = f.width;
	p.h[0] = f.height;
	switch (f.layout)
	{
		case VideoLayout::Planar:
			p.n = 3;
			p.w[1] = p.w[2] = cw;
			p.h[1] = p.h[2] = ch;
			p.slot[0] = p.slot[1] = p.slot[2] = FormatSlot(wide, 1);
			break;
		case VideoLayout::PlanarGBR:
			p.n = 3;
			p.w[1] = p.w[2] = f.width;
			p.h[1] = p.h[2] = f.height;
			p.slot[0] = p.slot[1] = p.slot[2] = FormatSlot(wide, 1);
			break;
		case VideoLayout::SemiPlanarUV:
		case VideoLayout::SemiPlanarVU:
			p.n = 2;
			p.w[1] = cw;
			p.h[1] = ch;
			p.slot[0] = FormatSlot(wide, 1);
			p.slot[1] = FormatSlot(wide, 2);
			break;
		case VideoLayout::Gray:
			p.n = 1;
			p.slot[0] = FormatSlot(wide, 1);
			break;
		case VideoLayout::Packed:
			if (wide)
				return false;
			p.n = 1;
			p.slot[0] = FormatSlot(false, 4);
			for (int i = 0; i < 3; i++)
				if (f.rgb_index[i] < 0 || f.rgb_index[i] > 3)
					return false;
			break;
		default:
			return false;
	}
	for (int i = 0; i < p.n; i++)
	{
		if (!gv.sampled_ok[p.slot[i]])
			return false;
		const int64_t row = int64_t(p.w[i]) * SlotBytes(p.slot[i]);
		if (!f.plane[i].data || f.plane[i].pitch < row)
			return false;
	}
	return true;
}

void ReleasePending()
{
	if (gv.pend.set && gv.pend.slot < 0 && gv.pend.release)
		gv.pend.release(gv.pend.ctx);
	gv.pend = VideoPending();
}

// the frame in flight is done (its fence waited): what it read can go
void RetireVideo()
{
	gv.ring_busy = -1;
	gv.busy_mem = nullptr;
	if (gv.busy_release)
	{
		void (*r)(void*) = gv.busy_release;
		gv.busy_release = nullptr;
		r(gv.busy_ctx);
	}
	gv.busy_ctx = nullptr;
}

void FreeNextPlanes()
{
	for (Image& im : gv.next_planes)
		FreeImage(im);
	gv.next_n = 0;
	gv.pend_next = false;
}

void FreeVideoPlanes()
{
	for (Image& im : gv.planes)
		FreeImage(im);
	gv.nplanes = 0;
	gv.picture = false;
	gv.set_dirty = true;
}

void FreeVideo()
{
	ReleasePending();
	RetireVideo();
	FreeVideoPlanes();
	FreeNextPlanes();
	for (Buffer& b : gv.ring)
		FreeBuffer(b);
	FreeBuffer(gv.ubo);
	if (gv.pipe)
		vkDestroyPipeline(g.device, gv.pipe, nullptr);
	if (gv.layout)
		vkDestroyPipelineLayout(g.device, gv.layout, nullptr);
	if (gv.set_layout)
		vkDestroyDescriptorSetLayout(g.device, gv.set_layout, nullptr);
	if (gv.set && g.dpool)
	{
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		vkFreeDescriptorSets(g.device, g.dpool, 1, &gv.set);
	}
	gv.~VideoGpu();
	new (&gv) VideoGpu();
}

// The colour matrix rows (R', G', B' from Y, Cb, Cr: Cb, Cr in -0.5..0.5) for Kr, Kb
void MatrixRows(ColorMatrix m, float* r0, float* r1, float* r2)
{
	double kr = 0.2126, kb = 0.0722;
	switch (m)
	{
		case ColorMatrix::BT601: kr = 0.299, kb = 0.114; break;
		case ColorMatrix::BT2020: kr = 0.2627, kb = 0.0593; break;
		case ColorMatrix::FCC: kr = 0.30, kb = 0.11; break;
		case ColorMatrix::SMPTE240M: kr = 0.212, kb = 0.087; break;
		default: break;
	}
	const double kg = 1.0 - kr - kb;
	r0[0] = 1, r0[1] = 0, r0[2] = float(2 * (1 - kr)), r0[3] = 0;
	r1[0] = 1, r1[1] = float(-2 * kb * (1 - kb) / kg), r1[2] = float(-2 * kr * (1 - kr) / kg), r1[3] = 0;
	r2[0] = 1, r2[1] = float(2 * (1 - kb)), r2[2] = 0, r2[3] = 0;
}

double PqInvD(double nits)
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double y = std::pow(std::max(nits, 0.0) / 10000.0, m1);
	return std::pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

// The uniform block for the picture the images hold, drawn at `out` (output pixels)
void FillVideoUbo(VideoUbo& u, const VideoFrame& f, const VideoView& v, double out_w, double out_h, bool hdr_output = false)
{
	memset(&u, 0, sizeof(u));
	const int cw = gv.nplanes > 1 ? gv.pw[1] : gv.pw[0], ch = gv.nplanes > 1 ? gv.ph[1] : gv.ph[0];
	u.lsize[0] = float(gv.pw[0]), u.lsize[1] = float(gv.ph[0]), u.lsize[2] = 1.0f / gv.pw[0], u.lsize[3] = 1.0f / gv.ph[0];
	u.csize[0] = float(cw), u.csize[1] = float(ch), u.csize[2] = 1.0f / cw, u.csize[3] = 1.0f / ch;
	const double ow = std::max(1.0, out_w), oh = std::max(1.0, out_h);
	u.ratio[0] = float(gv.pw[0] / ow), u.ratio[1] = float(gv.ph[0] / oh);
	u.ratio[2] = float(cw / ow), u.ratio[3] = float(ch / oh);
	// the codes: 8 bits as 0..255 of the texture; 16-bit words as 0..65535, the bits at the top or the bottom
	const int bits = f.bits;
	const double cmax = bits <= 8 ? 255.0 : (f.msb ? 65535.0 / double(1 << (16 - bits)) : 65535.0);
	const double sh = double(1 << (bits - 8));
	double yoff, yrange, coff, crange;
	if (f.full_range || f.layout == VideoLayout::PlanarGBR)
	{
		yoff = 0, yrange = double((1 << bits) - 1);
		coff = double(1 << (bits - 1)), crange = double((1 << bits) - 1);
	}
	else
	{
		yoff = 16 * sh, yrange = 219 * sh;
		coff = 128 * sh, crange = 224 * sh;
	}
	u.norm[0] = float(cmax / yrange), u.norm[1] = float(-yoff / yrange);
	u.norm[2] = float(cmax / crange), u.norm[3] = float(-coff / crange);
	MatrixRows(f.matrix, u.m0, u.m1, u.m2);
	// tone mapping (mpc_tonemap.cpp's numbers): the curve's top is the film's peak (HLG: 1000 nits), never below white
	const double white = std::max(1.0, v.white_nits);
	const double top = f.transfer == VideoTransfer::Hlg ? 1000.0 : std::max(f.peak_nits, white);
	u.tm[0] = float(top), u.tm[1] = float(white), u.tm[2] = float(1.0 / white);
	u.tm[3] = hdr_output ? 1.0f : 0.0f;
	const double src = PqInvD(top), max_lum = PqInvD(white) / src;
	u.pq[0] = float(src), u.pq[1] = float(max_lum), u.pq[2] = float(1.5 * max_lum - 0.5);
	// the chroma siting: a chroma sample covering s luma pixels sits at luma s*j + 0.5 + pos*(s - 1); in chroma texels
	// that is an offset of 0.5 - (0.5 + pos*(s - 1)) / s from Tc * csize (0 when centred)
	if (f.layout == VideoLayout::Planar || f.layout == VideoLayout::SemiPlanarUV || f.layout == VideoLayout::SemiPlanarVU)
	{
		const double sx = double(1 << f.chroma_shift_x), sy = double(1 << f.chroma_shift_y);
		const double px = std::max(0.0, std::min(1.0, f.chroma_pos_x)), py = std::max(0.0, std::min(1.0, f.chroma_pos_y));
		u.coff[0] = float(0.5 - (0.5 + px * (sx - 1)) / sx);
		u.coff[1] = float(0.5 - (0.5 + py * (sy - 1)) / sy);
	}
	int layout = 0;
	switch (f.layout)
	{
		case VideoLayout::Planar: layout = 0; break;
		case VideoLayout::SemiPlanarUV: layout = 1; break;
		case VideoLayout::SemiPlanarVU: layout = 2; break;
		case VideoLayout::Packed: layout = 3; break;
		case VideoLayout::Gray: layout = 4; break;
		case VideoLayout::PlanarGBR: layout = 5; break;
	}
	u.mode[0] = layout;
	u.mode[1] = int(f.transfer);
	u.mode[2] = int(v.tone_map);
	u.mode[3] = v.filter == VideoFilter::Bicubic ? 1 : 0;
	u.swz[0] = f.rgb_index[0], u.swz[1] = f.rgb_index[1], u.swz[2] = f.rgb_index[2];
	bool linear = true;
	for (int i = 0; i < gv.nplanes; i++)
		linear = linear && gv.linear_ok[SlotOf(gv.fmt[i])];
	u.swz[3] = linear ? 1 : 0;
}

bool CurMatches(const PlaneGeom& p)
{
	bool same = gv.nplanes == p.n;
	for (int i = 0; same && i < p.n; i++)
		same = gv.pw[i] == p.w[i] && gv.ph[i] == p.h[i] && gv.fmt[i] == SlotFormat(p.slot[i]) && gv.planes[i].img;
	return same;
}

bool NextMatches(const PlaneGeom& p)
{
	bool same = gv.next_n == p.n;
	for (int i = 0; same && i < p.n; i++)
		same = gv.next_w[i] == p.w[i] && gv.next_h[i] == p.h[i] && gv.next_fmt[i] == SlotFormat(p.slot[i]) &&
			   gv.next_planes[i].img;
	return same;
}

// the plane images a picture of another size or format needs (the GPU never read these: freed at once when replaced).
// Made first: when they can't be, the ones there (the pending picture's) stay.
bool MakeNextPlanes(const PlaneGeom& p)
{
#ifndef __PROSPERO__
	// host tests: PS5GPU_TEST_VIDEO_IMAGE_FAIL=n: the n-th set of plane images made (counting the self-test's two) and
	// every later one can't be
	static int made = 0;
	made++;
	if (const char* e = getenv("PS5GPU_TEST_VIDEO_IMAGE_FAIL"))
		if (atoi(e) > 0 && made >= atoi(e))
			return false;
#endif
	Image fresh[3];
	for (int i = 0; i < p.n; i++)
	{
		if (!MakeImage(fresh[i], p.w[i], p.h[i], SlotFormat(p.slot[i]), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		{
			for (Image& im : fresh)
				FreeImage(im);
			return false;
		}
	}
	const bool pend_next = gv.pend_next;
	FreeNextPlanes();
	gv.pend_next = pend_next; // (SetVideo decides it for the picture it takes)
	for (int i = 0; i < p.n; i++)
	{
		gv.next_planes[i] = fresh[i];
		gv.next_fmt[i] = SlotFormat(p.slot[i]);
		gv.next_w[i] = p.w[i];
		gv.next_h[i] = p.h[i];
	}
	gv.next_n = p.n;
	return true;
}

void UpdateVideoSet()
{
	if (!gv.set_dirty || !gv.nplanes)
		return;
	VkDescriptorImageInfo img[3];
	for (int i = 0; i < 3; i++)
	{
		const int pi = i < gv.nplanes ? i : 0; // a layout with fewer planes: the unused bindings see plane 0
		const int s = SlotOf(gv.fmt[pi]);
		img[i] = {g.samplers[gv.linear_ok[s] ? 1 : 0][1], gv.planes[pi].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	}
	VkDescriptorBufferInfo ub = {gv.ubo.buf, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet w[4] = {};
	for (int i = 0; i < 4; i++)
	{
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].dstSet = gv.set;
		w[i].dstBinding = uint32_t(i);
		w[i].descriptorCount = 1;
		w[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		if (i < 3)
			w[i].pImageInfo = &img[i];
		else
			w[i].pBufferInfo = &ub;
	}
	vkUpdateDescriptorSets(g.device, 4, w, 0, nullptr);
	gv.set_dirty = false;
}

// The pending picture's copies into the plane images (before the render pass). Nothing is in flight now.
void RecordVideoUpload(VkCommandBuffer cmd)
{
	if (!gv.pend.set)
		return;
	PlaneGeom p;
	VideoFrame probe = gv.meta;
	// the geometry again from the description (the pointers were checked when it was taken)
	for (int i = 0; i < 3; i++)
		probe.plane[i] = {reinterpret_cast<const void*>(uintptr_t(1)), 1 << 30};
	if (gv.pend_next && Geometry(probe, p) && NextMatches(p))
	{
		// the frame that read the old images is done (Present waited for it): they go, the new ones take their place
		FreeVideoPlanes();
		for (int i = 0; i < 3; i++)
		{
			gv.planes[i] = gv.next_planes[i];
			gv.next_planes[i] = Image();
			gv.fmt[i] = gv.next_fmt[i];
			gv.pw[i] = gv.next_w[i];
			gv.ph[i] = gv.next_h[i];
		}
		gv.nplanes = gv.next_n;
		gv.next_n = 0;
		gv.pend_next = false;
		gv.set_dirty = true;
		internal::Log("[gpu] video planes: %d (%dx%d%s)", p.n, p.w[0], p.h[0],
			p.n > 1 ? (", chroma " + std::to_string(p.w[1]) + "x" + std::to_string(p.h[1])).c_str() : "");
	}
	else if (!Geometry(probe, p) || !CurMatches(p))
	{
		// (SetVideo always leaves the images a picture needs: not expected)
		internal::Log("[gpu] video: no plane images for a %dx%d picture: not drawn", gv.meta.width, gv.meta.height);
		ReleasePending();
		gv.picture = false;
		return;
	}
	VkBuffer src = gv.pend.slot >= 0 ? gv.ring[gv.pend.slot].buf : gv.pend.mem->b.buf;
	for (int i = 0; i < p.n; i++)
	{
		Barrier(cmd, gv.planes[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy r = {};
		r.bufferOffset = gv.pend.off[i];
		r.bufferRowLength = gv.pend.row_texels[i];
		r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.imageExtent = {uint32_t(p.w[i]), uint32_t(p.h[i]), 1};
		vkCmdCopyBufferToImage(cmd, src, gv.planes[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
		Barrier(cmd, gv.planes[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	// (what this frame reads becomes busy once it is submitted: CommitVideoUpload)
	gv.upload_recorded = true;
	gv.shown = gv.meta;
	gv.picture = true;
}

// The frame with the pending picture's upload was submitted: what it reads is busy until its fence, the picture no
// longer pending.
void CommitVideoUpload()
{
	if (!gv.upload_recorded)
		return;
	gv.upload_recorded = false;
	if (gv.pend.slot >= 0)
		gv.ring_busy = gv.pend.slot;
	else
	{
		gv.busy_mem = gv.pend.mem;
		gv.busy_release = gv.pend.release;
		gv.busy_ctx = gv.pend.ctx;
	}
	gv.pend = VideoPending();
}

// 1.1 review: the frame couldn't be submitted: nothing of it reached the GPU. A picture whose upload it held stays
// pending (slot, reference and all) for the next Present, its images' contents unknown until then (the upload starts
// from UNDEFINED); without an upload the images still hold the shown picture (a paused one stays on screen). The UI
// surface's copy didn't happen either: copied whole next time.
void VideoSubmitFailed()
{
	if (gv.upload_recorded)
	{
		for (Image& im : gv.planes)
			if (im.img)
				im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
		gv.picture = false;
		gv.upload_recorded = false;
	}
	g.ui.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	g.ui_full = true;
	// (final review 2) the game's frame too: the layouts its recording tracked never happened. Its picture is uploaded
	// again from the staging buffer (which still holds it); the history and the passes' outputs start from UNDEFINED
	// (their contents for one frame lost: a feedback or history pass sees black once). The LUTs are never touched by a
	// frame's recording.
	if (g.frame_drew_game)
	{
		g.frame_drew_game = false;
		if (g.original.img)
			g.original.layout = VK_IMAGE_LAYOUT_UNDEFINED;
		for (Image& im : g.game_chain.hist)
			if (im.img)
				im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
		for (PassRt& p : g.game_chain.passes)
			for (Image& im : p.out)
				if (im.img)
					im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
		g.game_new = true;
	}
}

// The picture into the render pass (after the clear, before the UI surface)
void RecordVideoDraw(VkCommandBuffer cmd)
{
	gv.drawn_this_frame = false;
	if (!gv.ok || !gv.show || !gv.picture || gv.nplanes == 0)
		return;
	const Rect d = gv.view.dst;
	if (d.w <= 0 || d.h <= 0)
		return;
	const double sx = double(g.out_w) / g.ui_w, sy = double(g.out_h) / g.ui_h;
	const double x0 = d.x * sx, y0 = d.y * sy, x1 = (d.x + d.w) * sx, y1 = (d.y + d.h) * sy;
	const int cx0 = std::max(0, int(std::floor(x0 + 0.5))), cy0 = std::max(0, int(std::floor(y0 + 0.5)));
	const int cx1 = std::min(g.out_w, int(std::floor(x1 + 0.5))), cy1 = std::min(g.out_h, int(std::floor(y1 + 0.5)));
	if (cx1 <= cx0 || cy1 <= cy0)
		return;
	VideoUbo u;
	FillVideoUbo(u, gv.shown, gv.view, x1 - x0, y1 - y0, g.hdr_active);
	memcpy(gv.ubo.map, &u, sizeof(u));
	UpdateVideoSet();
	const float rect[4] = {float(x0 / g.out_w * 2 - 1), float(y0 / g.out_h * 2 - 1), float(x1 / g.out_w * 2 - 1),
		float(y1 / g.out_h * 2 - 1)};
	const VkViewport view = {0, 0, float(g.out_w), float(g.out_h), 0, 1};
	const VkRect2D sc = {{cx0, cy0}, {uint32_t(cx1 - cx0), uint32_t(cy1 - cy0)}};
	vkCmdSetViewport(cmd, 0, 1, &view);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.hdr_active ? g.hdr_video_pipe : gv.pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gv.layout, 0, 1, &gv.set, 0, nullptr);
	vkCmdPushConstants(cmd, gv.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rect), rect);
	vkCmdDraw(cmd, 4, 1, 0, 0);
	gv.drawn_this_frame = true;
}

bool BuildVideoPipeline(VkRenderPass rp, VkPipeline* out)
{
	VkShaderModule vs = Module(kVideoVs, sizeof(kVideoVs)), fs = Module(kVideoFs, sizeof(kVideoFs));
	bool ok = vs && fs;
	if (ok)
	{
		VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
		st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		st[0].module = vs;
		st[0].pName = "main";
		st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		st[1].module = fs;
		st[1].pName = "main";
		VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState ba = {};
		ba.colorWriteMask = 0xf;
		VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &ba;
		VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		ds.dynamicStateCount = 2;
		ds.pDynamicStates = dyn;
		VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pi.stageCount = 2;
		pi.pStages = st;
		pi.pVertexInputState = &vi;
		pi.pInputAssemblyState = &ia;
		pi.pViewportState = &vp;
		pi.pRasterizationState = &rs;
		pi.pMultisampleState = &ms;
		pi.pColorBlendState = &cb;
		pi.pDynamicState = &ds;
		pi.layout = gv.layout;
		pi.renderPass = rp;
		ok = Check(vkCreateGraphicsPipelines(g.device, g.pcache, 1, &pi, nullptr, out), "vkCreateGraphicsPipelines (video)");
	}
	if (vs)
		vkDestroyShaderModule(g.device, vs, nullptr);
	if (fs)
		vkDestroyShaderModule(g.device, fs, nullptr);
	return ok;
}

// HDR10 uses the same presentation method as VLC-PS5: typed 10-bit rendering, then a size-compatible raw image
// copy into the WSI's 32-bit buffers after VideoOut changes their scanout attribute. No 8-bit render or quantization.
void FreeHdr()
{
	FreeChain(g.hdr_ui_chain);
	if (g.hdr_video_pipe) vkDestroyPipeline(g.device, g.hdr_video_pipe, nullptr);
	g.hdr_video_pipe = VK_NULL_HANDLE;
	FreeImage(g.hdr_target);
	if (g.hdr_pass) vkDestroyRenderPass(g.device, g.hdr_pass, nullptr);
	g.hdr_pass = VK_NULL_HANDLE;
}

bool MakeHdr()
{
	if (g.hdr_pass) return true;
	const VkFormat format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	VkFormatProperties props = {};
	vkGetPhysicalDeviceFormatProperties(g.phys, format, &props);
	const auto need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	if ((props.optimalTilingFeatures & need) != need) return false;
	bool ok = MakeImage(g.hdr_target, g.out_w, g.out_h, format,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	if (ok)
	{
		g.hdr_pass = MakeRenderPass(format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true);
		g.hdr_target.fb = MakeFramebuffer(g.hdr_pass, g.hdr_target.view, g.out_w, g.out_h);
		ok = g.hdr_pass && g.hdr_target.fb && BuildVideoPipeline(g.hdr_pass, &g.hdr_video_pipe) &&
			BuildChain(g.hdr_ui_chain, 1, true, g.hdr_pass);
	}
	if (!ok) FreeHdr();
	return ok;
}

bool ChangeHdr(bool on)
{
	const auto& c = CurrentConfig();
	if (on && !MakeHdr()) return false;
	// No frame in the previous pixel format may be submitted after VideoOut switches.
	if (!Check(vkDeviceWaitIdle(g.device), "vkDeviceWaitIdle (HDR switch)")) return false;
	const int rc = c.set_hdr_output ? c.set_hdr_output(on, g.out_w, g.out_h) : g.offscreen ? 0 : -1;
	internal::Log("[gpu] HDR10 output %s: %s (%#x)", on ? "on" : "off", rc == 0 ? "accepted" : "refused", unsigned(rc));
	if (rc != 0) return false; // Keep writing the format that VideoOut last accepted.
	g.hdr_active = on;
	g.hdr_check_frames = on && c.hdr_output_range ? 60 : 0;
	return true;
}

void UpdateHdr()
{
	const auto& c = CurrentConfig();
	const auto& f = gv.pend.set ? gv.meta : gv.shown;
	const bool wish = c.hdr_output && gv.ok && gv.show && (gv.pend.set || gv.picture) &&
		f.transfer != VideoTransfer::Sdr && !g.game && !g.scene_on && !g.ui_straight;
	const double now = NowMs();
	if (wish != g.hdr_wish)
	{
		g.hdr_wish = wish;
		g.hdr_wish_since = now;
		if (!wish) g.hdr_refused = false;
	}
	if (g.hdr_check_frames > 0 && --g.hdr_check_frames == 0 && c.hdr_output_range() != 2)
	{
		internal::Log("[gpu] HDR10 was not confirmed by the display: restoring SDR tone mapping");
		g.hdr_refused = true;
		if (!ChangeHdr(false)) g.hdr_broken = true;
	}
	const double hold = c.sync_build ? 0 : wish ? 400 : 1000;
	if (wish != g.hdr_active && !(wish && g.hdr_refused) && now - g.hdr_wish_since >= hold)
	{
		if (!ChangeHdr(wish))
		{
			if (wish) g.hdr_refused = true;
			else g.hdr_broken = true; // Never send SDR pixels while VideoOut still expects PQ (or vice versa).
		}
	}
}

void CopyHdrToOutput(VkCommandBuffer cmd, uint32_t index)
{
	VkImage dst = g.offscreen ? g.offscreen_target.img : g.swap_images[index];
	VkImageMemoryBarrier barriers[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
	for (auto& b : barriers)
	{
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	}
	barriers[0].image = g.hdr_target.img;
	barriers[0].oldLayout = barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[1].image = dst;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 2, barriers);
	VkImageCopy copy = {};
	copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.extent = {uint32_t(g.out_w), uint32_t(g.out_h), 1};
	vkCmdCopyImage(cmd, g.hdr_target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].newLayout = g.offscreen ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = g.offscreen ? VK_ACCESS_TRANSFER_READ_BIT : 0;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		g.offscreen ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barriers[1]);
}

// The host callback stays BGRA8; decode the raw 10-bit copy only for its preview, never on the console.
void HostPresent(bool vsync)
{
	const auto callback = CurrentConfig().host_present;
	if (!callback) return;
	const uint8_t* pixels = static_cast<const uint8_t*>(g.readback.map);
	if (g.hdr_readback)
	{
		const size_t count = size_t(g.out_w) * g.out_h;
		g.hdr_host_pixels.resize(count * 4);
		for (size_t i = 0; i < count; i++)
		{
			uint32_t p;
			memcpy(&p, pixels + 4 * i, 4);
			for (int k = 0; k < 3; k++)
				g.hdr_host_pixels[i * 4 + k] = uint8_t((((p >> (k * 10)) & 1023u) * 255 + 511) / 1023);
			g.hdr_host_pixels[i * 4 + 3] = 255;
		}
		pixels = g.hdr_host_pixels.data();
	}
	callback(pixels, g.out_w, g.out_h, vsync);
}

// The pipeline's self-test: a 4x2 NV12 picture of known colours and a 16-bit planar one drawn into an image of the
// target's format and read back, against the same arithmetic on the CPU. A driver that gets them wrong leaves the
// pictures to the program's CPU path.
bool VideoSelfTest();

void InitVideo(bool host_import)
{
	for (int s = 0; s < 5; s++)
	{
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(g.phys, SlotFormat(s), &fp);
		gv.sampled_ok[s] = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
						   (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
		gv.linear_ok[s] = gv.sampled_ok[s] && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	}
	if (!gv.sampled_ok[0] || !gv.sampled_ok[1])
	{
		internal::Log("[gpu] video: the driver can't sample R8 / R8G8 images: pictures stay on the CPU");
		return;
	}
	VkDescriptorSetLayoutBinding b[4] = {};
	for (int i = 0; i < 4; i++)
	{
		b[i].binding = uint32_t(i);
		b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = 4;
	li.pBindings = b;
	VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pc;
	bool ok = Check(vkCreateDescriptorSetLayout(g.device, &li, nullptr, &gv.set_layout), "vkCreateDescriptorSetLayout (video)");
	pli.pSetLayouts = &gv.set_layout;
	ok = ok && Check(vkCreatePipelineLayout(g.device, &pli, nullptr, &gv.layout), "vkCreatePipelineLayout (video)");
	ok = ok && BuildVideoPipeline(g.final_pass, &gv.pipe);
	ok = ok && MakeBuffer(gv.ubo, sizeof(VideoUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	if (ok)
	{
		VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = g.dpool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &gv.set_layout;
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		ok = Check(vkAllocateDescriptorSets(g.device, &ai, &gv.set), "vkAllocateDescriptorSets (video)");
	}
	if (ok && host_import)
	{
		VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
		VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		p2.pNext = &hp;
		vkGetPhysicalDeviceProperties2(g.phys, &p2);
		gv.host_align = size_t(hp.minImportedHostPointerAlignment);
		gv.host_import = vkGetMemoryHostPointerPropertiesEXT && gv.host_align > 0;
	}
	if (!ok)
	{
		internal::Log("[gpu] video: set-up failed: pictures stay on the CPU");
		FreeVideo();
		return;
	}
	gv.ok = true; // (the self-test draws through SetVideo's own path)
	if (!VideoSelfTest())
	{
		internal::Log("[gpu] video: the self-test's pictures came out wrong on this driver: pictures stay on the CPU");
		FreeVideo();
		return;
	}
	{
		std::lock_guard<std::mutex> l(gv.stats_mx); // the self-test's pictures aren't the program's
		gv.st = VideoStats();
	}
	internal::Log("[gpu] video: ready (16-bit planes %s, bilinear on 16-bit %s; pictures read in place: %s, alignment %zu)",
		gv.sampled_ok[3] && gv.sampled_ok[4] ? "yes" : "no", gv.linear_ok[3] && gv.linear_ok[4] ? "yes" : "no",
		gv.host_import ? "yes (VK_EXT_external_memory_host)" : "no (one copy each)", gv.host_align);
}
} // namespace

bool HdrOutputActive() { return g.ok && g.hdr_active; }

bool VideoAvailable()
{
	return g.ok && gv.ok;
}

bool VideoFormatSupported(const VideoFrame& f)
{
	PlaneGeom p;
	return VideoAvailable() && Geometry(f, p);
}

size_t HostMemoryAlignment()
{
	return VideoAvailable() && gv.host_import ? gv.host_align : 0;
}

HostMemory* ImportHostMemory(void* base, size_t bytes)
{
	if (!VideoAvailable() || !gv.host_import || !base || !bytes || uintptr_t(base) % gv.host_align || bytes % gv.host_align)
		return nullptr;
	VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
	if (vkGetMemoryHostPointerPropertiesEXT(g.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hp) !=
		VK_SUCCESS)
	{
		internal::Log("[gpu] video: the driver won't import %zu KiB at %p", bytes >> 10, base);
		return nullptr;
	}
	HostMemory* m = new (std::nothrow) HostMemory;
	if (!m)
		return nullptr;
	VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
	eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.pNext = &eb;
	bi.size = bytes;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bool ok = Check(vkCreateBuffer(g.device, &bi, nullptr, &m->b.buf), "vkCreateBuffer (imported)");
	VkMemoryRequirements req = {};
	if (ok)
		vkGetBufferMemoryRequirements(g.device, m->b.buf, &req);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; ok && i < g.memprops.memoryTypeCount; i++)
		if ((hp.memoryTypeBits & req.memoryTypeBits & (1u << i)))
		{
			type = i;
			break;
		}
	ok = ok && type != UINT32_MAX;
	if (ok)
	{
		VkImportMemoryHostPointerInfoEXT ih = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
		ih.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
		ih.pHostPointer = base;
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext = &ih;
		ai.allocationSize = bytes;
		ai.memoryTypeIndex = type;
		ok = Check(vkAllocateMemory(g.device, &ai, nullptr, &m->b.mem), "vkAllocateMemory (imported)") &&
			 Check(vkBindBufferMemory(g.device, m->b.buf, m->b.mem, 0), "vkBindBufferMemory (imported)");
	}
	if (!ok)
	{
		FreeBuffer(m->b);
		delete m;
		return nullptr;
	}
	m->b.map = base;
	m->b.size = bytes;
	m->bytes = bytes;
	internal::Log("[gpu] video: %zu KiB at %p imported (the GPU reads the pictures there)", bytes >> 10, base);
	return m;
}

void ReleaseHostMemory(HostMemory* m)
{
	if (!m)
		return;
	if (g.ok)
	{
		// the frame in flight, or the one waiting for its upload, may read it
		if (gv.busy_mem == m && g.in_flight)
		{
			vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
			g.in_flight = false;
			RetireVideo();
		}
		if (gv.pend.set && gv.pend.mem == m)
			ReleasePending();
		m->b.map = nullptr; // not ours to unmap
		FreeBuffer(m->b);
	}
	delete m;
}

bool SetVideo(const VideoFrame& f, const VideoView& view)
{
	auto refuse = [&](const char* why) {
		static int logged = 0;
		if (why && logged++ < 8)
			internal::Log("[gpu] video: a %dx%d picture not taken: %s", f.width, f.height, why);
		if (f.release)
			f.release(f.release_ctx);
		std::lock_guard<std::mutex> l(gv.stats_mx);
		gv.st.refused++;
		return false;
	};
	if (!VideoAvailable())
		return refuse(nullptr);
	PlaneGeom p;
	if (!Geometry(f, p))
		return refuse("a layout, size or pitch the GPU path doesn't take");
	// Everything that can refuse it comes before anything changes (1.1 review: a refused picture leaves the pending one
	// and its plane images as they were): where its planes are read from, then its plane images.
	VideoPending np;
	np.set = true;
	int slot = -1;
	VkDeviceSize off[3] = {};
	uint32_t rows[3] = {};
	bool whole[3] = {};
	if (f.memory)
	{
		// in place: each plane inside the imported block, at offsets and pitches a buffer-to-image copy takes
		const uint8_t* base = static_cast<const uint8_t*>(f.memory->b.map);
		bool fits = base != nullptr;
		for (int i = 0; fits && i < p.n; i++)
		{
			const int tb = SlotBytes(p.slot[i]);
			const uint8_t* d = static_cast<const uint8_t*>(f.plane[i].data);
			const size_t last = size_t(f.plane[i].pitch) * (p.h[i] - 1) + size_t(p.w[i]) * tb;
			fits = d >= base && size_t(d - base) + last <= f.memory->bytes && size_t(d - base) % 4 == 0 &&
				   size_t(d - base) % tb == 0 && f.plane[i].pitch % tb == 0;
			if (fits)
			{
				np.off[i] = VkDeviceSize(d - base);
				np.row_texels[i] = uint32_t(f.plane[i].pitch / tb);
			}
		}
		if (!fits)
			return refuse("its planes aren't inside the imported memory as a copy needs them");
	}
	else
	{
		// one copy, into a slot neither the frame in flight nor the pending picture reads: whole planes at their pitch
		// where it suits a copy
		for (int k = 0; k < kVideoRing && slot < 0; k++)
		{
			const int s = (gv.ring_next + k) % kVideoRing;
			if (s != gv.ring_busy && !(gv.pend.set && gv.pend.slot == s))
				slot = s;
		}
		VkDeviceSize need = 0;
		for (int i = 0; i < p.n; i++)
		{
			const int tb = SlotBytes(p.slot[i]);
			whole[i] = f.plane[i].pitch % tb == 0;
			const VkDeviceSize pitch = whole[i] ? VkDeviceSize(f.plane[i].pitch) : VkDeviceSize(p.w[i]) * tb;
			need = (need + 15) & ~VkDeviceSize(15);
			off[i] = need;
			rows[i] = uint32_t(pitch / tb);
			need += pitch * VkDeviceSize(p.h[i]);
		}
		if (slot < 0)
			return refuse("no staging slot free");
		Buffer& b = gv.ring[slot];
		if (b.size < need)
		{
			FreeBuffer(b);
			if (!MakeBuffer(b, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
			{
				FreeBuffer(b);
				return refuse("no staging buffer for it");
			}
		}
	}
	// its plane images now (review M1): a picture taken is a picture the next Present can draw
	const bool use_next = !CurMatches(p);
	if (use_next && !NextMatches(p) && !MakeNextPlanes(p))
		return refuse("no memory for its plane images");
	const double t0 = NowMs();
	if (f.memory)
	{
		np.mem = f.memory;
		np.release = f.release;
		np.ctx = f.release_ctx;
	}
	else
	{
		uint8_t* dst = static_cast<uint8_t*>(gv.ring[slot].map);
		for (int i = 0; i < p.n; i++)
		{
			const uint8_t* src = static_cast<const uint8_t*>(f.plane[i].data);
			const int tb = SlotBytes(p.slot[i]);
			if (whole[i])
				memcpy(dst + off[i], src, size_t(f.plane[i].pitch) * (p.h[i] - 1) + size_t(p.w[i]) * tb);
			else
				for (int y = 0; y < p.h[i]; y++)
					memcpy(dst + off[i] + size_t(y) * p.w[i] * tb, src + size_t(y) * f.plane[i].pitch, size_t(p.w[i]) * tb);
			np.off[i] = off[i];
			np.row_texels[i] = rows[i];
		}
		np.slot = slot;
		gv.ring_next = (slot + 1) % kVideoRing;
		if (f.release)
			f.release(f.release_ctx);
	}
	// the pending picture this replaces (never uploaded) is let go; plane images made for it and not needed now go too
	ReleasePending();
	if (!use_next)
		FreeNextPlanes();
	gv.pend = np;
	gv.pend_next = use_next;
	gv.meta = f;
	for (VideoPlane& pl : gv.meta.plane)
		pl = VideoPlane();
	gv.meta.memory = nullptr;
	gv.meta.release = nullptr;
	gv.meta.release_ctx = nullptr;
	gv.view = view;
	gv.show = true;
	const double dt = NowMs() - t0;
	std::lock_guard<std::mutex> l(gv.stats_mx);
	gv.st.frames++;
	if (f.memory)
		gv.st.in_place++;
	else
	{
		gv.st.copied++;
		gv.st.copy_ms += dt;
	}
	return true;
}

void SetVideoView(const VideoView& view)
{
	gv.view = view;
	if (gv.picture || gv.pend.set)
		gv.show = true;
}

void HideVideo()
{
	gv.show = false;
	ReleasePending();
}

namespace
{
void WaitFrame();
}

void ReleaseVideo()
{
	if (!g.ok)
		return;
	if (g.in_flight)
		WaitFrame();
	RetireVideo();
	ReleasePending();
	gv.show = false;
}

VideoStats TakeVideoStats()
{
	std::lock_guard<std::mutex> l(gv.stats_mx);
	VideoStats s = gv.st;
	if (s.copied)
		s.copy_ms /= double(s.copied);
	if (gv.present_count)
	{
		s.present_ms /= double(gv.present_count);
		s.wait_ms /= double(gv.present_count);
	}
	s.gpu_ms = gv.gpu_frames ? gv.gpu_ms_sum / gv.gpu_frames : 0;
	gv.st = VideoStats();
	gv.present_count = 0;
	gv.gpu_ms_sum = 0;
	gv.gpu_frames = 0;
	return s;
}

namespace
{
// (VideoSelfTest) the expected R'G'B' of a flat picture, as the shader computes it, 0..255
void ExpectRgb(const VideoFrame& f, double y, double cb, double cr, double* rgb)
{
	VideoUbo u;
	FillVideoUbo(u, f, VideoView(), 1, 1);
	const double Y = y * u.norm[0] + u.norm[1], Cb = cb * u.norm[2] + u.norm[3], Cr = cr * u.norm[2] + u.norm[3];
	const float* m[3] = {u.m0, u.m1, u.m2};
	for (int i = 0; i < 3; i++)
		rgb[i] = std::max(0.0, std::min(1.0, m[i][0] * Y + m[i][1] * Cb + m[i][2] * Cr)) * 255.0;
}

bool VideoSelfTest()
{
	constexpr int kW = 8, kH = 4;
	Image target;
	Buffer rb;
	VkRenderPass rp = MakeRenderPass(g.target_format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true);
	bool ok = rp && MakeImage(target, kW, kH, g.target_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
			  MakeBuffer(rb, kW * kH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	VkFramebuffer fb = ok ? MakeFramebuffer(rp, target.view, kW, kH) : VK_NULL_HANDLE;
	ok = ok && fb;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	if (ok)
	{
		VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = g.upload_pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		std::lock_guard<std::mutex> l(g.upload_mx);
		ok = Check(vkAllocateCommandBuffers(g.device, &cai, &cmd), "vkAllocateCommandBuffers (video test)") &&
			 Check(vkCreateFence(g.device, &fi, nullptr, &fence), "vkCreateFence (video test)");
	}
	// two pictures: 8-bit NV12 (BT.709, limited) and 10-bit planar 4:4:4 at the bottom of 16-bit words (BT.2020,
	// full); each a flat colour, so the filter can't change it
	struct Case
	{
		VideoLayout layout;
		int bits, shift;
		ColorMatrix m;
		bool full;
		int y, cb, cr;
	};
	const Case cases[2] = {{VideoLayout::SemiPlanarUV, 8, 1, ColorMatrix::BT709, false, 81, 90, 240},
		{VideoLayout::Planar, 10, 0, ColorMatrix::BT2020, true, 600, 300, 700}};
	int worst = 0;
	for (int ci = 0; ok && ci < 2; ci++)
	{
		const Case& c = cases[ci];
		if (c.bits > 8 && !gv.sampled_ok[3])
			continue;
		std::vector<uint8_t> y8(kW * kH), uv8(kW * kH);
		std::vector<uint16_t> y16(kW * kH), u16(kW * kH), v16(kW * kH);
		VideoFrame f;
		f.width = kW;
		f.height = kH;
		f.layout = c.layout;
		f.bits = c.bits;
		f.chroma_shift_x = f.chroma_shift_y = c.shift;
		f.matrix = c.m;
		f.full_range = c.full;
		if (c.bits == 8)
		{
			std::fill(y8.begin(), y8.end(), uint8_t(c.y));
			for (size_t i = 0; i < uv8.size(); i += 2)
				uv8[i] = uint8_t(c.cb), uv8[i + 1] = uint8_t(c.cr);
			f.plane[0] = {y8.data(), kW};
			f.plane[1] = {uv8.data(), kW};
		}
		else
		{
			std::fill(y16.begin(), y16.end(), uint16_t(c.y));
			std::fill(u16.begin(), u16.end(), uint16_t(c.cb));
			std::fill(v16.begin(), v16.end(), uint16_t(c.cr));
			f.plane[0] = {y16.data(), kW * 2};
			f.plane[1] = {u16.data(), kW * 2};
			f.plane[2] = {v16.data(), kW * 2};
		}
		VideoView v;
		v.dst = {0, 0, g.ui_w, g.ui_h};
		if (!SetVideo(f, v))
		{
			ok = false;
			break;
		}
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &bi);
		RecordVideoUpload(cmd);
		VkClearValue cv = {};
		VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rbi.renderPass = rp;
		rbi.framebuffer = fb;
		rbi.renderArea = {{0, 0}, {uint32_t(kW), uint32_t(kH)}};
		rbi.clearValueCount = 1;
		rbi.pClearValues = &cv;
		vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		// drawn as if the output were kW x kH
		const int ow = g.out_w, oh = g.out_h;
		g.out_w = kW, g.out_h = kH;
		RecordVideoDraw(cmd);
		g.out_w = ow, g.out_h = oh;
		vkCmdEndRenderPass(cmd);
		target.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkBufferImageCopy r = {};
		r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.imageExtent = {uint32_t(kW), uint32_t(kH), 1};
		vkCmdCopyImageToBuffer(cmd, target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
		vkEndCommandBuffer(cmd);
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		vkResetFences(g.device, 1, &fence);
		{
			std::lock_guard<std::mutex> l(g.queue_mx);
			ok = Check(vkQueueSubmit(g.queue, 1, &si, fence), "vkQueueSubmit (video test)");
		}
		ok = ok && vkWaitForFences(g.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
		RetireVideo();
		if (!ok)
			break;
		const double code_y = c.bits == 8 ? c.y / 255.0 : c.y / 65535.0, code_cb = c.bits == 8 ? c.cb / 255.0 : c.cb / 65535.0,
					 code_cr = c.bits == 8 ? c.cr / 255.0 : c.cr / 65535.0;
		double want[3];
		ExpectRgb(f, code_y, code_cb, code_cr, want);
		const bool bgra = g.target_format == VK_FORMAT_B8G8R8A8_UNORM;
		const uint8_t* px = static_cast<const uint8_t*>(rb.map);
		for (int i = 0; i < kW * kH; i++)
		{
			const int got[3] = {px[i * 4 + (bgra ? 2 : 0)], px[i * 4 + 1], px[i * 4 + (bgra ? 0 : 2)]};
			for (int k = 0; k < 3; k++)
				worst = std::max(worst, int(std::lround(std::fabs(got[k] - want[k]))));
		}
		internal::Log("[gpu] video self-test %d: want %.0f %.0f %.0f, got %d %d %d", ci, want[0], want[1], want[2],
			px[bgra ? 2 : 0], px[1], px[bgra ? 0 : 2]);
	}
	gv.show = false;
	gv.picture = false;
	if (cmd)
	{
		std::lock_guard<std::mutex> l(g.upload_mx);
		vkFreeCommandBuffers(g.device, g.upload_pool, 1, &cmd);
	}
	if (fence)
		vkDestroyFence(g.device, fence, nullptr);
	if (fb)
		vkDestroyFramebuffer(g.device, fb, nullptr);
	FreeImage(target);
	FreeBuffer(rb);
	if (rp)
		vkDestroyRenderPass(g.device, rp, nullptr);
	return ok && worst <= 2;
}
} // namespace

bool Init(int ui_w, int ui_h, bool offscreen, int out_w, int out_h)
{
	if (g.ok)
		return true;
	if (ui_w <= 0 || ui_h <= 0 || (offscreen && (out_w <= 0 || out_h <= 0)))
	{
		internal::Log("[gpu] Init: no surface size (%dx%d, output %dx%d)", ui_w, ui_h, out_w, out_h);
		return false;
	}
	const Config& cfg = CurrentConfig();
	g.offscreen = offscreen;
	g.ui_w = ui_w;
	g.ui_h = ui_h;
	g.ui_straight = !cfg.ui_premultiplied;
	g.ui_format = cfg.ui_format == UiFormat::BGRA8 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
#define GPU_LOAD_GLOBAL(name) vk##name = reinterpret_cast<PFN_vk##name>(GPU_GET_INSTANCE_PROC(nullptr, "vk" #name));
	internal::Log("[gpu] Init: the driver's entry points"); // (each step logged: a crash names the last one)
	GPU_GLOBAL_FNS(GPU_LOAD_GLOBAL)
#undef GPU_LOAD_GLOBAL
	if (!vkCreateInstance)
	{
		internal::Log("[gpu] no Vulkan driver");
		return false;
	}
	uint32_t n = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> iexts(n);
	vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
	std::vector<const char*> want;
	if (!offscreen)
	{
		if (!HasExtension(iexts, VK_KHR_SURFACE_EXTENSION_NAME) || !HasExtension(iexts, VK_KHR_DISPLAY_EXTENSION_NAME))
		{
			internal::Log("[gpu] the driver has no VK_KHR_display");
			return false;
		}
		want = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
	}
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = cfg.app_name.c_str();
	app.pEngineName = "ps5-slang-gpu";
	app.apiVersion = VK_API_VERSION_1_0;
	// (Config::video) the host-pointer import's properties (minImportedHostPointerAlignment) come through
	// VK_KHR_get_physical_device_properties2 on the 1.0 instance (a 1.1 instance crashed the PS5's driver: see
	// vkGetPhysicalDeviceProperties2 above); its buffers need VK_KHR_external_memory_capabilities on the instance
	bool props2 = false;
	if (cfg.video && cfg.host_import && HasExtension(iexts, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) &&
		HasExtension(iexts, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME))
	{
		want.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
		want.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
		props2 = true;
	}
	{
		std::string list;
		for (const char* w : want)
			list += std::string(" ") + w;
		internal::Log("[gpu] Init: %u instance extensions; enabled:%s", n, list.empty() ? " none" : list.c_str());
	}
	VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ii.pApplicationInfo = &app;
	ii.enabledExtensionCount = uint32_t(want.size());
	ii.ppEnabledExtensionNames = want.data();
	internal::Log("[gpu] Init: instance (Vulkan 1.%u)", VK_API_VERSION_MINOR(app.apiVersion));
	if (!Check(vkCreateInstance(&ii, nullptr, &g.instance), "vkCreateInstance"))
		return false;
	// (MPC 1.6 test10 on the PS5: the process died in here with no more said -- every sub-step is logged now, and
	// the log is written out at once, so the next report names the call)
	internal::Log("[gpu] Init: the GPUs: the instance's entry points");
#define GPU_LOAD_INSTANCE(name) vk##name = reinterpret_cast<PFN_vk##name>(GPU_GET_INSTANCE_PROC(g.instance, "vk" #name));
	GPU_INSTANCE_FNS(GPU_LOAD_INSTANCE)
	if (!offscreen)
	{
		GPU_DISPLAY_FNS(GPU_LOAD_INSTANCE)
	}
#undef GPU_LOAD_INSTANCE
	if (props2)
	{
		vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
			GPU_GET_INSTANCE_PROC(g.instance, "vkGetPhysicalDeviceProperties2KHR"));
		if (!vkGetPhysicalDeviceProperties2)
			vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
				GPU_GET_INSTANCE_PROC(g.instance, "vkGetPhysicalDeviceProperties2"));
	}
	internal::Log("[gpu] Init: the GPUs: counting them");
	uint32_t pc = 0;
	if (!Check(vkEnumeratePhysicalDevices(g.instance, &pc, nullptr), "vkEnumeratePhysicalDevices (count)"))
	{
		Shutdown();
		return false;
	}
	if (pc == 0)
	{
		internal::Log("[gpu] no GPU");
		Shutdown();
		return false;
	}
	internal::Log("[gpu] Init: the GPUs: %u, listing them", pc);
	std::vector<VkPhysicalDevice> devs(pc);
	if (!Check(vkEnumeratePhysicalDevices(g.instance, &pc, devs.data()), "vkEnumeratePhysicalDevices (list)"))
	{
		Shutdown();
		return false;
	}
	g.phys = devs[0];
	internal::Log("[gpu] Init: the GPUs: the first one's properties");
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(g.phys, &props);
	snprintf(g.device_name, sizeof(g.device_name), "%s", props.deviceName);
	g.ts_period = props.limits.timestampPeriod;
	internal::Log("[gpu] Init: the GPUs: %s, Vulkan %u.%u, driver %#x; its memory", g.device_name,
		VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), props.driverVersion);
	vkGetPhysicalDeviceMemoryProperties(g.phys, &g.memprops);
	internal::Log("[gpu] Init: the GPUs: %u memory heaps; its queue families", g.memprops.memoryHeapCount);
	uint32_t qc = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qc, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qc);
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qc, qf.data());
	internal::Log("[gpu] Init: the GPUs: %u queue families; its extensions", qc);
	g.family = UINT32_MAX;
	for (uint32_t i = 0; i < qc; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			g.family = i;
			g.timestamps = qf[i].timestampValidBits > 0 && g.ts_period > 0;
			break;
		}
	if (g.family == UINT32_MAX)
	{
		internal::Log("[gpu] no graphics queue");
		Shutdown();
		return false;
	}
	vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> dexts(n);
	vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, dexts.data());
	std::vector<const char*> dwant;
	if (!offscreen)
	{
		if (!HasExtension(dexts, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
		{
			internal::Log("[gpu] the driver has no VK_KHR_swapchain");
			Shutdown();
			return false;
		}
		dwant.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}
	// 1.1: pictures read where they are (a hardware decoder's buffers): VK_EXT_external_memory_host and what it needs
	// (VK_KHR_external_memory; the instance's properties2 and external_memory_capabilities), on the 1.0 instance
	const bool host_import = cfg.video && vkGetPhysicalDeviceProperties2 &&
							 HasExtension(dexts, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) &&
							 HasExtension(dexts, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
	if (host_import)
	{
		dwant.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
		dwant.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
	}
	internal::Log("[gpu] Init: the GPUs: %u device extensions%s", n, host_import ? ", the host-pointer import among them" : "");
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueFamilyIndex = g.family;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;
	VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	di.queueCreateInfoCount = 1;
	di.pQueueCreateInfos = &qi;
	di.enabledExtensionCount = uint32_t(dwant.size());
	di.ppEnabledExtensionNames = dwant.data();
	internal::Log("[gpu] Init: device on %s", g.device_name);
	if (!Check(vkCreateDevice(g.phys, &di, nullptr, &g.device), "vkCreateDevice"))
	{
		Shutdown();
		return false;
	}
	internal::Log("[gpu] Init: the output");
#define GPU_LOAD_DEVICE(name) vk##name = reinterpret_cast<PFN_vk##name>(vkGetDeviceProcAddr(g.device, "vk" #name));
	GPU_DEVICE_FNS(GPU_LOAD_DEVICE)
	if (!offscreen)
	{
		GPU_SWAPCHAIN_FNS(GPU_LOAD_DEVICE)
	}
	if (host_import)
	{
		GPU_VIDEO_DEVICE_FNS(GPU_LOAD_DEVICE)
	}
#undef GPU_LOAD_DEVICE
	vkGetDeviceQueue(g.device, g.family, 0, &g.queue);

	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = g.family;
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	bool ok = Check(vkCreateCommandPool(g.device, &cpi, nullptr, &g.pool), "vkCreateCommandPool");
	if (ok)
	{
		cai.commandPool = g.pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		ok = Check(vkAllocateCommandBuffers(g.device, &cai, &g.cmd), "vkAllocateCommandBuffers") &&
			 Check(vkCreateFence(g.device, &fi, nullptr, &g.fence), "vkCreateFence") &&
			 Check(vkCreateSemaphore(g.device, &smi, nullptr, &g.acquired), "vkCreateSemaphore") &&
			 Check(vkCreateSemaphore(g.device, &smi, nullptr, &g.rendered), "vkCreateSemaphore");
	}
	if (ok)
	{
		VkCommandPoolCreateInfo upi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		upi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		upi.queueFamilyIndex = g.family;
		ok = Check(vkCreateCommandPool(g.device, &upi, nullptr, &g.upload_pool), "vkCreateCommandPool (uploads)");
	}
	if (ok)
	{
		// the pipelines built before (the driver checks the header: another driver or GPU starts empty)
		std::vector<uint8_t> data;
		if (FILE* f = fopen(PipelineCachePath().c_str(), "rb"))
		{
			fseek(f, 0, SEEK_END);
			const long n = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (n > 0 && n < (64 << 20))
			{
				data.resize(size_t(n));
				if (fread(data.data(), 1, data.size(), f) != data.size())
					data.clear();
			}
			fclose(f);
		}
		VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
		ci.initialDataSize = data.size();
		ci.pInitialData = data.empty() ? nullptr : data.data();
		if (vkCreatePipelineCache(g.device, &ci, nullptr, &g.pcache) == VK_SUCCESS)
			g.pcache_saved = data.size();
		else
		{
			ci.initialDataSize = 0;
			ci.pInitialData = nullptr;
			vkCreatePipelineCache(g.device, &ci, nullptr, &g.pcache);
		}
		internal::Log("[gpu] pipeline cache: %zu KiB from %s", data.size() >> 10, PipelineCachePath().c_str());
	}
	if (ok && g.timestamps)
	{
		VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qpi.queryCount = 2;
		g.timestamps = vkCreateQueryPool(g.device, &qpi, nullptr, &g.queries) == VK_SUCCESS;
	}
	// samplers: [linear][wrap]
	for (int f = 0; ok && f < 2; f++)
		for (int w = 0; ok && w < 4; w++)
		{
			static const VkSamplerAddressMode kWrap[4] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
				VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
			VkSamplerCreateInfo s = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			s.magFilter = s.minFilter = f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
			s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			s.addressModeU = s.addressModeV = s.addressModeW = kWrap[w];
			s.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			s.maxLod = 0.0f;
			ok = Check(vkCreateSampler(g.device, &s, nullptr, &g.samplers[f][w]), "vkCreateSampler");
		}
	if (ok)
	{
		VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
		VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		dpi.maxSets = 1024;
		dpi.poolSizeCount = 2;
		dpi.pPoolSizes = sizes;
		ok = Check(vkCreateDescriptorPool(g.device, &dpi, nullptr, &g.dpool), "vkCreateDescriptorPool");
	}
	// the quad: Position (vec4), TexCoord (vec2), as RetroArch feeds slang passes
	static const float kQuad[4][6] = {{0, 0, 0, 1, 0, 0}, {1, 0, 0, 1, 1, 0}, {0, 1, 0, 1, 0, 1}, {1, 1, 0, 1, 1, 1}};
	ok = ok && MakeBuffer(g.vertices, sizeof(kQuad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	if (ok)
		memcpy(g.vertices.map, kQuad, sizeof(kQuad));
	ok = ok && MakeTarget(out_w, out_h);
	ok = ok && MakeImage(g.ui, ui_w, ui_h, g.ui_format,
				   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
		 MakeBuffer(g.ui_staging, VkDeviceSize(ui_w) * ui_h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) &&
		 MakeBuffer(g.game_staging, 2048 * 1024 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	if (ok)
	{
		const uint8_t black[4] = {0, 0, 0, 255};
		ok = MakeImage(g.dummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
				 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
			 UploadImage(g.dummy, black, 1, 1);
	}
	ok = ok && BuildChain(g.ui_chain, 1, true); // preset 1: the surface over the game
	if (!ok)
	{
		internal::Log("[gpu] set-up failed: the CPU draws the picture");
		Shutdown();
		return false;
	}
	g.ok = true;
	if (cfg.scene) // no scene: nothing is compiled for it
	{
		if (!InitScene())
			FreeScene(); // the shelf stays on the CPU
		else if (!SceneSelfTest())
		{
			internal::Log("[gpu] the shelf's quads did not come out right on this driver: the CPU draws the shelf");
			FreeScene();
		}
	}
	if (cfg.video)
		InitVideo(host_import); // without it (logged): SetVideo refuses, the program draws its pictures itself
	ps5slang::ScanExternal();
	if (cfg.check_all)
		ps5slang::CheckAllExternal();
	LoadShaderParams();
	internal::Log("[gpu] %s: Vulkan renderer, %dx%d output, %d shader presets", g.device_name, g.out_w, g.out_h,
		ShaderCount());
	return true;
}

bool Active()
{
	return g.ok;
}

void Shutdown()
{
	if (g.builder.joinable())
		g.builder.join();
	if (g.built)
	{
		FreeChain(*g.built);
		delete g.built;
		g.built = nullptr;
	}
	if (g.device)
	{
		vkDeviceWaitIdle(g.device);
		SavePipelineCache();
		if (g.pcache)
			vkDestroyPipelineCache(g.device, g.pcache, nullptr);
		if (g.upload_pool)
			vkDestroyCommandPool(g.device, g.upload_pool, nullptr);
		if (g.hdr_active && CurrentConfig().set_hdr_output)
			CurrentConfig().set_hdr_output(false, g.out_w, g.out_h);
		FreeHdr();
		FreeScene();
		FreeVideo();
		FreeChain(g.game_chain);
		FreeChain(g.ui_chain);
		FreeImage(g.ui);
		FreeImage(g.original);
		FreeImage(g.dummy);
		FreeImage(g.offscreen_target);
		FreeBuffer(g.ui_staging);
		FreeBuffer(g.game_staging);
		FreeBuffer(g.readback);
		FreeBuffer(g.vertices);
		for (VkFramebuffer f : g.swap_fbs)
			vkDestroyFramebuffer(g.device, f, nullptr);
		for (VkImageView v : g.swap_views)
			vkDestroyImageView(g.device, v, nullptr);
		if (g.swapchain)
			vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
		if (g.final_pass)
			vkDestroyRenderPass(g.device, g.final_pass, nullptr);
		for (auto& row : g.samplers)
			for (VkSampler s : row)
				if (s)
					vkDestroySampler(g.device, s, nullptr);
		if (g.dpool)
			vkDestroyDescriptorPool(g.device, g.dpool, nullptr);
		if (g.queries)
			vkDestroyQueryPool(g.device, g.queries, nullptr);
		if (g.acquired)
			vkDestroySemaphore(g.device, g.acquired, nullptr);
		if (g.rendered)
			vkDestroySemaphore(g.device, g.rendered, nullptr);
		if (g.fence)
			vkDestroyFence(g.device, g.fence, nullptr);
		if (g.pool)
			vkDestroyCommandPool(g.device, g.pool, nullptr);
		vkDestroyDevice(g.device, nullptr);
	}
	if (g.surface && vkDestroySurfaceKHR)
		vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
	if (g.instance && vkDestroyInstance)
		vkDestroyInstance(g.instance, nullptr);
	g.~State();
	new (&g) State();
}

int OutWidth()
{
	return g.out_w;
}

int OutHeight()
{
	return g.out_h;
}

const char* DeviceName()
{
	return g.device_name;
}

// preset 0 is the plain picture, 1 the surface; the shaders the user picks are 0 (plain) and presets 2... (the
// built-in ones, then the user's own)
int ShaderCount()
{
	return std::max(1, ps5slang::PresetCount() - 1);
}

const char* ShaderName(int i)
{
	if (i <= 0 || i + 1 >= ps5slang::PresetCount())
		return "Off";
	return ps5slang::DisplayName(i + 1);
}

bool Built()
{
	return true;
}

bool ShaderDamaged(int i)
{
	return i > 0 && ps5slang::Damaged(i + 1);
}

const char* ShaderPresetName(int i)
{
	if (i <= 0 || i + 1 >= ps5slang::PresetCount())
		return "Off";
	return ps5slang::PresetAt(i + 1).name;
}

int FindShader(const char* preset_name)
{
	if (!preset_name)
		return -1;
	for (int i = 0; i < ShaderCount(); i++)
		if (!strcmp(preset_name, ShaderPresetName(i)))
			return i;
	return -1;
}

bool ShaderFullscreen(int i)
{
	const int p = i <= 0 ? 0 : i + 1;
	return p < ps5slang::PresetCount() && ps5slang::PresetAt(p).fullscreen;
}

int PassCount(int i)
{
	const int p = i <= 0 ? 0 : i + 1;
	return p < ps5slang::PresetCount() ? ps5slang::PresetAt(p).pass_count : 0;
}

namespace
{
// one pixel of the core's frame as B8G8R8A8 (alpha 255)
inline uint32_t ToBgra(const uint8_t* row, int x, PixelFormat f)
{
	switch (f)
	{
		case PixelFormat::XRGB8888:
		{
			uint32_t v; // as it is, its top byte too (as the frames were always uploaded)
			memcpy(&v, row + size_t(x) * 4, 4);
			return v;
		}
		case PixelFormat::XBGR8888:
		{
			uint32_t v;
			memcpy(&v, row + size_t(x) * 4, 4);
			return 0xff000000u | ((v & 0xffu) << 16) | (v & 0xff00u) | ((v >> 16) & 0xffu);
		}
		case PixelFormat::RGB565:
		{
			uint16_t v;
			memcpy(&v, row + size_t(x) * 2, 2);
			const uint32_t r = (v >> 11) & 31, gg = (v >> 5) & 63, b = v & 31;
			return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((gg << 2) | (gg >> 4)) << 8) | ((b << 3) | (b >> 2));
		}
		case PixelFormat::XRGB1555:
		{
			uint16_t v;
			memcpy(&v, row + size_t(x) * 2, 2);
			const uint32_t r = (v >> 10) & 31, gg = (v >> 5) & 31, b = v & 31;
			return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((gg << 3) | (gg >> 2)) << 8) | ((b << 3) | (b >> 2));
		}
	}
	return 0xff000000u;
}

int PixelBytes(PixelFormat f)
{
	return f == PixelFormat::RGB565 || f == PixelFormat::XRGB1555 ? 2 : 4;
}

// the previous frame done (its fence), its GPU time read
void WaitFrame()
{
	if (!g.in_flight)
		return;
	vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
	if (g.timestamps && g.queries_written)
	{
		uint64_t ts[2];
		if (vkGetQueryPoolResults(g.device, g.queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
			VK_SUCCESS)
		{
			g.gpu_ms += double(ts[1] - ts[0]) * g.ts_period / 1e6;
			g.gpu_frames++;
			if (gv.drawn_this_frame)
			{
				std::lock_guard<std::mutex> l(gv.stats_mx);
				gv.gpu_ms_sum += double(ts[1] - ts[0]) * g.ts_period / 1e6;
				gv.gpu_frames++;
			}
		}
		g.queries_written = false;
	}
	g.in_flight = false;
}
} // namespace

void SetFrame(const Frame& f, Rect dst, int shader, bool smooth, bool scanlines)
{
	if (!g.ok || !f.pixels || f.width <= 0 || f.height <= 0)
		return;
	const int bpp = PixelBytes(f.format);
	const size_t pitch = f.pitch > 0 ? size_t(f.pitch) : size_t(f.width) * bpp;
	if (pitch < size_t(f.width) * bpp)
		return;
	// the crop, kept inside the frame (at least one pixel left)
	const int cl = std::max(0, std::min(f.crop.left, f.width - 1)), ct = std::max(0, std::min(f.crop.top, f.height - 1));
	const int cw = std::max(1, f.width - cl - std::max(0, f.crop.right)), ch = std::max(1, f.height - ct - std::max(0, f.crop.bottom));
	if (cw > 8192 || ch > 8192)
		return;
	shader = std::max(0, std::min(shader, ShaderCount() - 1));
	int rot = int(f.rotation) & 3;
	// a fullscreen preset lays out the whole screen itself: it is given the picture turned already
	const bool turn_here = rot != 0 && ShaderFullscreen(shader);
	const bool swap = turn_here && (rot & 1);
	const int w = swap ? ch : cw, h = swap ? cw : ch;
	WaitFrame();
	const VkDeviceSize need = VkDeviceSize(w) * h * 4;
	if (g.game_staging.size < need)
	{
		vkDeviceWaitIdle(g.device);
		FreeBuffer(g.game_staging);
		if (!MakeBuffer(g.game_staging, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
		{
			internal::Log("[gpu] no staging buffer for a %dx%d frame", w, h);
			FreeBuffer(g.game_staging);
			MakeBuffer(g.game_staging, 2048 * 1024 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
			return;
		}
	}
	uint32_t* out = static_cast<uint32_t*>(g.game_staging.map);
	const uint8_t* base = static_cast<const uint8_t*>(f.pixels) + size_t(ct) * pitch;
	if (!turn_here && f.format == PixelFormat::XRGB8888)
		for (int y = 0; y < ch; y++)
			memcpy(out + size_t(y) * w, base + size_t(y) * pitch + size_t(cl) * 4, size_t(cw) * 4); // as it is (alpha too)
	else if (!turn_here)
		for (int y = 0; y < ch; y++)
		{
			const uint8_t* row = base + size_t(y) * pitch;
			uint32_t* o = out + size_t(y) * w;
			for (int x = 0; x < cw; x++)
				o[x] = ToBgra(row, cl + x, f.format);
		}
	else
		for (int y = 0; y < ch; y++)
		{
			const uint8_t* row = base + size_t(y) * pitch;
			for (int x = 0; x < cw; x++)
			{
				// (x, y) of the picture to its place turned counter-clockwise
				int ox, oy;
				if (rot == 1)
					ox = y, oy = cw - 1 - x;
				else if (rot == 2)
					ox = cw - 1 - x, oy = ch - 1 - y;
				else
					ox = ch - 1 - y, oy = x;
				out[size_t(oy) * w + ox] = ToBgra(row, cl + x, f.format);
			}
		}
	g.game = true;
	g.presents_without_game = 0;
	g.game_new = true;
	g.game_w = w;
	g.game_h = h;
	g.game_dst = dst;
	g.rotation = turn_here ? 0 : rot;
	g.shader = shader;
	g.smooth = smooth;
	g.scanlines = scanlines;
}

void SetGame(const uint32_t* argb, int w, int h, Rect dst, int shader, bool smooth, bool scanlines)
{
	Frame f;
	f.pixels = argb;
	f.width = w;
	f.height = h;
	SetFrame(f, dst, shader, smooth, scanlines);
}

void HideGame()
{
	g.game = false;
}

double TakeShaderMs()
{
	const double ms = g.gpu_frames ? g.gpu_ms / g.gpu_frames : 0.0;
	g.gpu_ms = 0;
	g.gpu_frames = 0;
	return ms;
}

bool SceneAvailable()
{
	return g.ok && g.scene_pipe != VK_NULL_HANDLE;
}

bool HasSceneTexture(uint64_t key)
{
	return g.scene_tex.count(key) != 0;
}

bool SceneTexture(uint64_t key, const uint32_t* px, int w, int h, bool column_major)
{
	if (!SceneAvailable() || !px || w <= 0 || h <= 0)
		return false;
	if (g.scene_tex.count(key))
		return true;
	// at most kMax textures: the ones not drawn for the longest go first (none drawn in the frame in flight)
	constexpr size_t kMax = 48;
	if (g.scene_tex.size() >= kMax)
	{
		if (g.in_flight)
		{
			vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
		}
		while (g.scene_tex.size() >= kMax)
		{
			auto old = g.scene_tex.begin();
			for (auto it = g.scene_tex.begin(); it != g.scene_tex.end(); ++it)
				if (it->second.used < old->second.used)
					old = it;
			bool in_scene = false;
			for (const SceneQuad& q : g.scene)
				in_scene |= q.tex == old->first;
			if (in_scene)
				break; // all in use: one more, then
			FreeSceneTex(old->second);
			g.scene_tex.erase(old);
		}
	}
	State::SceneTex t;
	if (!MakeSceneTex(t, px, w, h, column_major))
	{
		FreeSceneTex(t);
		static int failures = 0;
		if (failures++ < 5)
			internal::Log("[gpu] a %dx%d texture for the shelf could not be made", w, h);
		return false;
	}
	t.used = g.presents;
	g.scene_tex[key] = t;
	return true;
}

void SetScene(const std::vector<SceneQuad>& quads)
{
	g.scene = quads;
	g.scene_on = true;
}

void ClearScene()
{
	g.scene.clear();
	g.scene_on = false;
}

bool Present(const uint32_t* ui, Rect dirty, bool wait_vsync)
{
	if (!g.ok)
		return false;
	// 1.1: the CPU's own time in Present and its waits (the GPU, the display), for TakeVideoStats
	const double present_t0 = NowMs();
	double waited = 0;
	if (g.in_flight)
	{
		WaitFrame(); // its GPU time read, as SetFrame does
		waited += NowMs() - present_t0;
	}
	RetireVideo(); // what the last frame read (a ring slot, an imported picture) is free
	if (g.deferred_host_present)
	{
		// host tests (PS5GPU_TEST_OFFSCREEN_IN_FLIGHT): the last frame's read-back, now that its fence was waited
		g.deferred_host_present = false;
		if (CurrentConfig().host_present)
			HostPresent(wait_vsync);
	}
	vkResetFences(g.device, 1, &g.fence);
	// the surface's changed rows to the staging buffer
	if (g.ui_full)
		dirty = {0, 0, g.ui_w, g.ui_h};
	dirty.x = std::max(0, dirty.x);
	dirty.y = std::max(0, dirty.y);
	dirty.w = std::min(dirty.w, g.ui_w - dirty.x);
	dirty.h = std::min(dirty.h, g.ui_h - dirty.y);
	const bool ui_copy = dirty.w > 0 && dirty.h > 0;
	if (ui_copy)
		for (int y = dirty.y; y < dirty.y + dirty.h; y++)
			memcpy(static_cast<uint32_t*>(g.ui_staging.map) + size_t(y) * g.ui_w + dirty.x,
				ui + size_t(y) * g.ui_w + dirty.x, size_t(dirty.w) * 4);

	UpdateHdr();
	if (g.hdr_broken) return false;

	// the game's chain for the shader picked: built on a thread of its own, the picture going on meanwhile with the
	// shader it had (or the plain picture, which builds at once)
	int preset = g.shader <= 0 ? 0 : g.shader + 1;
	// a preset from the app folder whose files are damaged (RetroStation build): the plain picture (nothing is tried)
	if (preset > 1 && ps5slang::Damaged(preset))
		preset = 0;
	if (g.built_ready)
	{
		if (g.builder.joinable())
			g.builder.join();
		g.built_ready = false;
		Chain* c = g.built;
		g.built = nullptr;
		if (c)
		{
			vkDeviceWaitIdle(g.device);
			FreeChain(g.game_chain);
			g.game_chain = std::move(*c);
			delete c;
			ApplyOverrides(g.game_chain); // what the menu changed while it was being built
			internal::Log("[gpu] shader %s: %d pass(es)", ps5slang::PresetAt(g.game_chain.preset).name,
				int(g.game_chain.passes.size()));
			SavePipelineCache();
		}
		else if (g.building_preset == preset)
		{
			internal::Log("[gpu] shader %s could not be set up: the plain picture", ShaderName(g.shader));
			g.shader = 0;
			preset = 0;
		}
		g.building_preset = -1;
	}
	if (g.game && g.game_chain.preset != preset)
	{
		if (preset == 0 || !g.game_chain.built)
		{
			// the plain picture (or nothing yet on screen): built here, at once
			vkDeviceWaitIdle(g.device);
			BuildChain(g.game_chain, 0, false);
			if (preset == 0)
				internal::Log("[gpu] shader Off: 1 pass(es)");
		}
		if (preset != 0 && !g.building && g.building_preset != preset)
		{
			StartBuild(preset);
			// host tests (Config::sync_build): waited for (a shader's picture checked at a set frame)
			if (CurrentConfig().sync_build && g.builder.joinable())
				g.builder.join();
		}
	}
	// the plain picture's filter and scanlines
	if (g.game && preset == 0 && !g.game_chain.params.empty())
		g.game_chain.params[0] = g.scanlines ? 1.0f : 0.0f;

	uint32_t index = 0;
	if (!g.offscreen)
	{
		const double a0 = NowMs();
		const VkResult r = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.acquired, VK_NULL_HANDLE, &index);
		waited += NowMs() - a0;
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			internal::Log("[gpu] vkAcquireNextImageKHR -> %d", int(r));
			return false;
		}
	}
	VkCommandBuffer cmd = g.cmd;
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	if (ui_copy)
	{
		Barrier(cmd, g.ui, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy r = {};
		r.bufferOffset = (VkDeviceSize(dirty.y) * g.ui_w + dirty.x) * 4;
		r.bufferRowLength = uint32_t(g.ui_w);
		r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.imageOffset = {dirty.x, dirty.y, 0};
		r.imageExtent = {uint32_t(dirty.w), uint32_t(dirty.h), 1};
		vkCmdCopyBufferToImage(cmd, g.ui_staging.buf, g.ui.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
		g.ui_full = false;
	}
	Barrier(cmd, g.ui, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	// 1.1: a new video picture into its plane images
	RecordVideoUpload(cmd);

	// the game's frame, the history it pushes back
	Chain& gc = g.game_chain;
	Rect vp = {0, 0, 0, 0};
	int vpw = 0, vph = 0, rot = 0; // the chain's viewport in the picture's orientation, the last pass's turn
	const bool draw_game = g.game && gc.built && g.presents_without_game < 30;
	g.frame_drew_game = draw_game;
	g.presents_without_game++;
	if (draw_game)
	{
		if (g.game_new)
		{
			if (g.original.w != g.game_w || g.original.h != g.game_h)
			{
				vkDeviceWaitIdle(g.device);
				FreeImage(g.original);
				for (Image& im : gc.hist)
					FreeImage(im);
				gc.hist.clear();
				MakeImage(g.original, g.game_w, g.game_h, VK_FORMAT_B8G8R8A8_UNORM,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			}
			if (gc.history > 0)
			{
				if (gc.hist.size() != size_t(gc.history))
				{
					gc.hist.resize(size_t(gc.history));
					for (Image& im : gc.hist)
						if (!im.img)
							MakeImage(im, g.game_w, g.game_h, VK_FORMAT_B8G8R8A8_UNORM,
								VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
									VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
				}
				// the oldest goes, the others move back one, the last original becomes OriginalHistory1
				std::rotate(gc.hist.rbegin(), gc.hist.rbegin() + 1, gc.hist.rend());
				Barrier(cmd, g.original, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				Barrier(cmd, gc.hist[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				VkImageCopy ic = {};
				ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				ic.extent = {uint32_t(g.game_w), uint32_t(g.game_h), 1};
				vkCmdCopyImage(cmd, g.original.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gc.hist[0].img,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
			}
			Barrier(cmd, g.original, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkBufferImageCopy r = {};
			r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			r.imageExtent = {uint32_t(g.game_w), uint32_t(g.game_h), 1};
			vkCmdCopyBufferToImage(cmd, g.game_staging.buf, g.original.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
			g.game_new = false;
			g.frame_count++;
		}
		Barrier(cmd, g.original, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		// the surface rectangle on the target
		const double sx = double(g.out_w) / g.ui_w, sy = double(g.out_h) / g.ui_h;
		vp = {int(std::lround(g.game_dst.x * sx)), int(std::lround(g.game_dst.y * sy)),
			int(std::lround(g.game_dst.w * sx)), int(std::lround(g.game_dst.h * sy))};
		if (ps5slang::PresetAt(gc.preset).fullscreen)
			vp = {0, 0, g.out_w, g.out_h}; // the preset places the game itself, in its bezel
		vp.w = std::max(1, std::min(vp.w, g.out_w - vp.x));
		vp.h = std::max(1, std::min(vp.h, g.out_h - vp.y));
		// a turned picture: the chain runs in the picture's own orientation, the last pass turns it into vp
		rot = ps5slang::PresetAt(gc.preset).fullscreen ? 0 : g.rotation;
		vpw = (rot & 1) ? vp.h : vp.w;
		vph = (rot & 1) ? vp.w : vp.h;
		if (!SizeChain(gc, vpw, vph))
		{
			vkEndCommandBuffer(cmd);
			VideoSubmitFailed(); // (the frame recorded so far never runs)
			return false;
		}
	}
	if (g.timestamps)
	{
		vkCmdResetQueryPool(cmd, g.queries, 0, 2);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.queries, 0);
	}
	const bool plain_smooth = g.smooth && !(vpw % std::max(1, g.game_w) == 0 && vph % std::max(1, g.game_h) == 0);
	// the intermediate passes render before the final render pass begins; the textures the last pass reads go to
	// the shader's layout before it too (no barrier may sit inside a render pass)
	if (draw_game)
	{
		for (size_t i = 0; i + 1 < gc.passes.size(); i++)
			RecordPass(cmd, gc, i, vp, vpw, vph, rot, plain_smooth);
		const PassRt& lp = gc.passes.back();
		for (int s = 0; s < lp.def->sampler_count; s++)
		{
			Tex t;
			int tw, th;
			if (Resolve(gc, int(gc.passes.size() - 1), lp.def->samplers[s].name, t, tw, th) && t.im->img)
				Barrier(cmd, *t.im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
	}
	VkFramebuffer fb = g.hdr_active ? g.hdr_target.fb : g.offscreen ? g.offscreen_target.fb : g.swap_fbs[index];
	VkClearValue cv = {};
	cv.color.float32[3] = 1.0f;
	VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rb.renderPass = g.hdr_active ? g.hdr_pass : g.final_pass;
	rb.framebuffer = fb;
	rb.renderArea = {{0, 0}, {uint32_t(g.out_w), uint32_t(g.out_h)}};
	rb.clearValueCount = 1;
	rb.pClearValues = &cv;
	vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
	if (draw_game)
	{
		// a feedback last pass would flip its images: the last pass never has any (it draws to the target)
		RecordPass(cmd, gc, gc.passes.size() - 1, vp, vpw, vph, rot, plain_smooth);
	}
	// the shelf's scene, under the surface (no game then)
	if (g.scene_on && !draw_game && g.scene_pipe)
		RecordScene(cmd);
	else if (g.scene_on)
	{
		static int skipped = 0;
		if (skipped++ == 0)
			internal::Log("[gpu] shelf scene not drawn: game %d, pipeline %d", int(draw_game), int(g.scene_pipe != VK_NULL_HANDLE));
	}
	g.presents++;
	// 1.1: the video picture, under the surface
	RecordVideoDraw(cmd);
	// the surface over everything
	{
		Chain& uc = g.hdr_active ? g.hdr_ui_chain : g.ui_chain;
		PassRt& p = uc.passes[0];
		const ps5slang::Pass& d = *p.def;
		const int set_idx = int(g.frame_count & 1);
		// pixel-exact (nearest) when the output is a whole multiple of the surface (1080p -> 4K: 2x2 blocks); bilinear
		// otherwise (1440p: nearest would make some surface pixels wider than others) -- 1.1
		const bool whole = g.out_w % g.ui_w == 0 && g.out_h % g.ui_h == 0;
		VkDescriptorImageInfo info = {g.samplers[whole ? 0 : 1][1], g.ui.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorBufferInfo ubi = {p.ubo.buf, 0, VK_WHOLE_SIZE};
		VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
		w[0].dstSet = p.set[set_idx];
		w[0].dstBinding = uint32_t(d.ubo_binding);
		w[0].descriptorCount = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		w[0].pBufferInfo = &ubi;
		w[1].dstSet = p.set[set_idx];
		w[1].dstBinding = uint32_t(d.samplers[0].binding);
		w[1].descriptorCount = 1;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[1].pImageInfo = &info;
		vkUpdateDescriptorSets(g.device, 2, w, 0, nullptr);
		memcpy(p.ubo.map, kMvp, 64);
		float push[8];
		SizeVec(push, g.ui_w, g.ui_h);
		SizeVec(push + 4, g.out_w, g.out_h);
		if (g.hdr_active) { push[6] = -1; push[7] = 203; }
		const VkViewport view = {0, 0, float(g.out_w), float(g.out_h), 0, 1};
		const VkRect2D sc = {{0, 0}, {uint32_t(g.out_w), uint32_t(g.out_h)}};
		vkCmdSetViewport(cmd, 0, 1, &view);
		vkCmdSetScissor(cmd, 0, 1, &sc);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &p.set[set_idx], 0, nullptr);
		vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			uint32_t(std::min(d.push_size, 32)), push);
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &g.vertices.buf, &zero);
		vkCmdDraw(cmd, 4, 1, 0, 0);
	}
	vkCmdEndRenderPass(cmd);
	if (g.hdr_active) CopyHdrToOutput(cmd, index);
	g.hdr_readback = g.hdr_active;
	if (g.timestamps)
	{
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, 1);
		g.queries_written = draw_game || gv.drawn_this_frame;
	}
	if (g.offscreen)
	{
		g.offscreen_target.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkBufferImageCopy r = {};
		r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.imageExtent = {uint32_t(g.out_w), uint32_t(g.out_h), 1};
		vkCmdCopyImageToBuffer(cmd, g.offscreen_target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback.buf, 1, &r);
	}
	vkEndCommandBuffer(cmd);
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	if (!g.offscreen)
	{
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &g.acquired;
		si.pWaitDstStageMask = &stage;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &g.rendered;
	}
#ifndef __PROSPERO__
	// host tests: PS5GPU_TEST_SUBMIT_FAIL=1: this Present's submit fails (nothing reaches the GPU)
	if (const char* e = getenv("PS5GPU_TEST_SUBMIT_FAIL"))
		if (*e == '1')
		{
			VideoSubmitFailed();
			return false;
		}
#endif
	std::unique_lock<std::mutex> queue_lock(g.queue_mx);
	if (!Check(vkQueueSubmit(g.queue, 1, &si, g.fence), "vkQueueSubmit"))
	{
		VideoSubmitFailed();
		return false;
	}
	g.in_flight = true;
	CommitVideoUpload();
	if (!g.offscreen)
	{
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &g.rendered;
		pi.swapchainCount = 1;
		pi.pSwapchains = &g.swapchain;
		pi.pImageIndices = &index;
		const VkResult r = vkQueuePresentKHR(g.queue, &pi);
		queue_lock.unlock();
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			internal::Log("[gpu] vkQueuePresentKHR -> %d", int(r));
			return false;
		}
	}
	if (queue_lock.owns_lock())
		queue_lock.unlock();
	bool keep_in_flight = false;
#ifndef __PROSPERO__
	// host tests: PS5GPU_TEST_OFFSCREEN_IN_FLIGHT=1: the off-screen frame left in flight, its fence waited (and what it
	// read let go) at the next Present, as a swapchain's is; its read-back handed over then
	if (const char* e = getenv("PS5GPU_TEST_OFFSCREEN_IN_FLIGHT"))
		keep_in_flight = *e == '1';
#endif
	if (g.offscreen && keep_in_flight)
		g.deferred_host_present = true;
	else if (g.offscreen)
	{
		const double w0 = NowMs();
		WaitFrame();
		RetireVideo();
		waited += NowMs() - w0;
		if (CurrentConfig().host_present)
		{
			const double h0 = NowMs();
			HostPresent(wait_vsync);
			waited += NowMs() - h0; // the host's stand-in for the display (its read-back and its vsync)
		}
	}
	(void)wait_vsync;
	{
		std::lock_guard<std::mutex> l(gv.stats_mx);
		gv.present_count++;
		gv.st.present_ms += NowMs() - present_t0 - waited;
		gv.st.wait_ms += waited;
	}
	return true;
}
} // namespace ps5gpu
