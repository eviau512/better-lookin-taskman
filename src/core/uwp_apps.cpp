#include "uwp_apps.h"
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <unordered_map>

namespace taskman {

static std::wstring LowerStr(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

static std::wstring TrimW(std::wstring s) {
    while (!s.empty() && std::iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    return s;
}

static void ReplaceAllW(std::wstring& s, const wchar_t* from, const wchar_t* to) {
    size_t flen = wcslen(from);
    size_t tlen = wcslen(to);
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, flen, to);
        pos += tlen;
    }
}

static std::wstring DecodeXmlEntities(std::wstring s) {
    ReplaceAllW(s, L"&amp;", L"&");
    ReplaceAllW(s, L"&lt;", L"<");
    ReplaceAllW(s, L"&gt;", L">");
    ReplaceAllW(s, L"&quot;", L"\"");
    ReplaceAllW(s, L"&apos;", L"'");
    return s;
}

std::wstring ExtractPackageFamilyFromWindowsAppsPath(const std::wstring& exePath) {
    std::wstring lower = LowerStr(exePath);
    size_t waPos = lower.find(L"\\windowsapps\\");
    if (waPos == std::wstring::npos) return {};
    size_t start = waPos + 13; // len("\\windowsapps\\")
    size_t slash = lower.find(L'\\', start);
    if (slash == std::wstring::npos || slash <= start) return {};
    std::wstring pkgFull = lower.substr(start, slash - start);
    // Format: <Name>_<Version>_<Arch>_<ResourceId>_<PublisherId>
    size_t firstUnder = pkgFull.find(L'_');
    size_t lastUnder = pkgFull.rfind(L'_');
    if (firstUnder == std::wstring::npos || lastUnder == std::wstring::npos || lastUnder <= firstUnder) {
        return {};
    }
    return pkgFull.substr(0, firstUnder) + L"_" + pkgFull.substr(lastUnder + 1);
}

HICON HBitmapToIconClean(HBITMAP hBmp) {
    if (!hBmp) return nullptr;
    BITMAP bm{};
    if (GetObjectW(hBmp, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        return nullptr;
    }
    // Zero-initialized 1bpp mask ensures alpha comes cleanly from the 32bpp DIB color bitmap.
    size_t maskStride = static_cast<size_t>(((bm.bmWidth + 15) / 16) * 2);
    std::vector<BYTE> maskBits(maskStride * static_cast<size_t>(bm.bmHeight), 0);
    HBITMAP hMask = CreateBitmap(bm.bmWidth, bm.bmHeight, 1, 1, maskBits.data());
    if (!hMask) return nullptr;

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hBmp;
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hMask);
    return hIcon;
}

namespace {
struct PkgMeta {
    std::wstring pkgFull;
    std::wstring rootFolder;
};
} // namespace

static void CollectRepositoryPackages(HKEY hHive, const wchar_t* subKey,
                                      std::unordered_map<std::wstring, PkgMeta>& familyToMeta) {
    HKEY hRepo = nullptr;
    if (RegOpenKeyExW(hHive, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &hRepo) != ERROR_SUCCESS) return;

    wchar_t fullName[256];
    for (DWORD i = 0; ; ++i) {
        DWORD len = ARRAYSIZE(fullName);
        if (RegEnumKeyExW(hRepo, i, fullName, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring full(fullName, len);
        size_t firstU = full.find(L'_');
        size_t lastU = full.rfind(L'_');
        if (firstU == std::wstring::npos || lastU == std::wstring::npos || lastU <= firstU) continue;
        std::wstring familyLower = LowerStr(full.substr(0, firstU) + L"_" + full.substr(lastU + 1));
        if (familyToMeta.find(familyLower) != familyToMeta.end()) continue;

        HKEY hPkg = nullptr;
        if (RegOpenKeyExW(hRepo, fullName, 0, KEY_READ | KEY_WOW64_64KEY, &hPkg) == ERROR_SUCCESS) {
            wchar_t rootBuf[MAX_PATH * 2] = {};
            DWORD cb = sizeof(rootBuf);
            DWORD type = 0;
            if (RegQueryValueExW(hPkg, L"PackageRootFolder", nullptr, &type,
                                 reinterpret_cast<BYTE*>(rootBuf), &cb) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ) && rootBuf[0]) {
                familyToMeta[familyLower] = PkgMeta{full, rootBuf};
            }
            RegCloseKey(hPkg);
        }
    }
    RegCloseKey(hRepo);
}

static std::wstring ReadManifestPublisher(const PkgMeta& meta) {
    if (meta.rootFolder.empty()) return {};
    std::wstring xmlPath = meta.rootFolder + L"\\AppxManifest.xml";
    HANDLE hFile = CreateFileW(xmlPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    char buf[24576];
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!ok || bytesRead == 0) return {};
    buf[bytesRead] = '\0';

    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(bytesRead), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring xml(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(bytesRead), xml.data(), wlen);

    // 1) Try <PublisherDisplayName>...</PublisherDisplayName>
    std::wstring rawPub;
    size_t tagOpen = xml.find(L"<PublisherDisplayName>");
    if (tagOpen != std::wstring::npos) {
        tagOpen += 22;
        size_t tagClose = xml.find(L"</PublisherDisplayName>", tagOpen);
        if (tagClose != std::wstring::npos && tagClose > tagOpen) {
            rawPub = TrimW(DecodeXmlEntities(xml.substr(tagOpen, tagClose - tagOpen)));
        }
    }

    if (!rawPub.empty()) {
        if (_wcsnicmp(rawPub.c_str(), L"ms-resource:", 12) == 0) {
            wchar_t resolved[512] = {};
            std::wstring indirect = L"@{" + meta.pkgFull + L"?" + rawPub + L"}";
            if (SHLoadIndirectString(indirect.c_str(), resolved, ARRAYSIZE(resolved), nullptr) == S_OK && resolved[0]) {
                return TrimW(resolved);
            }
            // Fallback try under /resources/ subtree if relative ms-resource
            size_t firstU = meta.pkgFull.find(L'_');
            std::wstring pkgName = (firstU != std::wstring::npos) ? meta.pkgFull.substr(0, firstU) : meta.pkgFull;
            std::wstring keyOnly = rawPub.substr(12);
            while (!keyOnly.empty() && keyOnly.front() == L'/') keyOnly.erase(keyOnly.begin());
            indirect = L"@{" + meta.pkgFull + L"?ms-resource://" + pkgName + L"/resources/" + keyOnly + L"}";
            if (SHLoadIndirectString(indirect.c_str(), resolved, ARRAYSIZE(resolved), nullptr) == S_OK && resolved[0]) {
                return TrimW(resolved);
            }
            rawPub.clear(); // don't show raw ms-resource: URI to user
        } else {
            return rawPub;
        }
    }

    // 2) Fallback: parse CN= from <Identity ... Publisher="CN=..." ...>
    size_t idPos = xml.find(L"<Identity");
    if (idPos != std::wstring::npos) {
        size_t idEnd = xml.find(L'>', idPos);
        if (idEnd != std::wstring::npos) {
            std::wstring idTag = xml.substr(idPos, idEnd - idPos);
            size_t pubAttr = idTag.find(L"Publisher=\"");
            if (pubAttr != std::wstring::npos) {
                pubAttr += 11;
                size_t pubEnd = idTag.find(L'"', pubAttr);
                if (pubEnd != std::wstring::npos) {
                    std::wstring cert = DecodeXmlEntities(idTag.substr(pubAttr, pubEnd - pubAttr));
                    size_t cnPos = cert.find(L"CN=");
                    if (cnPos != std::wstring::npos) {
                        cnPos += 3;
                        size_t comma = cert.find(L',', cnPos);
                        std::wstring cn = TrimW(comma != std::wstring::npos
                                                    ? cert.substr(cnPos, comma - cnPos)
                                                    : cert.substr(cnPos));
                        // Filter out pure GUID CNs
                        if (!cn.empty() && cn.front() != L'{') return cn;
                    }
                }
            }
        }
    }
    return {};
}

std::vector<UwpAppEntry> EnumerateInstalledUwpApps(int iconSizePx) {
    std::vector<UwpAppEntry> result;

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comOwned = (hrCom == S_OK || hrCom == S_FALSE);

    std::unordered_map<std::wstring, PkgMeta> familyToMeta;
    CollectRepositoryPackages(
        HKEY_CURRENT_USER,
        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
        familyToMeta);
    CollectRepositoryPackages(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
        familyToMeta);

    IShellItem* pAppsFolder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&pAppsFolder))) &&
        pAppsFolder) {
        IEnumShellItems* pEnum = nullptr;
        if (SUCCEEDED(pAppsFolder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&pEnum))) && pEnum) {
            IShellItem* pItem = nullptr;
            while (pEnum->Next(1, &pItem, nullptr) == S_OK && pItem) {
                PWSTR pAumid = nullptr;
                PWSTR pDisp = nullptr;
                std::wstring aumid;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &pAumid)) && pAumid) {
                    aumid = pAumid;
                    CoTaskMemFree(pAumid);
                }

                // Store/UWP applications in AppsFolder always use <PackageFamilyName>!<AppId>.
                size_t bang = aumid.find(L'!');
                if (bang != std::wstring::npos && bang > 0) {
                    std::wstring dispName;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_NORMALDISPLAY, &pDisp)) && pDisp) {
                        dispName = pDisp;
                        CoTaskMemFree(pDisp);
                    }
                    if (!dispName.empty()) {
                        UwpAppEntry e;
                        e.aumid = aumid;
                        e.pkgFamily = LowerStr(aumid.substr(0, bang));
                        e.displayName = std::move(dispName);

                        auto metaIt = familyToMeta.find(e.pkgFamily);
                        if (metaIt != familyToMeta.end()) {
                            e.publisher = ReadManifestPublisher(metaIt->second);
                        }

                        IShellItemImageFactory* pImgFactory = nullptr;
                        if (SUCCEEDED(pItem->QueryInterface(IID_PPV_ARGS(&pImgFactory))) && pImgFactory) {
                            HBITMAP hBmp = nullptr;
                            SIZE sz{iconSizePx, iconSizePx};
                            if (SUCCEEDED(pImgFactory->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_SCALEUP, &hBmp)) && hBmp) {
                                e.icon = HBitmapToIconClean(hBmp);
                                DeleteObject(hBmp);
                            }
                            pImgFactory->Release();
                        }

                        result.push_back(std::move(e));
                    }
                }
                pItem->Release();
            }
            pEnum->Release();
        }
        pAppsFolder->Release();
    }

    if (comOwned) {
        CoUninitialize();
    }
    return result;
}

void FreeUwpAppEntries(std::vector<UwpAppEntry>& entries) {
    for (auto& e : entries) {
        if (e.icon) {
            DestroyIcon(e.icon);
            e.icon = nullptr;
        }
    }
    entries.clear();
}

} // namespace taskman
