// ps5-slang-gpu: RetroArch slang shader presets -- the built-in ones compiled at build time (tools/slang_compile.py)
// into the tables below, the ones that ship as separate files (tools/shader_manifest.py: their names, sizes and sha256
// only) and the player's own, both compiled on the console by glslang (src/slang_loader.cpp) -- run on the GPU by
// src/gpu.cpp. A program uses ps5gpu/gpu.h; this header is the tables' and the loader's.
//
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ps5slang
{
// a member of a pass's uniform block or push constants: a semantic (MVP, OutputSize...) or a parameter
struct Member
{
	const char* name;
	int offset;
	int kind; // 0 float, 1 int, 2 uint, 3 vec2, 4 vec3, 5 vec4, 6 mat4, 7 other
};

// a texture the pass samples: Source, Original, PassOutput1, an alias, a LUT...
struct Sampler
{
	const char* name;
	int binding;
};

struct Pass
{
	const uint32_t* vs;
	unsigned vs_bytes;
	const uint32_t* fs;
	unsigned fs_bytes;
	const Member* ubo;
	int ubo_count;
	int ubo_size;
	int ubo_binding; // -1: no uniform block
	const Member* push;
	int push_count;
	int push_size;
	const Sampler* samplers;
	int sampler_count;
	const char* alias;
	int framebuffer; // 0 8-bit UNORM, 1 sRGB, 2 16-bit float
	int scale_type_x, scale_type_y; // 0 source, 1 viewport, 2 absolute
	float scale_x, scale_y;
	bool linear;
	int wrap; // 0 border, 1 edge, 2 repeat, 3 mirrored repeat
	int frame_count_mod;
	bool mipmap_input;
};

struct Param
{
	const char* name;
	const char* desc;
	float value, min, max, step;
};

struct Lut
{
	const char* name;
	const uint8_t* png;
	unsigned png_bytes;
	bool linear;
	int wrap;
	bool mipmap;
};

struct Preset
{
	const char* name;
	const Pass* passes;
	int pass_count;
	const Param* params;
	int param_count;
	const Lut* luts;
	int lut_count;
	bool fullscreen; // draws on the whole screen (a bezel around the game): the viewport is the screen, not the game
};

// the presets compiled at build time (tools/slang_compile.py): [0] the plain picture, [1] the UI surface's pass
extern const Preset kBuiltinPresets[];
extern const int kBuiltinPresetCount;

// The presets whose files ship as separate files (Config::shipped_dir: /app0/shaders/slang on the console, read-only;
// tools/shader_manifest.py, from shaders/presets.txt's "file:" lines). They are libretro/slang-shaders at
// kShippedRevision, under their authors' licences (most of them GPL), and not part of the program: it has their names,
// sizes and sha256 only, no shader code, and compiles them on the console as it does the player's own presets, each
// file checked against its sha256 first. One that doesn't match is "damaged, reinstall" and the plain picture is drawn;
// nothing is written there.
struct ShippedFile
{
	const char* path; // in libretro/slang-shaders, and under Config::shipped_dir
	uint32_t bytes;
	const char* sha256;
};
struct ShippedPreset
{
	const char* name;
	const char* preset;    // the .slangp, a ShippedFile path
	const char* overrides; // "PARAM=value,..." set over the preset's own
	bool fullscreen;
	const uint16_t* files; // kShippedFiles indices
	int file_count;
	uint32_t bytes;
};
extern const char* const kShippedRevision;
// "rs-shaders-manifest=<sha256 of MANIFEST>": every program built with the same list carries the same id, and
// RetroStation's make app checks the app folder's shaders/slang/MANIFEST against it
extern const char* const kShippedManifestId;
extern const ShippedFile kShippedFiles[];
extern const int kShippedFileCount;
extern const ShippedPreset kShippedPresets[];
extern const int kShippedPresetCount;
// the menu after the plain picture (presets.txt's order): >= 0 kBuiltinPresets[2 + n], < 0 kShippedPresets[-1 - n]
extern const int16_t kMenuOrder[];
extern const int kMenuOrderCount;

// Every preset: the built-in and shipped ones in kMenuOrder's order, then the player's own from
// Config::user_shader_dir. A preset read from files is compiled the first time it is used (EnsureCompiled: on the GPU
// renderer's build thread); until then it is its name only (no passes, no parameters).
void ScanExternal();
// GLSL (Vulkan) to SPIR-V with the same compiler: the renderer's own small shaders (the scene's quads)
bool CompileGlsl(const std::string& src, bool vertex, std::vector<uint32_t>& spv, std::string& err);
int PresetCount();
const Preset& PresetAt(int i);
bool IsExternal(int i);
bool EnsureCompiled(int i, std::string* err);
// The name the menu shows: the preset's, or "<name>: damaged, reinstall" for a shipped preset whose files don't match the
// manifest (the preset's own name, in PresetAt, never changes: the parameters file keys on it).
const char* DisplayName(int i);
// That preset: its files missing or changed (found at start by their sizes, or when it is compiled by their sha256). The
// plain picture is drawn instead. False for every other preset.
bool Damaged(int i);
// Config::check_all (host tests): every preset read from files compiled now (the log says how many did)
void CheckAllExternal();

// SHA-256 (FIPS 180-4) of `size` bytes, as 64 lower-case hex digits
std::string Sha256Hex(const uint8_t* data, size_t size);
} // namespace ps5slang
