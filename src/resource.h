#pragma once

// Icons
#define IDI_APP_ICON                101

// Main Menu & Accelerators
#define IDR_MAIN_MENU               102
#define IDR_ACCEL                   103

// File menu commands
#define IDM_FILE_NEW_TASK           40001
#define IDM_FILE_ELEVATE            40002
#define IDM_FILE_EXIT               40003

// Options menu commands
#define IDM_OPT_ALWAYS_ON_TOP       40101
#define IDM_OPT_MINIMIZE_ON_USE     40102
#define IDM_OPT_HIDE_WHEN_MINIMIZED 40103
#define IDM_OPT_SHOW_FULL_ACCOUNT   40104
#define IDM_OPT_SHOW_ALL_HISTORY    40105
#define IDM_OPT_THEME_LIGHT         40106
#define IDM_OPT_THEME_DARK          40107
#define IDM_OPT_THEME_SYSTEM        40108
#define IDM_OPT_TOGGLE_THEME        40109
#define IDM_OPT_LANG_AUTO           40110
#define IDM_OPT_LANG_EN             40111
#define IDM_OPT_LANG_ZH             40112
#define IDM_OPT_PRELAUNCH_SAMPLING  40113
#define IDM_OPT_LANG_ZHTW           40114
#define IDM_OPT_LANG_JA             40115
#define IDM_OPT_LANG_KO             40116
#define IDM_OPT_LANG_DE             40117
#define IDM_OPT_LANG_FR             40118
#define IDM_OPT_LANG_ES             40119
#define IDM_OPT_LANG_RU             40120

// View menu commands
#define IDM_VIEW_REFRESH_NOW        40201
#define IDM_VIEW_SPEED_HIGH         40202
#define IDM_VIEW_SPEED_NORMAL       40203
#define IDM_VIEW_SPEED_LOW          40204
#define IDM_VIEW_SPEED_PAUSED       40205
#define IDM_VIEW_GROUP_BY_TYPE      40206
#define IDM_VIEW_EXPAND_ALL         40207
#define IDM_VIEW_COLLAPSE_ALL       40208
#define IDM_VIEW_SHOW_KERNEL_TIMES  40209
#define IDM_VIEW_SUMMARY_VIEW       40210

// Help menu commands
#define IDM_HELP_TOPICS             40301
#define IDM_HELP_ABOUT              40302

// Accelerator-only commands
#define IDA_TAB_NEXT                40401
#define IDA_TAB_PREV                40402
#define IDA_DELETE_ACTION           40403
#define IDA_END_PROCESS_TREE        40404
#define IDA_PROPERTIES              40405
#define IDA_ALT_E                   40406
#define IDA_ALT_D                   40407
#define IDA_ALT_S                   40408
#define IDA_ALT_T                   40409
#define IDA_ALT_O                   40410
#define IDA_ESCAPE                  40411
#define IDA_FIND                    40412

// Tray menu commands
#define IDM_TRAY_RESTORE            40501
#define IDM_TRAY_ALWAYS_ON_TOP      40502
#define IDM_TRAY_TOGGLE_THEME       40503
#define IDM_TRAY_EXIT               40504

// Context menu base IDs
#define IDM_CTX_END_TASK            41001
#define IDM_CTX_END_TREE            41002
#define IDM_CTX_EXPAND_COLLAPSE     41003
#define IDM_CTX_GO_TO_DETAILS       41004
#define IDM_CTX_OPEN_LOCATION       41005
#define IDM_CTX_SEARCH_ONLINE       41006
#define IDM_CTX_PROPERTIES          41007
#define IDM_CTX_CREATE_DUMP         41008
#define IDM_CTX_SET_AFFINITY        41009
#define IDM_CTX_RESTART_EXPLORER    41010
#define IDM_CTX_SWITCH_TO           41011
#define IDM_CTX_GO_TO_SERVICE       41012

#define IDM_CTX_PRIO_REALTIME       41020
#define IDM_CTX_PRIO_HIGH           41021
#define IDM_CTX_PRIO_ABOVE_NORMAL   41022
#define IDM_CTX_PRIO_NORMAL         41023
#define IDM_CTX_PRIO_BELOW_NORMAL   41024
#define IDM_CTX_PRIO_LOW            41025

#define IDM_CTX_MEM_VALUES          41030
#define IDM_CTX_MEM_PERCENT         41031
#define IDM_CTX_DISK_VALUES         41032
#define IDM_CTX_DISK_PERCENT        41033
#define IDM_CTX_NET_VALUES          41034
#define IDM_CTX_NET_PERCENT         41035

#define IDM_CTX_COL_BASE            41100 // up to +64

#define IDM_CTX_PERF_OVERALL        41200
#define IDM_CTX_PERF_LOGICAL        41201
#define IDM_CTX_PERF_KERNEL_TIMES   41202
#define IDM_CTX_PERF_SUMMARY_VIEW   41203
#define IDM_CTX_PERF_COPY           41204

#define IDM_CTX_STARTUP_ENABLE      41300
#define IDM_CTX_STARTUP_DISABLE     41301

#define IDM_CTX_SVC_START           41400
#define IDM_CTX_SVC_STOP            41401
#define IDM_CTX_SVC_RESTART         41402
#define IDM_CTX_SVC_OPEN_SERVICES   41403

#define IDM_CTX_USER_DISCONNECT     41500
#define IDM_CTX_USER_LOGOFF         41501
#define IDM_CTX_USER_EXPAND         41502
#define IDM_CTX_USER_COLLAPSE       41503

#define IDM_CTX_LOCKS_END           41600
#define IDM_CTX_LOCKS_OPEN_LOC      41601
#define IDM_CTX_LOCKS_GO_DETAILS    41602
#define IDM_CTX_LOCKS_COPY_PATH     41603

// Child control IDs
#define IDC_BTN_MORE_DETAILS        1001
#define IDC_BTN_PRIMARY_ACTION      1002
#define IDC_BTN_THEME_TOGGLE        1003
#define IDC_LIST_SIMPLE             1004
#define IDC_LIST_PROCESSES          1005
#define IDC_LIST_APP_HISTORY        1006
#define IDC_LIST_STARTUP            1007
#define IDC_LIST_USERS              1008
#define IDC_LIST_DETAILS            1009
#define IDC_LIST_SERVICES           1010
#define IDC_LINK_ACTION             1011
#define IDC_LIST_LOCKS              1012
#define IDC_LOCKS_PATH_EDIT         1013
#define IDC_LOCKS_BTN_FILE          1014
#define IDC_LOCKS_BTN_FOLDER        1015
#define IDC_LOCKS_BTN_SCAN          1016
#define IDC_LOCKS_BTN_ELEVATE       1017
#define IDC_EDIT_FOOTER_SEARCH      1018

// Dialog IDs
#define IDD_RUN_TASK                201
#define IDC_RUN_EDIT                2001
#define IDC_RUN_BROWSE              2002
#define IDC_RUN_ADMIN_CHK           2003
#define IDC_RUN_DESC                2004
#define IDC_RUN_LABEL               2005
#define IDC_RUN_ADMIN_HINT          2006

#define IDD_AFFINITY                202
#define IDC_AFFINITY_DESC           2010
#define IDC_AFFINITY_ALL            2011
#define IDC_AFFINITY_LIST           2012

#define IDD_SELECT_COLUMNS          203
#define IDC_COLS_DESC               2020
#define IDC_COLS_LIST               2021

#define IDD_ABOUT                   204
#define IDC_ABOUT_TEXT              2030
#define IDC_ABOUT_ICON              2031

// Custom window messages
#define WM_APP_TRAY                 (WM_APP + 1)
#define WM_APP_BRING_TO_FRONT       (WM_APP + 2)
#define WM_APP_THEME_CHANGED        (WM_APP + 3)
#define WM_APP_NAV_TO_DETAILS       (WM_APP + 4)
#define WM_APP_HISTORY_READY        (WM_APP + 13)
#define WM_APP_LOCKS_READY          (WM_APP + 14)
