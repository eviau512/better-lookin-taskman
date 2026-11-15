#include "lock_checker.h"
#include "format_util.h"
#include <winternl.h>
#include <psapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <restartmanager.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <unordered_map>

namespace taskman {

namespace {

constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr ULONG kObjectNameInformation = 1;
constexpr ULONG kObjectTypeInformation = 2;
constexpr ULONG kProcessImageFileName = 27;
constexpr ULONG kProcessBreakOnTermination = 29;
constexpr ULONG kProcessProtectionInformation = 61;

struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

struct SYSTEM_HANDLE_INFORMATION_EX_LOCAL {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL Handles[1];
};

struct OBJECT_TYPE_INFORMATION_LOCAL {
    UNICODE_STRING TypeName;
    ULONG TotalNumberOfObjects;
    ULONG TotalNumberOfHandles;
    ULONG TotalPagedPoolUsage;
    ULONG TotalNonPagedPoolUsage;
    ULONG TotalNamePoolUsage;
    ULONG TotalHandleTableUsage;
    ULONG HighWaterNumberOfObjects;
    ULONG HighWaterNumberOfHandles;
    ULONG HighWaterPagedPoolUsage;
    ULONG HighWaterNonPagedPoolUsage;
    ULONG HighWaterNamePoolUsage;
    ULONG HighWaterHandleTableUsage;
    ULONG InvalidAttributes;
    GENERIC_MAPPING GenericMapping;
    ULONG ValidAccessMask;
    BOOLEAN SecurityRequired;
    BOOLEAN MaintainHandleCount;
    UCHAR TypeIndex;
    CHAR ReservedByte;
    ULONG PoolType;
    ULONG DefaultPagedPoolCharge;
    ULONG DefaultNonPagedPoolCharge;
};

using PFN_NtQuerySystemInformation = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using PFN_NtQueryObject = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using PFN_NtQueryInformationProcess = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

using PFN_RmStartSession = DWORD(WINAPI*)(DWORD*, DWORD, WCHAR[]);
using PFN_RmRegisterResources = DWORD(WINAPI*)(DWORD, UINT, LPCWSTR[], UINT, void*, UINT, LPCWSTR[]);
using PFN_RmGetList = DWORD(WINAPI*)(DWORD, UINT*, UINT*, RM_PROCESS_INFO[], LPDWORD);
using PFN_RmEndSession = DWORD(WINAPI*)(DWORD);

struct NtApis {
    PFN_NtQuerySystemInformation querySystemInfo = nullptr;
    PFN_NtQueryObject queryObject = nullptr;
    PFN_NtQueryInformationProcess queryInfoProc = nullptr;
    bool ok = false;
};

const NtApis& GetNtApis() {
    static NtApis apis = []() {
        NtApis a;
        if (HMODULE hNt = GetModuleHandleW(L"ntdll.dll")) {
            a.querySystemInfo = reinterpret_cast<PFN_NtQuerySystemInformation>(
                GetProcAddress(hNt, "NtQuerySystemInformation"));
            a.queryObject = reinterpret_cast<PFN_NtQueryObject>(
                GetProcAddress(hNt, "NtQueryObject"));
            a.queryInfoProc = reinterpret_cast<PFN_NtQueryInformationProcess>(
                GetProcAddress(hNt, "NtQueryInformationProcess"));
            a.ok = (a.querySystemInfo && a.queryObject && a.queryInfoProc);
        }
        return a;
    }();
    return apis;
}

std::wstring ToLowerCopy(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

std::wstring BaseNameLower(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    std::wstring base = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
    return ToLowerCopy(base);
}

struct DriveDeviceMapEntry {
    std::wstring devicePrefix;      // e.g. \Device\HarddiskVolume3
    std::wstring devicePrefixLower; // lowercased
    std::wstring driveLetter;       // e.g. C:
};

std::vector<DriveDeviceMapEntry> BuildDriveDeviceMap() {
    std::vector<DriveDeviceMapEntry> out;
    wchar_t drives[512] = {};
    DWORD len = GetLogicalDriveStringsW(ARRAYSIZE(drives) - 1, drives);
    if (len == 0) return out;
    for (const wchar_t* p = drives; *p; p += wcslen(p) + 1) {
        if (wcslen(p) >= 2 && p[1] == L':') {
            wchar_t dosDrive[3] = { p[0], L':', L'\0' };
            wchar_t devName[1024] = {};
            if (QueryDosDeviceW(dosDrive, devName, ARRAYSIZE(devName)) > 0 && devName[0] != L'\0') {
                DriveDeviceMapEntry e;
                e.devicePrefix = devName;
                while (!e.devicePrefix.empty() && e.devicePrefix.back() == L'\\') {
                    e.devicePrefix.pop_back();
                }
                e.devicePrefixLower = ToLowerCopy(e.devicePrefix);
                e.driveLetter = dosDrive;
                out.push_back(std::move(e));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const DriveDeviceMapEntry& a, const DriveDeviceMapEntry& b) {
        return a.devicePrefixLower.size() > b.devicePrefixLower.size();
    });
    return out;
}

std::wstring NtDevicePathToDosPath(const std::wstring& ntPath, const std::vector<DriveDeviceMapEntry>& map) {
    if (ntPath.empty()) return L"";
    std::wstring low = ToLowerCopy(ntPath);
    for (const auto& e : map) {
        if (low.size() >= e.devicePrefixLower.size() &&
            low.compare(0, e.devicePrefixLower.size(), e.devicePrefixLower) == 0) {
            if (low.size() == e.devicePrefixLower.size()) {
                return e.driveLetter + L"\\";
            }
            if (low[e.devicePrefixLower.size()] == L'\\') {
                return e.driveLetter + ntPath.substr(e.devicePrefixLower.size());
            }
        }
    }
    if (low.compare(0, 12, L"\\device\\mup\\") == 0) {
        return L"\\\\" + ntPath.substr(12);
    }
    return ntPath;
}

bool DoesDosPathMatchTarget(const std::wstring& candidateDos, const std::wstring& targetLower, LockTargetType targetType) {
    if (candidateDos.empty() || targetLower.empty()) return false;
    std::wstring cand = ToLowerCopy(candidateDos);
    while (cand.size() > 3 && cand.back() == L'\\') cand.pop_back();

    if (targetType == LockTargetType::Drive) {
        // targetLower is e.g. "d:\" (3 chars)
        if (cand == targetLower) return true;
        if (cand.size() >= 2 && targetLower.size() >= 2 && cand[0] == targetLower[0] && cand[1] == L':') {
            return cand.size() == 2 || cand[2] == L'\\';
        }
        return false;
    }
    if (targetType == LockTargetType::Directory) {
        if (cand == targetLower) return true;
        if (cand.size() > targetLower.size() &&
            cand.compare(0, targetLower.size(), targetLower) == 0 &&
            cand[targetLower.size()] == L'\\') {
            return true;
        }
        return false;
    }
    return cand == targetLower;
}

// Persistent timeout-guarded helper thread so NtQueryObject(ObjectNameInformation) can never hang
// the scan on anomalous filesystem filter drivers or stale network handles.
struct NameQueryWorker {
    struct SharedState {
        HANDLE hReq = nullptr;
        HANDLE hDone = nullptr;
        PFN_NtQueryObject queryObject = nullptr;
        HANDLE targetHandle = nullptr;
        std::wstring resultName;
        bool stop = false;

        ~SharedState() {
            if (hReq) CloseHandle(hReq);
            if (hDone) CloseHandle(hDone);
        }
    };

    std::shared_ptr<SharedState> state;
    HANDLE hThread = nullptr;

    explicit NameQueryWorker(PFN_NtQueryObject pfn) {
        Init(pfn);
    }

    ~NameQueryWorker() {
        Shutdown();
    }

    void Init(PFN_NtQueryObject pfn) {
        Shutdown();
        state = std::make_shared<SharedState>();
        state->queryObject = pfn;
        state->hReq = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        state->hDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        auto* ctx = new std::shared_ptr<SharedState>(state);
        hThread = CreateThread(nullptr, 0, WorkerProc, ctx, 0, nullptr);
        if (!hThread) {
            delete ctx;
        }
    }

    void Shutdown() {
        if (state && hThread) {
            state->stop = true;
            SetEvent(state->hReq);
            if (WaitForSingleObject(hThread, 50) != WAIT_OBJECT_0) {
                CancelSynchronousIo(hThread);
                WaitForSingleObject(hThread, 20);
            }
            CloseHandle(hThread);
            hThread = nullptr;
        }
        state.reset();
    }

    bool QueryHandleName(HANDLE hDup, std::wstring& outName, DWORD timeoutMs = 20) {
        outName.clear();
        if (!state || !hThread || !state->queryObject) return false;
        state->targetHandle = hDup;
        state->resultName.clear();
        ResetEvent(state->hDone);
        SetEvent(state->hReq);
        DWORD wr = WaitForSingleObject(state->hDone, timeoutMs);
        if (wr == WAIT_OBJECT_0) {
            outName = state->resultName;
            return !outName.empty();
        }
        // Timed out: cancel synchronous I/O; if still blocked, spin a fresh worker cleanly.
        CancelSynchronousIo(hThread);
        if (WaitForSingleObject(state->hDone, 10) != WAIT_OBJECT_0) {
            PFN_NtQueryObject pfn = state->queryObject;
            CloseHandle(hThread);
            hThread = nullptr;
            state.reset();
            Init(pfn);
        }
        return false;
    }

    static DWORD WINAPI WorkerProc(LPVOID param) {
        auto* holder = static_cast<std::shared_ptr<SharedState>*>(param);
        std::shared_ptr<SharedState> st = *holder;
        delete holder;

        std::vector<BYTE> buf(2048);
        while (WaitForSingleObject(st->hReq, INFINITE) == WAIT_OBJECT_0) {
            if (st->stop) break;
            st->resultName.clear();
            ULONG retLen = 0;
            NTSTATUS status = st->queryObject(
                st->targetHandle, kObjectNameInformation,
                buf.data(), static_cast<ULONG>(buf.size()), &retLen);
            if (status == static_cast<NTSTATUS>(0xC0000004L) && retLen > buf.size() && retLen <= 65536) {
                buf.resize(retLen + 64);
                status = st->queryObject(
                    st->targetHandle, kObjectNameInformation,
                    buf.data(), static_cast<ULONG>(buf.size()), &retLen);
            }
            if (status >= 0) {
                auto* us = reinterpret_cast<const UNICODE_STRING*>(buf.data());
                if (us->Buffer && us->Length >= sizeof(wchar_t)) {
                    st->resultName.assign(us->Buffer, us->Length / sizeof(wchar_t));
                }
            }
            SetEvent(st->hDone);
        }
        return 0;
    }
};

bool IsKnownCriticalCoreImage(const std::wstring& lowerBaseName) {
    static const wchar_t* kCriticalImages[] = {
        L"system", L"registry", L"memcompression", L"memory compression", L"secure system",
        L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
        L"services.exe", L"lsass.exe", L"lsaiso.exe", L"dwm.exe", L"fontdrvhost.exe"
    };
    for (const wchar_t* item : kCriticalImages) {
        if (lowerBaseName == item) return true;
    }
    return false;
}

bool IsKnownSystemServiceImage(const std::wstring& lowerBaseName, const std::wstring& /*lowerFullPath*/) {
    static const wchar_t* kSystemServiceImages[] = {
        L"svchost.exe", L"sihost.exe", L"conhost.exe", L"dllhost.exe",
        L"wmiprvse.exe", L"spoolsv.exe", L"searchindexer.exe", L"searchprotocolhost.exe",
        L"searchfilterhost.exe", L"runtimebroker.exe", L"ctfmon.exe", L"audiodg.exe",
        L"taskhostw.exe", L"dashost.exe", L"smartscreen.exe", L"backgroundtaskhost.exe",
        L"securityhealthservice.exe", L"msdtc.exe", L"vds.exe", L"trustedinstaller.exe",
        L"tiworker.exe", L"sppsvc.exe", L"wbengine.exe", L"applicationframehost.exe",
        L"startmenuexperiencehost.exe", L"shellexperiencehost.exe"
    };
    for (const wchar_t* item : kSystemServiceImages) {
        if (lowerBaseName == item) return true;
    }
    return false;
}

struct PidServiceInfo {
    bool hasCriticalService = false;
    bool hasAnyService = false;
    std::wstring summaryLabel;
};

bool IsFatalCoreServiceName(const wchar_t* svcName) {
    if (!svcName) return false;
    static const wchar_t* kCriticalServices[] = {
        L"DcomLaunch", L"RpcSs", L"RpcEptMapper", L"LSM", L"Power", L"PlugPlay",
        L"BrokerInfrastructure", L"SystemEventsBroker", L"CoreMessagingRegistrar",
        L"ProfSvc", L"SamSs", L"VaultSvc", L"KeyIso", L"WinDefend", L"Sense"
    };
    for (const wchar_t* item : kCriticalServices) {
        if (_wcsicmp(svcName, item) == 0) return true;
    }
    return false;
}

std::unordered_map<DWORD, PidServiceInfo> BuildPidServiceMap() {
    std::unordered_map<DWORD, PidServiceInfo> out;
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!hScm) return out;

    DWORD bytesNeeded = 0;
    DWORD count = 0;
    DWORD resume = 0;
    EnumServicesStatusExW(hScm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
                          nullptr, 0, &bytesNeeded, &count, &resume, nullptr);
    if (GetLastError() == ERROR_MORE_DATA && bytesNeeded > 0) {
        std::vector<BYTE> buf(bytesNeeded + 4096);
        resume = 0;
        if (EnumServicesStatusExW(hScm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
                                  buf.data(), static_cast<DWORD>(buf.size()),
                                  &bytesNeeded, &count, &resume, nullptr)) {
            const auto* svcs = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
            for (DWORD i = 0; i < count; ++i) {
                DWORD pid = svcs[i].ServiceStatusProcess.dwProcessId;
                if (pid == 0) continue;
                auto& info = out[pid];
                info.hasAnyService = true;
                if (svcs[i].lpServiceName && svcs[i].lpServiceName[0] != L'\0') {
                    if (IsFatalCoreServiceName(svcs[i].lpServiceName)) {
                        info.hasCriticalService = true;
                    }
                    if (info.summaryLabel.size() < 48) {
                        if (!info.summaryLabel.empty()) info.summaryLabel += L", ";
                        info.summaryLabel += svcs[i].lpServiceName;
                    }
                }
            }
        }
    }
    CloseServiceHandle(hScm);
    return out;
}

void QueryProcessMetaAndCriticality(DWORD pid,
                                    const NtApis& nt,
                                    const std::unordered_map<DWORD, std::wstring>& toolhelpNames,
                                    const std::unordered_map<DWORD, PidServiceInfo>& svcMap,
                                    const std::vector<DriveDeviceMapEntry>& driveMap,
                                    LockEntry& entry,
                                    bool& outOpenDenied) {
    entry.pid = pid;
    if (pid == 0) {
        entry.processName = L"System Idle Process";
        entry.criticalLevel = ProcessCriticalLevel::CriticalCore;
        entry.canTerminate = false;
        return;
    }
    if (pid == 4) {
        entry.processName = L"System";
        entry.criticalLevel = ProcessCriticalLevel::CriticalCore;
        entry.canTerminate = false;
        return;
    }

    auto itTh = toolhelpNames.find(pid);
    if (itTh != toolhelpNames.end()) {
        entry.processName = itTh->second;
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        outOpenDenied = true;
    } else {
        wchar_t pathBuf[1024] = {};
        DWORD pathLen = ARRAYSIZE(pathBuf);
        if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen) && pathLen > 0) {
            entry.imagePath.assign(pathBuf, pathLen);
            if (entry.processName.empty()) {
                size_t slash = entry.imagePath.find_last_of(L"\\/");
                entry.processName = (slash == std::wstring::npos) ? entry.imagePath : entry.imagePath.substr(slash + 1);
            }
        } else if (nt.queryInfoProc) {
            std::vector<BYTE> nameBuf(1024);
            ULONG ret = 0;
            if (nt.queryInfoProc(hProc, kProcessImageFileName, nameBuf.data(),
                                 static_cast<ULONG>(nameBuf.size()), &ret) >= 0) {
                auto* us = reinterpret_cast<const UNICODE_STRING*>(nameBuf.data());
                if (us->Buffer && us->Length > 0) {
                    std::wstring ntImg(us->Buffer, us->Length / sizeof(wchar_t));
                    entry.imagePath = NtDevicePathToDosPath(ntImg, driveMap);
                    if (entry.processName.empty()) {
                        size_t slash = entry.imagePath.find_last_of(L"\\/");
                        entry.processName = (slash == std::wstring::npos) ? entry.imagePath : entry.imagePath.substr(slash + 1);
                    }
                }
            }
        }

        if (nt.queryInfoProc) {
            ULONG breakOnTerm = 0;
            ULONG ret = 0;
            if (nt.queryInfoProc(hProc, kProcessBreakOnTermination, &breakOnTerm, sizeof(breakOnTerm), &ret) >= 0 &&
                breakOnTerm != 0) {
                entry.isBreakOnTermination = true;
            }
            // PS_PROTECTION struct starts with UCHAR Level (Type:3 | Audit:1 | Signer:4)
            BYTE protLevel = 0;
            if (nt.queryInfoProc(hProc, kProcessProtectionInformation, &protLevel, sizeof(protLevel), &ret) >= 0 &&
                protLevel != 0) {
                entry.isProtectedProcess = true;
            }
        }
        CloseHandle(hProc);
    }

    if (entry.processName.empty()) {
        entry.processName = L"PID " + std::to_wstring(pid);
    }

    std::wstring lowBase = ToLowerCopy(entry.processName);
    std::wstring lowImg = ToLowerCopy(entry.imagePath);

    bool hasCriticalSvc = false;
    bool hasAnySvc = false;
    auto itSvc = svcMap.find(pid);
    if (itSvc != svcMap.end()) {
        hasCriticalSvc = itSvc->second.hasCriticalService;
        hasAnySvc = itSvc->second.hasAnyService;
        if (!itSvc->second.summaryLabel.empty() && lowBase == L"svchost.exe") {
            entry.processName += L" (" + itSvc->second.summaryLabel + L")";
        }
    }

    if (pid == GetCurrentProcessId() ||
        entry.isBreakOnTermination ||
        entry.isProtectedProcess ||
        hasCriticalSvc ||
        IsKnownCriticalCoreImage(lowBase)) {
        entry.criticalLevel = ProcessCriticalLevel::CriticalCore;
        entry.canTerminate = false;
    } else if (hasAnySvc || IsKnownSystemServiceImage(lowBase, lowImg)) {
        entry.criticalLevel = ProcessCriticalLevel::SystemService;
        entry.canTerminate = true;
    } else {
        entry.criticalLevel = ProcessCriticalLevel::Normal;
        entry.canTerminate = true;
    }
}

std::unordered_map<DWORD, std::wstring> BuildToolhelpProcessNameMap() {
    std::unordered_map<DWORD, std::wstring> names;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return names;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            names[pe.th32ProcessID] = pe.szExeFile;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return names;
}

bool CheckTokenAccessMask(PSECURITY_DESCRIPTOR pSD, HANDLE hImpToken, DWORD desiredAccess, bool& outGranted) {
    outGranted = false;
    if (!pSD || !hImpToken) return false;
    GENERIC_MAPPING gm{ FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    DWORD access = desiredAccess;
    MapGenericMask(&access, &gm);
    PRIVILEGE_SET privSet{};
    DWORD privLen = sizeof(privSet);
    DWORD grantedAccess = 0;
    BOOL status = FALSE;
    if (!AccessCheck(pSD, hImpToken, access, &gm, &privSet, &privLen, &grantedAccess, &status)) {
        return false;
    }
    outGranted = (status != FALSE);
    return true;
}

std::wstring SidToAccountString(PSID sid) {
    if (!sid || !IsValidSid(sid)) return L"";
    wchar_t name[256] = {};
    wchar_t dom[256] = {};
    DWORD cchName = ARRAYSIZE(name);
    DWORD cchDom = ARRAYSIZE(dom);
    SID_NAME_USE snu = SidTypeUnknown;
    if (LookupAccountSidW(nullptr, sid, name, &cchName, dom, &cchDom, &snu)) {
        if (dom[0] != L'\0') {
            return std::wstring(dom) + L"\\" + name;
        }
        return std::wstring(name);
    }
    LPWSTR sidStr = nullptr;
    if (ConvertSidToStringSidW(sid, &sidStr) && sidStr) {
        std::wstring res(sidStr);
        LocalFree(sidStr);
        return res;
    }
    return L"";
}

} // namespace

std::vector<std::wstring> LockChecker::EnumerateDriveRoots() {
    std::vector<std::wstring> out;
    wchar_t buf[512] = {};
    DWORD len = GetLogicalDriveStringsW(ARRAYSIZE(buf) - 1, buf);
    if (len == 0) return out;
    for (const wchar_t* p = buf; *p; p += wcslen(p) + 1) {
        UINT dt = GetDriveTypeW(p);
        if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE || dt == DRIVE_REMOTE || dt == DRIVE_RAMDISK) {
            out.emplace_back(p);
        }
    }
    return out;
}

std::wstring LockChecker::NormalizeTargetPath(const std::wstring& input, LockTargetType& outType, bool& outExists) {
    outType = LockTargetType::Unknown;
    outExists = false;

    std::wstring s = input;
    while (!s.empty() && (iswspace(s.front()) || s.front() == L'"')) s.erase(s.begin());
    while (!s.empty() && (iswspace(s.back()) || s.back() == L'"')) s.pop_back();
    if (s.empty()) return L"";

    for (wchar_t& c : s) {
        if (c == L'/') c = L'\\';
    }
    if (s.compare(0, 4, L"\\\\?\\") == 0) {
        s.erase(0, 4);
    }

    // Support shorthand drive letters like "D" or "D:"
    if (s.size() == 1 && iswalpha(s[0])) {
        s.push_back(L':');
        s.push_back(L'\\');
    } else if (s.size() == 2 && iswalpha(s[0]) && s[1] == L':') {
        s.push_back(L'\\');
    }

    wchar_t fullBuf[MAX_PATH * 4] = {};
    DWORD fullLen = GetFullPathNameW(s.c_str(), ARRAYSIZE(fullBuf), fullBuf, nullptr);
    if (fullLen > 0 && fullLen < ARRAYSIZE(fullBuf)) {
        s.assign(fullBuf, fullLen);
    }

    if (s.size() == 3 && iswalpha(s[0]) && s[1] == L':' && s[2] == L'\\') {
        s[0] = static_cast<wchar_t>(towupper(s[0]));
        outType = LockTargetType::Drive;
        UINT dt = GetDriveTypeW(s.c_str());
        outExists = (dt != DRIVE_UNKNOWN && dt != DRIVE_NO_ROOT_DIR);
        return s;
    }

    while (s.size() > 3 && s.back() == L'\\') s.pop_back();

    DWORD attrs = GetFileAttributesW(s.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        outExists = true;
        outType = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? LockTargetType::Directory : LockTargetType::File;
    } else {
        outExists = false;
        outType = LockTargetType::File;
    }
    return s;
}

LockCheckPermissionInfo LockChecker::InspectPermissions(const std::wstring& inputPath) {
    LockCheckPermissionInfo info;
    info.rawInput = inputPath;
    info.isCurrentElevated = IsCurrentProcessElevated();
    info.normalizedPath = NormalizeTargetPath(inputPath, info.targetType, info.exists);
    if (info.normalizedPath.empty()) return info;

    if (info.targetType == LockTargetType::Drive) {
        info.attributes = GetFileAttributesW(info.normalizedPath.c_str());
    } else {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(info.normalizedPath.c_str(), GetFileExInfoStandard, &fad)) {
            info.exists = true;
            info.attributes = fad.dwFileAttributes;
            if (!(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                info.sizeBytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            }
        }
    }

    if (info.attributes != INVALID_FILE_ATTRIBUTES) {
        info.isReadOnly = (info.attributes & FILE_ATTRIBUTE_READONLY) != 0;
        info.isSystemAttr = (info.attributes & FILE_ATTRIBUTE_SYSTEM) != 0;
    }

    if (!info.exists) return info;

    PSID pOwnerSid = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    DWORD secErr = GetNamedSecurityInfoW(
        info.normalizedPath.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &pOwnerSid, nullptr, nullptr, nullptr, &pSD);

    if (secErr == ERROR_SUCCESS && pSD) {
        info.ownerName = SidToAccountString(pOwnerSid);
        std::wstring lowOwner = ToLowerCopy(info.ownerName);
        info.ownerIsTrustedInstaller = (lowOwner.find(L"trustedinstaller") != std::wstring::npos);
        info.ownerIsSystem = (lowOwner == L"nt authority\\system" || lowOwner == L"system");

        if (ImpersonateSelf(SecurityImpersonation)) {
            HANDLE hImpToken = nullptr;
            if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hImpToken)) {
                info.aclChecked = true;
                CheckTokenAccessMask(pSD, hImpToken, FILE_GENERIC_READ, info.hasReadAccess);
                CheckTokenAccessMask(pSD, hImpToken, FILE_GENERIC_WRITE, info.hasWriteAccess);
                CheckTokenAccessMask(pSD, hImpToken, DELETE, info.hasDeleteAccess);

                // In NTFS, FILE_DELETE_CHILD on parent directory also permits file/subfolder deletion
                if (info.targetType == LockTargetType::File || info.targetType == LockTargetType::Directory) {
                    size_t slash = info.normalizedPath.find_last_of(L'\\');
                    if (slash != std::wstring::npos && slash >= 2) {
                        std::wstring parentDir = (slash == 2) ? info.normalizedPath.substr(0, 3)
                                                              : info.normalizedPath.substr(0, slash);
                        PSECURITY_DESCRIPTOR pParentSD = nullptr;
                        if (GetNamedSecurityInfoW(
                                parentDir.c_str(), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, nullptr, nullptr, &pParentSD) == ERROR_SUCCESS && pParentSD) {
                            CheckTokenAccessMask(pParentSD, hImpToken, FILE_DELETE_CHILD, info.hasParentDeleteChild);
                            LocalFree(pParentSD);
                        }
                    }
                }
                CloseHandle(hImpToken);
            }
            RevertToSelf();
        }
        LocalFree(pSD);
    } else {
        info.aclError = secErr;
    }

    return info;
}

LockCheckResult LockChecker::Analyze(const std::wstring& inputPath, const IconResolver& iconResolver) {
    ULONGLONG startTick = GetTickCount64();
    LockCheckResult result;
    result.target = InspectPermissions(inputPath);
    if (!result.target.exists || result.target.normalizedPath.empty()) {
        result.elapsedMs = GetTickCount64() - startTick;
        return result;
    }

    const std::wstring targetLower = ToLowerCopy(result.target.normalizedPath);
    const LockTargetType targetType = result.target.targetType;
    const auto driveMap = BuildDriveDeviceMap();
    const auto toolhelpNames = BuildToolhelpProcessNameMap();
    const auto svcMap = BuildPidServiceMap();
    const NtApis& nt = GetNtApis();

    std::unordered_map<DWORD, LockEntry> byPid;
    auto recordMatch = [&](DWORD pid, uint32_t srcFlag, const std::wstring& matchedDosPath, bool countHandle) {
        auto it = byPid.find(pid);
        if (it == byPid.end()) {
            LockEntry e;
            e.pid = pid;
            e.sourceMask = srcFlag;
            e.matchedPath = matchedDosPath;
            e.handleCount = countHandle ? 1 : 0;
            byPid.emplace(pid, std::move(e));
        } else {
            it->second.sourceMask |= srcFlag;
            if (countHandle) it->second.handleCount++;
            if (it->second.matchedPath.empty() && !matchedDosPath.empty()) {
                it->second.matchedPath = matchedDosPath;
            }
        }
    };

    // 1) System handle table scan via NtQuerySystemInformation(SystemExtendedHandleInformation)
    if (nt.ok) {
        std::vector<BYTE> handleBuf(2 * 1024 * 1024);
        NTSTATUS st = 0;
        for (int attempt = 0; attempt < 6; ++attempt) {
            ULONG needed = 0;
            st = nt.querySystemInfo(kSystemExtendedHandleInformation, handleBuf.data(),
                                    static_cast<ULONG>(handleBuf.size()), &needed);
            if (st == static_cast<NTSTATUS>(0xC0000004L)) {
                ULONG nextSize = (std::max)(needed + 512 * 1024UL, static_cast<ULONG>(handleBuf.size() * 2));
                if (nextSize > 128 * 1024 * 1024UL) break;
                handleBuf.resize(nextSize);
            } else {
                break;
            }
        }

        if (st >= 0) {
            const auto* info = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX_LOCAL*>(handleBuf.data());
            std::unordered_map<DWORD, std::vector<const SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL*>> perProc;
            perProc.reserve(256);
            for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
                const auto& h = info->Handles[i];
                DWORD pid = static_cast<DWORD>(h.UniqueProcessId);
                if (pid == 0 || pid == GetCurrentProcessId()) continue;
                perProc[pid].push_back(&h);
            }

            USHORT fileTypeIndex = 0xFFFF;
            bool fileTypeResolved = false;
            std::unordered_map<USHORT, std::wstring> typeNameCache;
            NameQueryWorker nameWorker(nt.queryObject);
            std::vector<BYTE> typeBuf(1024);

            for (const auto& kv : perProc) {
                DWORD pid = kv.first;
                HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
                if (!hProc) continue;

                for (const auto* h : kv.second) {
                    if (fileTypeResolved && h->ObjectTypeIndex != fileTypeIndex) {
                        continue;
                    }

                    HANDLE hDup = nullptr;
                    if (!DuplicateHandle(hProc, reinterpret_cast<HANDLE>(h->HandleValue),
                                         GetCurrentProcess(), &hDup, 0, FALSE, 0)) {
                        continue;
                    }

                    if (!fileTypeResolved) {
                        auto itType = typeNameCache.find(h->ObjectTypeIndex);
                        std::wstring typeName;
                        if (itType != typeNameCache.end()) {
                            typeName = itType->second;
                        } else {
                            ULONG retLen = 0;
                            if (nt.queryObject(hDup, kObjectTypeInformation, typeBuf.data(),
                                               static_cast<ULONG>(typeBuf.size()), &retLen) >= 0) {
                                const auto* oti = reinterpret_cast<const OBJECT_TYPE_INFORMATION_LOCAL*>(typeBuf.data());
                                if (oti->TypeName.Buffer && oti->TypeName.Length >= sizeof(wchar_t)) {
                                    typeName.assign(oti->TypeName.Buffer, oti->TypeName.Length / sizeof(wchar_t));
                                }
                            }
                            typeNameCache[h->ObjectTypeIndex] = typeName;
                        }
                        if (typeName == L"File") {
                            fileTypeIndex = h->ObjectTypeIndex;
                            fileTypeResolved = true;
                        } else {
                            CloseHandle(hDup);
                            continue;
                        }
                    }

                    DWORD fType = GetFileType(hDup);
                    if (fType != FILE_TYPE_DISK) {
                        CloseHandle(hDup);
                        continue;
                    }

                    std::wstring ntObjName;
                    if (nameWorker.QueryHandleName(hDup, ntObjName, 20)) {
                        std::wstring dosPath = NtDevicePathToDosPath(ntObjName, driveMap);
                        if (DoesDosPathMatchTarget(dosPath, targetLower, targetType)) {
                            recordMatch(pid, LOCK_SRC_HANDLE, dosPath, true);
                        }
                    }
                    CloseHandle(hDup);
                }
                CloseHandle(hProc);
            }
        }
    }

    // 2) Mapped DLLs / EXEs / Memory-mapped files via VirtualQueryEx + GetMappedFileNameW
    {
        wchar_t selfExeBuf[1024] = {};
        DWORD selfLen = GetModuleFileNameW(nullptr, selfExeBuf, ARRAYSIZE(selfExeBuf));
        if (selfLen > 0 && selfLen < ARRAYSIZE(selfExeBuf)) {
            std::wstring selfExe(selfExeBuf, selfLen);
            if (DoesDosPathMatchTarget(selfExe, targetLower, targetType)) {
                recordMatch(GetCurrentProcessId(), LOCK_SRC_MAPPED_MODULE, selfExe, false);
            }
        }
        wchar_t mappedNtBuf[1024] = {};
        for (const auto& kv : toolhelpNames) {
            DWORD pid = kv.first;
            if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) continue;

            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!hProc) {
                hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            }
            if (!hProc) continue;

            LPCVOID addr = nullptr;
            PVOID lastAllocBase = reinterpret_cast<PVOID>(static_cast<UINT_PTR>(-1));
            MEMORY_BASIC_INFORMATION mbi{};
            while (VirtualQueryEx(hProc, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                if ((mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) &&
                    mbi.AllocationBase != lastAllocBase) {
                    lastAllocBase = mbi.AllocationBase;
                    DWORD n = GetMappedFileNameW(hProc, mbi.AllocationBase, mappedNtBuf, ARRAYSIZE(mappedNtBuf));
                    if (n > 0 && n < ARRAYSIZE(mappedNtBuf)) {
                        std::wstring dosPath = NtDevicePathToDosPath(std::wstring(mappedNtBuf, n), driveMap);
                        if (DoesDosPathMatchTarget(dosPath, targetLower, targetType)) {
                            recordMatch(pid, LOCK_SRC_MAPPED_MODULE, dosPath, false);
                            break;
                        }
                    }
                }
                UINT_PTR nextAddr = reinterpret_cast<UINT_PTR>(mbi.BaseAddress) + mbi.RegionSize;
                if (nextAddr <= reinterpret_cast<UINT_PTR>(addr)) break;
                addr = reinterpret_cast<LPCVOID>(nextAddr);
            }
            CloseHandle(hProc);
        }
    }

    // 3) Restart Manager API supplement for single files (catches non-elevated user-session locks cleanly)
    if (targetType == LockTargetType::File) {
        if (HMODULE hRm = LoadLibraryW(L"rstrtmgr.dll")) {
            auto pStart = reinterpret_cast<PFN_RmStartSession>(GetProcAddress(hRm, "RmStartSession"));
            auto pReg = reinterpret_cast<PFN_RmRegisterResources>(GetProcAddress(hRm, "RmRegisterResources"));
            auto pList = reinterpret_cast<PFN_RmGetList>(GetProcAddress(hRm, "RmGetList"));
            auto pEnd = reinterpret_cast<PFN_RmEndSession>(GetProcAddress(hRm, "RmEndSession"));
            if (pStart && pReg && pList && pEnd) {
                DWORD sess = 0;
                WCHAR sessKey[CCH_RM_SESSION_KEY + 1] = {};
                if (pStart(&sess, 0, sessKey) == ERROR_SUCCESS) {
                    LPCWSTR files[1] = { result.target.normalizedPath.c_str() };
                    if (pReg(sess, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
                        UINT procNeeded = 0;
                        UINT procCount = 16;
                        DWORD rebootReason = 0;
                        std::vector<RM_PROCESS_INFO> rmList(procCount);
                        DWORD r = pList(sess, &procNeeded, &procCount, rmList.data(), &rebootReason);
                        if (r == ERROR_MORE_DATA && procNeeded > 0) {
                            procCount = procNeeded;
                            rmList.resize(procCount);
                            r = pList(sess, &procNeeded, &procCount, rmList.data(), &rebootReason);
                        }
                        if (r == ERROR_SUCCESS) {
                            for (UINT i = 0; i < procCount; ++i) {
                                DWORD pid = rmList[i].Process.dwProcessId;
                                if (pid == 0 || pid == GetCurrentProcessId()) continue;
                                recordMatch(pid, LOCK_SRC_RESTART_MANAGER, result.target.normalizedPath, false);
                            }
                        }
                    }
                    pEnd(sess);
                }
            }
            FreeLibrary(hRm);
        }
    }

    // 4) Non-elevated protected OS core awareness:
    //    Core processes (smss/csrss/wininit/services/lsass/winlogon/dwm/fontdrvhost/System) reject OpenProcess
    //    for standard users. When checking core OS executables, system folders/drive, or universal core DLLs
    //    under %SystemRoot%, ensure those unopenable running core components are not omitted as "0 critical".
    {
        wchar_t winDirBuf[MAX_PATH] = {};
        UINT winLen = GetWindowsDirectoryW(winDirBuf, ARRAYSIZE(winDirBuf));
        if (winLen > 0 && winLen < ARRAYSIZE(winDirBuf)) {
            std::wstring sys32(winDirBuf, winLen);
            while (!sys32.empty() && sys32.back() == L'\\') sys32.pop_back();
            sys32 += L"\\System32";
            std::wstring sys32Lower = ToLowerCopy(sys32);

            bool targetIsUniversalNtdll = (targetType == LockTargetType::File && targetLower == sys32Lower + L"\\ntdll.dll");
            bool targetIsUniversalWin32CoreDll = false;
            if (targetType == LockTargetType::File) {
                static const wchar_t* kCoreWin32Dlls[] = {
                    L"\\kernel32.dll", L"\\kernelbase.dll", L"\\sechost.dll",
                    L"\\rpcrt4.dll", L"\\advapi32.dll", L"\\ucrtbase.dll",
                    L"\\msvcrt.dll", L"\\bcryptprimitives.dll"
                };
                for (const wchar_t* d : kCoreWin32Dlls) {
                    if (targetLower == sys32Lower + d) {
                        targetIsUniversalWin32CoreDll = true;
                        break;
                    }
                }
            }
            bool targetCoversSys32 = ((targetType == LockTargetType::Directory || targetType == LockTargetType::Drive) &&
                                      DoesDosPathMatchTarget(sys32, targetLower, targetType));

            for (const auto& kv : toolhelpNames) {
                DWORD pid = kv.first;
                if (pid == 0 || pid == GetCurrentProcessId()) continue;
                std::wstring lowExe = ToLowerCopy(kv.second);

                if (pid == 4) {
                    if (targetCoversSys32 ||
                        (targetType == LockTargetType::File && targetLower == sys32Lower + L"\\ntoskrnl.exe")) {
                        recordMatch(pid, LOCK_SRC_PROTECTED_CORE, sys32 + L"\\ntoskrnl.exe", false);
                    }
                    continue;
                }

                if (!IsKnownCriticalCoreImage(lowExe)) continue;
                // Only supplement when OpenProcess(PROCESS_QUERY_INFORMATION) is actually denied to us
                HANDLE hCheck = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
                if (hCheck) {
                    CloseHandle(hCheck);
                    continue;
                }

                std::wstring expectedImg = sys32 + L"\\" + kv.second;
                bool matchedByName = (targetType == LockTargetType::File &&
                                      DoesDosPathMatchTarget(expectedImg, targetLower, targetType));
                bool matchedByDll = (targetIsUniversalNtdll && lowExe != L"registry" && lowExe != L"memcompression" && lowExe != L"memory compression" && lowExe != L"secure system") ||
                                    (targetIsUniversalWin32CoreDll && (lowExe == L"wininit.exe" || lowExe == L"services.exe" || lowExe == L"lsass.exe" || lowExe == L"winlogon.exe" || lowExe == L"dwm.exe" || lowExe == L"fontdrvhost.exe"));

                if (matchedByName || matchedByDll || targetCoversSys32) {
                    std::wstring matchObj = matchedByDll ? result.target.normalizedPath : expectedImg;
                    recordMatch(pid, LOCK_SRC_PROTECTED_CORE, matchObj, false);
                }
            }

            // Also match running svchost.exe when user checks System32\svchost.exe directly
            if (targetType == LockTargetType::File && targetLower == sys32Lower + L"\\svchost.exe") {
                for (const auto& kv : toolhelpNames) {
                    if (ToLowerCopy(kv.second) == L"svchost.exe") {
                        recordMatch(kv.first, LOCK_SRC_PROTECTED_CORE, sys32 + L"\\svchost.exe", false);
                    }
                }
            }
        }
    }

    // Count how many running processes could not be opened for handle inspection (used to inform UI)
    for (const auto& kv : toolhelpNames) {
        DWORD pid = kv.first;
        if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) continue;
        HANDLE hTest = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        if (!hTest) {
            result.deniedProcessOpenCount++;
        } else {
            CloseHandle(hTest);
        }
    }

    // Populate metadata & criticality for each matched PID
    result.entries.reserve(byPid.size());
    for (auto& kv : byPid) {
        bool openDenied = false;
        QueryProcessMetaAndCriticality(kv.first, nt, toolhelpNames, svcMap, driveMap, kv.second, openDenied);
        if (iconResolver && !kv.second.imagePath.empty()) {
            kv.second.icon = iconResolver(kv.second.imagePath);
        }
        if (kv.second.criticalLevel == ProcessCriticalLevel::CriticalCore) {
            result.criticalCount++;
        } else if (kv.second.criticalLevel == ProcessCriticalLevel::SystemService) {
            result.systemServiceCount++;
        }
        result.entries.push_back(std::move(kv.second));
    }

    std::sort(result.entries.begin(), result.entries.end(), [](const LockEntry& a, const LockEntry& b) {
        if (a.criticalLevel != b.criticalLevel) {
            return static_cast<int>(a.criticalLevel) > static_cast<int>(b.criticalLevel);
        }
        int cmp = _wcsicmp(a.processName.c_str(), b.processName.c_str());
        if (cmp != 0) return cmp < 0;
        return a.pid < b.pid;
    });

    result.elapsedMs = GetTickCount64() - startTick;
    return result;
}

} // namespace taskman
