#include "user_manager.h"
#include <wtsapi32.h>
#include <sddl.h>
#include <shlobj.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cwctype>
#include <memory>
#include <vector>

namespace taskman {

static std::wstring LowerStr(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

static bool FileExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring FindUserAvatarImagePath(const std::wstring& domain, const std::wstring& userName) {
    // 1) Resolve user SID -> HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AccountPicture\Users\<SID>
    auto lookupSidString = [](const std::wstring& account) -> std::wstring {
        if (account.empty()) return {};
        DWORD sidSz = 0, domSz = 0;
        SID_NAME_USE use = SidTypeUnknown;
        LookupAccountNameW(nullptr, account.c_str(), nullptr, &sidSz, nullptr, &domSz, &use);
        if (sidSz == 0) return {};
        std::vector<BYTE> sidBuf(sidSz);
        std::vector<wchar_t> domBuf(domSz + 1, L'\0');
        if (!LookupAccountNameW(nullptr, account.c_str(), sidBuf.data(), &sidSz,
                                domBuf.data(), &domSz, &use)) {
            return {};
        }
        LPWSTR sidStr = nullptr;
        if (!ConvertSidToStringSidW(sidBuf.data(), &sidStr) || !sidStr) return {};
        std::wstring out = sidStr;
        LocalFree(sidStr);
        return out;
    };

    std::wstring sidStr;
    if (!domain.empty()) {
        sidStr = lookupSidString(domain + L"\\" + userName);
    }
    if (sidStr.empty()) {
        sidStr = lookupSidString(userName);
    }

    if (!sidStr.empty()) {
        std::wstring keyPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\" + sidStr;
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            static const wchar_t* kVals[] = {L"Image48", L"Image40", L"Image64", L"Image32", L"Image96", L"Image192"};
            for (const wchar_t* valName : kVals) {
                wchar_t pathBuf[MAX_PATH * 2] = {};
                DWORD cb = sizeof(pathBuf);
                DWORD type = 0;
                if (RegQueryValueExW(hKey, valName, nullptr, &type,
                                     reinterpret_cast<BYTE*>(pathBuf), &cb) == ERROR_SUCCESS &&
                    (type == REG_SZ || type == REG_EXPAND_SZ) && pathBuf[0]) {
                    wchar_t expBuf[MAX_PATH * 2] = {};
                    ExpandEnvironmentStringsW(pathBuf, expBuf, ARRAYSIZE(expBuf));
                    if (FileExistsW(expBuf)) {
                        RegCloseKey(hKey);
                        return expBuf;
                    }
                }
            }
            RegCloseKey(hKey);
        }
    }

    // 2) Fallback: ProgramData default user account pictures
    wchar_t progData[MAX_PATH] = {};
    if (SHGetSpecialFolderPathW(nullptr, progData, CSIDL_COMMON_APPDATA, FALSE)) {
        std::wstring base = std::wstring(progData) + L"\\Microsoft\\User Account Pictures\\";
        static const wchar_t* kDefaults[] = {L"user-48.png", L"user-40.png", L"user-32.png", L"user.png", L"user.bmp"};
        for (const wchar_t* fn : kDefaults) {
            std::wstring cand = base + fn;
            if (FileExistsW(cand)) return cand;
        }
    }
    return {};
}

static HICON CreateCircularAvatarIcon(const std::wstring& imagePath) {
    constexpr int kSize = 32;
    Gdiplus::Bitmap outBmp(kSize, kSize, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&outBmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::RectF rcCircle(1.0f, 1.0f, static_cast<float>(kSize - 2), static_cast<float>(kSize - 2));
    Gdiplus::GraphicsPath clipPath;
    clipPath.AddEllipse(rcCircle);

    bool drewImage = false;
    if (!imagePath.empty()) {
        std::unique_ptr<Gdiplus::Bitmap> src(Gdiplus::Bitmap::FromFile(imagePath.c_str()));
        if (src && src->GetLastStatus() == Gdiplus::Ok && src->GetWidth() > 0 && src->GetHeight() > 0) {
            UINT w = src->GetWidth();
            UINT h = src->GetHeight();
            UINT side = (w < h) ? w : h;
            INT sx = static_cast<INT>((w - side) / 2);
            INT sy = static_cast<INT>((h - side) / 2);

            g.SetClip(&clipPath);
            g.DrawImage(src.get(), rcCircle,
                        static_cast<Gdiplus::REAL>(sx), static_cast<Gdiplus::REAL>(sy),
                        static_cast<Gdiplus::REAL>(side), static_cast<Gdiplus::REAL>(side),
                        Gdiplus::UnitPixel);
            g.ResetClip();
            drewImage = true;
        }
    }

    if (!drewImage) {
        // Crisp circular silhouette fallback if account picture file cannot be read
        Gdiplus::SolidBrush brCircle(Gdiplus::Color(255, 96, 108, 124));
        g.FillEllipse(&brCircle, rcCircle);

        Gdiplus::SolidBrush brFig(Gdiplus::Color(235, 240, 244, 248));
        g.SetClip(&clipPath);
        g.FillEllipse(&brFig, Gdiplus::RectF(10.5f, 7.0f, 11.0f, 11.0f));
        g.FillEllipse(&brFig, Gdiplus::RectF(5.5f, 19.5f, 21.0f, 16.0f));
        g.ResetClip();
    }

    Gdiplus::Pen borderPen(Gdiplus::Color(70, 150, 155, 165), 1.0f);
    g.DrawEllipse(&borderPen, rcCircle);

    HICON hIcon = nullptr;
    outBmp.GetHICON(&hIcon);
    return hIcon;
}

UserManager::~UserManager() {
    for (auto& kv : avatarCache_) {
        if (kv.second) DestroyIcon(kv.second);
    }
    avatarCache_.clear();
}

HICON UserManager::GetOrLoadAvatar(const std::wstring& domain, const std::wstring& userName) {
    std::wstring cacheKey = LowerStr((!domain.empty() ? domain + L"\\" : L"") + userName);
    auto it = avatarCache_.find(cacheKey);
    if (it != avatarCache_.end()) return it->second;

    std::wstring imgPath = FindUserAvatarImagePath(domain, userName);
    HICON hIco = CreateCircularAvatarIcon(imgPath);
    avatarCache_[cacheKey] = hIco;
    return hIco;
}

void UserManager::Refresh(const std::vector<ProcessInfo>& procs, bool showFullAccountName) {
    sessions_.clear();

    PWTS_SESSION_INFOW pSess = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSess, &count)) return;

    std::unordered_map<DWORD, size_t> sidToIdx;
    for (DWORD i = 0; i < count; ++i) {
        if (pSess[i].State != WTSActive && pSess[i].State != WTSDisconnected) continue;

        LPWSTR userBuf = nullptr, domBuf = nullptr;
        DWORD userLen = 0, domLen = 0;
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, pSess[i].SessionId, WTSUserName, &userBuf, &userLen);
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, pSess[i].SessionId, WTSDomainName, &domBuf, &domLen);

        std::wstring user = (userBuf && userBuf[0]) ? userBuf : L"";
        std::wstring dom = (domBuf && domBuf[0]) ? domBuf : L"";
        if (userBuf) WTSFreeMemory(userBuf);
        if (domBuf) WTSFreeMemory(domBuf);
        if (user.empty()) continue;

        UserSessionInfo ui;
        ui.sessionId = pSess[i].SessionId;
        ui.userName = user;
        ui.domain = dom;
        ui.fullUser = (showFullAccountName && !dom.empty()) ? (dom + L"\\" + user) : user;
        ui.active = (pSess[i].State == WTSActive);
        ui.avatarIcon = GetOrLoadAvatar(dom, user);

        sidToIdx[ui.sessionId] = sessions_.size();
        sessions_.push_back(std::move(ui));
    }
    WTSFreeMemory(pSess);

    for (const auto& p : procs) {
        auto it = sidToIdx.find(p.sessionId);
        if (it == sidToIdx.end()) continue;
        UserSessionInfo& s = sessions_[it->second];
        s.cpuPercent += p.cpuPercent;
        s.memoryBytes += p.privateWorkingSet;
        s.diskBytesPerSec += p.diskBytesPerSec;
        s.netBytesPerSec += p.netBytesPerSec;
        s.processes.push_back(&p);
    }
}

bool UserManager::DisconnectSession(DWORD sessionId) {
    return WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, sessionId, FALSE) != FALSE;
}

bool UserManager::LogoffSession(DWORD sessionId) {
    return WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, sessionId, FALSE) != FALSE;
}

} // namespace taskman
