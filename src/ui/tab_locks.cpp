#include "tab_locks.h"
#include "../resource.h"
#include "../core/theme.h"
#include "../core/i18n.h"
#include "../core/format_util.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cwctype>

namespace taskman {

namespace {

constexpr wchar_t kLocksWndClass[] = L"BetterWin10TaskmanLocksView";
constexpr UINT_PTR kEditSubclassId = 401;
constexpr UINT_PTR kBtnHoverIdBase = 410;

bool IsSamePathNoCase(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring FormatBytesSimple(uint64_t bytes) {
    if (bytes < 1024ULL) return std::to_wstring(bytes) + L" B";
    wchar_t buf[64] = {};
    if (bytes < 1024ULL * 1024ULL) {
        swprintf_s(buf, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

void CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (text.empty()) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* ptr = GlobalLock(hMem);
        if (ptr) {
            memcpy(ptr, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

} // namespace

TabLocks::~TabLocks() {
    Destroy();
}

LRESULT CALLBACK TabLocks::EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<TabLocks*>(dwRefData);
    if (self) {
        if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
            int len = GetWindowTextLengthW(hwnd);
            std::wstring text(static_cast<size_t>(std::max(0, len)) + 1, L'\0');
            if (len > 0) {
                GetWindowTextW(hwnd, &text[0], len + 1);
                text.resize(static_cast<size_t>(len));
            } else {
                text.clear();
            }
            self->StartScan(text);
            return 0;
        }
        if (msg == WM_DROPFILES) {
            return self->HandleMessage(msg, wParam, lParam);
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TabLocks::BtnHoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    auto* hovered = reinterpret_cast<bool*>(dwRefData);
    if (hovered) {
        if (msg == WM_MOUSEMOVE && !*hovered) {
            *hovered = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        } else if (msg == WM_MOUSELEAVE) {
            *hovered = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void TabLocks::Create(HWND parent, AppSettings* settings, ProcessMonitor* procMon) {
    hParent_ = parent;
    settings_ = settings;
    procMon_ = procMon;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kLocksWndClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_ACCEPTFILES, kLocksWndClass, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 100, 100,
        parent, nullptr, hInst, this);
    if (!hwnd_) return;

    DragAcceptFiles(hwnd_, TRUE);
    // Allow non-elevated Explorer to drag-and-drop even if Task Manager is elevated
    ChangeWindowMessageFilterEx(hwnd_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);

    hEditPath_ = CreateWindowExW(
        WS_EX_ACCEPTFILES, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 100, 24,
        hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LOCKS_PATH_EDIT)), hInst, nullptr);
    DragAcceptFiles(hEditPath_, TRUE);
    ChangeWindowMessageFilterEx(hEditPath_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hEditPath_, 0x0049, MSGFLT_ALLOW, nullptr);
    SetWindowSubclass(hEditPath_, EditSubclassProc, kEditSubclassId, reinterpret_cast<DWORD_PTR>(this));

    const Strings& s = GetStrings(settings_ ? settings_->langMode : LangMode::Auto);
    auto makeBtn = [&](UINT id, const wchar_t* text, bool& hoverState, UINT_PTR subId) -> HWND {
        HWND b = CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            0, 0, 80, 26,
            hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), hInst, nullptr);
        SetWindowSubclass(b, BtnHoverProc, subId, reinterpret_cast<DWORD_PTR>(&hoverState));
        return b;
    };

    hBtnFile_ = makeBtn(IDC_LOCKS_BTN_FILE, s.lockBrowseFile, hoverFile_, kBtnHoverIdBase + 1);
    hBtnFolder_ = makeBtn(IDC_LOCKS_BTN_FOLDER, s.lockBrowseFolder, hoverFolder_, kBtnHoverIdBase + 2);
    hBtnScan_ = makeBtn(IDC_LOCKS_BTN_SCAN, s.lockRescan, hoverScan_, kBtnHoverIdBase + 3);
    hBtnElevate_ = makeBtn(IDC_LOCKS_BTN_ELEVATE, s.lockElevateBtn, hoverElevate_, kBtnHoverIdBase + 4);
    if (IsCurrentProcessElevated() && hBtnElevate_) {
        ShowWindow(hBtnElevate_, SW_HIDE);
    }

    hList_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100,
        hwnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LIST_LOCKS)), hInst, nullptr);
    themedLv_.Attach(hList_, false);

    ListViewThemeCallbacks cb;
    cb.getCellText = [this](int row, int col) -> std::wstring {
        return GetCellText(row, col);
    };
    cb.getRowIcon = [this](int row) -> HICON {
        if (row >= 0 && row < static_cast<int>(result_.entries.size())) {
            return result_.entries[static_cast<size_t>(row)].icon;
        }
        return nullptr;
    };
    cb.getRowIndent = [](int) { return 1; };
    cb.getRowExpandState = [](int) { return -1; };
    cb.isCategoryHeader = [](int) { return false; };
    cb.getCellTextColor = [this](int row, int col, bool& overrideColor) -> COLORREF {
        overrideColor = false;
        if (col != 2 || row < 0 || row >= static_cast<int>(result_.entries.size())) return 0;
        const bool dark = ThemeManager::Instance().IsDark();
        const auto lvl = result_.entries[static_cast<size_t>(row)].criticalLevel;
        overrideColor = true;
        if (lvl == ProcessCriticalLevel::CriticalCore) {
            return dark ? RGB(242, 102, 102) : RGB(196, 43, 28);
        }
        if (lvl == ProcessCriticalLevel::SystemService) {
            return dark ? RGB(245, 178, 66) : RGB(178, 102, 0);
        }
        return dark ? RGB(108, 212, 134) : RGB(16, 124, 16);
    };
    cb.onHeaderTrack = [this]() {
        SaveColumnWidths();
        if (onColumnsChanged) onColumnsChanged();
    };
    themedLv_.SetCallbacks(std::move(cb));

    RefreshDrivePills();
    RebuildColumns();
    ApplyTheme();
}

void TabLocks::Destroy() {
    ++scanGen_;
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    scanning_.store(false, std::memory_order_release);

    if (hEditPath_) {
        RemoveWindowSubclass(hEditPath_, EditSubclassProc, kEditSubclassId);
        hEditPath_ = nullptr;
    }
    if (hBtnFile_) { RemoveWindowSubclass(hBtnFile_, BtnHoverProc, kBtnHoverIdBase + 1); hBtnFile_ = nullptr; }
    if (hBtnFolder_) { RemoveWindowSubclass(hBtnFolder_, BtnHoverProc, kBtnHoverIdBase + 2); hBtnFolder_ = nullptr; }
    if (hBtnScan_) { RemoveWindowSubclass(hBtnScan_, BtnHoverProc, kBtnHoverIdBase + 3); hBtnScan_ = nullptr; }
    if (hBtnElevate_) { RemoveWindowSubclass(hBtnElevate_, BtnHoverProc, kBtnHoverIdBase + 4); hBtnElevate_ = nullptr; }

    themedLv_.Detach();
    if (hList_) {
        DestroyWindow(hList_);
        hList_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TabLocks::Resize(const RECT& rc) {
    if (!hwnd_) return;
    MoveWindow(hwnd_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    LayoutControls();
}

void TabLocks::Show(bool visible) {
    if (!hwnd_) return;
    if (visible) {
        RefreshDrivePills();
        LayoutControls();
    }
    ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

void TabLocks::RefreshDrivePills() {
    drivePills_.clear();
    for (const auto& dr : LockChecker::EnumerateDriveRoots()) {
        DrivePill p;
        p.driveRoot = dr;
        drivePills_.push_back(std::move(p));
    }
}

void TabLocks::LayoutControls() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = std::max(0, static_cast<int>(rc.right - rc.left));
    int h = std::max(0, static_cast<int>(rc.bottom - rc.top));
    int pad = DpiScale(hwnd_, 12);
    int gap = DpiScale(hwnd_, 10);

    int leftW = std::clamp(w * 38 / 100, DpiScale(hwnd_, 260), DpiScale(hwnd_, 375));
    if (w < DpiScale(hwnd_, 620)) {
        leftW = std::max(DpiScale(hwnd_, 210), w * 42 / 100);
    }

    leftPaneRc_ = { 0, 0, leftW, h };
    rightPaneRc_ = { leftW + 1, 0, w, h };

    // Left controls: Edit path box -> 3 action buttons -> drive pills -> drop zone
    int editH = DpiScale(hwnd_, 28);
    editFrameRc_ = { pad, pad, std::max(pad + 40, leftW - pad), pad + editH };
    if (hEditPath_) {
        int ePadX = DpiScale(hwnd_, 7);
        int ePadY = DpiScale(hwnd_, 4);
        MoveWindow(hEditPath_, editFrameRc_.left + ePadX, editFrameRc_.top + ePadY,
                   std::max(20, static_cast<int>(editFrameRc_.right - editFrameRc_.left) - ePadX * 2),
                   std::max(16, editH - ePadY * 2), TRUE);
    }

    int btnY = editFrameRc_.bottom + DpiScale(hwnd_, 8);
    int btnH = DpiScale(hwnd_, 27);
    int availBtnW = std::max(60, leftW - pad * 2);
    int btnGap = DpiScale(hwnd_, 6);
    int eachBtnW = std::max(40, (availBtnW - btnGap * 2) / 3);
    if (hBtnFile_) MoveWindow(hBtnFile_, pad, btnY, eachBtnW, btnH, TRUE);
    if (hBtnFolder_) MoveWindow(hBtnFolder_, pad + eachBtnW + btnGap, btnY, eachBtnW, btnH, TRUE);
    if (hBtnScan_) {
        int lastX = pad + (eachBtnW + btnGap) * 2;
        MoveWindow(hBtnScan_, lastX, btnY, std::max(40, leftW - pad - lastX), btnH, TRUE);
    }

    int pillsLabelY = btnY + btnH + DpiScale(hwnd_, 12);
    int pillStartY = pillsLabelY + DpiScale(hwnd_, 22);
    int pillH = DpiScale(hwnd_, 24);
    int pillW = DpiScale(hwnd_, 46);
    int pillGapX = DpiScale(hwnd_, 6);
    int pillGapY = DpiScale(hwnd_, 6);
    int px = pad;
    int py = pillStartY;
    for (auto& pill : drivePills_) {
        if (px + pillW > leftW - pad && px > pad) {
            px = pad;
            py += pillH + pillGapY;
        }
        pill.rc = { px, py, px + pillW, py + pillH };
        px += pillW + pillGapX;
    }
    int pillsBottom = drivePills_.empty() ? pillStartY : (py + pillH);

    int dropTop = pillsBottom + DpiScale(hwnd_, 12);
    dropZoneRc_ = { pad, dropTop, std::max(pad + 60, leftW - pad), std::max(dropTop + DpiScale(hwnd_, 80), h - pad) };

    // Right pane: diagnosis card on top, lock list below
    int rx = rightPaneRc_.left + pad;
    int rw = std::max(60, static_cast<int>(rightPaneRc_.right - rightPaneRc_.left) - pad * 2);
    int diagH = DpiScale(hwnd_, 132);
    diagCardRc_ = { rx, pad, rx + rw, pad + diagH };

    if (hBtnElevate_) {
        bool showElev = !IsCurrentProcessElevated() && !currentTarget_.empty();
        int elevW = DpiScale(hwnd_, 132);
        int elevH = DpiScale(hwnd_, 25);
        MoveWindow(hBtnElevate_, diagCardRc_.right - pad - elevW, diagCardRc_.bottom - DpiScale(hwnd_, 33),
                   elevW, elevH, TRUE);
        ShowWindow(hBtnElevate_, showElev ? SW_SHOW : SW_HIDE);
    }

    int listTop = diagCardRc_.bottom + gap;
    if (hList_) {
        MoveWindow(hList_, rx, listTop, rw, std::max(40, h - listTop - pad), TRUE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TabLocks::ApplyTheme() {
    auto& tm = ThemeManager::Instance();
    if (hwnd_) {
        tm.ApplyToControl(hwnd_);
    }
    if (hEditPath_) {
        SendMessageW(hEditPath_, WM_SETFONT, reinterpret_cast<WPARAM>(tm.GetFont(hwnd_, 10, false)), TRUE);
        tm.ApplyToControl(hEditPath_);
    }
    for (HWND b : { hBtnFile_, hBtnFolder_, hBtnScan_, hBtnElevate_ }) {
        if (b) InvalidateRect(b, nullptr, FALSE);
    }
    themedLv_.ApplyTheme();
    RebuildColumns();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void TabLocks::RebuildColumns() {
    if (!hList_) return;
    while (Header_GetItemCount(ListView_GetHeader(hList_)) > 0) {
        ListView_DeleteColumn(hList_, 0);
    }
    const Strings& s = GetStrings(settings_ ? settings_->langMode : LangMode::Auto);
    const wchar_t* titles[5] = {
        s.lockColProcess, s.lockColPid, s.lockColProtection, s.lockColMethod, s.lockColPath
    };
    const int defaults[5] = { 175, 66, 165, 128, 270 };
    std::vector<HeaderColumnMeta> metas;
    for (int i = 0; i < 5; ++i) {
        int baseW = settings_ ? settings_->locksColWidths[i] : defaults[i];
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = (i == 1) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.cx = DpiScale(hList_, baseW);
        col.pszText = const_cast<LPWSTR>(titles[i]);
        ListView_InsertColumn(hList_, i, &col);
        HeaderColumnMeta m;
        m.title = titles[i];
        m.rightAlign = (i == 1);
        metas.push_back(std::move(m));
    }
    themedLv_.SetColumnsMeta(std::move(metas));
    themedLv_.SetSortIndicator(sortCol_, sortAsc_);

    if (hBtnFile_) SetWindowTextW(hBtnFile_, s.lockBrowseFile);
    if (hBtnFolder_) SetWindowTextW(hBtnFolder_, s.lockBrowseFolder);
    if (hBtnScan_) SetWindowTextW(hBtnScan_, s.lockRescan);
    if (hBtnElevate_) SetWindowTextW(hBtnElevate_, s.lockElevateBtn);
}

void TabLocks::SaveColumnWidths() {
    if (!hList_ || !settings_) return;
    static const int kSlots[5] = { 0, 1, 2, 3, 4 };
    ThemedListView::SaveColumnsToArray(hList_, kSlots, 5, settings_->locksColWidths, 5);
}

std::wstring TabLocks::FormatSourceMask(uint32_t mask, int handleCount) const {
    const Strings& s = GetStrings(settings_ ? settings_->langMode : LangMode::Auto);
    std::wstring out;
    auto appendPart = [&](const std::wstring& part) {
        if (!out.empty()) out += L" + ";
        out += part;
    };
    if (mask & LOCK_SRC_HANDLE) {
        if (handleCount > 1) {
            appendPart(std::wstring(s.lockSrcHandle) + L" (" + std::to_wstring(handleCount) + L")");
        } else {
            appendPart(s.lockSrcHandle);
        }
    }
    if (mask & LOCK_SRC_MAPPED_MODULE) {
        appendPart(s.lockSrcMapped);
    }
    if (mask & LOCK_SRC_RESTART_MANAGER) {
        appendPart(s.lockSrcRm);
    }
    if (mask & LOCK_SRC_PROTECTED_CORE) {
        appendPart(s.lockSrcProtectedCore);
    }
    return out;
}

void TabLocks::SortEntries() {
    std::sort(result_.entries.begin(), result_.entries.end(), [this](const LockEntry& a, const LockEntry& b) {
        int cmp = 0;
        switch (sortCol_) {
        case 0: cmp = _wcsicmp(a.processName.c_str(), b.processName.c_str()); break;
        case 1: cmp = (a.pid < b.pid) ? -1 : ((a.pid > b.pid) ? 1 : 0); break;
        case 2:
            cmp = static_cast<int>(a.criticalLevel) - static_cast<int>(b.criticalLevel);
            if (cmp == 0) cmp = _wcsicmp(a.processName.c_str(), b.processName.c_str());
            break;
        case 3:
            cmp = static_cast<int>(a.sourceMask) - static_cast<int>(b.sourceMask);
            if (cmp == 0) cmp = a.handleCount - b.handleCount;
            break;
        case 4:
            cmp = _wcsicmp(a.matchedPath.c_str(), b.matchedPath.c_str());
            break;
        }
        return sortAsc_ ? (cmp < 0) : (cmp > 0);
    });
}

void TabLocks::StartScan(const std::wstring& targetPath) {
    if (!hwnd_) return;
    std::wstring trimmed = targetPath;
    while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
    if (trimmed.empty()) return;

    LockTargetType tType = LockTargetType::Unknown;
    bool tExists = false;
    std::wstring norm = LockChecker::NormalizeTargetPath(trimmed, tType, tExists);
    currentTarget_ = norm.empty() ? trimmed : norm;

    if (hEditPath_) {
        SetWindowTextW(hEditPath_, currentTarget_.c_str());
    }

    uint64_t gen = ++scanGen_;
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    result_ = LockCheckResult{};
    if (hList_) {
        ListView_SetItemCountEx(hList_, 0, LVSICF_NOINVALIDATEALL);
    }
    if (onSelectionChanged) onSelectionChanged();
    scanning_.store(true, std::memory_order_release);
    LayoutControls();
    InvalidateRect(hwnd_, nullptr, FALSE);

    std::wstring pathCopy = currentTarget_;
    HWND notifyHwnd = hwnd_;
    ProcessMonitor* pm = procMon_;
    scanThread_ = std::thread([pathCopy, gen, notifyHwnd, pm]() {
        LockChecker::IconResolver resolver = nullptr;
        if (pm) {
            resolver = [pm](const std::wstring& imgPath) -> HICON {
                return pm->GetIconForPath(imgPath);
            };
        }
        auto* out = new LockCheckResult(LockChecker::Analyze(pathCopy, resolver));
        if (!IsWindow(notifyHwnd) ||
            !PostMessageW(notifyHwnd, WM_APP_LOCKS_READY, static_cast<WPARAM>(gen), reinterpret_cast<LPARAM>(out))) {
            delete out;
        }
    });
}

int TabLocks::GetSelectedRow() const {
    if (!hList_) return -1;
    return ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
}

bool TabLocks::HasKillableSelection() const {
    int row = GetSelectedRow();
    if (row < 0 || row >= static_cast<int>(result_.entries.size())) return false;
    return result_.entries[static_cast<size_t>(row)].canTerminate;
}

void TabLocks::EndSelectedLockingProcess() {
    int row = GetSelectedRow();
    if (row < 0 || row >= static_cast<int>(result_.entries.size())) return;
    const auto& e = result_.entries[static_cast<size_t>(row)];
    const Strings& s = GetStrings(settings_ ? settings_->langMode : LangMode::Auto);
    if (!e.canTerminate || e.criticalLevel == ProcessCriticalLevel::CriticalCore) {
        MessageBoxW(hwnd_, s.lockCannotKillCore, s.tabLocks, MB_OK | MB_ICONWARNING);
        return;
    }
    if (!ProcessMonitor::EndTask(e.pid)) {
        MessageBoxW(hwnd_, s.msgAccessDenied, s.tabLocks, MB_OK | MB_ICONWARNING);
        return;
    }
    // Re-scan quickly after process termination to verify lock release
    if (!currentTarget_.empty()) {
        StartScan(currentTarget_);
    }
}

void TabLocks::BrowseFile() {
    wchar_t fileBuf[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = ARRAYSIZE(fileBuf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        StartScan(fileBuf);
    }
}

void TabLocks::BrowseFolder() {
    IFileDialog* pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (SUCCEEDED(pfd->Show(hwnd_))) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                    std::wstring chosen(pszPath);
                    CoTaskMemFree(pszPath);
                    StartScan(chosen);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
}

std::wstring TabLocks::GetCellText(int row, int col) const {
    if (row < 0 || row >= static_cast<int>(result_.entries.size())) return L"";
    const auto& e = result_.entries[static_cast<size_t>(row)];
    const Strings& str = GetStrings(settings_ ? settings_->langMode : LangMode::Auto);
    switch (col) {
    case 0: return e.processName;
    case 1: return std::to_wstring(e.pid);
    case 2:
        if (e.criticalLevel == ProcessCriticalLevel::CriticalCore) return str.lockCritCore;
        if (e.criticalLevel == ProcessCriticalLevel::SystemService) return str.lockCritSystem;
        return str.lockCritNormal;
    case 3:
        return FormatSourceMask(e.sourceMask, e.handleCount);
    case 4:
        return !e.matchedPath.empty() ? e.matchedPath : e.imagePath;
    }
    return L"";
}

LRESULT TabLocks::HandleNotify(NMHDR* nm) {
    if (!nm || nm->hwndFrom != hList_) return 0;
    if (nm->code == NM_CUSTOMDRAW) {
        return themedLv_.HandleCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
    }
    if (nm->code == LVN_GETDISPINFOW) {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        if ((di->item.mask & LVIF_TEXT) && di->item.pszText && di->item.cchTextMax > 0) {
            std::wstring txt = GetCellText(di->item.iItem, di->item.iSubItem);
            wcsncpy_s(di->item.pszText, static_cast<size_t>(di->item.cchTextMax), txt.c_str(), _TRUNCATE);
        }
        return 0;
    }
    if (nm->code == LVN_COLUMNCLICK) {
        auto* nmlv = reinterpret_cast<NMLISTVIEW*>(nm);
        if (sortCol_ == nmlv->iSubItem) sortAsc_ = !sortAsc_;
        else { sortCol_ = nmlv->iSubItem; sortAsc_ = true; }
        SortEntries();
        themedLv_.SetSortIndicator(sortCol_, sortAsc_);
        InvalidateRect(hList_, nullptr, FALSE);
        return 0;
    }
    if (nm->code == LVN_ITEMCHANGED) {
        if (onSelectionChanged) onSelectionChanged();
        return 0;
    }
    if (nm->code == NM_RCLICK) {
        auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(nm);
        POINT pt{};
        if (nmia->iItem >= 0) {
            ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(hList_, nmia->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        GetCursorPos(&pt);
        HandleContextMenu(pt);
        return 0;
    }
    return 0;
}

void TabLocks::HandleContextMenu(POINT screenPt) {
    int row = GetSelectedRow();
    if (row < 0 || row >= static_cast<int>(result_.entries.size())) return;
    const auto entry = result_.entries[static_cast<size_t>(row)];
    const Strings& s = GetStrings(settings_ ? settings_->langMode : LangMode::Auto);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | (entry.canTerminate ? MF_ENABLED : MF_GRAYED),
                IDM_CTX_LOCKS_END, s.ctxEndTask);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (entry.imagePath.empty() ? MF_GRAYED : MF_ENABLED),
                IDM_CTX_LOCKS_OPEN_LOC, s.ctxOpenLocation);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_LOCKS_GO_DETAILS, s.ctxGoToDetails);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (entry.matchedPath.empty() && entry.imagePath.empty() ? MF_GRAYED : MF_ENABLED),
                IDM_CTX_LOCKS_COPY_PATH, s.lockCtxCopyPath);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);

    if (cmd == IDM_CTX_LOCKS_END) {
        EndSelectedLockingProcess();
    } else if (cmd == IDM_CTX_LOCKS_OPEN_LOC && !entry.imagePath.empty()) {
        OpenFileLocationInExplorer(entry.imagePath);
    } else if (cmd == IDM_CTX_LOCKS_GO_DETAILS && onGoToDetails) {
        onGoToDetails(entry.pid);
    } else if (cmd == IDM_CTX_LOCKS_COPY_PATH) {
        CopyTextToClipboard(hwnd_, !entry.matchedPath.empty() ? entry.matchedPath : entry.imagePath);
    }
}

void TabLocks::DrawOwnerButton(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    auto& tm = ThemeManager::Instance();
    const auto& pal = tm.Palette();
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    bool hovered = false;
    if (dis->CtlID == IDC_LOCKS_BTN_FILE) hovered = hoverFile_;
    else if (dis->CtlID == IDC_LOCKS_BTN_FOLDER) hovered = hoverFolder_;
    else if (dis->CtlID == IDC_LOCKS_BTN_SCAN) hovered = hoverScan_;
    else if (dis->CtlID == IDC_LOCKS_BTN_ELEVATE) hovered = hoverElevate_;

    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    COLORREF bg = disabled ? pal.btnDisabledBg : (pressed ? pal.btnPressedBg : (hovered ? pal.btnHoverBg : pal.btnBg));
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, hovered ? pal.accentColor : pal.btnBorder);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    wchar_t text[128] = {};
    GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));
    int oldBk = SetBkMode(hdc, TRANSPARENT);
    COLORREF txtClr = disabled ? pal.textDisabled : pal.textPrimary;
    tm.DrawModernText(hdc, hwnd_, 9, false, text, rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, txtClr);
    SetBkMode(hdc, oldBk);
}

void TabLocks::PaintPanel(HDC hdc, const RECT& rc) {
    auto& tm = ThemeManager::Instance();
    const auto& pal = tm.Palette();
    const Strings& s = GetStrings(settings_ ? settings_->langMode : LangMode::Auto);

    FillRect(hdc, &rc, tm.SurfaceBrush());
    int oldBkMode = SetBkMode(hdc, TRANSPARENT);

    // Vertical splitter between left and right panes
    HPEN splitPen = CreatePen(PS_SOLID, 1, pal.splitterColor);
    HGDIOBJ oldPen = SelectObject(hdc, splitPen);
    MoveToEx(hdc, leftPaneRc_.right, 0, nullptr);
    LineTo(hdc, leftPaneRc_.right, rc.bottom);
    SelectObject(hdc, oldPen);
    DeleteObject(splitPen);

    // Edit border
    if (editFrameRc_.right > editFrameRc_.left) {
        HBRUSH eb = CreateSolidBrush(pal.editBg);
        FillRect(hdc, &editFrameRc_, eb);
        DeleteObject(eb);
        HPEN ep = CreatePen(PS_SOLID, 1, pal.editBorder);
        HGDIOBJ op = SelectObject(hdc, ep);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, editFrameRc_.left, editFrameRc_.top, editFrameRc_.right, editFrameRc_.bottom);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(ep);
    }

    // Drive pills header label & clickable pills
    if (!drivePills_.empty()) {
        int pad = DpiScale(hwnd_, 12);
        RECT lblRc = { pad, drivePills_[0].rc.top - DpiScale(hwnd_, 22), leftPaneRc_.right - pad, drivePills_[0].rc.top };
        tm.DrawModernText(hdc, hwnd_, 9, false, s.lockQuickDrives, lblRc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, pal.textSecondary);

        for (size_t i = 0; i < drivePills_.size(); ++i) {
            const auto& p = drivePills_[i];
            bool active = IsSamePathNoCase(p.driveRoot, currentTarget_);
            bool hov = (static_cast<int>(i) == hoveredPill_);
            COLORREF pBg = active ? pal.tabActiveBg : (hov ? pal.btnHoverBg : pal.btnBg);
            COLORREF pBd = active ? pal.accentColor : (hov ? pal.accentColor : pal.btnBorder);
            HBRUSH pBr = CreateSolidBrush(pBg);
            FillRect(hdc, &p.rc, pBr);
            DeleteObject(pBr);
            HPEN pPen = CreatePen(PS_SOLID, 1, pBd);
            HGDIOBJ op2 = SelectObject(hdc, pPen);
            HGDIOBJ ob2 = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, p.rc.left, p.rc.top, p.rc.right, p.rc.bottom);
            SelectObject(hdc, ob2);
            SelectObject(hdc, op2);
            DeleteObject(pPen);

            tm.DrawModernText(hdc, hwnd_, 9, active, p.driveRoot.c_str(), p.rc,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE, pal.textPrimary);
        }
    }

    // Left drop zone with dashed border
    {
        HBRUSH dzBg = CreateSolidBrush(pal.surfaceAltBg);
        FillRect(hdc, &dropZoneRc_, dzBg);
        DeleteObject(dzBg);

        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        COLORREF borderClr = scanning_.load(std::memory_order_acquire) ? pal.accentColor : pal.borderColor;
        Gdiplus::Pen dashPen(Gdiplus::Color(255, GetRValue(borderClr), GetGValue(borderClr), GetBValue(borderClr)), 1.5f);
        dashPen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawRectangle(&dashPen,
                        static_cast<Gdiplus::REAL>(dropZoneRc_.left + 1),
                        static_cast<Gdiplus::REAL>(dropZoneRc_.top + 1),
                        static_cast<Gdiplus::REAL>(std::max<int>(4, dropZoneRc_.right - dropZoneRc_.left - 3)),
                        static_cast<Gdiplus::REAL>(std::max<int>(4, dropZoneRc_.bottom - dropZoneRc_.top - 3)));

        int dzPad = DpiScale(hwnd_, 16);
        int dzH = std::max(40, static_cast<int>(dropZoneRc_.bottom - dropZoneRc_.top));
        int titleY = dropZoneRc_.top + dzH / 2 - DpiScale(hwnd_, 26);
        RECT tRc = { dropZoneRc_.left + dzPad, titleY, dropZoneRc_.right - dzPad, titleY + DpiScale(hwnd_, 26) };
        const wchar_t* titleStr = scanning_.load(std::memory_order_acquire) ? s.lockScanning : s.lockDropTitle;
        tm.DrawModernText(hdc, hwnd_, 11, true, titleStr, tRc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                          scanning_.load(std::memory_order_acquire) ? pal.accentColor : pal.textPrimary);

        RECT hintRc = { dropZoneRc_.left + dzPad, tRc.bottom + DpiScale(hwnd_, 4),
                        dropZoneRc_.right - dzPad, dropZoneRc_.bottom - DpiScale(hwnd_, 10) };
        tm.DrawModernText(hdc, hwnd_, 9, false, s.lockDropHint, hintRc,
                          DT_CENTER | DT_TOP | DT_WORDBREAK, pal.textSecondary);
    }

    // Right top diagnosis card
    {
        HBRUSH cardBr = CreateSolidBrush(pal.surfaceAltBg);
        FillRect(hdc, &diagCardRc_, cardBr);
        DeleteObject(cardBr);
        HPEN cardPen = CreatePen(PS_SOLID, 1, pal.borderColor);
        HGDIOBJ op = SelectObject(hdc, cardPen);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, diagCardRc_.left, diagCardRc_.top, diagCardRc_.right, diagCardRc_.bottom);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(cardPen);

        int cPad = DpiScale(hwnd_, 12);
        int rx = diagCardRc_.left + cPad;
        int rr = diagCardRc_.right - cPad;
        int lineH = DpiScale(hwnd_, 24);
        int y = diagCardRc_.top + DpiScale(hwnd_, 10);

        if (currentTarget_.empty()) {
            RECT r0 = { rx, diagCardRc_.top, rr, diagCardRc_.bottom };
            tm.DrawModernText(hdc, hwnd_, 10, false, s.lockDropHint, r0,
                              DT_LEFT | DT_VCENTER | DT_WORDBREAK, pal.textSecondary);
        } else {
            const auto& t = result_.target;
            std::wstring typeTag = s.lockTypeFile;
            if (t.targetType == LockTargetType::Directory) typeTag = s.lockTypeDir;
            else if (t.targetType == LockTargetType::Drive) typeTag = s.lockTypeDrive;

            std::wstring line1 = L"[" + typeTag + L"]  " + (t.normalizedPath.empty() ? currentTarget_ : t.normalizedPath);
            if (t.exists && t.targetType == LockTargetType::File) {
                line1 += L"   (" + FormatBytesSimple(t.sizeBytes) + L")";
            }
            RECT r1 = { rx, y, rr, y + lineH };
            tm.DrawModernText(hdc, hwnd_, 10, true, line1.c_str(), r1,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS, pal.textPrimary);
            y += lineH + DpiScale(hwnd_, 4);

            if (!t.exists) {
                RECT rMissing = { rx, y, rr, y + lineH };
                COLORREF errClr = pal.isDark ? RGB(242, 102, 102) : RGB(196, 43, 28);
                tm.DrawModernText(hdc, hwnd_, 9, false, s.lockTargetMissing, rMissing,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE, errClr);
            } else {
                // Line 2: Owner
                std::wstring ownerStr = std::wstring(s.lockOwnerLabel) + L" " +
                    (t.ownerName.empty() ? s.lockOwnerUnknown : t.ownerName);
                if (t.isReadOnly) {
                    ownerStr += s.lockReadOnlyTag;
                }
                RECT r2 = { rx, y, rr, y + lineH };
                tm.DrawModernText(hdc, hwnd_, 9, false, ownerStr.c_str(), r2,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, pal.textSecondary);
                y += lineH + DpiScale(hwnd_, 2);

                // Line 3: Permission diagnosis
                bool canDel = t.hasDeleteAccess || t.hasParentDeleteChild;
                std::wstring permText;
                COLORREF permClr = pal.isDark ? RGB(108, 212, 134) : RGB(16, 124, 16);
                if (!t.aclChecked) {
                    permText = s.lockPermAclDenied;
                    permClr = pal.isDark ? RGB(242, 102, 102) : RGB(196, 43, 28);
                } else if (t.ownerIsTrustedInstaller || t.ownerIsSystem) {
                    if (!canDel) {
                        wchar_t buf[256] = {};
                        swprintf_s(buf, s.lockPermTrustedInst, t.ownerName.c_str());
                        permText = buf;
                        permClr = pal.isDark ? RGB(242, 102, 102) : RGB(196, 43, 28);
                    } else if (t.isReadOnly) {
                        permText = s.lockPermReadOnly;
                        permClr = pal.isDark ? RGB(245, 178, 66) : RGB(178, 102, 0);
                    } else {
                        permText = s.lockPermOkDelete;
                    }
                } else if (!canDel) {
                    permText = s.lockPermNoDelete;
                    permClr = pal.isDark ? RGB(242, 102, 102) : RGB(196, 43, 28);
                } else if (t.isReadOnly) {
                    permText = s.lockPermReadOnly;
                    permClr = pal.isDark ? RGB(245, 178, 66) : RGB(178, 102, 0);
                } else {
                    permText = s.lockPermOkDelete;
                }

                RECT r3 = { rx, y, rr, y + lineH };
                tm.DrawModernText(hdc, hwnd_, 9, true, (std::wstring(s.lockPermLabel) + L" " + permText).c_str(),
                                  r3, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, permClr);
                y += lineH + DpiScale(hwnd_, 2);

                // Line 4: Lock summary + elevation notice
                int rightLimit = (hBtnElevate_ && IsWindowVisible(hBtnElevate_))
                    ? (rr - DpiScale(hwnd_, 140))
                    : rr;
                std::wstring lockSum;
                if (scanning_.load(std::memory_order_acquire)) {
                    lockSum = s.lockScanning;
                } else {
                    wchar_t sBuf[256] = {};
                    swprintf_s(sBuf, s.lockSummaryFmt,
                               s.lockSummaryLabel,
                               static_cast<int>(result_.entries.size()),
                               result_.criticalCount,
                               result_.elapsedMs);
                    lockSum = sBuf;
                    if (!t.isCurrentElevated && result_.deniedProcessOpenCount > 0) {
                        lockSum += L"  |  ";
                        lockSum += s.lockPermNeedAdmin;
                    }
                }
                RECT r4 = { rx, y, std::max(rx + 40, rightLimit), y + lineH };
                COLORREF sumClr = (result_.criticalCount > 0)
                    ? (pal.isDark ? RGB(245, 178, 66) : RGB(178, 102, 0))
                    : pal.textSecondary;
                tm.DrawModernText(hdc, hwnd_, 9, false, lockSum.c_str(), r4,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, sumClr);
            }
        }
    }
    SetBkMode(hdc, oldBkMode);
}

LRESULT CALLBACK TabLocks::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TabLocks* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TabLocks*>(cs->lpCreateParams);
        if (self) self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TabLocks*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TabLocks::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        HDC memDc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<int>(1, rc.right), std::max<int>(1, rc.bottom));
        HGDIOBJ oldBmp = SelectObject(memDc, bmp);
        PaintPanel(memDc, rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        return reinterpret_cast<LRESULT>(
            ThemeManager::Instance().HandleCtlColor(msg, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
    case WM_DRAWITEM:
        DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_MOUSEMOVE: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        int nextHov = -1;
        for (size_t i = 0; i < drivePills_.size(); ++i) {
            if (PtInRect(&drivePills_[i].rc, pt)) {
                nextHov = static_cast<int>(i);
                break;
            }
        }
        if (nextHov != hoveredPill_) {
            hoveredPill_ = nextHov;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hoveredPill_ >= 0) {
            hoveredPill_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        for (const auto& pill : drivePills_) {
            if (PtInRect(&pill.rc, pt)) {
                StartScan(pill.driveRoot);
                return 0;
            }
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t dropped[MAX_PATH * 4] = {};
        if (DragQueryFileW(hDrop, 0, dropped, ARRAYSIZE(dropped)) > 0) {
            StartScan(dropped);
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        if (id == IDC_LOCKS_BTN_FILE) {
            BrowseFile();
            return 0;
        }
        if (id == IDC_LOCKS_BTN_FOLDER) {
            BrowseFolder();
            return 0;
        }
        if (id == IDC_LOCKS_BTN_SCAN) {
            if (hEditPath_) {
                int len = GetWindowTextLengthW(hEditPath_);
                std::wstring text(static_cast<size_t>(std::max(0, len)) + 1, L'\0');
                if (len > 0) {
                    GetWindowTextW(hEditPath_, &text[0], len + 1);
                    text.resize(static_cast<size_t>(len));
                } else {
                    text.clear();
                }
                if (!text.empty()) StartScan(text);
            }
            return 0;
        }
        if (id == IDC_LOCKS_BTN_ELEVATE) {
            if (onElevateRequested) onElevateRequested();
            return 0;
        }
        break;
    }
    case WM_NOTIFY:
        return HandleNotify(reinterpret_cast<NMHDR*>(lParam));
    case WM_CONTEXTMENU: {
        HWND src = reinterpret_cast<HWND>(wParam);
        if (src == hList_) {
            POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                int row = GetSelectedRow();
                if (row >= 0) ListView_GetItemRect(hList_, row, &rc, LVIR_BOUNDS);
                else GetClientRect(hList_, &rc);
                pt = { rc.left + 20, (rc.top + rc.bottom) / 2 };
                ClientToScreen(hList_, &pt);
            }
            HandleContextMenu(pt);
            return 0;
        }
        break;
    }
    case WM_APP_LOCKS_READY: {
        uint64_t gen = static_cast<uint64_t>(wParam);
        auto* incoming = reinterpret_cast<LockCheckResult*>(lParam);
        if (gen == scanGen_.load(std::memory_order_acquire) && incoming) {
            result_ = std::move(*incoming);
            scanning_.store(false, std::memory_order_release);
            SortEntries();
            if (hList_) {
                ListView_SetItemCountEx(hList_, static_cast<int>(result_.entries.size()), LVSICF_NOINVALIDATEALL);
                InvalidateRect(hList_, nullptr, FALSE);
            }
            LayoutControls();
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (onSelectionChanged) onSelectionChanged();
        }
        delete incoming;
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace taskman
