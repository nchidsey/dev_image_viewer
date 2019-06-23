#pragma once

#define MAIN_WINDOW_CLASS L"main_window_cls"

void set_file_watch(const WCHAR* path);

void main_window_file_changed(HWND hwnd);
void main_window_set_image(HWND hwnd, const WCHAR* path);
ATOM main_window_init_class(HINSTANCE hInstance);
