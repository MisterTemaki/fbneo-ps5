// ps5-slang-gpu: the GPU renderer of Temakix RetroStation's programs -- Vulkan 1.0, through RADV (Mesa's AMD driver,
// linked into the eboot) on the PS5 and any desktop driver (lavapipe) in the host tests.
//
// The GPU draws the game's picture through a RetroArch slang shader preset (single- or multi-pass, ps5gpu/slang.h: the
// built-in ones, compiled at build time; the ones that ship as separate files in the app folder, /app0/shaders/slang;
// the player's own .slangp files) at the display's resolution (4K on a 4K TV). The program draws its menus, OSD and
// text on the CPU into a "UI surface" (by default 1920x1080, RGBA, premultiplied alpha), which the GPU lays over
// everything: where the surface is transparent, the game shows. A shader is built on a thread of its own (the picture
// goes on meanwhile with the one it had) and its pipelines kept in a cache file, so the next time it loads at once.
//
// The usual frame:
//   ps5gpu::Configure(config);                        // once, before Init: paths, log, options
//   if (!ps5gpu::Init(1920, 1080, false, 0, 0))       // false: draw on the CPU (the fall-back)
//       ...
//   each frame:  ps5gpu::SetFrame(frame, dst, shader, smooth, scanlines);   // or HideGame()
//                ps5gpu::Present(ui_surface, dirty, true);
//
// README.md says how a program vendors and builds it.
//
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ps5gpu/slang.h"

namespace ps5gpu
{
// this library's version (README.md, "Versions")
extern const char* const kVersion;

struct Rect
{
	int x, y, w, h;
};

// ---- set-up ---------------------------------------------------------------------------------------------------------
enum class UiFormat
{
	RGBA8, // bytes R, G, B, A (a uint32 0xAABBGGRR): genplus's surface
	BGRA8, // bytes B, G, R, A (a uint32 0xAARRGGBB)
};

struct Config
{
	// a line of the log (no newline at the end); nullptr: no log. The program decides whether it is written (debug
	// logs are off by default in every program).
	void (*log)(const char* line) = nullptr;
	// VkApplicationInfo's name
	std::string app_name = "ps5-slang-gpu";
	// where the pipeline cache is kept ("": none); its folder is made when it is first written
	std::string cache_file;
	// the shader parameters set in the menu, "<preset>\t<parameter>\t<value>" lines ("": not kept)
	std::string params_file;
	// the player's own presets (.slangp anywhere under it, .slang at its top; "": none)
	std::string user_shader_dir;
	// the presets that ship as separate files (read-only; their files are checked against the manifest built in);
	// 1.1: "" for a program that ships none (a media player): they are left out quietly (damaged in the menu)
	std::string shipped_dir = "/app0/shaders/slang";
	// what the log says to do about a shipped preset whose files are missing or changed
	std::string reinstall_hint = "reinstall Temakix RetroStation";
	// the UI surface's byte order, and whether its colours are premultiplied by its alpha
	UiFormat ui_format = UiFormat::RGBA8;
	bool ui_premultiplied = true;
	// genplus's shelf: textured quads under the surface (SetScene). Off: no quad pipeline is compiled at Init.
	bool scene = false;
	// called first and last on each thread the library starts (the shader builder, glslang's): genplus arms its crash
	// handler there (an alternate signal stack, so a stack overflow is reported too); enter's result goes to leave
	void* (*thread_enter)() = nullptr;
	void (*thread_leave)(void* token) = nullptr;
	// host only (Init with offscreen = true): each frame read back, B8G8R8A8, out_w x out_h, handed here
	void (*host_present)(const uint8_t* bgra, int w, int h, bool wait_vsync) = nullptr;
	// host tests only: build each shader before Present returns (a shader's picture checked at a set frame), and
	// compile every preset read from files at Init (the log says how many compiled)
	bool sync_build = false;
	bool check_all = false;
	// 1.1: a media player's pictures (SetVideo): the video pipeline is built at Init, and the GPU may read pictures
	// where they are (ImportHostMemory, VK_EXT_external_memory_host) when the driver can. Off: nothing of it is made.
	bool video = false;
	// (video) pictures read where they are through VK_EXT_external_memory_host, which takes two instance extensions
	// (VK_KHR_get_physical_device_properties2, VK_KHR_external_memory_capabilities); false: no instance extension
	// beyond the display's, every picture copied (MPC on the PS5 died enumerating the GPUs; a start without them tells
	// whether they are why)
	bool host_import = true;
	// MPC local extension: switch VideoOut to HDR10 while an HDR video is visible. SDR remains the fallback.
	bool hdr_output = false;
	int (*set_hdr_output)(bool on, int width, int height) = nullptr;
	int (*hdr_output_range)() = nullptr;
};
// Before Init (a later call changes nothing of a running renderer but the log).
void Configure(const Config& config);
const Config& CurrentConfig();

// ui_w x ui_h: the UI surface. offscreen: no display (the host tests), out_w x out_h is the target; on the console the
// display's own mode (the one closest to 60 Hz, the largest of those) is used and out_w / out_h are ignored.
// False: no GPU (the log says why); everything below is then a no-op, and the program draws on the CPU.
bool Init(int ui_w, int ui_h, bool offscreen, int out_w, int out_h);
bool Active();
void Shutdown();
int OutWidth();
int OutHeight();
const char* DeviceName();
// This build has the GPU renderer (false: the library's stub was linked, the CPU always draws).
bool Built();

// A start that dies while the GPU is being set up must not keep the program from opening: GuardBegin(path) is false
// when `path` is there (the last start didn't reach the screen: it is removed, so the start after this one tries the
// GPU again), else it writes it and is true. GuardEnd() removes it: call it when the first frame is on screen, or when
// Init failed.
bool GuardBegin(const std::string& marker_path);
void GuardEnd();

// ---- the shaders (the menu) -----------------------------------------------------------------------------------------
// 0 is the plain picture ("Off"); 1.. are the presets in the menu's order (ps5gpu/slang.h: shaders/presets.txt's
// order, then the player's own).
int ShaderCount();
// The name the menu shows ("<name>: damaged, reinstall" for a shipped preset whose files are damaged).
const char* ShaderName(int i);
// The preset's own name (what shader_name= in a program's ini stores; never changes with its state).
const char* ShaderPresetName(int i);
// The index of the preset named so, or -1.
int FindShader(const char* preset_name);
int PassCount(int i);
// The shader draws the whole screen (a bezel around the game: the Mega Bezel), not only the game's rectangle.
bool ShaderFullscreen(int i);
// A shipped preset whose files are missing or changed: the plain picture is drawn, the menu says so.
bool ShaderDamaged(int i);
// A shader is being built (in the background; the picture goes on with the one it had).
bool Building();
// The game passes' GPU time per frame since the last call, in ms (0: not measured).
double TakeShaderMs();

// A shader's parameters (its #pragma parameter lines) and the values set in the menu, kept in Config::params_file:
// read at Init, written by SaveShaderParams (the menu, on leaving).
struct ParamInfo
{
	const char* name;
	const char* desc;
	float def, min, max, step, value;
};
int ShaderParamCount(int shader);
bool ShaderParam(int shader, int i, ParamInfo* out);
void SetShaderParam(int shader, const char* name, float value);
void ResetShaderParams(int shader);
bool SaveShaderParams();

// ---- the game's picture ---------------------------------------------------------------------------------------------
enum class PixelFormat
{
	XRGB8888, // a uint32 0x??RRGGBB per pixel (bytes B, G, R, x)
	XBGR8888, // a uint32 0x??BBGGRR per pixel (bytes R, G, B, x)
	RGB565,   // a uint16 RRRRRGGGGGGBBBBB
	XRGB1555, // a uint16 xRRRRRGGGGGBBBBB
};
// How the picture is turned on screen, counter-clockwise (libretro's RETRO_ENVIRONMENT_SET_ROTATION): a vertical arcade
// game is Rotate90 or Rotate270. The shader runs on the picture as the core made it (its scanlines follow the game's
// lines, as on a turned monitor) and its last pass draws it turned into `dst`; a fullscreen preset (the Mega Bezel) is
// given the picture already turned instead, since it lays out the whole screen itself.
enum class Rotation
{
	None = 0,
	Rotate90 = 1,
	Rotate180 = 2,
	Rotate270 = 3,
};
// Pixels left out at each edge of the core's frame (overscan), before anything else.
struct Crop
{
	int left = 0, top = 0, right = 0, bottom = 0;
};
struct Frame
{
	const void* pixels = nullptr;
	int width = 0, height = 0;
	int pitch = 0; // bytes per row; 0: width * the pixel's size
	PixelFormat format = PixelFormat::XRGB8888;
	Crop crop;
	Rotation rotation = Rotation::None;
};
// The picture of the next Present: the frame, where it goes on the UI surface (dst, in surface pixels: Fit() works it
// out from an aspect ratio), the shader, and for the plain picture the filter (smooth: bilinear, unless the scale is a
// whole number) and the scanlines. Copied now: the frame's memory may change after the call.
void SetFrame(const Frame& frame, Rect dst, int shader, bool smooth, bool scanlines);
// The same for a frame of 0x??RRGGBB pixels, w x h, tightly packed (genplus's call).
void SetGame(const uint32_t* argb, int w, int h, Rect dst, int shader, bool smooth, bool scanlines);
// No game picture (a menu of the program's own on the whole screen).
void HideGame();
// The UI surface (Config::ui_format), of which `dirty` changed since the last call, over the game picture, on screen.
// wait_vsync: on the console the swapchain is FIFO (each Present waits for its turn); on the host it is handed to
// Config::host_present.
bool Present(const uint32_t* ui, Rect dirty, bool wait_vsync);

// Where a picture of src_w x src_h goes in `area` (surface pixels), centred: at `aspect` (its width / height on
// screen; 0: square pixels), as large as fits, or at the largest whole multiple of its size that fits when `integer`
// (at least 1). For a turned picture (Rotate90 / 270) pass the turned size and aspect.
Rect Fit(int src_w, int src_h, double aspect, Rect area, bool integer);

// ---- a media player's pictures (1.1, Config::video) -------------------------------------------------------------------
// A decoded picture (Y'CbCr or RGB, 8 to 16 bits) drawn in one pass (shaders/video.frag): each plane sampled and
// filtered where the output pixel falls, the codes normalised, the colour matrix, and for HDR10 / HLG the tone mapping
// to the SDR picture, straight into the output (4K on a 4K TV), under the UI surface. Nothing of it runs on the CPU but
// one copy of the planes into a staging buffer (a ring of three, so the copy never waits for the GPU), and none at all
// for a picture in imported memory (ImportHostMemory: a hardware decoder's own buffers).
enum class VideoLayout
{
	Planar,       // Y, Cb, Cr in three planes (yuv420p, yuv422p10le, yuv444p...)
	SemiPlanarUV, // Y, then Cb Cr interleaved (NV12, P010)
	SemiPlanarVU, // Y, then Cr Cb (NV21)
	Packed,       // one plane of 4-byte RGB pixels (rgba, bgra, rgb0...): VideoFrame::rgb_index says which byte is which
	Gray,         // Y only
	PlanarGBR,    // G, B, R in three planes (gbrp, gbrp10le)
};
enum class ColorMatrix
{
	BT601,
	BT709,
	BT2020,   // non-constant luminance
	FCC,
	SMPTE240M,
};
enum class VideoTransfer
{
	Sdr,
	Pq,  // HDR10, SMPTE ST 2084
	Hlg, // ARIB STD-B67
};
enum class ToneMap
{
	Off = 0, // the signal as it comes (an HDR picture looks washed out)
	Bt2390 = 1,
	Hable = 2,
	Reinhard = 3,
};
enum class VideoFilter
{
	Bilinear, // the hardware's; a tent widened by the shrink factor when the picture is made smaller
	Bicubic,  // Catmull-Rom, widened the same way: sharper, a little more work
};
struct VideoPlane
{
	const void* data = nullptr;
	int pitch = 0; // bytes per row
};
// Memory the GPU reads pictures from where they are (ImportHostMemory).
struct HostMemory;
struct VideoFrame
{
	int width = 0, height = 0; // the picture shown (plane 0's size)
	VideoLayout layout = VideoLayout::Planar;
	int bits = 8;     // significant bits of a sample: 8, or 9..16 in 16-bit little-endian words
	bool msb = false; // 16-bit words: the bits at the top (P010), not the bottom (yuv420p10le, a decoder's 10-bit)
	int chroma_shift_x = 1, chroma_shift_y = 1; // log2 of the chroma subsampling: 4:2:0 1/1, 4:2:2 1/0, 4:4:4 0/0
	// where a chroma sample sits among the luma pixels it covers, each way: 0 on the first one's centre (MPEG-2, H.264
	// and HEVC's "left" siting horizontally; "top-left": both), 0.5 in the middle (MPEG-1 / JPEG; libswscale's default
	// when the stream doesn't say), 1 on the last one's
	double chroma_pos_x = 0.5, chroma_pos_y = 0.5;
	int rgb_index[3] = {0, 1, 2}; // Packed: the byte of R, G and B in each pixel
	VideoPlane plane[3];
	ColorMatrix matrix = ColorMatrix::BT709;
	bool full_range = false;
	VideoTransfer transfer = VideoTransfer::Sdr;
	double peak_nits = 1000; // HDR: the film's peak (its content light level or mastering display)
	// Zero copy: the planes lie inside this imported memory and are read there (memory must stay valid until release
	// is called); nullptr: they are copied now.
	HostMemory* memory = nullptr;
	// Called once the planes are no longer needed (a copied frame: before SetVideo returns; an imported one: once the GPU
	// has read it, at a later Present, HideVideo or Shutdown), on the thread that called that.
	void (*release)(void* ctx) = nullptr;
	void* release_ctx = nullptr;
};
// Where and how the picture is drawn; changes at each Present without a new picture.
struct VideoView
{
	Rect dst = {0, 0, 0, 0}; // in surface pixels; it may reach past the surface (a zoom): the part outside isn't drawn
	ToneMap tone_map = ToneMap::Bt2390;
	double white_nits = 250; // the picture's white after tone mapping
	VideoFilter filter = VideoFilter::Bilinear;
};
// The video pipeline is there (Config::video, the GPU on, its self-test passed).
bool VideoAvailable();
bool HdrOutputActive();
// This frame's layout can be drawn (planes, sizes, bits); false: draw it another way.
bool VideoFormatSupported(const VideoFrame& f);
// The next Present's picture. False: not taken (VideoAvailable() false, a layout or size it can't take, no memory for
// it): the program draws it another way. `release` is called in every case.
bool SetVideo(const VideoFrame& f, const VideoView& view);
// The same picture, at another place or with other settings.
void SetVideoView(const VideoView& view);
// No picture (the UI surface alone, or a game picture).
void HideVideo();
// 1.1 review: every picture the GPU still holds let go now (the pending one, and the one the last frame read: that
// frame waited for), their release called; nothing drawn until the next SetVideo. Before a program frees memory those
// pictures point into, or releases its imports, at its exit before Shutdown.
void ReleaseVideo();
// Imported memory (VK_EXT_external_memory_host): base and bytes multiples of HostMemoryAlignment(). nullptr: the driver
// can't (the planes are then copied). Release waits until the GPU has finished reading it.
size_t HostMemoryAlignment(); // 0: no import on this driver
HostMemory* ImportHostMemory(void* base, size_t bytes);
void ReleaseHostMemory(HostMemory* m);
// Since the last call: pictures taken (copied / read in place), the CPU time of the copies, the GPU time of the video
// pass and the UI surface (ms per frame), the CPU time of Present (recording and submitting, not the waits).
struct VideoStats
{
	uint64_t frames = 0, copied = 0, in_place = 0, refused = 0;
	double copy_ms = 0;    // per copied frame
	double gpu_ms = 0;     // per presented frame
	double present_ms = 0; // per Present, the CPU's own work
	double wait_ms = 0;    // per Present, waiting for the GPU and the display
};
VideoStats TakeVideoStats();

// ---- genplus's shelf: textured quads under the surface (Config::scene) -----------------------------------------------
// Coordinates are the surface's; w is each corner's distance from the eye, so the textures are mapped in perspective.
enum QuadMode
{
	kQuadImage = 0,      // the texture, times bright, at alpha
	kQuadReflection = 1, // the texture fading out from the top edge (the floor's reflection)
	kQuadGlow = 2,       // a glow of colour around rect (no texture), added
};
struct SceneQuad
{
	uint64_t tex = 0; // a SceneTexture key (0 for a glow)
	float x[4] = {}, y[4] = {}, w[4] = {1, 1, 1, 1}; // top-left, top-right, bottom-left, bottom-right
	float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
	float bright = 1, alpha = 1;
	int mode = kQuadImage;
	float rect[4] = {}; // glow: the rectangle it surrounds (l, t, r, b)
	float glow_r = 0;
	float color[3] = {}; // glow colour, 0..1
};
// The scene can be drawn (Config::scene, the GPU on, its quad shader compiled and its self-test passed).
bool SceneAvailable();
// A texture for the scene, kept by `key` (uploaded on the first call, with its mip levels): R8G8B8A8 pixels, row-major or
// column-major (px[x * h + y]). False: it could not be.
bool SceneTexture(uint64_t key, const uint32_t* px, int w, int h, bool column_major);
bool HasSceneTexture(uint64_t key);
// The quads drawn under the surface at every Present from now on (in order), until ClearScene; not with a game picture.
void SetScene(const std::vector<SceneQuad>& quads);
void ClearScene();
} // namespace ps5gpu
