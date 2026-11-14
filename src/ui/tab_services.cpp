#include "tab_services.h"
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

const wchar_t* ServiceStateText(DWORD st, const Strings& s) {
    switch (st) {
    case SERVICE_RUNNING: return s.statusRunning;
    case SERVICE_STOPPED: return s.svcStopped;
    case SERVICE_PAUSED:  return s.statusSuspended;
    case SERVICE_START_PENDING:
    case SERVICE_STOP_PENDING:
    case SERVICE_CONTINUE_PENDING:
    case SERVICE_PAUSE_PENDING:
        return L"...";
    default: return s.svcStopped;
    }
}

} // namespace

void TabServices::Create(HWND parent, AppSettings* settings) {
    hParent_ = parent;
    settings_ = settings;

    hList_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_CLIPCHILDREN | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100,
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LIST_SERVICES)),
        GetModuleHandleW(nullptr), nullptr);

    themedLv_.Attach(hList_, false);

    ListViewThemeCallbacks cb;
    cb.getCellText = [this](int row, int col) -> std::wstring {
        if (row < 0 || row >= static_cast<int>(items_.size())) return L"";
        const auto& it = items_[static_cast<size_t>(row)];
        const auto& s = GetStrings(settings_->langMode);
        switch (col) {
        case 0: return it.name;
        case 1: return it.pid != 0 ? std::to_wstring(it.pid) : L"";
        case 2: return !it.displayName.empty() ? it.displayName : it.description;
        case 3: return ServiceStateText(it.currentState, s);
        case 4: return it.group;
        default: return L"";
        }
    };
    cb.getRowIndent = [](int) { return 1; };
    cb.getRowExpandState = [](int) { return -1; };
    cb.isCategoryHeader = [](int) { return false; };
    cb.getCellTextColor = [this](int row, int col, bool& overrideColor) -> COLORREF {
        overrideColor = false;
        if (col == 3 && row >= 0 && row < static_cast<int>(items_.size())) {
            if (items_[static_cast<size_t>(row)].currentState == SERVICE_STOPPED) {
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

void TabServices::Destroy() {
    SaveColumnWidths();
    themedLv_.Detach();
    if (hList_) { DestroyWindow(hList_); hList_ = nullptr; }
}

void TabServices::Resize(const RECT& rc) {
    if (hList_) MoveWindow(hList_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void TabServices::Show(bool visible) {
    if (hList_) ShowWindow(hList_, visible ? SW_SHOW : SW_HIDE);
}

void TabServices::ApplyTheme() {
    themedLv_.ApplyTheme();
}

void TabServices::SaveColumnWidths() {
    if (!hList_ || !settings_) return;
    ThemedListView::SaveColumnsToArray(hList_, nullptr, 5, settings_->servicesColWidths, 5);
}

void TabServices::RebuildColumns() {
    if (!hList_ || !settings_) return;
    SaveColumnWidths();
    int oldCount = Header_GetItemCount(themedLv_.HeaderHwnd());
    for (int i = oldCount - 1; i >= 0; --i) {
        ListView_DeleteColumn(hList_, i);
    }

    const auto& s = GetStrings(settings_->langMode);
    struct ColDef { const wchar_t* title; int width; bool right; } cols[5] = {
        { s.colName, settings_->servicesColWidths[0], false },
        { s.colPid, settings_->servicesColWidths[1], true },
        { s.colDescription, settings_->servicesColWidths[2], false },
        { s.colStatus, settings_->servicesColWidths[3], false },
        { s.colGroup, settings_->servicesColWidths[4], false }
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
}

void TabServices::SortRows() {
    std::sort(items_.begin(), items_.end(), [this](const ServiceItem& a, const ServiceItem& b) {
        int cmp = 0;
        switch (sortCol_) {
        case 0: cmp = LowerStr(a.name).compare(LowerStr(b.name)); break;
        case 1: cmp = (a.pid < b.pid) ? -1 : (a.pid > b.pid ? 1 : 0); break;
        case 2: {
            const auto& da = !a.displayName.empty() ? a.displayName : a.description;
            const auto& db = !b.displayName.empty() ? b.displayName : b.description;
            cmp = LowerStr(da).compare(LowerStr(db));
            break;
        }
        case 3: cmp = (a.currentState < b.currentState) ? -1 : (a.currentState > b.currentState ? 1 : 0); break;
        case 4: cmp = LowerStr(a.group).compare(LowerStr(b.group)); break;
        }
        if (cmp == 0) cmp = LowerStr(a.name).compare(LowerStr(b.name));
        return sortAsc_ ? (cmp < 0) : (cmp > 0);
    });
}

void TabServices::RefreshData(const ServiceManager& mgr) {
    if (!hList_) return;

    std::wstring selName;
    int selRow = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (selRow >= 0 && selRow < static_cast<int>(items_.size())) {
        selName = items_[static_cast<size_t>(selRow)].name;
    }

    items_ = mgr.Items();
    SortRows();
    ListView_SetItemCountEx(hList_, static_cast<int>(items_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    if (!selName.empty()) {
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (items_[static_cast<size_t>(i)].name == selName) {
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

bool TabServices::HasSelection() const {
    if (!hList_) return false;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    return sel >= 0 && sel < static_cast<int>(items_.size());
}

void TabServices::StartSelected() {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    DWORD err = 0;
    if (!ServiceManager::StartServiceByName(items_[static_cast<size_t>(sel)].name, err)) {
        const auto& s = GetStrings(settings_->langMode);
        MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
    }
    if (onRefreshNeeded) onRefreshNeeded();
}

void TabServices::StopSelected() {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    DWORD err = 0;
    if (!ServiceManager::StopServiceByName(items_[static_cast<size_t>(sel)].name, err)) {
        const auto& s = GetStrings(settings_->langMode);
        MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
    }
    if (onRefreshNeeded) onRefreshNeeded();
}

void TabServices::RestartSelected() {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    DWORD err = 0;
    if (!ServiceManager::RestartServiceByName(items_[static_cast<size_t>(sel)].name, err)) {
        const auto& s = GetStrings(settings_->langMode);
        MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
    }
    if (onRefreshNeeded) onRefreshNeeded();
}

void TabServices::OpenServicesMsc() {
    ShellExecuteW(hParent_, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL);
}

void TabServices::SelectServiceByPid(DWORD pid) {
    if (!hList_ || pid == 0) return;
    ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (items_[static_cast<size_t>(i)].pid == pid) {
            ListView_SetItemState(hList_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList_, i, FALSE);
            SetFocus(hList_);
            break;
        }
    }
}

LRESULT TabServices::HandleNotify(NMHDR* nm) {
    if (!nm) return 0;
    if (ThemedListView::IsHeaderTrackNotify(nm, themedLv_.HeaderHwnd())) {
        SaveColumnWidths();
        if (onColumnsChanged) onColumnsChanged();
        return 0;
    }
    if (nm->idFrom != IDC_LIST_SERVICES) return 0;
    if (nm->code == NM_CUSTOMDRAW) {
        return themedLv_.HandleCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
    }
    if (nm->code == LVN_COLUMNCLICK) {
        auto* lvn = reinterpret_cast<NMLISTVIEW*>(nm);
        if (sortCol_ == lvn->iSubItem) sortAsc_ = !sortAsc_;
        else {
            sortCol_ = lvn->iSubItem;
            sortAsc_ = (sortCol_ != 1);
        }
        themedLv_.SetSortIndicator(sortCol_, sortAsc_);
        SortRows();
        InvalidateRect(hList_, nullptr, FALSE);
        return 0;
    }
    if (nm->code == NM_RCLICK) {
        POINT pt = HandleListRightClickSelection(hList_, nm, static_cast<int>(items_.size()));
        HandleContextMenu(pt);
        return 0;
    }
    return 0;
}

void TabServices::HandleContextMenu(POINT screenPt) {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    screenPt = ResolveListContextMenuPoint(hList_, sel, screenPt);
    const auto it = items_[static_cast<size_t>(sel)];
    const auto& s = GetStrings(settings_->langMode);

    bool isRunning = (it.currentState == SERVICE_RUNNING);
    bool isStopped = (it.currentState == SERVICE_STOPPED);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | (isStopped ? 0 : MF_GRAYED), IDM_CTX_SVC_START, s.svcStart);
    AppendMenuW(hMenu, MF_STRING | (isRunning ? 0 : MF_GRAYED), IDM_CTX_SVC_STOP, s.svcStop);
    AppendMenuW(hMenu, MF_STRING | (isRunning ? 0 : MF_GRAYED), IDM_CTX_SVC_RESTART, s.svcRestart);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SVC_OPEN_SERVICES, s.svcOpenServices);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SEARCH_ONLINE, s.ctxSearchOnline);
    AppendMenuW(hMenu, MF_STRING | (it.pid == 0 ? MF_GRAYED : 0), IDM_CTX_GO_TO_DETAILS, s.ctxGoToDetails);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hParent_, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
    case IDM_CTX_SVC_START: StartSelected(); break;
    case IDM_CTX_SVC_STOP: StopSelected(); break;
    case IDM_CTX_SVC_RESTART: RestartSelected(); break;
    case IDM_CTX_SVC_OPEN_SERVICES: OpenServicesMsc(); break;
    case IDM_CTX_SEARCH_ONLINE: SearchProcessOnline(it.name + L" windows service"); break;
    case IDM_CTX_GO_TO_DETAILS:
        if (it.pid != 0 && onGoToDetails) onGoToDetails(it.pid);
        break;
    }
}

} // namespace taskman
