#pragma once
#include <windows.h>
#include <string>

namespace taskman {

struct IfeoStatus {
    bool hasEntry = false;          // Any Debugger value exists under IFEO\Taskmgr.exe
    bool isOurs = false;            // Debugger points to TaskmgrEnhanced.exe (or current exe)
    bool matchesCurrentExe = false; // Normalized Debugger path matches current executable
    bool usesTestRoot = false;      // TASKMGR_ENH_IFEO_TEST_ROOT HKCU override active
    std::wstring rawDebugger;       // Raw REG_SZ Debugger value
    std::wstring resolvedPath;      // Extracted executable path
};

struct PathCheckResult {
    bool isBadFilename = false;     // Filename is Taskmgr.exe (would recurse under IFEO)
    bool isVolatilePath = false;    // Inside %TEMP%, Desktop, Downloads, or removable/network drive
    bool isRemovableOrNet = false;
    std::wstring currentExePath;
};

class IfeoManager {
public:
    static constexpr wchar_t kIfeoSubKey[] =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\Taskmgr.exe";

    static std::wstring GetCurrentExePath();
    static bool IsTestRootActive(std::wstring* outHkcuSubKey = nullptr);

    static IfeoStatus QueryStatus();
    static bool IsOurIfeoEnabled();

    static PathCheckResult InspectCurrentExePath();

    static std::wstring ExtractExePathFromCommand(const std::wstring& cmd);
    static bool PathsEqual(const std::wstring& a, const std::wstring& b);

    // Worker-level operations (in-process if elevated or TASKMGR_ENH_IFEO_TEST_ROOT is set)
    static bool InstallDirect(const std::wstring& targetExePath, DWORD* outError = nullptr);
    static bool UninstallDirect(const std::wstring& targetExePath, DWORD* outError = nullptr);

    // High-level operations (auto-elevates via runas `/ifeo-install` / `/ifeo-uninstall` when needed)
    static bool InstallReversible(HWND parent, const std::wstring& targetExePath, DWORD* outError = nullptr);
    static bool UninstallReversible(HWND parent, const std::wstring& targetExePath, DWORD* outError = nullptr);

    // Reversible NTFS DACL + ReadOnly protection
    static bool ApplyFileProtection(const std::wstring& exePath, DWORD* outError = nullptr);
    static bool RemoveFileProtection(const std::wstring& exePath, DWORD* outError = nullptr);
    static bool IsFileProtected(const std::wstring& exePath);

    // Explorer keep-alive (#54): checks current session Explorer and revives if absent
    static void EnsureExplorerAliveAsync();
    static bool IsExplorerAliveInCurrentSession();
    static bool ReviveExplorer(DWORD* outPid = nullptr);
};

} // namespace taskman
