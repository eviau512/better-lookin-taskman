#pragma once
#include <windows.h>
#include <string>
#include "../core/settings.h"

namespace taskman {

void ShowRunNewTaskDialog(HWND parent, const AppSettings& settings);
void ShowAffinityDialog(HWND parent, DWORD pid, const std::wstring& procName, const AppSettings& settings);
bool ShowSelectColumnsDialog(HWND parent, AppSettings& settings);
void ShowAboutDialog(HWND parent, const AppSettings& settings);

} // namespace taskman
