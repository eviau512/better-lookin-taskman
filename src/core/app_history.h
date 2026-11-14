#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include "process_monitor.h"

namespace taskman {

struct AppHistoryEntry {
    std::wstring key;
    std::wstring name;
    std::wstring imagePath;
    HICON icon = nullptr;
    bool isStoreApp = false;
    uint64_t cpuTimeSeconds = 0;
    uint64_t networkBytes = 0;
    uint64_t meteredBytes = 0;
    uint64_t tileUpdatesBytes = 0;
};

class AppHistoryManager {
public:
    AppHistoryManager() = default;
    ~AppHistoryManager();

    void Init();
    void InitAsync(HWND notifyHwnd = nullptr, UINT notifyMsg = 0);
    void Shutdown();
    void Accumulate(const std::vector<ProcessInfo>& procs, double dtSec);
    void DeleteHistory();

    std::vector<AppHistoryEntry> GetEntries(bool showAllProcesses) const;
    std::wstring GetSinceDate() const;

private:
    struct AccumState {
        std::wstring name;
        std::wstring imagePath;
        HICON icon = nullptr;
        bool isStoreApp = false;
        double srumCpuSeconds = 0.0;
        double cpuSecondsAccum = 0.0;
        uint64_t srumNetBytes = 0;
        double networkBytesAccum = 0.0;
        uint64_t srumMeteredBytes = 0;
        uint64_t meteredBytes = 0;
        uint64_t srumTileBytes = 0;
        uint64_t tileUpdatesBytes = 0;
        std::unordered_map<DWORD, uint64_t> lastSeenCpu100ns;
    };

    void RunInitWork(bool initCom);
    void LoadSrumHistory();

    mutable std::mutex mutex_;
    std::unordered_map<std::wstring, AccumState> entries_;
    std::vector<HICON> ownedIcons_;
    std::wstring winAppsDirLower_;
    std::wstring sinceDateStr_;
    bool clearedManually_ = false;
    FILETIME clearCutoffFt_{};

    std::thread initThread_;
    std::atomic<bool> initStarted_{ false };
    std::atomic<bool> stopRequested_{ false };
};

} // namespace taskman
