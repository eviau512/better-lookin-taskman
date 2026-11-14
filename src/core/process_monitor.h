#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>

namespace taskman {

enum class ProcessCategory : int {
    App        = 0,
    Background = 1,
    Windows    = 2
};

struct ProcessInfo {
    DWORD pid = 0;
    DWORD parentPid = 0;
    DWORD sessionId = 0;
    ULONGLONG createTime = 0;

    std::wstring name;          // e.g. chrome.exe
    std::wstring displayName;   // FileDescription or WindowTitle or name
    std::wstring windowTitle;   // Primary visible top-level window title if any
    std::vector<std::wstring> windowTitles; // All visible top-level window titles for this PID
    std::wstring imagePath;
    std::wstring userName;      // user or DOMAIN\user
    std::wstring commandLine;
    std::wstring description;
    std::wstring platform;      // "64-bit" / "32-bit" / "ARM64"
    std::wstring gpuEngine;     // e.g. "GPU 0 - 3D"

    HICON icon = nullptr;       // owned by icon cache, do not destroy

    ProcessCategory category = ProcessCategory::Background;
    bool isSuspended = false;
    bool isElevated = false;
    bool hasElevatedQuery = false;
    bool uacVirtualized = false;
    bool isUwpOrStore = false;

    double cpuPercent = 0.0;
    uint64_t totalCpuTime100ns = 0; // Kernel + User

    uint64_t privateWorkingSet = 0;
    uint64_t workingSet = 0;
    uint64_t commitSize = 0;
    DWORD threadCount = 0;
    DWORD handleCount = 0;

    double diskBytesPerSec = 0.0;
    double netBytesPerSec = 0.0;
    double gpuPercent = 0.0;
    uint64_t gpuDedicatedBytes = 0;
    uint64_t gpuSharedBytes = 0;

    // Power usage index (0..4 => Very low .. Very high)
    int powerUsageLevel = 0;
    int powerTrendLevel = 0;
};

struct ProcessTotals {
    double cpuPercent = 0.0;
    double memoryPercent = 0.0;
    double diskPercent = 0.0;
    double networkPercent = 0.0;
    double gpuPercent = 0.0;
    uint64_t totalMemoryBytes = 0;
    uint64_t usedMemoryBytes = 0;
};

class ProcessMonitor {
public:
    ProcessMonitor();
    ~ProcessMonitor();

    void Start();
    void Stop();

    // Sample current processes and system totals. Safe to call on UI thread or worker.
    void Refresh(bool showFullAccountName, bool needCommandLines);

    const std::vector<ProcessInfo>& Processes() const { return processes_; }
    const ProcessTotals& Totals() const { return totals_; }

    HICON GetIconForPath(const std::wstring& path);
    std::wstring GetDescriptionForPath(const std::wstring& path);

    // Process control actions
    static bool EndTask(DWORD pid);
    static bool EndProcessTree(DWORD pid);
    static bool SetPriority(DWORD pid, DWORD priorityClass);
    static DWORD GetPriority(DWORD pid);
    static bool SetAffinity(DWORD pid, DWORD_PTR mask);
    static bool GetAffinity(DWORD pid, DWORD_PTR& procMask, DWORD_PTR& sysMask);
    static bool CreateDumpFile(DWORD pid, const std::wstring& procName, std::wstring& outPath);

    // Optional elevated ETW status
    bool IsEtwActive() const { return etwRunning_.load(); }

private:
    struct PrevTiming {
        ULONGLONG createTime = 0;
        ULONGLONG cpuTime100ns = 0;
        ULONGLONG ioTransferBytes = 0;
        double prevPowerScore = 0.0;
    };

    struct StaticMeta {
        ULONGLONG createTime = 0;
        std::wstring imagePath;
        std::wstring description;
        std::wstring userOnly;
        std::wstring userFull;
        std::wstring commandLine;
        std::wstring platform;
        bool isElevated = false;
        bool hasElevatedQuery = false;
        bool uacVirtualized = false;
        bool isWindowsBinary = false;
        bool cmdLineQueried = false;
        HICON icon = nullptr;
    };

    void InitPdh();
    void ShutdownPdh();
    void SamplePdh();

    void StartEtwSession();
    void StopEtwSession();
    void EtwThreadProc();

    void QueryStaticMeta(DWORD pid, ULONGLONG createTime, const std::wstring& procName,
                         StaticMeta& meta, bool needCmdLine);
    void CollectVisibleWindows();

    std::vector<ProcessInfo> processes_;
    ProcessTotals totals_{};

    std::unordered_map<DWORD, PrevTiming> prevTimings_;
    std::unordered_map<DWORD, StaticMeta> metaCache_;
    std::unordered_map<std::wstring, HICON> iconCache_;
    std::unordered_map<std::wstring, std::wstring> descCache_;
    std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> sidCache_;

    // Window map: pid -> visible window titles
    std::unordered_map<DWORD, std::vector<std::wstring>> windowTitles_;

    ULONGLONG prevSysTime100ns_ = 0;
    ULONGLONG prevIdleTime100ns_ = 0;
    ULONGLONG prevTickMs_ = 0;
    DWORD processorCount_ = 1;
    std::wstring windowsDirLower_;

    // PDH for GPU per-process & Disk% & Net%
    void* pdhQuery_ = nullptr;
    void* pdhGpuEngine_ = nullptr;
    void* pdhGpuMemDed_ = nullptr;
    void* pdhGpuMemShared_ = nullptr;
    void* pdhDiskTime_ = nullptr;
    void* pdhNetBytes_ = nullptr;
    void* pdhNetBandwidth_ = nullptr;

    struct PdhGpuSample {
        double utilPct = 0.0;
        std::wstring topEngine;
        uint64_t dedBytes = 0;
        uint64_t sharedBytes = 0;
    };
    std::unordered_map<DWORD, PdhGpuSample> pdhGpuByPid_;

    // Optional elevated ETW counters (bytes accumulated since last Refresh)
    std::mutex etwMutex_;
    std::unordered_map<DWORD, uint64_t> etwDiskBytes_;
    std::unordered_map<DWORD, uint64_t> etwNetBytes_;
    std::thread etwThread_;
    std::atomic<bool> etwRunning_{ false };
    std::atomic<bool> etwStopRequested_{ false };
    uint64_t etwSessionHandle_ = 0;
    uint64_t etwTraceHandle_ = ~0ULL;

    std::thread pdhInitThread_;
    std::atomic<bool> pdhReady_{ false };
};

} // namespace taskman
