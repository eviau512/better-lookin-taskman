#include "app_history.h"
#include "uwp_apps.h"
#include <objbase.h>
#include <shlwapi.h>
#include <cwctype>
#include <algorithm>
#include <vector>
#include <unordered_map>

namespace taskman {

namespace {

std::wstring LowerStr(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

#pragma pack(push, 8)
struct SRU_STATS_COLUMN_NATIVE {
    DWORD columnId;
    DWORD dataType; // 0 = uint32, 1 = uint64
    uint64_t val;
    uint64_t val2;
};

struct SRU_STATS_RECORD_NATIVE {
    SYSTEMTIME timestamp;   // +0 (16 bytes)
    DWORD appType;          // +16 (0 = Win32/system, 2 = UWP/packaged/appx)
    DWORD reserved1;        // +20
    const wchar_t* appName; // +24
    PVOID reservedPtr1;     // +32
    PVOID reservedPtr2;     // +40
    WORD nrColumns;         // +48
    WORD pad[3];            // +50..55
    const SRU_STATS_COLUMN_NATIVE* columns; // +56
};

struct SRU_STATS_RECORD_SET_NATIVE {
    DWORD nrRecords;
    DWORD reserved;
    const SRU_STATS_RECORD_NATIVE* records;
};
#pragma pack(pop)

static_assert(sizeof(SRU_STATS_COLUMN_NATIVE) == 24, "SRU_STATS_COLUMN size mismatch");
static_assert(sizeof(SRU_STATS_RECORD_NATIVE) == 64, "SRU_STATS_RECORD size mismatch");

using PFN_SruQueryStats = DWORD(WINAPI*)(DWORD providerId, const SYSTEMTIME* startTime, const SYSTEMTIME* endTime,
                                         DWORD flags, SRU_STATS_RECORD_SET_NATIVE** outRecordSet);
using PFN_SruFreeRecordSet = void(WINAPI*)(SRU_STATS_RECORD_SET_NATIVE* recordSet);

uint64_t ReadColU64(const SRU_STATS_COLUMN_NATIVE& c) {
    return (c.dataType == 0) ? static_cast<uint64_t>(static_cast<DWORD>(c.val)) : c.val;
}

DWORD ReadCpuNominalMhz() {
    HKEY hKey = nullptr;
    DWORD mhz = 2500;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0;
        DWORD sz = sizeof(val);
        if (RegQueryValueExW(hKey, L"~MHz", nullptr, nullptr, reinterpret_cast<BYTE*>(&val), &sz) == ERROR_SUCCESS &&
            val >= 200 && val <= 15000) {
            mhz = val;
        }
        RegCloseKey(hKey);
    }
    return mhz;
}

std::vector<std::pair<std::wstring, std::wstring>> BuildDosDriveMap() {
    std::vector<std::pair<std::wstring, std::wstring>> map;
    wchar_t devName[MAX_PATH] = {};
    for (wchar_t ch = L'A'; ch <= L'Z'; ++ch) {
        wchar_t drive[3] = { ch, L':', L'\0' };
        if (QueryDosDeviceW(drive, devName, ARRAYSIZE(devName)) > 0) {
            std::wstring ntPrefix = LowerStr(devName);
            std::wstring dosPrefix = { static_cast<wchar_t>(std::towlower(ch)), L':' };
            map.emplace_back(std::move(ntPrefix), std::move(dosPrefix));
        }
    }
    // Longest NT prefix first to avoid partial prefix matches
    std::sort(map.begin(), map.end(), [](const auto& a, const auto& b) {
        return a.first.size() > b.first.size();
    });
    return map;
}

std::wstring TranslateNtPath(const std::wstring& pathLower,
                             const std::vector<std::pair<std::wstring, std::wstring>>& dosMap) {
    for (const auto& kv : dosMap) {
        if (pathLower.size() >= kv.first.size() &&
            pathLower.compare(0, kv.first.size(), kv.first) == 0 &&
            (pathLower.size() == kv.first.size() || pathLower[kv.first.size()] == L'\\')) {
            return kv.second + pathLower.substr(kv.first.size());
        }
    }
    return pathLower;
}

// Converts SRUM AppName (e.g. PackageFullName "Name_1.2.3.4_x64__pubid" or AUMID "Family!App") to lowercase pkgFamily
std::wstring ExtractSrumPkgFamily(const std::wstring& rawApp) {
    std::wstring s = rawApp;
    size_t bang = s.find(L'!');
    if (bang != std::wstring::npos) s.resize(bang);
    size_t first = s.find(L'_');
    size_t last = s.rfind(L'_');
    if (first == std::wstring::npos || last == std::wstring::npos) return L"";
    if (last > first) {
        // Check if there are >= 4 underscores (PackageFullName: Name_Ver_Arch_ResId_PubId)
        size_t underscores = 0;
        for (wchar_t c : s) {
            if (c == L'_') ++underscores;
        }
        if (underscores >= 4) {
            return LowerStr(s.substr(0, first) + s.substr(last));
        }
        // PackageFamilyName has single '_' or double '__'? Wait: Name_pubId has 1 underscore.
        return LowerStr(s);
    }
    return LowerStr(s);
}

std::wstring FormatSysDate(const SYSTEMTIME& st) {
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

int CompareSysTimeDate(const SYSTEMTIME& a, const SYSTEMTIME& b) {
    if (a.wYear != b.wYear) return (a.wYear < b.wYear) ? -1 : 1;
    if (a.wMonth != b.wMonth) return (a.wMonth < b.wMonth) ? -1 : 1;
    if (a.wDay != b.wDay) return (a.wDay < b.wDay) ? -1 : 1;
    return 0;
}

} // namespace

AppHistoryManager::~AppHistoryManager() {
    Shutdown();
    for (HICON ico : ownedIcons_) {
        if (ico) DestroyIcon(ico);
    }
    ownedIcons_.clear();
}

void AppHistoryManager::Shutdown() {
    stopRequested_.store(true, std::memory_order_release);
    if (initThread_.joinable()) {
        initThread_.join();
    }
}

void AppHistoryManager::Init() {
    bool expected = false;
    if (!initStarted_.compare_exchange_strong(expected, true)) return;
    RunInitWork(false);
}

void AppHistoryManager::InitAsync(HWND notifyHwnd, UINT notifyMsg) {
    bool expected = false;
    if (!initStarted_.compare_exchange_strong(expected, true)) return;
    initThread_ = std::thread([this, notifyHwnd, notifyMsg]() {
        RunInitWork(true);
        if (!stopRequested_.load(std::memory_order_acquire) && notifyHwnd && IsWindow(notifyHwnd) && notifyMsg != 0) {
            PostMessageW(notifyHwnd, notifyMsg, 0, 0);
        }
    });
}

void AppHistoryManager::RunInitWork(bool initCom) {
    if (initCom) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }

    wchar_t progFiles[MAX_PATH] = {};
    std::wstring winAppsLower;
    if (GetEnvironmentVariableW(L"ProgramW6432", progFiles, MAX_PATH) ||
        GetEnvironmentVariableW(L"ProgramFiles", progFiles, MAX_PATH)) {
        winAppsLower = LowerStr(std::wstring(progFiles) + L"\\windowsapps\\");
    }

    std::vector<UwpAppEntry> apps = EnumerateInstalledUwpApps(24);
    if (!stopRequested_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(mutex_);
        winAppsDirLower_ = winAppsLower;
        for (auto& app : apps) {
            if (app.pkgFamily.empty() || app.displayName.empty()) {
                if (app.icon) DestroyIcon(app.icon);
                continue;
            }
            std::wstring key = L"appx:" + app.pkgFamily;
            auto& st = entries_[key];
            if (st.name.empty()) {
                st.name = app.displayName;
            }
            if (!st.icon && app.icon) {
                st.icon = app.icon;
                ownedIcons_.push_back(app.icon);
                app.icon = nullptr;
            } else if (app.icon) {
                DestroyIcon(app.icon);
                app.icon = nullptr;
            }
            st.isStoreApp = true;
        }
    } else {
        FreeUwpAppEntries(apps);
    }

    if (!stopRequested_.load(std::memory_order_acquire)) {
        LoadSrumHistory();
    }

    if (initCom) {
        CoUninitialize();
    }
}

void AppHistoryManager::LoadSrumHistory() {
    HMODULE hSrum = LoadLibraryW(L"srumapi.dll");
    if (!hSrum) return;

    auto pQuery = reinterpret_cast<PFN_SruQueryStats>(GetProcAddress(hSrum, "SruQueryStats"));
    auto pFree = reinterpret_cast<PFN_SruFreeRecordSet>(GetProcAddress(hSrum, "SruFreeRecordSet"));
    if (!pQuery || !pFree) {
        FreeLibrary(hSrum);
        return;
    }

    FILETIME ftNow{};
    GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER uNow{};
    uNow.LowPart = ftNow.dwLowDateTime;
    uNow.HighPart = ftNow.dwHighDateTime;

    // Last 30 days (matching native Task Manager "last month" window)
    constexpr ULONGLONG k30Days100ns = 30ULL * 24ULL * 3600ULL * 10000000ULL;
    ULARGE_INTEGER uStart{};
    uStart.QuadPart = (uNow.QuadPart > k30Days100ns) ? (uNow.QuadPart - k30Days100ns) : 0ULL;

    FILETIME ftStart{};
    ftStart.dwLowDateTime = uStart.LowPart;
    ftStart.dwHighDateTime = uStart.HighPart;

    SYSTEMTIME stStart{}, stEnd{};
    FileTimeToSystemTime(&ftStart, &stStart);
    FileTimeToSystemTime(&ftNow, &stEnd);

    struct SrumAccum {
        double cpuSec = 0.0;
        uint64_t netBytes = 0;
        uint64_t meteredBytes = 0;
        uint64_t tileBytes = 0;
        std::wstring dosPath;
        bool isStoreApp = false;
    };

    const auto dosMap = BuildDosDriveMap();
    const double cyclesPerSec = static_cast<double>(ReadCpuNominalMhz()) * 1000000.0;

    std::unordered_map<std::wstring, SrumAccum> srumMap;
    SYSTEMTIME earliestTs{};
    bool hasEarliest = false;

    auto recordDate = [&](const SYSTEMTIME& ts) {
        if (ts.wYear < 2000 || ts.wYear > 2100 || ts.wMonth < 1 || ts.wMonth > 12 || ts.wDay < 1 || ts.wDay > 31) {
            return;
        }
        if (!hasEarliest || CompareSysTimeDate(ts, earliestTs) < 0) {
            earliestTs = ts;
            hasEarliest = true;
        }
    };

    auto mapAppKey = [&](const SRU_STATS_RECORD_NATIVE& rec, std::wstring& outDosPath, bool& outIsStore) -> std::wstring {
        outDosPath.clear();
        outIsStore = false;
        if (!rec.appName || !rec.appName[0]) return L"";
        std::wstring raw = rec.appName;
        if (raw.find(L'\\') == std::wstring::npos && raw.find(L'_') != std::wstring::npos) {
            std::wstring fam = ExtractSrumPkgFamily(raw);
            if (!fam.empty()) {
                outIsStore = true;
                return L"appx:" + fam;
            }
        }
        std::wstring low = LowerStr(raw);
        if (low.rfind(L"\\device\\", 0) == 0) {
            std::wstring dos = TranslateNtPath(low, dosMap);
            std::wstring fam = ExtractPackageFamilyFromWindowsAppsPath(dos);
            if (!fam.empty()) {
                outIsStore = true;
                return L"appx:" + fam;
            }
            outDosPath = dos;
            return dos;
        }
        return low;
    };

    // Provider 1: Application Resource Usage (cols 0=FG cycles, 1=BG cycles)
    if (!stopRequested_.load(std::memory_order_acquire)) {
        SRU_STATS_RECORD_SET_NATIVE* rs = nullptr;
        if (pQuery(1, &stStart, &stEnd, 1 /*SRU_QUERY_FLAG_CURRENT_USER*/, &rs) == ERROR_SUCCESS && rs) {
            for (DWORD i = 0; i < rs->nrRecords; ++i) {
                const auto& r = rs->records[i];
                if (!r.columns || r.nrColumns < 2) continue;
                recordDate(r.timestamp);
                std::wstring dosPath;
                bool isStore = false;
                std::wstring key = mapAppKey(r, dosPath, isStore);
                if (key.empty()) continue;
                uint64_t fg = ReadColU64(r.columns[0]);
                uint64_t bg = ReadColU64(r.columns[1]);
                auto& acc = srumMap[key];
                if (isStore) acc.isStoreApp = true;
                if (acc.dosPath.empty() && !dosPath.empty()) acc.dosPath = std::move(dosPath);
                acc.cpuSec += static_cast<double>(fg + bg) / cyclesPerSec;
            }
            pFree(rs);
        }
    }

    // Provider 0: Network Data Usage (col 2=L2ProfileFlags, col 3=BytesSent, col 4=BytesRecvd)
    if (!stopRequested_.load(std::memory_order_acquire)) {
        SRU_STATS_RECORD_SET_NATIVE* rs = nullptr;
        if (pQuery(0, &stStart, &stEnd, 1, &rs) == ERROR_SUCCESS && rs) {
            for (DWORD i = 0; i < rs->nrRecords; ++i) {
                const auto& r = rs->records[i];
                if (!r.columns || r.nrColumns < 5) continue;
                recordDate(r.timestamp);
                std::wstring dosPath;
                bool isStore = false;
                std::wstring key = mapAppKey(r, dosPath, isStore);
                if (key.empty()) continue;
                uint64_t flags = ReadColU64(r.columns[2]);
                uint64_t sent = ReadColU64(r.columns[3]);
                uint64_t recv = ReadColU64(r.columns[4]);
                uint64_t tot = sent + recv;
                auto& acc = srumMap[key];
                if (isStore) acc.isStoreApp = true;
                if (acc.dosPath.empty() && !dosPath.empty()) acc.dosPath = std::move(dosPath);
                acc.netBytes += tot;
                if ((flags & 1ULL) != 0) acc.meteredBytes += tot;
            }
            pFree(rs);
        }
    }

    // Provider 2: Windows Push Notification Data (col 1=PayloadSize)
    if (!stopRequested_.load(std::memory_order_acquire)) {
        SRU_STATS_RECORD_SET_NATIVE* rs = nullptr;
        if (pQuery(2, &stStart, &stEnd, 1, &rs) == ERROR_SUCCESS && rs) {
            for (DWORD i = 0; i < rs->nrRecords; ++i) {
                const auto& r = rs->records[i];
                if (!r.columns || r.nrColumns < 2) continue;
                std::wstring dosPath;
                bool isStore = false;
                std::wstring key = mapAppKey(r, dosPath, isStore);
                if (key.empty()) continue;
                uint64_t payload = ReadColU64(r.columns[1]);
                auto& acc = srumMap[key];
                if (isStore) acc.isStoreApp = true;
                acc.tileBytes += payload;
            }
            pFree(rs);
        }
    }

    FreeLibrary(hSrum);

    if (stopRequested_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (clearedManually_) {
        // User clicked Delete History while SRUM was still loading in background
        return;
    }

    if (hasEarliest) {
        sinceDateStr_ = FormatSysDate(earliestTs);
    }

    for (const auto& kv : srumMap) {
        const std::wstring& key = kv.first;
        const SrumAccum& acc = kv.second;

        auto it = entries_.find(key);
        if (it == entries_.end()) {
            // Only add unknown Store package keys if not a hidden system host (must be in AppsFolder if Store),
            // whereas Win32 executables go into entries_ with isStoreApp=false for "Show all processes".
            if (acc.isStoreApp) continue;
            if (acc.cpuSec < 0.5 && acc.netBytes == 0 && acc.tileBytes == 0) continue;

            auto& st = entries_[key];
            std::wstring display = !acc.dosPath.empty() ? acc.dosPath : key;
            const wchar_t* fname = PathFindFileNameW(display.c_str());
            st.name = (fname && fname[0]) ? fname : display;
            st.imagePath = acc.dosPath;
            st.isStoreApp = false;
            st.srumCpuSeconds = acc.cpuSec;
            st.srumNetBytes = acc.netBytes;
            st.srumMeteredBytes = acc.meteredBytes;
            st.srumTileBytes = acc.tileBytes;
        } else {
            auto& st = it->second;
            st.srumCpuSeconds = acc.cpuSec;
            st.srumNetBytes = acc.netBytes;
            st.srumMeteredBytes = acc.meteredBytes;
            st.srumTileBytes = acc.tileBytes;
            if (st.imagePath.empty() && !acc.dosPath.empty()) st.imagePath = acc.dosPath;
        }
    }
}

void AppHistoryManager::Accumulate(const std::vector<ProcessInfo>& procs, double dtSec) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : procs) {
        std::wstring key;
        bool isUwp = false;

        if (!p.imagePath.empty()) {
            std::wstring pkgFamily = ExtractPackageFamilyFromWindowsAppsPath(p.imagePath);
            if (!pkgFamily.empty()) {
                std::wstring appxKey = L"appx:" + pkgFamily;
                if (entries_.find(appxKey) != entries_.end()) {
                    key = appxKey;
                    isUwp = true;
                } else {
                    std::wstring pathLower = LowerStr(p.imagePath);
                    if (!winAppsDirLower_.empty() && pathLower.rfind(winAppsDirLower_, 0) == 0) {
                        key = appxKey;
                        isUwp = true;
                    } else {
                        key = pathLower;
                    }
                }
            } else {
                key = LowerStr(p.imagePath);
            }
        } else {
            key = LowerStr(p.name);
        }

        if (key.empty()) continue;

        if (isUwp && entries_.find(key) == entries_.end()) {
            continue;
        }

        auto& st = entries_[key];
        if (st.name.empty()) {
            st.name = !p.description.empty() ? p.description : p.name;
        }
        if (st.imagePath.empty()) st.imagePath = p.imagePath;
        if (!st.icon && p.icon) st.icon = p.icon;
        if (isUwp) st.isStoreApp = true;

        auto prevIt = st.lastSeenCpu100ns.find(p.pid);
        if (prevIt != st.lastSeenCpu100ns.end()) {
            if (p.totalCpuTime100ns >= prevIt->second) {
                uint64_t delta100ns = p.totalCpuTime100ns - prevIt->second;
                st.cpuSecondsAccum += static_cast<double>(delta100ns) / 10000000.0;
            }
        }
        st.lastSeenCpu100ns[p.pid] = p.totalCpuTime100ns;
        st.networkBytesAccum += p.netBytesPerSec * dtSec;
    }
}

void AppHistoryManager::DeleteHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    clearedManually_ = true;
    SYSTEMTIME stNow{};
    GetLocalTime(&stNow);
    sinceDateStr_ = FormatSysDate(stNow);
    GetSystemTimeAsFileTime(&clearCutoffFt_);

    for (auto& kv : entries_) {
        kv.second.srumCpuSeconds = 0.0;
        kv.second.cpuSecondsAccum = 0.0;
        kv.second.srumNetBytes = 0;
        kv.second.networkBytesAccum = 0.0;
        kv.second.srumMeteredBytes = 0;
        kv.second.meteredBytes = 0;
        kv.second.srumTileBytes = 0;
        kv.second.tileUpdatesBytes = 0;
    }
}

std::wstring AppHistoryManager::GetSinceDate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sinceDateStr_;
}

std::vector<AppHistoryEntry> AppHistoryManager::GetEntries(bool showAllProcesses) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AppHistoryEntry> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) {
        const auto& st = kv.second;
        if (!showAllProcesses && !st.isStoreApp) continue;
        AppHistoryEntry e;
        e.key = kv.first;
        e.name = st.name;
        e.imagePath = st.imagePath;
        e.icon = st.icon;
        e.isStoreApp = st.isStoreApp;
        e.cpuTimeSeconds = static_cast<uint64_t>(st.srumCpuSeconds + st.cpuSecondsAccum);
        e.networkBytes = st.srumNetBytes + static_cast<uint64_t>(st.networkBytesAccum);
        e.meteredBytes = st.srumMeteredBytes + st.meteredBytes;
        e.tileUpdatesBytes = st.srumTileBytes + st.tileUpdatesBytes;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const AppHistoryEntry& a, const AppHistoryEntry& b) {
        if (a.cpuTimeSeconds != b.cpuTimeSeconds) return a.cpuTimeSeconds > b.cpuTimeSeconds;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

} // namespace taskman
