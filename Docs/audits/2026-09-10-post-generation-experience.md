# 3D 生成后体验优化：实现与验收

日期：2026-09-10。工作树：`codex/team/model-generation`，基于 `8d14b41dd07ea3a37f949a218d8d2012878ee218`，包含上轮局部去杂的未提交改动。本轮目标见 [计划](../plans/2026-09-10-post-generation-experience.md)。

## 结论与范围

完成本轮五项目标：加快本地处理、聚焦局部、保留配色编辑意图、修正折返表面的保护条件、真实主窗口验证版本恢复及原生导入往返。没有新增 Python、Blender、MeshLab 或 Open3D 运行时，没有调用生成服务。

这是一轮有实测标准的可交付优化，不表示所有模型或实际打印已达到最优。验收使用同一真实 190 万面人物模型，以及针对异常网格的离线测试。

## 开源调研与取舍

| 参考 | 可复用价值 | 本轮决定 |
| --- | --- | --- |
| [Blender Expand](https://docs.blender.org/manual/en/5.0/sculpt_paint/sculpting/editing/expand.html) | 明确选区、按表面拓展与局部观察的交互 | 增加选区聚焦、隐藏高亮；借鉴交互，不嵌入应用或声称已实现全部 Expand 功能 |
| [MeshLab 保表面平滑](https://pymeshlab.readthedocs.io/en/latest/filter_list.html#apply-coord-laplacian-smoothing-surface-preserving) | 表面约束、控制过度平滑 | 保留现有位移限制、边界与锐边保护；修正反向面漏保护，暂不换引擎 |
| [libigl ARAP](https://libigl.github.io/tutorial/#as-rigid-as-possible) | 形变的预计算与求解分离 | 仓库已有 libigl；真正需要拖拽塑形时评估相关模块，本轮不引入没有入口的形变功能 |
| [CGAL Polygon Mesh Processing](https://doc.cgal.org/5.6.3/Polygon_mesh_processing/index.html) | 网格检测、修复与处理算法 | 仓库已使用 CGAL；保持现有 Orca 网格与导入责任边界 |
| [fast_float](https://github.com/fastfloat/fast_float) | 不依赖 locale 的数值解析，减少分配 | 复用仓库已 vendored 的库替换逐数字流解析，无新增运行时依赖 |

六色打印不适合直接做 RGB 拉普拉斯平均，否则会产生新色。局部去杂继续合并符合条件的小色块，复制已有邻色，保留几何。大面积黑斑、纽扣、眼眉等无法只凭“小色块”可靠区分，因此仍提供局部改色和对比确认。

## 本轮改动

- OBJ 解析采用轻量 token 扫描、已有 fast_float 和整数 from_chars；保留正负数、科学计数法、负索引及原始尾部，拒绝非有限数和多余字符。
- “放大选区（F）”将局部放入画面中心，扩大细节缩放范围；“显示选区高亮”可关闭覆盖色，选区继续有效。局部改色也可使用高亮开关。
- 同一作品在候选预览、前后对比、接受、放弃、撤销与重做时保留目标色、源颜色映射、锁色、数量、光照和显示状态。加载另一个作品仍初始化其颜色状态；工程耗材变化仍执行原有校验。
- 接受已经显示的候选模型时复用当前预览；真实模型尺寸与相机取景尺寸分开保存，避免尺寸显示被旧取景边界覆盖。
- 平滑时所有负法线点积均进入锐边保护，修复先平方造成折返面漏保护的问题。
- 实际 GUI 发现 Windows 原生滑杆标签重叠，替换为独立的“处理强度：15%”文字；拖动后自动预览仍有效。

## 性能与文件结果

基准模型：953,446 顶点、1,906,896 三角面、102,997,309 字节。原始 SHA256：`bbb5929a852be262457cd3527d495bb143b64df071d11f60e90a87614233e390`。

同机、同 64,121 面领口选区、同六色色板和 35% 力度，修改前后各连续运行三次：

| 项目 | 修改前 | 修改后 |
| --- | --- | --- |
| 三次引擎耗时（ms） | 13305 / 11870 / 10769 | 5968 / 5416 / 5668 |
| 中位数 | 11870 ms | 5668 ms |
| 中位数下降 | — | **52.2%**，超过 30% 目标 |

六个结果文件 SHA256 完全相同：`9342a7158d9f481df1c4d716eef146f966a88c7b2d3f3a7e9064700d550481a3`。这些时间包含引擎读入、解析、计算、写出和哈希，不包含 GUI 后续准备与渲染，不能称为整条交互链耗时。

最终实际 GUI 用五色色板与一次鼠标点选形成 59,565 面选区：清理 108 处、修改 230 个顶点颜色。逐行核对原文件和处理文件：几何、面、UV、法线、材质、alpha 均未变化；所有改变的顶点均不被未选面共享；RGB 全部来自已有源色。该结果与离线基准选区不同，不混用数字。

最终 DLL 82,992,128 字节，比上轮去杂版增加 **10,240 字节（10 KiB）**。这只是本次同配置 DLL 对比，不是跨配置安装包估计。未做内存峰值对照，不作内存下降承诺。

## 主窗口实际验收

独立运行 `build/post-generation-preview/orca-slicer.exe`，独立数据目录 `.tmp/post-generation-20260910/user-data`。原用户 `build/ux-preview` 窗口及未保存工程未操作。

1. 从历史模型加载原模型，原图、AI 图同时恢复；离线服务状态不阻碍本地编辑。
2. 设置非默认五色，锁定首个黑色、开启光照。进入局部去杂后点选领口、聚焦，隐藏高亮仍保留 59,565 面选区。滑杆百分比和按钮显示完整。
3. 预览去杂，切换处理前/后，观察相同相机位置；接受并保存新版本，返回上个版本，再重做。回作品页仍为相同五色、锁色和光照。
4. 局部改色中高亮开关可见；切换整体美颜，拖动到 15% 后自动预览，显示柔化 940,162 个顶点；放弃预览后恢复已接受的去杂作品，试色与输入仍保留。
5. 原生导入窗口收到相同五个目标色，显示使用三个现有实体耗材、零新增耗材。取消后准备页无模型残留，返回生成页输入与模型保留。
6. 重新导入并确认，准备页有单个 1,906,896 面人物模型；状态为等待手动切片，G-code 导出不可用。打印机、工艺与四个耗材槽保持测试工程原配置。
7. 保存验收 3MF，再返回生成页，输入“本地体验验收：保留绿色领口与衣褶细节。”、原图、AI 图、五色色板与锁色均保留；页面明确提示已导入。

工程文件核验：`beauty-import-acceptance.3mf` 内网格保持 1,906,896 面，存在三种涂色编码，无 G-code；打印机 `WonderMaker ZR 0.4 nozzle`、工艺 `0.20mm Standard @WonderMaker ZR`、层高 0.2，四个耗材色为 `#26A69A / #59453A / #BBB6B2 / #C18666`。

## 自动验证与证据

- `slic3rutils_tests.exe '[ModelFinishing],[VertexColorRegion],[ModelPreviewPalette]' --order rand`：**51 个用例、5,135 个断言通过**。覆盖折返边、未选区保护、异常数字解析、颜色与版本处理。强化原折叠面用例时加入独立温和网格，确保同时证明“保护问题区”和“正常区仍改善”。
- Release GUI 构建成功，隔离 Python/Pillow 打包校验通过。保留现有 `LNK4098 LIBCMT` 链接警告，未将它描述为无警告构建。
- `python scripts/verify_ai_integration.py --json`：`ok=true`、`errors=[]`、Git 检查未跳过。
- `git diff --check` 通过，仅有工作树 LF/CRLF 提示。
- 最终 DLL SHA256：`766BE36815AF12CED8E2CA7B8B6D85A5BC5056DAD1F1A47A95D7AEB58FCDC86C`。

原始证据保存在 `.tmp/post-generation-20260910/`：`benchmark.json`、`benchmark-hashes.json`、`tests-final.log`、`build-slider.log`、`build-slider-result.json`、`integration-delivery.json`、`delivery-file-verification.json`、`verify_delivery.py` 和验收 3MF。

GUI 截图包括：`focus-no-overlay-before.jpg`、`focus-cleanup-after.jpg`、`trial-after-accept-undo-redo.jpg`、`recolor-overlay-control.jpg`、`trial-after-discard.jpg`、`native-five-color-handoff.jpg`、`prepare-after-cancel.jpg`、`prepare-imported.jpg`、`return-input-retained.jpg`。

## 限制与后续优化排序

- 190 万面首次选择准备仍约 6.4–7.0 秒，在后台执行。当前先提升局部处理耗时；后续可单独评估邻接结构与射线查询缓存，需衡量额外常驻内存。
- 本轮 GUI 版本切换的相机状态保留；工具栏出现/消失使视口高度有变化，不承诺跨所有布局截图像素一致。
- 五个目标色在只有四槽的测试配置中合并到三个现有耗材，绿色会偏棕，这是耗材匹配结果；目标色与耗材色不能混为一谈。CMYW 叠色不等于已校准的 CMYK，实物结果需耗材、层高和打印测试。
- 大片杂色自动语义识别尚未实现；去杂支持带规范顶点 RGB/RGBA 的三角 OBJ，纯纹理模型不冒充已支持。下一步更有价值的是针对大片杂色的同色区域选择与边界修正，再考虑拖拽塑形。
- GUI 与构建仅在本机 Windows 验证；没有本轮实物打印、跨平台运行、实时生成服务或完整软件性能覆盖。取消导入虽然移除了模型，现有 Orca 工程仍可显示修改标记；未将它误报为保存状态完全不变。

本轮代码与预览保留在当前工作树，未提交、推送或发布。隔离预览启动入口为 `build/post-generation-preview/run-post-generation-preview.cmd`，不作为公共便携包分发。
