#include "tab_startup.h"
#include "../resource.h"
#include "../core/i18n.h"
#include "../core/theme.h"
#include "../core/format_util.h"
#include <algorithm>
#include <cwctype>

namespace taskman {

namespace {

std::wstring LowerStr(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

const wchar_t* ImpactText(int impact, const Strings& s) {
    switch (impact) {
    case 3: return s.impactHigh;
    case 2: return s.impactMedium;
    case 1: return s.impactLow;
    default: return s.impactNotMeasured;
    }
}

} // namespace

void TabStartup::Create(HWND parent, AppSettings* settings) {
    hParent_ = parent;
    settings_ = settings;

    hList_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_CLIPCHILDREN | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100,
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LIST_STARTUP)),
        GetModuleHandleW(nullptr), nullptr);

    themedLv_.Attach(hList_, false);

    ListViewThemeCallbacks cb;
    cb.getCellText = [this](int row, int col) -> std::wstring {
        if (!mgr_ || row < 0 || row >= static_cast<int>(order_.size())) return L"";
        const auto& items = mgr_->Items();
        size_t idx = order_[static_cast<size_t>(row)];
        if (idx >= items.size()) return L"";
        const auto& it = items[idx];
        const auto& s = GetStrings(settings_->langMode);
        switch (col) {
        case 0: return it.name;
        case 1: return it.publisher;
        case 2: return it.enabled ? s.statusEnabled : s.statusDisabled;
        case 3: return ImpactText(it.impact, s);
        default: return L"";
        }
    };
    cb.getRowIcon = [this](int row) -> HICON {
        if (!mgr_ || row < 0 || row >= static_cast<int>(order_.size())) return nullptr;
        size_t idx = order_[static_cast<size_t>(row)];
        return idx < mgr_->Items().size() ? mgr_->Items()[idx].icon : nullptr;
    };
    cb.getRowIndent = [](int) { return 1; };
    cb.getRowExpandState = [](int) { return -1; };
    cb.isCategoryHeader = [](int) { return false; };
    cb.getCellTextColor = [this](int row, int col, bool& overrideColor) -> COLORREF {
        overrideColor = false;
        if (col == 2 && mgr_ && row >= 0 && row < static_cast<int>(order_.size())) {
            size_t idx = order_[static_cast<size_t>(row)];
            if (idx < mgr_->Items().size() && !mgr_->Items()[idx].enabled) {
                overrideColor = true;
                return ThemeManager::Instance().Palette().textSecondary;
            }
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

void TabStartup::Destroy() {
    SaveColumnWidths();
    themedLv_.Detach();
    if (hList_) { DestroyWindow(hList_); hList_ = nullptr; }
}

void TabStartup::Resize(const RECT& rc) {
    if (hList_) MoveWindow(hList_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void TabStartup::Show(bool visible) {
    if (hList_) ShowWindow(hList_, visible ? SW_SHOW : SW_HIDE);
}

void TabStartup::ApplyTheme() {
    themedLv_.ApplyTheme();
}

void TabStartup::SaveColumnWidths() {
    if (!hList_ || !settings_) return;
    ThemedListView::SaveColumnsToArray(hList_, nullptr, 4, settings_->startupColWidths, 4);
}

void TabStartup::RebuildColumns() {
    if (!hList_ || !settings_) return;
    SaveColumnWidths();
    int oldCount = Header_GetItemCount(themedLv_.HeaderHwnd());
    for (int i = oldCount - 1; i >= 0; --i) {
        ListView_DeleteColumn(hList_, i);
    }

    const auto& s = GetStrings(settings_->langMode);
    struct ColDef { const wchar_t* title; int width; } cols[4] = {
        { s.colName, settings_->startupColWidths[0] },
        { s.colPublisher, settings_->startupColWidths[1] },
        { s.colStatus, settings_->startupColWidths[2] },
        { s.colStartupImpact, settings_->startupColWidths[3] }
    };

    std::vector<HeaderColumnMeta> metas;
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt = LVCFMT_LEFT;
        lvc.cx = DpiScale(hList_, cols[i].width);
        lvc.pszText = const_cast<LPWSTR>(cols[i].title);
        ListView_InsertColumn(hList_, i, &lvc);

        HeaderColumnMeta hm;
        hm.title = cols[i].title;
        metas.push_back(std::move(hm));
    }
    themedLv_.SetColumnsMeta(std::move(metas));
    themedLv_.SetSortIndicator(sortCol_, sortAsc_);
}

void TabStartup::SortRows() {
    if (!mgr_) return;
    const auto& items = mgr_->Items();
    std::sort(order_.begin(), order_.end(), [&](size_t ia, size_t ib) {
        const auto& a = items[ia];
        const auto& b = items[ib];
        int cmp = 0;
        switch (sortCol_) {
        case 0: cmp = LowerStr(a.name).compare(LowerStr(b.name)); break;
        case 1: cmp = LowerStr(a.publisher).compare(LowerStr(b.publisher)); break;
        case 2: cmp = (a.enabled ? 1 : 0) - (b.enabled ? 1 : 0); break;
        case 3: cmp = a.impact - b.impact; break;
        }
        if (cmp == 0) cmp = LowerStr(a.name).compare(LowerStr(b.name));
        return sortAsc_ ? (cmp < 0) : (cmp > 0);
    });
}

void TabStartup::RefreshData(StartupManager& mgr) {
    mgr_ = &mgr;
    if (!hList_) return;

    std::wstring selName;
    int selRow = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (selRow >= 0 && selRow < static_cast<int>(order_.size())) {
        size_t idx = order_[static_cast<size_t>(selRow)];
        if (idx < mgr_->Items().size()) selName = mgr_->Items()[idx].name;
    }

    order_.resize(mgr_->Items().size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = i;
    SortRows();

    ListView_SetItemCountEx(hList_, static_cast<int>(order_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    if (!selName.empty()) {
        for (int i = 0; i < static_cast<int>(order_.size()); ++i) {
            if (mgr_->Items()[order_[static_cast<size_t>(i)]].name == selName) {
                if (i != selRow) {
                    ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_SetItemState(hList_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                break;
            }
        }
    }
    InvalidateRect(hList_, nullptr, FALSE);
}

bool TabStartup::HasSelection() const {
    if (!hList_ || !mgr_) return false;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    return sel >= 0 && sel < static_cast<int>(order_.size());
}

bool TabStartup::SelectedIsEnabled() const {
    if (!HasSelection()) return true;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    return mgr_->Items()[order_[static_cast<size_t>(sel)]].enabled;
}

void TabStartup::SetSelectedEnabled(bool enable) {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    size_t idx = order_[static_cast<size_t>(sel)];
    if (!mgr_->SetEnabled(idx, enable)) {
        const auto& s = GetStrings(settings_->langMode);
        MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
    }
    if (onRefreshNeeded) onRefreshNeeded();
    if (onSelectionChanged) onSelectionChanged();
    InvalidateRect(hList_, nullptr, FALSE);
}

void TabStartup::ToggleSelected() {
    SetSelectedEnabled(!SelectedIsEnabled());
}

void TabStartup::OpenSelectedProperties() {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    const auto& it = mgr_->Items()[order_[static_cast<size_t>(sel)]];
    if (!it.exePath.empty()) ShowFilePropertiesDialog(hParent_, it.exePath);
}

LRESULT TabStartup::HandleNotify(NMHDR* nm) {
    if (!nm) return 0;
    if (ThemedListView::IsHeaderTrackNotify(nm, themedLv_.HeaderHwnd())) {
        SaveColumnWidths();
        if (onColumnsChanged) onColumnsChanged();
        return 0;
    }
    if (nm->idFrom != IDC_LIST_STARTUP) return 0;
    if (nm->code == NM_CUSTOMDRAW) {
        return themedLv_.HandleCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
    }
    if (nm->code == LVN_ITEMCHANGED) {
        if (onSelectionChanged) onSelectionChanged();
        return 0;
    }
    if (nm->code == LVN_COLUMNCLICK) {
        auto* lvn = reinterpret_cast<NMLISTVIEW*>(nm);
        if (sortCol_ == lvn->iSubItem) sortAsc_ = !sortAsc_;
        else {
            sortCol_ = lvn->iSubItem;
            sortAsc_ = true;
        }
        themedLv_.SetSortIndicator(sortCol_, sortAsc_);
        SortRows();
        InvalidateRect(hList_, nullptr, FALSE);
        return 0;
    }
    if (nm->code == NM_DBLCLK) {
        ToggleSelected();
        return 0;
    }
    if (nm->code == NM_RCLICK) {
        POINT pt = HandleListRightClickSelection(hList_, nm, static_cast<int>(order_.size()));
        HandleContextMenu(pt);
        return 0;
    }
    return 0;
}

void TabStartup::HandleContextMenu(POINT screenPt) {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    screenPt = ResolveListContextMenuPoint(hList_, sel, screenPt);
    const auto& it = mgr_->Items()[order_[static_cast<size_t>(sel)]];
    const auto& s = GetStrings(settings_->langMode);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | (it.enabled ? MF_GRAYED : 0), IDM_CTX_STARTUP_ENABLE, s.btnEnable);
    AppendMenuW(hMenu, MF_STRING | (!it.enabled ? MF_GRAYED : 0), IDM_CTX_STARTUP_DISABLE, s.btnDisable);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    UINT pathFlags = MF_STRING | (it.exePath.empty() ? MF_GRAYED : 0);
    AppendMenuW(hMenu, pathFlags, IDM_CTX_OPEN_LOCATION, s.ctxOpenLocation);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SEARCH_ONLINE, s.ctxSearchOnline);
    AppendMenuW(hMenu, pathFlags, IDM_CTX_PROPERTIES, s.ctxProperties);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hParent_, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
    case IDM_CTX_STARTUP_ENABLE: SetSelectedEnabled(true); break;
    case IDM_CTX_STARTUP_DISABLE: SetSelectedEnabled(false); break;
    case IDM_CTX_OPEN_LOCATION:
        if (!it.exePath.empty()) OpenFileLocationInExplorer(it.exePath);
        break;
    case IDM_CTX_SEARCH_ONLINE:
        SearchProcessOnline(it.name);
        break;
    case IDM_CTX_PROPERTIES:
        if (!it.exePath.empty()) ShowFilePropertiesDialog(hParent_, it.exePath);
        break;
    }
}

} // namespace taskman
