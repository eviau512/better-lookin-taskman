#pragma once
#include <windows.h>
#include "settings.h"

namespace taskman {

struct Strings {
    const wchar_t* appTitle;
    const wchar_t* appTitleAdmin;

    // Menus
    const wchar_t* menuFile;
    const wchar_t* menuRunNewTask;
    const wchar_t* menuElevate;
    const wchar_t* menuExit;

    const wchar_t* menuOptions;
    const wchar_t* menuAlwaysOnTop;
    const wchar_t* menuMinimizeOnUse;
    const wchar_t* menuHideWhenMinimized;
    const wchar_t* menuShowFullAccount;
    const wchar_t* menuShowAllHistory;
    const wchar_t* menuTheme;
    const wchar_t* menuThemeSystem;
    const wchar_t* menuThemeLight;
    const wchar_t* menuThemeDark;
    const wchar_t* menuToggleTheme;
    const wchar_t* menuLanguage;
    const wchar_t* menuLangAuto;
    const wchar_t* menuLangEn;
    const wchar_t* menuLangZh;

    const wchar_t* menuView;
    const wchar_t* menuRefreshNow;
    const wchar_t* menuUpdateSpeed;
    const wchar_t* menuSpeedHigh;
    const wchar_t* menuSpeedNormal;
    const wchar_t* menuSpeedLow;
    const wchar_t* menuSpeedPaused;
    const wchar_t* menuGroupByType;
    const wchar_t* menuExpandAll;
    const wchar_t* menuCollapseAll;
    const wchar_t* menuShowKernelTimes;
    const wchar_t* menuSummaryView;

    const wchar_t* menuHelp;
    const wchar_t* menuHelpTopics;
    const wchar_t* menuAbout;

    // Tabs
    const wchar_t* tabProcesses;
    const wchar_t* tabPerformance;
    const wchar_t* tabAppHistory;
    const wchar_t* tabStartup;
    const wchar_t* tabUsers;
    const wchar_t* tabDetails;
    const wchar_t* tabServices;

    // Footer & Common Buttons
    const wchar_t* btnFewerDetails;
    const wchar_t* btnMoreDetails;
    const wchar_t* btnEndTask;
    const wchar_t* btnEnable;
    const wchar_t* btnDisable;
    const wchar_t* btnDisconnect;
    const wchar_t* btnOpenServices;
    const wchar_t* btnOpenResMon;
    const wchar_t* btnDeleteHistory;
    const wchar_t* themeTipLight;
    const wchar_t* themeTipDark;
    const wchar_t* noAppsRunning;

    // Processes Tab
    const wchar_t* colName;
    const wchar_t* colStatus;
    const wchar_t* colCpu;
    const wchar_t* colMemory;
    const wchar_t* colDisk;
    const wchar_t* colNetwork;
    const wchar_t* colGpu;
    const wchar_t* colGpuEngine;
    const wchar_t* colPowerUsage;
    const wchar_t* colPowerTrend;

    const wchar_t* grpApps;
    const wchar_t* grpBackground;
    const wchar_t* grpWindows;
    const wchar_t* statusSuspended;

    const wchar_t* powerVeryLow;
    const wchar_t* powerLow;
    const wchar_t* powerModerate;
    const wchar_t* powerHigh;
    const wchar_t* powerVeryHigh;

    // Context Menus
    const wchar_t* ctxExpand;
    const wchar_t* ctxCollapse;
    const wchar_t* ctxEndTask;
    const wchar_t* ctxEndTree;
    const wchar_t* ctxRestart;
    const wchar_t* ctxResourceValues;
    const wchar_t* ctxResMemory;
    const wchar_t* ctxResDisk;
    const wchar_t* ctxResNetwork;
    const wchar_t* ctxValues;
    const wchar_t* ctxPercents;
    const wchar_t* ctxCreateDump;
    const wchar_t* ctxSwitchTo;
    const wchar_t* ctxGoToDetails;
    const wchar_t* ctxGoToService;
    const wchar_t* ctxOpenLocation;
    const wchar_t* ctxSearchOnline;
    const wchar_t* ctxProperties;
    const wchar_t* ctxSetPriority;
    const wchar_t* ctxPrioRealtime;
    const wchar_t* ctxPrioHigh;
    const wchar_t* ctxPrioAboveNormal;
    const wchar_t* ctxPrioNormal;
    const wchar_t* ctxPrioBelowNormal;
    const wchar_t* ctxPrioLow;
    const wchar_t* ctxSetAffinity;
    const wchar_t* ctxSelectColumns;

    // Performance Tab
    const wchar_t* perfCpu;
    const wchar_t* perfMemory;
    const wchar_t* perfDisk;
    const wchar_t* perfEthernet;
    const wchar_t* perfWifi;
    const wchar_t* perfGpu;
    const wchar_t* perfUtilization;
    const wchar_t* perfUtilization30s;
    const wchar_t* perfUtilization60s;
    const wchar_t* perfUtilization4m;
    const wchar_t* perf100Pct;
    const wchar_t* perf30Sec;
    const wchar_t* perf60Sec;
    const wchar_t* perf4Min;
    const wchar_t* perfSpeed;
    const wchar_t* perfProcesses;
    const wchar_t* perfThreads;
    const wchar_t* perfHandles;
    const wchar_t* perfUptime;
    const wchar_t* perfBaseSpeed;
    const wchar_t* perfSockets;
    const wchar_t* perfCores;
    const wchar_t* perfLogicalProcs;
    const wchar_t* perfVirtualization;
    const wchar_t* perfEnabled;
    const wchar_t* perfDisabled;
    const wchar_t* perfL1Cache;
    const wchar_t* perfL2Cache;
    const wchar_t* perfL3Cache;
    const wchar_t* perfChangeGraphTo;
    const wchar_t* perfOverallUtil;
    const wchar_t* perfLogicalView;
    const wchar_t* perfShowKernelTimes;
    const wchar_t* perfGraphSummaryView;
    const wchar_t* perfCopy;

    const wchar_t* perfMemUsage;
    const wchar_t* perfMemComposition;
    const wchar_t* perfInUse;
    const wchar_t* perfAvailable;
    const wchar_t* perfCommitted;
    const wchar_t* perfCached;
    const wchar_t* perfPagedPool;
    const wchar_t* perfNonPagedPool;
    const wchar_t* perfSlotsUsed;
    const wchar_t* perfFormFactor;
    const wchar_t* perfHwReserved;

    const wchar_t* perfActiveTime;
    const wchar_t* perfDiskTransferRate;
    const wchar_t* perfAvgResponse;
    const wchar_t* perfReadSpeed;
    const wchar_t* perfWriteSpeed;
    const wchar_t* perfCapacity;
    const wchar_t* perfFormatted;
    const wchar_t* perfSystemDisk;
    const wchar_t* perfPageFile;
    const wchar_t* perfType;
    const wchar_t* perfYes;
    const wchar_t* perfNo;

    const wchar_t* perfThroughput;
    const wchar_t* perfSend;
    const wchar_t* perfReceive;
    const wchar_t* perfAdapterName;
    const wchar_t* perfDnsName;
    const wchar_t* perfConnType;
    const wchar_t* perfIpv4;
    const wchar_t* perfIpv6;

    const wchar_t* perfDedGpuMem;
    const wchar_t* perfSharedGpuMem;
    const wchar_t* perfGpuMemory;
    const wchar_t* perfDriverVer;
    const wchar_t* perfDriverDate;
    const wchar_t* perfDxVersion;
    const wchar_t* perfPhysLocation;
    const wchar_t* perfGpuTemp;

    // App History Tab
    const wchar_t* colCpuTime;
    const wchar_t* colMeteredNet;
    const wchar_t* colTileUpdates;
    const wchar_t* appHistoryNote;

    // Startup Tab
    const wchar_t* colPublisher;
    const wchar_t* colStartupImpact;
    const wchar_t* statusEnabled;
    const wchar_t* statusDisabled;
    const wchar_t* impactHigh;
    const wchar_t* impactMedium;
    const wchar_t* impactLow;
    const wchar_t* impactNotMeasured;

    // Users Tab
    const wchar_t* colUser;
    const wchar_t* stateActive;
    const wchar_t* stateDisconnected;
    const wchar_t* ctxDisconnect;
    const wchar_t* ctxLogoff;

    // Details Tab
    const wchar_t* colPid;
    const wchar_t* colUserName;
    const wchar_t* colWorkingSet;
    const wchar_t* colCommitSize;
    const wchar_t* colThreads;
    const wchar_t* colHandles;
    const wchar_t* colSessionId;
    const wchar_t* colPlatform;
    const wchar_t* colElevated;
    const wchar_t* colUacVirt;
    const wchar_t* colCommandLine;
    const wchar_t* colDescription;
    const wchar_t* statusRunning;

    // Services Tab
    const wchar_t* colGroup;
    const wchar_t* svcStart;
    const wchar_t* svcStop;
    const wchar_t* svcRestart;
    const wchar_t* svcOpenServices;
    const wchar_t* svcStopped;

    // Dialogs & Confirmations
    const wchar_t* dlgRunTitle;
    const wchar_t* dlgRunDesc;
    const wchar_t* dlgRunOpen;
    const wchar_t* dlgRunAdmin;
    const wchar_t* dlgRunAdminHint;
    const wchar_t* dlgBrowse;
    const wchar_t* dlgOk;
    const wchar_t* dlgCancel;
    const wchar_t* dlgAffinityTitle;
    const wchar_t* dlgAffinityDesc;
    const wchar_t* dlgAllProcessors;
    const wchar_t* dlgSelectColsTitle;
    const wchar_t* dlgSelectColsDesc;
    const wchar_t* dlgAboutTitle;
    const wchar_t* dlgAboutBody;

    const wchar_t* msgConfirmEndTree;
    const wchar_t* msgConfirmLogoff;
    const wchar_t* msgConfirmDisconnect;
    const wchar_t* msgAccessDenied;
    const wchar_t* msgDumpSaved;
    const wchar_t* msgDumpFailed;
    const wchar_t* msgRealtimeWarn;
    const wchar_t* trayRestore;
};

inline const Strings& GetStrings(LangMode mode) {
    static const Strings kEn = {
        L"Task Manager",
        L"Task Manager (Administrator)",

        L"&File",
        L"Run &new task\tAlt+N",
        L"Restart as &administrator",
        L"E&xit",

        L"&Options",
        L"&Always on top",
        L"&Minimize on use",
        L"&Hide when minimized",
        L"Show &full account name",
        L"Show history for &all processes",
        L"&Theme",
        L"Follow &system",
        L"&Light mode",
        L"&Dark mode",
        L"Toggle Light/Dark\tCtrl+Shift+D",
        L"&Language",
        L"&Auto (System)",
        L"&English",
        L"简体中文 (&Chinese)",

        L"&View",
        L"&Refresh now\tF5",
        L"Display &time span",
        L"&30 seconds (High)",
        L"&60 seconds (Normal)",
        L"&4 minutes (Low)",
        L"&Paused",
        L"&Group by type",
        L"&Expand all",
        L"&Collapse all",
        L"Show &kernel times",
        L"Graph &summary view",

        L"&Help",
        L"Task Manager &Help",
        L"&About",

        L"Processes",
        L"Performance",
        L"App history",
        L"Startup",
        L"Users",
        L"Details",
        L"Services",

        L"Fewer &details",
        L"More &details",
        L"&End task",
        L"&Enable",
        L"&Disable",
        L"&Disconnect",
        L"&Open Services",
        L"&Open Resource Monitor",
        L"Delete usage history",
        L"Light mode (Ctrl+Shift+D)",
        L"Dark mode (Ctrl+Shift+D)",
        L"There are no running apps.",

        L"Name",
        L"Status",
        L"CPU",
        L"Memory",
        L"Disk",
        L"Network",
        L"GPU",
        L"GPU engine",
        L"Power usage",
        L"Power usage trend",

        L"Apps",
        L"Background processes",
        L"Windows processes",
        L"Suspended",

        L"Very low",
        L"Low",
        L"Moderate",
        L"High",
        L"Very high",

        L"E&xpand",
        L"C&ollapse",
        L"&End task",
        L"End process &tree",
        L"&Restart",
        L"Resource &values",
        L"&Memory",
        L"&Disk",
        L"&Network",
        L"&Values",
        L"&Percents",
        L"Create &dump file",
        L"&Switch to",
        L"&Go to details",
        L"Go to &service(s)",
        L"&Open file location",
        L"&Search online",
        L"P&roperties",
        L"Set priority",
        L"Realtime",
        L"High",
        L"Above normal",
        L"Normal",
        L"Below normal",
        L"Low",
        L"Set affinity",
        L"Select columns",

        L"CPU",
        L"Memory",
        L"Disk",
        L"Ethernet",
        L"Wi-Fi",
        L"GPU",
        L"Utilization",
        L"% Utilization over 30 seconds",
        L"% Utilization over 60 seconds",
        L"% Utilization over 4 minutes",
        L"100%",
        L"30 seconds",
        L"60 seconds",
        L"4 minutes",
        L"Speed",
        L"Processes",
        L"Threads",
        L"Handles",
        L"Up time",
        L"Base speed",
        L"Sockets",
        L"Cores",
        L"Logical processors",
        L"Virtualization",
        L"Enabled",
        L"Disabled",
        L"L1 cache",
        L"L2 cache",
        L"L3 cache",
        L"Change graph to",
        L"Overall utilization",
        L"Logical processors",
        L"Show kernel times",
        L"Graph summary view",
        L"Copy",

        L"Memory usage",
        L"Memory composition",
        L"In use (Compressed)",
        L"Available",
        L"Committed",
        L"Cached",
        L"Paged pool",
        L"Non-paged pool",
        L"Slots used",
        L"Form factor",
        L"Hardware reserved",

        L"Active time",
        L"Disk transfer rate",
        L"Average response time",
        L"Read speed",
        L"Write speed",
        L"Capacity",
        L"Formatted",
        L"System disk",
        L"Page file",
        L"Type",
        L"Yes",
        L"No",

        L"Throughput",
        L"Send",
        L"Receive",
        L"Adapter name",
        L"DNS name",
        L"Connection type",
        L"IPv4 address",
        L"IPv6 address",

        L"Dedicated GPU memory",
        L"Shared GPU memory",
        L"GPU Memory",
        L"Driver version",
        L"Driver date",
        L"DirectX version",
        L"Physical location",
        L"GPU Temperature",

        L"CPU time",
        L"Metered network",
        L"Tile updates",
        L"Resource usage history is tracked while Task Manager is active or via Windows SRUM.",

        L"Publisher",
        L"Startup impact",
        L"Enabled",
        L"Disabled",
        L"High",
        L"Medium",
        L"Low",
        L"Not measured",

        L"User",
        L"Active",
        L"Disconnected",
        L"Disconnect",
        L"Sign off",

        L"PID",
        L"User name",
        L"Working set (memory)",
        L"Commit size",
        L"Threads",
        L"Handles",
        L"Session ID",
        L"Platform",
        L"Elevated",
        L"UAC virtualization",
        L"Command line",
        L"Description",
        L"Running",

        L"Group",
        L"Start",
        L"Stop",
        L"Restart",
        L"Open Services",
        L"Stopped",

        L"Create new task",
        L"Type the name of a program, folder, document, or Internet resource, and Windows will open it for you.",
        L"&Open:",
        L"Create this task with administrative privileges.",
        L"Task Manager will elevate via UAC when creating this task.",
        L"&Browse...",
        L"OK",
        L"Cancel",
        L"Processor affinity",
        L"Which processors are allowed to run this process?",
        L"<All Processors>",
        L"Select columns",
        L"Select the columns that will appear on the Details tab.",
        L"About Task Manager",
        L"Better Lookin Win10 Task Manager\nVersion %ls (Native Win32 C++)\n\nby Gemini 4\ntest by eviau512",

        L"Do you want to end the process tree?\nEnding a process tree kills the process and all processes started by it.",
        L"Do you want to sign off the selected user session? Unsaved data may be lost.",
        L"Do you want to disconnect the selected user session?",
        L"Access is denied. Try restarting Task Manager as administrator (File -> Restart as administrator).",
        L"The dump file was created successfully:\n",
        L"Failed to create dump file. Make sure you have sufficient privileges.",
        L"Changing the priority to Realtime may cause system instability. Continue?",
        L"Restore Task Manager"
    };

    static const Strings kZh = {
        L"任务管理器",
        L"任务管理器 (管理员)",

        L"文件(&F)",
        L"运行新任务(&N)\tAlt+N",
        L"以管理员身份重启(&A)",
        L"退出(&X)",

        L"选项(&O)",
        L"前端显示(&A)",
        L"使用时最小化(&M)",
        L"最小化时隐藏(&H)",
        L"显示完整账户名(&F)",
        L"显示所有进程的历史记录(&S)",
        L"主题(&T)",
        L"跟随系统(&S)",
        L"浅色模式(&L)",
        L"深色模式(&D)",
        L"黑白主题切换\tCtrl+Shift+D",
        L"语言(&L)",
        L"自动 (跟随系统)",
        L"English",
        L"简体中文",

        L"查看(&V)",
        L"立即刷新(&R)\tF5",
        L"显示时间段(&U)",
        L"30 秒 - 高(&H)",
        L"60 秒 - 正常(&N)",
        L"4 分钟 - 低(&L)",
        L"已暂停(&P)",
        L"按类型分组(&G)",
        L"全部展开(&E)",
        L"全部折叠(&C)",
        L"显示内核时间(&K)",
        L"图表摘要视图(&S)",

        L"帮助(&H)",
        L"任务管理器帮助(&H)",
        L"关于(&A)",

        L"进程",
        L"性能",
        L"应用历史记录",
        L"启动",
        L"用户",
        L"详细信息",
        L"服务",

        L"简略信息(&D)",
        L"详细信息(&D)",
        L"结束任务(&E)",
        L"启用(&E)",
        L"禁用(&D)",
        L"断开连接(&D)",
        L"打开服务(&O)",
        L"打开资源监视器(&O)",
        L"删除使用历史记录",
        L"浅色模式 (Ctrl+Shift+D)",
        L"深色模式 (Ctrl+Shift+D)",
        L"没有正在运行的应用。",

        L"名称",
        L"状态",
        L"CPU",
        L"内存",
        L"磁盘",
        L"网络",
        L"GPU",
        L"GPU 引擎",
        L"电源使用情况",
        L"电源使用情况趋势",

        L"应用",
        L"后台进程",
        L"Windows 进程",
        L"已挂起",

        L"非常低",
        L"低",
        L"中",
        L"高",
        L"非常高",

        L"展开(&P)",
        L"折叠(&O)",
        L"结束任务(&E)",
        L"结束进程树(&T)",
        L"重新启动(&R)",
        L"资源值(&V)",
        L"内存(&M)",
        L"磁盘(&D)",
        L"网络(&N)",
        L"值(&V)",
        L"百分比(&P)",
        L"创建转储文件(&C)",
        L"切换到(&S)",
        L"转到详细信息(&G)",
        L"转到服务(&S)",
        L"打开文件所在的位置(&O)",
        L"联机搜索(&S)",
        L"属性(&I)",
        L"设置优先级",
        L"实时",
        L"高",
        L"高于正常",
        L"正常",
        L"低于正常",
        L"低",
        L"设置相关性",
        L"选择列",

        L"CPU",
        L"内存",
        L"磁盘",
        L"以太网",
        L"WLAN",
        L"GPU",
        L"利用率",
        L"30 秒内的利用率 %",
        L"60 秒内的利用率 %",
        L"4 分钟内的利用率 %",
        L"100%",
        L"30 秒",
        L"60 秒",
        L"4 分钟",
        L"速度",
        L"进程",
        L"线程",
        L"句柄",
        L"正常运行时间",
        L"基准速度",
        L"插槽",
        L"内核",
        L"逻辑处理器",
        L"虚拟化",
        L"已启用",
        L"已禁用",
        L"L1 缓存",
        L"L2 缓存",
        L"L3 缓存",
        L"将图形更改为",
        L"总体利用率",
        L"逻辑处理器",
        L"显示内核时间",
        L"图表摘要视图",
        L"复制",

        L"内存使用量",
        L"内存组合",
        L"使用中(已压缩)",
        L"可用",
        L"已提交",
        L"已缓存",
        L"分页缓冲池",
        L"非分页缓冲池",
        L"已使用的插槽",
        L"外形规格",
        L"为硬件保留的内存",

        L"活动时间",
        L"磁盘传输速率",
        L"平均响应时间",
        L"读取速度",
        L"写入速度",
        L"容量",
        L"已格式化",
        L"系统磁盘",
        L"页面文件",
        L"类型",
        L"是",
        L"否",

        L"吞吐量",
        L"发送",
        L"接收",
        L"适配器名称",
        L"DNS 名称",
        L"连接类型",
        L"IPv4 地址",
        L"IPv6 地址",

        L"专用 GPU 内存",
        L"共享 GPU 内存",
        L"GPU 内存",
        L"驱动程序版本",
        L"驱动程序日期",
        L"DirectX 版本",
        L"物理位置",
        L"GPU 温度",

        L"CPU 时间",
        L"按流量计费的网络",
        L"磁贴更新",
        L"资源使用历史记录在任务管理器运行期间实时累计，或从 Windows SRUM 读取。",

        L"发布者",
        L"启动影响",
        L"已启用",
        L"已禁用",
        L"高",
        L"中",
        L"低",
        L"未计量",

        L"用户",
        L"活动",
        L"已断开连接",
        L"断开连接",
        L"注销",

        L"PID",
        L"用户名",
        L"工作集(内存)",
        L"提交大小",
        L"线程",
        L"句柄",
        L"会话 ID",
        L"平台",
        L"已提升",
        L"UAC 虚拟化",
        L"命令行",
        L"描述",
        L"正在运行",

        L"组",
        L"开始",
        L"停止",
        L"重新启动",
        L"打开服务",
        L"已停止",

        L"新建任务",
        L"键入程序、文件夹、文档或 Internet 资源的名称，Windows 将为您打开它。",
        L"打开(&O):",
        L"以系统管理权限创建此任务。",
        L"创建此任务时将通过 UAC 申请管理员权限。",
        L"浏览(&B)...",
        L"确定",
        L"取消",
        L"处理器相关性",
        L"允许哪些处理器运行此进程？",
        L"<所有处理器>",
        L"选择列",
        L"选择将显示在“详细信息”选项卡上的列。",
        L"关于任务管理器",
        L"Win10 经典任务管理器（原生黑白双色版）\n版本 %ls (纯原生 Win32 C++ 单文件)\n\nby Gemini 4\ntest by eviau512",

        L"是否确实要结束该进程树？\n结束进程树将同时终止此进程及其启动的所有子进程。",
        L"是否确实要注销所选用户会话？未保存的数据将会丢失。",
        L"是否确实要断开所选用户会话？",
        L"拒绝访问。请尝试在“文件”菜单中点击“以管理员身份重启”。",
        L"转储文件已成功创建：\n",
        L"创建转储文件失败，请确认当前拥有足够权限。",
        L"将进程优先级更改为“实时”可能会导致系统不稳定。是否继续？",
        L"还原任务管理器"
    };

    if (mode == LangMode::Chinese) return kZh;
    if (mode == LangMode::English) return kEn;
    LANGID lang = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(lang) == LANG_CHINESE) ? kZh : kEn;
}

inline LangMode GetEffectiveLang(LangMode mode) {
    if (mode != LangMode::Auto) return mode;
    LANGID lang = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(lang) == LANG_CHINESE) ? LangMode::Chinese : LangMode::English;
}

} // namespace taskman
