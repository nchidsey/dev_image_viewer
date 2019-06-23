#include "dev_image_viewer.h"

#include <CommCtrl.h>
#include <PathCch.h>

#include <stdbool.h>
#include <stdlib.h>

#include "gdiplus_loader.h"
#include "main_window.h"
#include "canvas.h"

static WCHAR* file_change_path = NULL;
static HANDLE file_change_handle = INVALID_HANDLE_VALUE;
static FILETIME file_time = { 0 };

static void cleanup_file_watch()
{
	if (file_change_path) {
		free(file_change_path);
		file_change_path = NULL;
	}
	if (file_change_handle != INVALID_HANDLE_VALUE) {
		FindCloseChangeNotification(file_change_handle);
		file_change_handle = INVALID_HANDLE_VALUE;
	}
}

static FILETIME _get_file_time()
{
	WIN32_FILE_ATTRIBUTE_DATA info = { 0 };
	// ignoring error, assuming info.ftLastWriteTime is still zeros.
	GetFileAttributesExW(file_change_path, GetFileExInfoStandard, &info);
	return info.ftLastWriteTime;
}

// TODO: this silently ignores errors. the app just won't reload.
void set_file_watch(const WCHAR* path)
{
	cleanup_file_watch();

	file_change_path = _wcsdup(path);
	if (!file_change_path)
		return;

	WCHAR* dir_path = _wcsdup(path);
	if (!dir_path) {
		cleanup_file_watch();
		return;
	}
	if (FAILED(PathCchRemoveFileSpec(dir_path, wcslen(dir_path))))
	{
		free(dir_path);
		cleanup_file_watch();
		return;
	}

	file_change_handle = FindFirstChangeNotificationW(
		dir_path, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE |
		FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_CREATION |
		FILE_NOTIFY_CHANGE_FILE_NAME);
	free(dir_path);
	if (file_change_handle == INVALID_HANDLE_VALUE) {
		cleanup_file_watch();
		return;
	}

	file_time = _get_file_time();
}

// the file_change_handle has been signaled.
// returns true if the watched file was changed, false otherwise.
// no error return values.
static bool check_file_watch()
{
	if (!FindNextChangeNotification(file_change_handle)) {
		// TODO: indicate error in UI? retry later?
		cleanup_file_watch();
	}

	FILETIME new_time = _get_file_time();
	if (new_time.dwHighDateTime != file_time.dwHighDateTime ||
		new_time.dwLowDateTime != file_time.dwLowDateTime) {
		file_time = new_time;
		return true;
	}

	return false;
}

// the resulting buffer must be freed with LocalFree()
static WCHAR* _make_path_absolute(const WCHAR* path)
{
	DWORD cwd_bufcch = GetCurrentDirectory(0, NULL);
	if (!cwd_bufcch)
		return NULL;
	WCHAR* cwd = (WCHAR*)malloc(sizeof(WCHAR) * cwd_bufcch);
	if (!cwd)
		return NULL;
	if (!GetCurrentDirectoryW(cwd_bufcch, cwd)) {
		free(cwd);
		return NULL;
	}

	WCHAR* result = NULL;
	if (FAILED(PathAllocCombine(cwd, path, PATHCCH_ALLOW_LONG_PATHS, &result))) {
		free(cwd);
		return NULL;
	}
	free(cwd);
	return result;
}

static int _message_loop(HWND hwnd)
{
	MSG msg = { 0 };
	DWORD wait_result;
	DWORD num_handles;

	while (msg.message != WM_QUIT) {
		num_handles = 0;
		if (file_change_handle != INVALID_HANDLE_VALUE)
			num_handles = 1;

		wait_result = MsgWaitForMultipleObjectsEx(num_handles,
			&file_change_handle, INFINITE, QS_ALLINPUT, 0);

		if (wait_result == WAIT_OBJECT_0 + num_handles) {
			// one or more messages are in the queue
			while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_QUIT)
					break;
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
		else if (wait_result >= WAIT_OBJECT_0 &&
				 wait_result < WAIT_OBJECT_0 + num_handles) {
			if (wait_result == WAIT_OBJECT_0) {
				// the change notification.
				if (check_file_watch())
					main_window_file_changed(hwnd);
			}
		}
		else if (wait_result == WAIT_FAILED)
		{
			DWORD error = GetLastError();
		}
		else {
			// ignoring WAIT_ABANDONED_0...n-1 and WAIT_TIMEOUT
		}
	}

	return (int)msg.wParam;
}

int APIENTRY wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	// Initialize libraries and window classes
	init_gdiplus_loader();
	main_window_init_class(hInstance);
	canvas_init_class(hInstance);

	// Process command line
	int argc = 0;
	LPWSTR full_cmd_line = GetCommandLineW();
	LPWSTR* argv = CommandLineToArgvW(full_cmd_line, &argc);
	if (!argv) {
		MessageBoxW(NULL, L"error getting command line", L"dev_image_viewer",
			MB_OK | MB_ICONERROR);
		return 0;
	}

	// image_path points into argv; don't free.
	const WCHAR* image_path = NULL;

	if (argc == 1) {
		// with no parameters, just load empty window.
	}
	else if (argc == 2) {
		image_path = argv[1];
	}
	else {
		MessageBoxW(NULL, L"invalid command line arguments", L"dev_image_viewer",
			MB_OK | MB_ICONERROR);
	}

	// TODO: technically can call LocalFree() on result of CommandLineToArgvW().

	// Setup window
	HWND hwnd = CreateWindowW(MAIN_WINDOW_CLASS, L"",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL,
		NULL, hInstance, NULL);
	if (!hwnd)
		return 0;

	if (image_path) {
		WCHAR* abs_path = NULL;
		if (image_path) {
			abs_path = _make_path_absolute(image_path);
			if (!abs_path)
				return 0;
		}

		main_window_set_image(hwnd, abs_path);

		LocalFree(abs_path);
	}

	// Main loop
	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	int exit_code = _message_loop(hwnd);

	// Cleanup
	cleanup_file_watch();
	destroy_gdiplus_loader();

	return exit_code;
}
