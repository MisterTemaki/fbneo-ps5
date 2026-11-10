// Valid solid-color covers must resize without undefined pointer arithmetic in SIMD table lookup.
// SPDX-License-Identifier: MIT
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../frontend/third_party/stb_image_resize2.h"
#include <cstdio>
#include <vector>
#include <cmath>
int main()
{
	int checks = 0;
	for (auto layout : {STBIR_RGBA, STBIR_RA, STBIR_RGB})
	{
		const int channels = layout == STBIR_RA ? 2 : layout == STBIR_RGB ? 3 : 4;
		for (int value : {0, 1, 64, 128, 254, 255})
		{
			std::vector<unsigned char> input(32 * 37 * channels), output(19 * 23 * channels);
			for (size_t i = 0; i < input.size(); ++i)
				input[i] = (i % channels == size_t(channels - 1) && channels != 3) ? 255 : value;
			if (!stbir_resize_uint8_srgb(input.data(), 32, 37, 0, output.data(), 19, 23, 0, layout)) return 1;
			for (size_t i = 0; i < output.size(); ++i)
			{
				const int expected = (i % channels == size_t(channels - 1) && channels != 3) ? 255 : value;
				if (std::abs(int(output[i]) - expected) > 1) return 1;
			}
			++checks;
		}
	}
	printf("PASS %d SIMD solid-color resize cases preserve colors without sanitizer reports\n", checks);
}
