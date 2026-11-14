#include "startup_manager.h"
#include "uwp_apps.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <unordered_map>

namespace taskman {

static std::wstring LowerStr(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

StartupManager::~StartupManager() {
    for (HICON ico : ownedIcons_) {
        if (ico) DestroyIcon(ico);
    }
    ownedIcons_.clear();
}

std::wstring StartupManager::ResolveShortcutTarget(const std::wstring& lnkPath) {
    std::wstring target;
    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl)))) {
        IPersistFile* ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf)))) {
            if (SUCCEEDED(ppf->Load(lnkPath.c_str(), STGM_READ))) {
                wchar_t buf[MAX_PATH] = {};
                if (SUCCEEDED(psl->GetPath(buf, MAX_PATH, nullptr, 0)) && buf[0]) {
                    target = buf;
                }
            }
            ppf->Release();
        }
        psl->Release();
    }
    return target;
}

std::wstring StartupManager::ResolveCommandExe(const std::wstring& cmdLine) {
    if (cmdLine.empty()) return L"";
    wchar_t expanded[MAX_PATH * 2] = {};
    ExpandEnvironmentStringsW(cmdLine.c_str(), expanded, ARRAYSIZE(expanded));

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(expanded, &argc);
    std::wstring candidate;
    if (argv && argc > 0) {
        candidate = argv[0];
        LocalFree(argv);
    } else {
        candidate = expanded;
    }

    if (candidate.empty()) return L"";
    if (PathFileExistsW(candidate.c_str())) return candidate;

    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, candidate.c_str(), L".exe", MAX_PATH, found, nullptr)) {
        return found;
    }
    return candidate;
}

static std::wstring QueryVersionString(const std::wstring& exePath, const wchar_t* fieldName) {
    if (exePath.empty() || !PathFileExistsW(exePath.c_str())) return L"";
    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (verSize == 0) return L"";
    std::vector<BYTE> buf(verSize);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, verSize, buf.data())) return L"";

    auto tryQuery = [&](WORD lang, WORD cp) -> std::wstring {
        wchar_t sub[128];
        swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", lang, cp, fieldName);
        wchar_t* str = nullptr;
        UINT len = 0;
        if (VerQueryValueW(buf.data(), sub, reinterpret_cast<LPVOID*>(&str), &len) && str && len > 0) {
            std::wstring out = str;
            while (!out.empty() && std::iswspace(out.front())) out.erase(out.begin());
            while (!out.empty() && std::iswspace(out.back())) out.pop_back();
            return out;
        }
        return L"";
    };

    struct LANGANDCODEPAGE { WORD wLanguage; WORD wCodePage; }* trans = nullptr;
    UINT cbTrans = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<LPVOID*>(&trans), &cbTrans) && cbTrans >= 4) {
        UINT count = cbTrans / sizeof(LANGANDCODEPAGE);
        for (UINT i = 0; i < count; ++i) {
            std::wstring s = tryQuery(trans[i].wLanguage, trans[i].wCodePage);
            if (!s.empty()) return s;
        }
    }
    std::wstring en = tryQuery(0x0409, 0x04B0);
    if (!en.empty()) return en;
    return tryQuery(0x0409, 0x04E4);
}

std::wstring StartupManager::GetPublisher(const std::wstring& exePath) {
    if (exePath.empty() || !PathFileExistsW(exePath.c_str())) return L"";

    HCERTSTORE hStore = nullptr;
    HCRYPTMSG hMsg = nullptr;
    DWORD dwEncoding = 0, dwContentType = 0, dwFormatType = 0;
    if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, exePath.c_str(),
                         CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                         CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                         &dwEncoding, &dwContentType, &dwFormatType,
                         &hStore, &hMsg, nullptr)) {
        DWORD signerSize = 0;
        if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) && signerSize > 0) {
            std::vector<BYTE> signerBuf(signerSize);
            if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, signerBuf.data(), &signerSize)) {
                auto* si = reinterpret_cast<PCMSG_SIGNER_INFO>(signerBuf.data());
                CERT_INFO ci{};
                ci.Issuer = si->Issuer;
                ci.SerialNumber = si->SerialNumber;
                PCCERT_CONTEXT pCert = CertFindCertificateInStore(
                    hStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr);
                if (pCert) {
                    wchar_t name[256] = {};
                    if (CertGetNameStringW(pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256) > 1) {
                        CertFreeCertificateContext(pCert);
                        if (hStore) CertCloseStore(hStore, 0);
                        if (hMsg) CryptMsgClose(hMsg);
                        return name;
                    }
                    CertFreeCertificateContext(pCert);
                }
            }
        }
        if (hStore) CertCloseStore(hStore, 0);
        if (hMsg) CryptMsgClose(hMsg);
    }

    return QueryVersionString(exePath, L"CompanyName");
}

std::wstring StartupManager::GetFileDescription(const std::wstring& exePath) {
    return QueryVersionString(exePath, L"FileDescription");
}

bool StartupManager::ReadApprovedState(HKEY root, const std::wstring& approvedSubKey, const std::wstring& valueName) {
    HKEY hApp = nullptr;
    if (RegOpenKeyExW(root, approvedSubKey.c_str(), 0, KEY_READ, &hApp) != ERROR_SUCCESS) {
        return true;
    }
    BYTE data[32] = {};
    DWORD sz = sizeof(data);
    DWORD type = 0;
    bool enabled = true;
    if (RegQueryValueExW(hApp, valueName.c_str(), nullptr, &type, data, &sz) == ERROR_SUCCESS &&
        type == REG_BINARY && sz >= 1) {
        enabled = (data[0] & 1) == 0;
    }
    RegCloseKey(hApp);
    return enabled;
}

void StartupManager::Refresh() {
    items_.clear();
    for (HICON ico : ownedIcons_) {
        if (ico) DestroyIcon(ico);
    }
    ownedIcons_.clear();

    auto addIcon = [&](const std::wstring& path) -> HICON {
        if (!path.empty()) {
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
                ownedIcons_.push_back(sfi.hIcon);
                return sfi.hIcon;
            }
        }
        // Fallback generic application icon for missing / broken startup paths (e.g. Claude.exe)
        const wchar_t* ext = path.empty() ? nullptr : PathFindExtensionW(path.c_str());
        std::wstring dummy = (ext && ext[0]) ? (std::wstring(L"dummy") + ext) : L"dummy.exe";
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(dummy.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES) && sfi.hIcon) {
            ownedIcons_.push_back(sfi.hIcon);
            return sfi.hIcon;
        }
        return nullptr;
    };

    auto scanRunKey = [&](HKEY root, const wchar_t* runSubKey, const wchar_t* approvedSubKey, REGSAM wowFlag) {
        HKEY hRun = nullptr;
        if (RegOpenKeyExW(root, runSubKey, 0, KEY_READ | wowFlag, &hRun) != ERROR_SUCCESS) return;

        wchar_t valName[512];
        BYTE valData[2048];
        for (DWORD idx = 0; ; ++idx) {
            DWORD nameLen = ARRAYSIZE(valName);
            DWORD dataSize = sizeof(valData) - sizeof(wchar_t);
            DWORD type = 0;
            LONG res = RegEnumValueW(hRun, idx, valName, &nameLen, nullptr, &type, valData, &dataSize);
            if (res == ERROR_NO_MORE_ITEMS) break;
            if (res != ERROR_SUCCESS) continue;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;

            valData[dataSize] = 0;
            valData[dataSize + 1] = 0;
            std::wstring cmd = reinterpret_cast<const wchar_t*>(valData);
            if (valName[0] == L'\0' || cmd.empty()) continue;

            StartupItem item;
            item.valueName = valName;
            item.command = cmd;
            item.exePath = ResolveCommandExe(cmd);
            std::wstring desc = GetFileDescription(item.exePath);
            item.name = !desc.empty() ? desc : valName;
            item.publisher = GetPublisher(item.exePath);
            item.enabled = ReadApprovedState(root, approvedSubKey, item.valueName);
            item.rootKey = root;
            item.approvedSubKey = approvedSubKey;
            item.isFolderItem = false;
            item.icon = addIcon(item.exePath);

            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!item.exePath.empty() && GetFileAttributesExW(item.exePath.c_str(), GetFileExInfoStandard, &fad)) {
                uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                if (sz > 25ULL * 1024 * 1024) item.impact = 3;
                else if (sz > 5ULL * 1024 * 1024) item.impact = 2;
                else item.impact = 1;
            }
            items_.push_back(std::move(item));
        }
        RegCloseKey(hRun);
    };

    scanRunKey(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", 0);
    scanRunKey(HKEY_LOCAL_MACHINE,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", KEY_WOW64_64KEY);
    scanRunKey(HKEY_LOCAL_MACHINE,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32", KEY_WOW64_32KEY);

    auto scanStartupFolder = [&](REFKNOWNFOLDERID fid, HKEY root, const wchar_t* approvedSubKey) {
        PWSTR folderPath = nullptr;
        if (FAILED(SHGetKnownFolderPath(fid, 0, nullptr, &folderPath)) || !folderPath) return;

        std::wstring pattern = std::wstring(folderPath) + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;

                std::wstring fullPath = std::wstring(folderPath) + L"\\" + fd.cFileName;
                std::wstring target = fullPath;
                const wchar_t* ext = PathFindExtensionW(fd.cFileName);
                if (ext && _wcsicmp(ext, L".lnk") == 0) {
                    std::wstring resolved = ResolveShortcutTarget(fullPath);
                    if (!resolved.empty()) target = resolved;
                }

                StartupItem item;
                item.valueName = fd.cFileName;
                std::wstring fallbackName = fd.cFileName;
                if (ext && _wcsicmp(ext, L".lnk") == 0) {
                    size_t dot = fallbackName.rfind(L'.');
                    if (dot != std::wstring::npos) fallbackName.resize(dot);
                }
                std::wstring desc = GetFileDescription(target);
                item.name = !desc.empty() ? desc : fallbackName;
                item.command = fullPath;
                item.exePath = target;
                item.publisher = GetPublisher(target);
                item.enabled = ReadApprovedState(root, approvedSubKey, item.valueName);
                item.rootKey = root;
                item.approvedSubKey = approvedSubKey;
                item.isFolderItem = true;
                item.icon = addIcon(target);
                items_.push_back(std::move(item));
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        CoTaskMemFree(folderPath);
    };

    scanStartupFolder(FOLDERID_Startup, HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder");
    scanStartupFolder(FOLDERID_CommonStartup, HKEY_LOCAL_MACHINE,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder");

    // Enumerate Microsoft Store / UWP StartupTask entries under HKCU AppModel\SystemAppData\<pkgFamily>\<taskId>
    std::vector<UwpAppEntry> uwpApps = EnumerateInstalledUwpApps(24);
    std::unordered_map<std::wstring, size_t> familyToIdx;
    for (size_t i = 0; i < uwpApps.size(); ++i) {
        if (!uwpApps[i].pkgFamily.empty() && familyToIdx.find(uwpApps[i].pkgFamily) == familyToIdx.end()) {
            familyToIdx[uwpApps[i].pkgFamily] = i;
        }
    }

    const wchar_t* sysAppDataRoot =
        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\SystemAppData";
    HKEY hSysData = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sysAppDataRoot, 0, KEY_READ, &hSysData) == ERROR_SUCCESS) {
        wchar_t pkgKeyName[256];
        for (DWORD pIdx = 0; ; ++pIdx) {
            DWORD pLen = ARRAYSIZE(pkgKeyName);
            if (RegEnumKeyExW(hSysData, pIdx, pkgKeyName, &pLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            std::wstring pkgLower = LowerStr(std::wstring(pkgKeyName, pLen));
            auto appIt = familyToIdx.find(pkgLower);
            if (appIt == familyToIdx.end()) continue; // only show installed user-facing UWP apps

            HKEY hPkg = nullptr;
            if (RegOpenKeyExW(hSysData, pkgKeyName, 0, KEY_READ, &hPkg) != ERROR_SUCCESS) continue;

            wchar_t taskKeyName[256];
            for (DWORD tIdx = 0; ; ++tIdx) {
                DWORD tLen = ARRAYSIZE(taskKeyName);
                if (RegEnumKeyExW(hPkg, tIdx, taskKeyName, &tLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                    break;
                }
                HKEY hTask = nullptr;
                if (RegOpenKeyExW(hPkg, taskKeyName, 0, KEY_READ, &hTask) != ERROR_SUCCESS) continue;

                DWORD state = 0;
                DWORD stateSize = sizeof(state);
                DWORD stateType = 0;
                if (RegQueryValueExW(hTask, L"State", nullptr, &stateType,
                                     reinterpret_cast<BYTE*>(&state), &stateSize) == ERROR_SUCCESS &&
                    stateType == REG_DWORD) {
                    auto& app = uwpApps[appIt->second];
                    if (app.displayName.empty()) {
                        RegCloseKey(hTask);
                        continue;
                    }
                    StartupItem item;
                    item.name = app.displayName;
                    item.valueName = std::wstring(taskKeyName, tLen);
                    item.publisher = app.publisher;
                    item.enabled = (state == 2 || state == 4);
                    item.impact = 0; // Store startup tasks are reported as Not measured until measured
                    item.isStoreTask = true;
                    item.rootKey = HKEY_CURRENT_USER;
                    item.uwpTaskSubKey = std::wstring(sysAppDataRoot) + L"\\" + pkgKeyName + L"\\" + taskKeyName;
                    if (app.icon) {
                        item.icon = app.icon;
                        ownedIcons_.push_back(app.icon);
                        app.icon = nullptr;
                    }
                    items_.push_back(std::move(item));
                }
                RegCloseKey(hTask);
            }
            RegCloseKey(hPkg);
        }
        RegCloseKey(hSysData);
    }
    FreeUwpAppEntries(uwpApps);

    std::sort(items_.begin(), items_.end(), [](const StartupItem& a, const StartupItem& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

bool StartupManager::SetEnabled(size_t index, bool enable) {
    if (index >= items_.size()) return false;
    StartupItem& item = items_[index];

    if (item.isStoreTask) {
        HKEY hTask = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, item.uwpTaskSubKey.c_str(), 0, KEY_SET_VALUE, &hTask) != ERROR_SUCCESS) {
            return false;
        }
        DWORD newState = enable ? 2 : 1; // 2 = Enabled, 1 = DisabledByUser
        LONG res = RegSetValueExW(hTask, L"State", 0, REG_DWORD,
                                  reinterpret_cast<const BYTE*>(&newState), sizeof(newState));
        RegCloseKey(hTask);
        if (res == ERROR_SUCCESS) {
            item.enabled = enable;
            return true;
        }
        return false;
    }

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(item.rootKey, item.approvedSubKey.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    BYTE buf[12] = {};
    if (enable) {
        buf[0] = 0x02;
    } else {
        buf[0] = 0x03;
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        std::memcpy(buf + 4, &ft, sizeof(FILETIME));
    }

    LONG res = RegSetValueExW(hKey, item.valueName.c_str(), 0, REG_BINARY, buf, sizeof(buf));
    RegCloseKey(hKey);
    if (res == ERROR_SUCCESS) {
        item.enabled = enable;
        return true;
    }
    return false;
}

} // namespace taskman
