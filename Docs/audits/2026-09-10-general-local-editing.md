# 通用局部编辑实现与验证记录

日期：2026-09-10。范围：按 `Docs/research/2026-09-10-3d-local-editing-research.md` 实现前两阶段的通用三维选区和局部颜色基础，不限定领口、人物或任何语义类别。实际 GUI 阶段验收尚未完成。

## 实现

- 共用圈选、补选笔刷、保护笔刷、相似区域及旋转工具；橙色显示选区，蓝色显示保护区。圈选和笔刷通过当前视角的实际网格射线命中限制可见表面。边界三角面作为整面选择，不新增几何切割。
- 可选局部图割利用颜色、法向和正负笔触贴合边界；严格限制 ROI、硬种子与保护区。上限为 150,000 个 ROI 面，独立顶点接缝不跨越。未集成或评测 SAM 等 AI 权重，不声称自动语义分割。
- 可见面选择及图割在后台计算，可取消；切换模型使旧任务失效。选区保留、撤销和重做采用有内存上限的快照。
- 将“清理小杂点”和“统一这块颜色”分开。统一颜色作为面颜色覆盖保存，导出时仅在颜色边界复制顶点，保留面顺序及未选面的颜色。清理杂点保护既有手工指定颜色。
- 局部颜色独立于全局试色中心；颜色本身未改变但新增固定颜色意图时，仍可保存版本。候选颜色单独持有，不能依赖 384 MiB 预览缓存命中才能保留。
- 保存选区、保护、前景和 ROI 掩码，保存面颜色意图及完整试色中心/目标色；以面数和按三角面角点计算的几何 SHA-256 验证身份。重新打开时恢复手动试色快照，避免动态耗材包索引或当前工程变化重新解释旧版本。
- NativeMatch 导入传递并校验面颜色意图。高面数路径覆盖最终分类，低面数路径将锁定源面的颜色传递给拆分子面。带覆盖时不自动修复、重排或平滑已确认表面。物理耗材仍在原生界面确认；缺少相同颜色不等于打印能准确复现。
- 网格修复可能改变拓扑，因此修复后的版本不沿用旧面选区和颜色覆盖。原模型、普通无覆盖导入和既有工程由原流程处理。

## 验证边界

实际主窗口验收未完成。此前启动新版程序的命令被自动审批拒绝，原始结果为 `rejected: blocked by policy`，未提供更详细理由；没有通过其他工具或代理重试启动。以下离线检查不能替代实际界面验收，尤其不能证明 OpenGL shader 运行、工具切换状态、版本对比/撤销/重做及准备页交接的完整体验。

没有调用生成提供商、下载 AI 权重、联系测试者、推送或发布。本次构建是共享脏工作树的本地检查，不是集成分支合入包或公开发行包。工作树含此前 GLB 兼容性和其他任务修改，不能将整个差异归为本次独立提交。

## 构建尝试

固定源提交：`6c992edc39e1b2500d0e313fd9eaebf9658d5c6d` 加未提交工作树。环境：Windows x64 / Release / Visual Studio 2022 MSVC 14.44 / 项目现有 CMake 依赖。

| 尝试 | 记录 | 结果 |
| --- | --- | --- |
| 1 | `.tmp/general-local-edit-build.log` | OBJ 颜色变量遮蔽、GLModel 法向重载错误；已修正 |
| 2（测试） | `.tmp/general-local-edit-tests-build.log` | 图割测试索引类型错误；已修正 |
| 2 | `.tmp/general-local-edit-build-2.log` | 当时的 OrcaSlicer 与 slic3rutils_tests 编译通过；早于最新持久化和原生导入改动 |
| 3 | `.tmp/general-local-edit-build-3.log` | 链接写入 OrcaSlicer.dll 报 LNK1104 |
| 4 | `.tmp/general-local-edit-build-4.log` | 相同 LNK1104；模块枚举定位到既有 `orca-slicer` PID 50224 加载原输出目录 DLL，保留该实例 |
| 5（链接） | `.tmp/general-local-edit-link-5.log` | 独立 OutDir 的 DLL 链接成功；原有 post-build 脚本仍指向原输出目录，Python 目录删除步骤失败，整体命令退出 1 |
| 5（启动器） | `.tmp/general-local-edit-launcher-build-5.log` | 独立目录启动器构建成功；未启动程序 |
| 5（测试构建） | `.tmp/general-local-edit-tests-build-5.log` | 两套目标构建成功；后续测试与对象文件校验发现部分旧对象被复用，不能作为最终源码验证 |
| 6（重编） | `.tmp/general-local-edit-rebuild-6.log` | 源/头文件使用方、两套测试目标重新编译成功；包含原生格式适配器的依赖重编 |
| 6（独立程序） | `.tmp/general-local-edit-link-6.log`、`.tmp/general-local-edit-launcher-build-6.log` | DLL 和启动器成功；原生格式适配器后续重编后由尝试 7 取代 |
| 7（最终链接） | `.tmp/general-local-edit-link-7.log`、`.tmp/general-local-edit-launcher-build-7.log`、`.tmp/general-local-edit-tests-relink-7.log` | DLL、启动器和 slic3rutils_tests 基于更新后的依赖链接成功；禁用原目录 post-build，独立运行时已另行校验 |

尝试 5 的 Python 后处理发生部分删除；已只补回 1,185 个缺失文件，未停止运行实例，随后原目录和独立目录均通过 Python 3.12.13 / Pillow 12.2.0 的隔离 PNG 往返检查。独立目录另按 CMake 的明确安装清单复制并逐文件校验 22 个 sidecar Python 文件，无提供商配置。

原生测试首次为 15 个用例、14 通过、1 失败：低面数覆盖产生 19 面，基线为 24 面。未修改或削弱断言。CodeView 编译时文件校验发现 TextureToColor 对象仍包含较早的“锁面直接保留原三角形”路径，而源码已改为每个细分子面继承覆盖。另发现 TexturePainting、ModelGenerationPanel、ModelGenerationFinishingView 的部分编译记录过时。已冻结源码内容、刷新受影响源/头文件时间并安排重编。CodeView 只覆盖调试信息记录的文件，因此同时让相关头文件使用方重新编译，不能把未记录的依赖当作已校验。

对应调查：`.tmp/check_codeview_sources.py`、`.tmp/general-local-edit-codeview-audit.json`、`.tmp/general-local-edit-native-tests.log`。

早期相关离线测试为 69 个用例、5,520 次断言通过，已由下面最终快照测试取代。

## 最终验证快照与结果

源码清单：`.tmp/general-local-edit-source-e0d14e1c58d5.json`，完整文件摘要 `e0d14e1c58d52813a5150b46fbbcbc829fa46419944d83d9f52d30e61d075260`，共 55 个改变/新增的源和测试文件（含此前修改与夹具）；不是干净提交。最后一次本地 origin 集成引用为 `7b6b270349a48d5b85a3c74a934f89c84da1c8ad`，本轮没有 fetch、同步或提交集成候选。

链接后共享工作树的 `TexturePainting.cpp` 又出现独立的单位换算修改，本次二进制没有包含这次稍后修改。15 个关键对象的 CodeView 检查中，14 个匹配当时磁盘，剩余 TexturePainting 对象的编译 SHA-256 精确匹配上述验证快照里的 `82144a2ed35bf575376b0460aec10767d98093a335923d97d999c2e94945e19d`。因此本报告只证明该快照，不能用于宣称后来共享树的修改通过。证据：`.tmp/general-local-edit-codeview-final-15.json`。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 选区、图割、掩码/颜色/试色持久化、导出、局部修整与导入反馈 | 104 用例 / 6,469 断言通过，随机顺序；其中包含一次真实模型离线基准 | `.tmp/general-local-edit-tests-verified-snapshot.log` |
| 原生面颜色和普通顶点颜色导入 | 15 用例 / 2,557 断言通过，随机顺序 | `.tmp/general-local-edit-native-tests-final.log` |
| 双层遮挡色片 | 同一基准用例换独立夹具，11 断言通过 | `.tmp/general-local-edit-panels-verified-snapshot.log` |
| 集成边界检查 | `ok: true`、`errors: []`，未跳过 Git 检查 | `.tmp/general-local-edit-integration-final.json` |
| 两个目录的隔离 Python / Pillow | Python 3.12.13、Pillow 12.2.0、原生 PNG 往返通过 | `.tmp/general-local-edit-runtime-check.json`、`.tmp/general-local-edit-restored-runtime-check.json`（文本输出） |

可复核本地目录：`build/general-local-edit-preview`，未启动。构建 ID：`local-general-edit-6c992edc39e1-windows-x64-Release-20260910-a7`。运行文件清单 `.tmp/general-local-edit-runtime-f3b1c3d78851.json` 含 17,037 个文件，排除生成模型与缓存；目录文件摘要 `f3b1c3d78851c0bbdbe758e1a54a975b61a7f83c5c95b2c8f77a96abc93e9b5e`。

- `OrcaSlicer.dll`：83,256,320 字节，SHA-256 `83e6fde6ebf9812bcb69a12ec0da3ef3c629ebfad2dbce49c1e392e49d0ee2fc`。
- `orca-slicer.exe`：271,360 字节，SHA-256 `d0bf445ee24049658482e01e912c74a65a3a81ec0218cb1df952bd9eff8e3561`。

这是一份本地验证目录及记录，没有制作/上传公共安装包，没有声称 CI、非作者审批、集成合入或真实 GUI 验收通过。现有 C++17 属性忽略与 CRT 链接警告未在本次扩大修复。

## 离线实际模型测量

环境：Intel Core Ultra 7 155H、约 32 GiB RAM、Windows x64 Release。下面是单次运行、带可见性证据记录的耗时；首轮同时有原生测试目标编译，不是 p50/p95 或纯 GUI 响应测量。

| 模型 | 面数 | 读取 | 编辑器准备 | 圈选 | 补选 / 保护笔刷 | 图割贴合 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 现有模型副本 | 1,906,896 | 1,593 ms | 15,554 ms | 3,438 ms | 63 / 52 ms | 4,641 ms |
| 双层遮挡色片 | 9,600 | 64 ms | 15 ms | 11 ms | 0.5 / 0.2 ms | 0.7 ms |

大模型 ROI 69,317 面；双层色片 ROI 313 面。两者实际 BVH 可见性证据错误、硬种子错误、ROI 外选中面均为 0。大模型首次准备和圈选/图割耗时没有达到研究中的目标，后台执行不等于低延迟。记录在 `.tmp/general-local-edit-tests-final.log`、`.tmp/general-local-edit-panels-benchmark.log`。测试读本地模型，没有调用提供商。

最终快照复测：大模型读取 1,841 ms、编辑器准备 9,686 ms、圈选 1,883 ms、补选/保护 48/40 ms、图割 2,574 ms，ROI 和三个错误指标与上表一致。该结果仍未达到首次准备 3 秒、局部候选 500 ms 的研究目标；它也是单次测量，不能宣称 p95 达标。日志为 `.tmp/general-local-edit-tests-verified-snapshot.log`。

## 待验收

启动包含最终修改的实际程序，从主窗口检查：不同类型模型可见面圈选与背面隔离；补选/保护和图割；取消/切页；改色与去杂点；前后对比、接受、放弃、版本撤销/重做；重新打开模型库；原生准备页颜色映射与撤销；普通无 AI 的导入、准备行为。需要保存实际截图或日志后才能将相应项记为通过。
