#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace taskman {

struct ServiceItem {
    std::wstring name;
    std::wstring displayName;
    std::wstring description;
    std::wstring group;
    DWORD pid = 0;
    DWORD currentState = SERVICE_STOPPED;
    DWORD startType = SERVICE_DEMAND_START;
};

class ServiceManager {
public:
    void Refresh();
    const std::vector<ServiceItem>& Items() const { return items_; }

    static bool StartServiceByName(const std::wstring& name, DWORD& outErr);
    static bool StopServiceByName(const std::wstring& name, DWORD& outErr);
    static bool RestartServiceByName(const std::wstring& name, DWORD& outErr);

private:
    struct StaticSvcMeta {
        DWORD startType = SERVICE_DEMAND_START;
        std::wstring group;
        std::wstring description;
    };

    std::vector<ServiceItem> items_;
    std::unordered_map<std::wstring, StaticSvcMeta> metaCache_;
};

} // namespace taskman
