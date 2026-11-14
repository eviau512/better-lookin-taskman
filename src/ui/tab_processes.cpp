#include "tab_processes.h"
#include "../resource.h"
#include "../core/i18n.h"
#include "../core/theme.h"
#include "../core/format_util.h"
#include <algorithm>
#include <cwctype>
#include <unordered_map>

namespace taskman {

static std::wstring LowerKey(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

static int ComputeHeatLevel(double pct) {
    if (pct >= 75.0) return 4;
    if (pct >= 45.0) return 3;
    if (pct >= 18.0) return 2;
    if (pct >= 1.5)  return 1;
    return 0;
}

void TabProcesses::Create(HWND parent, AppSettings* settings) {
    hParent_ = parent;
    settings_ = settings;
    colVisMask_ = settings_ ? (settings_->procColMask | 1u) : 0x3FFu;
    sortCol_ = 0;
    sortAsc_ = true;

    hList_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_CLIPCHILDREN | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LIST_PROCESSES)),
        GetModuleHandleW(nullptr), nullptr);

    themedLv_.Attach(hList_, true);

    ListViewThemeCallbacks cb;
    cb.isCategoryHeader = [this](int row) -> bool {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
        return rows_[static_cast<size_t>(row)].kind == DisplayRow::CategoryHeader;
    };
    cb.isExpandedGroupRow = [this](int row) -> bool {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
        const auto& r = rows_[static_cast<size_t>(row)];
        if (r.kind == DisplayRow::CategoryHeader) return false;
        if (r.kind == DisplayRow::GroupParent) return r.expanded;
        return !r.groupKey.empty();
    };
    cb.getRowIndent = [this](int row) -> int {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return 1;
        const auto& r = rows_[static_cast<size_t>(row)];
        if (r.kind == DisplayRow::CategoryHeader) return 0;
        if (r.kind == DisplayRow::SingleOrChild && !r.groupKey.empty() && r.childCount == 0) return 2;
        return 1;
    };
    cb.getRowExpandState = [this](int row) -> int {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return -1;
        const auto& r = rows_[static_cast<size_t>(row)];
        if (r.kind == DisplayRow::CategoryHeader || r.kind == DisplayRow::GroupParent) {
            return r.expanded ? 1 : 0;
        }
        return -1;
    };
    cb.getRowIcon = [this](int row) -> HICON {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        return rows_[static_cast<size_t>(row)].icon;
    };
    cb.getCellText = [this](int row, int col) -> std::wstring {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return L"";
        if (col < 0 || col >= static_cast<int>(visibleCols_.size())) return L"";
        const auto& r = rows_[static_cast<size_t>(row)];
        if (r.kind == DisplayRow::CategoryHeader) return (col == 0) ? r.title : L"";
        int logicalCol = visibleCols_[static_cast<size_t>(col)];
        const auto& str = GetStrings(settings_->langMode);
        switch (logicalCol) {
        case 0: return r.title;
        case 1: return r.statusText;
        case 2: {
            wchar_t b[32];
            swprintf_s(b, L"%.1f%%", r.cpuPct);
            return b;
        }
        case 3:
            if (settings_->memShowPercent && totalMemBytes_ > 0) {
                return FormatPercent(100.0 * static_cast<double>(r.memBytes) / static_cast<double>(totalMemBytes_), 1);
            }
            return FormatMemoryMB(r.memBytes);
        case 4:
            if (settings_->diskShowPercent) {
                double pct = (std::min)(100.0, r.diskBps / (100.0 * 1024.0 * 1024.0) * 100.0);
                return FormatPercent(pct, 0);
            }
            return FormatDiskSpeed(r.diskBps);
        case 5:
            if (settings_->netShowPercent) {
                double pct = (std::min)(100.0, (r.netBps * 8.0) / 1000000000.0 * 100.0);
                return FormatPercent(pct, 0);
            }
            return FormatNetSpeedMbps(r.netBps);
        case 6: return FormatPercent(r.gpuPct, 1);
        case 7: return r.gpuEngine;
        case 8: {
            const wchar_t* lvl[] = { str.powerVeryLow, str.powerLow, str.powerModerate, str.powerHigh, str.powerVeryHigh };
            return lvl[(std::min)(4, (std::max)(0, r.powerUsage))];
        }
        case 9: {
            const wchar_t* lvl[] = { str.powerVeryLow, str.powerLow, str.powerModerate, str.powerHigh, str.powerVeryHigh };
            return lvl[(std::min)(4, (std::max)(0, r.powerTrend))];
        }
        }
        return L"";
    };
    cb.getCellHeat = [this](int row, int col) -> int {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return 0;
        if (col < 0 || col >= static_cast<int>(visibleCols_.size())) return 0;
        const auto& r = rows_[static_cast<size_t>(row)];
        if (r.kind == DisplayRow::CategoryHeader) return 0;
        int logicalCol = visibleCols_[static_cast<size_t>(col)];
        switch (logicalCol) {
        case 2: return ComputeHeatLevel(r.cpuPct);
        case 3: {
            double pct = totalMemBytes_ > 0 ? (100.0 * static_cast<double>(r.memBytes) / static_cast<double>(totalMemBytes_)) : 0.0;
            return ComputeHeatLevel(pct * 1.5);
        }
        case 4: {
            double mbps = r.diskBps / (1024.0 * 1024.0);
            if (mbps >= 50.0) return 4;
            if (mbps >= 15.0) return 3;
            if (mbps >= 2.0) return 2;
            if (mbps >= 0.2) return 1;
            return 0;
        }
        case 5: {
            double mbps = (r.netBps * 8.0) / 1000000.0;
            if (mbps >= 50.0) return 4;
            if (mbps >= 10.0) return 3;
            if (mbps >= 1.5) return 2;
            if (mbps >= 0.1) return 1;
            return 0;
        }
        case 6: return ComputeHeatLevel(r.gpuPct);
        case 8: return r.powerUsage;
        case 9: return r.powerTrend;
        }
        return 0;
    };
    cb.getCellTextColor = [this](int row, int col, bool& overrideColor) -> COLORREF {
        overrideColor = false;
        if (row < 0 || row >= static_cast<int>(rows_.size())) return 0;
        const auto& r = rows_[static_cast<size_t>(row)];
        if (r.kind != DisplayRow::CategoryHeader && col > 0 && visibleCols_[static_cast<size_t>(col)] == 1) {
            overrideColor = true;
            return ThemeManager::Instance().Palette().textSecondary;
        }
        return 0;
    };
    cb.onHeaderTrack = [this]() {
        SaveColumnWidths();
        if (onColumnsChanged) onColumnsChanged();
    };
    themedLv_.SetCallbacks(std::move(cb));

    RebuildColumns();
}

void TabProcesses::Destroy() {
    lastMon_ = nullptr;
    SaveColumnWidths();
    themedLv_.Detach();
    if (hList_) {
        DestroyWindow(hList_);
        hList_ = nullptr;
    }
}

void TabProcesses::SaveColumnWidths() {
    if (!hList_ || !settings_ || visibleCols_.empty()) return;
    ThemedListView::SaveColumnsToArray(
        hList_, visibleCols_.data(), static_cast<int>(visibleCols_.size()),
        settings_->procColWidths, 10);
}

void TabProcesses::RebuildColumns() {
    if (!hList_ || !settings_) return;
    SaveColumnWidths();

    int oldCount = Header_GetItemCount(themedLv_.HeaderHwnd());
    for (int i = oldCount - 1; i >= 0; --i) {
        ListView_DeleteColumn(hList_, i);
    }

    const auto& s = GetStrings(settings_->langMode);
    const wchar_t* titles[10] = {
        s.colName, s.colStatus, s.colCpu, s.colMemory, s.colDisk,
        s.colNetwork, s.colGpu, s.colGpuEngine, s.colPowerUsage, s.colPowerTrend
    };
    const bool rightAlign[10] = { false, false, true, true, true, true, true, false, false, false };

    visibleCols_.clear();
    std::vector<HeaderColumnMeta> metas;
    for (int lc = 0; lc < 10; ++lc) {
        if (!(colVisMask_ & (1u << lc))) continue;
        int colIdx = static_cast<int>(visibleCols_.size());
        visibleCols_.push_back(lc);

        LVCOLUMNW lvc{};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt = rightAlign[lc] ? LVCFMT_RIGHT : LVCFMT_LEFT;
        lvc.cx = DpiScale(hList_, settings_->procColWidths[lc]);
        lvc.pszText = const_cast<LPWSTR>(titles[lc]);
        ListView_InsertColumn(hList_, colIdx, &lvc);

        HeaderColumnMeta hm;
        hm.title = titles[lc];
        hm.rightAlign = rightAlign[lc];
        metas.push_back(std::move(hm));
    }

    themedLv_.SetColumnsMeta(std::move(metas));
    int sortVis = -1;
    for (size_t i = 0; i < visibleCols_.size(); ++i) {
        if (visibleCols_[i] == sortCol_) sortVis = static_cast<int>(i);
    }
    themedLv_.SetSortIndicator(sortVis, sortAsc_);
}

void TabProcesses::Resize(const RECT& rc) {
    if (hList_) {
        MoveWindow(hList_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    }
}

void TabProcesses::Show(bool visible) {
    if (hList_) {
        ShowWindow(hList_, visible ? SW_SHOW : SW_HIDE);
    }
}

void TabProcesses::ApplyTheme() {
    themedLv_.ApplyTheme();
}

static bool IsGenericShellOrHostName(const std::wstring& lowName) {
    return lowName == L"cmd.exe" || lowName == L"powershell.exe" || lowName == L"pwsh.exe" ||
           lowName == L"conhost.exe" || lowName == L"openconsole.exe" ||
           lowName == L"wscript.exe" || lowName == L"cscript.exe" ||
           lowName == L"python.exe" || lowName == L"node.exe" ||
           lowName == L"svchost.exe" || lowName == L"dllhost.exe" ||
           lowName == L"rundll32.exe" || lowName == L"regsvr32.exe" ||
           lowName == L"msiexec.exe" || lowName == L"taskhostw.exe" ||
           lowName == L"runtimebroker.exe" || lowName == L"explorer.exe";
}

static bool IsWindowsInfrastructureProcess(const std::wstring& lowName) {
    static const wchar_t* const kInfra[] = {
        L"system", L"registry", L"memory compression", L"secure system", L"vmmem", L"vmmemwsl",
        L"smss.exe", L"csrss.exe", L"wininit.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe",
        L"winlogon.exe", L"dwm.exe", L"fontdrvhost.exe", L"audiodg.exe", L"svchost.exe",
        L"sihost.exe", L"taskhostw.exe", L"ctfmon.exe", L"conhost.exe", L"openconsole.exe",
        L"dllhost.exe", L"runtimebroker.exe", L"searchindexer.exe", L"searchapp.exe",
        L"searchhost.exe", L"searchprotocolhost.exe", L"searchfilterhost.exe",
        L"startmenuexperiencehost.exe", L"shellexperiencehost.exe", L"textinputhost.exe",
        L"lockapp.exe", L"spoolsv.exe", L"sppsvc.exe", L"securityhealthservice.exe",
        L"securityhealthsystray.exe", L"msmpeng.exe", L"nissrv.exe", L"mpdefendercoreservice.exe",
        L"sgrmbroker.exe", L"dashost.exe", L"wudfhost.exe", L"wlanext.exe", L"tiworker.exe",
        L"trustedinstaller.exe", L"wmiprvse.exe", L"unsecapp.exe", L"aggregatorhost.exe",
        L"backgroundtaskhost.exe", L"smartscreen.exe", L"applicationframehost.exe",
        L"systemsettings.exe", L"systemsettingsbroker.exe", L"userinit.exe", L"explorer.exe"
    };
    for (const wchar_t* name : kInfra) {
        if (lowName == name) return true;
    }
    return false;
}

static bool IsSystemBoundaryName(const std::wstring& lowName) {
    return IsWindowsInfrastructureProcess(lowName);
}

static std::wstring ProcessExeKey(const ProcessInfo& p) {
    return !p.imagePath.empty() ? LowerKey(p.imagePath) : LowerKey(p.name);
}

static std::wstring BestProcessLabel(const ProcessInfo& p) {
    if (!p.description.empty()) return p.description;
    if (!p.windowTitle.empty()) return p.windowTitle;
    return p.name;
}

void TabProcesses::RefreshData(const ProcessMonitor& mon) {
    lastMon_ = &mon;
    lastTotals_ = mon.Totals();
    totalMemBytes_ = (std::max)(1ULL, lastTotals_.totalMemoryBytes);

    // Update header totals
    std::vector<std::wstring> totals;
    for (int lc : visibleCols_) {
        switch (lc) {
        case 2: totals.push_back(FormatPercent(lastTotals_.cpuPercent, 0)); break;
        case 3: totals.push_back(FormatPercent(lastTotals_.memoryPercent, 0)); break;
        case 4: totals.push_back(FormatPercent(lastTotals_.diskPercent, 0)); break;
        case 5: totals.push_back(FormatPercent(lastTotals_.networkPercent, 0)); break;
        case 6: totals.push_back(FormatPercent(lastTotals_.gpuPercent, 0)); break;
        default: totals.push_back(L""); break;
        }
    }
    themedLv_.UpdateHeaderTotals(totals);

    ApplyCurrentRows();
}

void TabProcesses::ApplyCurrentRows() {
    if (!lastMon_ || !hList_) return;

    // Remember selected PID / key to preserve selection across rebuild
    DWORD selPid = 0;
    std::wstring selGroupKey;
    std::wstring selTitle;
    bool wasGroupParent = false;
    int selIdx = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (selIdx >= 0 && selIdx < static_cast<int>(rows_.size())) {
        const auto& cur = rows_[static_cast<size_t>(selIdx)];
        selPid = cur.primaryPid;
        selGroupKey = cur.groupKey;
        selTitle = cur.title;
        wasGroupParent = (cur.kind == DisplayRow::GroupParent);
    }

    RebuildRows(*lastMon_);

    ListView_SetItemCountEx(hList_, static_cast<int>(rows_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    // Restore selection if index shifted
    int newSel = -1;
    if (selPid != 0 || !selGroupKey.empty()) {
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const auto& r = rows_[static_cast<size_t>(i)];
            if (r.kind == DisplayRow::CategoryHeader) continue;
            if (wasGroupParent) {
                if (!selGroupKey.empty() && r.kind == DisplayRow::GroupParent && r.groupKey == selGroupKey) {
                    newSel = i;
                    break;
                }
            } else {
                if (selPid != 0 && r.primaryPid == selPid && r.kind == DisplayRow::SingleOrChild &&
                    (selTitle.empty() || r.title == selTitle)) {
                    newSel = i;
                    break;
                }
            }
        }
        if (newSel < 0 && selPid != 0) {
            for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
                const auto& r = rows_[static_cast<size_t>(i)];
                if (r.kind != DisplayRow::CategoryHeader && r.primaryPid == selPid) {
                    newSel = i;
                    break;
                }
            }
        }
    }
    if (newSel >= 0 && newSel != selIdx) {
        ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(hList_, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    InvalidateRect(hList_, nullptr, FALSE);
}

void TabProcesses::RebuildRows(const ProcessMonitor& mon) {
    rows_.clear();
    const auto& procs = mon.Processes();
    const auto& s = GetStrings(settings_->langMode);
    const size_t n = procs.size();

    struct GroupBucket {
        ProcessCategory category = ProcessCategory::Background;
        std::wstring groupKey;
        std::wstring title;
        HICON icon = nullptr;
        std::vector<const ProcessInfo*> members;
        double cpuPct = 0.0;
        uint64_t memBytes = 0;
        double diskBps = 0.0;
        double netBps = 0.0;
        double gpuPct = 0.0;
        std::wstring gpuEngine;
        int powerUsage = 0;
        int powerTrend = 0;
        bool anySuspended = false;
    };

    std::vector<GroupBucket> buckets;
    buckets.reserve(n);

    auto addMemberToBucket = [](GroupBucket& b, const ProcessInfo* p) {
        b.members.push_back(p);
        b.cpuPct += p->cpuPercent;
        b.memBytes += p->privateWorkingSet;
        b.diskBps += p->diskBytesPerSec;
        b.netBps += p->netBytesPerSec;
        if (p->gpuPercent > b.gpuPct) {
            b.gpuPct = p->gpuPercent;
            b.gpuEngine = p->gpuEngine;
        }
        if (p->powerUsageLevel > b.powerUsage) b.powerUsage = p->powerUsageLevel;
        if (p->powerTrendLevel > b.powerTrend) b.powerTrend = p->powerTrendLevel;
        if (p->isSuspended) b.anySuspended = true;
        if (!b.icon && p->icon) b.icon = p->icon;
    };

    if (settings_->groupByType) {
        std::unordered_map<DWORD, int> pidToIdx;
        pidToIdx.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            pidToIdx[procs[i].pid] = static_cast<int>(i);
        }

        auto aliveParent = [&](int idx) -> int {
            DWORD ppid = procs[static_cast<size_t>(idx)].parentPid;
            auto it = pidToIdx.find(ppid);
            if (it == pidToIdx.end()) return -1;
            int pIdx = it->second;
            if (pIdx == idx || procs[static_cast<size_t>(pIdx)].pid <= 4) return -1;
            if (procs[static_cast<size_t>(pIdx)].createTime > procs[static_cast<size_t>(idx)].createTime) return -1;
            return pIdx;
        };

        std::vector<int> appRoot(n, -1);
        std::vector<bool> isAppRoot(n, false);

        // Pass 1: Identify root for each process that owns a visible window
        for (size_t i = 0; i < n; ++i) {
            if (procs[i].category != ProcessCategory::App) continue;
            std::wstring exeI = ProcessExeKey(procs[i]);
            int topSameExe = -1;
            int cur = aliveParent(static_cast<int>(i));
            for (int depth = 0; cur >= 0 && depth < 64; ++depth) {
                const auto& ap = procs[static_cast<size_t>(cur)];
                if (IsWindowsInfrastructureProcess(LowerKey(ap.name))) {
                    break;
                }
                if (ProcessExeKey(ap) == exeI) {
                    topSameExe = cur;
                }
                cur = aliveParent(cur);
            }
            int root = (topSameExe >= 0) ? topSameExe : static_cast<int>(i);
            isAppRoot[static_cast<size_t>(root)] = true;
            appRoot[i] = root;
            appRoot[static_cast<size_t>(root)] = root;
        }

        // Pass 2: Attach descendant subtrees (workers, language_server.exe, cmd.exe, etc.) to their App root
        // while leaving Windows OS infrastructure processes (conhost, svchost, csrss, etc.) in Windows section.
        for (size_t i = 0; i < n; ++i) {
            if (appRoot[i] >= 0) continue;
            const auto& p = procs[i];
            if (IsWindowsInfrastructureProcess(LowerKey(p.name))) {
                continue;
            }
            int cur = aliveParent(static_cast<int>(i));
            for (int depth = 0; cur >= 0 && depth < 64; ++depth) {
                const auto& ap = procs[static_cast<size_t>(cur)];
                if (IsWindowsInfrastructureProcess(LowerKey(ap.name))) {
                    break;
                }
                if (appRoot[static_cast<size_t>(cur)] >= 0) {
                    appRoot[i] = appRoot[static_cast<size_t>(cur)];
                    break;
                }
                if (isAppRoot[static_cast<size_t>(cur)]) {
                    appRoot[i] = cur;
                    break;
                }
                cur = aliveParent(cur);
            }
        }

        // Pass 3: Fold orphaned background helpers sharing the same non-generic App executable
        std::unordered_map<std::wstring, int> exeToAppRoot;
        for (size_t i = 0; i < n; ++i) {
            if (!isAppRoot[i]) continue;
            std::wstring lowName = LowerKey(procs[i].name);
            if (IsGenericShellOrHostName(lowName)) continue;
            std::wstring exe = ProcessExeKey(procs[i]);
            if (exeToAppRoot.find(exe) == exeToAppRoot.end()) {
                exeToAppRoot[exe] = static_cast<int>(i);
            }
        }
        for (size_t i = 0; i < n; ++i) {
            if (appRoot[i] >= 0 || procs[i].category != ProcessCategory::Background) continue;
            if (IsGenericShellOrHostName(LowerKey(procs[i].name))) continue;
            auto it = exeToAppRoot.find(ProcessExeKey(procs[i]));
            if (it != exeToAppRoot.end()) {
                appRoot[i] = it->second;
            }
        }

        // Build App buckets by root index
        std::unordered_map<int, size_t> rootToBucket;
        for (size_t i = 0; i < n; ++i) {
            int root = appRoot[i];
            if (root < 0) continue;
            auto bit = rootToBucket.find(root);
            if (bit == rootToBucket.end()) {
                const auto& rp = procs[static_cast<size_t>(root)];
                GroupBucket b;
                b.category = ProcessCategory::App;
                b.groupKey = L"app|" + std::to_wstring(rp.pid);
                b.title = BestProcessLabel(rp);
                b.icon = rp.icon;
                rootToBucket[root] = buckets.size();
                buckets.push_back(std::move(b));
                bit = rootToBucket.find(root);
            }
            GroupBucket& b = buckets[bit->second];
            addMemberToBucket(b, &procs[i]);
            // Prefer FileDescription from any windowed child if root lacked one
            if (b.title == procs[static_cast<size_t>(root)].name) {
                if (!procs[i].description.empty()) {
                    b.title = procs[i].description;
                } else if (b.members.size() == 1 && !procs[i].windowTitle.empty()) {
                    b.title = procs[i].windowTitle;
                }
            }
            if (!b.icon && procs[i].icon) b.icon = procs[i].icon;
        }

        // Remaining processes bucket into Background or Windows by category + exe key
        std::unordered_map<std::wstring, size_t> otherBucketMap;
        for (size_t i = 0; i < n; ++i) {
            if (appRoot[i] >= 0) continue;
            const auto& p = procs[i];
            std::wstring keyBase = std::to_wstring(static_cast<int>(p.category)) + L"|" + ProcessExeKey(p);
            auto it = otherBucketMap.find(keyBase);
            if (it == otherBucketMap.end()) {
                GroupBucket b;
                b.category = p.category;
                b.groupKey = keyBase;
                b.title = BestProcessLabel(p);
                b.icon = p.icon;
                otherBucketMap[keyBase] = buckets.size();
                buckets.push_back(std::move(b));
                it = otherBucketMap.find(keyBase);
            }
            addMemberToBucket(buckets[it->second], &p);
        }
    } else {
        for (const auto& p : procs) {
            GroupBucket b;
            b.category = p.category;
            b.groupKey = L"pid|" + std::to_wstring(p.pid);
            b.title = !p.windowTitle.empty() ? p.windowTitle : BestProcessLabel(p);
            b.icon = p.icon;
            addMemberToBucket(b, &p);
            buckets.push_back(std::move(b));
        }
    }

    auto cmpBuckets = [this](const GroupBucket& a, const GroupBucket& b) -> bool {
        int cmp = 0;
        switch (sortCol_) {
        case 0: cmp = _wcsicmp(a.title.c_str(), b.title.c_str()); break;
        case 1: cmp = (a.anySuspended ? 1 : 0) - (b.anySuspended ? 1 : 0); break;
        case 2: cmp = (a.cpuPct < b.cpuPct) ? -1 : (a.cpuPct > b.cpuPct ? 1 : 0); break;
        case 3: cmp = (a.memBytes < b.memBytes) ? -1 : (a.memBytes > b.memBytes ? 1 : 0); break;
        case 4: cmp = (a.diskBps < b.diskBps) ? -1 : (a.diskBps > b.diskBps ? 1 : 0); break;
        case 5: cmp = (a.netBps < b.netBps) ? -1 : (a.netBps > b.netBps ? 1 : 0); break;
        case 6: cmp = (a.gpuPct < b.gpuPct) ? -1 : (a.gpuPct > b.gpuPct ? 1 : 0); break;
        case 7: cmp = _wcsicmp(a.gpuEngine.c_str(), b.gpuEngine.c_str()); break;
        case 8: cmp = a.powerUsage - b.powerUsage; break;
        case 9: cmp = a.powerTrend - b.powerTrend; break;
        }
        if (cmp == 0) cmp = _wcsicmp(a.title.c_str(), b.title.c_str());
        return sortAsc_ ? (cmp < 0) : (cmp > 0);
    };

    auto emitBucket = [&](const GroupBucket& b) {
        const bool isAppWithWindowChildren =
            (b.category == ProcessCategory::App && b.members.size() == 1 && !b.members[0]->windowTitles.empty());
        const bool hasChevron = (b.members.size() > 1) || isAppWithWindowChildren;
        const bool isExp = hasChevron && (expandedGroups_.count(b.groupKey) > 0);

        DisplayRow row;
        row.kind = hasChevron ? DisplayRow::GroupParent : DisplayRow::SingleOrChild;
        row.category = b.category;
        row.groupKey = hasChevron ? b.groupKey : L"";
        row.title = (b.members.size() > 1)
                        ? (b.title + L" (" + std::to_wstring(b.members.size()) + L")")
                        : b.title;
        row.statusText = b.anySuspended ? s.statusSuspended : L"";
        row.icon = b.icon;
        row.primaryPid = b.members[0]->pid;
        for (const auto* m : b.members) row.pids.push_back(m->pid);
        row.imagePath = b.members[0]->imagePath;
        row.procName = b.members[0]->name;
        row.childCount = hasChevron ? static_cast<int>(b.members.size()) : 0;
        row.expanded = isExp;
        row.cpuPct = (std::min)(100.0, b.cpuPct);
        row.memBytes = b.memBytes;
        row.diskBps = b.diskBps;
        row.netBps = b.netBps;
        row.gpuPct = b.gpuPct;
        row.gpuEngine = b.gpuEngine;
        row.powerUsage = b.powerUsage;
        row.powerTrend = b.powerTrend;
        rows_.push_back(std::move(row));

        if (isExp) {
            if (isAppWithWindowChildren) {
                const auto* m = b.members[0];
                for (size_t wi = 0; wi < m->windowTitles.size(); ++wi) {
                    DisplayRow child;
                    child.kind = DisplayRow::SingleOrChild;
                    child.category = b.category;
                    child.groupKey = b.groupKey;
                    child.title = m->windowTitles[wi];
                    child.statusText = m->isSuspended ? s.statusSuspended : L"";
                    child.icon = m->icon;
                    child.primaryPid = m->pid;
                    child.pids = { m->pid };
                    child.imagePath = m->imagePath;
                    child.procName = m->name;
                    child.childCount = 0;
                    if (wi == 0) {
                        child.cpuPct = m->cpuPercent;
                        child.memBytes = m->privateWorkingSet;
                        child.diskBps = m->diskBytesPerSec;
                        child.netBps = m->netBytesPerSec;
                        child.gpuPct = m->gpuPercent;
                        child.gpuEngine = m->gpuEngine;
                        child.powerUsage = m->powerUsageLevel;
                        child.powerTrend = m->powerTrendLevel;
                    }
                    rows_.push_back(std::move(child));
                }
            } else {
                std::vector<const ProcessInfo*> sortedMembers = b.members;
                std::stable_sort(sortedMembers.begin(), sortedMembers.end(),
                    [](const ProcessInfo* x, const ProcessInfo* y) {
                        std::wstring lx = !x->windowTitle.empty() ? x->windowTitle : BestProcessLabel(*x);
                        std::wstring ly = !y->windowTitle.empty() ? y->windowTitle : BestProcessLabel(*y);
                        return _wcsicmp(lx.c_str(), ly.c_str()) < 0;
                    });
                for (const auto* m : sortedMembers) {
                    DisplayRow child;
                    child.kind = DisplayRow::SingleOrChild;
                    child.category = b.category;
                    child.groupKey = b.groupKey;
                    child.title = !m->windowTitle.empty() ? m->windowTitle : BestProcessLabel(*m);
                    child.statusText = m->isSuspended ? s.statusSuspended : L"";
                    child.icon = m->icon;
                    child.primaryPid = m->pid;
                    child.pids = { m->pid };
                    child.imagePath = m->imagePath;
                    child.procName = m->name;
                    child.childCount = 0; // indented child
                    child.cpuPct = m->cpuPercent;
                    child.memBytes = m->privateWorkingSet;
                    child.diskBps = m->diskBytesPerSec;
                    child.netBps = m->netBytesPerSec;
                    child.gpuPct = m->gpuPercent;
                    child.gpuEngine = m->gpuEngine;
                    child.powerUsage = m->powerUsageLevel;
                    child.powerTrend = m->powerTrendLevel;
                    rows_.push_back(std::move(child));
                }
            }
        }
    };

    if (settings_->groupByType) {
        std::vector<GroupBucket> catBuckets[3];
        for (auto& b : buckets) {
            catBuckets[static_cast<int>(b.category)].push_back(std::move(b));
        }
        const wchar_t* catTitles[3] = { s.grpApps, s.grpBackground, s.grpWindows };
        for (int c = 0; c < 3; ++c) {
            if (catBuckets[c].empty()) continue;
            std::sort(catBuckets[c].begin(), catBuckets[c].end(), cmpBuckets);

            DisplayRow hdr;
            hdr.kind = DisplayRow::CategoryHeader;
            hdr.category = static_cast<ProcessCategory>(c);
            hdr.title = std::wstring(catTitles[c]) + L" (" + std::to_wstring(catBuckets[c].size()) + L")";
            hdr.expanded = !collapsedCategories_[c];
            rows_.push_back(std::move(hdr));

            if (!collapsedCategories_[c]) {
                for (const auto& b : catBuckets[c]) {
                    emitBucket(b);
                }
            }
        }
    } else {
        std::sort(buckets.begin(), buckets.end(), cmpBuckets);
        for (const auto& b : buckets) {
            emitBucket(b);
        }
    }
}

bool TabProcesses::HasSelection() const {
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(rows_.size())) return false;
    return rows_[static_cast<size_t>(sel)].kind != DisplayRow::CategoryHeader;
}

int TabProcesses::GetSelectedPidList(std::vector<DWORD>& outPids, std::wstring& outName, std::wstring& outPath) const {
    outPids.clear();
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(rows_.size())) return -1;
    const auto& r = rows_[static_cast<size_t>(sel)];
    if (r.kind == DisplayRow::CategoryHeader) return -1;
    outPids = r.pids;
    outName = r.procName;
    outPath = r.imagePath;
    return sel;
}

void TabProcesses::EndPidList(const std::vector<DWORD>& pids) {
    if (pids.empty()) return;
    bool anyOk = false;
    for (DWORD pid : pids) {
        if (ProcessMonitor::EndTask(pid)) anyOk = true;
    }
    if (!anyOk) {
        const auto& s = GetStrings(settings_->langMode);
        MessageBoxW(hParent_, s.msgAccessDenied, s.appTitle, MB_ICONERROR);
    } else if (settings_->minimizeOnUse && onActionMinimize) {
        onActionMinimize();
    }
}

void TabProcesses::EndSelectedTask() {
    std::vector<DWORD> pids;
    std::wstring name, path;
    if (GetSelectedPidList(pids, name, path) < 0) return;
    EndPidList(pids);
}

void TabProcesses::ExpandOrCollapseSelected(int forceState) {
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(rows_.size())) return;
    const auto& r = rows_[static_cast<size_t>(sel)];
    if (r.kind == DisplayRow::CategoryHeader) {
        int c = static_cast<int>(r.category);
        bool next = (forceState >= 0) ? (forceState == 0) : !collapsedCategories_[c];
        collapsedCategories_[c] = next;
    } else if (r.kind == DisplayRow::GroupParent) {
        bool next = (forceState >= 0) ? (forceState == 1) : (expandedGroups_.count(r.groupKey) == 0);
        if (next) expandedGroups_.insert(r.groupKey);
        else expandedGroups_.erase(r.groupKey);
    }
    ApplyCurrentRows();
}

void TabProcesses::ExpandAll(bool expand) {
    if (expand) {
        for (int c = 0; c < 3; ++c) collapsedCategories_[c] = false;
        for (const auto& r : rows_) {
            if (r.kind == DisplayRow::GroupParent) expandedGroups_.insert(r.groupKey);
        }
    } else {
        expandedGroups_.clear();
    }
    ApplyCurrentRows();
}

void TabProcesses::OpenSelectedProperties() {
    std::vector<DWORD> pids;
    std::wstring name, path;
    if (GetSelectedPidList(pids, name, path) >= 0 && !path.empty()) {
        ShowFilePropertiesDialog(hParent_, path);
    }
}

void TabProcesses::HandleKeyDown(WPARAM vk) {
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(rows_.size())) return;
    if (vk == VK_RIGHT) {
        ExpandOrCollapseSelected(1);
    } else if (vk == VK_LEFT) {
        const auto& r = rows_[static_cast<size_t>(sel)];
        if (r.kind == DisplayRow::SingleOrChild && !r.groupKey.empty() && r.childCount == 0) {
            expandedGroups_.erase(r.groupKey);
            ApplyCurrentRows();
        } else {
            ExpandOrCollapseSelected(0);
        }
    }
}

void TabProcesses::HandleClick(NMITEMACTIVATE* nmia) {
    if (!nmia || nmia->iItem < 0 || nmia->iItem >= static_cast<int>(rows_.size())) return;
    const auto& r = rows_[static_cast<size_t>(nmia->iItem)];
    if (r.kind == DisplayRow::CategoryHeader || themedLv_.HitTestChevron(nmia->iItem, nmia->ptAction)) {
        ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(hList_, nmia->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ExpandOrCollapseSelected(-1);
    }
}

void TabProcesses::HandleDoubleClick(NMITEMACTIVATE* nmia) {
    if (!nmia || nmia->iItem < 0 || nmia->iItem >= static_cast<int>(rows_.size())) return;
    const auto& r = rows_[static_cast<size_t>(nmia->iItem)];
    if (r.kind == DisplayRow::CategoryHeader || r.kind == DisplayRow::GroupParent) {
        ExpandOrCollapseSelected(-1);
    }
}

void TabProcesses::HandleColumnClick(int col) {
    if (col < 0 || col >= static_cast<int>(visibleCols_.size())) return;
    int logicalCol = visibleCols_[static_cast<size_t>(col)];
    if (sortCol_ == logicalCol) {
        sortAsc_ = !sortAsc_;
    } else {
        sortCol_ = logicalCol;
        sortAsc_ = (logicalCol == 0 || logicalCol == 1 || logicalCol == 7);
    }
    themedLv_.SetSortIndicator(col, sortAsc_);
    ApplyCurrentRows();
}

void TabProcesses::HandleHeaderContextMenu(POINT screenPt) {
    const auto& s = GetStrings(settings_->langMode);
    HMENU hMenu = CreatePopupMenu();

    const wchar_t* colNames[10] = {
        s.colName, s.colStatus, s.colCpu, s.colMemory, s.colDisk,
        s.colNetwork, s.colGpu, s.colGpuEngine, s.colPowerUsage, s.colPowerTrend
    };
    for (int i = 1; i < 10; ++i) {
        UINT flags = MF_STRING | ((colVisMask_ & (1u << i)) ? MF_CHECKED : MF_UNCHECKED);
        AppendMenuW(hMenu, flags, IDM_CTX_COL_BASE + i, colNames[i]);
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    HMENU hResMenu = CreatePopupMenu();
    HMENU hMemMenu = CreatePopupMenu();
    AppendMenuW(hMemMenu, MF_STRING | (!settings_->memShowPercent ? MF_CHECKED : 0), IDM_CTX_MEM_VALUES, s.ctxValues);
    AppendMenuW(hMemMenu, MF_STRING | (settings_->memShowPercent ? MF_CHECKED : 0), IDM_CTX_MEM_PERCENT, s.ctxPercents);
    HMENU hDiskMenu = CreatePopupMenu();
    AppendMenuW(hDiskMenu, MF_STRING | (!settings_->diskShowPercent ? MF_CHECKED : 0), IDM_CTX_DISK_VALUES, s.ctxValues);
    AppendMenuW(hDiskMenu, MF_STRING | (settings_->diskShowPercent ? MF_CHECKED : 0), IDM_CTX_DISK_PERCENT, s.ctxPercents);
    HMENU hNetMenu = CreatePopupMenu();
    AppendMenuW(hNetMenu, MF_STRING | (!settings_->netShowPercent ? MF_CHECKED : 0), IDM_CTX_NET_VALUES, s.ctxValues);
    AppendMenuW(hNetMenu, MF_STRING | (settings_->netShowPercent ? MF_CHECKED : 0), IDM_CTX_NET_PERCENT, s.ctxPercents);

    AppendMenuW(hResMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hMemMenu), s.ctxResMemory);
    AppendMenuW(hResMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hDiskMenu), s.ctxResDisk);
    AppendMenuW(hResMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hNetMenu), s.ctxResNetwork);
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hResMenu), s.ctxResourceValues);

    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hParent_, nullptr);
    DestroyMenu(hMenu);

    if (cmd >= IDM_CTX_COL_BASE + 1 && cmd < IDM_CTX_COL_BASE + 10) {
        int lc = static_cast<int>(cmd - IDM_CTX_COL_BASE);
        colVisMask_ ^= (1u << lc);
        colVisMask_ |= 1u;
        settings_->procColMask = colVisMask_;
        RebuildColumns();
        if (onColumnsChanged) onColumnsChanged();
        SendMessageW(hParent_, WM_COMMAND, IDM_VIEW_REFRESH_NOW, 0);
    } else if (cmd == IDM_CTX_MEM_VALUES) { settings_->memShowPercent = false; InvalidateRect(hList_, nullptr, FALSE); }
    else if (cmd == IDM_CTX_MEM_PERCENT) { settings_->memShowPercent = true; InvalidateRect(hList_, nullptr, FALSE); }
    else if (cmd == IDM_CTX_DISK_VALUES) { settings_->diskShowPercent = false; InvalidateRect(hList_, nullptr, FALSE); }
    else if (cmd == IDM_CTX_DISK_PERCENT) { settings_->diskShowPercent = true; InvalidateRect(hList_, nullptr, FALSE); }
    else if (cmd == IDM_CTX_NET_VALUES) { settings_->netShowPercent = false; InvalidateRect(hList_, nullptr, FALSE); }
    else if (cmd == IDM_CTX_NET_PERCENT) { settings_->netShowPercent = true; InvalidateRect(hList_, nullptr, FALSE); }
}

void TabProcesses::HandleContextMenu(POINT screenPt) {
    int sel = ListView_GetNextItem(hList_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(rows_.size())) return;

    if (screenPt.x == -1 && screenPt.y == -1) {
        RECT rc{};
        ListView_GetItemRect(hList_, sel, &rc, LVIR_BOUNDS);
        POINT pt{ rc.left + DpiScale(hList_, 24), (rc.top + rc.bottom) / 2 };
        ClientToScreen(hList_, &pt);
        screenPt = pt;
    }

    const auto& r = rows_[static_cast<size_t>(sel)];
    const auto& s = GetStrings(settings_->langMode);

    // Capture target identity before TrackPopupMenu in case background refresh shifts rows_
    bool isCategoryHeader = (r.kind == DisplayRow::CategoryHeader);
    bool isGroupParent = (r.kind == DisplayRow::GroupParent);
    ProcessCategory targetCategory = r.category;
    std::wstring targetGroupKey = r.groupKey;
    DWORD targetPid = r.primaryPid;
    std::vector<DWORD> targetPids = r.pids;
    std::wstring targetName = r.procName;
    std::wstring targetPath = r.imagePath;

    HWND switchWnd = nullptr;
    if (!isCategoryHeader) {
        switchWnd = FindMainWindowForPid(targetPid);
        if (!switchWnd) {
            for (DWORD cpid : targetPids) {
                switchWnd = FindMainWindowForPid(cpid);
                if (switchWnd) break;
            }
        }
    }

    HMENU hMenu = CreatePopupMenu();
    if (isCategoryHeader) {
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_EXPAND_COLLAPSE, r.expanded ? s.ctxCollapse : s.ctxExpand);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_VIEW_EXPAND_ALL, s.menuExpandAll);
        AppendMenuW(hMenu, MF_STRING, IDM_VIEW_COLLAPSE_ALL, s.menuCollapseAll);
    } else {
        if (isGroupParent) {
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_EXPAND_COLLAPSE, r.expanded ? s.ctxCollapse : s.ctxExpand);
        }
        if (switchWnd) {
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_SWITCH_TO, s.ctxSwitchTo);
        }

        bool isExplorer = (_wcsicmp(r.procName.c_str(), L"explorer.exe") == 0);
        if (isExplorer) {
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_RESTART_EXPLORER, s.ctxRestart);
        }
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_END_TASK, s.ctxEndTask);

        HMENU hResMenu = CreatePopupMenu();
        struct ResEntry { const wchar_t* label; bool asPct; UINT idVal; UINT idPct; } resItems[] = {
            { s.ctxResMemory,  settings_->memShowPercent,  IDM_CTX_MEM_VALUES,  IDM_CTX_MEM_PERCENT },
            { s.ctxResDisk,    settings_->diskShowPercent, IDM_CTX_DISK_VALUES, IDM_CTX_DISK_PERCENT },
            { s.ctxResNetwork, settings_->netShowPercent,  IDM_CTX_NET_VALUES,  IDM_CTX_NET_PERCENT },
        };
        for (const auto& ri : resItems) {
            HMENU hSub = CreatePopupMenu();
            AppendMenuW(hSub, MF_STRING | (!ri.asPct ? MF_CHECKED : 0), ri.idVal, s.ctxValues);
            AppendMenuW(hSub, MF_STRING | (ri.asPct ? MF_CHECKED : 0),  ri.idPct, s.ctxPercents);
            AppendMenuW(hResMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSub), ri.label);
        }
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hResMenu), s.ctxResourceValues);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_CREATE_DUMP, s.ctxCreateDump);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_GO_TO_DETAILS, s.ctxGoToDetails);
        AppendMenuW(hMenu, MF_STRING | (r.imagePath.empty() ? MF_GRAYED : 0), IDM_CTX_OPEN_LOCATION, s.ctxOpenLocation);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_SEARCH_ONLINE, s.ctxSearchOnline);
        AppendMenuW(hMenu, MF_STRING | (r.imagePath.empty() ? MF_GRAYED : 0), IDM_CTX_PROPERTIES, s.ctxProperties);
    }

    if (isCategoryHeader || isGroupParent) {
        SetMenuDefaultItem(hMenu, 0, TRUE);
    }

    UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hParent_, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
    case IDM_CTX_EXPAND_COLLAPSE:
        if (isCategoryHeader) {
            int catIdx = static_cast<int>(targetCategory);
            if (catIdx >= 0 && catIdx < 3) {
                collapsedCategories_[catIdx] = !collapsedCategories_[catIdx];
                ApplyCurrentRows();
            }
        } else if (isGroupParent && !targetGroupKey.empty()) {
            if (expandedGroups_.count(targetGroupKey)) expandedGroups_.erase(targetGroupKey);
            else expandedGroups_.insert(targetGroupKey);
            ApplyCurrentRows();
        }
        break;
    case IDM_VIEW_EXPAND_ALL: ExpandAll(true); break;
    case IDM_VIEW_COLLAPSE_ALL: ExpandAll(false); break;
    case IDM_CTX_END_TASK: EndPidList(targetPids); break;
    case IDM_CTX_RESTART_EXPLORER: {
        ProcessMonitor::EndTask(targetPid);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case IDM_CTX_MEM_VALUES:  settings_->memShowPercent = false; InvalidateRect(hList_, nullptr, FALSE); break;
    case IDM_CTX_MEM_PERCENT: settings_->memShowPercent = true; InvalidateRect(hList_, nullptr, FALSE); break;
    case IDM_CTX_DISK_VALUES: settings_->diskShowPercent = false; InvalidateRect(hList_, nullptr, FALSE); break;
    case IDM_CTX_DISK_PERCENT:settings_->diskShowPercent = true; InvalidateRect(hList_, nullptr, FALSE); break;
    case IDM_CTX_NET_VALUES:  settings_->netShowPercent = false; InvalidateRect(hList_, nullptr, FALSE); break;
    case IDM_CTX_NET_PERCENT: settings_->netShowPercent = true; InvalidateRect(hList_, nullptr, FALSE); break;
    case IDM_CTX_CREATE_DUMP: {
        std::wstring dumpPath;
        if (ProcessMonitor::CreateDumpFile(targetPid, targetName, dumpPath)) {
            std::wstring msg = std::wstring(s.msgDumpSaved) + dumpPath;
            MessageBoxW(hParent_, msg.c_str(), s.appTitle, MB_ICONINFORMATION);
        } else {
            MessageBoxW(hParent_, s.msgDumpFailed, s.appTitle, MB_ICONERROR);
        }
        break;
    }
    case IDM_CTX_SWITCH_TO:
        if (BringWindowToFront(switchWnd) && onActionMinimize) {
            onActionMinimize();
        }
        break;
    case IDM_CTX_GO_TO_DETAILS:
        if (onGoToDetails) onGoToDetails(targetPid);
        break;
    case IDM_CTX_OPEN_LOCATION:
        OpenFileLocationInExplorer(targetPath);
        break;
    case IDM_CTX_SEARCH_ONLINE:
        SearchProcessOnline(targetName);
        break;
    case IDM_CTX_PROPERTIES:
        ShowFilePropertiesDialog(hParent_, targetPath);
        break;
    }
}

LRESULT TabProcesses::HandleNotify(NMHDR* nm) {
    if (!nm) return 0;
    if (nm->hwndFrom == themedLv_.HeaderHwnd()) {
        if (nm->code == NM_RCLICK) {
            POINT pt{};
            GetCursorPos(&pt);
            HandleHeaderContextMenu(pt);
            return 1;
        }
        if (ThemedListView::IsHeaderTrackNotify(nm, themedLv_.HeaderHwnd())) {
            SaveColumnWidths();
            if (onColumnsChanged) onColumnsChanged();
            return 0;
        }
    }
    if (nm->hwndFrom != hList_) return 0;
    switch (nm->code) {
    case NM_CUSTOMDRAW:
        return themedLv_.HandleCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
    case NM_CLICK:
        HandleClick(reinterpret_cast<NMITEMACTIVATE*>(nm));
        return 0;
    case NM_DBLCLK:
        HandleDoubleClick(reinterpret_cast<NMITEMACTIVATE*>(nm));
        return 0;
    case LVN_COLUMNCLICK:
        HandleColumnClick(reinterpret_cast<NMLISTVIEW*>(nm)->iSubItem);
        return 0;
    case LVN_KEYDOWN:
        HandleKeyDown(reinterpret_cast<NMLVKEYDOWN*>(nm)->wVKey);
        return 0;
    case NM_RCLICK: {
        auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(nm);
        int hitRow = nmia ? nmia->iItem : -1;
        if (hitRow < 0 && nmia) {
            LVHITTESTINFO hti{};
            hti.pt = nmia->ptAction;
            hitRow = ListView_SubItemHitTest(hList_, &hti);
        }
        if (hitRow >= 0 && hitRow < static_cast<int>(rows_.size())) {
            ListView_SetItemState(hList_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(hList_, hitRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        POINT pt{};
        if (nmia && nmia->ptAction.x >= 0 && nmia->ptAction.y >= 0) {
            pt = nmia->ptAction;
            ClientToScreen(hList_, &pt);
        } else if (!GetCursorPos(&pt)) {
            pt = { -1, -1 };
        }
        HandleContextMenu(pt);
        return 1;
    }
    }
    return 0;
}

} // namespace taskman
