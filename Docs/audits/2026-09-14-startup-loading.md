# 启动加载与白屏修复

> 后续交付更新：本文保留上一轮启动修复和测量记录。包含完整本地 AI 服务及 Orca 加载图标的新版入口为 `build/local-startup-preview/orca-slicer.exe`，请以[本地服务与加载图标报告](2026-09-14-local-service-and-loading-brand.md)中的运行目录、DLL 哈希和验证结果为准。

本文中的 `build/` 日志和截图是本机历史证据，不随 Git 提交；可在 GitHub 查看新版报告中随 PR 保存的关键截图。以下基线、测试数量和时间均保留实际执行时的记录。

## 版本与实现

- 工作分支：`codex/team/maintenance`，基于 `077f97f6a44ce1f90f9165f80f2cafb392cce73d` 的工作树修改。
- 集成基线：`codex/team/integration` 的 `7b6b270349a48d5b85a3c74a934f89c84da1c8ad`。维护分支已包含它；两者最终文件内容一致，保留维护分支的两个额外提交。
- 原启动流程在主窗口 `Show()` 后立即销毁 splash，随后在 UI 线程冻结窗口并完成图形初始化；显示窗口并不等于内容已绘制。
- 主窗口增加原生加载界面，先绘制再关闭 splash。关闭 splash 偏好时仍显示窗口内加载提示。
- Windows 下同步更新面板及其文字、按钮子控件；只更新父面板会留下已擦除背景、子控件等待重绘的间隙。
- Windows 下给主窗口的直接子窗口临时启用 `WS_CLIPSIBLINGS`，防止准备页侧栏绘穿覆盖层；加载面板销毁时只恢复本次增加的标志，使用弱引用及 HWND 核对窗口生命周期。
- 图形初始化按运行时、上下文、画布、字体、首帧和工作区分阶段调度。各阶段在 UI 线程执行，阶段间返回事件循环，不再围绕整个过程 `Freeze()`。
- 准备/预览页必须完成实际交换缓冲区后才撤除加载界面。文件导入后的最终页面单独确认，避免把空白场景的预热帧误当作 G-code 预览就绪。
- 首页保留自己的原生加载提示，等待主文档 `LOADED`；失败或可见等待超过 10 秒提供重试和进入准备页。重试重建浏览器并忽略上一实例的事件。
- 退出时停止启动定时器，后续回调检查关闭状态和窗口生命周期；启动期间拦截菜单与快捷键，保留退出。
- 画布暂停渲染时也暂停依赖 ImGui 字体的 idle 通知更新，避免画布已初始化而字体尚未准备好的阶段访问空字体。首页计时同时检查父 notebook 是否启用，切页隐藏时重置可见等待时间。
- 简体中文提示通过 gettext 翻译。未修改字体覆盖、项目格式、预设格式或云端授权语义。

## 验证方法

验证使用独立工作目录 `D:/TEST/OrcaSlicer-maintenance` 和 `build/startup-validation/` 内的专用配置；原 `D:/TEST/OrcaSlicer` 保留。

- 构建使用 CMake 3.29.2、Visual Studio 2022 x64、Release；复用已核对未变化的依赖前缀。
- 运行 `python scripts/verify_ai_integration.py --json`，保留 Git 检查。
- 运行 `tools/msgfmt.exe --check-format` 检查新增中文条目，并生成运行时 `.mo`。
- 实际 GUI 检查：首次/重复启动、首页/准备页、中文/英文、关闭 splash、失败/超时/重试、错误代理、启动中退出、模型导入和基本切片。
- 首页故障使用隔离运行目录中的资源副本；超时使用只绑定 `127.0.0.1:18777` 的延迟资源服务。没有修改正常首页资源，也不需要真实供应商调用。

## 性能指标含义

`Startup timing:` 日志使用 `steady_clock`。窗口显示、加载内容绘制和工作区揭示记录从 `on_init_inner()` 开始的累计时间；图形阶段和 `font_*` 记录阶段耗时。`startup.home` 记录主文档就绪、失败或超时及可见等待时间。

字体记录拆分为字体文件准备、每次 atlas 构建及总耗时、RGBA 提取、SVG 栅格化和纹理上传。纹理上传记录 API 调用耗时，没有增加 `glFinish()`；不能将它理解为 GPU 完成时间。

阶段调度让等待内容可见，但单个字体/图形阶段仍可能占用 UI 线程。字体缓存、字体 CPU 准备、云端同步和隐藏页面延迟创建应根据本次测量另行优化，不能仅凭加载提示宣称启动变快。

## 验证记录

本机：Windows x64，Intel Arc 130T，OpenGL 4.6，WebView2 152.0.4191.66。测试使用独立 datadir，未修改真实用户配置，未调用付费模型生成服务。

### 构建与自动检查

- Release 主 EXE、DLL、依赖复制及两套测试程序构建成功。可选 uv 下载证书警告不阻塞主程序。
- `verify_ai_integration.py --json`：通过，`errors=[]`，没有跳过 Git 检查。
- 中文 `msgfmt --check-format`：通过，运行资源已包含编译后的 `.mo`。
- `fff_print`：145 通过、0 失败。
- `slic3rutils`：239 通过、5 因内嵌 Python 缺 NumPy 跳过、0 失败；另外排除 3 项真实联网的 `[Http]` 测试。
- 完整命令及构建记录：`build/startup-build-commands.txt`；最终构建日志：`build/startup-build-sibling-clipping.log`；测试日志：`build/startup-fff-print-ctest.log`、`build/startup-slic3rutils-sibling-clipping-ctest.log`。最终 DLL 重链接后的 slic3rutils 检查耗时 19.57 秒。
- 可运行程序：`D:/TEST/OrcaSlicer-maintenance/build/src/Release/orca-slicer.exe`，运行时需要同目录 DLL 及资源，不能只复制 EXE。
- 最终 `OrcaSlicer.dll`：2026-09-14 11:54:26，108320768 字节，SHA256 `7818BE56B71DD551363FDD91B0602E867A62178A258B2BAD6550FAF1A4895D03`。隔离测试 runtime 已同步最终 EXE/DLL 并核对 DLL 哈希一致。

### 实际 GUI 回归

| 用例 | 结果与证据 |
|---|---|
| 中文首页首次使用独立配置、重复启动 | 正常进入首页，随后正常关闭；PID 39988、38060 为无编译负载样本 |
| 关闭 splash 后加载提示 | 实际捕捉到“正在准备界面…”、“正在准备字体和图形…”；截图见下文 |
| 默认准备页，无启动文件 | 最终 PID 40284 的连续阶段截图中中文提示完整，侧栏没有绘穿加载面板；正常进入准备页后退出，退出码 0 |
| 启动中关闭 | PID 42232 在 Finish 后、Reveal 前关闭，无 `workspace_revealed`，进程退出码 0 |
| 首页文件缺失 | 隔离资源副本缺少 `index.html` 时显示中文失败及两个按钮；进入准备页正常 |
| 首页失败后重试 | 恢复隔离资源文件后重试，213 ms 记录主文档就绪，首页恢复 |
| 首页 10 秒超时 | 本地资源延迟 30 秒；实际可见等待 10013 ms 后显示超时操作入口；迟到响应没有撤掉失败状态 |
| 超时后重试 | 恢复正常首页后重试，264 ms 记录主文档就绪 |
| 代理不可用 | 子进程代理指向 `127.0.0.1:1`，日志确认 Connection refused；首页与准备页均可使用 |
| STL 导入、基本切片 | 导入 20×20×20 mm 测试立方体；手动切片成功，249 层，预览显示打印时间及耗材 |
| 3MF 保存、启动重新打开 | 保存 `startup-cube.3mf`（25306 字节），PID 14684 启动打开后显示原立方体、打印机与耗材预设 |
| G-code 导出、英文启动预览 | 导出 `startup-cube.gcode`（609097 字节），PID 43448 在英文配置下直接启动预览；记录当前 Preview canvas 首帧成功 |

测试产物、各次 PID/退出码、日志均在 `build/startup-validation/`。正常完成的上述运行退出码均为 0；应用运行时长包含人工查看和操作时间，不能当作启动耗时。

可查看的界面证据：

- [中文加载](../../build/startup-validation/startup-loading.png)
- [字体准备提示](../../build/startup-validation/startup-fonts-zh.png)
- [英文加载](../../build/startup-validation/startup-loading-en.png)
- [首页失败](../../build/startup-validation/home-failure.png)
- [首页超时](../../build/startup-validation/home-timeout.png)
- [最终版本：正在准备界面](../../build/startup-validation/final-clipped-stage-0.png)
- [最终版本：正在绘制首帧](../../build/startup-validation/final-clipped-stage-1.png)
- [最终版本：正在打开工作区](../../build/startup-validation/final-clipped-stage-2.png)
- [最终版本：工作区等待期间](../../build/startup-validation/final-clipped-stage-3.png)
- [最终版本：准备页就绪](../../build/startup-validation/final-clipped-ready.png)

### 实测耗时与后续优化

以下为本轮修复版本在无编译负载下的运行，单位秒；最后的子控件绘制补充前采集，不能作为修复前后加速对照。

| 场景 / PID | 窗口构造 | 窗口显示累计 | 工作区揭示累计 | 显示后等待 |
|---|---:|---:|---:|---:|
| 首页 / 39988 | 3.872 | 5.230 | 12.048 | 6.818 |
| 首页重复 / 38060 | 4.012 | 5.471 | 12.335 | 6.864 |
| 准备页 + 3MF / 14684 | 3.892 | 5.275 | 18.726 | 13.451 |
| 英文 G-code / 43448 | 3.874 | 5.177 | 18.197 | 13.020 |
| 默认准备页 / 43068 | 3.914 | 5.074 | 17.923 | 12.849 |

两次首页样本显示后平均等待 6.841 秒。首次 home / PID 14060 与 CTest 有短时重叠，仅用于功能回归，不纳入性能均值。

最终同步绘制及兄弟裁剪版本补充记录，单位秒，同样不作为前后加速对照：

| 场景 / PID | 窗口构造 | 窗口显示累计 | 工作区揭示累计 | 显示后等待 |
|---|---:|---:|---:|---:|
| 默认准备页 / 40284 | 4.154 | 5.426 | 18.425 | 12.999 |
| 首页 / 20160 | 3.825 | 5.123 | 11.984 | 6.861 |

PID 40284 在显示后 5 ms 完成加载内容同步绘制；准备页两次字体总耗时分别为 2.418 秒和 5.668 秒，纹理 API 调用分别为 2.069 秒和 5.214 秒，首次渲染阶段为 2.034 秒。日志见 `build/startup-validation/prepare/log/debug_Mon_Sep_14_11_58_12_40284.log.0`，退出记录见 `prepare/20260914-115812603.result.json`。PID 20160 使用 `early-close` 配置目录，但操作时已进入首页，因此仅计入正常首页启动/退出回归，不算启动中关闭样本。

主要瓶颈与优先级：

1. **先消除字体完整重建。** 准备页/文件启动中 atlas 从 `4096×2733` 重建为 `4096×6896`，两次都是 `attempt=0`，不是尺寸拓宽重试。3MF 和 G-code 启动中字体合计约 8.05 秒，其中 DXT5 纹理调用合计约 7.26 秒。首次 Fonts 阶段直接 `new_frame()`，后续画布 `_resize()` 才调用 `set_scaling()` 并销毁旧字体；建议首次 atlas 前应用最终字号和缩放。尚未记录前后字号/DPI，具体触发值仍需补测。
2. **比较字体纹理策略。** 小 atlas 的 DXT5 调用约 2.06–2.12 秒，大 atlas 约 5.16–5.21 秒；atlas 构建本身约 0.33–0.41 秒。应比较 RGBA、缓存压缩纹理和显存成本，当前日志无法再区分驱动压缩与上传。本次未贸然切换纹理格式。
3. **再细分窗口创建、隐藏页面和联网。** 窗口构造约 3.9–4.0 秒；AI sidecar 不可达时仍有后台探测。可进一步测量隐藏页创建和网络初始化，再确定延迟到首次使用或后台执行的范围。

本次优先解决启动反馈；单个字体阶段仍可能暂时阻塞 UI，首次从首页切到准备页也可能因大字体重建等待约 5–6 秒。本次不宣称启动耗时已下降，也不宣称全程动画或交互都不受阻。

### 验证中修正与限制

首次功能运行发现 `Canvas → Fonts` 间隙的 idle 通知访问空 ImGui 字体（`NotificationManager::count_spaces → ImGui::CalcTextSize`），崩溃栈保存在 `build/startup-validation/home/log/crash_Mon_Sep_14_11_06_36_0.log`；添加暂停渲染期间的 idle 保护后，上述正常/失败/文件导入路径没有再次出现此崩溃。

最终补充修正：Windows `Refresh()` 使子树失效，而 `UpdateWindow()` 只同步父 HWND。已增加子控件同步绘制，防止父面板已擦背景、文字仍等待绘制时进入耗时阶段。连续截图又发现准备页侧栏绘穿覆盖层；wxWidgets 默认省略 `WS_CLIPSIBLINGS`（依赖源码 `src/msw/window.cpp:1512`），已针对这个重叠布局临时启用兄弟裁剪。最终版本构建成功；PID 40284 在 11:58:18–11:58:28 的四张连续阶段截图均显示完整中文提示，无侧栏绘穿或无字白面板，随后准备页首帧成功，正常退出码为 0。截图时间记录保存在 `build/startup-validation/final-clipped-captures.json`。

旧版本 9 月 11 日的日志只作历史参考。首次独立配置启动包含预设安装成本；本次没有重启操作系统或清空系统文件缓存，不能标注为严格冷启动基准。网络用错误代理模拟连接失败，没有断开系统网卡。未在 macOS/Linux、多种显卡、所有旧 3MF/profile 版本或真实打印机上验收。
