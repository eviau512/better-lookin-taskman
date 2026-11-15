#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace taskman {

enum class LockTargetType : int {
    Unknown   = 0,
    File      = 1,
    Directory = 2,
    Drive     = 3
};

enum class ProcessCriticalLevel : int {
    Normal        = 0,
    SystemService = 1,
    CriticalCore  = 2
};

enum LockSourceFlags : uint32_t {
    LOCK_SRC_NONE            = 0,
    LOCK_SRC_HANDLE          = 1u << 0,
    LOCK_SRC_MAPPED_MODULE   = 1u << 1,
    LOCK_SRC_RESTART_MANAGER = 1u << 2,
    LOCK_SRC_PROTECTED_CORE  = 1u << 3
};

struct LockEntry {
    DWORD pid = 0;
    std::wstring processName;
    std::wstring imagePath;
    HICON icon = nullptr; // owned by shared icon cache, do not destroy

    ProcessCriticalLevel criticalLevel = ProcessCriticalLevel::Normal;
    bool isBreakOnTermination = false;
    bool isProtectedProcess = false;
    bool canTerminate = true;

    uint32_t sourceMask = LOCK_SRC_NONE;
    int handleCount = 0;
    std::wstring matchedPath;
};

struct LockCheckPermissionInfo {
    bool exists = false;
    LockTargetType targetType = LockTargetType::Unknown;
    std::wstring rawInput;
    std::wstring normalizedPath;
    DWORD attributes = 0;
    uint64_t sizeBytes = 0;
    bool isReadOnly = false;
    bool isSystemAttr = false;

    std::wstring ownerName;
    bool ownerIsTrustedInstaller = false;
    bool ownerIsSystem = false;

    bool isCurrentElevated = false;
    bool aclChecked = false;
    DWORD aclError = 0;
    bool hasReadAccess = false;
    bool hasWriteAccess = false;
    bool hasDeleteAccess = false;
    bool hasParentDeleteChild = false;
};

struct LockCheckResult {
    LockCheckPermissionInfo target;
    std::vector<LockEntry> entries;
    int criticalCount = 0;
    int systemServiceCount = 0;
    int deniedProcessOpenCount = 0;
    ULONGLONG elapsedMs = 0;
};

class LockChecker {
public:
    using IconResolver = std::function<HICON(const std::wstring& imagePath)>;

    static std::wstring NormalizeTargetPath(const std::wstring& input, LockTargetType& outType, bool& outExists);
    static LockCheckPermissionInfo InspectPermissions(const std::wstring& inputPath);
    static LockCheckResult Analyze(const std::wstring& inputPath, const IconResolver& iconResolver = nullptr);
    static std::vector<std::wstring> EnumerateDriveRoots();
};

} // namespace taskman
