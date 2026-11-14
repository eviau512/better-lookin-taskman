#pragma once
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <cstdio>
#include <algorithm>

namespace taskman {

inline bool IsCurrentProcessElevated() {
    BOOL elevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION te{};
        DWORD size = sizeof(te);
        if (GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &size)) {
            elevated = te.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return elevated != FALSE;
}

inline bool EnablePrivilege(const wchar_t* privName) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }
    LUID luid{};
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, privName, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(hToken);
    return ok;
}

inline bool RestartCurrentProcessAsAdmin(HWND hwnd) {
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;

    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.hwnd = hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = L"/elevated";
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

inline void OpenFileLocationInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    } else {
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

inline void ShowFilePropertiesDialog(HWND hwnd, const std::wstring& path) {
    if (path.empty()) return;
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.hwnd = hwnd;
    sei.lpVerb = L"properties";
    sei.lpFile = path.c_str();
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.nShow = SW_SHOW;
    ShellExecuteExW(&sei);
}

inline void SearchProcessOnline(const std::wstring& query) {
    if (query.empty()) return;
    std::wstring encoded;
    for (wchar_t ch : query) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.') {
            encoded.push_back(ch);
        } else if (ch == L' ') {
            encoded.push_back(L'+');
        } else {
            wchar_t buf[8];
            swprintf_s(buf, L"%%%02X", static_cast<unsigned>(ch & 0xFF));
            encoded += buf;
        }
    }
    std::wstring url = L"https://www.bing.com/search?q=" + encoded;
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

inline std::wstring FormatMemoryMB(uint64_t bytes) {
    wchar_t buf[64];
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    swprintf_s(buf, L"%.1f MB", mb);
    return buf;
}

inline std::wstring FormatMemoryKB(uint64_t bytes) {
    wchar_t buf[64];
    uint64_t kb = bytes / 1024ULL;
    // Insert thousands commas
    std::wstring raw = std::to_wstring(kb);
    std::wstring formatted;
    int cnt = 0;
    for (int i = static_cast<int>(raw.size()) - 1; i >= 0; --i) {
        if (cnt > 0 && cnt % 3 == 0) formatted.push_back(L',');
        formatted.push_back(raw[i]);
        ++cnt;
    }
    std::reverse(formatted.begin(), formatted.end());
    swprintf_s(buf, L"%s K", formatted.c_str());
    return buf;
}

inline std::wstring FormatBytesAuto(uint64_t bytes) {
    wchar_t buf[64];
    double val = static_cast<double>(bytes);
    if (val >= 1024.0 * 1024.0 * 1024.0 * 1024.0) {
        swprintf_s(buf, L"%.1f TB", val / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    } else if (val >= 1024.0 * 1024.0 * 1024.0) {
        swprintf_s(buf, L"%.1f GB", val / (1024.0 * 1024.0 * 1024.0));
    } else if (val >= 1024.0 * 1024.0) {
        swprintf_s(buf, L"%.1f MB", val / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%.0f KB", val / 1024.0);
    }
    return buf;
}

inline std::wstring FormatDiskSpeed(double bytesPerSec) {
    wchar_t buf[64];
    double mb = bytesPerSec / (1024.0 * 1024.0);
    if (mb >= 100.0) swprintf_s(buf, L"%.0f MB/s", mb);
    else if (mb >= 10.0) swprintf_s(buf, L"%.1f MB/s", mb);
    else if (mb >= 0.05) swprintf_s(buf, L"%.1f MB/s", mb);
    else {
        double kb = bytesPerSec / 1024.0;
        if (kb >= 1.0) swprintf_s(buf, L"%.0f KB/s", kb);
        else swprintf_s(buf, L"0 MB/s");
    }
    return buf;
}

inline std::wstring FormatNetSpeedMbps(double bytesPerSec) {
    wchar_t buf[64];
    double mbps = (bytesPerSec * 8.0) / 1000000.0;
    swprintf_s(buf, L"%.1f Mbps", mbps);
    return buf;
}

inline std::wstring FormatNetSpeedAuto(double bytesPerSec) {
    wchar_t buf[64];
    double bps = bytesPerSec * 8.0;
    if (bps >= 1000000000.0) swprintf_s(buf, L"%.1f Gbps", bps / 1000000000.0);
    else if (bps >= 1000000.0) swprintf_s(buf, L"%.1f Mbps", bps / 1000000.0);
    else swprintf_s(buf, L"%.0f Kbps", bps / 1000.0);
    return buf;
}

inline std::wstring FormatUptime(uint64_t ms) {
    uint64_t sec = ms / 1000ULL;
    uint64_t days = sec / 86400ULL;
    sec %= 86400ULL;
    uint64_t hrs = sec / 3600ULL;
    sec %= 3600ULL;
    uint64_t mins = sec / 60ULL;
    sec %= 60ULL;
    wchar_t buf[64];
    swprintf_s(buf, L"%llu:%02llu:%02llu:%02llu", days, hrs, mins, sec);
    return buf;
}

inline std::wstring FormatDurationHMS(uint64_t totalSeconds) {
    uint64_t hrs = totalSeconds / 3600ULL;
    uint64_t mins = (totalSeconds % 3600ULL) / 60ULL;
    uint64_t secs = totalSeconds % 60ULL;
    wchar_t buf[64];
    swprintf_s(buf, L"%llu:%02llu:%02llu", hrs, mins, secs);
    return buf;
}

inline std::wstring FormatPercent(double pct, int decimals = 1) {
    wchar_t buf[32];
    if (decimals == 0) swprintf_s(buf, L"%.0f%%", pct);
    else swprintf_s(buf, L"%.1f%%", pct);
    return buf;
}

inline BOOL CALLBACK FindTopWindowForPidProc(HWND hwnd, LPARAM lParam) {
    struct Ctx { DWORD pid; HWND found; }* ctx = reinterpret_cast<Ctx*>(lParam);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && GetWindowTextLengthW(hwnd) > 0) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

inline HWND FindMainWindowForPid(DWORD pid) {
    if (pid == 0) return nullptr;
    struct Ctx { DWORD pid; HWND found; } ctx{ pid, nullptr };
    EnumWindows(FindTopWindowForPidProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

inline bool BringWindowToFront(HWND target) {
    if (!target || !IsWindow(target)) return false;
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
    return true;
}

inline bool SwitchToProcessWindow(DWORD pid) {
    return BringWindowToFront(FindMainWindowForPid(pid));
}

inline POINT ResolveListContextMenuPoint(HWND hList, int sel, POINT screenPt) {
    if (screenPt.x != -1 || screenPt.y != -1) return screenPt;
    RECT rc{};
    if (sel >= 0 && ListView_GetItemRect(hList, sel, &rc, LVIR_BOUNDS)) {
        int offset = (rc.bottom - rc.top) > 16 ? (rc.bottom - rc.top) : 16;
        POINT pt{ rc.left + offset, (rc.top + rc.bottom) / 2 };
        ClientToScreen(hList, &pt);
        return pt;
    }
    RECT wrc{};
    GetWindowRect(hList, &wrc);
    return POINT{ wrc.left + 32, wrc.top + 32 };
}

inline POINT HandleListRightClickSelection(HWND hList, NMHDR* nm, int itemCount) {
    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(nm);
    int hitRow = nmia ? nmia->iItem : -1;
    if (hitRow < 0 && nmia) {
        LVHITTESTINFO hti{};
        hti.pt = nmia->ptAction;
        hitRow = ListView_SubItemHitTest(hList, &hti);
    }
    if (hitRow >= 0 && hitRow < itemCount) {
        ListView_SetItemState(hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(hList, hitRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    POINT pt{};
    if (nmia && nmia->ptAction.x >= 0 && nmia->ptAction.y >= 0) {
        pt = nmia->ptAction;
        ClientToScreen(hList, &pt);
    } else if (!GetCursorPos(&pt)) {
        pt = { -1, -1 };
    }
    return pt;
}

} // namespace taskman
