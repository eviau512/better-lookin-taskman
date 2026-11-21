#include "main_window.h"
#include "dialogs.h"
#include "../resource.h"
#include "../core/theme.h"
#include "../core/i18n.h"
#include "../core/format_util.h"
#include "../core/ifeo_manager.h"

#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace taskman {

namespace {

constexpr UINT_PTR kRefreshTimerId = 1;
constexpr UINT_PTR kWarmupTimerId = 2;
constexpr UINT_PTR kPerfSampleTimerId = 3;

std::wstring StripMnemonicAmpersand(const wchar_t* s) {
    std::wstring out;
    if (!s) return out;
    for (size_t i = 0; s[i] != L'\0'; ++i) {
        if (s[i] == L'&') {
            if (s[i + 1] == L'&') {
                out.push_back(L'&');
                ++i;
            }
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

int MeasureButtonTextWidth(HDC hdc, HWND hwnd, HWND btn) {
    wchar_t buf[128] = {};
    GetWindowTextW(btn, buf, 128);
    std::wstring visible = StripMnemonicAmpersand(buf);
    if (visible.empty()) return 0;
    return ThemeManager::Instance().MeasureTextWidth(hdc, hwnd, 10, false, visible.c_str());
}

void MoveButtonIfChanged(HWND parent, HWND btn, int x, int y, int bw, int bh) {
    if (!btn) return;
    RECT rc{};
    GetWindowRect(btn, &rc);
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<LPPOINT>(&rc), 2);
    if (rc.left != x || rc.top != y || (rc.right - rc.left) != bw || (rc.bottom - rc.top) != bh) {
        MoveWindow(btn, x, y, bw, bh, TRUE);
        InvalidateRect(btn, nullptr, FALSE);
    }
}

LRESULT CALLBACK OwnerBtnHoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    bool* hoverFlag = reinterpret_cast<bool*>(dwRefData);
    switch (msg) {
    case WM_MOUSEMOVE:
        if (hoverFlag && !*hoverFlag) {
            *hoverFlag = true;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (hoverFlag && *hoverFlag) {
            *hoverFlag = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, OwnerBtnHoverProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    HWND hMain = reinterpret_cast<HWND>(dwRefData);
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (GetWindowTextLengthW(hwnd) > 0) {
                SetWindowTextW(hwnd, L"");
            } else if (hMain) {
                SetFocus(hMain);
            }
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == 1 || wParam == 27) return 0; // Suppress Ctrl+A / Esc beep
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (hMain) InvalidateRect(hMain, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, SearchEditSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

bool MainWindow::Create(int nCmdShow, int initialTab) {
    LoadSettings(settings_);
    if (IfeoManager::IsOurIfeoEnabled() && !settings_.preLaunchSamplingUserSet && !settings_.preLaunchSampling) {
        settings_.preLaunchSampling = true;
        SaveSettings(settings_);
    }
    if (settings_.keepExplorerAlive) {
        IfeoManager::EnsureExplorerAliveAsync();
    }
    if (initialTab >= 0 && initialTab < kTabCount) {
        settings_.activeTab = initialTab;
        settings_.moreDetails = true;
    }
    settings_.activeTab = std::clamp(settings_.activeTab, 0, kTabCount - 1);

    ThemeManager::Instance().Init();
    ThemeManager::Instance().ApplyTheme(settings_.themeMode);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    // Compute initial window rect (DPI-scaled default when no saved rect exists)
    UINT sysDpi = 96;
    if (HDC sdc = GetDC(nullptr)) {
        sysDpi = static_cast<UINT>(GetDeviceCaps(sdc, LOGPIXELSX));
        ReleaseDC(nullptr, sdc);
    }
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    int w = MulDiv(1020, static_cast<int>(sysDpi), 96);
    int h = MulDiv(700, static_cast<int>(sysDpi), 96);
    bool hasSavedRect = false;
    if (!settings_.moreDetails) {
        w = MulDiv(480, static_cast<int>(sysDpi), 96);
        h = MulDiv(440, static_cast<int>(sysDpi), 96);
    } else if (settings_.windowRect.left > -32000 && settings_.windowRect.top > -32000 &&
               (settings_.windowRect.right - settings_.windowRect.left) >= 350 &&
               (settings_.windowRect.bottom - settings_.windowRect.top) >= 260) {
        RECT r = settings_.windowRect;
        HMONITOR hMon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            int workW = mi.rcWork.right - mi.rcWork.left;
            int workH = mi.rcWork.bottom - mi.rcWork.top;
            w = std::clamp<int>(r.right - r.left, MulDiv(740, static_cast<int>(sysDpi), 96), workW);
            h = std::clamp<int>(r.bottom - r.top, MulDiv(600, static_cast<int>(sysDpi), 96), workH);
            x = std::clamp<int>(r.left, mi.rcWork.left, mi.rcWork.right - 250);
            y = std::clamp<int>(r.top, mi.rcWork.top, mi.rcWork.bottom - 200);
            hasSavedRect = true;
        }
    }

    settings_.summaryView = false;
    const Strings& s = GetStrings(settings_.langMode);
    const wchar_t* title = IsCurrentProcessElevated() ? s.appTitleAdmin : s.appTitle;

    DWORD exStyle = settings_.alwaysOnTop ? WS_EX_TOPMOST : 0;
    hwnd_ = CreateWindowExW(
        exStyle, kWndClass, title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, w, h,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    WNDCLASSEXW wcSum{ sizeof(wcSum) };
    wcSum.style = CS_DBLCLKS;
    wcSum.lpfnWndProc = SummaryWndProc;
    wcSum.hInstance = hInst;
    wcSum.hIcon = wc.hIcon;
    wcSum.hIconSm = wc.hIconSm;
    wcSum.hCursor = wc.hCursor;
    wcSum.hbrBackground = nullptr;
    wcSum.lpszClassName = kSummaryWndClass;
    RegisterClassExW(&wcSum);

    hSummaryWnd_ = CreateWindowExW(
        WS_EX_APPWINDOW | (settings_.alwaysOnTop ? WS_EX_TOPMOST : 0),
        kSummaryWndClass, title,
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 260, 420,
        nullptr, nullptr, hInst, this);

    ChangeWindowMessageFilterEx(hwnd_, WM_APP_BRING_TO_FRONT, MSGFLT_ALLOW, nullptr);

    InitMonitorsAndTabs();
    perfMon_.SetUpdateSpeed(settings_.updateSpeed);
    RebuildMenu();
    ApplyCurrentTheme(false);

    // Single fast baseline sample before showing window; live CPU/IO deltas populate 120ms later via kWarmupTimerId.
    bool needCmd = (settings_.moreDetails && settings_.activeTab == 5 && tabDetails_.NeedsCommandLines());
    procMon_.Refresh(settings_.showFullAccountName, needCmd);
    DWORD totalThreads = 0;
    DWORD totalHandles = 0;
    for (const auto& p : procMon_.Processes()) {
        totalThreads += p.threadCount;
        totalHandles += p.handleCount;
    }
    perfMon_.Sample(static_cast<DWORD>(procMon_.Processes().size()), totalThreads, totalHandles);
    tabPerf_.UpdateData(perfMon_);
    appHistory_.Accumulate(procMon_.Processes(), 0.0);

    SwitchTab(settings_.activeTab);
    LayoutChildren();
    if (settings_.moreDetails && !settings_.maximized) {
        RECT curWrc{};
        GetWindowRect(hwnd_, &curWrc);
        int minW = GetMinimumDetailedWidth();
        int minH = GetMinimumDetailedHeight();
        int targetW = hasSavedRect ? std::max<int>(curWrc.right - curWrc.left, minW) : minW;
        int targetH = hasSavedRect ? std::max<int>(curWrc.bottom - curWrc.top, minH) : minH;
        int targetX = curWrc.left;
        int targetY = curWrc.top;
        HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            targetW = std::min<int>(targetW, mi.rcWork.right - mi.rcWork.left);
            targetH = std::min<int>(targetH, mi.rcWork.bottom - mi.rcWork.top);
            if (targetX + targetW > mi.rcWork.right) {
                targetX = std::max<int>(mi.rcWork.left, mi.rcWork.right - targetW);
            }
            if (targetY + targetH > mi.rcWork.bottom) {
                targetY = std::max<int>(mi.rcWork.top, mi.rcWork.bottom - targetH);
            }
        }
        if (targetW != (curWrc.right - curWrc.left) || targetH != (curWrc.bottom - curWrc.top) ||
            targetX != curWrc.left || targetY != curWrc.top) {
            SetWindowPos(hwnd_, nullptr, targetX, targetY, targetW, targetH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    SetUpdateSpeed(settings_.updateSpeed);

    int showCmd = (settings_.moreDetails && settings_.maximized) ? SW_SHOWMAXIMIZED : nCmdShow;
    ShowWindow(hwnd_, showCmd);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kWarmupTimerId, 120, nullptr);
    SetTimer(hwnd_, kPerfSampleTimerId, 500, nullptr);
    return true;
}

void MainWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void MainWindow::InitMonitorsAndTabs() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    procMon_.Start();
    perfMon_.Init();
    appHistory_.InitAsync(hwnd_, WM_APP_HISTORY_READY);
    lastSampleTick_ = GetTickCount64();
    lastStartupRefreshTick_ = 0;
    lastServiceRefreshTick_ = 0;

    // Footer buttons (owner-drawn for crisp Light & Dark parity)
    hBtnMore_ = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 120, 26,
        hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_MORE_DETAILS)), hInst, nullptr);
    SetWindowSubclass(hBtnMore_, OwnerBtnHoverProc, 1, reinterpret_cast<DWORD_PTR>(&hoverBtnMore_));

    hBtnAction_ = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 100, 26,
        hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_PRIMARY_ACTION)), hInst, nullptr);
    SetWindowSubclass(hBtnAction_, OwnerBtnHoverProc, 1, reinterpret_cast<DWORD_PTR>(&hoverBtnAction_));

    hSearchEdit_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 22,
        hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_EDIT_FOOTER_SEARCH)), hInst, nullptr);
    if (hSearchEdit_) {
        SetWindowSubclass(hSearchEdit_, SearchEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(hwnd_));
    }

    // Simple mode running apps list
    hListSimple_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100,
        hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LIST_SIMPLE)), hInst, nullptr);
    simpleLv_.Attach(hListSimple_, false);
    {
        LVCOLUMNW col{};
        col.mask = LVCF_WIDTH;
        col.cx = DpiScale(hwnd_, 420);
        ListView_InsertColumn(hListSimple_, 0, &col);

        ListViewThemeCallbacks cb;
        cb.getCellText = [this](int row, int col) -> std::wstring {
            if (col == 0 && row >= 0 && row < static_cast<int>(simpleRows_.size())) {
                return simpleRows_[static_cast<size_t>(row)].title;
            }
            return L"";
        };
        cb.getRowIcon = [this](int row) -> HICON {
            if (row >= 0 && row < static_cast<int>(simpleRows_.size())) {
                return simpleRows_[static_cast<size_t>(row)].icon;
            }
            return nullptr;
        };
        cb.getRowIndent = [](int) { return 1; };
        cb.getRowExpandState = [](int) { return -1; };
        cb.isCategoryHeader = [](int) { return false; };
        simpleLv_.SetCallbacks(std::move(cb));
    }

    // Create the 7 tabs
    tabProcesses_.Create(hwnd_, &settings_);
    tabProcesses_.onGoToDetails = [this](DWORD pid) { NavigateToDetails(pid); };
    tabProcesses_.onActionMinimize = [this]() {
        if (settings_.minimizeOnUse) ShowWindow(hwnd_, SW_MINIMIZE);
    };
    tabProcesses_.onColumnsChanged = [this]() { SaveSettings(settings_); };

    tabPerf_.Create(hwnd_, &settings_);
    tabPerf_.onSummaryModeChanged = [this](PerfSummaryMode mode) {
        SetPerfSummaryMode(mode);
    };
    tabPerf_.onSpeedChanged = [this](UpdateSpeed speed) {
        SetUpdateSpeed(speed);
    };

    tabAppHistory_.Create(hwnd_, &settings_);
    tabAppHistory_.onDeleteHistory = [this]() {
        appHistory_.DeleteHistory();
        tabAppHistory_.RefreshData(appHistory_);
    };
    tabAppHistory_.onColumnsChanged = [this]() { SaveSettings(settings_); };

    tabStartup_.Create(hwnd_, &settings_);
    tabStartup_.onSelectionChanged = [this]() { UpdatePrimaryActionButton(); };
    tabStartup_.onRefreshNeeded = [this]() {
        startupMgr_.Refresh();
        lastStartupRefreshTick_ = GetTickCount64();
        startupInited_ = true;
        tabStartup_.RefreshData(startupMgr_);
        UpdatePrimaryActionButton();
    };
    tabStartup_.onColumnsChanged = [this]() { SaveSettings(settings_); };

    tabUsers_.Create(hwnd_, &settings_);
    tabUsers_.onGoToDetails = [this](DWORD pid) { NavigateToDetails(pid); };
    tabUsers_.onSelectionChanged = [this]() { UpdatePrimaryActionButton(); };
    tabUsers_.onRefreshNeeded = [this]() { RefreshAll(true); };
    tabUsers_.onColumnsChanged = [this]() { SaveSettings(settings_); };

    tabDetails_.Create(hwnd_, &settings_);
    tabDetails_.onGoToService = [this](DWORD pid) {
        SwitchTab(6);
        tabServices_.SelectServiceByPid(pid);
        SyncSearchBoxState();
    };
    tabDetails_.onSelectionChanged = [this]() { UpdatePrimaryActionButton(); };
    tabDetails_.onRefreshNeeded = [this]() { RefreshAll(true); };
    tabDetails_.onActionMinimize = [this]() {
        if (settings_.minimizeOnUse) ShowWindow(hwnd_, SW_MINIMIZE);
    };
    tabDetails_.onColumnsChanged = [this]() { SaveSettings(settings_); };

    tabServices_.Create(hwnd_, &settings_);
    tabServices_.onGoToDetails = [this](DWORD pid) { NavigateToDetails(pid); };
    tabServices_.onRefreshNeeded = [this]() {
        serviceMgr_.Refresh();
        lastServiceRefreshTick_ = GetTickCount64();
        servicesInited_ = true;
        tabServices_.RefreshData(serviceMgr_);
    };
    tabServices_.onColumnsChanged = [this]() { SaveSettings(settings_); };

    tabLocks_.Create(hwnd_, &settings_, &procMon_);
    tabLocks_.onGoToDetails = [this](DWORD pid) { NavigateToDetails(pid); };
    tabLocks_.onSelectionChanged = [this]() { UpdatePrimaryActionButton(); };
    tabLocks_.onColumnsChanged = [this]() { SaveSettings(settings_); };
    tabLocks_.onElevateRequested = [this]() { HandleCommand(IDM_FILE_ELEVATE); };
}

void MainWindow::RebuildMenu() {
    if (!hwnd_) return;
    if (!settings_.moreDetails) {
        if (hMainMenu_) {
            SetMenu(hwnd_, nullptr);
            DestroyMenu(hMainMenu_);
            hMainMenu_ = nullptr;
        }
        return;
    }

    const Strings& s = GetStrings(settings_.langMode);
    HMENU hRoot = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_NEW_TASK, s.menuRunNewTask);
    if (!IsCurrentProcessElevated()) {
        AppendMenuW(hFile, MF_STRING, IDM_FILE_ELEVATE, s.menuElevate);
    }
    AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, s.menuExit);
    AppendMenuW(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hFile), s.menuFile);

    HMENU hOpt = CreatePopupMenu();
    AppendMenuW(hOpt, MF_STRING, IDM_OPT_ALWAYS_ON_TOP, s.menuAlwaysOnTop);
    AppendMenuW(hOpt, MF_STRING, IDM_OPT_MINIMIZE_ON_USE, s.menuMinimizeOnUse);
    AppendMenuW(hOpt, MF_STRING, IDM_OPT_HIDE_WHEN_MINIMIZED, s.menuHideWhenMinimized);
    AppendMenuW(hOpt, MF_STRING, IDM_OPT_SHOW_FULL_ACCOUNT, s.menuShowFullAccount);
    AppendMenuW(hOpt, MF_STRING, IDM_OPT_SHOW_ALL_HISTORY, s.menuShowAllHistory);
    AppendMenuW(hOpt, MF_SEPARATOR, 0, nullptr);

    HMENU hTheme = CreatePopupMenu();
    AppendMenuW(hTheme, MF_STRING, IDM_OPT_THEME_SYSTEM, s.menuThemeSystem);
    AppendMenuW(hTheme, MF_STRING, IDM_OPT_THEME_LIGHT, s.menuThemeLight);
    AppendMenuW(hTheme, MF_STRING, IDM_OPT_THEME_DARK, s.menuThemeDark);
    AppendMenuW(hTheme, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hTheme, MF_STRING, IDM_OPT_TOGGLE_THEME, s.menuToggleTheme);
    AppendMenuW(hOpt, MF_POPUP, reinterpret_cast<UINT_PTR>(hTheme), s.menuTheme);

    HMENU hLang = CreatePopupMenu();
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_AUTO, s.menuLangAuto);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_EN, s.menuLangEn);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_ZH, s.menuLangZh);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_ZHTW, s.menuLangZhTw);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_JA, s.menuLangJa);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_KO, s.menuLangKo);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_DE, s.menuLangDe);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_FR, s.menuLangFr);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_ES, s.menuLangEs);
    AppendMenuW(hLang, MF_STRING, IDM_OPT_LANG_RU, s.menuLangRu);
    AppendMenuW(hOpt, MF_POPUP, reinterpret_cast<UINT_PTR>(hLang), s.menuLanguage);

    AppendMenuW(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hOpt), s.menuOptions);

    HMENU hEnh = CreatePopupMenu();
    AppendMenuW(hEnh, MF_STRING, IDM_OPT_PRELAUNCH_SAMPLING, s.menuPreLaunchSampling);
    AppendMenuW(hEnh, MF_STRING, IDM_ENH_KEEP_EXPLORER, s.enhKeepExplorer);
    AppendMenuW(hEnh, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hEnh, MF_STRING, IDM_ENH_REPLACE_TASKMGR, s.enhReplaceTaskmgr);
    AppendMenuW(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hEnh), s.menuEnhance);

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, IDM_VIEW_REFRESH_NOW, s.menuRefreshNow);
    HMENU hSpeed = CreatePopupMenu();
    AppendMenuW(hSpeed, MF_STRING, IDM_VIEW_SPEED_HIGH, s.menuSpeedHigh);
    AppendMenuW(hSpeed, MF_STRING, IDM_VIEW_SPEED_NORMAL, s.menuSpeedNormal);
    AppendMenuW(hSpeed, MF_STRING, IDM_VIEW_SPEED_LOW, s.menuSpeedLow);
    AppendMenuW(hSpeed, MF_STRING, IDM_VIEW_SPEED_PAUSED, s.menuSpeedPaused);
    AppendMenuW(hView, MF_POPUP, reinterpret_cast<UINT_PTR>(hSpeed), s.menuUpdateSpeed);
    AppendMenuW(hView, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_GROUP_BY_TYPE, s.menuGroupByType);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_EXPAND_ALL, s.menuExpandAll);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_COLLAPSE_ALL, s.menuCollapseAll);
    AppendMenuW(hView, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_SHOW_KERNEL_TIMES, s.menuShowKernelTimes);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_SUMMARY_VIEW, s.menuSummaryView);
    AppendMenuW(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hView), s.menuView);

    AppendMenuW(hRoot, MF_STRING, IDM_HELP_ABOUT, s.menuAbout);
    if (IfeoManager::IsOurIfeoEnabled()) {
        AppendMenuW(hRoot, MF_STRING, IDM_ENH_RESTORE_TASKMGR, s.menuRestoreTaskmgr);
    }

    HMENU old = hMainMenu_;
    hMainMenu_ = hRoot;
    SetMenu(hwnd_, hMainMenu_);
    if (old) DestroyMenu(old);
    UpdateMenuChecks();
    DrawMenuBar(hwnd_);
}

void MainWindow::UpdateMenuChecks() {
    if (!hMainMenu_) return;
    auto checkItem = [&](UINT id, bool checked) {
        CheckMenuItem(hMainMenu_, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    };
    auto enableItem = [&](UINT id, bool enabled) {
        EnableMenuItem(hMainMenu_, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };

    checkItem(IDM_OPT_ALWAYS_ON_TOP, settings_.alwaysOnTop);
    checkItem(IDM_OPT_MINIMIZE_ON_USE, settings_.minimizeOnUse);
    checkItem(IDM_OPT_HIDE_WHEN_MINIMIZED, settings_.hideWhenMinimized);
    checkItem(IDM_OPT_SHOW_FULL_ACCOUNT, settings_.showFullAccountName);
    checkItem(IDM_OPT_SHOW_ALL_HISTORY, settings_.showAllHistory);
    checkItem(IDM_OPT_PRELAUNCH_SAMPLING, settings_.preLaunchSampling);
    checkItem(IDM_ENH_KEEP_EXPLORER, settings_.keepExplorerAlive);
    checkItem(IDM_ENH_REPLACE_TASKMGR, IfeoManager::IsOurIfeoEnabled());

    checkItem(IDM_OPT_THEME_SYSTEM, settings_.themeMode == ThemeMode::System);
    checkItem(IDM_OPT_THEME_LIGHT, settings_.themeMode == ThemeMode::Light);
    checkItem(IDM_OPT_THEME_DARK, settings_.themeMode == ThemeMode::Dark);

    checkItem(IDM_OPT_LANG_AUTO, settings_.langMode == LangMode::Auto);
    checkItem(IDM_OPT_LANG_EN, settings_.langMode == LangMode::English);
    checkItem(IDM_OPT_LANG_ZH, settings_.langMode == LangMode::Chinese);
    checkItem(IDM_OPT_LANG_ZHTW, settings_.langMode == LangMode::TraditionalChinese);
    checkItem(IDM_OPT_LANG_JA, settings_.langMode == LangMode::Japanese);
    checkItem(IDM_OPT_LANG_KO, settings_.langMode == LangMode::Korean);
    checkItem(IDM_OPT_LANG_DE, settings_.langMode == LangMode::German);
    checkItem(IDM_OPT_LANG_FR, settings_.langMode == LangMode::French);
    checkItem(IDM_OPT_LANG_ES, settings_.langMode == LangMode::Spanish);
    checkItem(IDM_OPT_LANG_RU, settings_.langMode == LangMode::Russian);

    checkItem(IDM_VIEW_SPEED_HIGH, settings_.updateSpeed == UpdateSpeed::High);
    checkItem(IDM_VIEW_SPEED_NORMAL, settings_.updateSpeed == UpdateSpeed::Normal);
    checkItem(IDM_VIEW_SPEED_LOW, settings_.updateSpeed == UpdateSpeed::Low);
    checkItem(IDM_VIEW_SPEED_PAUSED, settings_.updateSpeed == UpdateSpeed::Paused);

    checkItem(IDM_VIEW_GROUP_BY_TYPE, settings_.groupByType);
    checkItem(IDM_VIEW_SHOW_KERNEL_TIMES, settings_.showKernelTimes);
    checkItem(IDM_VIEW_SUMMARY_VIEW, settings_.summaryView);

    bool isProc = (settings_.activeTab == 0);
    bool isPerf = (settings_.activeTab == 1);
    bool isUsers = (settings_.activeTab == 4);
    enableItem(IDM_VIEW_GROUP_BY_TYPE, isProc);
    enableItem(IDM_VIEW_EXPAND_ALL, isProc || isUsers);
    enableItem(IDM_VIEW_COLLAPSE_ALL, isProc || isUsers);
    enableItem(IDM_VIEW_SHOW_KERNEL_TIMES, isPerf);
    enableItem(IDM_VIEW_SUMMARY_VIEW, isPerf);
}

void MainWindow::LayoutChildren() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    if (settings_.moreDetails && settings_.activeTab == 1 && IsInPerfSummary()) {
        RECT sumRc{};
        GetClientRect(hSummaryWnd_ ? hSummaryWnd_ : hwnd_, &sumRc);
        tabStripRect_ = { 0, 0, 0, 0 };
        contentRect_ = { 0, 0, sumRc.right - sumRc.left, sumRc.bottom - sumRc.top };
        footerRect_ = { 0, contentRect_.bottom, contentRect_.right, contentRect_.bottom };
        if (hBtnMore_ && IsWindowVisible(hBtnMore_)) ShowWindow(hBtnMore_, SW_HIDE);
        if (hBtnAction_ && IsWindowVisible(hBtnAction_)) ShowWindow(hBtnAction_, SW_HIDE);
        if (hSearchEdit_ && IsWindowVisible(hSearchEdit_)) ShowWindow(hSearchEdit_, SW_HIDE);
        tabPerf_.Resize(contentRect_);
        return;
    }
    if (hBtnMore_ && !IsWindowVisible(hBtnMore_)) ShowWindow(hBtnMore_, SW_SHOW);

    int footerH = DpiScale(hwnd_, 46);
    int tabH = settings_.moreDetails ? DpiScale(hwnd_, 32) : 0;

    tabStripRect_ = { 0, 0, w, tabH };
    contentRect_ = { 0, tabH, w, std::max(tabH, h - footerH) };
    footerRect_ = { 0, std::max(tabH, h - footerH), w, h };

    // Calculate individual tab rects + theme pill on right of tab strip
    if (settings_.moreDetails) {
        const Strings& s = GetStrings(settings_.langMode);
        auto& tm = ThemeManager::Instance();
        HDC hdc = GetDC(hwnd_);

        const wchar_t* themeTxt = tm.IsDark() ? s.themeDarkLabel : s.themeLightLabel;
        int pillW = tm.MeasureTextWidth(hdc, hwnd_, 10, false, themeTxt) + DpiScale(hwnd_, 32);
        int pillPad = DpiScale(hwnd_, 8);
        int pillV = DpiScale(hwnd_, 3);
        themeBtnRect_ = { std::max(0, w - pillPad - pillW), pillV, std::max(0, w - pillPad), tabH - pillV };

        const wchar_t* tabLabels[kTabCount] = {
            s.tabProcesses, s.tabPerformance, s.tabAppHistory,
            s.tabStartup, s.tabUsers, s.tabDetails, s.tabServices, s.tabLocks
        };
        int curX = DpiScale(hwnd_, 4);
        int maxTabsRight = themeBtnRect_.left - DpiScale(hwnd_, 6);
        int padH = DpiScale(hwnd_, 13);
        for (int i = 0; i < kTabCount; ++i) {
            int tw = tm.MeasureTextWidth(hdc, hwnd_, 10, false, tabLabels[i]) + padH * 2;
            int tTop = (i == settings_.activeTab) ? DpiScale(hwnd_, 2) : DpiScale(hwnd_, 4);
            tabRects_[i] = { curX, tTop, std::min(maxTabsRight, curX + tw), tabH };
            curX += tw + DpiScale(hwnd_, 2);
        }
        ReleaseDC(hwnd_, hdc);
    }

    // Resize active tab / simple list
    if (settings_.moreDetails) {
        if (hListSimple_) ShowWindow(hListSimple_, SW_HIDE);
        tabProcesses_.Resize(contentRect_);
        tabPerf_.Resize(contentRect_);
        tabAppHistory_.Resize(contentRect_);
        tabStartup_.Resize(contentRect_);
        tabUsers_.Resize(contentRect_);
        tabDetails_.Resize(contentRect_);
        tabServices_.Resize(contentRect_);
        tabLocks_.Resize(contentRect_);
    } else {
        if (hListSimple_) {
            MoveWindow(hListSimple_, contentRect_.left, contentRect_.top,
                       contentRect_.right - contentRect_.left,
                       contentRect_.bottom - contentRect_.top, TRUE);
            ListView_SetColumnWidth(hListSimple_, 0, std::max(100, static_cast<int>(contentRect_.right - contentRect_.left - DpiScale(hwnd_, 22))));
            ShowWindow(hListSimple_, simpleRows_.empty() ? SW_HIDE : SW_SHOW);
        }
    }

    UpdatePrimaryActionButton();
    LayoutFooterButtons();
}

bool MainWindow::IsSearchTabActive() const {
    return settings_.moreDetails && !IsInPerfSummary() &&
           (settings_.activeTab == 0 || settings_.activeTab == 5 || settings_.activeTab == 6);
}

void MainWindow::SyncSearchBoxState() {
    if (!hSearchEdit_) return;
    if (!IsSearchTabActive()) {
        searchFrameRect_ = {};
        searchClearRect_ = {};
        hoverSearchClear_ = false;
        if (IsWindowVisible(hSearchEdit_)) ShowWindow(hSearchEdit_, SW_HIDE);
        return;
    }

    const Strings& s = GetStrings(settings_.langMode);
    const wchar_t* cue = s.searchCueProcesses;
    const std::wstring* activeFilter = nullptr;
    if (settings_.activeTab == 0) {
        cue = s.searchCueProcesses;
        activeFilter = &tabProcesses_.Filter();
    } else if (settings_.activeTab == 5) {
        cue = s.searchCueDetails;
        activeFilter = &tabDetails_.Filter();
    } else if (settings_.activeTab == 6) {
        cue = s.searchCueServices;
        activeFilter = &tabServices_.Filter();
    }
    SendMessageW(hSearchEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));

    if (activeFilter) {
        wchar_t curBuf[256] = {};
        GetWindowTextW(hSearchEdit_, curBuf, ARRAYSIZE(curBuf));
        if (*activeFilter != curBuf) {
            suppressSearchSync_ = true;
            SetWindowTextW(hSearchEdit_, activeFilter->c_str());
            suppressSearchSync_ = false;
        }
    }
    LayoutFooterButtons();
    InvalidateRect(hwnd_, &footerRect_, FALSE);
}

void MainWindow::OnSearchTextChanged() {
    if (suppressSearchSync_ || !hSearchEdit_) return;
    int len = GetWindowTextLengthW(hSearchEdit_);
    std::wstring query(static_cast<size_t>(std::max(0, len)) + 1, L'\0');
    if (len > 0) {
        GetWindowTextW(hSearchEdit_, &query[0], len + 1);
        query.resize(static_cast<size_t>(len));
    } else {
        query.clear();
    }

    if (settings_.activeTab == 0) {
        tabProcesses_.SetFilter(query);
    } else if (settings_.activeTab == 5) {
        tabDetails_.SetFilter(query);
    } else if (settings_.activeTab == 6) {
        tabServices_.SetFilter(query);
    }
    UpdatePrimaryActionButton();
    if (searchFrameRect_.right > searchFrameRect_.left) {
        InvalidateRect(hwnd_, &searchFrameRect_, FALSE);
    }
}

int MainWindow::GetMinimumDetailedWidth() const {
    if (!hwnd_) return 760;
    int baseMin = DpiScale(hwnd_, 740);

    // Ensure 8 tabs + theme pill always fit comfortably without crowding
    const Strings& s = GetStrings(settings_.langMode);
    auto& tm = ThemeManager::Instance();
    int tabsMinClient = 0;
    if (HDC hdc = GetDC(hwnd_)) {
        const wchar_t* themeTxt = tm.IsDark() ? s.themeDarkLabel : s.themeLightLabel;
        int pillW = tm.MeasureTextWidth(hdc, hwnd_, 10, false, themeTxt) + DpiScale(hwnd_, 32);
        const wchar_t* tabLabels[kTabCount] = {
            s.tabProcesses, s.tabPerformance, s.tabAppHistory,
            s.tabStartup, s.tabUsers, s.tabDetails, s.tabServices, s.tabLocks
        };
        int padH = DpiScale(hwnd_, 13);
        int curX = DpiScale(hwnd_, 4);
        for (int i = 0; i < kTabCount; ++i) {
            curX += tm.MeasureTextWidth(hdc, hwnd_, 10, false, tabLabels[i]) + padH * 2 + DpiScale(hwnd_, 2);
        }
        tabsMinClient = curX + DpiScale(hwnd_, 16) + pillW + DpiScale(hwnd_, 8);
        ReleaseDC(hwnd_, hdc);
    }

    // Account for window non-client border delta
    RECT wrc{}, crc{};
    GetWindowRect(hwnd_, &wrc);
    GetClientRect(hwnd_, &crc);
    int frameW = std::max<int>(DpiScale(hwnd_, 16), (wrc.right - wrc.left) - (crc.right - crc.left));

    int perfMinClient = tabPerf_.GetMinimumRequiredWidth();
    int neededClient = std::max(perfMinClient, tabsMinClient);
    int result = std::max(baseMin, neededClient + frameW);
    HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(hMon, &mi)) {
        int workCap = (mi.rcWork.right - mi.rcWork.left) - DpiScale(hwnd_, 16);
        if (workCap > DpiScale(hwnd_, 640)) {
            result = std::min(result, workCap);
        }
    }
    return result;
}

int MainWindow::GetMinimumDetailedHeight() const {
    if (!hwnd_) return 600;
    int baseMin = DpiScale(hwnd_, 600);
    int chromeH = DpiScale(hwnd_, 32) + DpiScale(hwnd_, 46); // tab strip + footer
    RECT wrc{}, crc{};
    GetWindowRect(hwnd_, &wrc);
    GetClientRect(hwnd_, &crc);
    int frameH = std::max<int>(DpiScale(hwnd_, 60), (wrc.bottom - wrc.top) - (crc.bottom - crc.top));
    int perfMinH = tabPerf_.GetMinimumRequiredHeight() + chromeH + frameH;
    int result = std::max(baseMin, perfMinH);
    HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(hMon, &mi)) {
        int workCap = (mi.rcWork.bottom - mi.rcWork.top) - DpiScale(hwnd_, 24);
        if (workCap > DpiScale(hwnd_, 480)) {
            result = std::min(result, workCap);
        }
    }
    return result;
}

void MainWindow::LayoutFooterButtons() {
    if (!hwnd_) return;
    int w = std::max(0, static_cast<int>(footerRect_.right - footerRect_.left));
    int footerH = std::max(1, static_cast<int>(footerRect_.bottom - footerRect_.top));
    int padX = DpiScale(hwnd_, 10);
    int btnH = DpiScale(hwnd_, 28);
    int btnY = footerRect_.top + (footerH - btnH) / 2;

    HDC hdc = GetDC(hwnd_);
    int moreTextW = hBtnMore_ ? MeasureButtonTextWidth(hdc, hwnd_, hBtnMore_) : DpiScale(hwnd_, 64);
    int actTextW = hBtnAction_ ? MeasureButtonTextWidth(hdc, hwnd_, hBtnAction_) : DpiScale(hwnd_, 64);
    ReleaseDC(hwnd_, hdc);

    int moreW = std::max(DpiScale(hwnd_, 84), DpiScale(hwnd_, 20) + moreTextW + DpiScale(hwnd_, 10));
    int actW = std::max(DpiScale(hwnd_, 74), actTextW + DpiScale(hwnd_, 22));
    int gap = DpiScale(hwnd_, 8);
    int actX = std::max(padX + moreW + gap, w - padX - actW);

    MoveButtonIfChanged(hwnd_, hBtnMore_, padX, btnY, moreW, btnH);
    MoveButtonIfChanged(hwnd_, hBtnAction_, actX, btnY, actW, btnH);

    if (IsSearchTabActive()) {
        int leftBound = padX + moreW + gap * 2;
        int rightBound = actX - gap * 2;
        int avail = rightBound - leftBound;
        if (avail >= DpiScale(hwnd_, 110)) {
            int boxW = std::clamp(avail, DpiScale(hwnd_, 140), DpiScale(hwnd_, 320));
            int boxX = leftBound + (avail - boxW) / 2;
            searchFrameRect_ = { boxX, btnY, boxX + boxW, btnY + btnH };
            int iconPadL = DpiScale(hwnd_, 24);
            int clearW = DpiScale(hwnd_, 22);
            searchClearRect_ = { searchFrameRect_.right - clearW, searchFrameRect_.top + 1,
                                 searchFrameRect_.right - 1, searchFrameRect_.bottom - 1 };
            if (hSearchEdit_) {
                int editH = std::max(DpiScale(hwnd_, 16), btnH - DpiScale(hwnd_, 8));
                int editY = btnY + (btnH - editH) / 2 + DpiScale(hwnd_, 1);
                MoveButtonIfChanged(hwnd_, hSearchEdit_,
                                    searchFrameRect_.left + iconPadL, editY,
                                    std::max(30, boxW - iconPadL - clearW), editH);
                if (!IsWindowVisible(hSearchEdit_)) ShowWindow(hSearchEdit_, SW_SHOW);
            }
        } else {
            searchFrameRect_ = {};
            searchClearRect_ = {};
            if (hSearchEdit_ && IsWindowVisible(hSearchEdit_)) ShowWindow(hSearchEdit_, SW_HIDE);
        }
    } else {
        searchFrameRect_ = {};
        searchClearRect_ = {};
        if (hSearchEdit_ && IsWindowVisible(hSearchEdit_)) ShowWindow(hSearchEdit_, SW_HIDE);
    }
}

void MainWindow::SwitchTab(int tabIdx) {
    tabIdx = std::clamp(tabIdx, 0, kTabCount - 1);
    if (tabIdx != 1 && IsInPerfSummary()) {
        SetPerfSummaryMode(PerfSummaryMode::Normal);
    }
    settings_.activeTab = tabIdx;

    bool detail = settings_.moreDetails;
    tabProcesses_.Show(detail && tabIdx == 0);
    tabPerf_.Show(detail && tabIdx == 1);
    tabAppHistory_.Show(detail && tabIdx == 2);
    tabStartup_.Show(detail && tabIdx == 3);
    tabUsers_.Show(detail && tabIdx == 4);
    tabDetails_.Show(detail && tabIdx == 5);
    tabServices_.Show(detail && tabIdx == 6);
    tabLocks_.Show(detail && tabIdx == 7);

    if (detail) {
        RefreshActiveView(true, 15000);
        HWND targetFocus = nullptr;
        switch (tabIdx) {
        case 0: targetFocus = tabProcesses_.Hwnd(); break;
        case 1: targetFocus = tabPerf_.Hwnd(); break;
        case 2: targetFocus = tabAppHistory_.Hwnd(); break;
        case 3: targetFocus = tabStartup_.Hwnd(); break;
        case 4: targetFocus = tabUsers_.Hwnd(); break;
        case 5: targetFocus = tabDetails_.Hwnd(); break;
        case 6: targetFocus = tabServices_.Hwnd(); break;
        case 7: targetFocus = tabLocks_.Hwnd(); break;
        }
        if (targetFocus && IsWindowVisible(targetFocus)) SetFocus(targetFocus);
    } else {
        RefreshActiveView(false);
        if (hListSimple_) ShowWindow(hListSimple_, simpleRows_.empty() ? SW_HIDE : SW_SHOW);
    }

    UpdateMenuChecks();
    LayoutChildren();
    SyncSearchBoxState();
    UpdatePrimaryActionButton();
    InvalidateRect(hwnd_, &tabStripRect_, FALSE);
    InvalidateRect(hwnd_, &footerRect_, FALSE);
    if (IsWindowVisible(hwnd_)) {
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
}

void MainWindow::ToggleMoreDetails() {
    if (!hwnd_) return;
    if (IsInPerfSummary()) {
        SetPerfSummaryMode(PerfSummaryMode::Normal);
    }
    RECT curRc{};
    GetWindowRect(hwnd_, &curRc);

    if (settings_.moreDetails) {
        // Save full-mode window rect before shrinking
        if (!IsZoomed(hwnd_) && !IsIconic(hwnd_)) {
            settings_.windowRect = curRc;
        }
        settings_.maximized = (IsZoomed(hwnd_) != FALSE);
        settings_.moreDetails = false;
        RebuildMenu();
        if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetWindowPos(hwnd_, nullptr, 0, 0, DpiScale(hwnd_, 480), DpiScale(hwnd_, 440),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        settings_.moreDetails = true;
        RebuildMenu();
        int minW = GetMinimumDetailedWidth();
        int minH = GetMinimumDetailedHeight();
        bool validSaved = (settings_.windowRect.left > -32000 && settings_.windowRect.top > -32000 &&
                           (settings_.windowRect.right - settings_.windowRect.left) >= DpiScale(hwnd_, 480) &&
                           (settings_.windowRect.bottom - settings_.windowRect.top) >= DpiScale(hwnd_, 340));
        int w = validSaved ? (settings_.windowRect.right - settings_.windowRect.left) : minW;
        int h = validSaved ? (settings_.windowRect.bottom - settings_.windowRect.top) : minH;
        SetWindowPos(hwnd_, nullptr, 0, 0, std::max<int>(w, minW), std::max<int>(h, minH),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SwitchTab(settings_.activeTab);
    RefreshAll(true);
}

void MainWindow::SetPerfSummaryMode(PerfSummaryMode mode) {
    if (!hwnd_) return;
    if (perfSummaryMode_ == mode) return;
    const PerfSummaryMode prevMode = perfSummaryMode_;
    perfSummaryMode_ = mode;

    const bool entering = (mode != PerfSummaryMode::Normal);
    RECT perfScreenRc{};
    if (tabPerf_.Hwnd()) {
        GetWindowRect(tabPerf_.Hwnd(), &perfScreenRc);
    } else {
        GetWindowRect(hwnd_, &perfScreenRc);
    }
    RECT graphRc = tabPerf_.GetRightGraphScreenRect();

    tabPerf_.SetSummaryMode(mode, false);
    settings_.summaryView = entering;

    if (entering && hSummaryWnd_) {
        if (!settings_.moreDetails) settings_.moreDetails = true;
        if (settings_.activeTab != 1) SwitchTab(1);

        if (prevMode == PerfSummaryMode::Normal) {
            savedPreSummaryMax_ = (IsZoomed(hwnd_) != FALSE);
            if (savedPreSummaryMax_) {
                WINDOWPLACEMENT wp{ sizeof(wp) };
                if (GetWindowPlacement(hwnd_, &wp)) {
                    savedPreSummaryRect_ = wp.rcNormalPosition;
                }
            } else {
                GetWindowRect(hwnd_, &savedPreSummaryRect_);
            }
        }

        int tx = perfScreenRc.left;
        int ty = perfScreenRc.top;
        int tw = tabPerf_.GetSidebarPreferredWidth();
        int th = std::clamp(tabPerf_.GetSidebarPreferredHeight(), DpiScale(hwnd_, 170), DpiScale(hwnd_, 680));
        if (mode == PerfSummaryMode::GraphOnly) {
            tx = graphRc.left;
            ty = graphRc.top;
            tw = std::max(DpiScale(hwnd_, 420), static_cast<int>(graphRc.right - graphRc.left));
            th = std::max(DpiScale(hwnd_, 260), static_cast<int>(graphRc.bottom - graphRc.top));
        }

        HMONITOR hMon = MonitorFromPoint(POINT{ tx, ty }, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            int workW = mi.rcWork.right - mi.rcWork.left;
            int workH = mi.rcWork.bottom - mi.rcWork.top;
            tw = std::min(tw, workW);
            th = std::min(th, workH);
            tx = std::clamp<int>(tx, mi.rcWork.left, mi.rcWork.right - tw);
            ty = std::clamp<int>(ty, mi.rcWork.top, mi.rcWork.bottom - th);
        }

        if (mode == PerfSummaryMode::SidebarOnly) {
            summaryFixedWidth_ = tw;
            summaryFixedHeight_ = th;
        } else {
            summaryFixedWidth_ = 0;
            summaryFixedHeight_ = 0;
        }

        SetParent(tabPerf_.Hwnd(), hSummaryWnd_);
        SetWindowPos(hSummaryWnd_, settings_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     tx, ty, tw, th, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RECT sRc{ 0, 0, tw, th };
        tabPerf_.Resize(sRc);
        tabPerf_.Show(true);
        ShowWindow(hwnd_, SW_HIDE);
        UpdateMenuChecks();
        SetForegroundWindow(hSummaryWnd_);
        SetFocus(tabPerf_.Hwnd());
        RedrawWindow(hSummaryWnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    } else {
        summaryFixedWidth_ = 0;
        summaryFixedHeight_ = 0;
        if (tabPerf_.Hwnd() && hSummaryWnd_ && GetParent(tabPerf_.Hwnd()) == hSummaryWnd_) {
            SetParent(tabPerf_.Hwnd(), hwnd_);
        }
        if (hSummaryWnd_) {
            ShowWindow(hSummaryWnd_, SW_HIDE);
        }
        ShowWindow(hwnd_, savedPreSummaryMax_ ? SW_SHOWMAXIMIZED : SW_SHOW);
        savedPreSummaryMax_ = false;
        SetForegroundWindow(hwnd_);
        ApplyCurrentTheme(true);
        LayoutChildren();
        UpdatePrimaryActionButton();
        UpdateMenuChecks();
        SetFocus(tabPerf_.Hwnd());
        RedrawWindow(hwnd_, nullptr, nullptr,
                     RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
}

void MainWindow::ApplyCurrentTheme(bool redrawTabs) {
    ThemeManager& tm = ThemeManager::Instance();
    tm.ApplyTheme(settings_.themeMode);
    tm.ApplyToWindow(hwnd_);
    UpdateWindowTitle();

    simpleLv_.ApplyTheme();
    tabProcesses_.ApplyTheme();
    tabPerf_.ApplyTheme();
    tabAppHistory_.ApplyTheme();
    tabStartup_.ApplyTheme();
    tabUsers_.ApplyTheme();
    tabDetails_.ApplyTheme();
    tabServices_.ApplyTheme();
    tabLocks_.ApplyTheme();

    if (hBtnMore_) InvalidateRect(hBtnMore_, nullptr, FALSE);
    if (hBtnAction_) InvalidateRect(hBtnAction_, nullptr, FALSE);
    if (hSearchEdit_) {
        SendMessageW(hSearchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(tm.GetFont(hwnd_, 10, false)), TRUE);
        tm.ApplyToControl(hSearchEdit_);
        InvalidateRect(hSearchEdit_, nullptr, TRUE);
    }
    if (redrawTabs) {
        LayoutChildren();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (IsInPerfSummary()) {
        if (redrawTabs && hSummaryWnd_ && IsWindowVisible(hSummaryWnd_)) {
            RedrawWindow(hSummaryWnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
        return;
    }
    DrawMenuBar(hwnd_);
    if (redrawTabs && IsWindowVisible(hwnd_)) {
        // Force Windows 10 DWM to repaint non-client caption bar immediately on theme switch
        // while keeping its real activation state and repainting the menu bar cleanly.
        bool wasActive = (GetForegroundWindow() == hwnd_);
        SendMessageW(hwnd_, WM_NCACTIVATE, wasActive ? FALSE : TRUE, 0);
        SendMessageW(hwnd_, WM_NCACTIVATE, wasActive ? TRUE : FALSE, 0);
        DrawMenuBar(hwnd_);
        RedrawWindow(hwnd_, nullptr, nullptr,
                     RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
}

void MainWindow::ToggleThemeQuick() {
    bool currentlyDark = ThemeManager::Instance().IsDark();
    settings_.themeMode = currentlyDark ? ThemeMode::Light : ThemeMode::Dark;
    ApplyCurrentTheme(true);
    UpdateMenuChecks();
}

void MainWindow::SetLanguageMode(LangMode mode) {
    settings_.langMode = mode;
    UpdateWindowTitle();
    RebuildMenu();
    tabProcesses_.RebuildColumns();
    tabAppHistory_.RebuildColumns();
    tabStartup_.RebuildColumns();
    tabUsers_.RebuildColumns();
    tabDetails_.RebuildColumns();
    tabServices_.RebuildColumns();
    tabLocks_.RebuildColumns();
    SyncSearchBoxState();
    if (settings_.moreDetails && !IsZoomed(hwnd_) && !IsInPerfSummary()) {
        RECT curWrc{};
        GetWindowRect(hwnd_, &curWrc);
        int minW = GetMinimumDetailedWidth();
        if ((curWrc.right - curWrc.left) < minW) {
            int nx = curWrc.left;
            int ny = curWrc.top;
            HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi)) {
                minW = std::min<int>(minW, mi.rcWork.right - mi.rcWork.left);
                if (nx + minW > mi.rcWork.right) {
                    nx = std::max<int>(mi.rcWork.left, mi.rcWork.right - minW);
                }
            }
            SetWindowPos(hwnd_, nullptr, nx, ny, minW, curWrc.bottom - curWrc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    LayoutChildren();
    RefreshAll(true);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::SetUpdateSpeed(UpdateSpeed speed) {
    settings_.updateSpeed = speed;
    perfMon_.SetUpdateSpeed(speed);
    if (timerRunning_) {
        KillTimer(hwnd_, kRefreshTimerId);
        timerRunning_ = false;
    }
    DWORD ms = GetUpdateIntervalMs(speed);
    if (ms > 0) {
        SetTimer(hwnd_, kRefreshTimerId, ms, nullptr);
        timerRunning_ = true;
    }
    UpdateMenuChecks();
    if (hwnd_ && settings_.moreDetails && settings_.activeTab == 1) {
        tabPerf_.UpdateData(perfMon_);
    }
    if (hwnd_) {
        UpdateTrayIcon(procMon_.Totals().cpuPercent);
    }
}

void MainWindow::UpdateWindowTitle() {
    if (!hwnd_) return;
    const Strings& s = GetStrings(settings_.langMode);
    const wchar_t* base = IsCurrentProcessElevated() ? s.appTitleAdmin : s.appTitle;
    bool dark = ThemeManager::Instance().IsDark();
    std::wstring fullTitle = std::wstring(base) + L"  (" + (dark ? s.themeDarkLabel : s.themeLightLabel) + L")";
    SetWindowTextW(hwnd_, fullTitle.c_str());
    if (hSummaryWnd_) SetWindowTextW(hSummaryWnd_, fullTitle.c_str());
}

void MainWindow::UpdateAlwaysOnTop() {
    if (!hwnd_) return;
    HWND zOrder = settings_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(hwnd_, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (hSummaryWnd_) {
        SetWindowPos(hSummaryWnd_, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    UpdateMenuChecks();
}

void MainWindow::SyncPreLaunchDaemon() {
    if (settings_.preLaunchSampling) {
        std::wstring daemonExe;
        auto ifeoSt = IfeoManager::QueryStatus();
        if (ifeoSt.isOurs && !ifeoSt.resolvedPath.empty() &&
            GetFileAttributesW(ifeoSt.resolvedPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            daemonExe = ifeoSt.resolvedPath;
        } else {
            daemonExe = IfeoManager::GetCurrentExePath();
        }
        if (!daemonExe.empty()) {
            std::wstring cmd = L"\"" + daemonExe + L"\" /daemon";
            HKEY hRun = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                              0, KEY_READ | KEY_WRITE, &hRun) == ERROR_SUCCESS) {
                RegSetValueExW(hRun, L"BetterWin10TaskmanDaemon", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(cmd.c_str()),
                               static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(hRun);
            }
            if (FindWindowExW(HWND_MESSAGE, nullptr, L"BetterWin10TaskmanDaemonWnd", nullptr) == nullptr) {
                STARTUPINFOW si{ sizeof(si) };
                PROCESS_INFORMATION pi{};
                std::wstring mutableCmd = cmd;
                if (CreateProcessW(daemonExe.c_str(), &mutableCmd[0], nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
        }
    } else {
        HKEY hRun = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_SET_VALUE, &hRun) == ERROR_SUCCESS) {
            RegDeleteValueW(hRun, L"BetterWin10TaskmanDaemon");
            RegCloseKey(hRun);
        }
        HWND hDaemon = FindWindowExW(HWND_MESSAGE, nullptr, L"BetterWin10TaskmanDaemonWnd", nullptr);
        if (hDaemon) {
            PostMessageW(hDaemon, WM_CLOSE, 0, 0);
        }
    }
}

void MainWindow::ToggleReplaceTaskManager() {
    IfeoStatus st = IfeoManager::QueryStatus();
    if (st.isOurs) {
        RestoreOriginalTaskManager();
        return;
    }

    PathCheckResult pathCheck = IfeoManager::InspectCurrentExePath();
    std::wstring chosenTarget;
    IfeoPathChoice choice = ShowIfeoPathChoiceDialog(hwnd_, settings_, pathCheck, chosenTarget);
    if (choice == IfeoPathChoice::Cancel || chosenTarget.empty()) {
        return;
    }

    const Strings& s = GetStrings(settings_.langMode);
    st = IfeoManager::QueryStatus();
    if (st.hasEntry && !st.isOurs) {
        const wchar_t* curDisplay = st.resolvedPath.empty() ? st.rawDebugger.c_str() : st.resolvedPath.c_str();
        wchar_t prompt[1024] = {};
        swprintf_s(prompt, s.ifeoConfirmOverwriteOther, curDisplay);
        if (MessageBoxW(hwnd_, prompt, s.ifeoChoiceTitle,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    DWORD err = ERROR_SUCCESS;
    if (!IfeoManager::InstallReversible(hwnd_, chosenTarget, &err)) {
        if (err != ERROR_CANCELLED) {
            wchar_t errBuf[256] = {};
            swprintf_s(errBuf, s.ifeoErrFailedFmt, err);
            MessageBoxW(hwnd_, errBuf, s.ifeoErrTitle, MB_OK | MB_ICONERROR);
        }
        return;
    }

    if (!settings_.preLaunchSamplingUserSet && !settings_.preLaunchSampling) {
        settings_.preLaunchSampling = true;
    }
    SyncPreLaunchDaemon();
    SaveSettings(settings_);
    RebuildMenu();

    ShowIfeoCountdownDialog(hwnd_, settings_, chosenTarget);
}

void MainWindow::RestoreOriginalTaskManager() {
    const Strings& s = GetStrings(settings_.langMode);
    if (MessageBoxW(hwnd_, s.ifeoConfirmRestore, s.ifeoErrTitle,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    IfeoStatus st = IfeoManager::QueryStatus();
    std::wstring targetToUnlock = !st.resolvedPath.empty()
        ? st.resolvedPath
        : IfeoManager::GetCurrentExePath();

    DWORD err = ERROR_SUCCESS;
    if (!IfeoManager::UninstallReversible(hwnd_, targetToUnlock, &err)) {
        if (err != ERROR_CANCELLED) {
            wchar_t errBuf[256] = {};
            swprintf_s(errBuf, s.ifeoErrFailedFmt, err);
            MessageBoxW(hwnd_, errBuf, s.ifeoErrTitle, MB_OK | MB_ICONERROR);
        }
        return;
    }

    // Turn off auto-sampling & cleanly shut down any background daemon so the unlocked binary can be deleted freely.
    settings_.preLaunchSampling = false;
    settings_.preLaunchSamplingUserSet = false;
    HWND hDaemon = FindWindowExW(HWND_MESSAGE, nullptr, L"BetterWin10TaskmanDaemonWnd", nullptr);
    if (hDaemon) {
        DWORD dPid = 0;
        GetWindowThreadProcessId(hDaemon, &dPid);
        HANDLE hProc = dPid ? OpenProcess(SYNCHRONIZE, FALSE, dPid) : nullptr;
        PostMessageW(hDaemon, WM_CLOSE, 0, 0);
        if (hProc) {
            WaitForSingleObject(hProc, 1500);
            CloseHandle(hProc);
        }
    }
    SyncPreLaunchDaemon();
    SaveSettings(settings_);

    RebuildMenu();

    if (GetEnvironmentVariableW(L"TASKMGR_ENH_IFEO_NO_EXPLORER", nullptr, 0) == 0) {
        OpenFileLocationInExplorer(targetToUnlock);
    }

    MessageBoxW(hwnd_, s.ifeoRestoreDoneBody, s.ifeoRestoreDoneTitle, MB_OK | MB_ICONINFORMATION);
}

void MainWindow::UpdatePrimaryActionButton() {
    if (!hBtnMore_ || !hBtnAction_) return;
    if (IsInPerfSummary()) {
        if (IsWindowVisible(hBtnMore_)) ShowWindow(hBtnMore_, SW_HIDE);
        if (IsWindowVisible(hBtnAction_)) ShowWindow(hBtnAction_, SW_HIDE);
        return;
    }
    const Strings& s = GetStrings(settings_.langMode);
    auto setTextIfChanged = [](HWND btn, const wchar_t* text) -> bool {
        wchar_t buf[128] = {};
        GetWindowTextW(btn, buf, 128);
        if (wcscmp(buf, text) != 0) {
            SetWindowTextW(btn, text);
            InvalidateRect(btn, nullptr, FALSE);
            return true;
        }
        return false;
    };
    bool textChanged = setTextIfChanged(hBtnMore_, settings_.moreDetails ? s.btnFewerDetails : s.btnMoreDetails);

    bool showBtn = true;
    bool enableBtn = false;
    const wchar_t* label = s.btnEndTask;

    if (!settings_.moreDetails) {
        int sel = ListView_GetNextItem(hListSimple_, -1, LVNI_SELECTED);
        enableBtn = (sel >= 0 && sel < static_cast<int>(simpleRows_.size()));
        bool isExp = enableBtn && IsExplorerProcess(L"", simpleRows_[static_cast<size_t>(sel)].imagePath);
        label = isExp ? s.ctxRestart : s.btnEndTask;
    } else {
        switch (settings_.activeTab) {
        case 0:
            enableBtn = tabProcesses_.HasSelection();
            label = (enableBtn && tabProcesses_.IsSelectedExplorer()) ? s.ctxRestart : s.btnEndTask;
            break;
        case 1:
            enableBtn = true;
            label = s.btnOpenResMon;
            break;
        case 2:
            showBtn = false;
            break;
        case 3:
            enableBtn = tabStartup_.HasSelection();
            label = (enableBtn && tabStartup_.SelectedIsEnabled()) ? s.btnDisable : s.btnEnable;
            break;
        case 4:
            enableBtn = tabUsers_.IsUserSelected();
            label = s.btnDisconnect;
            break;
        case 5:
            enableBtn = tabDetails_.HasSelection();
            label = (enableBtn && tabDetails_.IsSelectedExplorer()) ? s.ctxRestart : s.btnEndTask;
            break;
        case 6:
            enableBtn = true;
            label = s.btnOpenServices;
            break;
        case 7:
            enableBtn = tabLocks_.HasKillableSelection();
            label = s.btnEndTask;
            break;
        }
    }

    bool wasVisible = (IsWindowVisible(hBtnAction_) != FALSE);
    if (wasVisible != showBtn) {
        ShowWindow(hBtnAction_, showBtn ? SW_SHOW : SW_HIDE);
    }
    bool wasEnabled = (IsWindowEnabled(hBtnAction_) != FALSE);
    if (wasEnabled != enableBtn) {
        EnableWindow(hBtnAction_, enableBtn ? TRUE : FALSE);
        if (showBtn) InvalidateRect(hBtnAction_, nullptr, FALSE);
    }
    if (setTextIfChanged(hBtnAction_, label)) textChanged = true;
    if (textChanged) {
        LayoutFooterButtons();
        InvalidateRect(hwnd_, &footerRect_, FALSE);
    }
}

void MainWindow::SaveAllTabColumnWidths() {
    tabProcesses_.SaveColumnWidths();
    tabAppHistory_.SaveColumnWidths();
    tabStartup_.SaveColumnWidths();
    tabUsers_.SaveColumnWidths();
    tabDetails_.SaveColumnWidths();
    tabServices_.SaveColumnWidths();
    tabLocks_.SaveColumnWidths();
}

void MainWindow::RefreshAll(bool force) {
    if (!force && GetForegroundWindow() == hwnd_ && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        return; // Win10 behavior: holding Ctrl pauses updates while focused
    }

    ULONGLONG now = GetTickCount64();
    double dtSec = std::max(0.05, static_cast<double>(now - lastSampleTick_) / 1000.0);
    lastSampleTick_ = now;

    bool needCmd = (settings_.moreDetails && settings_.activeTab == 5 && tabDetails_.NeedsCommandLines());
    procMon_.Refresh(settings_.showFullAccountName, needCmd);

    DWORD totalThreads = 0;
    DWORD totalHandles = 0;
    for (const auto& p : procMon_.Processes()) {
        totalThreads += p.threadCount;
        totalHandles += p.handleCount;
    }

    perfMon_.Sample(static_cast<DWORD>(procMon_.Processes().size()), totalThreads, totalHandles);
    appHistory_.Accumulate(procMon_.Processes(), dtSec);
    UpdateTrayIcon(procMon_.Totals().cpuPercent);

    RefreshActiveView(true, force ? 0 : 4500);
    UpdatePrimaryActionButton();
}

void MainWindow::RefreshActiveView(bool allowManagerRefresh, ULONGLONG minStaleMs) {
    if (!settings_.moreDetails) {
        RefreshSimpleList();
        return;
    }
    ULONGLONG now = GetTickCount64();
    switch (settings_.activeTab) {
    case 0:
        tabProcesses_.RefreshData(procMon_);
        break;
    case 1:
        tabPerf_.UpdateData(perfMon_);
        break;
    case 2:
        tabAppHistory_.RefreshData(appHistory_);
        break;
    case 3:
        if (!startupInited_ || (allowManagerRefresh && (minStaleMs == 0 || now - lastStartupRefreshTick_ >= minStaleMs))) {
            startupMgr_.Refresh();
            lastStartupRefreshTick_ = now;
            startupInited_ = true;
        }
        tabStartup_.RefreshData(startupMgr_);
        break;
    case 4:
        userMgr_.Refresh(procMon_.Processes(), settings_.showFullAccountName);
        tabUsers_.RefreshData(userMgr_, perfMon_.Memory().totalBytes);
        break;
    case 5:
        tabDetails_.RefreshData(procMon_);
        break;
    case 6:
        if (!servicesInited_ || (allowManagerRefresh && (minStaleMs == 0 || now - lastServiceRefreshTick_ >= minStaleMs))) {
            serviceMgr_.Refresh();
            lastServiceRefreshTick_ = now;
            servicesInited_ = true;
        }
        tabServices_.RefreshData(serviceMgr_);
        break;
    }
}

void MainWindow::RefreshSimpleList() {
    if (!hListSimple_) return;
    DWORD selPid = 0;
    int sel = ListView_GetNextItem(hListSimple_, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < static_cast<int>(simpleRows_.size())) {
        selPid = simpleRows_[static_cast<size_t>(sel)].pid;
    }

    simpleRows_.clear();
    for (const auto& p : procMon_.Processes()) {
        if (p.category == ProcessCategory::App) {
            SimpleAppRow r;
            r.pid = p.pid;
            r.title = !p.windowTitle.empty() ? p.windowTitle : (!p.description.empty() ? p.description : p.name);
            r.imagePath = p.imagePath;
            r.icon = p.icon;
            simpleRows_.push_back(std::move(r));
        }
    }
    std::sort(simpleRows_.begin(), simpleRows_.end(), [](const SimpleAppRow& a, const SimpleAppRow& b) {
        return _wcsicmp(a.title.c_str(), b.title.c_str()) < 0;
    });

    ListView_SetItemCountEx(hListSimple_, static_cast<int>(simpleRows_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (!settings_.moreDetails) {
        ShowWindow(hListSimple_, simpleRows_.empty() ? SW_HIDE : SW_SHOW);
        if (simpleRows_.empty()) InvalidateRect(hwnd_, &contentRect_, FALSE);
    }

    if (selPid != 0) {
        for (int i = 0; i < static_cast<int>(simpleRows_.size()); ++i) {
            if (simpleRows_[static_cast<size_t>(i)].pid == selPid) {
                ListView_SetItemState(hListSimple_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }
    InvalidateRect(hListSimple_, nullptr, FALSE);
}

void MainWindow::UpdateTrayIcon(double cpuPct) {
    if (!hwnd_) return;
    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz <= 0) sz = 16;

    HDC screenDc = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = sz; // bottom-up DIB
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hColor = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    int rowBytes = ((sz + 15) / 16) * 2;
    std::vector<uint8_t> maskBits(static_cast<size_t>(rowBytes) * static_cast<size_t>(sz), 0);
    HBITMAP hMask = CreateBitmap(sz, sz, 1, 1, maskBits.data());

    if (hColor && bits) {
        uint32_t* px = static_cast<uint32_t*>(bits);
        std::fill_n(px, static_cast<size_t>(sz) * static_cast<size_t>(sz), 0x00000000u);

        int margin = (sz >= 20) ? 1 : 0;
        int boxMin = margin;
        int boxMax = sz - 1 - margin;
        int innerMin = margin + 1;
        int innerMax = sz - 2 - margin;
        int innerW = std::max(1, innerMax - innerMin + 1);
        int innerH = innerW;

        std::vector<double> colPct(static_cast<size_t>(innerW), std::clamp(cpuPct, 0.0, 100.0));
        const auto& hist = perfMon_.Cpu().history;
        if (!hist.empty() && innerW > 1) {
            size_t n = hist.size();
            for (int ix = 0; ix < innerW; ++ix) {
                size_t s0 = (static_cast<size_t>(ix) * n) / static_cast<size_t>(innerW);
                size_t s1 = (static_cast<size_t>(ix + 1) * n) / static_cast<size_t>(innerW);
                if (s1 <= s0) s1 = s0 + 1;
                if (s1 > n) s1 = n;
                double v = 0.0;
                for (size_t k = s0; k < s1; ++k) {
                    v = std::max(v, hist[k]);
                }
                colPct[static_cast<size_t>(ix)] = std::clamp(v, 0.0, 100.0);
            }
            colPct[static_cast<size_t>(innerW - 1)] = std::clamp(cpuPct, 0.0, 100.0);
        }

        auto colHeightPx = [&](int ix) -> double {
            double pct = colPct[static_cast<size_t>(ix)];
            double hv = (pct / 100.0) * static_cast<double>(innerH);
            if (pct > 0.5) hv = std::max(hv, 0.85);
            return std::clamp(hv, 0.0, static_cast<double>(innerH));
        };

        auto blendArgb = [](uint32_t bg, uint32_t fg, double alpha) -> uint32_t {
            alpha = std::clamp(alpha, 0.0, 1.0);
            if (alpha <= 0.0) return bg;
            if (alpha >= 1.0) return fg;
            double br = static_cast<double>((bg >> 16) & 0xFFu);
            double bgc = static_cast<double>((bg >> 8) & 0xFFu);
            double bb = static_cast<double>(bg & 0xFFu);
            double fr = static_cast<double>((fg >> 16) & 0xFFu);
            double fgc = static_cast<double>((fg >> 8) & 0xFFu);
            double fb = static_cast<double>(fg & 0xFFu);
            uint32_t r = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(std::lround(br + (fr - br) * alpha)), 0, 255));
            uint32_t g = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(std::lround(bgc + (fgc - bgc) * alpha)), 0, 255));
            uint32_t b = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(std::lround(bb + (fb - bb) * alpha)), 0, 255));
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        };

        constexpr uint32_t kBorderCol = 0xFF5A646Eu;
        constexpr uint32_t kBgCol     = 0xFF16222Cu;
        constexpr uint32_t kMidGrid   = 0xFF203342u;
        constexpr uint32_t kFillCol   = 0xFF56BEFDu; // Win10 tray sky-blue
        constexpr uint32_t kLineCol   = 0xFFBFE5FDu; // Win10 tray sky-blue highlight

        for (int y = boxMin; y <= boxMax; ++y) {
            for (int x = boxMin; x <= boxMax; ++x) {
                bool isEdge = (x == boxMin || y == boxMin || x == boxMax || y == boxMax);
                if (isEdge) {
                    px[y * sz + x] = kBorderCol;
                    continue;
                }
                int ix = x - innerMin;
                int iy = y - innerMin; // 0 = bottom, innerH - 1 = top
                double hVal = colHeightPx(ix);
                double prevH = (ix > 0) ? colHeightPx(ix - 1) : hVal;
                double nextH = (ix + 1 < innerW) ? colHeightPx(ix + 1) : hVal;

                uint32_t col = (iy == innerH / 2) ? kMidGrid : kBgCol;
                double fillCov = std::clamp(hVal - static_cast<double>(iy), 0.0, 1.0);
                col = blendArgb(col, kFillCol, fillCov);

                if (hVal > 0.05) {
                    double loMid = std::min(hVal, 0.5 * (hVal + std::min(prevH, nextH)));
                    double hiMid = std::max(hVal, 0.5 * (hVal + std::max(prevH, nextH)));
                    double bandLo = std::max(0.0, loMid - 1.0);
                    double bandHi = std::min(static_cast<double>(innerH), hiMid + 0.25);
                    double lineCov = std::clamp(
                        std::min(bandHi, static_cast<double>(iy) + 1.0) - std::max(bandLo, static_cast<double>(iy)),
                        0.0, 1.0);
                    col = blendArgb(col, kLineCol, lineCov);
                }
                px[y * sz + x] = col;
            }
        }
    }
    ReleaseDC(nullptr, screenDc);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hColor;
    ii.hbmMask = hMask;
    HICON hNew = CreateIconIndirect(&ii);
    if (hColor) DeleteObject(hColor);
    if (hMask) DeleteObject(hMask);

    if (hNew) {
        HICON hOld = hTrayIcon_;
        hTrayIcon_ = hNew;

        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid_.uCallbackMessage = WM_APP_TRAY;
        nid_.hIcon = hTrayIcon_;
        const Strings& s = GetStrings(settings_.langMode);
        swprintf_s(nid_.szTip, L"%s - CPU: %.0f%%", s.appTitle, cpuPct);

        if (!trayAdded_) {
            trayAdded_ = (Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE);
        } else {
            Shell_NotifyIconW(NIM_MODIFY, &nid_);
        }
        if (hOld) DestroyIcon(hOld);
    }
}

void MainWindow::DestroyTrayIcon() {
    if (trayAdded_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        trayAdded_ = false;
    }
    if (hTrayIcon_) {
        DestroyIcon(hTrayIcon_);
        hTrayIcon_ = nullptr;
    }
}

void MainWindow::ShowTrayContextMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    const Strings& s = GetStrings(settings_.langMode);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, s.trayRestore);
    AppendMenuW(hMenu, MF_STRING | (settings_.alwaysOnTop ? MF_CHECKED : 0), IDM_TRAY_ALWAYS_ON_TOP, s.menuAlwaysOnTop);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_TOGGLE_THEME, s.menuToggleTheme);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, s.menuExit);
    SetMenuDefaultItem(hMenu, IDM_TRAY_RESTORE, FALSE);

    HWND hOwner = (IsInPerfSummary() && hSummaryWnd_) ? hSummaryWnd_ : hwnd_;
    SetForegroundWindow(hOwner);
    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hOwner, nullptr);
    PostMessageW(hOwner, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    switch (cmd) {
    case IDM_TRAY_RESTORE:
        if (IsInPerfSummary() && hSummaryWnd_) {
            ShowWindow(hSummaryWnd_, SW_SHOW);
            SetForegroundWindow(hSummaryWnd_);
        } else {
            ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
        }
        break;
    case IDM_TRAY_ALWAYS_ON_TOP:
        settings_.alwaysOnTop = !settings_.alwaysOnTop;
        UpdateAlwaysOnTop();
        break;
    case IDM_TRAY_TOGGLE_THEME:
        ToggleThemeQuick();
        break;
    case IDM_TRAY_EXIT:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    }
}

void MainWindow::PaintClient(HDC hdc, const RECT& rc) {
    ThemeManager& tm = ThemeManager::Instance();
    const ThemePalette& pal = tm.Palette();
    const Strings& s = GetStrings(settings_.langMode);

    // Chrome fill for tabstrip + footer
    HBRUSH brChrome = CreateSolidBrush(pal.chromeBg);
    FillRect(hdc, &rc, brChrome);
    DeleteObject(brChrome);

    if (settings_.moreDetails) {
        DrawTabStrip(hdc, pal, s);
    } else if (simpleRows_.empty()) {
        HBRUSH brSurf = CreateSolidBrush(pal.surfaceBg);
        FillRect(hdc, &contentRect_, brSurf);
        DeleteObject(brSurf);

        SetBkMode(hdc, TRANSPARENT);
        tm.DrawModernText(hdc, hwnd_, 11, false, s.noAppsRunning, contentRect_,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, pal.textSecondary);
    }

    // Crisp 1px top border for footer bar
    HPEN pen = CreatePen(PS_SOLID, 1, pal.borderColor);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, 0, footerRect_.top, nullptr);
    LineTo(hdc, rc.right, footerRect_.top);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    // Search box chrome inside footer (Processes / Details / Services)
    if (IsSearchTabActive() && searchFrameRect_.right > searchFrameRect_.left) {
        HBRUSH eb = CreateSolidBrush(pal.editBg);
        FillRect(hdc, &searchFrameRect_, eb);
        DeleteObject(eb);

        COLORREF bdClr = (GetFocus() == hSearchEdit_) ? pal.accentColor : pal.editBorder;
        HPEN ep = CreatePen(PS_SOLID, 1, bdClr);
        HGDIOBJ op = SelectObject(hdc, ep);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, searchFrameRect_.left, searchFrameRect_.top,
                  searchFrameRect_.right, searchFrameRect_.bottom);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(ep);

        Gdiplus::Graphics gp(hdc);
        gp.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Color crIcon(255, GetRValue(pal.textSecondary), GetGValue(pal.textSecondary), GetBValue(pal.textSecondary));
        Gdiplus::Pen iconPen(crIcon, 1.3f);
        int cy = (searchFrameRect_.top + searchFrameRect_.bottom) / 2;
        int ix = searchFrameRect_.left + DpiScale(hwnd_, 8);
        int d = DpiScale(hwnd_, 8);
        gp.DrawEllipse(&iconPen, static_cast<float>(ix), static_cast<float>(cy - d / 2 - 1),
                       static_cast<float>(d), static_cast<float>(d));
        gp.DrawLine(&iconPen,
                    static_cast<float>(ix + d * 0.78), static_cast<float>(cy - d / 2 - 1 + d * 0.78),
                    static_cast<float>(ix + d + DpiScale(hwnd_, 3)), static_cast<float>(cy + d / 2 + DpiScale(hwnd_, 2)));

        if (hSearchEdit_ && GetWindowTextLengthW(hSearchEdit_) > 0 && searchClearRect_.right > searchClearRect_.left) {
            if (hoverSearchClear_) {
                HBRUSH hb = CreateSolidBrush(pal.btnHoverBg);
                FillRect(hdc, &searchClearRect_, hb);
                DeleteObject(hb);
            }
            COLORREF xClr = hoverSearchClear_ ? pal.textPrimary : pal.textSecondary;
            Gdiplus::Pen xPen(Gdiplus::Color(255, GetRValue(xClr), GetGValue(xClr), GetBValue(xClr)), 1.4f);
            int cx = (searchClearRect_.left + searchClearRect_.right) / 2;
            int r = DpiScale(hwnd_, 4);
            gp.DrawLine(&xPen, cx - r, cy - r, cx + r, cy + r);
            gp.DrawLine(&xPen, cx + r, cy - r, cx - r, cy + r);
        }
    }
}

void MainWindow::DrawTabStrip(HDC hdc, const ThemePalette& pal, const Strings& s) {
    int tabH = tabStripRect_.bottom;
    const wchar_t* tabLabels[kTabCount] = {
        s.tabProcesses, s.tabPerformance, s.tabAppHistory,
        s.tabStartup, s.tabUsers, s.tabDetails, s.tabServices, s.tabLocks
    };

    // Baseline separator under inactive tabs
    HPEN penBorder = CreatePen(PS_SOLID, 1, pal.borderColor);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, penBorder));
    MoveToEx(hdc, 0, tabH - 1, nullptr);
    LineTo(hdc, tabStripRect_.right, tabH - 1);

    SetBkMode(hdc, TRANSPARENT);
    auto& tm = ThemeManager::Instance();

    for (int i = 0; i < kTabCount; ++i) {
        const RECT& tr = tabRects_[i];
        if (tr.right <= tr.left) continue;
        bool active = (i == settings_.activeTab);
        bool hover = (i == hoveredTab_);

        COLORREF bg = active ? pal.tabActiveBg : (hover ? pal.tabHoverBg : pal.tabInactiveBg);
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(hdc, &tr, br);
        DeleteObject(br);

        if (active) {
            // Left, top, right borders for active tab (bottom open into content surface)
            SelectObject(hdc, penBorder);
            MoveToEx(hdc, tr.left, tr.bottom - 1, nullptr);
            LineTo(hdc, tr.left, tr.top);
            LineTo(hdc, tr.right - 1, tr.top);
            LineTo(hdc, tr.right - 1, tr.bottom);

            // Top thin Win10 accent line optional or clean surface junction
            RECT bottomCover{ tr.left + 1, tr.bottom - 1, tr.right - 1, tr.bottom };
            HBRUSH brCov = CreateSolidBrush(pal.tabActiveBg);
            FillRect(hdc, &bottomCover, brCov);
            DeleteObject(brCov);
        } else {
            // Top & side border hint for inactive tabs
            SelectObject(hdc, penBorder);
            MoveToEx(hdc, tr.left, tr.bottom - 1, nullptr);
            LineTo(hdc, tr.left, tr.top);
            LineTo(hdc, tr.right - 1, tr.top);
            LineTo(hdc, tr.right - 1, tr.bottom - 1);
        }

        RECT rcTxt = tr;
        tm.DrawModernText(hdc, hwnd_, 10, false, tabLabels[i], rcTxt,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                          active ? pal.textPrimary : pal.textSecondary);
    }

    // Draw Light/Dark quick toggle pill on the right of tab strip
    if (themeBtnRect_.right > themeBtnRect_.left + 20) {
        COLORREF pillBg = hoverThemeBtn_ ? pal.btnHoverBg : pal.btnBg;
        HBRUSH brPill = CreateSolidBrush(pillBg);
        FillRect(hdc, &themeBtnRect_, brPill);
        DeleteObject(brPill);

        SelectObject(hdc, penBorder);
        MoveToEx(hdc, themeBtnRect_.left, themeBtnRect_.top, nullptr);
        LineTo(hdc, themeBtnRect_.right - 1, themeBtnRect_.top);
        LineTo(hdc, themeBtnRect_.right - 1, themeBtnRect_.bottom - 1);
        LineTo(hdc, themeBtnRect_.left, themeBtnRect_.bottom - 1);
        LineTo(hdc, themeBtnRect_.left, themeBtnRect_.top);

        // Vector sun/moon icon via GDI+
        Gdiplus::Graphics gp(hdc);
        gp.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        int iconSz = DpiScale(hwnd_, 13);
        int ix = themeBtnRect_.left + DpiScale(hwnd_, 7);
        int iy = (themeBtnRect_.top + themeBtnRect_.bottom - iconSz) / 2;

        Gdiplus::Color crIcon(255, GetRValue(pal.textPrimary), GetGValue(pal.textPrimary), GetBValue(pal.textPrimary));
        if (pal.isDark) {
            // Draw crisp crescent moon (filled circle minus offset circle)
            Gdiplus::GraphicsPath path;
            path.AddEllipse(static_cast<float>(ix), static_cast<float>(iy), static_cast<float>(iconSz), static_cast<float>(iconSz));
            Gdiplus::Region rgn(&path);
            Gdiplus::GraphicsPath cut;
            cut.AddEllipse(static_cast<float>(ix) + iconSz * 0.35f, static_cast<float>(iy) - iconSz * 0.15f,
                           static_cast<float>(iconSz) * 0.9f, static_cast<float>(iconSz) * 0.9f);
            rgn.Exclude(&cut);
            Gdiplus::SolidBrush brMoon(Gdiplus::Color(255, 255, 210, 90));
            gp.FillRegion(&brMoon, &rgn);
        } else {
            // Draw crisp sun icon
            float cx = ix + iconSz * 0.5f;
            float cy = iy + iconSz * 0.5f;
            float rCore = iconSz * 0.27f;
            Gdiplus::SolidBrush brSun(Gdiplus::Color(255, 215, 130, 0));
            gp.FillEllipse(&brSun, cx - rCore, cy - rCore, rCore * 2.0f, rCore * 2.0f);
            Gdiplus::Pen penSun(Gdiplus::Color(255, 215, 130, 0), 1.3f);
            for (int a = 0; a < 8; ++a) {
                double ang = a * 3.14159265 / 4.0;
                float x1 = cx + static_cast<float>(cos(ang) * (iconSz * 0.36));
                float y1 = cy + static_cast<float>(sin(ang) * (iconSz * 0.36));
                float x2 = cx + static_cast<float>(cos(ang) * (iconSz * 0.50));
                float y2 = cy + static_cast<float>(sin(ang) * (iconSz * 0.50));
                gp.DrawLine(&penSun, x1, y1, x2, y2);
            }
        }

        const wchar_t* themeTxt = pal.isDark ? s.themeDarkLabel : s.themeLightLabel;
        RECT rcLbl{ ix + iconSz + DpiScale(hwnd_, 5), themeBtnRect_.top, themeBtnRect_.right - DpiScale(hwnd_, 6), themeBtnRect_.bottom };
        tm.DrawModernText(hdc, hwnd_, 10, false, themeTxt, rcLbl,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, pal.textPrimary);
    }

    SelectObject(hdc, oldPen);
    DeleteObject(penBorder);
}

void MainWindow::DrawOwnerButton(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    auto& tm = ThemeManager::Instance();
    const ThemePalette& pal = tm.Palette();
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool hover = (dis->CtlID == IDC_BTN_MORE_DETAILS) ? hoverBtnMore_ : hoverBtnAction_;

    COLORREF bg = disabled ? pal.btnDisabledBg : (pressed ? pal.btnPressedBg : (hover ? pal.btnHoverBg : pal.btnBg));
    COLORREF border = (dis->itemState & ODS_FOCUS) ? pal.accentColor : pal.btnBorder;

    HBRUSH br = CreateSolidBrush(bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    wchar_t text[128] = {};
    GetWindowTextW(dis->hwndItem, text, 128);

    SetBkMode(hdc, TRANSPARENT);
    COLORREF fg = disabled ? pal.textDisabled : pal.textPrimary;
    UINT prefixFlag = (dis->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0;

    if (dis->CtlID == IDC_BTN_MORE_DETAILS) {
        // Draw small up/down chevron on left with compact horizontal margins, then label
        int cy = (rc.top + rc.bottom) / 2;
        int cx = rc.left + DpiScale(hwnd_, 11);
        int aw = DpiScale(hwnd_, 4);
        int ah = DpiScale(hwnd_, 3);
        HPEN chevPen = CreatePen(PS_SOLID, 1, pal.textPrimary);
        HPEN op = static_cast<HPEN>(SelectObject(hdc, chevPen));
        if (settings_.moreDetails) {
            // Up chevron (Fewer details)
            MoveToEx(hdc, cx - aw, cy + ah / 2, nullptr);
            LineTo(hdc, cx, cy - ah);
            LineTo(hdc, cx + aw + 1, cy + ah / 2 + 1);
        } else {
            // Down chevron (More details)
            MoveToEx(hdc, cx - aw, cy - ah / 2, nullptr);
            LineTo(hdc, cx, cy + ah);
            LineTo(hdc, cx + aw + 1, cy - ah / 2 - 1);
        }
        SelectObject(hdc, op);
        DeleteObject(chevPen);

        RECT rcT{ rc.left + DpiScale(hwnd_, 20), rc.top, rc.right - DpiScale(hwnd_, 6), rc.bottom };
        tm.DrawModernText(hdc, hwnd_, 10, false, text, rcT,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | prefixFlag, fg);
    } else {
        tm.DrawModernText(hdc, hwnd_, 10, false, text, rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | prefixFlag, fg);
    }
}

void MainWindow::HandleTabStripClick(POINT pt) {
    if (!settings_.moreDetails) return;
    if (PtInRect(&themeBtnRect_, pt)) {
        ToggleThemeQuick();
        return;
    }
    for (int i = 0; i < kTabCount; ++i) {
        if (PtInRect(&tabRects_[i], pt)) {
            if (settings_.activeTab != i) SwitchTab(i);
            return;
        }
    }
}

void MainWindow::HandleTabStripMove(POINT pt) {
    if (!settings_.moreDetails) return;
    int newTab = -1;
    bool newThemeHover = PtInRect(&themeBtnRect_, pt) != FALSE;
    if (!newThemeHover) {
        for (int i = 0; i < kTabCount; ++i) {
            if (PtInRect(&tabRects_[i], pt)) {
                newTab = i;
                break;
            }
        }
    }
    if (newTab != hoveredTab_ || newThemeHover != hoverThemeBtn_) {
        hoveredTab_ = newTab;
        hoverThemeBtn_ = newThemeHover;
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd_, &tabStripRect_, FALSE);
    }
}

void MainWindow::HandleSimpleListContextMenu(POINT screenPt) {
    int sel = ListView_GetNextItem(hListSimple_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(simpleRows_.size())) return;
    screenPt = ResolveListContextMenuPoint(hListSimple_, sel, screenPt);
    const auto r = simpleRows_[static_cast<size_t>(sel)];
    const auto& s = GetStrings(settings_.langMode);

    const bool isExplorer = IsExplorerProcess(L"", r.imagePath);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_EXPAND_COLLAPSE, s.ctxSwitchTo);
    if (isExplorer) {
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_RESTART_EXPLORER, s.ctxRestart);
    } else {
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_END_TASK, s.ctxEndTask);
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_GO_TO_DETAILS, s.ctxGoToDetails);
    UINT pFlags = MF_STRING | (r.imagePath.empty() ? MF_GRAYED : 0);
    AppendMenuW(hMenu, pFlags, IDM_CTX_OPEN_LOCATION, s.ctxOpenLocation);
    AppendMenuW(hMenu, pFlags, IDM_CTX_PROPERTIES, s.ctxProperties);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);

    if (cmd == IDM_CTX_EXPAND_COLLAPSE) {
        SwitchToSimpleSelected();
    } else if (cmd == IDM_CTX_RESTART_EXPLORER) {
        ProcessMonitor::EndTask(r.pid);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        RefreshAll(true);
    } else if (cmd == IDM_CTX_END_TASK && !isExplorer) {
        ProcessMonitor::EndTask(r.pid);
        RefreshAll(true);
    } else if (cmd == IDM_CTX_GO_TO_DETAILS) {
        bool wasSimple = !settings_.moreDetails;
        settings_.moreDetails = true;
        RebuildMenu();
        if (wasSimple) {
            SetWindowPos(hwnd_, nullptr, 0, 0, DpiScale(hwnd_, 1000), DpiScale(hwnd_, 680),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        NavigateToDetails(r.pid);
    } else if (cmd == IDM_CTX_OPEN_LOCATION && !r.imagePath.empty()) {
        OpenFileLocationInExplorer(r.imagePath);
    } else if (cmd == IDM_CTX_PROPERTIES && !r.imagePath.empty()) {
        ShowFilePropertiesDialog(hwnd_, r.imagePath);
    }
}

void MainWindow::SwitchToSimpleSelected() {
    int sel = ListView_GetNextItem(hListSimple_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(simpleRows_.size())) return;
    if (SwitchToProcessWindow(simpleRows_[static_cast<size_t>(sel)].pid)) {
        if (settings_.minimizeOnUse) ShowWindow(hwnd_, SW_MINIMIZE);
    }
}

void MainWindow::NavigateToDetails(DWORD pid) {
    SwitchTab(5);
    RefreshAll(true);
    tabDetails_.SelectProcessByPid(pid);
    SyncSearchBoxState();
}

void MainWindow::HandleCommand(UINT id) {
    switch (id) {
    case IDC_BTN_MORE_DETAILS:
        ToggleMoreDetails();
        break;
    case IDC_BTN_PRIMARY_ACTION:
        if (!settings_.moreDetails) {
            int sel = ListView_GetNextItem(hListSimple_, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < static_cast<int>(simpleRows_.size())) {
                const auto& sr = simpleRows_[static_cast<size_t>(sel)];
                if (IsExplorerProcess(L"", sr.imagePath)) {
                    ProcessMonitor::EndTask(sr.pid);
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
                } else {
                    ProcessMonitor::EndTask(sr.pid);
                }
                if (settings_.minimizeOnUse) ShowWindow(hwnd_, SW_MINIMIZE);
                RefreshAll(true);
            }
        } else {
            switch (settings_.activeTab) {
            case 0:
                if (tabProcesses_.IsSelectedExplorer()) tabProcesses_.RestartSelectedExplorer();
                else tabProcesses_.EndSelectedTask();
                RefreshAll(true);
                break;
            case 1: ShellExecuteW(hwnd_, L"open", L"resmon.exe", nullptr, nullptr, SW_SHOWNORMAL); break;
            case 3: tabStartup_.ToggleSelected(); break;
            case 4: tabUsers_.DisconnectSelected(); break;
            case 5:
                if (tabDetails_.IsSelectedExplorer()) tabDetails_.RestartSelectedExplorer();
                else tabDetails_.EndSelectedTask();
                break;
            case 6: tabServices_.OpenServicesMsc(); break;
            case 7: tabLocks_.EndSelectedLockingProcess(); break;
            }
        }
        break;

    // File
    case IDM_FILE_NEW_TASK:
        ShowRunNewTaskDialog(hwnd_, settings_);
        break;
    case IDM_FILE_ELEVATE: {
        HANDLE hElevateReady = CreateEventW(nullptr, TRUE, FALSE, L"Local\\BetterWin10TaskmanElevateReady");
        if (hElevateReady) ResetEvent(hElevateReady);
        if (RestartCurrentProcessAsAdmin(hwnd_)) {
            if (hElevateReady) {
                // Hold the pure-memory shared ring open until the elevated process maps it.
                WaitForSingleObject(hElevateReady, 3000);
            }
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        if (hElevateReady) CloseHandle(hElevateReady);
        break;
    }
    case IDM_FILE_EXIT:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;

    // Options
    case IDM_OPT_ALWAYS_ON_TOP:
        settings_.alwaysOnTop = !settings_.alwaysOnTop;
        UpdateAlwaysOnTop();
        break;
    case IDM_OPT_MINIMIZE_ON_USE:
        settings_.minimizeOnUse = !settings_.minimizeOnUse;
        UpdateMenuChecks();
        break;
    case IDM_OPT_HIDE_WHEN_MINIMIZED:
        settings_.hideWhenMinimized = !settings_.hideWhenMinimized;
        UpdateMenuChecks();
        break;
    case IDM_OPT_SHOW_FULL_ACCOUNT:
        settings_.showFullAccountName = !settings_.showFullAccountName;
        UpdateMenuChecks();
        RefreshAll(true);
        break;
    case IDM_OPT_SHOW_ALL_HISTORY:
        settings_.showAllHistory = !settings_.showAllHistory;
        UpdateMenuChecks();
        if (settings_.activeTab == 2) tabAppHistory_.RefreshData(appHistory_);
        break;
    case IDM_OPT_PRELAUNCH_SAMPLING:
        settings_.preLaunchSampling = !settings_.preLaunchSampling;
        settings_.preLaunchSamplingUserSet = true;
        SyncPreLaunchDaemon();
        SaveSettings(settings_);
        UpdateMenuChecks();
        break;
    case IDM_ENH_KEEP_EXPLORER:
        settings_.keepExplorerAlive = !settings_.keepExplorerAlive;
        SaveSettings(settings_);
        UpdateMenuChecks();
        if (settings_.keepExplorerAlive) {
            IfeoManager::EnsureExplorerAliveAsync();
        }
        break;
    case IDM_ENH_REPLACE_TASKMGR:
        ToggleReplaceTaskManager();
        break;
    case IDM_ENH_RESTORE_TASKMGR:
        RestoreOriginalTaskManager();
        break;

    case IDM_OPT_THEME_SYSTEM:
        settings_.themeMode = ThemeMode::System;
        ApplyCurrentTheme(true);
        UpdateMenuChecks();
        break;
    case IDM_OPT_THEME_LIGHT:
        settings_.themeMode = ThemeMode::Light;
        ApplyCurrentTheme(true);
        UpdateMenuChecks();
        break;
    case IDM_OPT_THEME_DARK:
        settings_.themeMode = ThemeMode::Dark;
        ApplyCurrentTheme(true);
        UpdateMenuChecks();
        break;
    case IDM_OPT_TOGGLE_THEME:
        ToggleThemeQuick();
        break;

    case IDM_OPT_LANG_AUTO: SetLanguageMode(LangMode::Auto); break;
    case IDM_OPT_LANG_EN:   SetLanguageMode(LangMode::English); break;
    case IDM_OPT_LANG_ZH:   SetLanguageMode(LangMode::Chinese); break;
    case IDM_OPT_LANG_ZHTW: SetLanguageMode(LangMode::TraditionalChinese); break;
    case IDM_OPT_LANG_JA:   SetLanguageMode(LangMode::Japanese); break;
    case IDM_OPT_LANG_KO:   SetLanguageMode(LangMode::Korean); break;
    case IDM_OPT_LANG_DE:   SetLanguageMode(LangMode::German); break;
    case IDM_OPT_LANG_FR:   SetLanguageMode(LangMode::French); break;
    case IDM_OPT_LANG_ES:   SetLanguageMode(LangMode::Spanish); break;
    case IDM_OPT_LANG_RU:   SetLanguageMode(LangMode::Russian); break;

    // View
    case IDM_VIEW_REFRESH_NOW:
        RefreshAll(true);
        break;
    case IDM_VIEW_SPEED_HIGH:   SetUpdateSpeed(UpdateSpeed::High); break;
    case IDM_VIEW_SPEED_NORMAL: SetUpdateSpeed(UpdateSpeed::Normal); break;
    case IDM_VIEW_SPEED_LOW:    SetUpdateSpeed(UpdateSpeed::Low); break;
    case IDM_VIEW_SPEED_PAUSED: SetUpdateSpeed(UpdateSpeed::Paused); break;

    case IDM_VIEW_GROUP_BY_TYPE:
        settings_.groupByType = !settings_.groupByType;
        UpdateMenuChecks();
        if (settings_.activeTab == 0) tabProcesses_.RefreshData(procMon_);
        break;
    case IDM_VIEW_EXPAND_ALL:
        if (settings_.activeTab == 0) tabProcesses_.ExpandAll(true);
        else if (settings_.activeTab == 4) tabUsers_.ExpandAll(true);
        break;
    case IDM_VIEW_COLLAPSE_ALL:
        if (settings_.activeTab == 0) tabProcesses_.ExpandAll(false);
        else if (settings_.activeTab == 4) tabUsers_.ExpandAll(false);
        break;
    case IDM_VIEW_SHOW_KERNEL_TIMES:
        settings_.showKernelTimes = !settings_.showKernelTimes;
        UpdateMenuChecks();
        if (settings_.activeTab == 1) InvalidateRect(tabPerf_.Hwnd(), nullptr, FALSE);
        break;
    case IDM_VIEW_SUMMARY_VIEW:
        if (settings_.moreDetails && settings_.activeTab == 1) {
            SetPerfSummaryMode(IsInPerfSummary() ? PerfSummaryMode::Normal : PerfSummaryMode::SidebarOnly);
        }
        break;

    // Help
    case IDM_HELP_TOPICS:
        ShellExecuteW(hwnd_, L"open",
                      L"https://support.microsoft.com/windows/task-manager", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case IDM_HELP_ABOUT:
        ShowAboutDialog(hwnd_, settings_);
        break;

    // Accelerators
    case IDA_TAB_NEXT:
        if (settings_.moreDetails) SwitchTab((settings_.activeTab + 1) % kTabCount);
        break;
    case IDA_TAB_PREV:
        if (settings_.moreDetails) SwitchTab((settings_.activeTab + kTabCount - 1) % kTabCount);
        break;
    case IDA_DELETE_ACTION:
        if (!settings_.moreDetails) {
            int sel = ListView_GetNextItem(hListSimple_, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < static_cast<int>(simpleRows_.size()) &&
                !IsExplorerProcess(L"", simpleRows_[static_cast<size_t>(sel)].imagePath)) {
                HandleCommand(IDC_BTN_PRIMARY_ACTION);
            }
        } else if (settings_.activeTab == 0) {
            tabProcesses_.EndSelectedTask();
            RefreshAll(true);
        } else if (settings_.activeTab == 5) {
            tabDetails_.EndSelectedTask();
        }
        break;
    case IDA_END_PROCESS_TREE:
        if (settings_.moreDetails && settings_.activeTab == 5) {
            tabDetails_.EndSelectedProcessTree();
        }
        break;
    case IDA_PROPERTIES:
        if (settings_.moreDetails) {
            if (settings_.activeTab == 0) tabProcesses_.OpenSelectedProperties();
            else if (settings_.activeTab == 3) tabStartup_.OpenSelectedProperties();
            else if (settings_.activeTab == 4) tabUsers_.OpenSelectedProperties();
            else if (settings_.activeTab == 5) tabDetails_.OpenSelectedProperties();
        }
        break;
    case IDA_ALT_E:
        if (!settings_.moreDetails) HandleCommand(IDC_BTN_PRIMARY_ACTION);
        else if (settings_.activeTab == 0 || settings_.activeTab == 5 || settings_.activeTab == 7) HandleCommand(IDC_BTN_PRIMARY_ACTION);
        else if (settings_.activeTab == 3 && tabStartup_.HasSelection()) tabStartup_.SetSelectedEnabled(true);
        break;
    case IDA_ALT_D:
        if (!settings_.moreDetails) {
            ToggleMoreDetails();
        } else if (settings_.activeTab == 3 && tabStartup_.HasSelection() && tabStartup_.SelectedIsEnabled()) {
            tabStartup_.SetSelectedEnabled(false);
        } else if (settings_.activeTab == 4 && tabUsers_.IsUserSelected()) {
            tabUsers_.DisconnectSelected();
        } else {
            ToggleMoreDetails();
        }
        break;
    case IDA_ALT_S:
        if (settings_.moreDetails && settings_.activeTab == 6) tabServices_.StartSelected();
        break;
    case IDA_ALT_T:
        if (settings_.moreDetails && settings_.activeTab == 6) tabServices_.StopSelected();
        break;
    case IDA_ALT_O:
        if (settings_.moreDetails && settings_.activeTab == 6) tabServices_.OpenServicesMsc();
        break;
    case IDA_ESCAPE:
        if (settings_.moreDetails && settings_.activeTab == 1 && IsInPerfSummary()) {
            SetPerfSummaryMode(PerfSummaryMode::Normal);
        }
        break;
    case IDA_FIND:
        if (IsSearchTabActive() && hSearchEdit_) {
            SetFocus(hSearchEdit_);
            SendMessageW(hSearchEdit_, EM_SETSEL, 0, -1);
        }
        break;
    }
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) {
        LRESULT res = self->HandleMessage(msg, wParam, lParam);
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return res;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::SummaryWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED && self->IsInPerfSummary()) {
                RECT rc{ 0, 0, LOWORD(lParam), HIWORD(lParam) };
                self->tabPerf_.Resize(rc);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (self->perfSummaryMode_ == PerfSummaryMode::SidebarOnly &&
                self->summaryFixedWidth_ > 0 && self->summaryFixedHeight_ > 0) {
                mmi->ptMinTrackSize.x = self->summaryFixedWidth_;
                mmi->ptMaxTrackSize.x = self->summaryFixedWidth_;
                mmi->ptMinTrackSize.y = self->summaryFixedHeight_;
                mmi->ptMaxTrackSize.y = self->summaryFixedHeight_;
            } else {
                mmi->ptMinTrackSize.x = DpiScale(hwnd, 220);
                mmi->ptMinTrackSize.y = DpiScale(hwnd, 170);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                self->SetPerfSummaryMode(PerfSummaryMode::Normal);
                return 0;
            }
            break;
        case WM_COMMAND:
            self->HandleCommand(LOWORD(wParam));
            return 0;
        case WM_CLOSE:
            if (self->hwnd_) PostMessageW(self->hwnd_, WM_CLOSE, 0, 0);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    LRESULT menuRes = 0;
    if (ThemeManager::Instance().HandleMenuBarMessage(hwnd_, msg, wParam, lParam, menuRes)) {
        return menuRes;
    }

    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        trayAdded_ = false;
        UpdateTrayIcon(procMon_.Totals().cpuPercent);
        return 0;
    }

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (settings_.hideWhenMinimized) {
                ShowWindow(hwnd_, SW_HIDE);
            }
            return 0;
        }
        LayoutChildren();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (settings_.moreDetails) {
            mmi->ptMinTrackSize.x = GetMinimumDetailedWidth();
            mmi->ptMinTrackSize.y = GetMinimumDetailedHeight();
        } else {
            mmi->ptMinTrackSize.x = DpiScale(hwnd_, 340);
            mmi->ptMinTrackSize.y = DpiScale(hwnd_, 260);
        }
        return 0;
    }

    case WM_NCHITTEST: {
        LRESULT ht = DefWindowProcW(hwnd_, msg, wParam, lParam);
        if (!settings_.moreDetails || IsInPerfSummary()) return ht;
        POINT screenPt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        if (ht == HTMENU && hMainMenu_) {
            int cnt = GetMenuItemCount(hMainMenu_);
            if (cnt > 0 && MenuItemFromPoint(hwnd_, hMainMenu_, screenPt) == -1) {
                RECT rcLast{};
                if (GetMenuItemRect(hwnd_, hMainMenu_, static_cast<UINT>(cnt - 1), &rcLast) &&
                    screenPt.x >= rcLast.right) {
                    return HTCAPTION;
                }
            }
        } else if (ht == HTCLIENT) {
            POINT cliPt = screenPt;
            ScreenToClient(hwnd_, &cliPt);
            if (PtInRect(&tabStripRect_, cliPt)) {
                bool onInteractive = (PtInRect(&themeBtnRect_, cliPt) != FALSE);
                if (!onInteractive) {
                    for (int i = 0; i < kTabCount; ++i) {
                        if (PtInRect(&tabRects_[i], cliPt)) {
                            onInteractive = true;
                            break;
                        }
                    }
                }
                if (!onInteractive) {
                    return HTCAPTION;
                }
            }
        }
        return ht;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldB = static_cast<HBITMAP>(SelectObject(mem, bmp));
            PaintClient(mem, rc);
            BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldB);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = reinterpret_cast<HDC>(wParam);
        const auto& pal = ThemeManager::Instance().Palette();
        SetTextColor(hdcEdit, pal.textPrimary);
        SetBkColor(hdcEdit, pal.editBg);
        return reinterpret_cast<LRESULT>(ThemeManager::Instance().EditBrush());
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (dis && dis->CtlType == ODT_BUTTON) {
            DrawOwnerButton(dis);
            return TRUE;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        HandleTabStripMove(pt);
        bool newHoverClear = (IsSearchTabActive() && hSearchEdit_ && GetWindowTextLengthW(hSearchEdit_) > 0 &&
                              PtInRect(&searchClearRect_, pt) != FALSE);
        if (newHoverClear != hoverSearchClear_) {
            hoverSearchClear_ = newHoverClear;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd_, &searchClearRect_, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (hoveredTab_ != -1 || hoverThemeBtn_ || hoverSearchClear_) {
            hoveredTab_ = -1;
            hoverThemeBtn_ = false;
            hoverSearchClear_ = false;
            InvalidateRect(hwnd_, &tabStripRect_, FALSE);
            if (searchClearRect_.right > searchClearRect_.left) InvalidateRect(hwnd_, &searchClearRect_, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        if (IsSearchTabActive() && hSearchEdit_ && GetWindowTextLengthW(hSearchEdit_) > 0 &&
            PtInRect(&searchClearRect_, pt)) {
            SetWindowTextW(hSearchEdit_, L"");
            SetFocus(hSearchEdit_);
            return 0;
        }
        HandleTabStripClick(pt);
        return 0;
    }

    case WM_TIMER:
        if (wParam == kWarmupTimerId) {
            KillTimer(hwnd_, kWarmupTimerId);
            RefreshAll(true);
            return 0;
        }
        if (wParam == kPerfSampleTimerId) {
            if (settings_.updateSpeed != UpdateSpeed::High &&
                settings_.updateSpeed != UpdateSpeed::Paused) {
                DWORD totalThreads = 0;
                DWORD totalHandles = 0;
                for (const auto& p : procMon_.Processes()) {
                    totalThreads += p.threadCount;
                    totalHandles += p.handleCount;
                }
                perfMon_.Sample(static_cast<DWORD>(procMon_.Processes().size()), totalThreads, totalHandles);
                UpdateTrayIcon(perfMon_.Cpu().utilization);
            }
            return 0;
        }
        if (wParam == kRefreshTimerId) {
            RefreshAll(false);
            return 0;
        }
        break;

    case WM_INITMENUPOPUP:
        UpdateMenuChecks();
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == IDC_EDIT_FOOTER_SEARCH) {
            OnSearchTextChanged();
            return 0;
        }
        HandleCommand(LOWORD(wParam));
        return 0;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lParam);
        if (!nm) break;
        if (nm->idFrom == IDC_LIST_SIMPLE) {
            if (nm->code == NM_CUSTOMDRAW) return simpleLv_.HandleCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
            if (nm->code == LVN_ITEMCHANGED) { UpdatePrimaryActionButton(); return 0; }
            if (nm->code == NM_DBLCLK) { SwitchToSimpleSelected(); return 0; }
            if (nm->code == NM_RCLICK) {
                POINT pt = HandleListRightClickSelection(hListSimple_, nm, static_cast<int>(simpleRows_.size()));
                HandleSimpleListContextMenu(pt);
                return 0;
            }
            return 0;
        }
        LRESULT res = 0;
        if (nm->idFrom == IDC_LIST_PROCESSES || nm->hwndFrom == ListView_GetHeader(tabProcesses_.Hwnd())) {
            res = tabProcesses_.HandleNotify(nm);
            if (nm->code == LVN_ITEMCHANGED) UpdatePrimaryActionButton();
            return res;
        }
        if (nm->idFrom == IDC_LIST_APP_HISTORY || nm->hwndFrom == ListView_GetHeader(tabAppHistory_.Hwnd())) {
            return tabAppHistory_.HandleNotify(nm);
        }
        if (nm->idFrom == IDC_LIST_STARTUP || nm->hwndFrom == ListView_GetHeader(tabStartup_.Hwnd())) {
            res = tabStartup_.HandleNotify(nm);
            if (nm->code == LVN_ITEMCHANGED) UpdatePrimaryActionButton();
            return res;
        }
        if (nm->idFrom == IDC_LIST_USERS || nm->hwndFrom == ListView_GetHeader(tabUsers_.Hwnd())) {
            res = tabUsers_.HandleNotify(nm);
            if (nm->code == LVN_ITEMCHANGED) UpdatePrimaryActionButton();
            return res;
        }
        if (nm->idFrom == IDC_LIST_DETAILS || nm->hwndFrom == ListView_GetHeader(tabDetails_.Hwnd())) {
            res = tabDetails_.HandleNotify(nm);
            if (nm->code == LVN_ITEMCHANGED) UpdatePrimaryActionButton();
            return res;
        }
        if (nm->idFrom == IDC_LIST_SERVICES || nm->hwndFrom == ListView_GetHeader(tabServices_.Hwnd())) {
            return tabServices_.HandleNotify(nm);
        }
        break;
    }

    case WM_CONTEXTMENU: {
        int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        if (x == -1 && y == -1) {
            HWND target = wParam ? reinterpret_cast<HWND>(wParam) : GetFocus();
            auto dispatchTabCtx = [&](int tab) {
                switch (tab) {
                case 0: tabProcesses_.HandleContextMenu({ -1, -1 }); break;
                case 1: tabPerf_.HandleContextMenu({ -1, -1 }); break;
                case 2: tabAppHistory_.HandleContextMenu({ -1, -1 }); break;
                case 3: tabStartup_.HandleContextMenu({ -1, -1 }); break;
                case 4: tabUsers_.HandleContextMenu({ -1, -1 }); break;
                case 5: tabDetails_.HandleContextMenu({ -1, -1 }); break;
                case 6: tabServices_.HandleContextMenu({ -1, -1 }); break;
                }
            };
            if (target == hListSimple_ || (!settings_.moreDetails && target == hwnd_)) {
                HandleSimpleListContextMenu({ -1, -1 });
                return 0;
            }
            if (target == tabProcesses_.Hwnd()) { dispatchTabCtx(0); return 0; }
            if (target == tabPerf_.Hwnd()) { dispatchTabCtx(1); return 0; }
            if (target == tabAppHistory_.Hwnd()) { dispatchTabCtx(2); return 0; }
            if (target == tabStartup_.Hwnd()) { dispatchTabCtx(3); return 0; }
            if (target == tabUsers_.Hwnd()) { dispatchTabCtx(4); return 0; }
            if (target == tabDetails_.Hwnd()) { dispatchTabCtx(5); return 0; }
            if (target == tabServices_.Hwnd()) { dispatchTabCtx(6); return 0; }
            if (target == hwnd_ && settings_.moreDetails) {
                dispatchTabCtx(settings_.activeTab);
                return 0;
            }
        }
        break;
    }

    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            if (settings_.themeMode == ThemeMode::System) {
                ApplyCurrentTheme(true);
            }
        }
        break;

    case WM_DPICHANGED: {
        auto* prc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyCurrentTheme(true);
        tabProcesses_.RebuildColumns();
        tabAppHistory_.RebuildColumns();
        tabStartup_.RebuildColumns();
        tabUsers_.RebuildColumns();
        tabDetails_.RebuildColumns();
        tabServices_.RebuildColumns();
        tabLocks_.RebuildColumns();
        LayoutChildren();
        return 0;
    }

    case WM_APP_TRAY: {
        UINT mouseMsg = LOWORD(lParam);
        if (mouseMsg == WM_LBUTTONUP || mouseMsg == WM_LBUTTONDBLCLK) {
            if (IsInPerfSummary() && hSummaryWnd_) {
                ShowWindow(hSummaryWnd_, SW_SHOW);
                SetForegroundWindow(hSummaryWnd_);
            } else {
                ShowWindow(hwnd_, SW_RESTORE);
                SetForegroundWindow(hwnd_);
            }
        } else if (mouseMsg == WM_RBUTTONUP || mouseMsg == WM_CONTEXTMENU) {
            ShowTrayContextMenu();
        }
        return 0;
    }

    case WM_APP_BRING_TO_FRONT: {
        int reqTab = static_cast<int>(wParam);
        if (IsInPerfSummary()) {
            if (reqTab >= 0 && reqTab < kTabCount && reqTab != 1) {
                SetPerfSummaryMode(PerfSummaryMode::Normal);
            } else if (hSummaryWnd_) {
                ShowWindow(hSummaryWnd_, SW_SHOW);
                SetForegroundWindow(hSummaryWnd_);
                return 0;
            }
        }
        if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOW);
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        if (reqTab >= 0 && reqTab < kTabCount) {
            if (!settings_.moreDetails) ToggleMoreDetails();
            SwitchTab(reqTab);
        }
        return 0;
    }

    case WM_APP_NAV_TO_DETAILS:
        NavigateToDetails(static_cast<DWORD>(wParam));
        return 0;

    case WM_APP_HISTORY_READY:
        appHistory_.Accumulate(procMon_.Processes(), 0.0);
        if (settings_.moreDetails && settings_.activeTab == 2) {
            tabAppHistory_.RefreshData(appHistory_);
        }
        return 0;

    case WM_ENDSESSION:
        if (wParam) {
            if (settings_.moreDetails) {
                if (IsInPerfSummary()) {
                    settings_.maximized = savedPreSummaryMax_;
                    settings_.windowRect = savedPreSummaryRect_;
                } else {
                    settings_.maximized = (IsZoomed(hwnd_) != FALSE);
                    if (!settings_.maximized && !IsIconic(hwnd_)) {
                        GetWindowRect(hwnd_, &settings_.windowRect);
                    }
                }
            }
            settings_.summaryView = false;
            SaveAllTabColumnWidths();
            SaveSettings(settings_);
        }
        return 0;

    case WM_CLOSE:
        KillTimer(hwnd_, kWarmupTimerId);
        KillTimer(hwnd_, kPerfSampleTimerId);
        if (timerRunning_) {
            KillTimer(hwnd_, kRefreshTimerId);
            timerRunning_ = false;
        }
        if (settings_.moreDetails) {
            if (IsInPerfSummary()) {
                settings_.maximized = savedPreSummaryMax_;
                settings_.windowRect = savedPreSummaryRect_;
            } else {
                settings_.maximized = (IsZoomed(hwnd_) != FALSE);
                if (!settings_.maximized && !IsIconic(hwnd_)) {
                    GetWindowRect(hwnd_, &settings_.windowRect);
                }
            }
        }
        settings_.summaryView = false;
        SaveAllTabColumnWidths();
        SaveSettings(settings_);
        if (!settings_.preLaunchSampling) {
            SyncPreLaunchDaemon();
        }
        if (tabPerf_.Hwnd() && hSummaryWnd_ && GetParent(tabPerf_.Hwnd()) == hSummaryWnd_) {
            SetParent(tabPerf_.Hwnd(), hwnd_);
        }
        if (hSummaryWnd_) {
            DestroyWindow(hSummaryWnd_);
            hSummaryWnd_ = nullptr;
        }
        DestroyTrayIcon();
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        simpleLv_.Detach();
        tabProcesses_.Destroy();
        tabPerf_.Destroy();
        tabAppHistory_.Destroy();
        tabStartup_.Destroy();
        tabUsers_.Destroy();
        tabDetails_.Destroy();
        tabServices_.Destroy();
        tabLocks_.Destroy();
        appHistory_.Shutdown();
        perfMon_.Shutdown();
        procMon_.Stop();
        if (hMainMenu_) { DestroyMenu(hMainMenu_); hMainMenu_ = nullptr; }
        ThemeManager::Instance().Shutdown();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace taskman
