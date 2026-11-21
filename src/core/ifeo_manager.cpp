#include "ifeo_manager.h"
#include "format_util.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <thread>
#include <vector>

namespace taskman {
namespace {

std::wstring NormalizeFullPath(const std::wstring& path) {
    if (path.empty()) return L"";
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD len = GetFullPathNameW(path.c_str(), ARRAYSIZE(buf), buf, nullptr);
    if (len > 0 && len < ARRAYSIZE(buf)) {
        return std::wstring(buf, len);
    }
    return path;
}

std::wstring LowerCopy(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return _wcsicmp(s.c_str() + (s.size() - suffix.size()), suffix.c_str()) == 0;
}

bool StartsWithPathPrefixNoCase(const std::wstring& path, const std::wstring& dirPrefix) {
    if (dirPrefix.empty()) return false;
    std::wstring normP = LowerCopy(NormalizeFullPath(path));
    std::wstring normD = LowerCopy(NormalizeFullPath(dirPrefix));
    while (normD.size() > 3 && (normD.back() == L'\\' || normD.back() == L'/')) {
        normD.pop_back();
    }
    if (normP == normD) return true;
    if (normP.size() > normD.size() &&
        normP.compare(0, normD.size(), normD) == 0 &&
        (normP[normD.size()] == L'\\' || normP[normD.size()] == L'/')) {
        return true;
    }
    return false;
}

bool GetKnownFolderPathStr(REFKNOWNFOLDERID rfid, std::wstring& out) {
    PWSTR p = nullptr;
    HRESULT hr = SHGetKnownFolderPath(rfid, 0, nullptr, &p);
    if (SUCCEEDED(hr) && p) {
        out = p;
        CoTaskMemFree(p);
        return true;
    }
    return false;
}

bool GetCurrentUserSidString(std::wstring& outSid) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &sz);
    if (sz == 0) {
        CloseHandle(hToken);
        return false;
    }
    std::vector<BYTE> buf(sz);
    bool ok = false;
    if (GetTokenInformation(hToken, TokenUser, buf.data(), sz, &sz)) {
        auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
        LPWSTR sidStr = nullptr;
        if (ConvertSidToStringSidW(tu->User.Sid, &sidStr) && sidStr) {
            outSid = sidStr;
            LocalFree(sidStr);
            ok = true;
        }
    }
    CloseHandle(hToken);
    return ok;
}

bool RunElevatedIfeoWorker(HWND parent, const wchar_t* verbFlag, const std::wstring& targetPath, DWORD* outError) {
    if (outError) *outError = ERROR_SUCCESS;
    std::wstring selfExe = IfeoManager::GetCurrentExePath();
    if (selfExe.empty()) {
        if (outError) *outError = ERROR_FILE_NOT_FOUND;
        return false;
    }

    std::wstring params = std::wstring(verbFlag) + L" \"" + targetPath + L"\"";
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = parent;
    sei.lpVerb = L"runas";
    sei.lpFile = selfExe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        if (outError) *outError = GetLastError();
        return false;
    }
    DWORD exitCode = ERROR_GEN_FAILURE;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
    }
    if (exitCode != ERROR_SUCCESS) {
        if (outError) *outError = exitCode;
        return false;
    }
    return true;
}

bool HasShellTrayInCurrentSession(DWORD curSession) {
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray || !IsWindow(hTray)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hTray, &pid);
    if (pid == 0) return false;
    DWORD sess = MAXDWORD;
    if (ProcessIdToSessionId(pid, &sess) && sess == curSession) {
        return true;
    }
    return false;
}

bool HasExplorerProcessInCurrentSession(DWORD curSession) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true; // fail open to avoid duplicate spawns
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                DWORD sess = MAXDWORD;
                if (ProcessIdToSessionId(pe.th32ProcessID, &sess) && sess == curSession) {
                    found = true;
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

} // namespace

std::wstring IfeoManager::GetCurrentExePath() {
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    if (len == 0 || len >= ARRAYSIZE(buf)) return L"";
    return NormalizeFullPath(std::wstring(buf, len));
}

bool IfeoManager::IsTestRootActive(std::wstring* outHkcuSubKey) {
    wchar_t envBuf[512] = {};
    DWORD n = GetEnvironmentVariableW(L"TASKMGR_ENH_IFEO_TEST_ROOT", envBuf, ARRAYSIZE(envBuf));
    if (n == 0 || n >= ARRAYSIZE(envBuf)) return false;
    std::wstring sub = envBuf;
    while (!sub.empty() && (sub.back() == L'\\' || sub.back() == L'/')) sub.pop_back();
    if (sub.empty()) return false;
    if (!EndsWithNoCase(sub, L"\\Taskmgr.exe")) {
        sub += L"\\Taskmgr.exe";
    }
    if (outHkcuSubKey) *outHkcuSubKey = sub;
    return true;
}

std::wstring IfeoManager::ExtractExePathFromCommand(const std::wstring& cmd) {
    size_t i = 0;
    while (i < cmd.size() && iswspace(cmd[i])) ++i;
    if (i >= cmd.size()) return L"";

    std::wstring raw;
    if (cmd[i] == L'"') {
        size_t end = cmd.find(L'"', i + 1);
        if (end != std::wstring::npos) {
            raw = cmd.substr(i + 1, end - (i + 1));
        } else {
            raw = cmd.substr(i + 1);
        }
    } else {
        // Unquoted command line: match up to ".exe" if spaces appear, otherwise first whitespace token
        std::wstring lower = LowerCopy(cmd.substr(i));
        size_t exePos = lower.find(L".exe");
        if (exePos != std::wstring::npos &&
            (i + exePos + 4 >= cmd.size() || iswspace(cmd[i + exePos + 4]))) {
            raw = cmd.substr(i, exePos + 4);
        } else {
            size_t ws = cmd.find_first_of(L" \t", i);
            raw = (ws == std::wstring::npos) ? cmd.substr(i) : cmd.substr(i, ws - i);
        }
    }
    return NormalizeFullPath(raw);
}

bool IfeoManager::PathsEqual(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(NormalizeFullPath(a).c_str(), NormalizeFullPath(b).c_str()) == 0;
}

IfeoStatus IfeoManager::QueryStatus() {
    IfeoStatus st;
    std::wstring testSub;
    st.usesTestRoot = IsTestRootActive(&testSub);
    HKEY hRoot = st.usesTestRoot ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    const wchar_t* subKey = st.usesTestRoot ? testSub.c_str() : kIfeoSubKey;

    HKEY hKey = nullptr;
    REGSAM sam = KEY_READ | (st.usesTestRoot ? 0 : KEY_WOW64_64KEY);
    if (RegOpenKeyExW(hRoot, subKey, 0, sam, &hKey) != ERROR_SUCCESS) {
        return st;
    }

    wchar_t buf[2048] = {};
    DWORD type = 0;
    DWORD cb = sizeof(buf);
    if (RegQueryValueExW(hKey, L"Debugger", nullptr, &type, reinterpret_cast<BYTE*>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::wstring raw = buf;
        if (type == REG_EXPAND_SZ) {
            wchar_t expBuf[2048] = {};
            DWORD en = ExpandEnvironmentStringsW(raw.c_str(), expBuf, ARRAYSIZE(expBuf));
            if (en > 0 && en < ARRAYSIZE(expBuf)) raw = expBuf;
        }
        size_t firstNonWs = raw.find_first_not_of(L" \t\r\n");
        if (firstNonWs != std::wstring::npos) {
            st.hasEntry = true;
            st.rawDebugger = raw;
            st.resolvedPath = ExtractExePathFromCommand(raw);

            std::wstring curExe = GetCurrentExePath();
            st.matchesCurrentExe = PathsEqual(st.resolvedPath, curExe);
            const wchar_t* base = PathFindFileNameW(st.resolvedPath.c_str());
            bool nameLooksOurs = base && (_wcsicmp(base, L"TaskmgrEnhanced.exe") == 0);
            st.isOurs = st.matchesCurrentExe || nameLooksOurs;
        }
    }
    RegCloseKey(hKey);
    return st;
}

bool IfeoManager::IsOurIfeoEnabled() {
    return QueryStatus().isOurs;
}

PathCheckResult IfeoManager::InspectCurrentExePath() {
    PathCheckResult res;
    res.currentExePath = GetCurrentExePath();
    if (res.currentExePath.empty()) return res;

    const wchar_t* fileName = PathFindFileNameW(res.currentExePath.c_str());
    if (fileName && _wcsicmp(fileName, L"Taskmgr.exe") == 0) {
        res.isBadFilename = true;
    }

    // Check drive type (removable, CD-ROM, RAM disk, network)
    if (res.currentExePath.size() >= 2 && res.currentExePath[0] == L'\\' && res.currentExePath[1] == L'\\') {
        res.isRemovableOrNet = true;
        res.isVolatilePath = true;
    } else if (res.currentExePath.size() >= 3 && res.currentExePath[1] == L':') {
        std::wstring root = res.currentExePath.substr(0, 3);
        UINT dt = GetDriveTypeW(root.c_str());
        if (dt == DRIVE_REMOVABLE || dt == DRIVE_REMOTE || dt == DRIVE_CDROM || dt == DRIVE_RAMDISK) {
            res.isRemovableOrNet = true;
            res.isVolatilePath = true;
        }
    }

    if (!res.isVolatilePath) {
        wchar_t tempDir[MAX_PATH] = {};
        if (GetTempPathW(ARRAYSIZE(tempDir), tempDir) > 0 &&
            StartsWithPathPrefixNoCase(res.currentExePath, tempDir)) {
            res.isVolatilePath = true;
        }
    }

    std::wstring folder;
    if (!res.isVolatilePath && GetKnownFolderPathStr(FOLDERID_Downloads, folder) &&
        StartsWithPathPrefixNoCase(res.currentExePath, folder)) {
        res.isVolatilePath = true;
    }
    if (!res.isVolatilePath && GetKnownFolderPathStr(FOLDERID_Desktop, folder) &&
        StartsWithPathPrefixNoCase(res.currentExePath, folder)) {
        res.isVolatilePath = true;
    }
    return res;
}

bool IfeoManager::ApplyFileProtection(const std::wstring& exePath, DWORD* outError) {
    if (outError) *outError = ERROR_SUCCESS;
    if (exePath.empty() || GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (outError) *outError = ERROR_FILE_NOT_FOUND;
        return false;
    }

    // Explicit DENY (0x10116 = DELETE | WRITE_DATA | APPEND_DATA | WRITE_EA | WRITE_ATTRIBUTES) for Everyone (WD)
    // followed by ALLOW RX+WRITE_DAC+WRITE_OWNER (0x1e00a9) for SYSTEM and Administrators,
    // and ALLOW RX (0x1200a9) for Authenticated Users and Builtin Users.
    const wchar_t* sddl = L"D:P(D;;0x10116;;;WD)(A;;0x1e00a9;;;SY)(A;;0x1e00a9;;;BA)(A;;0x1200a9;;;AU)(A;;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &pSD, nullptr) || !pSD) {
        if (outError) *outError = GetLastError();
        return false;
    }

    PACL pDacl = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    DWORD err = ERROR_INVALID_SECURITY_DESCR;
    if (GetSecurityDescriptorDacl(pSD, &daclPresent, &pDacl, &daclDefaulted) && daclPresent) {
        err = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(exePath.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, pDacl, nullptr);
    } else {
        err = GetLastError();
    }
    LocalFree(pSD);

    if (err != ERROR_SUCCESS) {
        if (outError) *outError = err;
        return false;
    }

    DWORD attrs = GetFileAttributesW(exePath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesW(exePath.c_str(), attrs | FILE_ATTRIBUTE_READONLY);
    }
    return true;
}

bool IfeoManager::RemoveFileProtection(const std::wstring& exePath, DWORD* outError) {
    if (outError) *outError = ERROR_SUCCESS;
    if (exePath.empty() || GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (outError) *outError = ERROR_FILE_NOT_FOUND;
        return true; // target already gone; don't block uninstall
    }

    // Must restore DACL BEFORE clearing ReadOnly, because DENY (0x10116) blocks FILE_WRITE_ATTRIBUTES (0x100).
    std::wstring userSid;
    std::wstring ownerSid;
    PSECURITY_DESCRIPTOR pCurSd = nullptr;
    PSID pOwner = nullptr;
    if (GetNamedSecurityInfoW(exePath.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                              &pOwner, nullptr, nullptr, nullptr, &pCurSd) == ERROR_SUCCESS && pOwner) {
        LPWSTR ownerStr = nullptr;
        if (ConvertSidToStringSidW(pOwner, &ownerStr) && ownerStr) {
            ownerSid = ownerStr;
            LocalFree(ownerStr);
        }
    }
    if (pCurSd) LocalFree(pCurSd);

    std::wstring sddl = L"D:(A;;FA;;;SY)(A;;FA;;;BA)";
    if (!ownerSid.empty()) {
        sddl += L"(A;;FA;;;" + ownerSid + L")";
    }
    if (GetCurrentUserSidString(userSid) && !userSid.empty() && _wcsicmp(userSid.c_str(), ownerSid.c_str()) != 0) {
        sddl += L"(A;;FA;;;" + userSid + L")";
    } else if (ownerSid.empty()) {
        sddl += L"(A;;FA;;;AU)";
    }

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &pSD, nullptr) || !pSD) {
        if (outError) *outError = GetLastError();
        return false;
    }

    PACL pDacl = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    DWORD err = ERROR_INVALID_SECURITY_DESCR;
    if (GetSecurityDescriptorDacl(pSD, &daclPresent, &pDacl, &daclDefaulted) && daclPresent) {
        err = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(exePath.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, pDacl, nullptr);
    } else {
        err = GetLastError();
    }
    LocalFree(pSD);

    if (err != ERROR_SUCCESS) {
        if (outError) *outError = err;
        return false;
    }

    DWORD attrs = GetFileAttributesW(exePath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        DWORD newAttrs = attrs & ~FILE_ATTRIBUTE_READONLY;
        SetFileAttributesW(exePath.c_str(), newAttrs == 0 ? FILE_ATTRIBUTE_NORMAL : newAttrs);
    }
    return true;
}

bool IfeoManager::IsFileProtected(const std::wstring& exePath) {
    DWORD attrs = GetFileAttributesW(exePath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0;
}

bool IfeoManager::InstallDirect(const std::wstring& targetExePath, DWORD* outError) {
    if (outError) *outError = ERROR_SUCCESS;
    std::wstring normTarget = NormalizeFullPath(targetExePath);
    if (normTarget.empty()) {
        if (outError) *outError = ERROR_INVALID_PARAMETER;
        return false;
    }

    const wchar_t* targetName = PathFindFileNameW(normTarget.c_str());
    if (targetName && _wcsicmp(targetName, L"Taskmgr.exe") == 0) {
        if (outError) *outError = ERROR_INVALID_NAME;
        return false;
    }

    std::wstring curExe = GetCurrentExePath();
    if (curExe.empty()) {
        if (outError) *outError = ERROR_FILE_NOT_FOUND;
        return false;
    }

    if (!PathsEqual(curExe, normTarget)) {
        std::wstring parentDir = normTarget;
        size_t slash = parentDir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            parentDir.resize(slash);
            if (!parentDir.empty()) {
                SHCreateDirectoryExW(nullptr, parentDir.c_str(), nullptr);
            }
        }
        if (GetFileAttributesW(normTarget.c_str()) != INVALID_FILE_ATTRIBUTES) {
            RemoveFileProtection(normTarget);
        }
        if (!CopyFileW(curExe.c_str(), normTarget.c_str(), FALSE)) {
            if (outError) *outError = GetLastError();
            return false;
        }
    }

    std::wstring testSub;
    bool usesTestRoot = IsTestRootActive(&testSub);
    HKEY hRoot = usesTestRoot ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    const wchar_t* subKey = usesTestRoot ? testSub.c_str() : kIfeoSubKey;

    HKEY hKey = nullptr;
    REGSAM sam = KEY_SET_VALUE | KEY_QUERY_VALUE | (usesTestRoot ? 0 : KEY_WOW64_64KEY);
    LONG rc = RegCreateKeyExW(hRoot, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE, sam, nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        if (outError) *outError = static_cast<DWORD>(rc);
        return false;
    }

    std::wstring debuggerVal = L"\"" + normTarget + L"\"";
    DWORD cb = static_cast<DWORD>((debuggerVal.size() + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(hKey, L"Debugger", 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(debuggerVal.c_str()), cb);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        if (outError) *outError = static_cast<DWORD>(rc);
        return false;
    }

    DWORD protErr = ERROR_SUCCESS;
    if (!ApplyFileProtection(normTarget, &protErr)) {
        // File protection failed; roll back Debugger value so state stays consistent.
        HKEY hDel = nullptr;
        if (RegOpenKeyExW(hRoot, subKey, 0, sam, &hDel) == ERROR_SUCCESS) {
            RegDeleteValueW(hDel, L"Debugger");
            RegCloseKey(hDel);
        }
        if (outError) *outError = protErr;
        return false;
    }

    return true;
}

bool IfeoManager::UninstallDirect(const std::wstring& targetExePath, DWORD* outError) {
    if (outError) *outError = ERROR_SUCCESS;

    IfeoStatus curStatus = QueryStatus();
    std::wstring registeredPath = curStatus.resolvedPath;

    std::wstring testSub;
    bool usesTestRoot = IsTestRootActive(&testSub);
    HKEY hRoot = usesTestRoot ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    const wchar_t* subKey = usesTestRoot ? testSub.c_str() : kIfeoSubKey;

    HKEY hKey = nullptr;
    REGSAM sam = KEY_SET_VALUE | KEY_QUERY_VALUE | (usesTestRoot ? 0 : KEY_WOW64_64KEY);
    LONG rc = RegOpenKeyExW(hRoot, subKey, 0, sam, &hKey);
    if (rc == ERROR_SUCCESS) {
        rc = RegDeleteValueW(hKey, L"Debugger");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;

        DWORD subKeys = 0, values = 0;
        if (RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr,
                             &values, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
            subKeys == 0 && values == 0) {
            RegCloseKey(hKey);
            hKey = nullptr;
            if (usesTestRoot) {
                RegDeleteKeyW(hRoot, subKey);
            } else {
                RegDeleteKeyExW(hRoot, subKey, KEY_WOW64_64KEY, 0);
            }
        }
        if (hKey) RegCloseKey(hKey);

        if (rc != ERROR_SUCCESS) {
            if (outError) *outError = static_cast<DWORD>(rc);
            return false;
        }
    } else if (rc != ERROR_FILE_NOT_FOUND) {
        if (outError) *outError = static_cast<DWORD>(rc);
        return false;
    }

    DWORD unlockErr = ERROR_SUCCESS;
    std::wstring normTarget = NormalizeFullPath(targetExePath);
    if (!normTarget.empty() && GetFileAttributesW(normTarget.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!RemoveFileProtection(normTarget, &unlockErr)) {
            if (outError) *outError = unlockErr;
            return false;
        }
    }
    if (!registeredPath.empty() && !PathsEqual(registeredPath, normTarget) &&
        GetFileAttributesW(registeredPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DWORD regErr = ERROR_SUCCESS;
        if (!RemoveFileProtection(registeredPath, &regErr)) {
            if (outError) *outError = regErr;
            return false;
        }
    }
    return true;
}

bool IfeoManager::InstallReversible(HWND parent, const std::wstring& targetExePath, DWORD* outError) {
    if (IsTestRootActive() || IsCurrentProcessElevated()) {
        return InstallDirect(targetExePath, outError);
    }
    return RunElevatedIfeoWorker(parent, L"/ifeo-install", NormalizeFullPath(targetExePath), outError);
}

bool IfeoManager::UninstallReversible(HWND parent, const std::wstring& targetExePath, DWORD* outError) {
    if (IsTestRootActive() || IsCurrentProcessElevated()) {
        return UninstallDirect(targetExePath, outError);
    }
    std::wstring norm = NormalizeFullPath(targetExePath.empty() ? GetCurrentExePath() : targetExePath);
    return RunElevatedIfeoWorker(parent, L"/ifeo-uninstall", norm, outError);
}

bool IfeoManager::IsExplorerAliveInCurrentSession() {
    DWORD curSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &curSession);
    return HasShellTrayInCurrentSession(curSession) || HasExplorerProcessInCurrentSession(curSession);
}

bool IfeoManager::ReviveExplorer(DWORD* outPid) {
    if (outPid) *outPid = 0;
    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, ARRAYSIZE(winDir)) == 0) return false;
    std::wstring explorerPath = std::wstring(winDir) + L"\\explorer.exe";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    BOOL launched = FALSE;

    // When TaskmgrEnhanced runs elevated, spawn Explorer with the linked standard user token
    // so Desktop/Taskbar do not run at High Integrity Level.
    if (IsCurrentProcessElevated()) {
        HANDLE hTok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) {
            TOKEN_LINKED_TOKEN linked{};
            DWORD cb = sizeof(linked);
            if (GetTokenInformation(hTok, TokenLinkedToken, &linked, sizeof(linked), &cb) && linked.LinkedToken) {
                HANDLE hPrimary = nullptr;
                if (DuplicateTokenEx(linked.LinkedToken, MAXIMUM_ALLOWED, nullptr,
                                     SecurityImpersonation, TokenPrimary, &hPrimary)) {
                    launched = CreateProcessWithTokenW(
                        hPrimary, LOGON_WITH_PROFILE,
                        explorerPath.c_str(), nullptr,
                        CREATE_DEFAULT_ERROR_MODE, nullptr, winDir, &si, &pi);
                    CloseHandle(hPrimary);
                }
                CloseHandle(linked.LinkedToken);
            }
            CloseHandle(hTok);
        }
    }

    if (!launched) {
        std::wstring cmd = L"\"" + explorerPath + L"\"";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        launched = CreateProcessW(
            explorerPath.c_str(), cmdBuf.data(),
            nullptr, nullptr, FALSE,
            DETACHED_PROCESS, nullptr, winDir, &si, &pi);
    }

    if (launched) {
        if (outPid) *outPid = pi.dwProcessId;
        if (pi.hThread) CloseHandle(pi.hThread);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        return true;
    }

    HINSTANCE hInst = ShellExecuteW(nullptr, L"open", explorerPath.c_str(), nullptr, winDir, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(hInst) > 32;
}

void IfeoManager::EnsureExplorerAliveAsync() {
    std::thread([]() {
        // Optional non-destructive self-test hook: skip touching desktop Explorer if disabled by test
        wchar_t markerBuf[512] = {};
        if (GetEnvironmentVariableW(L"TASKMGR_ENH_EXPLORER_TEST_MARKER", markerBuf, ARRAYSIZE(markerBuf)) > 0) {
            bool forceDead = GetEnvironmentVariableW(L"TASKMGR_ENH_EXPLORER_TEST_DEAD", nullptr, 0) > 0;
            bool alive = forceDead ? false : IsExplorerAliveInCurrentSession();
            if (!alive) {
                Sleep(250);
                alive = forceDead ? false : IsExplorerAliveInCurrentSession();
            }
            HANDLE hFile = CreateFileW(markerBuf, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                std::string msg = alive ? "ALIVE_NOOP\r\n" : "WOULD_REVIVE_EXPLORER\r\n";
                DWORD written = 0;
                WriteFile(hFile, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
                CloseHandle(hFile);
            }
            return;
        }

        if (IsExplorerAliveInCurrentSession()) return;
        Sleep(300);
        if (IsExplorerAliveInCurrentSession()) return;
        ReviveExplorer();
    }).detach();
}

} // namespace taskman
