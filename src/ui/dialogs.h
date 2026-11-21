#pragma once
#include <windows.h>
#include <string>
#include "../core/settings.h"
#include "../core/ifeo_manager.h"

namespace taskman {

void ShowRunNewTaskDialog(HWND parent, const AppSettings& settings);
void ShowAffinityDialog(HWND parent, DWORD pid, const std::wstring& procName, const AppSettings& settings);
bool ShowSelectColumnsDialog(HWND parent, AppSettings& settings);
void ShowAboutDialog(HWND parent, const AppSettings& settings);

enum class IfeoPathChoice {
    Cancel = 0,
    UseCurrent = 1,
    CopyToFolder = 2,
};

IfeoPathChoice ShowIfeoPathChoiceDialog(HWND parent, const AppSettings& settings,
                                        const PathCheckResult& pathCheck, std::wstring& outTargetPath);
void ShowIfeoCountdownDialog(HWND parent, const AppSettings& settings, const std::wstring& protectedExePath);

} // namespace taskman
