#include "tab_users.h"
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

int HeatLevel(double pct) {
    if (pct >= 75.0) return 4;
    if (pct >= 45.0) return 3;
    if (pct >= 18.0) return 2;
    if (pct >= 1.5)  return 1;
    return 0;
}

} // namespace

void TabUsers::Create(HWND parent, AppSettings* settings) {
    hParent_ = parent;
    settings_ = settings;

    hList_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_CLIPCHILDREN | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100,
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LIST_USERS)),
        GetModuleHandleW(nullptr), nullptr);

    themedLv_.Attach(hList_, false);

    ListViewThemeCallbacks cb;
    cb.getCellText = [this](int row, int col) -> std::wstring {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return L"";
        const auto& r = rows_[static_cast<size_t>(row)];
        switch (col) {
        case 0: return r.title;
        case 1: return r.status;
        case 2: return FormatPercent(r.cpuPct, 1);
        case 3:
            if (settings_->memShowPercent && totalMemBytes_ > 0) {
                return FormatPercent(100.0 * static_cast<double>(r.memBytes) / static_cast<double>(totalMemBytes_), 1);
            }
            return FormatMemoryMB(r.memBytes);
        case 4: return FormatDiskSpeed(r.diskBps);
        case 5: return FormatNetSpeedMbps(r.netBps);
        default: return L"";
        }
    };
    cb.getRowIcon = [this](int row) -> HICON {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        return rows_[static_cast<size_t>(row)].icon;
    };
    cb.getRowIndent = [this](int row) -> int {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return 1;
        return rows_[static_cast<size_t>(row)].isSession ? 1 : 2;
    };
    cb.getRowExpandState = [this](int row) -> int {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
        const auto& r = rows_[static_cast<size_t>(row)];
        return r.isSession ? (r.expanded ? 1 : 0) : -1;
    };
    cb.isCategoryHeader = [](int) { return false; };
    cb.getCellHeat = [this](int row, int col) -> int {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return 0;
        const auto& r = rows_[static_cast<size_t>(row)];
        if (col == 2) return HeatLevel(r.cpuPct);
        if (col == 3 && totalMemBytes_ > 0) {
            return HeatLevel(100.0 * static_cast<double>(r.memBytes) / static_cast<double>(totalMemBytes_));
        }
        if (col == 4) return HeatLevel((r.diskBps / (50.0 * 1024.0 * 1024.0)) * 100.0);
        if (col == 5) return HeatLevel(((r.netBps * 8.0) / 100000000.0) * 100.0);
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

void TabUsers::Destroy() {
    SaveColumnWidths();
    themedLv_.Detach();
    if (hList_) { DestroyWindow(hList_); hList_ = nullptr; }
}

void TabUsers::Resize(const RECT& rc) {
    if (hList_) MoveWindow(hList_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void TabUsers::Show(bool visible) {
    if (hList_) ShowWindow(hList_, visible ? SW_SHOW : SW_HIDE);
}

void TabUsers::ApplyTheme() {
    themedLv_.ApplyTheme();
}

void TabUsers::SaveColumnWidths() {
    if (!hList_ || !settings_) return;
    ThemedListView::SaveColumnsToArray(hList_, nullptr, 6, settings_->usersColWidths, 6);
}

void TabUsers::RebuildColumns() {
    if (!hList_ || !settings_) return;
    SaveColumnWidths();
    int oldCount = Header_GetItemCount(themedLv_.HeaderHwnd());
    for (int i = oldCount - 1; i >= 0; --i) {
        ListView_DeleteColumn(hList_, i);
    }

    const auto& s = GetStrings(settings_->langMode);
    struct ColDef { const wchar_t* title; int width; bool right; } cols[6] = {
        { s.colUser, settings_->usersColWidths[0], false },
        { s.colStatus, settings_->usersColWidths[1], false },
        { s.colCpu, settings_->usersColWidths[2], true },
        { s.colMemory, settings_->usersColWidths[3], true },
        { s.colDisk, settings_->usersColWidths[4], true },
        { s.colNetwork, settings_->usersColWidths[5], true }
    };

    std::vector<HeaderColumnMeta> metas;
    for (int i = 0; i < 6; ++i) {
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

void TabUsers::RebuildRows(const UserManager& mgr) {
    rows_.clear();
    const auto& s = GetStrings(settings_->langMode);
    std::vector<const UserSessionInfo*> sessions;
    for (const auto& u : mgr.Sessions()) sessions.push_back(&u);

    std::sort(sessions.begin(), sessions.end(), [this](const UserSessionInfo* a, const UserSessionInfo* b) {
        int cmp = 0;
        switch (sortCol_) {
        case 0: cmp = LowerStr(a->fullUser).compare(LowerStr(b->fullUser)); break;
        case 1: cmp = (a->active ? 0 : 1) - (b->active ? 0 : 1); break;
        case 2: cmp = (a->cpuPercent < b->cpuPercent) ? -1 : (a->cpuPercent > b->cpuPercent ? 1 : 0); break;
        case 3: cmp = (a->memoryBytes < b->memoryBytes) ? -1 : (a->memoryBytes > b->memoryBytes ? 1 : 0); break;
        case 4: cmp = (a->diskBytesPerSec < b->diskBytesPerSec) ? -1 : (a->diskBytesPerSec > b->diskBytesPerSec ? 1 : 0); break;
        case 5: cmp = (a->netBytesPerSec < b->netBytesPerSec) ? -1 : (a->netBytesPerSec > b->netBytesPerSec ? 1 : 0); break;
        }
        if (cmp == 0) cmp = (a->sessionId < b->sessionId) ? -1 : 1;
        return sortAsc_ ? (cmp < 0) : (cmp > 0);
    });

    for (const auto* u : sessions) {
        bool exp = expandedSessions_.count(u->sessionId) != 0;
        DisplayRow parentRow;
        parentRow.isSession = true;
        parentRow.sessionId = u->sessionId;
        parentRow.active = u->active;
        parentRow.expanded = exp;
        parentRow.icon = u->avatarIcon;
        parentRow.title = u->fullUser;
        if (u->processes.size() > 0) {
            parentRow.title += L" (" + std::to_wstring(u->processes.size()) + L")";
        }
        parentRow.status = u->active ? s.stateActive : s.stateDisconnected;
        parentRow.cpuPct = u->cpuPercent;
        parentRow.memBytes = u->memoryBytes;
        parentRow.diskBps = u->diskBytesPerSec;
        parentRow.netBps = u->netBytesPerSec;
        rows_.push_back(std::move(parentRow));

        if (exp) {
            std::vector<const ProcessInfo*> children = u->processes;
            std::sort(children.begin(), children.end(), [this](const ProcessInfo* a, const ProcessInfo* b) {
                int cmp = 0;
                switch (sortCol_) {
                case 0: cmp = LowerStr(a->name).compare(LowerStr(b->name)); break;
                case 2: cmp = (a->cpuPercent < b->cpuPercent) ? -1 : (a->cpuPercent > b->cpuPercent ? 1 : 0); break;
                case 3: cmp = (a->privateWorkingSet < b->privateWorkingSet) ? -1 : (a->privateWorkingSet > b->privateWorkingSet ? 1 : 0); break;
                case 4: cmp = (a->diskBytesPerSec < b->diskBytesPerSec) ? -1 : (a->diskBytesPerSec > b->diskBytesPerSec ? 1 : 0); break;
                case 5: cmp = (a->netBytesPerSec < b->netBytesPerSec) ? -1 : (a->netBytesPerSec > b->netBytesPerSec ? 1 : 0); break;
                }
                if (cmp == 0) cmp = LowerStr(a->name).compare(LowerStr(b->name));
                return sortAsc_ ? (cmp < 0) : (cmp > 0);
            });

            for (const auto* p : children) {
                DisplayRow child;
                child.isSession = false;
                child.sessionId = u->sessionId;
                child.pid = p->pid;
                child.icon = p->icon;
                child.title = !p->description.empty() ? p->description : p->name;
                child.status = p->isSuspended ? s.statusSuspended : L"";
                child.imagePath = p->imagePath;
                child.cpuPct = p->cpuPercent;
                child.memBytes = p->privateWorkingSet;
                child.diskBps = p->diskBytesPerSec;
                child.netBps = p->netBytesPerSec;
                rows_.push_back(std::move(child));
            }
        }
    }
}

void TabUsers::RefreshData(const UserManager& mgr, uint64_t totalMemBytes) {
    lastMgr_ = &mgr;
    totalMemBytes_ = std::max<uint64_t>(1, totalMemBytes);
    if (!hList_) return;

    DWORD selSid = 0;
    DWORD selPid = 0;
    bool hadSel = false;
    int selRow = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (selRow >= 0 && selRow < static_cast<int>(rows_.size())) {
        hadSel = true;
        selSid = rows_[static_cast<size_t>(selRow)].sessionId;
        selPid = rows_[static_cast<size_t>(selRow)].pid;
    }

    RebuildRows(mgr);
    ListView_SetItemCountEx(hList_, static_cast<int>(rows_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    if (hadSel) {
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const auto& r = rows_[static_cast<size_t>(i)];
            if ((selPid == 0 && r.isSession && r.sessionId == selSid) ||
                (selPid != 0 && !r.isSession && r.pid == selPid)) {
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

bool TabUsers::HasSelection() const {
    if (!hList_) return false;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    return sel >= 0 && sel < static_cast<int>(rows_.size());
}

bool TabUsers::IsUserSelected() const {
    if (!HasSelection()) return false;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    return rows_[static_cast<size_t>(sel)].isSession;
}

void TabUsers::DisconnectSelected() {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    DWORD sid = rows_[static_cast<size_t>(sel)].sessionId;
    const auto& s = GetStrings(settings_->langMode);
    if (MessageBoxW(hParent_, s.msgConfirmDisconnect, s.appTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
        if (!UserManager::DisconnectSession(sid)) {
            MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
        }
        if (onRefreshNeeded) onRefreshNeeded();
    }
}

void TabUsers::ExpandAll(bool expand) {
    if (!lastMgr_) return;
    if (expand) {
        for (const auto& u : lastMgr_->Sessions()) expandedSessions_.insert(u.sessionId);
    } else {
        expandedSessions_.clear();
    }
    RefreshData(*lastMgr_, totalMemBytes_);
}

void TabUsers::OpenSelectedProperties() {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    const auto& r = rows_[static_cast<size_t>(sel)];
    if (!r.isSession && !r.imagePath.empty()) {
        ShowFilePropertiesDialog(hParent_, r.imagePath);
    }
}

LRESULT TabUsers::HandleNotify(NMHDR* nm) {
    if (!nm) return 0;
    if (ThemedListView::IsHeaderTrackNotify(nm, themedLv_.HeaderHwnd())) {
        SaveColumnWidths();
        if (onColumnsChanged) onColumnsChanged();
        return 0;
    }
    if (nm->idFrom != IDC_LIST_USERS) return 0;
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
            sortAsc_ = (sortCol_ <= 1);
        }
        themedLv_.SetSortIndicator(sortCol_, sortAsc_);
        if (lastMgr_) RefreshData(*lastMgr_, totalMemBytes_);
        return 0;
    }
    if (nm->code == NM_CLICK || nm->code == NM_DBLCLK) {
        auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(nm);
        if (nmia->iItem >= 0 && nmia->iItem < static_cast<int>(rows_.size())) {
            const auto& r = rows_[static_cast<size_t>(nmia->iItem)];
            if (r.isSession && (nm->code == NM_DBLCLK || themedLv_.HitTestChevron(nmia->iItem, nmia->ptAction))) {
                if (expandedSessions_.count(r.sessionId)) expandedSessions_.erase(r.sessionId);
                else expandedSessions_.insert(r.sessionId);
                if (lastMgr_) RefreshData(*lastMgr_, totalMemBytes_);
            }
        }
        return 0;
    }
    if (nm->code == NM_RCLICK) {
        POINT pt = HandleListRightClickSelection(hList_, nm, static_cast<int>(rows_.size()));
        HandleContextMenu(pt);
        return 0;
    }
    return 0;
}

void TabUsers::HandleContextMenu(POINT screenPt) {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    screenPt = ResolveListContextMenuPoint(hList_, sel, screenPt);
    const auto r = rows_[static_cast<size_t>(sel)];
    const auto& s = GetStrings(settings_->langMode);

    HWND switchWnd = r.isSession ? nullptr : FindMainWindowForPid(r.pid);
    HMENU hMenu = CreatePopupMenu();
    if (r.isSession) {
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_EXPAND_COLLAPSE, r.expanded ? s.ctxCollapse : s.ctxExpand);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_USER_DISCONNECT, s.ctxDisconnect);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_USER_LOGOFF, s.ctxLogoff);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_USER_EXPAND, s.menuExpandAll);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_USER_COLLAPSE, s.menuCollapseAll);
    } else {
        if (switchWnd) {
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_SWITCH_TO, s.ctxSwitchTo);
        }
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_END_TASK, s.ctxEndTask);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_GO_TO_DETAILS, s.ctxGoToDetails);
        UINT pFlags = MF_STRING | (r.imagePath.empty() ? MF_GRAYED : 0);
        AppendMenuW(hMenu, pFlags, IDM_CTX_OPEN_LOCATION, s.ctxOpenLocation);
        AppendMenuW(hMenu, pFlags, IDM_CTX_PROPERTIES, s.ctxProperties);
    }

    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hParent_, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
    case IDM_CTX_SWITCH_TO:
        BringWindowToFront(switchWnd);
        break;
    case IDM_CTX_EXPAND_COLLAPSE:
        if (expandedSessions_.count(r.sessionId)) expandedSessions_.erase(r.sessionId);
        else expandedSessions_.insert(r.sessionId);
        if (lastMgr_) RefreshData(*lastMgr_, totalMemBytes_);
        break;
    case IDM_CTX_USER_DISCONNECT:
        DisconnectSelected();
        break;
    case IDM_CTX_USER_LOGOFF:
        if (MessageBoxW(hParent_, s.msgConfirmLogoff, s.appTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            if (!UserManager::LogoffSession(r.sessionId)) {
                MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
            }
            if (onRefreshNeeded) onRefreshNeeded();
        }
        break;
    case IDM_CTX_USER_EXPAND:
        ExpandAll(true);
        break;
    case IDM_CTX_USER_COLLAPSE:
        ExpandAll(false);
        break;
    case IDM_CTX_END_TASK:
        ProcessMonitor::EndTask(r.pid);
        if (onRefreshNeeded) onRefreshNeeded();
        break;
    case IDM_CTX_GO_TO_DETAILS:
        if (onGoToDetails) onGoToDetails(r.pid);
        break;
    case IDM_CTX_OPEN_LOCATION:
        if (!r.imagePath.empty()) OpenFileLocationInExplorer(r.imagePath);
        break;
    case IDM_CTX_PROPERTIES:
        if (!r.imagePath.empty()) ShowFilePropertiesDialog(hParent_, r.imagePath);
        break;
    }
}

} // namespace taskman
