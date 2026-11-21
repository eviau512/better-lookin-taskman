#include "settings.h"

namespace taskman {

static const wchar_t* kRegPath = L"Software\\BetterWin10Taskman";

static void ReadDword(HKEY hKey, const wchar_t* name, DWORD& out) {
    DWORD type = 0, size = sizeof(DWORD), val = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        out = val;
    }
}

static void ReadBool(HKEY hKey, const wchar_t* name, bool& out) {
    DWORD val = out ? 1 : 0;
    ReadDword(hKey, name, val);
    out = (val != 0);
}

static void WriteDword(HKEY hKey, const wchar_t* name, DWORD val) {
    RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
}

static void ReadIntArray(HKEY hKey, const wchar_t* name, int* dst, int count, int minVal, int maxVal) {
    int tmp[32] = {};
    if (count <= 0 || count > 32) return;
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(count) * static_cast<DWORD>(sizeof(int));
    if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<BYTE*>(tmp), &size) == ERROR_SUCCESS &&
        type == REG_BINARY &&
        size == static_cast<DWORD>(count) * static_cast<DWORD>(sizeof(int))) {
        for (int i = 0; i < count; ++i) {
            if (tmp[i] >= minVal && tmp[i] <= maxVal) {
                dst[i] = tmp[i];
            }
        }
    }
}

static void WriteIntArray(HKEY hKey, const wchar_t* name, const int* src, int count) {
    RegSetValueExW(hKey, name, 0, REG_BINARY,
                   reinterpret_cast<const BYTE*>(src),
                   static_cast<DWORD>(count) * static_cast<DWORD>(sizeof(int)));
}

void LoadSettings(AppSettings& s) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    DWORD tmp = 0;
    tmp = static_cast<DWORD>(s.themeMode);
    ReadDword(hKey, L"ThemeMode", tmp);
    if (tmp <= 2) s.themeMode = static_cast<ThemeMode>(tmp);

    tmp = static_cast<DWORD>(s.langMode);
    ReadDword(hKey, L"LangMode", tmp);
    if (tmp <= static_cast<DWORD>(LangMode::Russian)) s.langMode = static_cast<LangMode>(tmp);

    tmp = static_cast<DWORD>(s.updateSpeed);
    ReadDword(hKey, L"UpdateSpeed", tmp);
    if (tmp <= 3) s.updateSpeed = static_cast<UpdateSpeed>(tmp);

    ReadBool(hKey, L"AlwaysOnTop", s.alwaysOnTop);
    ReadBool(hKey, L"MinimizeOnUse", s.minimizeOnUse);
    ReadBool(hKey, L"HideWhenMinimized", s.hideWhenMinimized);
    ReadBool(hKey, L"ShowFullAccountName", s.showFullAccountName);
    ReadBool(hKey, L"ShowAllHistory", s.showAllHistory);
    ReadBool(hKey, L"PreLaunchSampling", s.preLaunchSampling);
    ReadBool(hKey, L"PreLaunchSamplingUserSet", s.preLaunchSamplingUserSet);
    ReadBool(hKey, L"KeepExplorerAlive", s.keepExplorerAlive);
    ReadBool(hKey, L"GroupByType", s.groupByType);
    ReadBool(hKey, L"ShowKernelTimes", s.showKernelTimes);
    ReadBool(hKey, L"CpuLogicalView", s.cpuLogicalView);
    ReadBool(hKey, L"SummaryView", s.summaryView);
    ReadBool(hKey, L"MoreDetails", s.moreDetails);

    tmp = static_cast<DWORD>(s.activeTab);
    ReadDword(hKey, L"ActiveTab", tmp);
    if (tmp <= 7) s.activeTab = static_cast<int>(tmp);

    tmp = static_cast<DWORD>(s.perfSelectedItem);
    ReadDword(hKey, L"PerfSelectedItem", tmp);
    s.perfSelectedItem = static_cast<int>(tmp);

    ReadBool(hKey, L"MemShowPercent", s.memShowPercent);
    ReadBool(hKey, L"DiskShowPercent", s.diskShowPercent);
    ReadBool(hKey, L"NetShowPercent", s.netShowPercent);
    ReadBool(hKey, L"Maximized", s.maximized);

    RECT rc = s.windowRect;
    DWORD type = 0, size = sizeof(rc);
    if (RegQueryValueExW(hKey, L"WindowRect", nullptr, &type, reinterpret_cast<BYTE*>(&rc), &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size == sizeof(rc)) {
        int rw = rc.right - rc.left;
        int rh = rc.bottom - rc.top;
        if (rc.left > -32000 && rc.top > -32000 && rw >= 350 && rw <= 16000 && rh >= 260 && rh <= 16000) {
            s.windowRect = rc;
        }
    }

    DWORD procMask = s.procColMask;
    ReadDword(hKey, L"ProcColMask", procMask);
    s.procColMask = (procMask & 0x3FFu) | 1u; // Name always visible

    DWORD colMask = s.detailsColMask;
    ReadDword(hKey, L"DetailsColMask", colMask);
    s.detailsColMask = colMask;
    s.detailsColMask |= (1u << DCOL_NAME); // Name always visible

    ReadIntArray(hKey, L"ProcColWidths",     s.procColWidths,     10,         24, 2400);
    ReadIntArray(hKey, L"AppHistColWidths",  s.appHistColWidths,  5,          24, 2400);
    ReadIntArray(hKey, L"StartupColWidths",  s.startupColWidths,  4,          24, 2400);
    ReadIntArray(hKey, L"UsersColWidths",    s.usersColWidths,    6,          24, 2400);
    ReadIntArray(hKey, L"DetailsColWidths",  s.detailsColWidths,  DCOL_COUNT, 24, 2400);
    ReadIntArray(hKey, L"ServicesColWidths", s.servicesColWidths, 5,          24, 2400);
    ReadIntArray(hKey, L"LocksColWidths",    s.locksColWidths,    5,          24, 2400);

    RegCloseKey(hKey);
}

void SaveSettings(const AppSettings& s) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return;
    }

    WriteDword(hKey, L"ThemeMode", static_cast<DWORD>(s.themeMode));
    WriteDword(hKey, L"LangMode", static_cast<DWORD>(s.langMode));
    WriteDword(hKey, L"UpdateSpeed", static_cast<DWORD>(s.updateSpeed));

    WriteDword(hKey, L"AlwaysOnTop", s.alwaysOnTop ? 1 : 0);
    WriteDword(hKey, L"MinimizeOnUse", s.minimizeOnUse ? 1 : 0);
    WriteDword(hKey, L"HideWhenMinimized", s.hideWhenMinimized ? 1 : 0);
    WriteDword(hKey, L"ShowFullAccountName", s.showFullAccountName ? 1 : 0);
    WriteDword(hKey, L"ShowAllHistory", s.showAllHistory ? 1 : 0);
    WriteDword(hKey, L"PreLaunchSampling", s.preLaunchSampling ? 1 : 0);
    WriteDword(hKey, L"PreLaunchSamplingUserSet", s.preLaunchSamplingUserSet ? 1 : 0);
    WriteDword(hKey, L"KeepExplorerAlive", s.keepExplorerAlive ? 1 : 0);
    WriteDword(hKey, L"GroupByType", s.groupByType ? 1 : 0);
    WriteDword(hKey, L"ShowKernelTimes", s.showKernelTimes ? 1 : 0);
    WriteDword(hKey, L"CpuLogicalView", s.cpuLogicalView ? 1 : 0);
    WriteDword(hKey, L"SummaryView", s.summaryView ? 1 : 0);
    WriteDword(hKey, L"MoreDetails", s.moreDetails ? 1 : 0);
    WriteDword(hKey, L"ActiveTab", static_cast<DWORD>(s.activeTab));
    WriteDword(hKey, L"PerfSelectedItem", static_cast<DWORD>(s.perfSelectedItem));

    WriteDword(hKey, L"MemShowPercent", s.memShowPercent ? 1 : 0);
    WriteDword(hKey, L"DiskShowPercent", s.diskShowPercent ? 1 : 0);
    WriteDword(hKey, L"NetShowPercent", s.netShowPercent ? 1 : 0);
    WriteDword(hKey, L"Maximized", s.maximized ? 1 : 0);

    RegSetValueExW(hKey, L"WindowRect", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&s.windowRect), sizeof(s.windowRect));
    WriteDword(hKey, L"ProcColMask", s.procColMask);
    WriteDword(hKey, L"DetailsColMask", s.detailsColMask);
    WriteIntArray(hKey, L"ProcColWidths",     s.procColWidths,     10);
    WriteIntArray(hKey, L"AppHistColWidths",  s.appHistColWidths,  5);
    WriteIntArray(hKey, L"StartupColWidths",  s.startupColWidths,  4);
    WriteIntArray(hKey, L"UsersColWidths",    s.usersColWidths,    6);
    WriteIntArray(hKey, L"DetailsColWidths",  s.detailsColWidths,  DCOL_COUNT);
    WriteIntArray(hKey, L"ServicesColWidths", s.servicesColWidths, 5);
    WriteIntArray(hKey, L"LocksColWidths",    s.locksColWidths,    5);

    RegCloseKey(hKey);
}

DWORD GetUpdateIntervalMs(UpdateSpeed speed) {
    switch (speed) {
        case UpdateSpeed::High:   return 500;
        case UpdateSpeed::Normal: return 1000;
        case UpdateSpeed::Low:    return 4000;
        case UpdateSpeed::Paused: return 0;
    }
    return 1000;
}

} // namespace taskman
