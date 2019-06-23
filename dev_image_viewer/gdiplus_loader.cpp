#include "dev_image_viewer.h"

#include <gdiplus.h>

#include "gdiplus_loader.h"

static ULONG_PTR gdiplusToken = 0;

void init_bitmap_header(BITMAPV5HEADER* bmi, int width, int height)
{
	ZeroMemory(bmi, sizeof(BITMAPV5HEADER));
	bmi->bV5Size = sizeof(BITMAPV5HEADER);
	bmi->bV5Width = width;
	bmi->bV5Height = -height;  // is top-down okay/efficient in GDI?
	bmi->bV5Planes = 1;
	bmi->bV5BitCount = 32;
	bmi->bV5Compression = BI_RGB;
	bmi->bV5SizeImage = width * height * 4;
	bmi->bV5RedMask = 0x00FF0000;
	bmi->bV5GreenMask = 0x0000FF00;
	bmi->bV5BlueMask = 0x000000FF;
	bmi->bV5AlphaMask = 0xFF000000;
	bmi->bV5CSType = LCS_WINDOWS_COLOR_SPACE;
	bmi->bV5Intent = LCS_GM_IMAGES;
}

bool canvas_read_image(const WCHAR* path, HBITMAP* out_hbitmap,
	void** out_bits, int* out_width, int* out_height)
{
	Gdiplus::Bitmap bitmap(path);
	if (bitmap.GetLastStatus() != Gdiplus::Ok)
		return false;

	int width = bitmap.GetWidth();
	int height = bitmap.GetHeight();
	Gdiplus::BitmapData lockedBitmapData;
	if (bitmap.LockBits(&Gdiplus::Rect(0, 0, width, height),
		Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB,
		&lockedBitmapData) != Gdiplus::Ok)
		return false;

	BITMAPV5HEADER bmi;
	init_bitmap_header(&bmi, width, height);

	HDC hdc = GetDC(NULL);
	void* bits = 0;
	HBITMAP hbitmap = CreateDIBSection(hdc, (BITMAPINFO*)&bmi, DIB_RGB_COLORS,
		&bits, NULL, 0);
	ReleaseDC(NULL, hdc);

	if (!hbitmap) {
		bitmap.UnlockBits(&lockedBitmapData);
		return false;
	}

	for (int y = 0; y < height; y++) {
		memcpy((char*)bits + (size_t)y * width * 4,
			(char*)lockedBitmapData.Scan0 + (size_t)y * lockedBitmapData.Stride,
			(size_t)width * 4);
	}

	bitmap.UnlockBits(&lockedBitmapData);

	*out_hbitmap = hbitmap;
	*out_bits = bits;
	*out_width = width;
	*out_height = height;
	return true;
}

void init_gdiplus_loader()
{
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
}

void destroy_gdiplus_loader()
{
	Gdiplus::GdiplusShutdown(gdiplusToken);
}
