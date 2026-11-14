#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace taskman {

struct UwpAppEntry {
    std::wstring pkgFamily;   // lowercase PackageFamilyName (portion before '!')
    std::wstring aumid;       // <PackageFamilyName>!<AppId>
    std::wstring displayName; // localized Store app display name from AppsFolder
    std::wstring publisher;   // localized PublisherDisplayName from AppxManifest.xml
    HICON icon = nullptr;     // owned icon handle
};

// Converts a 32bpp/color HBITMAP into an owned HICON using an explicit zero-filled 1bpp mask.
HICON HBitmapToIconClean(HBITMAP hBmp);

// Enumerates installed Store / UWP applications via FOLDERID_AppsFolder + AppModel publisher lookup.
std::vector<UwpAppEntry> EnumerateInstalledUwpApps(int iconSizePx = 24);
void FreeUwpAppEntries(std::vector<UwpAppEntry>& entries);

// Extracts lowercase PackageFamilyName (<Name>_<PubId>) from a path inside \WindowsApps\<PackageFullName>\...
std::wstring ExtractPackageFamilyFromWindowsAppsPath(const std::wstring& exePath);

} // namespace taskman
