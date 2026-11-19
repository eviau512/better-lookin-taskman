#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include "process_monitor.h"
#include "format_util.h"
#include <winternl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <evntrace.h>
#include <evntcons.h>
#include <sddl.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <tuple>

namespace taskman {

// Complete Win10 SYSTEM_THREAD_INFORMATION & SYSTEM_PROCESS_INFORMATION layouts
struct NT_SYSTEM_THREAD_INFORMATION {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG         WaitTime;
    PVOID         StartAddress;
    CLIENT_ID     ClientId;
    LONG          Priority;
    LONG          BasePriority;
    ULONG         ContextSwitches;
    ULONG         ThreadState;
    ULONG         WaitReason;
};

struct NT_SYSTEM_PROCESS_INFORMATION {
    ULONG32       NextEntryOffset;
    ULONG32       NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG32       HardFaultCount;
    ULONG32       NumberOfThreadsHighWatermark;
    ULONGLONG     CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG          BasePriority;
    HANDLE        UniqueProcessId;
    HANDLE        InheritedFromUniqueProcessId;
    ULONG32       HandleCount;
    ULONG32       SessionId;
    ULONG_PTR     UniqueProcessKey;
    SIZE_T        PeakVirtualSize;
    SIZE_T        VirtualSize;
    ULONG32       PageFaultCount;
    SIZE_T        PeakWorkingSetSize;
    SIZE_T        WorkingSetSize;
    SIZE_T        QuotaPeakPagedPoolUsage;
    SIZE_T        QuotaPagedPoolUsage;
    SIZE_T        QuotaPeakNonPagedPoolUsage;
    SIZE_T        QuotaNonPagedPoolUsage;
    SIZE_T        PagefileUsage;
    SIZE_T        PeakPagefileUsage;
    SIZE_T        PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    NT_SYSTEM_THREAD_INFORMATION Threads[1];
};

using PFN_NtQuerySystemInformation = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using PFN_NtQueryInformationProcess = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using PFN_IsWow64Process2 = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
using PFN_MiniDumpWriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, ULONG, PVOID, PVOID, PVOID);

#ifndef EVENT_TRACE_SYSTEM_LOGGER_MODE
#define EVENT_TRACE_SYSTEM_LOGGER_MODE 0x02000000
#endif

// {A8B4C6D2-3E1F-4B9A-9D2C-7E8F1A2B3C4D}
static const GUID kSessionGuid =
    { 0xa8b4c6d2, 0x3e1f, 0x4b9a, { 0x9d, 0x2c, 0x7e, 0x8f, 0x1a, 0x2b, 0x3c, 0x4d } };
// DiskIoGuid: {3D6FA8D4-FE05-11D0-9DDA-00C04FD7BA7C}
static const GUID kDiskIoGuid =
    { 0x3d6fa8d4, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
// TcpIpGuid: {9A280AC0-C8E0-11D1-84E2-00C04FB998A2}
static const GUID kTcpIpGuid =
    { 0x9a280ac0, 0xc8e0, 0x11d1, { 0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2 } };
// UdpIpGuid: {BF3A50C5-A9C9-4988-A005-2DF0B7C80F80}
static const GUID kUdpIpGuid =
    { 0xbf3a50c5, 0xa9c9, 0x4988, { 0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80 } };

static void QueryActiveSocketOwners(std::unordered_map<DWORD, uint32_t>& tcpEstabByPid,
                                    std::unordered_map<DWORD, uint32_t>& udpByPid) {
    auto isActiveTcp = [](DWORD state) -> bool {
        return state == MIB_TCP_STATE_ESTAB ||
               state == MIB_TCP_STATE_SYN_SENT ||
               state == MIB_TCP_STATE_SYN_RCVD ||
               state == MIB_TCP_STATE_FIN_WAIT1 ||
               state == MIB_TCP_STATE_FIN_WAIT2 ||
               state == MIB_TCP_STATE_CLOSE_WAIT;
    };

    // TCP IPv4
    ULONG sz = 0;
    if (GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER && sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetExtendedTcpTable(buf.data(), &sz, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            const auto* tbl = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < tbl->dwNumEntries; ++i) {
                DWORD pid = tbl->table[i].dwOwningPid;
                if (pid > 0 && isActiveTcp(tbl->table[i].dwState)) {
                    tcpEstabByPid[pid]++;
                }
            }
        }
    }
    // TCP IPv6
    sz = 0;
    if (GetExtendedTcpTable(nullptr, &sz, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER && sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetExtendedTcpTable(buf.data(), &sz, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            const auto* tbl = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < tbl->dwNumEntries; ++i) {
                DWORD pid = tbl->table[i].dwOwningPid;
                if (pid > 0 && isActiveTcp(tbl->table[i].dwState)) {
                    tcpEstabByPid[pid]++;
                }
            }
        }
    }
    // UDP IPv4
    sz = 0;
    if (GetExtendedUdpTable(nullptr, &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER && sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetExtendedUdpTable(buf.data(), &sz, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            const auto* tbl = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < tbl->dwNumEntries; ++i) {
                DWORD pid = tbl->table[i].dwOwningPid;
                if (pid > 0) udpByPid[pid]++;
            }
        }
    }
    // UDP IPv6
    sz = 0;
    if (GetExtendedUdpTable(nullptr, &sz, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER && sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetExtendedUdpTable(buf.data(), &sz, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            const auto* tbl = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < tbl->dwNumEntries; ++i) {
                DWORD pid = tbl->table[i].dwOwningPid;
                if (pid > 0) udpByPid[pid]++;
            }
        }
    }
}

static const wchar_t* kEtwSessionName = L"BetterWin10TaskmanTrace";

static std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

static uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

ProcessMonitor::ProcessMonitor() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    processorCount_ = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;

    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, MAX_PATH)) {
        windowsDirLower_ = ToLower(winDir);
    }

    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        prevIdleTime100ns_ = FileTimeToU64(idle);
        prevSysTime100ns_ = FileTimeToU64(kernel) + FileTimeToU64(user);
    }
    prevTickMs_ = GetTickCount64();
}

ProcessMonitor::~ProcessMonitor() {
    Stop();
}

void ProcessMonitor::Start() {
    if (!pdhInitThread_.joinable() && !pdhReady_.load(std::memory_order_acquire)) {
        pdhInitThread_ = std::thread([this]() { InitPdh(); });
    }
    if (IsCurrentProcessElevated()) {
        StartEtwSession();
    }
}

void ProcessMonitor::Stop() {
    if (pdhInitThread_.joinable()) {
        pdhInitThread_.join();
    }
    StopEtwSession();
    ShutdownPdh();
    for (auto& kv : iconCache_) {
        if (kv.second) DestroyIcon(kv.second);
    }
    iconCache_.clear();
}

void ProcessMonitor::InitPdh() {
    if (pdhQuery_) {
        pdhReady_.store(true, std::memory_order_release);
        return;
    }
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    pdhQuery_ = q;

    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) == ERROR_SUCCESS) {
        pdhGpuEngine_ = c;
    }
    if (PdhAddEnglishCounterW(q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &c) == ERROR_SUCCESS) {
        pdhGpuMemDed_ = c;
    }
    if (PdhAddEnglishCounterW(q, L"\\GPU Process Memory(*)\\Shared Usage", 0, &c) == ERROR_SUCCESS) {
        pdhGpuMemShared_ = c;
    }
    if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(_Total)\\% Disk Time", 0, &c) == ERROR_SUCCESS) {
        pdhDiskTime_ = c;
    }
    if (PdhAddEnglishCounterW(q, L"\\Network Interface(*)\\Bytes Total/sec", 0, &c) == ERROR_SUCCESS) {
        pdhNetBytes_ = c;
    }
    if (PdhAddEnglishCounterW(q, L"\\Network Interface(*)\\Current Bandwidth", 0, &c) == ERROR_SUCCESS) {
        pdhNetBandwidth_ = c;
    }
    PdhCollectQueryData(q);
    pdhReady_.store(true, std::memory_order_release);
}

void ProcessMonitor::ShutdownPdh() {
    pdhReady_.store(false, std::memory_order_release);
    if (pdhInitThread_.joinable()) {
        pdhInitThread_.join();
    }
    if (pdhQuery_) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(pdhQuery_));
        pdhQuery_ = nullptr;
        pdhGpuEngine_ = nullptr;
        pdhGpuMemDed_ = nullptr;
        pdhGpuMemShared_ = nullptr;
        pdhDiskTime_ = nullptr;
        pdhNetBytes_ = nullptr;
        pdhNetBandwidth_ = nullptr;
    }
}

static bool ParseGpuPid(const wchar_t* instName, DWORD& outPid, std::wstring& outEngineLabel) {
    // Format: pid_1234_luid_0x00000000_0x00012345_phys_0_eng_0_engtype_3D
    if (!instName) return false;
    const wchar_t* p = wcsstr(instName, L"pid_");
    if (!p) return false;
    outPid = static_cast<DWORD>(_wtoi(p + 4));
    if (outPid == 0) return false;

    int phys = 0;
    const wchar_t* pPhys = wcsstr(instName, L"phys_");
    if (pPhys) phys = _wtoi(pPhys + 5);

    const wchar_t* pEngType = wcsstr(instName, L"engtype_");
    if (pEngType) {
        wchar_t buf[64];
        swprintf_s(buf, L"GPU %d - %s", phys, pEngType + 8);
        outEngineLabel = buf;
    }
    return true;
}

void ProcessMonitor::SamplePdh() {
    pdhGpuByPid_.clear();
    totals_.diskPercent = 0.0;
    totals_.networkPercent = 0.0;
    totals_.networkBytesPerSec = 0.0;
    totals_.gpuPercent = 0.0;

    if (!pdhReady_.load(std::memory_order_acquire) || !pdhQuery_) return;
    PDH_HQUERY q = static_cast<PDH_HQUERY>(pdhQuery_);
    if (PdhCollectQueryData(q) != ERROR_SUCCESS) return;

    // Total Disk %
    if (pdhDiskTime_) {
        PDH_FMT_COUNTERVALUE val{};
        if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdhDiskTime_), PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
            totals_.diskPercent = (std::min)(100.0, (std::max)(0.0, val.doubleValue));
        }
    }

    // Total Network %
    if (pdhNetBytes_ && pdhNetBandwidth_) {
        DWORD sz = 0, cnt = 0;
        double sumBytes = 0.0;
        PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhNetBytes_), PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
        if (sz > 0) {
            std::vector<BYTE> buf(sz);
            auto items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
            if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhNetBytes_), PDH_FMT_DOUBLE, &sz, &cnt, items) == ERROR_SUCCESS) {
                for (DWORD i = 0; i < cnt; ++i) {
                    if (items[i].FmtValue.CStatus == ERROR_SUCCESS) sumBytes += items[i].FmtValue.doubleValue;
                }
            }
        }
        sz = 0; cnt = 0;
        double maxBw = 0.0;
        PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhNetBandwidth_), PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
        if (sz > 0) {
            std::vector<BYTE> buf(sz);
            auto items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
            if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhNetBandwidth_), PDH_FMT_DOUBLE, &sz, &cnt, items) == ERROR_SUCCESS) {
                for (DWORD i = 0; i < cnt; ++i) {
                    if (items[i].FmtValue.CStatus == ERROR_SUCCESS && items[i].FmtValue.doubleValue > maxBw) {
                        maxBw = items[i].FmtValue.doubleValue;
                    }
                }
            }
        }
        totals_.networkBytesPerSec = sumBytes;
        if (maxBw > 0.0) {
            totals_.networkPercent = (std::min)(100.0, (sumBytes * 8.0 / maxBw) * 100.0);
        }
    }

    // Per-process GPU Engine utilization
    if (pdhGpuEngine_) {
        DWORD sz = 0, cnt = 0;
        PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhGpuEngine_), PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
        if (sz > 0) {
            std::vector<BYTE> buf(sz);
            auto items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
            if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhGpuEngine_), PDH_FMT_DOUBLE, &sz, &cnt, items) == ERROR_SUCCESS) {
                // Track max engine per pid and sum system gpu approx
                std::unordered_map<DWORD, double> pidTopEngineVal;
                std::unordered_map<std::wstring, double> engineTotals;
                for (DWORD i = 0; i < cnt; ++i) {
                    if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
                    double v = items[i].FmtValue.doubleValue;
                    if (v <= 0.0) continue;
                    DWORD pid = 0;
                    std::wstring eng;
                    if (ParseGpuPid(items[i].szName, pid, eng)) {
                        auto& entry = pdhGpuByPid_[pid];
                        if (v > entry.utilPct) entry.utilPct = (std::min)(100.0, v);
                        if (v > pidTopEngineVal[pid]) {
                            pidTopEngineVal[pid] = v;
                            entry.topEngine = eng;
                        }
                        engineTotals[eng] += v;
                    }
                }
                double maxEng = 0.0;
                for (const auto& kv : engineTotals) {
                    if (kv.second > maxEng) maxEng = kv.second;
                }
                totals_.gpuPercent = (std::min)(100.0, maxEng);
            }
        }
    }

    // Per-process GPU Dedicated / Shared memory
    auto readGpuMem = [&](void* hCounter, bool dedicated) {
        if (!hCounter) return;
        DWORD sz = 0, cnt = 0;
        PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(hCounter), PDH_FMT_LARGE, &sz, &cnt, nullptr);
        if (sz == 0) return;
        std::vector<BYTE> buf(sz);
        auto items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
        if (PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(hCounter), PDH_FMT_LARGE, &sz, &cnt, items) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < cnt; ++i) {
                if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
                DWORD pid = 0;
                std::wstring dummy;
                if (ParseGpuPid(items[i].szName, pid, dummy)) {
                    uint64_t bytes = static_cast<uint64_t>((std::max)(0LL, items[i].FmtValue.largeValue));
                    if (dedicated) pdhGpuByPid_[pid].dedBytes += bytes;
                    else pdhGpuByPid_[pid].sharedBytes += bytes;
                }
            }
        }
    };
    readGpuMem(pdhGpuMemDed_, true);
    readGpuMem(pdhGpuMemShared_, false);
}

// ETW helper struct for clean callback access
struct EtwStateBridge {
    std::mutex* mtx = nullptr;
    std::unordered_map<DWORD, uint64_t>* disk = nullptr;
    std::unordered_map<DWORD, uint64_t>* net = nullptr;
};
static EtwStateBridge g_etwBridge{};

static VOID WINAPI EtwCallback(PEVENT_RECORD ev) {
    if (!ev || !g_etwBridge.mtx) return;
    const GUID& g = ev->EventHeader.ProviderId;
    UCHAR op = ev->EventHeader.EventDescriptor.Opcode;

    if (IsEqualGUID(g, kTcpIpGuid) || IsEqualGUID(g, kUdpIpGuid)) {
        if ((op == 10 || op == 11 || op == 26 || op == 27) && ev->UserData && ev->UserDataLength >= 8) {
            const uint32_t* d = reinterpret_cast<const uint32_t*>(ev->UserData);
            DWORD pid = d[0];
            uint32_t size = d[1];
            if (pid > 0 && size > 0 && size < (1u << 26)) {
                std::lock_guard<std::mutex> lk(*g_etwBridge.mtx);
                (*g_etwBridge.net)[pid] += size;
            }
        }
    } else if (IsEqualGUID(g, kDiskIoGuid)) {
        // Opcode 10=Read, 11=Write. TransferSize is at offset 8 (after DiskNumber+IrpFlags)
        if ((op == 10 || op == 11) && ev->UserData && ev->UserDataLength >= 12) {
            const uint32_t* d = reinterpret_cast<const uint32_t*>(ev->UserData);
            uint32_t transferSize = d[2];
            DWORD pid = ev->EventHeader.ProcessId;
            if (pid > 0 && pid != 0xFFFFFFFFu && transferSize > 0 && transferSize < (1u << 26)) {
                std::lock_guard<std::mutex> lk(*g_etwBridge.mtx);
                (*g_etwBridge.disk)[pid] += transferSize;
            }
        }
    }
}

void ProcessMonitor::StartEtwSession() {
    if (etwRunning_.load()) return;

    size_t propsSize = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(kEtwSessionName) + 1) * sizeof(wchar_t);
    std::vector<BYTE> propBuf(propsSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propBuf.data());
    props->Wnode.BufferSize = static_cast<ULONG>(propsSize);
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;
    props->Wnode.Guid = kSessionGuid;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    props->EnableFlags = EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_NETWORK_TCPIP;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize = 64;
    props->MinimumBuffers = 4;
    props->MaximumBuffers = 16;

    // Stop stale session if left behind
    ControlTraceW(0, kEtwSessionName, props, EVENT_TRACE_CONTROL_STOP);
    ZeroMemory(propBuf.data(), propsSize);
    props->Wnode.BufferSize = static_cast<ULONG>(propsSize);
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;
    props->Wnode.Guid = kSessionGuid;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    props->EnableFlags = EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_NETWORK_TCPIP;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize = 64;

    TRACEHANDLE hSession = 0;
    if (StartTraceW(&hSession, kEtwSessionName, props) != ERROR_SUCCESS) {
        return;
    }
    etwSessionHandle_ = static_cast<uint64_t>(hSession);
    g_etwBridge.mtx = &etwMutex_;
    g_etwBridge.disk = &etwDiskBytes_;
    g_etwBridge.net = &etwNetBytes_;
    etwStopRequested_ = false;
    etwRunning_ = true;
    etwThread_ = std::thread(&ProcessMonitor::EtwThreadProc, this);
}

void ProcessMonitor::EtwThreadProc() {
    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = const_cast<LPWSTR>(kEtwSessionName);
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = EtwCallback;

    TRACEHANDLE hTrace = OpenTraceW(&lf);
    if (hTrace != INVALID_PROCESSTRACE_HANDLE) {
        etwTraceHandle_ = static_cast<uint64_t>(hTrace);
        ProcessTrace(&hTrace, 1, nullptr, nullptr);
        CloseTrace(hTrace);
        etwTraceHandle_ = ~0ULL;
    }
    etwRunning_ = false;
}

void ProcessMonitor::StopEtwSession() {
    etwStopRequested_ = true;
    if (etwSessionHandle_ != 0) {
        size_t propsSize = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(kEtwSessionName) + 1) * sizeof(wchar_t);
        std::vector<BYTE> propBuf(propsSize, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propBuf.data());
        props->Wnode.BufferSize = static_cast<ULONG>(propsSize);
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(static_cast<TRACEHANDLE>(etwSessionHandle_), kEtwSessionName, props, EVENT_TRACE_CONTROL_STOP);
        etwSessionHandle_ = 0;
    }
    if (etwThread_.joinable()) {
        etwThread_.join();
    }
    g_etwBridge.mtx = nullptr;
}

void ProcessMonitor::CollectVisibleWindows() {
    windowTitles_.clear();
    struct EnumCtx {
        std::unordered_map<DWORD, std::vector<std::wstring>>* map;
    } ctx{ &windowTitles_ };

    struct ChildFindCtx {
        DWORD hostPid;
        DWORD uwpPid;
    };
    auto findUwpChild = [](HWND child, LPARAM lp) -> BOOL {
        auto* cc = reinterpret_cast<ChildFindCtx*>(lp);
        DWORD cpid = 0;
        GetWindowThreadProcessId(child, &cpid);
        if (cpid != 0 && cpid != cc->hostPid) {
            wchar_t ccls[64] = {};
            GetClassNameW(child, ccls, ARRAYSIZE(ccls));
            if (wcscmp(ccls, L"Windows.UI.Core.CoreWindow") == 0 || cc->uwpPid == 0) {
                cc->uwpPid = cpid;
                if (wcscmp(ccls, L"Windows.UI.Core.CoreWindow") == 0) return FALSE;
            }
        }
        return TRUE;
    };

    struct LambdaCtx {
        std::unordered_map<DWORD, std::vector<std::wstring>>* map;
        BOOL (CALLBACK *findUwpChild)(HWND, LPARAM);
    } lctx{ &windowTitles_, findUwpChild };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<LambdaCtx*>(lp);
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW)) return TRUE;

        // Ignore cloaked UWP background windows (Win10+)
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
            return TRUE;
        }

        int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0) return TRUE;

        wchar_t cls[64] = {};
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0 &&
            wcscmp(cls, L"ApplicationFrameWindow") == 0) {
            ChildFindCtx cc{ pid, 0 };
            EnumChildWindows(hwnd, c->findUwpChild, reinterpret_cast<LPARAM>(&cc));
            if (cc.uwpPid != 0) pid = cc.uwpPid;
        }

        std::wstring title(static_cast<size_t>(len) + 1, L'\0');
        int copied = GetWindowTextW(hwnd, &title[0], len + 1);
        title.resize(static_cast<size_t>(copied));
        if (!title.empty()) {
            (*c->map)[pid].push_back(std::move(title));
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&lctx));
}

HICON ProcessMonitor::GetIconForPath(const std::wstring& path) {
    if (path.empty()) return nullptr;
    std::wstring key = ToLower(path);
    auto it = iconCache_.find(key);
    if (it != iconCache_.end()) return it->second;

    SHFILEINFOW sfi{};
    HICON ico = nullptr;
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) {
        ico = sfi.hIcon;
    }
    iconCache_[key] = ico;
    return ico;
}

std::wstring ProcessMonitor::GetDescriptionForPath(const std::wstring& path) {
    if (path.empty()) return L"";
    std::wstring key = ToLower(path);
    auto it = descCache_.find(key);
    if (it != descCache_.end()) return it->second;

    std::wstring desc;
    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (verSize > 0) {
        std::vector<BYTE> verBuf(verSize);
        if (GetFileVersionInfoW(path.c_str(), 0, verSize, verBuf.data())) {
            struct LANGANDCODEPAGE { WORD wLanguage; WORD wCodePage; };
            LANGANDCODEPAGE* lpTranslate = nullptr;
            UINT cbTranslate = 0;
            if (VerQueryValueW(verBuf.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate) &&
                cbTranslate >= sizeof(LANGANDCODEPAGE)) {
                for (UINT i = 0; i < cbTranslate / sizeof(LANGANDCODEPAGE); ++i) {
                    wchar_t subBlock[128];
                    swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                               lpTranslate[i].wLanguage, lpTranslate[i].wCodePage);
                    wchar_t* strPtr = nullptr;
                    UINT strLen = 0;
                    if (VerQueryValueW(verBuf.data(), subBlock, reinterpret_cast<LPVOID*>(&strPtr), &strLen) &&
                        strPtr && strLen > 0) {
                        desc = strPtr;
                        break;
                    }
                }
            }
            if (desc.empty()) {
                wchar_t* strPtr = nullptr;
                UINT strLen = 0;
                if (VerQueryValueW(verBuf.data(), L"\\StringFileInfo\\040904b0\\FileDescription",
                                   reinterpret_cast<LPVOID*>(&strPtr), &strLen) && strPtr && strLen > 0) {
                    desc = strPtr;
                }
            }
        }
    }
    descCache_[key] = desc;
    return desc;
}

static bool IsKnownWindowsProcessName(const std::wstring& lowName) {
    static const wchar_t* const kWinProcs[] = {
        L"system", L"registry", L"memory compression", L"secure system", L"vmmem", L"vmmemwsl",
        L"smss.exe", L"csrss.exe", L"wininit.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe",
        L"winlogon.exe", L"dwm.exe", L"fontdrvhost.exe", L"audiodg.exe", L"svchost.exe",
        L"sihost.exe", L"taskhostw.exe", L"ctfmon.exe", L"conhost.exe", L"dllhost.exe",
        L"runtimebroker.exe", L"searchindexer.exe", L"searchapp.exe", L"searchhost.exe",
        L"searchprotocolhost.exe", L"searchfilterhost.exe", L"startmenuexperiencehost.exe",
        L"shellexperiencehost.exe", L"textinputhost.exe", L"lockapp.exe", L"spoolsv.exe",
        L"sppsvc.exe", L"securityhealthservice.exe", L"securityhealthsystray.exe",
        L"msmpeng.exe", L"nissrv.exe", L"mpdefendercoreservice.exe", L"sgrmbroker.exe",
        L"dashost.exe", L"wudfhost.exe", L"wlanext.exe", L"tiworker.exe",
        L"trustedinstaller.exe", L"wmiprvse.exe", L"unsecapp.exe", L"aggregatorhost.exe",
        L"backgroundtaskhost.exe", L"smartscreen.exe", L"applicationframehost.exe",
        L"systemsettings.exe", L"systemsettingsbroker.exe", L"musnotification.exe"
    };
    for (const wchar_t* name : kWinProcs) {
        if (lowName == name) return true;
    }
    return false;
}

void ProcessMonitor::QueryStaticMeta(DWORD pid, ULONGLONG createTime, const std::wstring& procName,
                                     StaticMeta& meta, bool needCmdLine) {
    bool fresh = (meta.createTime != createTime || meta.createTime == 0);
    if (!fresh && (!needCmdLine || meta.cmdLineQueried)) {
        return;
    }

    if (fresh) {
        meta = StaticMeta{};
        meta.createTime = createTime;
    }

    if (pid == 0 || pid == 4) {
        meta.isWindowsBinary = true;
        meta.platform = L"64-bit";
        meta.userOnly = L"SYSTEM";
        meta.userFull = L"NT AUTHORITY\\SYSTEM";
        if (pid == 0) meta.description = L"System Idle Process";
        else meta.description = L"NT Kernel & System";
        meta.cmdLineQueried = true;
        return;
    }

    std::wstring lowName = ToLower(procName);
    if (IsKnownWindowsProcessName(lowName)) {
        meta.isWindowsBinary = true;
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        if (lowName == L"registry" || lowName == L"memory compression" || lowName == L"secure system") {
            meta.userOnly = L"SYSTEM";
            meta.userFull = L"NT AUTHORITY\\SYSTEM";
        }
        if (fresh && meta.isWindowsBinary && !windowsDirLower_.empty() && procName.find(L'.') != std::wstring::npos) {
            std::wstring sysCand = windowsDirLower_ + L"\\system32\\" + procName;
            if (GetFileAttributesW(sysCand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                meta.icon = GetIconForPath(sysCand);
                meta.description = GetDescriptionForPath(sysCand);
            }
        }
        return;
    }

    if (fresh) {
        wchar_t pathBuf[MAX_PATH * 2] = {};
        DWORD pathLen = ARRAYSIZE(pathBuf);
        if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen)) {
            meta.imagePath.assign(pathBuf, pathLen);
            meta.icon = GetIconForPath(meta.imagePath);
            meta.description = GetDescriptionForPath(meta.imagePath);

            std::wstring low = ToLower(meta.imagePath);
            if (!windowsDirLower_.empty() &&
                (low.rfind(windowsDirLower_ + L"\\system32\\", 0) == 0 ||
                 low.rfind(windowsDirLower_ + L"\\syswow64\\", 0) == 0 ||
                 low.rfind(windowsDirLower_ + L"\\systemapps\\", 0) == 0 ||
                 low.rfind(windowsDirLower_ + L"\\servicing\\", 0) == 0 ||
                 low.rfind(windowsDirLower_ + L"\\immersivecontrolpanel\\", 0) == 0 ||
                 low == windowsDirLower_ + L"\\explorer.exe")) {
                meta.isWindowsBinary = true;
            }
        }

        // Architecture
        static auto pfnIsWow64_2 = reinterpret_cast<PFN_IsWow64Process2>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
        if (pfnIsWow64_2) {
            USHORT procMach = 0, nativeMach = 0;
            if (pfnIsWow64_2(hProc, &procMach, &nativeMach)) {
                if (procMach == IMAGE_FILE_MACHINE_UNKNOWN) {
                    meta.platform = (nativeMach == IMAGE_FILE_MACHINE_ARM64) ? L"ARM64" : L"64-bit";
                } else {
                    meta.platform = L"32-bit";
                }
            }
        } else {
            BOOL wow = FALSE;
            if (IsWow64Process(hProc, &wow)) {
                meta.platform = wow ? L"32-bit" : L"64-bit";
            }
        }

        // Token info: user, elevation, UAC virtualization
        HANDLE hToken = nullptr;
        if (OpenProcessToken(hProc, TOKEN_QUERY, &hToken)) {
            BYTE tokBuf[512];
            DWORD retLen = 0;
            if (GetTokenInformation(hToken, TokenUser, tokBuf, sizeof(tokBuf), &retLen)) {
                PSID sid = reinterpret_cast<TOKEN_USER*>(tokBuf)->User.Sid;
                LPWSTR sidStr = nullptr;
                std::wstring sidKey;
                if (ConvertSidToStringSidW(sid, &sidStr)) {
                    sidKey = sidStr;
                    LocalFree(sidStr);
                }
                auto sit = sidCache_.find(sidKey);
                if (sit != sidCache_.end()) {
                    meta.userOnly = sit->second.first;
                    meta.userFull = sit->second.second;
                } else {
                    wchar_t nameBuf[128] = {}, domBuf[128] = {};
                    DWORD nameLen = 128, domLen = 128;
                    SID_NAME_USE snu = SidTypeUnknown;
                    if (LookupAccountSidW(nullptr, sid, nameBuf, &nameLen, domBuf, &domLen, &snu)) {
                        meta.userOnly = nameBuf;
                        meta.userFull = (domBuf[0] ? std::wstring(domBuf) + L"\\" + nameBuf : std::wstring(nameBuf));
                        if (!sidKey.empty()) sidCache_[sidKey] = { meta.userOnly, meta.userFull };
                    }
                }
            }
            TOKEN_ELEVATION te{};
            if (GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &retLen)) {
                meta.hasElevatedQuery = true;
                meta.isElevated = (te.TokenIsElevated != 0);
            }
            DWORD virt = 0;
            if (GetTokenInformation(hToken, TokenVirtualizationEnabled, &virt, sizeof(virt), &retLen)) {
                meta.uacVirtualized = (virt != 0);
            }
            CloseHandle(hToken);
        }
    }

    if (needCmdLine && !meta.cmdLineQueried) {
        meta.cmdLineQueried = true;
        static auto pfnNtQueryProc = reinterpret_cast<PFN_NtQueryInformationProcess>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        if (pfnNtQueryProc) {
            // ProcessCommandLineInformation == 60
            ULONG len = 0;
            std::vector<BYTE> buf(2048);
            NTSTATUS st = pfnNtQueryProc(hProc, 60, buf.data(), static_cast<ULONG>(buf.size()), &len);
            if (st == static_cast<NTSTATUS>(0xC0000004L) /*STATUS_INFO_LENGTH_MISMATCH*/ ||
                st == static_cast<NTSTATUS>(0xC0000023L) /*STATUS_BUFFER_TOO_SMALL*/) {
                if (len > buf.size() && len <= 65536) {
                    buf.resize(len);
                    st = pfnNtQueryProc(hProc, 60, buf.data(), len, &len);
                }
            }
            if (NT_SUCCESS(st)) {
                const UNICODE_STRING* us = reinterpret_cast<const UNICODE_STRING*>(buf.data());
                if (us->Buffer && us->Length > 0) {
                    meta.commandLine.assign(us->Buffer, us->Length / sizeof(wchar_t));
                }
            }
        }
    }

    CloseHandle(hProc);
}

void ProcessMonitor::Refresh(bool showFullAccountName, bool needCommandLines) {
    static auto pfnNtQuerySys = reinterpret_cast<PFN_NtQuerySystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!pfnNtQuerySys) return;

    CollectVisibleWindows();
    SamplePdh();

    // System timing delta
    FILETIME idleFt{}, kernelFt{}, userFt{};
    ULONGLONG sysDelta = 0;
    ULONGLONG idleDelta = 0;
    if (GetSystemTimes(&idleFt, &kernelFt, &userFt)) {
        ULONGLONG curIdle = FileTimeToU64(idleFt);
        ULONGLONG curSys = FileTimeToU64(kernelFt) + FileTimeToU64(userFt);
        if (curSys > prevSysTime100ns_) {
            sysDelta = curSys - prevSysTime100ns_;
            idleDelta = (curIdle >= prevIdleTime100ns_) ? (curIdle - prevIdleTime100ns_) : 0;
            totals_.cpuPercent = (std::min)(100.0, (std::max)(0.0,
                100.0 * static_cast<double>(sysDelta - idleDelta) / static_cast<double>(sysDelta)));
        }
        prevIdleTime100ns_ = curIdle;
        prevSysTime100ns_ = curSys;
    }

    ULONGLONG nowTick = GetTickCount64();
    double dtSec = (nowTick > prevTickMs_) ? static_cast<double>(nowTick - prevTickMs_) / 1000.0 : 1.0;
    if (dtSec < 0.05) dtSec = 0.05;
    prevTickMs_ = nowTick;

    // Memory totals
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        totals_.totalMemoryBytes = ms.ullTotalPhys;
        totals_.usedMemoryBytes = ms.ullTotalPhys - ms.ullAvailPhys;
        totals_.memoryPercent = static_cast<double>(ms.dwMemoryLoad);
    }

    // Drain ETW deltas
    std::unordered_map<DWORD, uint64_t> etwDiskSnapshot, etwNetSnapshot;
    if (etwRunning_.load()) {
        std::lock_guard<std::mutex> lk(etwMutex_);
        etwDiskSnapshot.swap(etwDiskBytes_);
        etwNetSnapshot.swap(etwNetBytes_);
    }

    // Query SystemProcessInformation
    ULONG bufSize = 512 * 1024;
    std::vector<BYTE> sysBuf(bufSize);
    ULONG retLen = 0;
    NTSTATUS st = pfnNtQuerySys(5 /*SystemProcessInformation*/, sysBuf.data(), bufSize, &retLen);
    while (st == static_cast<NTSTATUS>(0xC0000004L) /*STATUS_INFO_LENGTH_MISMATCH*/) {
        bufSize = (retLen > bufSize ? retLen : bufSize * 2) + 64 * 1024;
        sysBuf.resize(bufSize);
        st = pfnNtQuerySys(5, sysBuf.data(), bufSize, &retLen);
    }
    if (!NT_SUCCESS(st)) return;

    std::vector<ProcessInfo> nextList;
    nextList.reserve(320);
    std::vector<ULONGLONG> nextOtherDeltas;
    nextOtherDeltas.reserve(320);

    std::unordered_map<DWORD, PrevTiming> nextTimings;
    nextTimings.reserve(320);

    const BYTE* ptr = sysBuf.data();
    while (true) {
        const auto* spi = reinterpret_cast<const NT_SYSTEM_PROCESS_INFORMATION*>(ptr);
        DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(spi->UniqueProcessId));

        if (pid != 0) { // Win10 Processes/Details skips Idle PID 0 in Processes tab (or shows in Details; keep PID >= 4 in Processes)
            ProcessInfo info;
            info.pid = pid;
            info.parentPid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(spi->InheritedFromUniqueProcessId));
            info.sessionId = spi->SessionId;
            info.createTime = static_cast<ULONGLONG>(spi->CreateTime.QuadPart);

            if (spi->ImageName.Buffer && spi->ImageName.Length > 0) {
                info.name.assign(spi->ImageName.Buffer, spi->ImageName.Length / sizeof(wchar_t));
            } else if (pid == 4) {
                info.name = L"System";
            } else {
                info.name = L"System Process";
            }

            StaticMeta& meta = metaCache_[pid];
            QueryStaticMeta(pid, info.createTime, info.name, meta, needCommandLines);

            info.imagePath = meta.imagePath;
            info.description = meta.description;
            info.icon = meta.icon;
            info.userName = showFullAccountName ? meta.userFull : meta.userOnly;
            info.commandLine = meta.commandLine;
            info.platform = meta.platform;
            info.isElevated = meta.isElevated;
            info.hasElevatedQuery = meta.hasElevatedQuery;
            info.uacVirtualized = meta.uacVirtualized;

            auto winIt = windowTitles_.find(pid);
            if (winIt != windowTitles_.end() && !winIt->second.empty()) {
                info.windowTitles = winIt->second;
                info.windowTitle = winIt->second[0];
            }

            if (!info.description.empty()) info.displayName = info.description;
            else if (!info.windowTitle.empty()) info.displayName = info.windowTitle;
            else {
                // Strip .exe if cleaner? Actually Win10 shows description if present else file name without .exe or with .exe
                info.displayName = info.name;
            }

            // Category classification
            if (!info.windowTitle.empty()) {
                info.category = ProcessCategory::App;
            } else if (meta.isWindowsBinary || pid == 4) {
                info.category = ProcessCategory::Windows;
            } else {
                info.category = ProcessCategory::Background;
            }

            // Check suspended (all threads State==5 && WaitReason==5)
            if (spi->NumberOfThreads > 0) {
                bool allSuspended = true;
                for (ULONG t = 0; t < spi->NumberOfThreads; ++t) {
                    if (!(spi->Threads[t].ThreadState == 5 && spi->Threads[t].WaitReason == 5)) {
                        allSuspended = false;
                        break;
                    }
                }
                info.isSuspended = allSuspended;
            }

            info.threadCount = spi->NumberOfThreads;
            info.handleCount = spi->HandleCount;
            info.privateWorkingSet = static_cast<uint64_t>(spi->WorkingSetPrivateSize.QuadPart);
            if (info.privateWorkingSet == 0 && spi->WorkingSetSize > 0) {
                info.privateWorkingSet = static_cast<uint64_t>(spi->WorkingSetSize);
            }
            info.workingSet = static_cast<uint64_t>(spi->WorkingSetSize);
            info.commitSize = static_cast<uint64_t>(spi->PrivatePageCount);

            ULONGLONG cpuTotal = static_cast<ULONGLONG>(spi->KernelTime.QuadPart) +
                                 static_cast<ULONGLONG>(spi->UserTime.QuadPart);
            info.totalCpuTime100ns = cpuTotal;

            ULONGLONG ioTransfer = static_cast<ULONGLONG>(spi->ReadTransferCount.QuadPart) +
                                   static_cast<ULONGLONG>(spi->WriteTransferCount.QuadPart);
            ULONGLONG ioOther = static_cast<ULONGLONG>(spi->OtherTransferCount.QuadPart);
            ULONGLONG ioOtherDelta = 0;

            PrevTiming curT{};
            curT.createTime = info.createTime;
            curT.cpuTime100ns = cpuTotal;
            curT.ioTransferBytes = ioTransfer;
            curT.ioOtherBytes = ioOther;

            auto ptIt = prevTimings_.find(pid);
            if (ptIt != prevTimings_.end() && ptIt->second.createTime == info.createTime) {
                if (ioOther >= ptIt->second.ioOtherBytes) {
                    ioOtherDelta = ioOther - ptIt->second.ioOtherBytes;
                }
                if (sysDelta > 0 && cpuTotal >= ptIt->second.cpuTime100ns) {
                    ULONGLONG dCpu = cpuTotal - ptIt->second.cpuTime100ns;
                    info.cpuPercent = (std::min)(100.0,
                        100.0 * static_cast<double>(dCpu) / static_cast<double>(sysDelta));
                }

                // Disk I/O
                auto edIt = etwDiskSnapshot.find(pid);
                if (etwRunning_.load() && edIt != etwDiskSnapshot.end()) {
                    info.diskBytesPerSec = static_cast<double>(edIt->second) / dtSec;
                } else if (ioTransfer >= ptIt->second.ioTransferBytes) {
                    info.diskBytesPerSec = static_cast<double>(ioTransfer - ptIt->second.ioTransferBytes) / dtSec;
                }

                // Network I/O
                auto enIt = etwNetSnapshot.find(pid);
                if (enIt != etwNetSnapshot.end()) {
                    info.netBytesPerSec = static_cast<double>(enIt->second) / dtSec;
                }
            }

            // GPU data
            auto gIt = pdhGpuByPid_.find(pid);
            if (gIt != pdhGpuByPid_.end()) {
                info.gpuPercent = gIt->second.utilPct;
                info.gpuEngine = gIt->second.topEngine;
                info.gpuDedicatedBytes = gIt->second.dedBytes;
                info.gpuSharedBytes = gIt->second.sharedBytes;
            }

            // Power usage heuristic matching Win10 (CPU + GPU + Disk activity)
            double powerScore = info.cpuPercent * 1.4 + info.gpuPercent * 0.8 +
                                (info.diskBytesPerSec / (1024.0 * 1024.0)) * 0.5;
            if (powerScore >= 35.0) info.powerUsageLevel = 4;
            else if (powerScore >= 15.0) info.powerUsageLevel = 3;
            else if (powerScore >= 4.0) info.powerUsageLevel = 2;
            else if (powerScore >= 0.3) info.powerUsageLevel = 1;
            else info.powerUsageLevel = 0;

            double trendScore = (ptIt != prevTimings_.end()) ? (0.65 * ptIt->second.prevPowerScore + 0.35 * powerScore) : powerScore;
            curT.prevPowerScore = trendScore;
            if (trendScore >= 35.0) info.powerTrendLevel = 4;
            else if (trendScore >= 15.0) info.powerTrendLevel = 3;
            else if (trendScore >= 4.0) info.powerTrendLevel = 2;
            else if (trendScore >= 0.3) info.powerTrendLevel = 1;
            else info.powerTrendLevel = 0;

            nextTimings[pid] = curT;
            nextList.push_back(std::move(info));
            nextOtherDeltas.push_back(ioOtherDelta);
        }

        if (spi->NextEntryOffset == 0) break;
        ptr += spi->NextEntryOffset;
    }

    // Non-elevated fallback (and backup when ETW kernel session is unavailable):
    // Attribute live system adapter throughput across processes holding active TCP/UDP sockets,
    // weighted by per-process OtherTransferCount (Winsock/AFD IOCTL byte deltas).
    if (!etwRunning_.load() && totals_.networkBytesPerSec > 0.0 && !nextList.empty()) {
        std::unordered_map<DWORD, uint32_t> tcpEstabByPid;
        std::unordered_map<DWORD, uint32_t> udpByPid;
        QueryActiveSocketOwners(tcpEstabByPid, udpByPid);

        double sumOtherWeight = 0.0;
        double sumSockWeight = 0.0;
        for (size_t i = 0; i < nextList.size(); ++i) {
            DWORD pid = nextList[i].pid;
            auto tIt = tcpEstabByPid.find(pid);
            auto uIt = udpByPid.find(pid);
            uint32_t tc = (tIt != tcpEstabByPid.end()) ? tIt->second : 0;
            uint32_t uc = (uIt != udpByPid.end()) ? uIt->second : 0;
            if (tc == 0 && uc == 0) continue;
            sumOtherWeight += static_cast<double>(nextOtherDeltas[i]);
            sumSockWeight += static_cast<double>(tc * 4u + uc);
        }

        if (sumOtherWeight > 0.0) {
            for (size_t i = 0; i < nextList.size(); ++i) {
                DWORD pid = nextList[i].pid;
                if (tcpEstabByPid.find(pid) == tcpEstabByPid.end() &&
                    udpByPid.find(pid) == udpByPid.end()) continue;
                if (nextOtherDeltas[i] > 0) {
                    nextList[i].netBytesPerSec = totals_.networkBytesPerSec *
                        (static_cast<double>(nextOtherDeltas[i]) / sumOtherWeight);
                }
            }
        } else if (sumSockWeight > 0.0) {
            for (size_t i = 0; i < nextList.size(); ++i) {
                DWORD pid = nextList[i].pid;
                auto tIt = tcpEstabByPid.find(pid);
                auto uIt = udpByPid.find(pid);
                uint32_t tc = (tIt != tcpEstabByPid.end()) ? tIt->second : 0;
                uint32_t uc = (uIt != udpByPid.end()) ? uIt->second : 0;
                if (tc == 0 && uc == 0) continue;
                nextList[i].netBytesPerSec = totals_.networkBytesPerSec *
                    (static_cast<double>(tc * 4u + uc) / sumSockWeight);
            }
        }
    }

    prevTimings_.swap(nextTimings);
    processes_.swap(nextList);

    // Prune dead PIDs from metaCache_ occasionally
    if (metaCache_.size() > 600) {
        std::unordered_map<DWORD, StaticMeta> pruned;
        for (const auto& p : processes_) {
            auto it = metaCache_.find(p.pid);
            if (it != metaCache_.end()) pruned[p.pid] = std::move(it->second);
        }
        metaCache_.swap(pruned);
    }
}

bool ProcessMonitor::EndTask(DWORD pid) {
    if (pid <= 4) return false;
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) return false;
    BOOL ok = TerminateProcess(hProc, 1);
    CloseHandle(hProc);
    return ok != FALSE;
}

static void CollectDescendants(DWORD rootPid,
                               const std::vector<std::tuple<DWORD, DWORD, ULONGLONG>>& allProcs,
                               ULONGLONG rootCreateTime,
                               std::vector<DWORD>& outOrder) {
    for (const auto& item : allProcs) {
        DWORD pid = std::get<0>(item);
        DWORD ppid = std::get<1>(item);
        ULONGLONG ctime = std::get<2>(item);
        if (ppid == rootPid && pid != rootPid && ctime >= rootCreateTime) {
            CollectDescendants(pid, allProcs, ctime, outOrder);
            outOrder.push_back(pid);
        }
    }
}

bool ProcessMonitor::EndProcessTree(DWORD rootPid) {
    if (rootPid <= 4) return false;
    static auto pfnNtQuerySys = reinterpret_cast<PFN_NtQuerySystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!pfnNtQuerySys) return EndTask(rootPid);

    ULONG bufSize = 512 * 1024;
    std::vector<BYTE> sysBuf(bufSize);
    ULONG retLen = 0;
    NTSTATUS st = pfnNtQuerySys(5, sysBuf.data(), bufSize, &retLen);
    while (st == static_cast<NTSTATUS>(0xC0000004L)) {
        bufSize = (retLen > bufSize ? retLen : bufSize * 2) + 64 * 1024;
        sysBuf.resize(bufSize);
        st = pfnNtQuerySys(5, sysBuf.data(), bufSize, &retLen);
    }
    if (!NT_SUCCESS(st)) return EndTask(rootPid);

    std::vector<std::tuple<DWORD, DWORD, ULONGLONG>> list;
    ULONGLONG rootCreate = 0;
    const BYTE* ptr = sysBuf.data();
    while (true) {
        const auto* spi = reinterpret_cast<const NT_SYSTEM_PROCESS_INFORMATION*>(ptr);
        DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(spi->UniqueProcessId));
        DWORD ppid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(spi->InheritedFromUniqueProcessId));
        ULONGLONG ctime = static_cast<ULONGLONG>(spi->CreateTime.QuadPart);
        if (pid == rootPid) rootCreate = ctime;
        if (pid > 4) list.emplace_back(pid, ppid, ctime);
        if (spi->NextEntryOffset == 0) break;
        ptr += spi->NextEntryOffset;
    }

    std::vector<DWORD> killOrder;
    CollectDescendants(rootPid, list, rootCreate, killOrder);
    killOrder.push_back(rootPid);

    bool anyOk = false;
    for (DWORD pid : killOrder) {
        if (EndTask(pid)) anyOk = true;
    }
    return anyOk;
}

bool ProcessMonitor::SetPriority(DWORD pid, DWORD priorityClass) {
    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    BOOL ok = SetPriorityClass(hProc, priorityClass);
    CloseHandle(hProc);
    return ok != FALSE;
}

DWORD ProcessMonitor::GetPriority(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return NORMAL_PRIORITY_CLASS;
    DWORD p = GetPriorityClass(hProc);
    CloseHandle(hProc);
    return p ? p : NORMAL_PRIORITY_CLASS;
}

bool ProcessMonitor::SetAffinity(DWORD pid, DWORD_PTR mask) {
    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    BOOL ok = SetProcessAffinityMask(hProc, mask);
    CloseHandle(hProc);
    return ok != FALSE;
}

bool ProcessMonitor::GetAffinity(DWORD pid, DWORD_PTR& procMask, DWORD_PTR& sysMask) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    BOOL ok = GetProcessAffinityMask(hProc, &procMask, &sysMask);
    CloseHandle(hProc);
    return ok != FALSE;
}

bool ProcessMonitor::CreateDumpFile(DWORD pid, const std::wstring& procName, std::wstring& outPath) {
    HMODULE hDbg = LoadLibraryW(L"dbghelp.dll");
    if (!hDbg) return false;
    auto pfnDump = reinterpret_cast<PFN_MiniDumpWriteDump>(GetProcAddress(hDbg, "MiniDumpWriteDump"));
    if (!pfnDump) {
        FreeLibrary(hDbg);
        return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        FreeLibrary(hDbg);
        return false;
    }

    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring base = procName;
    size_t dot = base.rfind(L'.');
    if (dot != std::wstring::npos) base.resize(dot);
    if (base.empty()) base = L"process";

    std::wstring dumpPath = std::wstring(tempDir) + base + L".DMP";
    for (int idx = 2; PathFileExistsW(dumpPath.c_str()) && idx < 100; ++idx) {
        dumpPath = std::wstring(tempDir) + base + L" (" + std::to_wstring(idx) + L").DMP";
    }

    HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (hFile != INVALID_HANDLE_VALUE) {
        // MiniDumpWithFullMemory (2) matching Task Manager
        ok = pfnDump(hProc, pid, hFile, 2 /*MiniDumpWithFullMemory*/, nullptr, nullptr, nullptr) != FALSE;
        if (!ok) {
            // Fallback to MiniDumpNormal (0) if full memory fails
            SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
            SetEndOfFile(hFile);
            ok = pfnDump(hProc, pid, hFile, 0, nullptr, nullptr, nullptr) != FALSE;
        }
        CloseHandle(hFile);
        if (!ok) DeleteFileW(dumpPath.c_str());
    }

    CloseHandle(hProc);
    FreeLibrary(hDbg);
    if (ok) outPath = dumpPath;
    return ok;
}

} // namespace taskman
