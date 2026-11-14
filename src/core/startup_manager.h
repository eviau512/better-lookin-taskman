#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace taskman {

struct StartupItem {
    std::wstring name;          // Display name / registry value name or filename
    std::wstring valueName;     // Exact registry value or file name in startup folder
    std::wstring command;       // Raw command line or shortcut path
    std::wstring exePath;       // Resolved target exe path
    std::wstring publisher;     // Authenticode signer or CompanyName
    bool enabled = true;
    int impact = 0;             // 0=Not measured, 1=Low, 2=Medium, 3=High
    HICON icon = nullptr;

    HKEY rootKey = nullptr;     // HKCU or HKLM
    std::wstring approvedSubKey;
    bool isFolderItem = false;
    bool isStoreTask = false;
    std::wstring uwpTaskSubKey; // HKCU\...\AppModel\SystemAppData\<pkgFamily>\<taskId>
};

class StartupManager {
public:
    StartupManager() = default;
    ~StartupManager();

    void Refresh();
    const std::vector<StartupItem>& Items() const { return items_; }

    bool SetEnabled(size_t index, bool enable);

private:
    std::wstring GetPublisher(const std::wstring& exePath);
    std::wstring GetFileDescription(const std::wstring& exePath);
    std::wstring ResolveShortcutTarget(const std::wstring& lnkPath);
    std::wstring ResolveCommandExe(const std::wstring& cmdLine);
    bool ReadApprovedState(HKEY root, const std::wstring& approvedSubKey, const std::wstring& valueName);

    std::vector<StartupItem> items_;
    std::vector<HICON> ownedIcons_;
};

} // namespace taskman
