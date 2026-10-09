# Steam 原始开始按钮的戴夫离线启动

在 Steam 的潜水员戴夫属性中，把启动选项加入 `-offline`，随后照常点击库页面原来的“开始游戏”。OpenSteamTool 在初始化时启用后台准备组件；原生开始游戏调用先等待 Steam 真正离线且后台连接断开，再继续正常启动。游戏进程创建阶段的后台组件等待本轮 DLC 检查及标题初始化，随后恢复在线。

正常流程没有控制台或确认框，不点击窗口，不修改票据或授权返回值。没有 `-offline` 时保留原始启动行为。当前仅支持戴夫 AppID `1868140`，并不代表其它游戏已经适配。`--offline`、`-offline=no` 及包含该字样的路径都不会误触发。

## 安装

正式发行包为 `OpenSteamTool-v1.4.8.14-dave.1-Release-x64.zip`，使用 Windows x64 Release 构建。更新前完整退出 Steam 和所有 Steam 应用，备份原有三个 DLL 及 `dave-launcher` 文件夹。

将包内的 `OpenSteamTool.dll`、`dwmapi.dll`、`xinput1_4.dll` 放进 Steam 根目录，与 `steam.exe` 同级；把整个 `dave-launcher` 文件夹放在旁边，保留其中的 `DaveLauncher.exe`、`DaveLauncher.json` 和 `SteamPlayGate.js`。不要把这些 DLL 放进 Steam 的 `bin` 子目录。保留现有 Lua、用户 TOML 和 `opensteamtool` 配置；调整过的 `DaveLauncher.json` 应先备份，再合并配置。

首次安装需在 Steam 根目录创建空文件 `.cef-enable-remote-debugging`，已有则保留。可使用以下 PowerShell 命令，把路径改成自己的 Steam 根目录：

```powershell
New-Item -ItemType File -Path (Join-Path '你的Steam目录' '.cef-enable-remote-debugging') -Force
```

这是 Steam 库界面的本地控制接口开关，不是 `-console` 参数。重启 Steam，等待库界面就绪，在“潜水员戴夫 → 属性 → 通用 → 启动选项”加入 `-offline`，随后直接点击原来的“开始游戏”。删除这个参数即可禁用本流程。

后台准备组件随 Steam 启动和退出，不需要开机计划任务。它每两秒检查原生开始游戏接口是否已准备好，并在 UI 重载后重新接入；只有目标为戴夫且有精确 `-offline` 时才切模式。切换前要求最近六秒内确认没有其它 Steam 应用运行，状态不明时停止。此流程针对 Steam 库界面的原始“开始游戏”调用；其它直接启动 EXE 或绕过该界面的启动方式不在本次验证范围。

启动时保留 Steam 原 AppID、进程环境、工作目录和其它游戏参数，只移除用于编排的 `-offline` 并给游戏设置本轮独立日志。后台组件保持运行至游戏退出，让 Steam 继续跟踪这次游戏会话。缺少组件或已确认的请求不安全时会触发 Steam 自身的启动失败，而不是悄悄在线启动。不要与 AppID 重映射的 `-onlinefix` 同时使用。

## 使用条件

- Windows x64，系统已有 .NET Framework 4.8。
- 已安装 Steam 和戴夫（AppID 1868140），并有可用的 Steam 登录缓存。
- Steam 已启用 `.cef-enable-remote-debugging`，控制端口仅监听 `127.0.0.1:8080`。启动器不会自动修改登录配置或防火墙。
- 戴夫及其它 Steam 应用已退出。Steam 本身可运行；若已退出，启动器会先启动 Steam。

这会暂时让整个 Steam 进入离线模式。完成初始化后会恢复在线，好友运行状态由 Steam 自己更新。

## 初始化判断

每次启动给游戏传入单独的 Unity `-logFile` 路径，将日志与新游戏 PID、创建时间和安装路径绑定。只有本轮日志满足以下顺序，才允许恢复在线：

1. 配置列出的每个 DLC 本地检查均为 True。
2. Steam 加密票据的原生离线回调返回 `NoConnection`。
3. 标题数据加载成功，接着出现标题音乐初始化调用栈。
4. 标题初始化事件后再经过 10 秒保护时间，且本次游戏窗口仍正常响应。

第 4 步的保护时间不能独立触发上线。该判定在本机游戏 `v1.0.6.2113.steam` 验证过，但日志没有提供“菜单已渲染”的专门事件；不同游戏版本、特别慢的资源加载仍需复核。若日志格式变化或任何条件缺失，启动器会停止并保持离线，不假装成功。

`DaveLauncher.json` 的 `ExpectedDlcs` 默认是本机游戏实际查询的五个内容包：2677020、2841140、3543180、4158580、4394810。它不表示商店所有商品或未来新增 DLC。`TitleSettleSeconds` 可在 3–30 秒之间调整，`ReadyTimeoutSeconds` 可在 30–600 秒之间调整。

## 失败恢复

如果游戏未启动或已退出，启动器会尝试恢复原来的在线状态。如果游戏仍运行或是否启动尚不确定，它会保留离线状态并报错，避免初始化中途上线。

需要主动恢复时，在 PowerShell 运行以下命令，先改成自己的 Steam 路径：

```powershell
& '你的Steam目录\dave-launcher\DaveLauncher.exe' --online
```

该入口不验证游戏初始化，适合你明确决定恢复时使用。后台组件等待游戏退出时已释放流程互斥锁，不会阻止明确的恢复操作。

Release 构建关闭 OpenSteamTool 核心的 Debug 日志，仍保留初始化判断所需的游戏会话日志和简要运行记录，位于程序旁的 `sessions/`、`last-run.json` 和 `last-error.txt`。`--preview` 只检查前置条件，`--status` 只查询状态；二者写 `last-probe.json`，不会覆盖最近启动记录。游戏原始日志可能包含本机数据，请勿随意上传整个 sessions 目录。

## 验证记录

已有原始 Steam 库按钮调用链实测覆盖：确认 Steam 离线、绑定本轮游戏进程、完成五项 DLC 检查和原生离线票据路径、等待标题初始化、恢复后台连接，以及退出游戏后清除 Steam 的运行标记。用户确认离线启动后主菜单不再显示购买 DLC 提示。已有验证针对戴夫 `v1.0.6.2113.steam` 和 Steam Build ID `1788652215`，不能据此保证未来版本仍适用。

170 项自动化检查包括 46 项 C# 测试、108 项原生检查和 16 项 JavaScript 测试，覆盖编排顺序、异常恢复、旧日志隔离、DLC 检查缺项、退出检测、Unity 协程堆栈、Windows 参数引用、恢复互斥锁，以及开始调用前的模式切换、超时和重复点击。

此适配版基于上游 `mmxlyo/OpenSteamTool` 的 `322d1d8`，另含本仓库的戴夫适配与稳定性修正，不包含当前 `main` 的全部上游更新。本功能自动执行 Steam 的原生离线/在线流程，不下载 DLC，也不修改服务端授权校验结果。

## 构建

在仓库运行：

```powershell
./tools/DaveLauncher/build.ps1 -Test
```

系统 C# 编译器直接生成独立 EXE，无 NuGet 或额外运行库下载。构建结果默认位于 `build-dave-launcher/`。Steam 原按钮接入还需要对应版本的 OpenSteamTool 核心，通过 MSVC 工作流构建；完整产物同时包含核心 DLL 和后台组件。直接运行 EXE 的独立启动模式仍保留，但不是原按钮接入方式。
