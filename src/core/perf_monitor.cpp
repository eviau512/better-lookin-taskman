#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <winternl.h>
#include "perf_monitor.h"
#include "format_util.h"
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <setupapi.h>
#include <devguid.h>
#include <winioctl.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <unordered_map>
#include <set>

namespace taskman {

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

struct NT_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

struct NT_PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

struct NT_SYSTEM_MEMORY_LIST_INFORMATION {
    ULONG_PTR ZeroPageCount;
    ULONG_PTR FreePageCount;
    ULONG_PTR ModifiedPageCount;
    ULONG_PTR ModifiedNoWritePageCount;
    ULONG_PTR BadPageCount;
    ULONG_PTR PageCountByPriority[8];
    ULONG_PTR RepurposedPagesByPriority[8];
    ULONG_PTR ModifiedPageCountPageFile;
};

using PFN_NtQuerySystemInformation = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

// D3DKMT dynamic structs for GPU temperature
struct D3DKMT_ADAPTERINFO_TEMP {
    UINT hAdapter;
    LUID AdapterLuid;
    ULONG NumOfSources;
    BOOL bPresentMoveRegionsPreferred;
};
struct D3DKMT_ENUMADAPTERS2_TEMP {
    ULONG NumAdapters;
    D3DKMT_ADAPTERINFO_TEMP* pAdapters;
};
struct D3DKMT_QUERYADAPTERINFO_TEMP {
    UINT hAdapter;
    UINT Type;
    VOID* pPrivateDriverData;
    UINT PrivateDriverDataSize;
};
struct D3DKMT_CLOSEADAPTER_TEMP {
    UINT hAdapter;
};
struct D3DKMT_ADAPTER_PERFDATA_TEMP {
    UINT32 PhysicalAdapterIndex;
    UINT64 MemoryFrequency;
    UINT64 MaxMemoryFrequency;
    UINT64 MaxMemoryFrequencyOC;
    UINT64 MemoryBandwidth;
    UINT64 PCIEBandwidth;
    ULONG  FanRPM;
    ULONG  Power;
    ULONG  Temperature;
    UCHAR  PowerStateOverride;
};

using PFN_D3DKMTEnumAdapters2 = NTSTATUS(WINAPI*)(D3DKMT_ENUMADAPTERS2_TEMP*);
using PFN_D3DKMTQueryAdapterInfo = NTSTATUS(WINAPI*)(const D3DKMT_QUERYADAPTERINFO_TEMP*);
using PFN_D3DKMTCloseAdapter = NTSTATUS(WINAPI*)(const D3DKMT_CLOSEADAPTER_TEMP*);

template <typename Fn>
static void ForEachD3dkmtAdapter(Fn&& fn) {
    HMODULE hGdi = GetModuleHandleW(L"gdi32.dll");
    auto pEnum = hGdi ? reinterpret_cast<PFN_D3DKMTEnumAdapters2>(GetProcAddress(hGdi, "D3DKMTEnumAdapters2")) : nullptr;
    auto pQuery = hGdi ? reinterpret_cast<PFN_D3DKMTQueryAdapterInfo>(GetProcAddress(hGdi, "D3DKMTQueryAdapterInfo")) : nullptr;
    auto pClose = hGdi ? reinterpret_cast<PFN_D3DKMTCloseAdapter>(GetProcAddress(hGdi, "D3DKMTCloseAdapter")) : nullptr;
    if (!pEnum || !pQuery || !pClose) return;

    D3DKMT_ENUMADAPTERS2_TEMP ea{};
    if (!NT_SUCCESS(pEnum(&ea)) || ea.NumAdapters == 0) return;

    std::vector<D3DKMT_ADAPTERINFO_TEMP> list(ea.NumAdapters);
    ea.pAdapters = list.data();
    if (!NT_SUCCESS(pEnum(&ea))) return;

    for (ULONG k = 0; k < ea.NumAdapters; ++k) {
        fn(list[k], pQuery);
        D3DKMT_CLOSEADAPTER_TEMP ca{ list[k].hAdapter };
        pClose(&ca);
    }
}

static uint64_t GetNowUtcMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart / 10000ULL;
}

static std::wstring GetPerfHistoryFilePath() {
    wchar_t appData[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, ARRAYSIZE(appData));
    if (len == 0 || len >= ARRAYSIZE(appData)) return L"";
    std::wstring dir = std::wstring(appData) + L"\\BetterWin10Taskman";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\perf_history.bin";
}

static std::wstring TrimStr(std::wstring s) {
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

static std::wstring AnsiToWide(const char* s) {
    if (!s || !s[0]) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring out(static_cast<size_t>(n) - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], n);
    return TrimStr(out);
}

PerfMonitor::PerfMonitor() = default;

PerfMonitor::~PerfMonitor() {
    Shutdown();
}

void PerfMonitor::Init() {
    InitCpuHardware();
    InitMemoryHardware();
    InitDisks();
    InitNetworks();
    InitGpus();
    InitSharedHistory();
    SyncFromSharedRing(true);
    RebuildHistoriesForSpeed(activeSpeed_);
    if (!pdhInitThread_.joinable() && !pdhReady_.load(std::memory_order_acquire)) {
        pdhInitThread_ = std::thread([this]() { InitPdh(); });
    }
    prevSampleTickMs_ = GetTickCount64();
}

void PerfMonitor::Shutdown() {
    if (pdhInitThread_.joinable()) {
        pdhInitThread_.join();
    }
    pdhReady_.store(false, std::memory_order_release);
    if (pdhQuery_) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(pdhQuery_));
        pdhQuery_ = nullptr;
        pdhCpuPerf_ = nullptr;
        pdhDiskTime_ = nullptr;
        pdhDiskRead_ = nullptr;
        pdhDiskWrite_ = nullptr;
        pdhDiskAvgSec_ = nullptr;
        pdhGpuEngine_ = nullptr;
        pdhGpuDed_ = nullptr;
        pdhGpuShared_ = nullptr;
    }
    if (sharedRing_) {
        SharedPerfRing snapshot{};
        bool haveSnap = false;
        if (hRingMutex_) WaitForSingleObject(hRingMutex_, 50);
        if (sharedRing_->magic == kPerfRingMagic && sharedRing_->version == kPerfRingVersion) {
            snapshot = *sharedRing_;
            haveSnap = true;
        }
        if (hRingMutex_) ReleaseMutex(hRingMutex_);
        if (haveSnap) SaveRingToDisk(snapshot);
        UnmapViewOfFile(sharedRing_);
        sharedRing_ = nullptr;
    }
    if (hMapFile_) {
        CloseHandle(hMapFile_);
        hMapFile_ = nullptr;
    }
    if (hRingMutex_) {
        CloseHandle(hRingMutex_);
        hRingMutex_ = nullptr;
    }
}

void PerfMonitor::InitSharedHistory() {
    if (!hRingMutex_) {
        hRingMutex_ = CreateMutexW(nullptr, FALSE, L"Local\\BetterWin10TaskmanPerfRingLock");
    }
    if (!hMapFile_) {
        hMapFile_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, static_cast<DWORD>(sizeof(SharedPerfRing)),
            L"Local\\BetterWin10TaskmanPerfRing");
        if (hMapFile_) {
            bool createdNew = (GetLastError() != ERROR_ALREADY_EXISTS);
            sharedRing_ = static_cast<SharedPerfRing*>(
                MapViewOfFile(hMapFile_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedPerfRing)));
            if (sharedRing_) {
                if (hRingMutex_) WaitForSingleObject(hRingMutex_, 50);
                if (createdNew || sharedRing_->magic != kPerfRingMagic || sharedRing_->version != kPerfRingVersion) {
                    std::memset(sharedRing_, 0, sizeof(SharedPerfRing));
                    sharedRing_->magic = kPerfRingMagic;
                    sharedRing_->version = kPerfRingVersion;
                    LoadRingFromDisk(*sharedRing_);
                }
                if (hRingMutex_) ReleaseMutex(hRingMutex_);
            }
        }
    }
}

void PerfMonitor::LoadRingFromDisk(SharedPerfRing& ring) {
    std::wstring path = GetPerfHistoryFilePath();
    if (path.empty()) return;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    SharedPerfRing tmp{};
    DWORD readBytes = 0;
    bool ok = ReadFile(hFile, &tmp, static_cast<DWORD>(sizeof(SharedPerfRing)), &readBytes, nullptr) != FALSE;
    CloseHandle(hFile);
    if (ok && readBytes == sizeof(SharedPerfRing) &&
        tmp.magic == kPerfRingMagic && tmp.version == kPerfRingVersion) {
        ring = tmp;
    }
}

void PerfMonitor::SaveRingToDisk(const SharedPerfRing& ring) {
    std::wstring path = GetPerfHistoryFilePath();
    if (path.empty()) return;
    std::wstring tmpPath = path + L".tmp";
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, &ring, static_cast<DWORD>(sizeof(SharedPerfRing)), &written, nullptr);
    CloseHandle(hFile);
    if (ok && written == sizeof(SharedPerfRing)) {
        MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    } else {
        DeleteFileW(tmpPath.c_str());
    }
}

void PerfMonitor::SyncFromSharedRing(bool updateLiveReadout) {
    cachedSlots_.clear();
    uint64_t nowMs = GetNowUtcMs();
    constexpr uint64_t kMaxAgeMs = 245000ULL;

    SharedPerfRing snap{};
    bool valid = false;
    if (sharedRing_) {
        if (hRingMutex_) WaitForSingleObject(hRingMutex_, 50);
        if (sharedRing_->magic == kPerfRingMagic && sharedRing_->version == kPerfRingVersion) {
            snap = *sharedRing_;
            valid = true;
        }
        if (hRingMutex_) ReleaseMutex(hRingMutex_);
    }
    if (!valid) return;

    cachedSlots_.reserve(kRawHistoryLen);
    for (int i = 0; i < kRawHistoryLen; ++i) {
        const auto& sl = snap.slots[i];
        if (sl.timestampMs == 0) continue;
        if (sl.timestampMs > nowMs + 5000ULL) continue;
        if (nowMs > sl.timestampMs && (nowMs - sl.timestampMs) > kMaxAgeMs) continue;
        cachedSlots_.push_back(sl);
    }
    std::sort(cachedSlots_.begin(), cachedSlots_.end(), [](const RawPerfSlot& a, const RawPerfSlot& b) {
        return a.timestampMs < b.timestampMs;
    });

    if (updateLiveReadout && !cachedSlots_.empty() && (nowMs - cachedSlots_.back().timestampMs) <= 4000ULL) {
        const auto& latest = cachedSlots_.back();
        cpu_.utilization = latest.cpuUtil;
        cpu_.kernelUtilization = latest.cpuKernel;
        int nc = (std::min)(static_cast<int>(cpu_.coreCurrent.size()), kMaxRawCores);
        for (int i = 0; i < nc; ++i) cpu_.coreCurrent[static_cast<size_t>(i)] = latest.coreUtil[i];
        mem_.utilization = latest.memUtil;
        int nd = (std::min)(static_cast<int>(disks_.size()), kMaxRawDisks);
        for (int i = 0; i < nd; ++i) {
            disks_[static_cast<size_t>(i)].activeTimePct = latest.diskActive[i];
            disks_[static_cast<size_t>(i)].readBytesPerSec = latest.diskReadMBs[i] * 1024.0 * 1024.0;
            disks_[static_cast<size_t>(i)].writeBytesPerSec = latest.diskWriteMBs[i] * 1024.0 * 1024.0;
        }
        int nn = (std::min)(static_cast<int>(nets_.size()), kMaxRawNets);
        for (int i = 0; i < nn; ++i) {
            nets_[static_cast<size_t>(i)].sendBytesPerSec = latest.netSendMbps[i] * 1000000.0 / 8.0;
            nets_[static_cast<size_t>(i)].recvBytesPerSec = latest.netRecvMbps[i] * 1000000.0 / 8.0;
        }
        int ng = (std::min)(static_cast<int>(gpus_.size()), kMaxRawGpus);
        for (int i = 0; i < ng; ++i) {
            gpus_[static_cast<size_t>(i)].utilization = latest.gpuOverall[i];
        }
    }
}

void PerfMonitor::AppendSlotToSharedRing(const RawPerfSlot& slot) {
    if (!sharedRing_) {
        if (cachedSlots_.size() >= kRawHistoryLen) {
            cachedSlots_.erase(cachedSlots_.begin());
        }
        cachedSlots_.push_back(slot);
        return;
    }

    SharedPerfRing persistCopy{};
    bool shouldPersist = false;
    if (hRingMutex_) WaitForSingleObject(hRingMutex_, 50);
    if (sharedRing_->magic == kPerfRingMagic && sharedRing_->version == kPerfRingVersion) {
        uint64_t lastTs = 0;
        if (sharedRing_->writePos > 0) {
            uint32_t lastIdx = (sharedRing_->writePos - 1) % kRawHistoryLen;
            lastTs = sharedRing_->slots[lastIdx].timestampMs;
        }
        // Keep shared sampling timeline cleanly spaced (~500ms cadence) when daemon + UI run concurrently
        if (lastTs == 0 || slot.timestampMs >= lastTs + 380ULL || slot.timestampMs + 2000ULL < lastTs) {
            uint32_t idx = sharedRing_->writePos % kRawHistoryLen;
            sharedRing_->slots[idx] = slot;
            sharedRing_->writePos++;
            if ((++savePersistCounter_ % 10u) == 0u) {
                persistCopy = *sharedRing_;
                shouldPersist = true;
            }
        }
    }
    if (hRingMutex_) ReleaseMutex(hRingMutex_);

    if (shouldPersist) {
        SaveRingToDisk(persistCopy);
    }
    SyncFromSharedRing(false);
}

void PerfMonitor::SetUpdateSpeed(UpdateSpeed speed) {
    if (speed != UpdateSpeed::Paused) {
        activeSpeed_ = speed;
    }
    SyncFromSharedRing(false);
    RebuildHistoriesForSpeed(activeSpeed_);
}

void PerfMonitor::RebuildHistoriesForSpeed(UpdateSpeed speed) {
    if (speed == UpdateSpeed::Paused) speed = activeSpeed_;
    uint64_t stepMs = 1000ULL;
    if (speed == UpdateSpeed::High) stepMs = 500ULL;
    else if (speed == UpdateSpeed::Low) stepMs = 4000ULL;

    auto resetZeroVec = [](std::vector<double>& v) {
        v.assign(kHistoryLen, 0.0);
    };
    resetZeroVec(cpu_.history);
    resetZeroVec(cpu_.kernelHistory);
    for (auto& ch : cpu_.coreHistories) resetZeroVec(ch);
    resetZeroVec(mem_.history);
    for (auto& d : disks_) {
        resetZeroVec(d.activeHistory);
        resetZeroVec(d.readHistory);
        resetZeroVec(d.writeHistory);
    }
    for (auto& n : nets_) {
        resetZeroVec(n.sendHistory);
        resetZeroVec(n.recvHistory);
    }
    for (auto& g : gpus_) {
        resetZeroVec(g.hist3D);
        resetZeroVec(g.histCopy);
        resetZeroVec(g.histVideoEncode);
        resetZeroVec(g.histVideoDecode);
        resetZeroVec(g.histOverall);
        resetZeroVec(g.histDedMem);
    }

    if (cachedSlots_.empty()) return;

    uint64_t nowMs = GetNowUtcMs();
    uint64_t newestTs = cachedSlots_.back().timestampMs;
    uint64_t oldestTs = cachedSlots_.front().timestampMs;
    uint64_t endTarget = (nowMs >= newestTs && (nowMs - newestTs) <= 2500ULL) ? newestTs : nowMs;
    int64_t halfW = static_cast<int64_t>(stepMs / 2ULL);

    int numCores = (std::min)(static_cast<int>(cpu_.coreHistories.size()), kMaxRawCores);
    int numDisks = (std::min)(static_cast<int>(disks_.size()), kMaxRawDisks);
    int numNets  = (std::min)(static_cast<int>(nets_.size()), kMaxRawNets);
    int numGpus  = (std::min)(static_cast<int>(gpus_.size()), kMaxRawGpus);

    for (int pt = 0; pt < kHistoryLen; ++pt) {
        int64_t backBuckets = static_cast<int64_t>(kHistoryLen - 1 - pt);
        int64_t center = static_cast<int64_t>(endTarget) - backBuckets * static_cast<int64_t>(stepMs);
        int64_t winLo = center - halfW;
        int64_t winHi = (pt == kHistoryLen - 1) ? (static_cast<int64_t>(endTarget) + 1500LL) : (center + halfW);

        double sCpu = 0.0, sKern = 0.0, sMem = 0.0;
        double sCore[kMaxRawCores] = {};
        double sDiskAct[kMaxRawDisks] = {}, sDiskRd[kMaxRawDisks] = {}, sDiskWr[kMaxRawDisks] = {};
        double sNetTx[kMaxRawNets] = {}, sNetRx[kMaxRawNets] = {};
        double sGpu3d[kMaxRawGpus] = {}, sGpuCp[kMaxRawGpus] = {}, sGpuVe[kMaxRawGpus] = {},
               sGpuVd[kMaxRawGpus] = {}, sGpuOv[kMaxRawGpus] = {}, sGpuDm[kMaxRawGpus] = {};
        int count = 0;

        auto accumSlot = [&](const RawPerfSlot& sl) {
            sCpu += sl.cpuUtil;
            sKern += sl.cpuKernel;
            sMem += sl.memUtil;
            for (int c = 0; c < numCores; ++c) sCore[c] += sl.coreUtil[c];
            for (int d = 0; d < numDisks; ++d) {
                sDiskAct[d] += sl.diskActive[d];
                sDiskRd[d]  += sl.diskReadMBs[d];
                sDiskWr[d]  += sl.diskWriteMBs[d];
            }
            for (int n = 0; n < numNets; ++n) {
                sNetTx[n] += sl.netSendMbps[n];
                sNetRx[n] += sl.netRecvMbps[n];
            }
            for (int g = 0; g < numGpus; ++g) {
                sGpu3d[g] += sl.gpu3D[g];
                sGpuCp[g] += sl.gpuCopy[g];
                sGpuVe[g] += sl.gpuVEnc[g];
                sGpuVd[g] += sl.gpuVDec[g];
                sGpuOv[g] += sl.gpuOverall[g];
                sGpuDm[g] += sl.gpuDedMem[g];
            }
            count++;
        };

        for (const auto& sl : cachedSlots_) {
            int64_t ts = static_cast<int64_t>(sl.timestampMs);
            if (ts > winLo && ts <= winHi) {
                accumSlot(sl);
            }
        }

        // Prevent single-tick 500ms timer jitter holes inside active recorded window
        if (count == 0 && center >= static_cast<int64_t>(oldestTs) - halfW &&
            center <= static_cast<int64_t>(newestTs) + halfW) {
            const RawPerfSlot* nearest = nullptr;
            int64_t bestDist = static_cast<int64_t>(stepMs) + 450LL;
            for (const auto& sl : cachedSlots_) {
                int64_t dist = std::llabs(static_cast<int64_t>(sl.timestampMs) - center);
                if (dist < bestDist) {
                    bestDist = dist;
                    nearest = &sl;
                }
            }
            if (nearest) accumSlot(*nearest);
        }

        if (count > 0) {
            double inv = 1.0 / static_cast<double>(count);
            size_t idx = static_cast<size_t>(pt);
            cpu_.history[idx] = sCpu * inv;
            cpu_.kernelHistory[idx] = sKern * inv;
            mem_.history[idx] = sMem * inv;
            for (int c = 0; c < numCores; ++c) cpu_.coreHistories[static_cast<size_t>(c)][idx] = sCore[c] * inv;
            for (int d = 0; d < numDisks; ++d) {
                disks_[static_cast<size_t>(d)].activeHistory[idx] = sDiskAct[d] * inv;
                disks_[static_cast<size_t>(d)].readHistory[idx]   = sDiskRd[d] * inv;
                disks_[static_cast<size_t>(d)].writeHistory[idx]  = sDiskWr[d] * inv;
            }
            for (int n = 0; n < numNets; ++n) {
                nets_[static_cast<size_t>(n)].sendHistory[idx] = sNetTx[n] * inv;
                nets_[static_cast<size_t>(n)].recvHistory[idx] = sNetRx[n] * inv;
            }
            for (int g = 0; g < numGpus; ++g) {
                gpus_[static_cast<size_t>(g)].hist3D[idx]          = sGpu3d[g] * inv;
                gpus_[static_cast<size_t>(g)].histCopy[idx]        = sGpuCp[g] * inv;
                gpus_[static_cast<size_t>(g)].histVideoEncode[idx] = sGpuVe[g] * inv;
                gpus_[static_cast<size_t>(g)].histVideoDecode[idx] = sGpuVd[g] * inv;
                gpus_[static_cast<size_t>(g)].histOverall[idx]     = sGpuOv[g] * inv;
                gpus_[static_cast<size_t>(g)].histDedMem[idx]      = sGpuDm[g] * inv;
            }
        }
    }
}

void PerfMonitor::InitCpuHardware() {
    cpu_.history.assign(kHistoryLen, 0.0);
    cpu_.kernelHistory.assign(kHistoryLen, 0.0);

    // CPU Brand String
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[256] = {};
        DWORD sz = sizeof(buf);
        if (RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, nullptr,
                             reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS) {
            cpu_.name = TrimStr(buf);
        }
        DWORD mhz = 0;
        sz = sizeof(mhz);
        if (RegQueryValueExW(hKey, L"~MHz", nullptr, nullptr,
                             reinterpret_cast<BYTE*>(&mhz), &sz) == ERROR_SUCCESS && mhz > 0) {
            cpu_.baseSpeedGhz = static_cast<double>(mhz) / 1000.0;
        }
        RegCloseKey(hKey);
    }

    // Topology & caches via GetLogicalProcessorInformationEx
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetLogicalProcessorInformationEx(RelationAll,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
            int sockets = 0, cores = 0, logical = 0;
            uint64_t l1 = 0, l2 = 0, l3 = 0;
            std::set<ULONGLONG> seenL3;
            const BYTE* p = buf.data();
            while (p < buf.data() + len) {
                auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
                if (info->Relationship == RelationProcessorPackage) {
                    sockets++;
                } else if (info->Relationship == RelationProcessorCore) {
                    cores++;
                    for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
                        KAFFINITY mask = info->Processor.GroupMask[g].Mask;
                        while (mask) {
                            logical += (mask & 1) ? 1 : 0;
                            mask >>= 1;
                        }
                    }
                } else if (info->Relationship == RelationCache) {
                    const CACHE_RELATIONSHIP& c = info->Cache;
                    if (c.Level == 1) l1 += c.CacheSize;
                    else if (c.Level == 2) l2 += c.CacheSize;
                    else if (c.Level == 3) {
                        ULONGLONG km = c.GroupMask.Mask;
                        if (seenL3.insert(km).second) l3 += c.CacheSize;
                    }
                }
                p += info->Size;
            }
            if (sockets > 0) cpu_.sockets = sockets;
            if (cores > 0) cpu_.cores = cores;
            if (logical > 0) cpu_.logicalProcessors = logical;
            cpu_.l1CacheBytes = l1;
            cpu_.l2CacheBytes = l2;
            cpu_.l3CacheBytes = l3;
        }
    }

    if (cpu_.logicalProcessors <= 0) {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        cpu_.logicalProcessors = static_cast<int>(si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1);
    }

    cpu_.coreHistories.assign(static_cast<size_t>(cpu_.logicalProcessors),
                              std::vector<double>(kHistoryLen, 0.0));
    cpu_.coreCurrent.assign(static_cast<size_t>(cpu_.logicalProcessors), 0.0);
    prevCoreTimes_.resize(static_cast<size_t>(cpu_.logicalProcessors));

    // Base speed from CallNtPowerInformation
    std::vector<NT_PROCESSOR_POWER_INFORMATION> ppi(static_cast<size_t>(cpu_.logicalProcessors));
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(),
                               static_cast<ULONG>(ppi.size() * sizeof(NT_PROCESSOR_POWER_INFORMATION))) == 0) {
        if (ppi[0].MaxMhz > 0) {
            cpu_.baseSpeedGhz = static_cast<double>(ppi[0].MaxMhz) / 1000.0;
        }
        cpu_.currentSpeedGhz = static_cast<double>(ppi[0].CurrentMhz) / 1000.0;
    }

    cpu_.virtualization = IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED) != FALSE;

    // Initial core timing baseline
    static auto pfnNtQuerySys = reinterpret_cast<PFN_NtQuerySystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (pfnNtQuerySys) {
        std::vector<NT_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> sppi(static_cast<size_t>(cpu_.logicalProcessors));
        ULONG ret = 0;
        if (NT_SUCCESS(pfnNtQuerySys(8 /*SystemProcessorPerformanceInformation*/, sppi.data(),
                                     static_cast<ULONG>(sppi.size() * sizeof(NT_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)), &ret))) {
            for (int i = 0; i < cpu_.logicalProcessors; ++i) {
                prevCoreTimes_[static_cast<size_t>(i)] = {
                    sppi[static_cast<size_t>(i)].IdleTime.QuadPart,
                    sppi[static_cast<size_t>(i)].KernelTime.QuadPart,
                    sppi[static_cast<size_t>(i)].UserTime.QuadPart
                };
            }
        }
    }
}

void PerfMonitor::InitMemoryHardware() {
    mem_.history.assign(kHistoryLen, 0.0);
    mem_.formFactor = L"DIMM";

    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        mem_.totalBytes = ms.ullTotalPhys;
    }

    // Read installed memory, speed, slots, form factor from SMBIOS Type 17
    DWORD sz = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (sz > 8) {
        std::vector<BYTE> buf(sz);
        if (GetSystemFirmwareTable('RSMB', 0, buf.data(), sz) == sz) {
            struct RawSMBIOS {
                BYTE used20, major, minor, dmiRev;
                DWORD length;
                BYTE data[1];
            };
            const auto* raw = reinterpret_cast<const RawSMBIOS*>(buf.data());
            const BYTE* p = raw->data;
            const BYTE* end = raw->data + raw->length;
            uint64_t totalInstalledMb = 0;
            int slotsTotal = 0, slotsUsed = 0, maxSpeed = 0;
            BYTE bestForm = 0;

            while (p + 4 <= end) {
                BYTE type = p[0];
                BYTE hlen = p[1];
                if (hlen < 4 || p + hlen > end) break;
                if (type == 17 && hlen >= 0x17) {
                    slotsTotal++;
                    WORD sizeField = *reinterpret_cast<const WORD*>(p + 0x0C);
                    uint64_t mb = 0;
                    if (sizeField != 0 && sizeField != 0xFFFF) {
                        if (sizeField == 0x7FFF && hlen >= 0x20) {
                            mb = (*reinterpret_cast<const DWORD*>(p + 0x1C)) & 0x7FFFFFFF;
                        } else if (sizeField & 0x8000) {
                            mb = (sizeField & 0x7FFF) / 1024;
                        } else {
                            mb = sizeField;
                        }
                    }
                    if (mb > 0) {
                        slotsUsed++;
                        totalInstalledMb += mb;
                        bestForm = p[0x0E];
                        WORD spd = *reinterpret_cast<const WORD*>(p + 0x15);
                        WORD cfgSpd = (hlen >= 0x22) ? *reinterpret_cast<const WORD*>(p + 0x20) : 0;
                        int eff = (cfgSpd > 0 && cfgSpd != 0xFFFF) ? cfgSpd : (spd != 0xFFFF ? spd : 0);
                        if (eff > maxSpeed) maxSpeed = eff;
                    }
                }
                const BYTE* next = p + hlen;
                while (next + 1 < end && !(next[0] == 0 && next[1] == 0)) next++;
                next += 2;
                p = next;
            }

            if (slotsTotal > 0) {
                mem_.slotsTotal = slotsTotal;
                mem_.slotsUsed = slotsUsed;
            }
            if (maxSpeed > 0) mem_.speedMhz = maxSpeed;
            if (bestForm == 0x0D) mem_.formFactor = L"SODIMM";
            else if (bestForm == 0x09) mem_.formFactor = L"DIMM";
            else if (bestForm == 0x10) mem_.formFactor = L"Row of chips";

            if (totalInstalledMb > 0) {
                uint64_t installedBytes = totalInstalledMb * 1024ULL * 1024ULL;
                mem_.installedBytes = installedBytes;
                if (installedBytes > mem_.totalBytes) {
                    mem_.hardwareReservedBytes = installedBytes - mem_.totalBytes;
                }
            }
        }
    }
}

void PerfMonitor::InitDisks() {
    disks_.clear();

    // Map drive letters to physical device numbers
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    wchar_t sysDrive = towupper(winDir[0]);

    std::unordered_map<int, std::vector<std::wstring>> driveLettersByDisk;
    std::unordered_map<int, uint64_t> formattedByDisk;
    std::unordered_map<int, bool> sysDiskByDisk;
    std::unordered_map<int, bool> pageFileByDisk;

    // Collect page file drives
    std::set<wchar_t> pageFileDrives;
    EnumPageFilesW([](LPVOID ctx, PENUM_PAGE_FILE_INFORMATION /*info*/, LPCWSTR name) -> BOOL {
        auto* s = reinterpret_cast<std::set<wchar_t>*>(ctx);
        if (name && name[0]) {
            // Device path or DOS path; check drive letters via GetLogicalDriveStrings
            const wchar_t* colon = wcschr(name, L':');
            if (colon && colon > name) s->insert(towupper(*(colon - 1)));
        }
        return TRUE;
    }, &pageFileDrives);
    // Fallback check pagefile.sys directly on root drives
    DWORD drivesMask = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drivesMask & (1u << d))) continue;
        wchar_t letter = static_cast<wchar_t>(L'A' + d);
        wchar_t root[4] = { letter, L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t pfPath[16] = { letter, L':', L'\\', L'p', L'a', L'g', L'e', L'f', L'i', L'l', L'e', L'.', L's', L'y', L's', 0 };
        if (GetFileAttributesW(pfPath) != INVALID_FILE_ATTRIBUTES) {
            pageFileDrives.insert(letter);
        }

        wchar_t volPath[8] = { L'\\', L'\\', L'.', L'\\', letter, L':', 0 };
        HANDLE hVol = CreateFileW(volPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hVol != INVALID_HANDLE_VALUE) {
            STORAGE_DEVICE_NUMBER sdn{};
            DWORD bytes = 0;
            if (DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof(sdn), &bytes, nullptr)) {
                int diskIdx = static_cast<int>(sdn.DeviceNumber);
                wchar_t lstr[4] = { letter, L':', 0 };
                driveLettersByDisk[diskIdx].push_back(lstr);

                ULARGE_INTEGER totalBytes{};
                if (GetDiskFreeSpaceExW(root, nullptr, &totalBytes, nullptr)) {
                    formattedByDisk[diskIdx] += totalBytes.QuadPart;
                }
                if (letter == sysDrive) sysDiskByDisk[diskIdx] = true;
                if (pageFileDrives.count(letter)) pageFileByDisk[diskIdx] = true;
            }
            CloseHandle(hVol);
        }
    }

    // Enumerate physical drives 0..15 via \\.\PhysicalDriveN
    for (int i = 0; i < 16; ++i) {
        wchar_t devPath[32];
        swprintf_s(devPath, L"\\\\.\\PhysicalDrive%d", i);
        HANDLE hDisk = CreateFileW(devPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDisk == INVALID_HANDLE_VALUE) continue;

        PerfDiskInfo info;
        info.index = i;
        info.activeHistory.assign(kHistoryLen, 0.0);
        info.readHistory.assign(kHistoryLen, 0.0);
        info.writeHistory.assign(kHistoryLen, 0.0);

        // Query model/vendor
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        BYTE descBuf[1024] = {};
        DWORD ret = 0;
        if (DeviceIoControl(hDisk, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), descBuf, sizeof(descBuf), &ret, nullptr)) {
            const auto* dd = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(descBuf);
            std::wstring vendor, prod;
            if (dd->VendorIdOffset && dd->VendorIdOffset < ret) {
                vendor = AnsiToWide(reinterpret_cast<const char*>(descBuf + dd->VendorIdOffset));
            }
            if (dd->ProductIdOffset && dd->ProductIdOffset < ret) {
                prod = AnsiToWide(reinterpret_cast<const char*>(descBuf + dd->ProductIdOffset));
            }
            if (!vendor.empty() && prod.find(vendor) == std::wstring::npos) {
                info.model = vendor + L" " + prod;
            } else {
                info.model = prod;
            }
            if (info.model.empty()) info.model = L"Physical Disk";
        }

        // SSD vs HDD (seek penalty)
        STORAGE_PROPERTY_QUERY qSeek{};
        qSeek.PropertyId = StorageDeviceSeekPenaltyProperty;
        qSeek.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR seekDesc{};
        info.type = L"SSD";
        if (DeviceIoControl(hDisk, IOCTL_STORAGE_QUERY_PROPERTY, &qSeek, sizeof(qSeek),
                            &seekDesc, sizeof(seekDesc), &ret, nullptr)) {
            info.type = seekDesc.IncursSeekPenalty ? L"HDD" : L"SSD";
        }

        // Capacity: IOCTL_DISK_GET_DRIVE_GEOMETRY_EX succeeds even on un-elevated
        // PhysicalDrive handles, whereas IOCTL_DISK_GET_LENGTH_INFO often fails with ERROR_ACCESS_DENIED.
        BYTE geomBuf[256] = {};
        if (DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                            geomBuf, sizeof(geomBuf), &ret, nullptr) &&
            ret >= FIELD_OFFSET(DISK_GEOMETRY_EX, Data)) {
            const auto* geom = reinterpret_cast<const DISK_GEOMETRY_EX*>(geomBuf);
            if (geom->DiskSize.QuadPart > 0) {
                info.capacityBytes = static_cast<uint64_t>(geom->DiskSize.QuadPart);
            }
        }
        if (info.capacityBytes == 0) {
            GET_LENGTH_INFORMATION lenInfo{};
            if (DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &lenInfo, sizeof(lenInfo), &ret, nullptr)) {
                info.capacityBytes = static_cast<uint64_t>(lenInfo.Length.QuadPart);
            }
        }

        CloseHandle(hDisk);

        auto lit = driveLettersByDisk.find(i);
        if (lit != driveLettersByDisk.end()) {
            for (size_t k = 0; k < lit->second.size(); ++k) {
                if (k > 0) info.letters += L" ";
                info.letters += lit->second[k];
            }
        }
        info.formattedBytes = formattedByDisk[i];
        info.isSystemDisk = sysDiskByDisk[i];
        info.hasPageFile = pageFileByDisk[i];

        if (!info.letters.empty()) {
            info.title = L"Disk " + std::to_wstring(i) + L" (" + info.letters + L")";
        } else {
            info.title = L"Disk " + std::to_wstring(i);
        }

        disks_.push_back(std::move(info));
    }
}

void PerfMonitor::InitNetworks() {
    nets_.clear();

    ULONG sz = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &sz);
    if (sz == 0) return;
    std::vector<BYTE> buf(sz);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addrs, &sz) != ERROR_SUCCESS) return;

    int ethCount = 0, wifiCount = 0;
    for (auto* a = addrs; a != nullptr; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        if (a->OperStatus != IfOperStatusUp) continue;

        PerfNetInfo ni;
        ni.luidValue = a->Luid.Value;
        ni.adapterName = a->Description ? a->Description : L"Network Adapter";
        ni.dnsName = a->DnsSuffix ? a->DnsSuffix : L"";
        ni.connected = true;

        bool isWifi = (a->IfType == IF_TYPE_IEEE80211);
        if (isWifi) {
            wifiCount++;
            ni.connType = L"Wi-Fi";
            ni.title = (wifiCount == 1) ? L"Wi-Fi" : (L"Wi-Fi " + std::to_wstring(wifiCount));
        } else {
            ethCount++;
            ni.connType = L"Ethernet";
            ni.title = (ethCount == 1) ? L"Ethernet" : (L"Ethernet " + std::to_wstring(ethCount));
        }
        if (a->FriendlyName && a->FriendlyName[0]) {
            ni.title = a->FriendlyName;
        }

        int bestIpv6Rank = 0;
        for (auto* ua = a->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
            if (!ua->Address.lpSockaddr) continue;
            wchar_t ipBuf[64] = {};
            if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                InetNtopW(AF_INET, &sin->sin_addr, ipBuf, 64);
                if (ni.ipv4.empty()) ni.ipv4 = ipBuf;
            } else if (ua->Address.lpSockaddr->sa_family == AF_INET6) {
                auto* sin6 = reinterpret_cast<sockaddr_in6*>(ua->Address.lpSockaddr);
                InetNtopW(AF_INET6, &sin6->sin6_addr, ipBuf, 64);
                const uint8_t b0 = sin6->sin6_addr.u.Byte[0];
                const uint8_t b1 = sin6->sin6_addr.u.Byte[1];
                const bool isLinkLocal = (b0 == 0xFE && (b1 & 0xC0) == 0x80);
                const bool isUniqueLocal = ((b0 & 0xFE) == 0xFC);
                const bool isGlobalUnicast = ((b0 & 0xE0) == 0x20);
                const bool isPreferred = (ua->DadState == IpDadStatePreferred);
                const bool isStableSuffix = (ua->SuffixOrigin != IpSuffixOriginRandom);

                int rank = 10;
                if (isGlobalUnicast) {
                    rank = isPreferred ? (isStableSuffix ? 60 : 55) : 45;
                } else if (!isUniqueLocal && !isLinkLocal) {
                    rank = isPreferred ? 35 : 30;
                } else if (isUniqueLocal) {
                    rank = isPreferred ? 25 : 20;
                } else {
                    rank = isPreferred ? 12 : 10;
                }
                if (rank > bestIpv6Rank) {
                    bestIpv6Rank = rank;
                    ni.ipv6 = ipBuf;
                }
            }
        }

        MIB_IF_ROW2 row{};
        row.InterfaceLuid = a->Luid;
        if (GetIfEntry2(&row) == NO_ERROR) {
            ni.linkSpeedBps = row.TransmitLinkSpeed;
            ni.prevInOctets = row.InOctets;
            ni.prevOutOctets = row.OutOctets;
            ni.hasPrevOctets = true;
        }

        ni.sendHistory.assign(kHistoryLen, 0.0);
        ni.recvHistory.assign(kHistoryLen, 0.0);
        nets_.push_back(std::move(ni));
    }
}

void PerfMonitor::InitGpus() {
    gpus_.clear();

    // Enumerate display driver metadata from registry
    struct RegGpuMeta {
        std::wstring desc;
        std::wstring ver;
        std::wstring date;
    };
    std::vector<RegGpuMeta> regMeta;
    HKEY hClass = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                      0, KEY_READ, &hClass) == ERROR_SUCCESS) {
        wchar_t subName[32];
        for (DWORD idx = 0; ; ++idx) {
            DWORD subLen = ARRAYSIZE(subName);
            if (RegEnumKeyExW(hClass, idx, subName, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (!iswdigit(subName[0])) continue;
            HKEY hSub = nullptr;
            if (RegOpenKeyExW(hClass, subName, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
                wchar_t desc[256] = {}, ver[64] = {}, date[64] = {};
                DWORD sz = sizeof(desc);
                RegQueryValueExW(hSub, L"DriverDesc", nullptr, nullptr, reinterpret_cast<BYTE*>(desc), &sz);
                sz = sizeof(ver);
                RegQueryValueExW(hSub, L"DriverVersion", nullptr, nullptr, reinterpret_cast<BYTE*>(ver), &sz);
                sz = sizeof(date);
                RegQueryValueExW(hSub, L"DriverDate", nullptr, nullptr, reinterpret_cast<BYTE*>(date), &sz);
                if (desc[0]) regMeta.push_back({ desc, ver, date });
                RegCloseKey(hSub);
            }
        }
        RegCloseKey(hClass);
    }

    // Query WDDM adapter type bitfields so indirect/virtual display drivers (e.g. Parsec VDD)
    // and software adapters that share a hardware name via DXGI are excluded like native Task Manager.
    struct KmtTypeEntry { LUID luid; uint32_t flags; };
    std::vector<KmtTypeEntry> kmtTypes;
    ForEachD3dkmtAdapter([&](const D3DKMT_ADAPTERINFO_TEMP& info, PFN_D3DKMTQueryAdapterInfo pQuery) {
        uint32_t flags = 0;
        D3DKMT_QUERYADAPTERINFO_TEMP qai{ info.hAdapter, 15 /*KMTQAITYPE_ADAPTERTYPE*/, &flags, sizeof(flags) };
        if (NT_SUCCESS(pQuery(&qai))) {
            kmtTypes.push_back({ info.AdapterLuid, flags });
        }
    });

    auto isHardwareRenderGpu = [&](const DXGI_ADAPTER_DESC1& desc) -> bool {
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;
        for (const auto& kt : kmtTypes) {
            if (kt.luid.HighPart == desc.AdapterLuid.HighPart &&
                kt.luid.LowPart == desc.AdapterLuid.LowPart) {
                constexpr uint32_t kRenderSupported = 0x0001u;
                constexpr uint32_t kSoftwareDevice  = 0x0004u;
                constexpr uint32_t kIndirectDisplay = 0x0040u;
                if ((kt.flags & kRenderSupported) == 0) return false;
                if ((kt.flags & (kSoftwareDevice | kIndirectDisplay)) != 0) return false;
                break;
            }
        }
        return true;
    };

    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                if (isHardwareRenderGpu(desc)) {
                    PerfGpuInfo gi;
                    gi.index = static_cast<int>(gpus_.size());
                    gi.title = L"GPU " + std::to_wstring(gi.index);
                    gi.name = desc.Description;
                    gi.luid = desc.AdapterLuid;
                    gi.dedicatedTotal = desc.DedicatedVideoMemory;
                    gi.sharedTotal = desc.SharedSystemMemory;
                    gi.directXVersion = L"12 (FL 12.1)";
                    wchar_t locBuf[64];
                    swprintf_s(locBuf, L"Device 0x%04X, Vendor 0x%04X", desc.DeviceId, desc.VendorId);
                    gi.physLocation = locBuf;

                    for (const auto& rm : regMeta) {
                        if (_wcsicmp(rm.desc.c_str(), gi.name.c_str()) == 0 ||
                            gi.name.find(rm.desc) != std::wstring::npos ||
                            rm.desc.find(gi.name) != std::wstring::npos) {
                            gi.driverVersion = rm.ver;
                            gi.driverDate = rm.date;
                            break;
                        }
                    }

                    gi.hist3D.assign(kHistoryLen, 0.0);
                    gi.histCopy.assign(kHistoryLen, 0.0);
                    gi.histVideoEncode.assign(kHistoryLen, 0.0);
                    gi.histVideoDecode.assign(kHistoryLen, 0.0);
                    gi.histOverall.assign(kHistoryLen, 0.0);
                    gi.histDedMem.assign(kHistoryLen, 0.0);
                    gpus_.push_back(std::move(gi));
                }
            }
            adapter->Release();
        }
        factory->Release();
    }
}

void PerfMonitor::InitPdh() {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    pdhQuery_ = q;

    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Performance", 0, &c) == ERROR_SUCCESS)
        pdhCpuPerf_ = c;
    if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\% Disk Time", 0, &c) == ERROR_SUCCESS)
        pdhDiskTime_ = c;
    if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", 0, &c) == ERROR_SUCCESS)
        pdhDiskRead_ = c;
    if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", 0, &c) == ERROR_SUCCESS)
        pdhDiskWrite_ = c;
    if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer", 0, &c) == ERROR_SUCCESS)
        pdhDiskAvgSec_ = c;
    if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) == ERROR_SUCCESS)
        pdhGpuEngine_ = c;
    if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &c) == ERROR_SUCCESS)
        pdhGpuDed_ = c;
    if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &c) == ERROR_SUCCESS)
        pdhGpuShared_ = c;

    PdhCollectQueryData(q);
    pdhReady_.store(true, std::memory_order_release);
}

void PerfMonitor::Sample(DWORD processCount, DWORD totalThreads, DWORD totalHandles) {
    ULONGLONG now = GetTickCount64();
    bool baselineTick = (now >= prevSampleTickMs_ && (now - prevSampleTickMs_) < 80ULL);
    double dtSec = (now > prevSampleTickMs_) ? static_cast<double>(now - prevSampleTickMs_) / 1000.0 : 1.0;
    if (dtSec < 0.05) dtSec = 0.05;
    prevSampleTickMs_ = now;

    if (pdhReady_.load(std::memory_order_acquire) && pdhQuery_) {
        PdhCollectQueryData(static_cast<PDH_HQUERY>(pdhQuery_));
    }

    RawPerfSlot slot{};
    slot.timestampMs = GetNowUtcMs();

    double prevPrelaunchCpu = cpu_.utilization;
    SampleCpu(processCount, totalThreads, totalHandles);
    SampleMemory();
    SampleDisks();
    SampleNetworks(dtSec);

    if (baselineTick) {
        SampleGpus();
        SyncFromSharedRing(!cachedSlots_.empty());
        if (cpu_.utilization <= 0.0 && prevPrelaunchCpu > 0.0) {
            cpu_.utilization = prevPrelaunchCpu;
        }
        RebuildHistoriesForSpeed(activeSpeed_);
        return;
    }

    slot.cpuUtil = static_cast<float>(cpu_.utilization);
    slot.cpuKernel = static_cast<float>(cpu_.kernelUtilization);
    int nc = (std::min)(static_cast<int>(cpu_.coreCurrent.size()), kMaxRawCores);
    for (int i = 0; i < nc; ++i) {
        slot.coreUtil[i] = static_cast<float>(cpu_.coreCurrent[static_cast<size_t>(i)]);
    }
    slot.memUtil = static_cast<float>(mem_.utilization);
    int nd = (std::min)(static_cast<int>(disks_.size()), kMaxRawDisks);
    for (int i = 0; i < nd; ++i) {
        slot.diskActive[i]   = static_cast<float>(disks_[static_cast<size_t>(i)].activeTimePct);
        slot.diskReadMBs[i]  = static_cast<float>(disks_[static_cast<size_t>(i)].readBytesPerSec / (1024.0 * 1024.0));
        slot.diskWriteMBs[i] = static_cast<float>(disks_[static_cast<size_t>(i)].writeBytesPerSec / (1024.0 * 1024.0));
    }
    int nn = (std::min)(static_cast<int>(nets_.size()), kMaxRawNets);
    for (int i = 0; i < nn; ++i) {
        slot.netSendMbps[i] = static_cast<float>((nets_[static_cast<size_t>(i)].sendBytesPerSec * 8.0) / 1000000.0);
        slot.netRecvMbps[i] = static_cast<float>((nets_[static_cast<size_t>(i)].recvBytesPerSec * 8.0) / 1000000.0);
    }

    SampleGpus();
    int ng = (std::min)(static_cast<int>(gpus_.size()), kMaxRawGpus);
    for (int i = 0; i < ng; ++i) {
        const auto& g = gpus_[static_cast<size_t>(i)];
        slot.gpu3D[i]      = static_cast<float>(g.hist3D.empty() ? 0.0 : g.hist3D.back());
        slot.gpuCopy[i]    = static_cast<float>(g.histCopy.empty() ? 0.0 : g.histCopy.back());
        slot.gpuVEnc[i]    = static_cast<float>(g.histVideoEncode.empty() ? 0.0 : g.histVideoEncode.back());
        slot.gpuVDec[i]    = static_cast<float>(g.histVideoDecode.empty() ? 0.0 : g.histVideoDecode.back());
        slot.gpuOverall[i] = static_cast<float>(g.utilization);
        slot.gpuDedMem[i]  = static_cast<float>(g.histDedMem.empty() ? 0.0 : g.histDedMem.back());
    }

    AppendSlotToSharedRing(slot);
    RebuildHistoriesForSpeed(activeSpeed_);
}

void PerfMonitor::SampleCpu(DWORD processCount, DWORD totalThreads, DWORD totalHandles) {
    cpu_.processes = processCount;
    cpu_.threads = totalThreads;
    cpu_.handles = totalHandles;
    cpu_.uptimeMs = GetTickCount64();

    static auto pfnNtQuerySys = reinterpret_cast<PFN_NtQuerySystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));

    if (pfnNtQuerySys && cpu_.logicalProcessors > 0) {
        size_t n = static_cast<size_t>(cpu_.logicalProcessors);
        std::vector<NT_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> sppi(n);
        ULONG ret = 0;
        if (NT_SUCCESS(pfnNtQuerySys(8, sppi.data(),
                                     static_cast<ULONG>(n * sizeof(NT_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)), &ret))) {
            double sumUtil = 0.0, sumKernel = 0.0;
            for (size_t i = 0; i < n; ++i) {
                LONGLONG curIdle = sppi[i].IdleTime.QuadPart;
                LONGLONG curKern = sppi[i].KernelTime.QuadPart;
                LONGLONG curUser = sppi[i].UserTime.QuadPart;

                LONGLONG dIdle = curIdle - prevCoreTimes_[i].idle;
                LONGLONG dKern = curKern - prevCoreTimes_[i].kernel;
                LONGLONG dUser = curUser - prevCoreTimes_[i].user;
                LONGLONG dTotal = dKern + dUser;

                prevCoreTimes_[i] = { curIdle, curKern, curUser };

                double util = 0.0, kernOnly = 0.0;
                if (dTotal > 0) {
                    LONGLONG dActiveKern = (dKern >= dIdle) ? (dKern - dIdle) : 0;
                    LONGLONG dActive = dActiveKern + dUser;
                    util = (std::min)(100.0, (std::max)(0.0, 100.0 * static_cast<double>(dActive) / static_cast<double>(dTotal)));
                    kernOnly = (std::min)(100.0, (std::max)(0.0, 100.0 * static_cast<double>(dActiveKern) / static_cast<double>(dTotal)));
                }
                cpu_.coreCurrent[i] = util;
                sumUtil += util;
                sumKernel += kernOnly;
            }
            cpu_.utilization = sumUtil / static_cast<double>(n);
            cpu_.kernelUtilization = sumKernel / static_cast<double>(n);
        }
    }

    // Dynamic clock speed via PDH "% Processor Performance" (captures Turbo Boost)
    if (pdhReady_.load(std::memory_order_acquire) && pdhCpuPerf_ && cpu_.baseSpeedGhz > 0.0) {
        PDH_FMT_COUNTERVALUE val{};
        if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdhCpuPerf_), PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS &&
            val.doubleValue > 0.0) {
            cpu_.currentSpeedGhz = cpu_.baseSpeedGhz * (val.doubleValue / 100.0);
        }
    } else if (cpu_.logicalProcessors > 0) {
        std::vector<NT_PROCESSOR_POWER_INFORMATION> ppi(static_cast<size_t>(cpu_.logicalProcessors));
        if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(),
                                   static_cast<ULONG>(ppi.size() * sizeof(NT_PROCESSOR_POWER_INFORMATION))) == 0) {
            if (ppi[0].CurrentMhz > 0) cpu_.currentSpeedGhz = static_cast<double>(ppi[0].CurrentMhz) / 1000.0;
        }
    }
}

void PerfMonitor::SampleMemory() {
    PERFORMANCE_INFORMATION pi{ sizeof(pi) };
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        uint64_t ps = static_cast<uint64_t>(pi.PageSize);
        mem_.totalBytes = static_cast<uint64_t>(pi.PhysicalTotal) * ps;
        mem_.availableBytes = static_cast<uint64_t>(pi.PhysicalAvailable) * ps;
        mem_.inUseBytes = (mem_.totalBytes >= mem_.availableBytes) ? (mem_.totalBytes - mem_.availableBytes) : 0;
        mem_.committedBytes = static_cast<uint64_t>(pi.CommitTotal) * ps;
        mem_.commitLimitBytes = static_cast<uint64_t>(pi.CommitLimit) * ps;
        mem_.cachedBytes = static_cast<uint64_t>(pi.SystemCache) * ps;
        mem_.pagedPoolBytes = static_cast<uint64_t>(pi.KernelPaged) * ps;
        mem_.nonPagedPoolBytes = static_cast<uint64_t>(pi.KernelNonpaged) * ps;
        mem_.utilization = mem_.totalBytes > 0 ? (100.0 * static_cast<double>(mem_.inUseBytes) / static_cast<double>(mem_.totalBytes)) : 0.0;

        // Try detailed memory lists (Standby / Modified / Free)
        static auto pfnNtQuerySys = reinterpret_cast<PFN_NtQuerySystemInformation>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
        bool gotList = false;
        if (pfnNtQuerySys) {
            NT_SYSTEM_MEMORY_LIST_INFORMATION mli{};
            ULONG ret = 0;
            if (NT_SUCCESS(pfnNtQuerySys(80 /*SystemMemoryListInformation*/, &mli, sizeof(mli), &ret))) {
                uint64_t standbyPages = 0;
                for (int i = 0; i < 8; ++i) standbyPages += mli.PageCountByPriority[i];
                mem_.standbyBytes = standbyPages * ps;
                mem_.modifiedBytes = (mli.ModifiedPageCount + mli.ModifiedNoWritePageCount) * ps;
                mem_.freeBytes = (mli.FreePageCount + mli.ZeroPageCount) * ps;
                mem_.cachedBytes = mem_.standbyBytes + mem_.modifiedBytes;
                gotList = true;
            }
        }
        if (!gotList) {
            mem_.standbyBytes = (std::min)(mem_.availableBytes, mem_.cachedBytes);
            mem_.modifiedBytes = 0;
            mem_.freeBytes = (mem_.availableBytes > mem_.standbyBytes) ? (mem_.availableBytes - mem_.standbyBytes) : 0;
        }
    }
}

static int ParsePdhDiskIndex(const wchar_t* instName) {
    if (!instName || _wcsicmp(instName, L"_Total") == 0) return -1;
    // Format is "0 C:" or "1"
    if (!iswdigit(instName[0])) return -1;
    return _wtoi(instName);
}

void PerfMonitor::SampleDisks() {
    bool pdhOk = pdhReady_.load(std::memory_order_acquire) && pdhQuery_ != nullptr;
    auto readDiskArray = [&](void* hCounter, auto&& fn) {
        if (!pdhOk || !hCounter) return;
        DWORD sz = 0, cnt = 0;
        PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(hCounter), PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
        if (sz == 0) return;
        std::vector<BYTE> buf(sz);
        auto items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
        if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(hCounter), PDH_FMT_DOUBLE, &sz, &cnt, items) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < cnt; ++i) {
                if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
                int idx = ParsePdhDiskIndex(items[i].szName);
                if (idx >= 0) fn(idx, items[i].FmtValue.doubleValue);
            }
        }
    };

    for (auto& d : disks_) {
        d.activeTimePct = 0.0;
        d.readBytesPerSec = 0.0;
        d.writeBytesPerSec = 0.0;
        d.avgResponseMs = 0.0;
    }

    auto findDisk = [&](int idx) -> PerfDiskInfo* {
        for (auto& d : disks_) if (d.index == idx) return &d;
        return nullptr;
    };

    readDiskArray(pdhDiskTime_, [&](int idx, double v) {
        if (auto* d = findDisk(idx)) d->activeTimePct = (std::min)(100.0, (std::max)(0.0, v));
    });
    readDiskArray(pdhDiskRead_, [&](int idx, double v) {
        if (auto* d = findDisk(idx)) d->readBytesPerSec = (std::max)(0.0, v);
    });
    readDiskArray(pdhDiskWrite_, [&](int idx, double v) {
        if (auto* d = findDisk(idx)) d->writeBytesPerSec = (std::max)(0.0, v);
    });
    readDiskArray(pdhDiskAvgSec_, [&](int idx, double v) {
        if (auto* d = findDisk(idx)) d->avgResponseMs = (std::max)(0.0, v * 1000.0);
    });
}

void PerfMonitor::SampleNetworks(double dtSec) {
    for (auto& n : nets_) {
        MIB_IF_ROW2 row{};
        row.InterfaceLuid.Value = n.luidValue;
        if (GetIfEntry2(&row) == NO_ERROR) {
            n.linkSpeedBps = row.TransmitLinkSpeed;
            n.connected = (row.OperStatus == IfOperStatusUp);
            if (n.hasPrevOctets) {
                uint64_t dOut = (row.OutOctets >= n.prevOutOctets) ? (row.OutOctets - n.prevOutOctets) : 0;
                uint64_t dIn  = (row.InOctets >= n.prevInOctets)   ? (row.InOctets - n.prevInOctets)   : 0;
                n.sendBytesPerSec = static_cast<double>(dOut) / dtSec;
                n.recvBytesPerSec = static_cast<double>(dIn) / dtSec;
            }
            n.prevOutOctets = row.OutOctets;
            n.prevInOctets = row.InOctets;
            n.hasPrevOctets = true;
        }
    }
}

static bool ParseGpuAdapterLuid(const wchar_t* instName, LUID& outLuid, std::wstring& outEngType) {
    if (!instName) return false;
    const wchar_t* pLuid = wcsstr(instName, L"luid_0x");
    if (!pLuid) return false;
    const wchar_t* p1 = pLuid + 5;
    wchar_t* end1 = nullptr;
    unsigned long high = wcstoul(p1, &end1, 16);
    if (!end1 || wcsncmp(end1, L"_0x", 3) != 0) return false;
    unsigned long low = wcstoul(end1 + 1, nullptr, 16);
    outLuid.HighPart = static_cast<LONG>(high);
    outLuid.LowPart = static_cast<DWORD>(low);

    const wchar_t* pEng = wcsstr(instName, L"engtype_");
    if (pEng) outEngType = pEng + 8;
    return true;
}

void PerfMonitor::SampleGpus() {
    if (gpus_.empty()) return;

    struct Accum {
        double e3D = 0.0, eCopy = 0.0, eVEnc = 0.0, eVDec = 0.0, eMax = 0.0;
        uint64_t ded = 0, shared = 0;
    };
    std::vector<Accum> acc(gpus_.size());

    auto findByLuid = [&](LUID luid) -> int {
        for (size_t i = 0; i < gpus_.size(); ++i) {
            if (gpus_[i].luid.HighPart == luid.HighPart && gpus_[i].luid.LowPart == luid.LowPart) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    bool pdhOk = pdhReady_.load(std::memory_order_acquire) && pdhQuery_ != nullptr;

    if (pdhOk && pdhGpuEngine_) {
        DWORD sz = 0, cnt = 0;
        PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhGpuEngine_), PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
        if (sz > 0) {
            std::vector<BYTE> buf(sz);
            auto items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
            if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhGpuEngine_), PDH_FMT_DOUBLE, &sz, &cnt, items) == ERROR_SUCCESS) {
                for (DWORD i = 0; i < cnt; ++i) {
                    if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
                    LUID luid{};
                    std::wstring eng;
                    if (ParseGpuAdapterLuid(items[i].szName, luid, eng)) {
                        int idx = findByLuid(luid);
                        if (idx < 0) continue;
                        double v = (std::max)(0.0, items[i].FmtValue.doubleValue);
                        auto& a = acc[static_cast<size_t>(idx)];
                        if (_wcsicmp(eng.c_str(), L"3D") == 0) a.e3D += v;
                        else if (_wcsicmp(eng.c_str(), L"Copy") == 0) a.eCopy += v;
                        else if (_wcsicmp(eng.c_str(), L"VideoEncode") == 0) a.eVEnc += v;
                        else if (_wcsicmp(eng.c_str(), L"VideoDecode") == 0) a.eVDec += v;
                    }
                }
            }
        }
    }

    auto readMem = [&](void* hCounter, bool isDed) {
        if (!pdhOk || !hCounter) return;
        DWORD sz = 0, cnt = 0;
        PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(hCounter), PDH_FMT_LARGE, &sz, &cnt, nullptr);
        if (sz == 0) return;
        std::vector<BYTE> buf(sz);
        auto items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
        if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(hCounter), PDH_FMT_LARGE, &sz, &cnt, items) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < cnt; ++i) {
                if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
                LUID luid{};
                std::wstring dummy;
                if (ParseGpuAdapterLuid(items[i].szName, luid, dummy)) {
                    int idx = findByLuid(luid);
                    if (idx < 0) continue;
                    uint64_t bytes = static_cast<uint64_t>((std::max)(0LL, items[i].FmtValue.largeValue));
                    if (isDed) acc[static_cast<size_t>(idx)].ded += bytes;
                    else acc[static_cast<size_t>(idx)].shared += bytes;
                }
            }
        }
    };
    readMem(pdhGpuDed_, true);
    readMem(pdhGpuShared_, false);

    // Query GPU temperature via D3DKMT (Win10 WDDM 2.4+)
    ForEachD3dkmtAdapter([&](const D3DKMT_ADAPTERINFO_TEMP& info, PFN_D3DKMTQueryAdapterInfo pQuery) {
        int idx = findByLuid(info.AdapterLuid);
        if (idx < 0) return;
        D3DKMT_ADAPTER_PERFDATA_TEMP perf{};
        D3DKMT_QUERYADAPTERINFO_TEMP qai{ info.hAdapter, 62 /*KMTQAITYPE_ADAPTERPERFDATA*/, &perf, sizeof(perf) };
        if (NT_SUCCESS(pQuery(&qai)) && perf.Temperature > 0 && perf.Temperature < 2000) {
            gpus_[static_cast<size_t>(idx)].hasTemperature = true;
            gpus_[static_cast<size_t>(idx)].temperatureC = static_cast<double>(perf.Temperature) / 10.0;
        }
    });

    for (size_t i = 0; i < gpus_.size(); ++i) {
        auto& g = gpus_[i];
        const auto& a = acc[i];
        double e3d = (std::min)(100.0, a.e3D);
        double ecp = (std::min)(100.0, a.eCopy);
        double eve = (std::min)(100.0, a.eVEnc);
        double evd = (std::min)(100.0, a.eVDec);
        g.utilization = (std::max)({ e3d, ecp, eve, evd });
        g.dedicatedUsed = a.ded;
        g.sharedUsed = a.shared;

        // Store latest sample in back element for Sample() to pack into RawPerfSlot before RebuildHistoriesForSpeed()
        if (g.hist3D.size() == kHistoryLen)          g.hist3D.back() = e3d;
        if (g.histCopy.size() == kHistoryLen)        g.histCopy.back() = ecp;
        if (g.histVideoEncode.size() == kHistoryLen) g.histVideoEncode.back() = eve;
        if (g.histVideoDecode.size() == kHistoryLen) g.histVideoDecode.back() = evd;
        if (g.histOverall.size() == kHistoryLen)     g.histOverall.back() = g.utilization;
        double dedPct = g.dedicatedTotal > 0 ? (100.0 * static_cast<double>(g.dedicatedUsed) / static_cast<double>(g.dedicatedTotal)) : 0.0;
        if (g.histDedMem.size() == kHistoryLen)      g.histDedMem.back() = (std::min)(100.0, dedPct);
    }
}

} // namespace taskman
