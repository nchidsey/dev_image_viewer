#pragma once

#define CANVAS_CLASS_NAME L"canvas"

// WM_NOTIFY message codes
#define CANVAS_NM_ZOOM			1
#define CANVAS_NM_MOUSEMOVE		2
#define CANVAS_NM_PREV			3
#define CANVAS_NM_NEXT			4

// parameter for CANVAS_NM_MOUSEMOVE
typedef struct {
	NMHDR nmhdr;
	POINT pos;	// client coords
} canvas_nm_mousemove_t;

ATOM canvas_init_class(HINSTANCE inst);

bool canvas_set_image(HWND hwnd, const WCHAR* path);
bool canvas_reload_image(HWND hwnd);
int canvas_get_zoom(HWND hwnd);
bool canvas_get_image_size(HWND hwnd, UINT* width, UINT* height);
POINT canvas_client_to_image(HWND hwnd, const POINT* client_pos);
