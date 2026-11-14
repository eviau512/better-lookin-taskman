#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "process_monitor.h"

namespace taskman {

struct UserSessionInfo {
    DWORD sessionId = 0;
    std::wstring userName;
    std::wstring domain;
    std::wstring fullUser;
    bool active = true;
    HICON avatarIcon = nullptr;
    double cpuPercent = 0.0;
    uint64_t memoryBytes = 0;
    double diskBytesPerSec = 0.0;
    double netBytesPerSec = 0.0;
    std::vector<const ProcessInfo*> processes;
};

class UserManager {
public:
    UserManager() = default;
    ~UserManager();

    void Refresh(const std::vector<ProcessInfo>& procs, bool showFullAccountName);
    const std::vector<UserSessionInfo>& Sessions() const { return sessions_; }

    static bool DisconnectSession(DWORD sessionId);
    static bool LogoffSession(DWORD sessionId);

private:
    HICON GetOrLoadAvatar(const std::wstring& domain, const std::wstring& userName);

    std::vector<UserSessionInfo> sessions_;
    std::unordered_map<std::wstring, HICON> avatarCache_;
};

} // namespace taskman
