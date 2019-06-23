#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void init_gdiplus_loader();
void destroy_gdiplus_loader();

void init_bitmap_header(BITMAPV5HEADER* bmi, int width, int height);
bool canvas_read_image(const WCHAR* path, HBITMAP* out_hbitmap,
	void** out_bits, int* out_width, int* out_height);

#ifdef __cplusplus
}
#endif
