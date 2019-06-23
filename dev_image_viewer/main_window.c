#include "dev_image_viewer.h"

#include <CommCtrl.h>
#include <strsafe.h>
#include <PathCch.h>

#include <stdbool.h>

#include "Resource.h"
#include "main_window.h"
#include "canvas.h"

#define MAINWINDOW_WNDLONG_PRIVATE 0

#define MAINWINDOW_TITLE L"Dev Image Viewer"

enum {
	STATUSBAR_PART_MAIN = 0,
	STATUSBAR_PART_SIZE = 1,
	STATUSBAR_PART_COORDS = 2,
	STATUSBAR_PART_ZOOM = 3,
	STATUSBAR_NUM_PARTS = 4,
};
// width of each part.  the first part is ignored, and takes up the remainder.
static const int status_bar_part_sizes[STATUSBAR_NUM_PARTS] = {
	-1,
	120,
	120,
	80,
};

typedef struct {
	HWND canvas;
	HWND status;

	WCHAR* path;
} main_window_t;

main_window_t* _main_window_new_private()
{
	main_window_t* priv = (main_window_t*)malloc(sizeof(main_window_t));
	if (!priv)
		return NULL;
	ZeroMemory(priv, sizeof(main_window_t));

	return priv;
}

void _main_window_destroy_private(main_window_t* priv)
{
	if (priv->path)
		free(priv->path);

	free(priv);
}

static main_window_t* _main_window_get_private(HWND hwnd)
{
	return (main_window_t*)GetWindowLongPtr(hwnd, MAINWINDOW_WNDLONG_PRIVATE);
}

static void _statusbar_set_message(HWND hwnd, const WCHAR* text)
{
	main_window_t* priv = _main_window_get_private(hwnd);
	SendMessageW(priv->status, SB_SETTEXTW, MAKEWPARAM(STATUSBAR_PART_MAIN, 0), (LPARAM)text);
}

static void _statusbar_update_size(HWND hwnd)
{
	main_window_t* priv = _main_window_get_private(hwnd);
	UINT width = 0, height = 0;
	WCHAR text[100];
	if (canvas_get_image_size(priv->canvas, &width, &height) &&
		SUCCEEDED(StringCchPrintfW(text, 100, L"%u \xD7 %u px", width, height)))
		SendMessageW(priv->status, SB_SETTEXTW, MAKEWPARAM(STATUSBAR_PART_SIZE, 0), (LPARAM)text);
	else
		SendMessageW(priv->status, SB_SETTEXTW, MAKEWPARAM(STATUSBAR_PART_SIZE, 0), (LPARAM)L"");
}

static void _statusbar_update_zoom(HWND hwnd)
{
	main_window_t* priv = _main_window_get_private(hwnd);
	int zoom = canvas_get_zoom(priv->canvas);
	WCHAR text[100];
	if (zoom >= 0) {
		if (FAILED(StringCchPrintfW(text, 100, L"%dX", 1 << zoom)))
			return;
	}
	else {
		if (FAILED(StringCchPrintfW(text, 100, L"1/%dX", 1 << (-zoom))))
			return;
	}
	SendMessageW(priv->status, SB_SETTEXTW, MAKEWPARAM(STATUSBAR_PART_ZOOM, 0), (LPARAM)text);
}

static void _statusbar_update_coords(HWND hwnd, canvas_nm_mousemove_t* nm)
{
	main_window_t* priv = _main_window_get_private(hwnd);
	WCHAR text[100];

	POINT client_pos;
	if (nm) {
		client_pos = nm->pos;
	}
	else {
		GetCursorPos(&client_pos);
		ScreenToClient(priv->canvas, &client_pos);
	}
	POINT image_pos = canvas_client_to_image(priv->canvas, &client_pos);

	if (FAILED(StringCchPrintfW(text, 100, L"%d, %d", image_pos.x, image_pos.y)))
		return;

	SendMessageW(priv->status, SB_SETTEXTW, MAKEWPARAM(STATUSBAR_PART_COORDS, 0), (LPARAM)text);
}

static void _update_statusbar_layout(HWND hwnd)
{
	main_window_t* priv = _main_window_get_private(hwnd);
	RECT client_rect;
	GetClientRect(hwnd, &client_rect);
	int width = client_rect.right - client_rect.left;

	// calculate the right edge of each part, starting from the right.
	// whatever is left goes to the first part.
	int parts[STATUSBAR_NUM_PARTS];
	int x = width;
	for (int i = STATUSBAR_NUM_PARTS - 1; i >= 0; i--) {
		parts[i] = x;
		x -= status_bar_part_sizes[i];
	}
	parts[STATUSBAR_NUM_PARTS - 1] = -1;

	SendMessageW(priv->status, SB_SETPARTS, ARRAYSIZE(parts), (LPARAM)parts);
}

// replacement for PathFindFileNameW() so I can remove need for shlwapi
static const WCHAR* _find_file_name(const WCHAR* path)
{
	const WCHAR* filename = path;
	for (const WCHAR* ptr = path; *ptr; ptr++) {
		if (*ptr == L'\\' || *ptr == L'/') {
			if (ptr[1])
				filename = ptr + 1;
		}
	}
	return filename;
}

static bool _filter_dir_entry(const WIN32_FIND_DATAW* ffd)
{
	if (ffd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return false;

	WCHAR* ext = NULL;
	if (FAILED(PathCchFindExtension(ffd->cFileName, ARRAYSIZE(ffd->cFileName),
		&ext)))
		return false;

	if (!_wcsicmp(ext, L".png") || !_wcsicmp(ext, L".jpg") ||
		!_wcsicmp(ext, L".jpeg") || !_wcsicmp(ext, L".tif") ||
		!_wcsicmp(ext, L".gif") || !_wcsicmp(ext, L".bmp"))
		return true;

	return false;
}

// returns true on success, false on error.
// On true, the resulting string must be released with LocalFree(),
// but it may be an empty string.
// If there are no files found at all (including the input_path) then
// the resulting path will be an empty string.
// If only the input file is found, the resulting path will be equal to
// input_path.
static bool _find_prevnext_file(const WCHAR* input_path, bool find_prev, WCHAR** output)
{
	// do not free. points into input_path.
	const WCHAR* filename = _find_file_name(input_path);

	WCHAR* dir_path = _wcsdup(input_path);
	if (!dir_path) {
		return false;
	}
	if (FAILED(PathCchRemoveFileSpec(dir_path, wcslen(dir_path))))
	{
		free(dir_path);
		return false;
	}

	WCHAR* glob = NULL;
	if (FAILED(PathAllocCombine(dir_path, L"*", PATHCCH_ALLOW_LONG_PATHS, &glob))) {
		free(dir_path);
		return false;
	}

	DWORD error;
	WIN32_FIND_DATAW ffd;
	HANDLE hfind = FindFirstFileW(glob, &ffd);
	error = GetLastError();
	LocalFree(glob);
	if (hfind == INVALID_HANDLE_VALUE) {
		free(dir_path);
		if (error == ERROR_FILE_NOT_FOUND) {
			// the glob found nothing, not even input_path.
			// return an empty string in output.
			*output = LocalAlloc(LPTR, sizeof(WCHAR));  // initializes to empty.
			if (!*output)
				return false;
			return true;
		}
		// an error occured
		return false;
	}

	WCHAR target_name[MAX_PATH] = { 0 };

	if (find_prev) {
		bool want_last = false;

		do {
			if (!_filter_dir_entry(&ffd))
				continue;

			if (!want_last && !_wcsicmp(filename, ffd.cFileName)) {
				// found current filename.
				if (!target_name[0])
					want_last = true;
				else
					break;
			}
			if (wcscpy_s(target_name, MAX_PATH, ffd.cFileName))
				target_name[0] = 0;
		} while (FindNextFileW(hfind, &ffd));
	}
	else {
		bool want_next = false;
		bool want_first = true;

		do {
			if (!_filter_dir_entry(&ffd))
				continue;

			if (want_first) {
				// copy the first, in case the last matches the current name
				if (wcscpy_s(target_name, MAX_PATH, ffd.cFileName))
					target_name[0] = 0;
				want_first = false;
			}

			if (want_next) {
				if (wcscpy_s(target_name, MAX_PATH, ffd.cFileName))
					target_name[0] = 0;
				break;
			}
			if (!_wcsicmp(filename, ffd.cFileName)) {
				want_next = true;
			}
		} while (FindNextFileW(hfind, &ffd));
	}

	// is GetLastError possibly messed up above? really only want it to
	// look for errors in FindNextFileW(), but that isn't always the last
	// thing called
	error = GetLastError();
	FindClose(hfind);
	if (FAILED(error) && error != ERROR_NO_MORE_FILES)
	{
		free(dir_path);
		return false;
	}

	if (FAILED(PathAllocCombine(dir_path, target_name, PATHCCH_ALLOW_LONG_PATHS, output))) {
		free(dir_path);
		return false;
	}

	free(dir_path);
	return true;
}

static void _cycle_image(HWND hwnd, bool find_prev)
{
	main_window_t* priv = _main_window_get_private(hwnd);
	if (!priv->path)
		return;

	WCHAR* new_path = NULL;
	if (!_find_prevnext_file(priv->path, find_prev, &new_path)) {
		// ERROR
		return;
	}

	if (new_path[0]) {
		main_window_set_image(hwnd, new_path);
		UpdateWindow(hwnd);
	}
	else {
		// no files were found, not even the old priv->path
		// TODO: should it display in UI that old file no longer found?
	}

	LocalFree(new_path);
}

static void _main_window_update_title(HWND hwnd)
{
	main_window_t* priv = _main_window_get_private(hwnd);

	WCHAR title[2000];
	if (priv->path && priv->path[0]) {
		HRESULT hr = StringCchPrintfW(title, ARRAYSIZE(title), L"%s - %s", priv->path,
			MAINWINDOW_TITLE);
		if (SUCCEEDED(hr) || hr == STRSAFE_E_INSUFFICIENT_BUFFER)
			SetWindowTextW(hwnd, title);
	}
	else {
		SetWindowTextW(hwnd, MAINWINDOW_TITLE);
	}
}

static LRESULT CALLBACK _wndproc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
		case WM_NCCREATE:
		{
			main_window_t* priv = _main_window_new_private();
			if (!priv)
				return FALSE;
			SetWindowLongPtrW(hwnd, MAINWINDOW_WNDLONG_PRIVATE, (LONG_PTR)priv);
			return TRUE;
		}

		case WM_NCDESTROY:
		{
			main_window_t* priv = _main_window_get_private(hwnd);
			if (priv)
				_main_window_destroy_private(priv);
			// I guess don't bother calling SetWindowLongPtr
			return 0;
		}

		case WM_CREATE:
		{
			CREATESTRUCT* cs = (CREATESTRUCT*)lParam;

			if (cs->lpszName)
				SetWindowTextW(hwnd, cs->lpszName);

			main_window_t* priv = _main_window_get_private(hwnd);
			HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
			priv->status = CreateWindowW(STATUSCLASSNAME, L"",
				WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, hwnd, NULL, hInstance,
				NULL);
			_update_statusbar_layout(hwnd);
			_statusbar_update_zoom(hwnd);

			priv->canvas = CreateWindowW(CANVAS_CLASS_NAME, L"",
				WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, hwnd, NULL, hInstance,
				NULL);

			_statusbar_update_size(hwnd);
			_main_window_update_title(hwnd);

			DragAcceptFiles(hwnd, TRUE);

			return 0;
		}

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;

		case WM_NOTIFY:
		{
			main_window_t* priv = _main_window_get_private(hwnd);
			NMHDR* nmhdr = (NMHDR*)lParam;
			if (nmhdr) {
				if (nmhdr->hwndFrom == priv->canvas) {
					switch (nmhdr->code) {
						case CANVAS_NM_ZOOM:
							_statusbar_update_zoom(hwnd);
							_statusbar_update_coords(hwnd, NULL);
							break;

						case CANVAS_NM_MOUSEMOVE:
							_statusbar_update_coords(hwnd,
								(canvas_nm_mousemove_t*)nmhdr);
							break;

						case CANVAS_NM_PREV:
							_cycle_image(hwnd, true);
							break;

						case CANVAS_NM_NEXT:
							_cycle_image(hwnd, false);
							break;

					}
				}
			}
			return 0;
		}

		case WM_SIZE:
		{
			main_window_t* priv = _main_window_get_private(hwnd);
			int width = LOWORD(lParam);
			int height = HIWORD(lParam);

			SetWindowPos(priv->status, NULL, 0, 0, 0, 0, 0);
			RECT status_rect;
			GetWindowRect(priv->status, &status_rect);
			_update_statusbar_layout(hwnd);

			SetWindowPos(priv->canvas, NULL, 0, 0, width,
				height - (status_rect.bottom - status_rect.top), 0);
			return 0;
		}

		case WM_KEYDOWN:
		{
			switch (wParam) {
				case VK_LEFT:
					_cycle_image(hwnd, true);
					return 0;

				case VK_RIGHT:
					_cycle_image(hwnd, false);
					return 0;
			}

			break;
		}

		case WM_DROPFILES:
		{
			HDROP hdrop = (HDROP)wParam;
			UINT count = DragQueryFileW(hdrop, -1, NULL, 0);
			if (count >= 1) {  // just load the first and ignore the rest
				UINT buf_length = DragQueryFileW(hdrop, 0, NULL, 0) + 1;
				if (buf_length < 10000) {
					WCHAR* path = malloc(sizeof(WCHAR) * buf_length);
					if (path) {
						UINT result = DragQueryFileW(hdrop, 0, path,
							buf_length);
						if (result) {
							main_window_set_image(hwnd, path);
						}
						free(path);
					}
				}

			}
			DragFinish(hdrop);
			return 0;
		}

	}

	return DefWindowProc(hwnd, message, wParam, lParam);
}

void main_window_file_changed(HWND hwnd)
{
	main_window_t* priv = _main_window_get_private(hwnd);
	if (!priv)
		return;

	if (canvas_reload_image(priv->canvas))
		_statusbar_set_message(hwnd, L"");
	else
		_statusbar_set_message(hwnd, L"Error reloading image");
	_statusbar_update_size(hwnd);
}

void main_window_set_image(HWND hwnd, const WCHAR* path)
{
	main_window_t* priv = _main_window_get_private(hwnd);
	if (!priv)
		return;

	if (priv->path) {
		free(priv->path);
		priv->path = NULL;
	}
	priv->path = _wcsdup(path);
	if (!priv->path)
		return;		// FIXME - abort or clear canvas too.. probably can't recover anyway

	canvas_set_image(priv->canvas, path);
	_statusbar_set_message(hwnd, L"");
	_statusbar_update_size(hwnd);
	_main_window_update_title(hwnd);

	set_file_watch(path);
}

ATOM main_window_init_class(HINSTANCE hinstance)
{
	WNDCLASSW wndclass;

	wndclass.style = 0;
	wndclass.lpfnWndProc = _wndproc;
	wndclass.cbClsExtra = 0;
	wndclass.cbWndExtra = sizeof(void*);
	wndclass.hInstance = hinstance;
	wndclass.hIcon = LoadIcon(hinstance, MAKEINTRESOURCE(IDI_DEVIMAGEVIEWER));
	wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
	wndclass.hbrBackground = NULL;
	wndclass.lpszMenuName = NULL;
	wndclass.lpszClassName = MAIN_WINDOW_CLASS;

	return RegisterClassW(&wndclass);
}
