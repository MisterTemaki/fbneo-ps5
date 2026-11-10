// Real Vulkan adapter integration: color, geometry, UI alpha, preset dispatch and runtime recovery.
// SPDX-License-Identifier: MIT
#include "ProsperoVideo.h"
#include "ProsperoGpu.h"
#include "OrbisPaths.h"
#include "fe_text.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace { std::string root; std::vector<uint8_t> output; int checks = 0, failures = 0; }
const std::string& OrbisRoot() { return root; }
void OrbisLog(const char* fmt, ...) { va_list args; va_start(args,fmt); vfprintf(stderr,fmt,args); va_end(args); fputc('\n',stderr); }
namespace crashlog { void* ArmThread() { return nullptr; } void DisarmThread(void*) {} }
extern "C" void FBNeoHostGpuPresent(const uint8_t* data, int w, int h, bool)
{
	output.assign(data, data + size_t(w)*h*4);
}
void Check(bool ok, const char* text)
{
	++checks; failures += !ok; printf("%s %s\n", ok ? "PASS" : "FAIL", text);
}
int Red(int x=960, int y=540) { return output.empty() ? -1 : output[(size_t(y)*1920+x)*4+2]; }
void Frame(std::vector<uint32_t>& pixels, int w, int h, int shader=0, double aspect=4.0/3.0,
	ps5video::Scale scale=ps5video::Scale::Stretch)
{
	ps5video::DrawFrame(pixels.data(),w,h,h,aspect,scale,false,false,shader);
}
void Present() { ps5video::Present(0,0,0,0,true); }
int main()
{
	char path[]="/tmp/fbneo-gpu-video-XXXXXX";
	if (!mkdtemp(path)) return 2;
	root=path;
	ps5gpufront::Configure(true);
	Check(ps5video::Init() && ps5gpufront::Active(), "Vulkan initializes through the real PS5 video adapter");
	if (!ps5gpufront::Active()) return 1; // Never count a CPU fallback as a GPU test.
	std::vector<uint32_t> pixels(32*24,0xffff0000);
	Frame(pixels,32,24); Present();
	Check(Red()==255 && output[(size_t(540)*1920+960)*4]==0, "ARGB upload and RGBA UI preserve red/blue channel order");
	Check((ps5video::Surface()[size_t(540)*1920+960]>>24)==0, "GPU game pixels leave a transparent UI surface");
	std::vector<uint32_t> quadrants={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
	Frame(quadrants,2,2); Present();
	Check(Red(480,270)==255 && Red(1440,270)==0 && Red(480,810)==0 && Red(1440,810)==255,
		"texture orientation and all four source quadrants remain correct");
	Frame(pixels,32,24);
	fe::DrawText(900,500,"GPU",4,ps5video::Rgb(255,255,255),true);
	bool coverage=false, transparent=false;
	for(int y=500;y<550;++y) for(int x=900;x<1000;++x)
	{
		const unsigned a=ps5video::Surface()[size_t(y)*1920+x]>>24;
		coverage |= a>0 && a<255; transparent |= a==0;
	}
	Check(coverage && transparent,"antialiased HUD text preserves alpha over a GPU game");
	Frame(pixels,32,24);
	std::fill(pixels.begin(),pixels.end(),0xff0000ff); Present();
	Check(Red()==255, "uploaded game frame is owned before caller reuses its buffer");
	std::fill(pixels.begin(),pixels.end(),0xffff0000);
	Frame(pixels,32,24,0,0.5,ps5video::Scale::Fit); Present();
	Check(Red()==255 && Red(50,540)==0, "vertical aspect and black borders are preserved");
	Frame(pixels,32,24);
	ps5video::DarkenRect(0,0,1920,1080); ps5video::DarkenRect(0,0,1920,1080); Present();
	Check(Red()>=60 && Red()<=66, "pause background darkens the GPU game twice instead of hiding it");
	Frame(pixels,32,24);
	std::vector<uint32_t> overlay(1,0x800000ff);
	ps5video::BlendOverlay(overlay.data(),1,1,nullptr); Present();
	const size_t center=(size_t(540)*1920+960)*4;
	Check(Red()>=120 && Red()<=136 && output[center]>=120 && output[center]<=136,
		"half-transparent HUD composites over the GPU picture");
	std::atomic<bool> stop{false}, timing_valid{true};
	std::thread timing([&] {
		while (!stop.load()) {
			const double ms=ps5video::TakeShaderMs();
			if (!std::isfinite(ms) || ms<0) timing_valid=false;
			std::this_thread::yield();
		}
	});
	for (int shader=1; shader<=11; ++shader)
	{
		Frame(pixels,32,24,shader); Present();
		Check(ps5gpufront::Active() && (ps5video::Surface()[size_t(540)*1920+960]>>24)==0 && Red()>15,
			("GPU shader " + std::to_string(shader) + " draws without CPU filtering").c_str());
	}
	stop=true; timing.join();
	Check(timing_valid, "emulation thread can read timing while the render thread presents");
	Frame(pixels,32,24,12); Present();
	Check(ps5gpufront::Active() && (ps5video::Surface()[size_t(540)*1920+960]>>24)==255,
		"ScaleFX+rAA style retains its CPU filter with GPU presentation");
	Frame(pixels,32,24);
	setenv("PS5GPU_TEST_SUBMIT_FAIL","1",1); Present(); unsetenv("PS5GPU_TEST_SUBMIT_FAIL");
	Check(!ps5gpufront::Active() && (ps5video::Surface()[size_t(540)*1920+960]&255)==255,
		"failed GPU submit recovers the current image through CPU VideoOut");
	ps5video::Shutdown();
	const std::string marker=root+"/logs/gpu-starting";
	if (FILE* f=fopen(marker.c_str(),"w")) fclose(f);
	Check(ps5video::Init() && !ps5gpufront::Active(), "unfinished GPU startup selects CPU for the next launch");
	ps5video::Shutdown();
	Check(ps5video::Init() && ps5gpufront::Active(), "following launch can initialize GPU again");
	ps5video::Shutdown();
	ps5gpufront::Configure(false);
	Check(ps5video::Init() && !ps5gpufront::Active(), "explicit CPU preference is respected");
	ps5video::Shutdown();
	printf("GPU video: %d checks, %d failed\n",checks,failures);
	return failures ? 1 : 0;
}
