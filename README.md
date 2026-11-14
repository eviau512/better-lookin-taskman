# Better Lookin' Win10 Task Manager

[中文说明](#中文说明) | [English](#english)

> A personal Win32 / C++ experimental project inspired by the classic Windows 10 Task Manager, focusing mainly on Dark / Light theme support and a few small quality-of-life tweaks.  
> It is still rough around the edges and **does not try to match or replace the stability and completeness of Microsoft's inbox Task Manager** — bug reports, suggestions, and PRs are very welcome!

---

<a id="中文说明"></a>
## 中文说明

这是一个基于原生 Win32 C++（无第三方 GUI 框架，MSVC `/MT` 静态编译单文件）的开源实验项目，主要想为喜欢的经典 Windows 10 任务管理器样式补上较统一的 **深色 / 浅色 / 跟随系统** 主题体验，并尝试加入几个自己平时顺手的小功能。

作者能力与测试环境有限，很多系统底层细节、兼容性与边缘场景仍在摸索和完善中，**远不敢说比系统原版更好**；如果你在使用中遇到显示错位、计数器偏差或兼容性问题，非常欢迎提 Issue 交流或帮忙指正。

### 目前尝试实现的功能
- **深浅色主题与菜单栏适配**：支持跟随系统（`ImmersiveColorSet`）、浅色、深色三档切换（`Ctrl+Shift+D`、选项菜单或标签栏右侧切换胶囊），含标题栏、菜单栏与自绘列表/图表配色同步。
- **双语界面与混排字体**：支持随系统或手动切换简体中文 / English，中文辅以 `Noto Sans SC DemiLight` 与 `Segoe UI` 混排以改善观感。
- **7 个基础选项卡与简略模式**：进程、性能（CPU / 内存 / 磁盘 / 网络 / GPU）、应用历史记录（读取本地 SRUM 记录）、启动（含 UWP 应用与启用/禁用）、用户、详细信息、服务。
- **几个微小的附加尝试**：
  - **显示时间段重采样**：将性能视图时间窗细分为 `30 秒（高）` / `60 秒（正常）` / `4 分钟（低）`，后台维持 `500ms` 环形缓冲采样，切换时间跨度时可立即重绘过去对应时段的曲线。
  - **启动前历史采集（可选 `/daemon`）**：通过轻量无窗口守护采样与命名共享内存环缓冲（及 `%APPDATA%\BetterWin10Taskman\perf_history.bin`），尝试保留打开界面前最近一段时间的系统占用波形（若仅带 `/multi` 测试则不启用）。
  - **性能页双击缩小浮窗**：双击性能选项卡左侧导航栏或右侧主图表区域，可临时收起边框与多余控件为迷你浮窗（支持拖拽、卡片切换与双击 / `Esc` 还原）。
  - **天蓝色 CPU 微缩曲线托盘图标**：托盘图标尝试采用接近原版的天蓝色调并绘制迷你 CPU 历史波形（支持“最小化时隐藏”）。

### 已知不足与局限
- 代码与功能仍在持续修补中，部分细节和极端多屏 / 多硬件环境下可能与原版行为存在差异。
- 本地直接编译出的二进制**默认没有微软受信任的数字签名**，首次运行可能触发 SmartScreen 提示；后续若通过社区开源签名服务接入签名再另行说明。
- 某些磁盘、温度或跨会话信息在非管理员权限下会受系统 API 限制（已尽量做了温和兜底）。
- 如果使用 **IFEO (`Image File Execution Options`)** 替换系统默认任务管理器，请务必确认可随时恢复原版，不建议直接强行覆盖 `C:\Windows\System32\Taskmgr.exe`。

### 编译方法
需要安装 Visual Studio 2022（或兼容版本的 Build Tools，“使用 C++ 的桌面开发”工作负载）：

```bat
build.bat
```

构建完成后会在仓库根目录生成独立的 `Taskmgr.exe`（默认已列入 `.gitignore`，不会提交编译产物到仓库）。

### 常用启动参数
- `/0` .. `/6`：启动时直接定位到第 0–6 个选项卡（例如 `/1` 直接打开性能页）。
- `/multi`：允许并存多开实例（用于对比测试，不触发常驻 `/daemon`）。
- `/daemon`：仅在后台维持 500ms 性能环形缓冲采样（无界面）。

---

<a id="english"></a>
## English

An exploratory Win32 / C++ desktop project built from scratch (no external GUI frameworks, single statically linked `/MT` binary) inspired by the classic Windows 10 Task Manager layout, mainly to experiment with consistent **Light / Dark / Follow System** theming and a few small tweaks.

This is very much a work-in-progress hobby/learning project and makes **no claim to rival Microsoft's inbox Task Manager in reliability or coverage**. Feedback, corrections, and pull requests are always appreciated.

### Current Capabilities
- **Light / Dark / Follow System Theme**: Instant switching via `Ctrl+Shift+D`, **Options → Theme**, or the top-right tab bar pill (including DWM dark caption and custom-drawn menus/tables/charts).
- **Bilingual UI (English / 简体中文)**: Auto-follows Windows display language or switches manually under **Options → Language**.
- **7 Classic Tabs + Fewer Details Mode**: Processes, Performance (CPU, Memory, Disk, Network with public Global IPv6 preference, GPU), App history (SRUM-backed where available), Startup (including UWP tasks), Users, Details, and Services.
- **Small Experimental Additions**:
  - **Display Time Span Resampling (`30s` / `60s` / `4m`)**: Maintains a `500ms` 4-minute ring buffer so switching speed/span immediately redraws past utilization curves.
  - **Optional Pre-launch Background Sampling (`/daemon`)**: Shares a lightweight ring buffer (`%APPDATA%\BetterWin10Taskman\perf_history.bin` + named shared memory) so charts can show recent activity prior to opening the UI.
  - **Double-Click Compact Floating Views**: Double-clicking the Performance sidebar or right chart pane shrinks the window into a frameless floating card/graph view (`Esc` or double-click again to restore).
  - **Sky-Blue Mini CPU Graph Tray Icon**: Renders a compact CPU history waveform in the notification area.

### Known Limitations
- Expect rough edges across uncommon hardware/multi-monitor setups; several low-level counters depend on standard Windows API permissions (non-elevated runs may omit certain protected process/adapter details).
- Self-compiled executables are **unsigned** and may trigger Windows SmartScreen warnings.
- Always back up or prefer reversible launch methods rather than overwriting `C:\Windows\System32\Taskmgr.exe`.

### Build from Source
Requires Visual Studio 2022 / Build Tools with the **Desktop development with C++** workload:

```bat
build.bat
```

### CLI Options
- `/0` .. `/6`: Open directly on the corresponding tab (`0 = Processes`, `1 = Performance`, etc.).
- `/multi`: Allow running another instance side-by-side without starting `/daemon`.
- `/daemon`: Run the headless `500ms` ring-buffer collector only.

## License

Licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE) for the full text.
