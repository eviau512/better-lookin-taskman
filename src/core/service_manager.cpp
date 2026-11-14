#include "service_manager.h"
#include <algorithm>
#include <cwctype>

namespace taskman {

static std::wstring ExtractSvchostGroup(const wchar_t* binPath) {
    if (!binPath) return L"";
    std::wstring s = binPath;
    std::wstring low = s;
    for (auto& ch : low) ch = static_cast<wchar_t>(std::towlower(ch));
    size_t pos = low.find(L"svchost.exe");
    if (pos == std::wstring::npos) return L"";
    size_t kPos = low.find(L"-k ", pos);
    if (kPos == std::wstring::npos) kPos = low.find(L"/k ", pos);
    if (kPos == std::wstring::npos) return L"";
    size_t start = kPos + 3;
    while (start < s.size() && iswspace(s[start])) start++;
    size_t end = start;
    while (end < s.size() && !iswspace(s[end])) end++;
    return s.substr(start, end - start);
}

void ServiceManager::Refresh() {
    items_.clear();
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!hScm) return;

    DWORD bytesNeeded = 0, count = 0, resume = 0;
    EnumServicesStatusExW(hScm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          nullptr, 0, &bytesNeeded, &count, &resume, nullptr);
    if (bytesNeeded == 0) {
        CloseServiceHandle(hScm);
        return;
    }

    std::vector<BYTE> buf(bytesNeeded);
    resume = 0;
    if (EnumServicesStatusExW(hScm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                              buf.data(), bytesNeeded, &bytesNeeded, &count, &resume, nullptr)) {
        const auto* arr = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
        items_.reserve(count);
        for (DWORD i = 0; i < count; ++i) {
            ServiceItem item;
            item.name = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
            item.displayName = arr[i].lpDisplayName ? arr[i].lpDisplayName : L"";
            item.pid = arr[i].ServiceStatusProcess.dwProcessId;
            item.currentState = arr[i].ServiceStatusProcess.dwCurrentState;

            auto mit = metaCache_.find(item.name);
            if (mit != metaCache_.end()) {
                item.startType = mit->second.startType;
                item.group = mit->second.group;
                item.description = mit->second.description;
            } else {
                StaticSvcMeta meta;
                SC_HANDLE hSvc = OpenServiceW(hScm, item.name.c_str(), SERVICE_QUERY_CONFIG);
                if (hSvc) {
                    BYTE cfgBuf[2048];
                    DWORD needed = 0;
                    if (QueryServiceConfigW(hSvc, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(cfgBuf),
                                            sizeof(cfgBuf), &needed)) {
                        auto* cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(cfgBuf);
                        meta.startType = cfg->dwStartType;
                        meta.group = ExtractSvchostGroup(cfg->lpBinaryPathName);
                        if (meta.group.empty() && cfg->lpLoadOrderGroup && cfg->lpLoadOrderGroup[0]) {
                            meta.group = cfg->lpLoadOrderGroup;
                        }
                    }
                    BYTE descBuf[2048];
                    if (QueryServiceConfig2W(hSvc, SERVICE_CONFIG_DESCRIPTION, descBuf, sizeof(descBuf), &needed)) {
                        auto* sd = reinterpret_cast<LPSERVICE_DESCRIPTIONW>(descBuf);
                        if (sd->lpDescription) meta.description = sd->lpDescription;
                    }
                    CloseServiceHandle(hSvc);
                }
                if (meta.description.empty()) meta.description = item.displayName;
                item.startType = meta.startType;
                item.group = meta.group;
                item.description = meta.description;
                metaCache_[item.name] = std::move(meta);
            }

            if (item.description.empty()) item.description = item.displayName;
            items_.push_back(std::move(item));
        }
    }

    CloseServiceHandle(hScm);
}

bool ServiceManager::StartServiceByName(const std::wstring& name, DWORD& outErr) {
    outErr = ERROR_SUCCESS;
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) { outErr = GetLastError(); return false; }
    SC_HANDLE hSvc = OpenServiceW(hScm, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) { outErr = GetLastError(); CloseServiceHandle(hScm); return false; }

    BOOL ok = StartServiceW(hSvc, 0, nullptr);
    if (!ok) outErr = GetLastError();
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
    return ok != FALSE;
}

bool ServiceManager::StopServiceByName(const std::wstring& name, DWORD& outErr) {
    outErr = ERROR_SUCCESS;
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) { outErr = GetLastError(); return false; }
    SC_HANDLE hSvc = OpenServiceW(hScm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hSvc) { outErr = GetLastError(); CloseServiceHandle(hScm); return false; }

    SERVICE_STATUS ss{};
    BOOL ok = ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);
    if (!ok) outErr = GetLastError();
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
    return ok != FALSE;
}

bool ServiceManager::RestartServiceByName(const std::wstring& name, DWORD& outErr) {
    outErr = ERROR_SUCCESS;
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) { outErr = GetLastError(); return false; }
    SC_HANDLE hSvc = OpenServiceW(hScm, name.c_str(), SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) { outErr = GetLastError(); CloseServiceHandle(hScm); return false; }

    SERVICE_STATUS ss{};
    if (ControlService(hSvc, SERVICE_CONTROL_STOP, &ss)) {
        for (int i = 0; i < 30; ++i) {
            if (!QueryServiceStatus(hSvc, &ss) || ss.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(150);
        }
    }
    BOOL ok = StartServiceW(hSvc, 0, nullptr);
    if (!ok) outErr = GetLastError();
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);
    return ok != FALSE;
}

} // namespace taskman
