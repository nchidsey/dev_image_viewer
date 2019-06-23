#include "dev_image_viewer.h"

#include <stdlib.h>
#include <math.h>
#include <strsafe.h>
#include <stdbool.h>

#include "canvas.h"
#include "gdiplus_loader.h"

#define CANVAS_WNDLONG_PRIVATE 0

typedef struct {
	WCHAR* path;

	int tx;
	int ty;
	int zoom;

	bool panning;
	int prev_mousex;
	int prev_mousey;
	int wheel_accum;

	HBITMAP hbitmap;
	void* bits;   // owned by hbitmap. do not free.
	int image_width;
	int image_height;

	HBITMAP downsized_hbitmap;
	int downsized_width;
	int downsized_height;

	DWORD bg_color;
	HFONT hfont;
} canvas_data_t;

static canvas_data_t* _canvas_new_private()
{
	canvas_data_t* priv = (canvas_data_t*)malloc(sizeof(canvas_data_t));
	if (!priv)
		return NULL;
	ZeroMemory(priv, sizeof(canvas_data_t));

	priv->bg_color = 0x404040;

	return priv;
}

static void _canvas_unload_image(canvas_data_t* priv)
{
	if (priv->path) {
		free(priv->path);
		priv->path = NULL;
	}
	if (priv->hbitmap) {
		DeleteObject(priv->hbitmap);
		priv->hbitmap = NULL;
	}
	priv->bits = NULL;
	if (priv->downsized_hbitmap) {
		DeleteObject(priv->downsized_hbitmap);
		priv->downsized_hbitmap = NULL;
	}
	priv->image_width = 0;
	priv->image_height = 0;
	priv->downsized_width = 0;
	priv->downsized_height = 0;
}

static void _canvas_destroy_private(canvas_data_t* priv)
{
	_canvas_unload_image(priv);
	if (priv->hfont)
		DeleteObject(priv->hfont);
	free(priv);
}

static canvas_data_t* _canvas_get_private(HWND hwnd)
{
	return (canvas_data_t*)GetWindowLongPtr(hwnd, CANVAS_WNDLONG_PRIVATE);
}

static void _canvas_downsize(canvas_data_t* priv)
{
	if (priv->downsized_hbitmap) {
		DeleteObject(priv->downsized_hbitmap);
		priv->downsized_hbitmap = NULL;
	}

	if (!priv->hbitmap)
		return;

	if (priv->zoom >= 0)
		return;

	int scale = 1 << (-priv->zoom);
	int downsized_width = (priv->image_width + scale - 1) / scale;
	int downsized_height = (priv->image_height + scale - 1) / scale;

	BITMAPV5HEADER bmi;
	init_bitmap_header(&bmi, downsized_width, downsized_height);

	HDC hdc = GetDC(NULL);
	void* bits = NULL;
	HBITMAP hbitmap = CreateDIBSection(hdc, (BITMAPINFO*)&bmi, DIB_RGB_COLORS,
		&bits, NULL, 0);
	ReleaseDC(NULL, hdc);
	if (!hbitmap || !bits) {
		return;
	}

	int back_r = (priv->bg_color >> 16) & 0xFF;
	int back_g = (priv->bg_color >> 8) & 0xFF;
	int back_b = priv->bg_color & 0xFF;

	int r, g, b, a;
	DWORD src_pixel;
	int scale_sqr = scale * scale;
	for (int desty = 0; desty < downsized_height; desty++) {
		for (int destx = 0; destx < downsized_width; destx++) {
			r = g = b = a = 0;
			for (int srcy = desty * scale; srcy < desty * scale + scale; srcy++) {
				for (int srcx = destx * scale; srcx < destx * scale + scale; srcx++) {
					if (srcx < priv->image_width && srcy < priv->image_height) {
						src_pixel = ((DWORD*)priv->bits)[srcy * priv->image_width + srcx];
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

			((DWORD*)bits)[desty * downsized_width + destx] =
				(a << 24) | (r << 16) | (g << 8) | b;
		}
	}

	priv->downsized_hbitmap = hbitmap;
	priv->downsized_width = downsized_width;
	priv->downsized_height = downsized_height;
}

static bool _canvas_reload(canvas_data_t* priv)
{
	if (!canvas_read_image(priv->path, &priv->hbitmap, &priv->bits,
		&priv->image_width, &priv->image_height)) {
		return false;
	}

	// Bake the background color in, making the image opaque.
	int back_r = (priv->bg_color >> 16) & 0xFF;
	int back_g = (priv->bg_color >> 8) & 0xFF;
	int back_b = priv->bg_color & 0xFF;
	int r, g, b, a;
	DWORD* pixel;
	for (int y = 0; y < priv->image_height; y++) {
		for (int x = 0; x < priv->image_width; x++) {
			pixel = &((DWORD*)priv->bits)[y * priv->image_width + x];
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

	_canvas_downsize(priv);

	return true;
}

static void _canvas_paint(HWND hwnd, HDC hdc, PAINTSTRUCT* ps)
{
	canvas_data_t* priv = _canvas_get_private(hwnd);

	RECT client_rect;
	GetClientRect(hwnd, &client_rect);

	// Draw image
	if (priv->hbitmap) {
		int scaled_width;
		int scaled_height;
		int src_width;
		int src_height;

		HDC bitmap_hdc = CreateCompatibleDC(hdc);

		if (priv->zoom < 0) {
			SelectObject(bitmap_hdc, priv->downsized_hbitmap);
			src_width = scaled_width = priv->downsized_width;
			src_height = scaled_height = priv->downsized_height;
		}
		else {
			SelectObject(bitmap_hdc, priv->hbitmap);
			src_width = priv->image_width;
			src_height = priv->image_height;
			scaled_width = priv->image_width << priv->zoom;
			scaled_height = priv->image_height << priv->zoom;
		}

		StretchBlt(hdc, priv->tx, priv->ty, scaled_width, scaled_height,
			bitmap_hdc, 0, 0, src_width, src_height, SRCCOPY);

		DeleteDC(bitmap_hdc);

		// exclude the scaled bitmap rect from the clip, for the background
		// to draw everywhere else.
		ExcludeClipRect(hdc, priv->tx, priv->ty,
			priv->tx + scaled_width, priv->ty + scaled_height);
	}

	// Draw the background inside the requested dirty rect and outside
	// the scaled bitmap.
	HBRUSH bg_brush = CreateSolidBrush(priv->bg_color);
	HGDIOBJ old_brush = SelectObject(hdc, bg_brush);
	FillRect(hdc, &ps->rcPaint, bg_brush);
	SelectObject(hdc, old_brush);
	DeleteObject(bg_brush);

	// Draw message text if appropriate.
	if (!priv->hbitmap) {
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
		scaled_width = priv->downsized_width;
		scaled_height = priv->downsized_height;
	}
	else {
		scaled_width = priv->image_width << priv->zoom;
		scaled_height = priv->image_height << priv->zoom;
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
			if (priv->hbitmap) {
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
			if (!priv->hbitmap)
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
				if (priv->zoom < -5)
					priv->zoom = -5;
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

					_canvas_downsize(priv);
					_canvas_clamp_xform(hwnd);  // must go after downsize..

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
	if (!priv || !priv->hbitmap)
		return false;
	*width = priv->image_width;
	*height = priv->image_height;
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

	_canvas_unload_image(priv);

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
