#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "settings.h"

namespace taskman {

static constexpr int kHistoryLen = 60;
static constexpr int kRawHistoryLen = 480; // 480 * 500ms = 240s (4 minutes)
static constexpr uint32_t kPerfRingMagic = 0x42544D50u; // 'BTMP'
static constexpr uint32_t kPerfRingVersion = 1u;
static constexpr int kMaxRawCores = 64;
static constexpr int kMaxRawDisks = 8;
static constexpr int kMaxRawNets  = 8;
static constexpr int kMaxRawGpus  = 4;

#pragma pack(push, 1)
struct RawPerfSlot {
    uint64_t timestampMs = 0; // UTC milliseconds (FILETIME / 10000 - epoch diff)
    float cpuUtil = 0.0f;
    float cpuKernel = 0.0f;
    float coreUtil[kMaxRawCores] = {};
    float memUtil = 0.0f;
    float diskActive[kMaxRawDisks] = {};
    float diskReadMBs[kMaxRawDisks] = {};
    float diskWriteMBs[kMaxRawDisks] = {};
    float netSendMbps[kMaxRawNets] = {};
    float netRecvMbps[kMaxRawNets] = {};
    float gpu3D[kMaxRawGpus] = {};
    float gpuCopy[kMaxRawGpus] = {};
    float gpuVEnc[kMaxRawGpus] = {};
    float gpuVDec[kMaxRawGpus] = {};
    float gpuOverall[kMaxRawGpus] = {};
    float gpuDedMem[kMaxRawGpus] = {};
};

struct SharedPerfRing {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t writePos = 0; // Monotonically increasing write counter (slot index = (writePos - 1) % kRawHistoryLen)
    uint32_t reserved = 0;
    RawPerfSlot slots[kRawHistoryLen]{};
};
#pragma pack(pop)

struct PerfCpuInfo {
    std::wstring name;
    double utilization = 0.0;
    double kernelUtilization = 0.0;
    double baseSpeedGhz = 0.0;
    double currentSpeedGhz = 0.0;
    int sockets = 1;
    int cores = 1;
    int logicalProcessors = 1;
    bool virtualization = false;
    uint64_t l1CacheBytes = 0;
    uint64_t l2CacheBytes = 0;
    uint64_t l3CacheBytes = 0;
    DWORD processes = 0;
    DWORD threads = 0;
    DWORD handles = 0;
    uint64_t uptimeMs = 0;

    std::vector<double> history;       // 60 samples (0..100)
    std::vector<double> kernelHistory; // 60 samples (0..100)
    std::vector<std::vector<double>> coreHistories;
    std::vector<double> coreCurrent;
};

struct PerfMemoryInfo {
    uint64_t totalBytes = 0;
    uint64_t installedBytes = 0;
    uint64_t hardwareReservedBytes = 0;
    uint64_t inUseBytes = 0;
    uint64_t availableBytes = 0;
    uint64_t committedBytes = 0;
    uint64_t commitLimitBytes = 0;
    uint64_t cachedBytes = 0;
    uint64_t pagedPoolBytes = 0;
    uint64_t nonPagedPoolBytes = 0;
    uint64_t standbyBytes = 0;
    uint64_t modifiedBytes = 0;
    uint64_t freeBytes = 0;
    int speedMhz = 0;
    int slotsUsed = 0;
    int slotsTotal = 0;
    std::wstring formFactor;
    double utilization = 0.0;
    std::vector<double> history; // 60 samples (0..100)
};

struct PerfDiskInfo {
    int index = 0;
    std::wstring title;       // e.g. "Disk 0 (C: D:)"
    std::wstring model;       // e.g. "Samsung SSD 980 PRO 1TB"
    std::wstring letters;     // e.g. "C: D:"
    std::wstring type;        // "SSD" or "HDD"
    uint64_t capacityBytes = 0;
    uint64_t formattedBytes = 0;
    bool isSystemDisk = false;
    bool hasPageFile = false;

    double activeTimePct = 0.0;
    double avgResponseMs = 0.0;
    double readBytesPerSec = 0.0;
    double writeBytesPerSec = 0.0;

    std::wstring pdhInstanceName;
    std::vector<double> activeHistory; // 60 samples (0..100)
    std::vector<double> readHistory;   // 60 samples (MB/s)
    std::vector<double> writeHistory;  // 60 samples (MB/s)
};

struct PerfNetInfo {
    uint64_t luidValue = 0;
    std::wstring title;       // "Ethernet" / "Wi-Fi"
    std::wstring adapterName; // Friendly description
    std::wstring dnsName;
    std::wstring connType;    // "Ethernet" / "Wi-Fi"
    std::wstring ipv4;
    std::wstring ipv6;
    uint64_t linkSpeedBps = 0;
    bool connected = false;

    double sendBytesPerSec = 0.0;
    double recvBytesPerSec = 0.0;
    uint64_t prevOutOctets = 0;
    uint64_t prevInOctets = 0;
    bool hasPrevOctets = false;

    std::vector<double> sendHistory; // Mbps
    std::vector<double> recvHistory; // Mbps
};

struct PerfGpuInfo {
    int index = 0;
    LUID luid{};
    std::wstring title;       // "GPU 0"
    std::wstring name;
    std::wstring driverVersion;
    std::wstring driverDate;
    std::wstring directXVersion;
    std::wstring physLocation;

    double utilization = 0.0;
    double temperatureC = 0.0;
    bool hasTemperature = false;

    uint64_t dedicatedTotal = 0;
    uint64_t dedicatedUsed = 0;
    uint64_t sharedTotal = 0;
    uint64_t sharedUsed = 0;

    std::vector<double> hist3D;
    std::vector<double> histCopy;
    std::vector<double> histVideoEncode;
    std::vector<double> histVideoDecode;
    std::vector<double> histOverall;
    std::vector<double> histDedMem;
};

class PerfMonitor {
public:
    PerfMonitor();
    ~PerfMonitor();

    void Init();
    void Shutdown();
    void Sample(DWORD processCount, DWORD totalThreads, DWORD totalHandles);
    void SetUpdateSpeed(UpdateSpeed speed);
    void RebuildHistoriesForSpeed(UpdateSpeed speed);
    UpdateSpeed GetEffectiveSpeed() const { return activeSpeed_; }

    const PerfCpuInfo& Cpu() const { return cpu_; }
    const PerfMemoryInfo& Memory() const { return mem_; }
    const std::vector<PerfDiskInfo>& Disks() const { return disks_; }
    const std::vector<PerfNetInfo>& Networks() const { return nets_; }
    const std::vector<PerfGpuInfo>& Gpus() const { return gpus_; }

private:
    void InitCpuHardware();
    void InitMemoryHardware();
    void InitDisks();
    void InitNetworks();
    void InitGpus();
    void InitPdh();
    void InitSharedHistory();
    void LoadRingFromDisk(SharedPerfRing& ring);
    void SaveRingToDisk(const SharedPerfRing& ring);
    void SyncFromSharedRing(bool updateLiveReadout);
    void AppendSlotToSharedRing(const RawPerfSlot& slot);

    void SampleCpu(DWORD processCount, DWORD totalThreads, DWORD totalHandles);
    void SampleMemory();
    void SampleDisks();
    void SampleNetworks(double dtSec);
    void SampleGpus();

    PerfCpuInfo cpu_;
    PerfMemoryInfo mem_;
    std::vector<PerfDiskInfo> disks_;
    std::vector<PerfNetInfo> nets_;
    std::vector<PerfGpuInfo> gpus_;

    struct CoreTimePrev {
        LONGLONG idle = 0;
        LONGLONG kernel = 0;
        LONGLONG user = 0;
    };
    std::vector<CoreTimePrev> prevCoreTimes_;
    ULONGLONG prevSampleTickMs_ = 0;

    UpdateSpeed activeSpeed_ = UpdateSpeed::Normal;
    HANDLE hMapFile_ = nullptr;
    SharedPerfRing* sharedRing_ = nullptr;
    HANDLE hRingMutex_ = nullptr;
    std::vector<RawPerfSlot> cachedSlots_;
    uint32_t savePersistCounter_ = 0;

    void* pdhQuery_ = nullptr;
    void* pdhCpuPerf_ = nullptr;
    void* pdhDiskTime_ = nullptr;
    void* pdhDiskRead_ = nullptr;
    void* pdhDiskWrite_ = nullptr;
    void* pdhDiskAvgSec_ = nullptr;
    void* pdhGpuEngine_ = nullptr;
    void* pdhGpuDed_ = nullptr;
    void* pdhGpuShared_ = nullptr;

    std::thread pdhInitThread_;
    std::atomic<bool> pdhReady_{ false };
};

} // namespace taskman
