#include "tab_app_history.h"
#include "../resource.h"
#include "../core/i18n.h"
#include "../core/theme.h"
#include "../core/format_util.h"
#include <algorithm>
#include <cwctype>

namespace taskman {

namespace {

constexpr wchar_t kAppHistBannerClass[] = L"BetterWin10TaskmanHistBanner";
bool g_bannerRegistered = false;

std::wstring LowerStr(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

LRESULT CALLBACK BannerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<TabAppHistory*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<TabAppHistory*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        self->HandleParentDraw(hwnd, hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        if (self->HitTestLink(pt)) {
            self->HandleCommand(IDC_LINK_ACTION);
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (self->HitTestLink(pt)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void TabAppHistory::Create(HWND parent, AppSettings* settings) {
    hParent_ = parent;
    settings_ = settings;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!g_bannerRegistered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = BannerWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kAppHistBannerClass;
        RegisterClassExW(&wc);
        g_bannerRegistered = true;
    }

    hBanner_ = CreateWindowExW(
        0, kAppHistBannerClass, L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 100, 30,
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LINK_ACTION)), hInst, this);

    hList_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100,
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LIST_APP_HISTORY)), hInst, nullptr);

    themedLv_.Attach(hList_, false);

    ListViewThemeCallbacks cb;
    cb.getCellText = [this](int row, int col) -> std::wstring {
        if (row < 0 || row >= static_cast<int>(entries_.size())) return L"";
        const auto& e = entries_[static_cast<size_t>(row)];
        switch (col) {
        case 0: return e.name;
        case 1: return FormatDurationHMS(e.cpuTimeSeconds);
        case 2: return FormatBytesAuto(e.networkBytes);
        case 3: return FormatBytesAuto(e.meteredBytes);
        case 4: return FormatBytesAuto(e.tileUpdatesBytes);
        default: return L"";
        }
    };
    cb.getRowIcon = [this](int row) -> HICON {
        if (row < 0 || row >= static_cast<int>(entries_.size())) return nullptr;
        return entries_[static_cast<size_t>(row)].icon;
    };
    cb.getRowIndent = [](int) { return 1; };
    cb.getRowExpandState = [](int) { return -1; };
    cb.isCategoryHeader = [](int) { return false; };
    cb.getCellHeat = [this](int row, int col) -> int {
        if (row < 0 || row >= static_cast<int>(entries_.size())) return 0;
        const auto& e = entries_[static_cast<size_t>(row)];
        if (col == 1) {
            if (e.cpuTimeSeconds >= 300) return 3;
            if (e.cpuTimeSeconds >= 30)  return 2;
            if (e.cpuTimeSeconds >= 3)   return 1;
        } else if (col == 2) {
            if (e.networkBytes >= 100ULL * 1024 * 1024) return 3;
            if (e.networkBytes >= 10ULL * 1024 * 1024)  return 2;
            if (e.networkBytes >= 1024ULL * 1024)       return 1;
        }
        return 0;
    };
    cb.onHeaderTrack = [this]() {
        SaveColumnWidths();
        if (onColumnsChanged) onColumnsChanged();
    };
    themedLv_.SetCallbacks(std::move(cb));
    RebuildColumns();
    ApplyTheme();
}

void TabAppHistory::Destroy() {
    SaveColumnWidths();
    themedLv_.Detach();
    if (hBanner_) { DestroyWindow(hBanner_); hBanner_ = nullptr; }
    if (hList_) { DestroyWindow(hList_); hList_ = nullptr; }
}

void TabAppHistory::Resize(const RECT& rc) {
    int bannerH = DpiScale(hParent_, 34);
    if (hBanner_) {
        MoveWindow(hBanner_, rc.left, rc.top, rc.right - rc.left, bannerH, TRUE);
    }
    if (hList_) {
        MoveWindow(hList_, rc.left, rc.top + bannerH, rc.right - rc.left,
                   std::max(0L, (rc.bottom - rc.top) - bannerH), TRUE);
    }
}

void TabAppHistory::Show(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    if (hBanner_) ShowWindow(hBanner_, cmd);
    if (hList_) ShowWindow(hList_, cmd);
}

void TabAppHistory::ApplyTheme() {
    themedLv_.ApplyTheme();
    if (hBanner_) InvalidateRect(hBanner_, nullptr, FALSE);
}

void TabAppHistory::SaveColumnWidths() {
    if (!hList_ || !settings_) return;
    ThemedListView::SaveColumnsToArray(hList_, nullptr, 5, settings_->appHistColWidths, 5);
}

void TabAppHistory::RebuildColumns() {
    if (!hList_ || !settings_) return;
    SaveColumnWidths();
    int oldCount = Header_GetItemCount(themedLv_.HeaderHwnd());
    for (int i = oldCount - 1; i >= 0; --i) {
        ListView_DeleteColumn(hList_, i);
    }

    const auto& s = GetStrings(settings_->langMode);
    struct ColDef { const wchar_t* title; int width; bool right; } cols[5] = {
        { s.colName, settings_->appHistColWidths[0], false },
        { s.colCpuTime, settings_->appHistColWidths[1], true },
        { s.colNetwork, settings_->appHistColWidths[2], true },
        { s.colMeteredNet, settings_->appHistColWidths[3], true },
        { s.colTileUpdates, settings_->appHistColWidths[4], true }
    };

    std::vector<HeaderColumnMeta> metas;
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt = cols[i].right ? LVCFMT_RIGHT : LVCFMT_LEFT;
        lvc.cx = DpiScale(hList_, cols[i].width);
        lvc.pszText = const_cast<LPWSTR>(cols[i].title);
        ListView_InsertColumn(hList_, i, &lvc);

        HeaderColumnMeta hm;
        hm.title = cols[i].title;
        hm.rightAlign = cols[i].right;
        metas.push_back(std::move(hm));
    }
    themedLv_.SetColumnsMeta(std::move(metas));
    themedLv_.SetSortIndicator(sortCol_, sortAsc_);
    if (hBanner_) InvalidateRect(hBanner_, nullptr, FALSE);
}

bool TabAppHistory::HandleParentDraw(HWND, HDC hdc, const RECT& rc) {
    ThemeManager& tm = ThemeManager::Instance();
    const auto& pal = tm.Palette();
    const auto& s = GetStrings(settings_->langMode);

    HBRUSH bg = CreateSolidBrush(pal.surfaceBg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    int padX = DpiScale(hBanner_, 14);
    int w = rc.right - rc.left;
    int linkW = tm.MeasureTextWidth(hdc, hBanner_, 10, false, s.btnDeleteHistory);
    int gap = DpiScale(hBanner_, 12);
    int leftRight = std::max<int>(padX, w - padX - linkW - gap);
    std::wstring bannerText;
    if (!sinceDate_.empty()) {
        if (GetEffectiveLang(settings_->langMode) == LangMode::Chinese) {
            bannerText = L"自 " + sinceDate_ + L" 以来，当前用户账户的资源使用情况。";
        } else {
            bannerText = L"Resource usage since " + sinceDate_ + L" for the current user account.";
        }
    } else {
        bannerText = s.appHistoryNote;
    }
    RECT rcLeft{ padX, rc.top, leftRight, rc.bottom };
    tm.DrawModernText(hdc, hBanner_, 10, false, bannerText.c_str(), rcLeft,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, pal.textSecondary);

    linkRect_ = { std::max<int>(rcLeft.right + gap, w - padX - linkW), rc.top, w - padX, rc.bottom };
    tm.DrawModernText(hdc, hBanner_, 10, false, s.btnDeleteHistory, linkRect_,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, pal.accentColor);

    HPEN pen = CreatePen(PS_SOLID, 1, pal.borderColor);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, 0, rc.bottom - 1, nullptr);
    LineTo(hdc, rc.right, rc.bottom - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    return true;
}

bool TabAppHistory::HandleCommand(WORD id) {
    if (id == IDC_LINK_ACTION) {
        if (onDeleteHistory) onDeleteHistory();
        return true;
    }
    return false;
}

void TabAppHistory::SortEntries() {
    std::sort(entries_.begin(), entries_.end(), [this](const AppHistoryEntry& a, const AppHistoryEntry& b) {
        int cmp = 0;
        switch (sortCol_) {
        case 0: cmp = LowerStr(a.name).compare(LowerStr(b.name)); break;
        case 1: cmp = (a.cpuTimeSeconds < b.cpuTimeSeconds) ? -1 : (a.cpuTimeSeconds > b.cpuTimeSeconds ? 1 : 0); break;
        case 2: cmp = (a.networkBytes < b.networkBytes) ? -1 : (a.networkBytes > b.networkBytes ? 1 : 0); break;
        case 3: cmp = (a.meteredBytes < b.meteredBytes) ? -1 : (a.meteredBytes > b.meteredBytes ? 1 : 0); break;
        case 4: cmp = (a.tileUpdatesBytes < b.tileUpdatesBytes) ? -1 : (a.tileUpdatesBytes > b.tileUpdatesBytes ? 1 : 0); break;
        }
        if (cmp == 0) cmp = LowerStr(a.name).compare(LowerStr(b.name));
        return sortAsc_ ? (cmp < 0) : (cmp > 0);
    });
}

void TabAppHistory::RefreshData(const AppHistoryManager& mgr) {
    if (!hList_ || !settings_) return;
    std::wstring newSince = mgr.GetSinceDate();
    if (newSince != sinceDate_) {
        sinceDate_ = std::move(newSince);
        if (hBanner_) InvalidateRect(hBanner_, nullptr, FALSE);
    }

    std::wstring selKey;
    int selIdx = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (selIdx >= 0 && selIdx < static_cast<int>(entries_.size())) {
        selKey = entries_[static_cast<size_t>(selIdx)].key;
    }

    entries_ = mgr.GetEntries(settings_->showAllHistory);
    SortEntries();
    ListView_SetItemCountEx(hList_, static_cast<int>(entries_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    if (!selKey.empty()) {
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            if (entries_[static_cast<size_t>(i)].key == selKey) {
                if (i != selIdx) {
                    ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_SetItemState(hList_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                break;
            }
        }
    }
    InvalidateRect(hList_, nullptr, FALSE);
}

LRESULT TabAppHistory::HandleNotify(NMHDR* nm) {
    if (!nm) return 0;
    if (ThemedListView::IsHeaderTrackNotify(nm, themedLv_.HeaderHwnd())) {
        SaveColumnWidths();
        if (onColumnsChanged) onColumnsChanged();
        return 0;
    }
    if (nm->idFrom == IDC_LIST_APP_HISTORY) {
        if (nm->code == NM_CUSTOMDRAW) {
            return themedLv_.HandleCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
        }
        if (nm->code == LVN_COLUMNCLICK) {
            auto* lvn = reinterpret_cast<NMLISTVIEW*>(nm);
            if (sortCol_ == lvn->iSubItem) sortAsc_ = !sortAsc_;
            else {
                sortCol_ = lvn->iSubItem;
                sortAsc_ = (sortCol_ == 0);
            }
            themedLv_.SetSortIndicator(sortCol_, sortAsc_);
            SortEntries();
            InvalidateRect(hList_, nullptr, FALSE);
            return 0;
        }
        if (nm->code == NM_RCLICK) {
            POINT pt = HandleListRightClickSelection(hList_, nm, static_cast<int>(entries_.size()));
            HandleContextMenu(pt);
            return 0;
        }
    }
    return 0;
}

void TabAppHistory::HandleContextMenu(POINT screenPt) {
    int selIdx = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (selIdx < 0 || selIdx >= static_cast<int>(entries_.size())) return;
    screenPt = ResolveListContextMenuPoint(hList_, selIdx, screenPt);
    const auto& entry = entries_[static_cast<size_t>(selIdx)];
    const auto& s = GetStrings(settings_->langMode);

    HMENU hMenu = CreatePopupMenu();
    UINT fileFlags = MF_STRING | (entry.imagePath.empty() ? MF_GRAYED : 0);
    AppendMenuW(hMenu, fileFlags, IDM_CTX_OPEN_LOCATION, s.ctxOpenLocation);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SEARCH_ONLINE, s.ctxSearchOnline);
    AppendMenuW(hMenu, fileFlags, IDM_CTX_PROPERTIES, s.ctxProperties);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hParent_, nullptr);
    DestroyMenu(hMenu);

    if (cmd == IDM_CTX_OPEN_LOCATION && !entry.imagePath.empty()) {
        OpenFileLocationInExplorer(entry.imagePath);
    } else if (cmd == IDM_CTX_SEARCH_ONLINE) {
        SearchProcessOnline(entry.name);
    } else if (cmd == IDM_CTX_PROPERTIES && !entry.imagePath.empty()) {
        ShowFilePropertiesDialog(hParent_, entry.imagePath);
    }
}

} // namespace taskman
