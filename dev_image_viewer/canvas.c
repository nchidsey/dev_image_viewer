#include "dev_image_viewer.h"

#include <stdlib.h>
#include <math.h>
#include <strsafe.h>
#include <stdbool.h>
#include <stdint.h>
#include <immintrin.h>
#include <emmintrin.h>

#include "canvas.h"
#include "gdiplus_loader.h"

#define CANVAS_WNDLONG_PRIVATE 0

#define CANVAS_NUM_MINIFY_LEVELS 5

typedef struct {
	HBITMAP hbitmap;
	void* bits;		// owned by hbitmap. do not free.
	int width;
	int height;
} canvas_level_t;

typedef struct {
	WCHAR* path;

	int tx;
	int ty;
	int zoom;

	bool panning;
	int prev_mousex;
	int prev_mousey;
	int wheel_accum;

	canvas_level_t levels[CANVAS_NUM_MINIFY_LEVELS + 1];

	DWORD bg_color;
	HFONT hfont;
} canvas_data_t;

static canvas_data_t* _canvas_new_private()
{
	canvas_data_t* priv = (canvas_data_t*)malloc(sizeof(canvas_data_t));
	if (!priv)
		return NULL;
	ZeroMemory(priv, sizeof(canvas_data_t));

	priv->bg_color = 0xFF404040;

	return priv;
}

static void _canvas_free_levels(canvas_level_t* levels)
{
	for (int i = 0; i <= CANVAS_NUM_MINIFY_LEVELS; i++) {
		if (levels[i].hbitmap) {
			DeleteObject(levels[i].hbitmap);
			levels[i].hbitmap = NULL;
		}
		levels[i].bits = NULL;
		levels[i].width = 0;
		levels[i].height = 0;
	}
}

static void _canvas_destroy_private(canvas_data_t* priv)
{
	_canvas_free_levels(priv->levels);
	if (priv->path)
		free(priv->path);
	if (priv->hfont)
		DeleteObject(priv->hfont);
	free(priv);
}

static canvas_data_t* _canvas_get_private(HWND hwnd)
{
	return (canvas_data_t*)GetWindowLongPtr(hwnd, CANVAS_WNDLONG_PRIVATE);
}

static void _downsize_naive(
	DWORD* src_pixels, int src_width, int src_height,
	DWORD* dest_pixels, int dest_width, int dest_height,
	DWORD bg_color)
{
	// this code has vestigial support for non-2X downsizing
	int scale = 2;

	int back_r = (bg_color >> 16) & 0xFF;
	int back_g = (bg_color >> 8) & 0xFF;
	int back_b = bg_color & 0xFF;

	int r, g, b, a;
	DWORD src_pixel;
	int scale_sqr = scale * scale;
	for (int desty = 0; desty < dest_height; desty++) {
		for (int destx = 0; destx < dest_width; destx++) {
			r = g = b = a = 0;
			for (int srcy = desty * scale; srcy < desty * scale + scale; srcy++) {
				for (int srcx = destx * scale; srcx < destx * scale + scale; srcx++) {
					if (srcx < src_width && srcy < src_height) {
						src_pixel = ((DWORD*)src_pixels)[srcy * src_width + srcx];
						r += (src_pixel >> 16) & 0xFF;
						g += (src_pixel >> 8) & 0xFF;
						b += src_pixel & 0xFF;
						a += src_pixel >> 24;
					}
				}
			}

			r /= scale_sqr;
			g /= scale_sqr;
			b /= scale_sqr;
			a /= scale_sqr;

			// bake the background color in. because original bitmap was
			// already baked, this only happens to right/bottom edges.
			r += back_r * (255 - a) / 255;
			g += back_g * (255 - a) / 255;
			b += back_b * (255 - a) / 255;
			a = 255;

			((DWORD*)dest_pixels)[desty * dest_width + destx] =
				(a << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

static void _downsize_sse2(
	DWORD* src_pixels, int src_width, int src_height,
	DWORD* dest_pixels, int dest_width, int dest_height,
	DWORD bg_color
)
{
	const __m128i zero = _mm_setzero_si128();
	__m128i temp = zero;

	// get 1x and 2x the background color, expanded to 16-bit components
	__m128i bg = _mm_unpacklo_epi8(_mm_loadu_si32(&bg_color), zero);
	__m128i bg_x2 = _mm_slli_epi16(bg, 1);

	// Main quadrant: 2x2 pixel groups
	int quad_area_width = src_width / 2;
	int quad_area_height = src_height / 2;
	for (int dest_y = 0; dest_y < quad_area_height; dest_y++) {
		DWORD* src_ptr = &src_pixels[dest_y * 2 * src_width];
		for (int dest_x = 0; dest_x < quad_area_width; dest_x++, src_ptr += 2) {
			// load top 2 and bottom 2 pixels, extend to 16-bit components
			__m128i top = _mm_unpacklo_epi8(_mm_loadu_si64(src_ptr), zero);
			__m128i bottom = _mm_unpacklo_epi8(_mm_loadu_si64(src_ptr + src_width), zero);
			// add all 4 together
			__m128i accum = _mm_add_epi16(top, bottom);
			temp = _mm_castps_si128(
				_mm_movehl_ps(_mm_castsi128_ps(temp), _mm_castsi128_ps(accum)));
			accum = _mm_add_epi16(accum, temp);
			// divide by 4
			accum = _mm_srli_epi16(accum, 2);
			// convert back to 8-bit and write destination pixel
			__m128i result = _mm_packus_epi16(accum, zero);
			_mm_storeu_si32(&dest_pixels[dest_y * dest_width + dest_x], result);
		}
	}

	// Bottom edge, if odd height
	if (src_height & 1) {
		int dest_y = src_height / 2;
		DWORD* src_ptr = &src_pixels[dest_y * 2 * src_width];
		for (int dest_x = 0; dest_x < quad_area_width; dest_x++, src_ptr += 2) {
			// load 2 horiz pixels, extend to 16-bit
			__m128i accum = _mm_unpacklo_epi8(_mm_loadu_si64(src_ptr), zero);
			// add together
			temp = _mm_castps_si128(
				_mm_movehl_ps(_mm_castsi128_ps(temp), _mm_castsi128_ps(accum)));
			accum = _mm_add_epi16(accum, temp);
			// add 2x the background color
			accum = _mm_add_epi16(accum, bg_x2);
			// divide by 4
			accum = _mm_srli_epi16(accum, 2);
			// convert back to 8-bit and write destination pixel
			__m128i result = _mm_packus_epi16(accum, zero);
			_mm_storeu_si32(&dest_pixels[dest_y * dest_width + dest_x], result);
		}
	}

	// Right edge, if odd width
	if (src_width & 1) {
		int dest_x = src_width / 2;
		DWORD* src_ptr = &src_pixels[src_width - 1];
		for (int dest_y = 0; dest_y < quad_area_height; dest_y++, src_ptr += src_width) {
			// load upper and lower pixels, extend
			__m128i top = _mm_unpacklo_epi8(_mm_loadu_si32(src_ptr), zero);
			__m128i bottom = _mm_unpacklo_epi8(_mm_loadu_si32(src_ptr + src_width), zero);
			// add together
			__m128i accum = _mm_add_epi16(top, bottom);
			// add 2x the background color
			accum = _mm_add_epi16(accum, bg_x2);
			// divide by 4
			accum = _mm_srli_epi16(accum, 2);
			// convert back to 8-bit and write destination pixel
			__m128i result = _mm_packus_epi16(accum, zero);
			_mm_storeu_si32(&dest_pixels[dest_y * dest_width + dest_x], result);
		}
	}

	// Bottom right corner pixel, if odd width and height
	if ((src_width & 1) && (src_height & 1)) {
		int dest_x = src_width / 2;
		int dest_y = src_height / 2;
		DWORD* src_ptr = &src_pixels[dest_y * 2 * src_width + dest_x * 2];
		// load the bottom right corner pixel
		__m128i accum = _mm_unpacklo_epi8(_mm_loadu_si32(src_ptr), zero);
		// add 3x the background color
		accum = _mm_add_epi16(accum, _mm_add_epi16(bg, bg_x2));
		// divide by 4
		accum = _mm_srli_epi16(accum, 2);
		// convert back to 8-bit and write destination pixel
		__m128i result = _mm_packus_epi16(accum, zero);
		_mm_storeu_si32(&dest_pixels[dest_y * dest_width + dest_x], result);
	}
}

static bool _canvas_create_level(canvas_level_t* src_level, 
	canvas_level_t* dest_level, DWORD bg_color)
{
	int downsized_width = (src_level->width + 1) / 2;
	int downsized_height = (src_level->height + 1) / 2;

	BITMAPV5HEADER bmi;
	init_bitmap_header(&bmi, downsized_width, downsized_height);

	HDC hdc = GetDC(NULL);
	void* bits = NULL;
	HBITMAP hbitmap = CreateDIBSection(hdc, (BITMAPINFO*)&bmi, DIB_RGB_COLORS,
		&bits, NULL, 0);
	ReleaseDC(NULL, hdc);
	if (!hbitmap || !bits) {
		return false;
	}

#if 0
	_downsize_naive(
#else
	_downsize_sse2(
#endif
		(DWORD*)src_level->bits, src_level->width, src_level->height,
		(DWORD*)bits, downsized_width, downsized_height,
		bg_color
	);

	dest_level->hbitmap = hbitmap;
	dest_level->bits = bits;
	dest_level->width = downsized_width;
	dest_level->height = downsized_height;
	return true;
}

static bool _canvas_downsize(canvas_level_t* levels, DWORD bg_color)
{
	for (int i = 1; i <= CANVAS_NUM_MINIFY_LEVELS; i++) {
		if (!_canvas_create_level(&levels[i - 1], &levels[i], bg_color))
			return false;
	}
	return true;
}

static void _bake_bg_dwords(DWORD* array, uint64_t count, DWORD color)
{
	int back_r = (color >> 16) & 0xFF;
	int back_g = (color >> 8) & 0xFF;
	int back_b = color & 0xFF;
	int r, g, b, a;
	DWORD* pixel;
	for (uint64_t i = 0; i < count; i++) {
		pixel = &array[i];
		r = (*pixel >> 16) & 0xFF;
		g = (*pixel >> 8) & 0xFF;
		b = *pixel & 0xFF;
		a = (*pixel >> 24);
		r += back_r * (255 - a) / 255;
		g += back_g * (255 - a) / 255;
		b += back_b * (255 - a) / 255;
		a = 255;
		*pixel = (a << 24) | (r << 16) | (g << 8) | b;
	}
}

static void _bake_bg_naive(canvas_level_t* level, DWORD color)
{
	uint64_t num_pixels = (uint64_t)level->width * (uint64_t)level->height;
	_bake_bg_dwords((DWORD*)level->bits, num_pixels, color);
}

static void _bake_bg_sse2(canvas_level_t* level, DWORD color)
{
	const __m128i zero = _mm_setzero_si128();
	const __m128i twofiftyfive = _mm_set1_epi16(255);
	const __m128i ones = _mm_set1_epi16(1);

	__m128i bg = _mm_loadu_si32(&color);
	bg = _mm_unpacklo_epi8(bg, zero);
	// duplicate bg into top 64 bits for processing two pixels at once
	bg = _mm_or_si128(bg, _mm_slli_si128(bg, 8));

	uint64_t num_pixels = (uint64_t)level->width * (uint64_t)level->height;
	uint64_t num_pair_pixels = num_pixels & ~1;

	DWORD* pixel;
	for (uint64_t i = 0; i < num_pair_pixels; i += 2) {
		pixel = &((DWORD*)level->bits)[i];

		// load 2 pixels, extend to 16-bit components
		__m128i orig_pixels = _mm_unpacklo_epi8(_mm_loadu_si64(pixel), zero);
		// duplicate the both alphas into all channels
		__m128i alphas = _mm_shufflelo_epi16(orig_pixels, 255);
		alphas = _mm_shufflehi_epi16(alphas, 255);
		__m128i inv_alphas = _mm_sub_epi16(twofiftyfive, alphas);
		__m128i bg_times_inv_alpha = _mm_mullo_epi16(inv_alphas, bg);
		// one way to approximate div by 255.
		__m128i blend = _mm_srli_epi16(
			_mm_add_epi16(
				_mm_add_epi16(bg_times_inv_alpha, ones),
				_mm_srli_epi16(bg_times_inv_alpha, 8)
			),
			8
		);
		__m128i result_16 = _mm_add_epi16(orig_pixels, blend);
		__m128i result_8 = _mm_packus_epi16(result_16, zero);

		_mm_storeu_si64(pixel, result_8);
	}

	// handle last pixel if odd
	if (num_pixels & 1) {
		_bake_bg_dwords(&((DWORD*)level->bits)[num_pixels - 1], 1, color);
	}
}

static bool _canvas_reload(canvas_data_t* priv)
{
	canvas_level_t new_levels[CANVAS_NUM_MINIFY_LEVELS + 1];
	ZeroMemory(new_levels, sizeof(new_levels));

	if (!canvas_read_image(priv->path,
		&new_levels[0].hbitmap, &new_levels[0].bits,
		&new_levels[0].width, &new_levels[0].height)) {
		return false;
	}

	// Bake the background color in, making the image opaque.
	_bake_bg_sse2(&new_levels[0], priv->bg_color);

	if (!_canvas_downsize(new_levels, priv->bg_color)) {
		_canvas_free_levels(new_levels);
		return false;
	}

	// success. replace old levels
	_canvas_free_levels(priv->levels);
	CopyMemory(priv->levels, new_levels, sizeof(new_levels));

	return true;
}

static void _canvas_paint(HWND hwnd, HDC hdc, PAINTSTRUCT* ps)
{
	canvas_data_t* priv = _canvas_get_private(hwnd);

	RECT client_rect;
	GetClientRect(hwnd, &client_rect);

	// Draw image
	if (priv->levels[0].hbitmap) {
		int scaled_width;
		int scaled_height;
		int src_width;
		int src_height;
		int src_x;
		int src_y;
		int src_x2;
		int src_y2;
		int dest_x;
		int dest_y;
		int dest_x2;
		int dest_y2;

		HDC bitmap_hdc = CreateCompatibleDC(hdc);

		if (priv->zoom < 0) {
			SelectObject(bitmap_hdc, priv->levels[-priv->zoom].hbitmap);
			src_width = scaled_width = priv->levels[-priv->zoom].width;
			src_height = scaled_height = priv->levels[-priv->zoom].height;
		}
		else {
			SelectObject(bitmap_hdc, priv->levels[0].hbitmap);
			src_width = priv->levels[0].width;
			src_height = priv->levels[0].height;
			scaled_width = priv->levels[0].width << priv->zoom;
			scaled_height = priv->levels[0].height << priv->zoom;
		}

		// Calculate new source and dest mapping for when the image is
		// very large and/or zoomed very far in, so extremely large coordinates
		// are avoided.
		// Certain checking isn't needed because of the xform clamp.
		int real_zoom = priv->zoom < 0 ? 0 : priv->zoom;
		if (priv->tx >= 0)
			src_x = 0;
		else
			src_x = (-priv->tx) >> real_zoom;
		dest_x = (src_x << real_zoom) + priv->tx;

		src_x2 = ((client_rect.right - priv->tx) >> real_zoom) + 1;
		if (src_x2 > src_width)
			src_x2 = src_width;
		dest_x2 = (src_x2 << real_zoom) + priv->tx;

		if (priv->ty >= 0)
			src_y = 0;
		else
			src_y = (-priv->ty) >> real_zoom;
		dest_y = (src_y << real_zoom) + priv->ty;

		src_y2 = ((client_rect.bottom - priv->ty) >> real_zoom) + 1;
		if (src_y2 > src_height)
			src_y2 = src_height;
		dest_y2 = (src_y2 << real_zoom) + priv->ty;

		StretchBlt(hdc, dest_x, dest_y, dest_x2 - dest_x, dest_y2 - dest_y,
			bitmap_hdc, src_x, src_y, src_x2 - src_x, src_y2 - src_y, SRCCOPY);

		DeleteDC(bitmap_hdc);

		// exclude the scaled bitmap rect from the clip, for the background
		// to draw everywhere else.
		ExcludeClipRect(hdc, priv->tx, priv->ty,
			priv->tx + scaled_width, priv->ty + scaled_height);
	}

	// Draw the background inside the requested dirty rect and outside
	// the scaled bitmap.
	HBRUSH bg_brush = CreateSolidBrush(priv->bg_color & 0xFFFFFF);
	HGDIOBJ old_brush = SelectObject(hdc, bg_brush);
	FillRect(hdc, &ps->rcPaint, bg_brush);
	SelectObject(hdc, old_brush);
	DeleteObject(bg_brush);

	// Draw message text if appropriate.
	if (!priv->levels[0].hbitmap) {
		SetBkMode(hdc, TRANSPARENT);
		COLORREF old_fg = SetTextColor(hdc, priv->path ? 0x0000FF : 0xFFFFFF);
		UINT old_ta = SetTextAlign(hdc, TA_CENTER | TA_BASELINE);
		HGDIOBJ old_font = NULL;
		if (priv->hfont)
			old_font = SelectObject(hdc, priv->hfont);

		const WCHAR* text = priv->path ? L"Error loading image" : L"No image loaded";
		TextOutW(hdc, client_rect.right / 2, client_rect.bottom / 2, text, (int)wcslen(text));

		// restore
		SetTextColor(hdc, old_fg);
		SetTextAlign(hdc, old_ta);
		if (old_font)
			SelectObject(hdc, old_font);
	}
}

static void _canvas_clamp_xform(HWND hwnd)
{
	canvas_data_t* priv = _canvas_get_private(hwnd);
	RECT client_rect;
	GetClientRect(hwnd, &client_rect);

	int scaled_width, scaled_height;
	if (priv->zoom < 0) {
		scaled_width = priv->levels[-priv->zoom].width;
		scaled_height = priv->levels[-priv->zoom].height;
	}
	else {
		scaled_width = priv->levels[0].width << priv->zoom;
		scaled_height = priv->levels[0].height << priv->zoom;
	}

	if (scaled_width <= client_rect.right) {
		priv->tx = (client_rect.right - scaled_width) / 2;
	}
	else {
		if (priv->tx > 0)
			priv->tx = 0;
		if (priv->tx + scaled_width < client_rect.right)
			priv->tx = client_rect.right - scaled_width;
	}

	if (scaled_height <= client_rect.bottom) {
		priv->ty = (client_rect.bottom - scaled_height) / 2;
	}
	else {
		if (priv->ty > 0)
			priv->ty = 0;
		if (priv->ty + scaled_height < client_rect.bottom)
			priv->ty = client_rect.bottom - scaled_height;
	}
}

static void _canvas_send_notify(HWND hwnd, UINT code)
{
	HWND parent = GetParent(hwnd);
	if (parent) {
		NMHDR nmhdr;
		nmhdr.hwndFrom = hwnd;
		nmhdr.code = code;
		nmhdr.idFrom = GetWindowLong(hwnd, GWL_ID);
		SendMessage(parent, WM_NOTIFY, nmhdr.idFrom, (LPARAM)&nmhdr);
	}
}

static void _canvas_send_notify_mousemove(HWND hwnd, int x, int y)
{
	HWND parent = GetParent(hwnd);
	if (parent) {
		canvas_nm_mousemove_t nm;
		nm.nmhdr.hwndFrom = hwnd;
		nm.nmhdr.code = CANVAS_NM_MOUSEMOVE;
		nm.nmhdr.idFrom = GetWindowLong(hwnd, GWL_ID);
		nm.pos.x = x;
		nm.pos.y = y;
		SendMessage(parent, WM_NOTIFY, nm.nmhdr.idFrom, (LPARAM)&nm);
	}
}

static LRESULT CALLBACK _canvas_wndproc(HWND hwnd, UINT message,
	WPARAM wParam, LPARAM lParam)
{
	switch (message) {
		case WM_NCCREATE:
		{
			canvas_data_t* priv = _canvas_new_private();
			if (!priv)
				return FALSE;
			SetWindowLongPtrW(hwnd, CANVAS_WNDLONG_PRIVATE, (LONG_PTR)priv);

			// get the default font
			NONCLIENTMETRICSW metrics;
			metrics.cbSize = sizeof(metrics);
			if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 0, &metrics, 0)) {
				priv->hfont = CreateFontIndirectW(&metrics.lfMessageFont);
			}

			return TRUE;
		}

		case WM_NCDESTROY:
		{
			canvas_data_t* priv = _canvas_get_private(hwnd);
			if (priv)
				_canvas_destroy_private(priv);
			return 0;
		}

		case WM_CREATE:
			break;

		case WM_DESTROY:
			return 0;

		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			_canvas_paint(hwnd, hdc, &ps);
			EndPaint(hwnd, &ps);
		}
		return 0;

		case WM_SIZE:
		{
			_canvas_clamp_xform(hwnd);
			InvalidateRect(hwnd, NULL, FALSE);
			return 0;
		}

		case WM_CAPTURECHANGED:
		{
			canvas_data_t* priv = _canvas_get_private(hwnd);
			priv->panning = false;
			return 0;
		}

		case WM_LBUTTONDOWN:
		{
			canvas_data_t* priv = _canvas_get_private(hwnd);
			if (priv->levels[0].hbitmap) {
				priv->panning = true;
				priv->prev_mousex = (SHORT)LOWORD(lParam);
				priv->prev_mousey = (SHORT)HIWORD(lParam);
				SetCapture(hwnd);
			}
			return 0;
		}

		case WM_LBUTTONUP:
		{
			canvas_data_t* priv = _canvas_get_private(hwnd);
			priv->panning = false;
			ReleaseCapture();
			return 0;
		}

		case WM_XBUTTONDOWN:
		{
			switch (HIWORD(wParam)) {
				case 1:
					_canvas_send_notify(hwnd, CANVAS_NM_PREV);
					break;
				case 2:
					_canvas_send_notify(hwnd, CANVAS_NM_NEXT);
					break;
			}
			return 0;
		}


		case WM_MOUSEMOVE:
		{
			canvas_data_t* priv = _canvas_get_private(hwnd);
			int mx = (SHORT)LOWORD(lParam);
			int my = (SHORT)HIWORD(lParam);
			if (priv->panning) {
				int dx = mx - priv->prev_mousex;
				int dy = my - priv->prev_mousey;
				priv->prev_mousex = mx;
				priv->prev_mousey = my;
				priv->tx += dx;
				priv->ty += dy;
				_canvas_clamp_xform(hwnd);
				InvalidateRect(hwnd, NULL, FALSE);
			}

			_canvas_send_notify_mousemove(hwnd, mx, my);
			return 0;
		}

		case WM_MOUSEWHEEL:
		{
			canvas_data_t* priv = _canvas_get_private(hwnd);
			if (!priv->levels[0].hbitmap)
				return 0;
			priv->wheel_accum += (SHORT)HIWORD(wParam);
			int old_zoom = priv->zoom;
			if (abs(priv->wheel_accum) >= WHEEL_DELTA) {
				while (priv->wheel_accum >= WHEEL_DELTA) {
					priv->wheel_accum -= WHEEL_DELTA;
					priv->zoom++;
				}
				while (priv->wheel_accum <= -WHEEL_DELTA) {
					priv->wheel_accum += WHEEL_DELTA;
					priv->zoom--;
				}
				if (priv->zoom > 5)
					priv->zoom = 5;
				if (priv->zoom < -CANVAS_NUM_MINIFY_LEVELS)
					priv->zoom = -CANVAS_NUM_MINIFY_LEVELS;
				if (priv->zoom != old_zoom) {
					POINT pos;
					pos.x = (SHORT)LOWORD(lParam);
					pos.y = (SHORT)HIWORD(lParam);
					// mouse wheel coords are supplied in screen space for some reason
					ScreenToClient(hwnd, &pos);
					int mx = pos.x;
					int my = pos.y;

					double old_scale = pow(2, old_zoom);
					double new_scale = pow(2, priv->zoom);
					priv->tx = (int)(mx - ((double)mx - priv->tx) / old_scale * new_scale);
					priv->ty = (int)(my - ((double)my - priv->ty) / old_scale * new_scale);

					_canvas_clamp_xform(hwnd);

					InvalidateRect(hwnd, NULL, FALSE);
					_canvas_send_notify(hwnd, CANVAS_NM_ZOOM);
				}
			}
			return 0;
		}

	}

	return DefWindowProcW(hwnd, message, wParam, lParam);
}

int canvas_get_zoom(HWND hwnd)
{
	if (!hwnd)
		return 0;
	canvas_data_t* priv = _canvas_get_private(hwnd);
	if (!priv)
		return 0;
	return priv->zoom;
}

bool canvas_get_image_size(HWND hwnd, UINT* width, UINT* height)
{
	if (!hwnd || !width || !height)
		return false;
	canvas_data_t* priv = _canvas_get_private(hwnd);
	if (!priv || !priv->levels[0].hbitmap)
		return false;
	*width = priv->levels[0].width;
	*height = priv->levels[0].height;
	return true;
}

POINT canvas_client_to_image(HWND hwnd, const POINT* client_pos)
{
	POINT image_pos = { 0, 0 };
	canvas_data_t* priv = _canvas_get_private(hwnd);
	if (priv) {
		double scale = pow(2, priv->zoom);
		image_pos.x = (int)floor(((double)client_pos->x - priv->tx) / scale);
		image_pos.y = (int)floor(((double)client_pos->y - priv->ty) / scale);
	}
	return image_pos;
}

bool canvas_set_image(HWND hwnd, const WCHAR* path)
{
	canvas_data_t* priv = _canvas_get_private(hwnd);
	if (!priv)
		return false;

	_canvas_free_levels(priv->levels);
	if (priv->path) {
		free(priv->path);
		priv->path = NULL;
	}

	priv->path = _wcsdup(path);
	if (!priv->path)
		return false;

	InvalidateRect(hwnd, NULL, FALSE);

	priv->zoom = 0;
	priv->tx = 0;
	priv->ty = 0;

	if (!_canvas_reload(priv))
		return false;
	_canvas_clamp_xform(hwnd);
	return true;
}

bool canvas_reload_image(HWND hwnd)
{
	canvas_data_t* priv = _canvas_get_private(hwnd);
	if (!priv)
		return false;

	InvalidateRect(hwnd, NULL, FALSE);

	if (!_canvas_reload(priv))
		return false;
	_canvas_clamp_xform(hwnd);
	return true;
}

ATOM canvas_init_class(HINSTANCE hinstance)
{
	WNDCLASSW wndclass;

	wndclass.style = CS_HREDRAW | CS_VREDRAW;
	wndclass.lpfnWndProc = _canvas_wndproc;
	wndclass.cbClsExtra = 0;
	wndclass.cbWndExtra = sizeof(void*);
	wndclass.hInstance = hinstance;
	wndclass.hIcon = NULL;
	wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
	wndclass.hbrBackground = NULL;
	wndclass.lpszMenuName = NULL;
	wndclass.lpszClassName = CANVAS_CLASS_NAME;

	return RegisterClassW(&wndclass);
}
