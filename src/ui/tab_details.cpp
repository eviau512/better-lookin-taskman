#include "tab_details.h"
#include "dialogs.h"
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

const wchar_t* ColTitleForId(int colId, const Strings& s) {
    switch (colId) {
    case DCOL_NAME: return s.colName;
    case DCOL_PID: return s.colPid;
    case DCOL_STATUS: return s.colStatus;
    case DCOL_USER: return s.colUserName;
    case DCOL_CPU: return s.colCpu;
    case DCOL_MEM_PRIVATE: return s.colMemory;
    case DCOL_MEM_WORKING_SET: return s.colWorkingSet;
    case DCOL_MEM_COMMIT: return s.colCommitSize;
    case DCOL_THREADS: return s.colThreads;
    case DCOL_HANDLES: return s.colHandles;
    case DCOL_SESSION_ID: return s.colSessionId;
    case DCOL_PLATFORM: return s.colPlatform;
    case DCOL_ELEVATED: return s.colElevated;
    case DCOL_UAC_VIRT: return s.colUacVirt;
    case DCOL_COMMAND_LINE: return s.colCommandLine;
    case DCOL_DESCRIPTION: return s.colDescription;
    default: return L"";
    }
}

bool ColRightAlign(int colId) {
    switch (colId) {
    case DCOL_PID:
    case DCOL_CPU:
    case DCOL_MEM_PRIVATE:
    case DCOL_MEM_WORKING_SET:
    case DCOL_MEM_COMMIT:
    case DCOL_THREADS:
    case DCOL_HANDLES:
    case DCOL_SESSION_ID:
        return true;
    default:
        return false;
    }
}

} // namespace

void TabDetails::Create(HWND parent, AppSettings* settings) {
    hParent_ = parent;
    settings_ = settings;

    hList_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_CLIPCHILDREN | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100,
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LIST_DETAILS)),
        GetModuleHandleW(nullptr), nullptr);

    themedLv_.Attach(hList_, false);

    ListViewThemeCallbacks cb;
    cb.getCellText = [this](int row, int col) -> std::wstring {
        if (row < 0 || row >= static_cast<int>(procs_.size())) return L"";
        if (col < 0 || col >= static_cast<int>(visibleCols_.size())) return L"";
        const auto& p = procs_[static_cast<size_t>(row)];
        const auto& s = GetStrings(settings_->langMode);
        int cid = visibleCols_[static_cast<size_t>(col)];
        switch (cid) {
        case DCOL_NAME: return p.name;
        case DCOL_PID: return std::to_wstring(p.pid);
        case DCOL_STATUS: return p.isSuspended ? s.statusSuspended : s.statusRunning;
        case DCOL_USER: return p.userName;
        case DCOL_CPU: {
            wchar_t b[32];
            swprintf_s(b, L"%02.0f", p.cpuPercent);
            return b;
        }
        case DCOL_MEM_PRIVATE: return FormatMemoryKB(p.privateWorkingSet);
        case DCOL_MEM_WORKING_SET: return FormatMemoryKB(p.workingSet);
        case DCOL_MEM_COMMIT: return FormatMemoryKB(p.commitSize);
        case DCOL_THREADS: return std::to_wstring(p.threadCount);
        case DCOL_HANDLES: return std::to_wstring(p.handleCount);
        case DCOL_SESSION_ID: return std::to_wstring(p.sessionId);
        case DCOL_PLATFORM: return p.platform;
        case DCOL_ELEVATED: return p.hasElevatedQuery ? (p.isElevated ? s.perfYes : s.perfNo) : L"";
        case DCOL_UAC_VIRT: return p.uacVirtualized ? s.perfEnabled : s.perfDisabled;
        case DCOL_COMMAND_LINE: return p.commandLine;
        case DCOL_DESCRIPTION: return p.description;
        default: return L"";
        }
    };
    cb.getRowIcon = [this](int row) -> HICON {
        if (row < 0 || row >= static_cast<int>(procs_.size())) return nullptr;
        return procs_[static_cast<size_t>(row)].icon;
    };
    cb.getRowIndent = [](int) { return 1; };
    cb.getRowExpandState = [](int) { return -1; };
    cb.isCategoryHeader = [](int) { return false; };
    cb.onHeaderTrack = [this]() {
        SaveColumnWidths();
        if (onColumnsChanged) onColumnsChanged();
    };
    themedLv_.SetCallbacks(std::move(cb));

    RebuildColumns();
    ApplyTheme();
}

void TabDetails::Destroy() {
    SaveColumnWidths();
    themedLv_.Detach();
    if (hList_) { DestroyWindow(hList_); hList_ = nullptr; }
}

void TabDetails::Resize(const RECT& rc) {
    if (hList_) MoveWindow(hList_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void TabDetails::Show(bool visible) {
    if (hList_) ShowWindow(hList_, visible ? SW_SHOW : SW_HIDE);
}

void TabDetails::ApplyTheme() {
    themedLv_.ApplyTheme();
}

bool TabDetails::NeedsCommandLines() const {
    return settings_ && ((settings_->detailsColMask & (1u << DCOL_COMMAND_LINE)) != 0);
}

void TabDetails::SaveColumnWidths() {
    if (!hList_ || !settings_ || visibleCols_.empty()) return;
    ThemedListView::SaveColumnsToArray(
        hList_, visibleCols_.data(), static_cast<int>(visibleCols_.size()),
        settings_->detailsColWidths, DCOL_COUNT);
}

void TabDetails::RebuildColumns() {
    if (!hList_ || !settings_) return;
    SaveColumnWidths();

    int oldCount = Header_GetItemCount(themedLv_.HeaderHwnd());
    for (int i = oldCount - 1; i >= 0; --i) {
        ListView_DeleteColumn(hList_, i);
    }

    settings_->detailsColMask |= (1u << DCOL_NAME);
    const auto& s = GetStrings(settings_->langMode);
    visibleCols_.clear();
    std::vector<HeaderColumnMeta> metas;

    for (int cid = 0; cid < DCOL_COUNT; ++cid) {
        if (!(settings_->detailsColMask & (1u << cid))) continue;
        int colIdx = static_cast<int>(visibleCols_.size());
        visibleCols_.push_back(cid);

        const wchar_t* title = ColTitleForId(cid, s);
        bool right = ColRightAlign(cid);

        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt = right ? LVCFMT_RIGHT : LVCFMT_LEFT;
        lvc.cx = DpiScale(hList_, settings_->detailsColWidths[cid]);
        lvc.pszText = const_cast<LPWSTR>(title);
        ListView_InsertColumn(hList_, colIdx, &lvc);

        HeaderColumnMeta hm;
        hm.title = title;
        hm.rightAlign = right;
        metas.push_back(std::move(hm));
    }

    themedLv_.SetColumnsMeta(std::move(metas));
    int sortVis = -1;
    for (size_t i = 0; i < visibleCols_.size(); ++i) {
        if (visibleCols_[i] == sortColId_) {
            sortVis = static_cast<int>(i);
            break;
        }
    }
    themedLv_.SetSortIndicator(sortVis, sortAsc_);
}

void TabDetails::SortRows() {
    std::sort(procs_.begin(), procs_.end(), [this](const ProcessInfo& a, const ProcessInfo& b) {
        auto cmpU64 = [](uint64_t x, uint64_t y) { return x < y ? -1 : (x > y ? 1 : 0); };
        auto cmpDbl = [](double x, double y) { return x < y ? -1 : (x > y ? 1 : 0); };
        int cmp = 0;
        switch (sortColId_) {
        case DCOL_NAME: cmp = LowerStr(a.name).compare(LowerStr(b.name)); break;
        case DCOL_PID: cmp = cmpU64(a.pid, b.pid); break;
        case DCOL_STATUS: cmp = (a.isSuspended ? 1 : 0) - (b.isSuspended ? 1 : 0); break;
        case DCOL_USER: cmp = LowerStr(a.userName).compare(LowerStr(b.userName)); break;
        case DCOL_CPU: cmp = cmpDbl(a.cpuPercent, b.cpuPercent); break;
        case DCOL_MEM_PRIVATE: cmp = cmpU64(a.privateWorkingSet, b.privateWorkingSet); break;
        case DCOL_MEM_WORKING_SET: cmp = cmpU64(a.workingSet, b.workingSet); break;
        case DCOL_MEM_COMMIT: cmp = cmpU64(a.commitSize, b.commitSize); break;
        case DCOL_THREADS: cmp = cmpU64(a.threadCount, b.threadCount); break;
        case DCOL_HANDLES: cmp = cmpU64(a.handleCount, b.handleCount); break;
        case DCOL_SESSION_ID: cmp = cmpU64(a.sessionId, b.sessionId); break;
        case DCOL_PLATFORM: cmp = a.platform.compare(b.platform); break;
        case DCOL_ELEVATED: cmp = (a.isElevated ? 1 : 0) - (b.isElevated ? 1 : 0); break;
        case DCOL_UAC_VIRT: cmp = (a.uacVirtualized ? 1 : 0) - (b.uacVirtualized ? 1 : 0); break;
        case DCOL_COMMAND_LINE: cmp = LowerStr(a.commandLine).compare(LowerStr(b.commandLine)); break;
        case DCOL_DESCRIPTION: cmp = LowerStr(a.description).compare(LowerStr(b.description)); break;
        }
        if (cmp == 0) cmp = cmpU64(a.pid, b.pid);
        return sortAsc_ ? (cmp < 0) : (cmp > 0);
    });
}

void TabDetails::SetFilter(const std::wstring& query) {
    filterText_ = query;
    std::wstring nextLower = TrimLowerCopy(query);
    if (nextLower == filterLower_) return;
    filterLower_ = std::move(nextLower);
    ApplyFilterAndSort();
}

void TabDetails::RefreshData(const ProcessMonitor& mon) {
    allProcs_ = mon.Processes();
    ApplyFilterAndSort();
}

void TabDetails::ApplyFilterAndSort() {
    if (!hList_) return;

    DWORD selPid = 0;
    bool hadSel = false;
    int selRow = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (selRow >= 0 && selRow < static_cast<int>(procs_.size())) {
        selPid = procs_[static_cast<size_t>(selRow)].pid;
        hadSel = true;
    }

    if (filterLower_.empty()) {
        procs_ = allProcs_;
    } else {
        procs_.clear();
        procs_.reserve(allProcs_.size());
        for (const auto& p : allProcs_) {
            if (ContainsNoCase(p.name, filterLower_) ||
                ContainsNoCase(p.description, filterLower_) ||
                ContainsNoCase(p.userName, filterLower_) ||
                ContainsNoCase(p.commandLine, filterLower_) ||
                ContainsNoCase(p.imagePath, filterLower_) ||
                ContainsNoCase(std::to_wstring(p.pid), filterLower_)) {
                procs_.push_back(p);
            }
        }
    }
    SortRows();
    ListView_SetItemCountEx(hList_, static_cast<int>(procs_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    if (hadSel) {
        int matched = -1;
        for (int i = 0; i < static_cast<int>(procs_.size()); ++i) {
            if (procs_[static_cast<size_t>(i)].pid == selPid) {
                matched = i;
                break;
            }
        }
        if (matched >= 0) {
            if (matched != selRow) {
                ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(hList_, matched, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        } else {
            ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    InvalidateRect(hList_, nullptr, FALSE);
}

bool TabDetails::HasSelection() const {
    if (!hList_) return false;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    return sel >= 0 && sel < static_cast<int>(procs_.size());
}

bool TabDetails::IsSelectedExplorer() const {
    if (!HasSelection()) return false;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    const auto& p = procs_[static_cast<size_t>(sel)];
    return IsExplorerProcess(p.name, p.imagePath);
}

void TabDetails::EndSelectedTask() {
    if (!HasSelection() || IsSelectedExplorer()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    DWORD pid = procs_[static_cast<size_t>(sel)].pid;
    if (!ProcessMonitor::EndTask(pid)) {
        const auto& s = GetStrings(settings_->langMode);
        MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
    }
    if (onActionMinimize) onActionMinimize();
    if (onRefreshNeeded) onRefreshNeeded();
}

void TabDetails::RestartSelectedExplorer() {
    if (!HasSelection() || !IsSelectedExplorer()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    DWORD pid = procs_[static_cast<size_t>(sel)].pid;
    ProcessMonitor::EndTask(pid);
    ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
    if (onRefreshNeeded) onRefreshNeeded();
}

void TabDetails::EndSelectedProcessTree() {
    if (!HasSelection() || IsSelectedExplorer()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    const auto p = procs_[static_cast<size_t>(sel)];
    const auto& s = GetStrings(settings_->langMode);
    if (MessageBoxW(hParent_, s.msgConfirmEndTree, s.appTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
        if (!ProcessMonitor::EndProcessTree(p.pid)) {
            MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
        }
        if (onActionMinimize) onActionMinimize();
        if (onRefreshNeeded) onRefreshNeeded();
    }
}

void TabDetails::OpenSelectedProperties() {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    const auto& p = procs_[static_cast<size_t>(sel)];
    if (!p.imagePath.empty()) ShowFilePropertiesDialog(hParent_, p.imagePath);
}

void TabDetails::SelectProcessByPid(DWORD pid) {
    if (!hList_) return;
    if (!filterLower_.empty()) {
        SetFilter(L"");
    }
    for (int i = 0; i < static_cast<int>(procs_.size()); ++i) {
        if (procs_[static_cast<size_t>(i)].pid == pid) {
            ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(hList_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList_, i, FALSE);
            SetFocus(hList_);
            break;
        }
    }
}

void TabDetails::HandleKeyDown(WPARAM vk) {
    if (vk == VK_DELETE) EndSelectedTask();
}

LRESULT TabDetails::HandleNotify(NMHDR* nm) {
    if (!nm) return 0;
    if (nm->hwndFrom == themedLv_.HeaderHwnd()) {
        if (nm->code == NM_RCLICK) {
            POINT pt{};
            GetCursorPos(&pt);
            HandleHeaderContextMenu(pt);
            return 0;
        }
        if (ThemedListView::IsHeaderTrackNotify(nm, themedLv_.HeaderHwnd())) {
            SaveColumnWidths();
            if (onColumnsChanged) onColumnsChanged();
            return 0;
        }
    }
    if (nm->idFrom == IDC_LIST_DETAILS) {
        if (nm->code == NM_CUSTOMDRAW) {
            return themedLv_.HandleCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
        }
        if (nm->code == LVN_ITEMCHANGED) {
            if (onSelectionChanged) onSelectionChanged();
            return 0;
        }
        if (nm->code == LVN_COLUMNCLICK) {
            auto* lvn = reinterpret_cast<NMLISTVIEW*>(nm);
            if (lvn->iSubItem >= 0 && lvn->iSubItem < static_cast<int>(visibleCols_.size())) {
                int cid = visibleCols_[static_cast<size_t>(lvn->iSubItem)];
                if (sortColId_ == cid) sortAsc_ = !sortAsc_;
                else {
                    sortColId_ = cid;
                    sortAsc_ = (cid == DCOL_NAME || cid == DCOL_PID || cid == DCOL_USER);
                }
                themedLv_.SetSortIndicator(lvn->iSubItem, sortAsc_);
                SortRows();
                InvalidateRect(hList_, nullptr, FALSE);
            }
            return 0;
        }
        if (nm->code == NM_RCLICK) {
            POINT pt = HandleListRightClickSelection(hList_, nm, static_cast<int>(procs_.size()));
            HandleContextMenu(pt);
            return 0;
        }
        if (nm->code == LVN_KEYDOWN) {
            auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
            HandleKeyDown(kd->wVKey);
            return 0;
        }
    }
    return 0;
}

void TabDetails::HandleHeaderContextMenu(POINT screenPt) {
    const auto& s = GetStrings(settings_->langMode);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_COL_BASE, s.ctxSelectColumns);
    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hParent_, nullptr);
    DestroyMenu(hMenu);
    if (cmd == IDM_CTX_COL_BASE) {
        if (ShowSelectColumnsDialog(hParent_, *settings_)) {
            RebuildColumns();
            if (onRefreshNeeded) onRefreshNeeded();
        }
    }
}

void TabDetails::HandleContextMenu(POINT screenPt) {
    if (!HasSelection()) return;
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    screenPt = ResolveListContextMenuPoint(hList_, sel, screenPt);
    const auto p = procs_[static_cast<size_t>(sel)];
    const auto& s = GetStrings(settings_->langMode);

    DWORD curPrio = ProcessMonitor::GetPriority(p.pid);
    HMENU hPrio = CreatePopupMenu();
    struct PrioOpt { DWORD cls; UINT id; const wchar_t* label; } opts[6] = {
        { REALTIME_PRIORITY_CLASS,     IDM_CTX_PRIO_REALTIME,     s.ctxPrioRealtime },
        { HIGH_PRIORITY_CLASS,         IDM_CTX_PRIO_HIGH,         s.ctxPrioHigh },
        { ABOVE_NORMAL_PRIORITY_CLASS, IDM_CTX_PRIO_ABOVE_NORMAL, s.ctxPrioAboveNormal },
        { NORMAL_PRIORITY_CLASS,       IDM_CTX_PRIO_NORMAL,       s.ctxPrioNormal },
        { BELOW_NORMAL_PRIORITY_CLASS, IDM_CTX_PRIO_BELOW_NORMAL, s.ctxPrioBelowNormal },
        { IDLE_PRIORITY_CLASS,         IDM_CTX_PRIO_LOW,          s.ctxPrioLow }
    };
    for (const auto& o : opts) {
        AppendMenuW(hPrio, MF_STRING | (curPrio == o.cls ? MF_CHECKED : 0), o.id, o.label);
    }

    const bool isExplorer = IsExplorerProcess(p.name, p.imagePath);
    HMENU hMenu = CreatePopupMenu();
    if (isExplorer) {
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_RESTART_EXPLORER, s.ctxRestart);
    } else {
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_END_TASK, s.ctxEndTask);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_END_TREE, s.ctxEndTree);
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hPrio), s.ctxSetPriority);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SET_AFFINITY, s.ctxSetAffinity);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_CREATE_DUMP, s.ctxCreateDump);
    UINT pathFlags = MF_STRING | (p.imagePath.empty() ? MF_GRAYED : 0);
    AppendMenuW(hMenu, pathFlags, IDM_CTX_OPEN_LOCATION, s.ctxOpenLocation);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_GO_TO_SERVICE, s.ctxGoToService);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SEARCH_ONLINE, s.ctxSearchOnline);
    AppendMenuW(hMenu, pathFlags, IDM_CTX_PROPERTIES, s.ctxProperties);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hParent_, nullptr);
    DestroyMenu(hMenu);

    if (cmd == IDM_CTX_RESTART_EXPLORER) {
        RestartSelectedExplorer();
    } else if (cmd == IDM_CTX_END_TASK) {
        EndSelectedTask();
    } else if (cmd == IDM_CTX_END_TREE) {
        EndSelectedProcessTree();
    } else if (cmd == IDM_CTX_SET_AFFINITY) {
        ShowAffinityDialog(hParent_, p.pid, p.name, *settings_);
    } else if (cmd == IDM_CTX_CREATE_DUMP) {
        std::wstring outPath;
        if (ProcessMonitor::CreateDumpFile(p.pid, p.name, outPath)) {
            std::wstring msg = std::wstring(s.msgDumpSaved) + L"\n\n" + outPath;
            MessageBoxW(hParent_, msg.c_str(), s.appTitle, MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(hParent_, s.msgDumpFailed, s.appTitle, MB_OK | MB_ICONWARNING);
        }
    } else if (cmd == IDM_CTX_OPEN_LOCATION && !p.imagePath.empty()) {
        OpenFileLocationInExplorer(p.imagePath);
    } else if (cmd == IDM_CTX_GO_TO_SERVICE) {
        if (onGoToService) onGoToService(p.pid);
    } else if (cmd == IDM_CTX_SEARCH_ONLINE) {
        SearchProcessOnline(p.name);
    } else if (cmd == IDM_CTX_PROPERTIES && !p.imagePath.empty()) {
        ShowFilePropertiesDialog(hParent_, p.imagePath);
    } else if (cmd >= IDM_CTX_PRIO_REALTIME && cmd <= IDM_CTX_PRIO_LOW) {
        DWORD targetCls = opts[cmd - IDM_CTX_PRIO_REALTIME].cls;
        if (targetCls == REALTIME_PRIORITY_CLASS) {
            if (MessageBoxW(hParent_, s.msgRealtimeWarn, s.appTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return;
            }
        }
        if (!ProcessMonitor::SetPriority(p.pid, targetCls)) {
            MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_OK | MB_ICONWARNING);
        }
    }
}

} // namespace taskman
