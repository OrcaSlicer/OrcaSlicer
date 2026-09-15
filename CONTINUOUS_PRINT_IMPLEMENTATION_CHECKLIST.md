# 零空驶连续打印(一笔画)实施清单

> 配套设计文档:`CONTINUOUS_PRINT_ZERO_TRAVEL_DESIGN.md`(下称"设计文档",章节号如 5.3 均指该文档)。
> 基线:OrcaSlicer `v2.4.2`。原则:**新功能默认关闭,不改既有行为**;每个里程碑完成后先过回归再进下一步。

---

## 2026-09-15 后续修正：真实层界终点与固定层高

按用户最新确认，连续打印的层间约束是 XY 连续：每层保持固定 Z，换层时在上一层终点原地升一个层高。本节取代此前连续打印的 Z-ramp、渐入减流和平滑插值要求；原 SpiralVase 模式仍保留原有功能。

用户的 `圆柱体_PLA_12m40s.gcode` 实际包含 75 层的 `HexagonalPrism.stl`。使用当前工程的几何与配置，在验证副本中将实例缩放到 30% 后复现了相同走线。原始工程和用户 G-code 均未改写。

- [X] 删除上一版为保留旧稀疏层衔接点而追加的实心层收尾。第 3 层在 `X125.549 Y139.043` 的实心填充末端结束，不再向外墙补画约 0.947mm。
- [X] 不再通过过滤 G-code 空驶来掩盖层界间隙。该样例第 4 层原始填充入口与第 3 层真实终点相距约 7.92mm；第 4 层从真实终点升层后沿内侧轮廓接入，局部填充避让，实际最小中心线间距约 0.449mm。普通稀疏层仍使用原规划分支，只有较远的层界入口才调用轮廓连接。
- [X] 短层界连接在几何计划中明确生成，流量按连接长度计算；常规回退后清除旧计划终点，改用实际喷嘴位置。新增测试覆盖上方有稀疏层也不追加收尾、六棱柱的较远层界入口和短接入不扭曲原始首段。
- [X] 移除连续打印的螺旋文本过滤器及其专属状态。每层按正常层高发射，换层纯 Z 抬升；不再整层渐升 Z、渐入减流或自动删除定位移动。界面中的 Smooth Spiral 参数只供原花瓶模式使用。
- [X] `tools/continuous_print_check.py` 改为检查全部模型层和固定 Z，不再按 Z-ramp 挑选连续区；支持模态角色、G92、绝对/相对坐标与 E、G2/G3，以及跨层前后两侧的空驶。`--allow-z-ramp` 仅用于审计历史输出。

验证结果：六棱柱 75/75 层、原圆柱体 130/130 层均为固定 Z、零层内与层间 XY 空驶、零 Z 回退。六棱柱第 3 层全部为 Z=0.6mm，第 4 层全部为 Z=0.8mm；第 5–69 层 XY/E/角色与用户文件逐段一致。原圆柱体移除旧收尾后墙/填充先后顺序会随真实终点改变，不能再沿用上一节对旧 G-code 逐段一致的结论；其普通稀疏规划分支未更改。

专项 C++ 测试 16 项、822 条断言通过；Python 审计测试 5 项通过；全量 libslic3r 为 131/132 通过，唯一失败仍为既有 `Placeholder parser coFloatsOrPercents vector access` SEGFAULT。1/2/3/4/6 圈墙立方体分别 49/49/50/50/49 层零层内空驶（每组 50 层）；1/2/6 圈的既有第 47 层仍回退。关闭连续打印的圆柱体输出除生成时间外与基线一致。

验证产物：`sandboxes/continuous_print/verify/handoff_before/`、`handoff_after/`、`handoff_cube/`。`handoff_after/layer3_4_handoff.png` 展示层界走线，`fixed_z_comparison.png` 展示固定 Z 与旧渐升 Z 的差异。交付切片为 `六棱柱_固定层高_连续打印.gcode` 和 `圆柱体_固定层高_连续打印.gcode`。以上结论来自几何、切片与 G-code 检查，尚未实机打印。

新程序入口为 `build/src/Release/orca-slicer.exe`，已用于上述全部切片验证。交付时旧程序仍占用 `build/OrcaSlicer/OrcaSlicer.dll`，原安装目录替换被文件锁阻止，仍是上一版；关闭旧程序后应启动新编译入口。

---

## 2026-09-15 实心层沿墙衔接与局部填充避让

本节修正 09-14 验证的不足：零空驶只能说明 XY 移动带有挤出，不能证明连接没有跨越实心填充。用户的 `圆柱体_PLA_20m22s.gcode` 在第 3 层仍有从外墙直连填充入口的长斜线。原始工程及用户提供的 G-code 均保持不变。

- [X] 仅为不含稀疏填充的实心层启用沿轮廓连接；喷嘴在外墙时，从当前位置依次打印外墙、内墙，再沿最内墙内侧的等距轮廓生成额外墙弧，连接至实心填充。喷嘴已在内部时可先接入填充，再由内向外打印墙，避免多墙时强行跨到外墙。
- [X] 按延伸墙与填充线宽之和的一半预留局部避让带。裁短进入该带的扫描线，沿剩余区域的边界连接折返及短填充连接段。完全由延伸墙替代的缝隙填充残段移除，其他段保留各自的挤出属性。
- [X] 逐线段验证填充位于避让后的区域内（坐标几何检查容差 0.002mm），同时验证整个输出链首尾精确相接；不再仅检查顶点间距或空驶计数。短直线接合仍受原有 2mm 参数上限控制，沿墙弧长不受该直线距离限制。
- [X] 最近的填充段可能位于一条可连通链的中部；当前入口失败时，尝试其他端点作为入口，再通过墙弧接入，避免多墙实心层因贪心排序误回退。
- [X] 保持稀疏层规划分支原样，并在实心层进入稀疏层前保留可用的既有衔接终点。圆柱体第 4–125 层共 122 层的每个挤出段起终点 XY、Z、E 和角色，与本轮修改前完全一致。

用原始 `圆柱体.3mf` 重新切片后的核验：

- 第 1、2、3、126、127、128、129、130 层均有沿轮廓的延伸墙；第 3 层起始段沿外墙行进，原长斜线消除。相邻实心层可因上一层出口改变而交替从墙或内部进入。
- 8 个实心层的实际 G-code 填充线段到延伸弧的最小中心线距离约 0.419–0.501mm，满足该工程对应的线宽避让；已生成逐层图及第 3 层前后对比图。该数值是此圆柱样例的结果，不作为其他配置的固定间距。
- 全部 130 层零层内空驶、零层间空驶、零 Z 回退；首层 Z=0.2mm，末层结束 Z=26.0mm。
- 同配置立方体 1/2/3/4/6 圈墙分别为 49/49/50/50/49 层零空驶（各 50 层），所有底面与最终顶面均零空驶。1/2/6 圈墙仍在既有第 47 层回退，未扩大连接上限强行连接。
- 连续打印专项：17 个测试、803 条断言通过；全量 `libslic3r`：132/133 通过，唯一失败仍为已记录的 `Placeholder parser coFloatsOrPercents vector access` SEGFAULT。关闭连续打印时，圆柱体输出除生成时间外与修复前逐行一致。

交付：`build/OrcaSlicer/OrcaSlicer.dll` 已更新并校验与 Release 产物 SHA256 一致；新的切片为 `圆柱体_沿墙连接修复.gcode`。审计、构建及测试日志、`solid_layers.png`、`layer3_comparison.png` 位于 `sandboxes/continuous_print/verify/contour_final/`，多墙产物位于 `contour_cube_final/`。以上是几何与 G-code 验证，尚未实机打印。

---

## 2026-09-14 圆柱体工程复验：顶底面缝隙填充

使用用户提供的 `圆柱体.3mf` 原始配置：Creality Hi 0.4、三圈墙、130 层、2.0mm 连接上限、平滑关闭、圆弧拟合开启；四种填充图案仍为 `monotonic` / `monotonic` / `alignedrectilinear` / `monotonic`。没有修改原始工程或原始 G-code。

原文件与修复前重新切片的结果一致：122/130 层零空驶，第 1、2、3、125、127、128、129、130 层回退。首层为“内墙→外墙→底面”，其中底面主迹之外还有 13 条缝隙填充小段；直接在主迹完成后排序这些小段，连接距离达到约 5.98mm，超过上限，导致整层回退。

- [X] 优先保留可以直接连通的完整路径排序；失败时才尝试将缝隙填充与短实心填充残段插入附近的主填充路径，原始段及其角色、线宽、流量均保留，每条新增挤出连接仍受原有距离上限约束。
- [X] 短曲线残段按弧长分段后就近接入；稀疏/实心过渡层的近闭合实心轮廓可补齐容差内的裁缝并调整接入位置。该轮廓处理仅用于含稀疏填充的层，避免把普通实心层中的窄折返残段当作环。
- [X] 连续链发射期间局部禁用圆弧拟合，使后续按 G1 弧长计算的 Z 连续抬升覆盖所有挤出段；常规回退及功能关闭时保留原设置。
- [X] 连续过滤器保留极短的实际 XY 挤出；相对 E 渐入减流后保留最小正输出，避免小段变成 `E0` 空驶。原 SpiralVase 未修改。

最终逐层 G-code 核验结果（包含平面首层，不仅统计最长螺旋连续区）：

| 项目                                       | 修复前           | 修复后           |
| ------------------------------------------ | ---------------- | ---------------- |
| 零层内空驶的模型层                         | 122 / 130        | 130 / 130        |
| 首层顺序                                   | 内墙→外墙→底面 | 外墙→内墙→底面 |
| 首层模型挤出之间的无挤出 XY 移动（含擦嘴） | 20               | 0                |
| 最终顶层模型挤出之间的无挤出 XY 移动       | 18               | 0                |
| 修复后层间空驶 / Z 回退                    | —               | 0 / 0            |

首层保持 Z=0.2mm，最后一层结束于 Z=26.0mm。首层缝隙填充耗丝量为 0.02436mm，最终顶面为 0.02880mm，与原文件一致；第 2 层仍按既有螺旋渐入机制减流。计数排除模型首段定位及打印完成后的擦嘴，保留模型挤出之间的实际空驶；核验脚本支持两种层/角色注释、G92、相对/绝对 E 和 G2/G3 端点。

验证记录：

- `libslic3r_tests.exe "[ContinuousPrint]"`：15 个测试、726 条断言通过，新增覆盖局部小段的几何/流量保留、短实心残段、过渡轮廓裁缝、窄折返不误闭合、极短挤出及已连通路径不增加绕行。
- `ctest -C Release --test-dir build/tests/libslic3r --output-on-failure`：130/131 通过；唯一失败为已记录的 `Placeholder parser coFloatsOrPercents vector access` SEGFAULT。
- 同配置立方体多墙回归：1/2/3/4/6 圈墙分别 49/49/49/50/49 层零空驶（每组共 50 层），所有底面与最终顶面均无层内空驶。仍回退的层分别为 47/47/49/无/47，不将此样例结果扩展为所有几何的保证。
- 关闭连续打印的圆柱体工程，修复前后输出除生成时间外逐行一致，圆弧拟合继续生效。
- 实际圆柱体 G-code：`圆柱体_连续打印修复.gcode`；程序 DLL 更新至 `build/OrcaSlicer/OrcaSlicer.dll`。源文件改动尚未提交。
- 复现与审计产物：`sandboxes/continuous_print/verify/cylinder_before/`、`cylinder_final/`、`cylinder_cube_final/`、`check_cylinder.py`（未跟踪）。上述结论来自切片与 G-code 验证，尚未做实机打印验证。

---

## 2026-09-13 复验：顶底面与三圈以上墙

以下结果修正此前“顶底面已接入”“多墙已解决”的过宽结论。使用同一 `solid_cube.stl`、BBL X1C 0.4 profile、2.0mm 连接上限，四种图案分别为 `monotonic` / `monotonic` / `alignedrectilinear` / `monotonic`，平滑关闭。

- [X] 有界连接模式保留完整墙环再排序，避免墙环在多个填充端点处被拆成开放片段后，按面积错误排序。严格不补线模式仍使用接合拆分。
- [X] 接合距离使用线段投影，墙先走时选择靠近上一层终点的填充入口；填充先走时墙接缝跟随实际填充出口。不会把填充出口再连回远处的入口。
- [X] 容差内仍有实际间隙时，也建立受最大连接距离约束的挤出衔接，保证发射前首尾相接；不再依赖过滤器删除空驶来掩盖间隙。
- [X] 首层有裙边/底边时，模型墙与底面仍做单链规划；准备动作保留，首层保持平面 Z，不对整层做螺旋过滤。
- [X] 修复墙环旋转时遗漏原始顶点的错误，保持墙角及轮廓周长。
- [X] 连续打印顶面在链尾收尾，不再复制最后一层做花瓶减流；开放链不适合该闭环收尾机制。原 SpiralVase 未修改。

逐层核验模型挤出之间的 XY 空驶（单独排除裙边、首段定位及打印完成后的擦嘴），结果如下：

| 墙圈数 | 底面层内空驶 | 顶面层内空驶 | 零空驶模型层 / 总模型层 | 仍回退的层（从 1 计数） |
| ------ | ------------ | ------------ | ----------------------- | ----------------------- |
| 1      | 0            | 0            | 48 / 50                 | 2、47                   |
| 2      | 0            | 0            | 48 / 50                 | 3、47                   |
| 3      | 0            | 0            | 49 / 50                 | 49                      |
| 4      | 0            | 0            | 50 / 50                 | 无                      |
| 6      | 0            | 0            | 49 / 50                 | 47                      |

剩余回退发生于内部实心/过渡填充层；这些图案在局部几何下仍可能输出多个分离实体，不能把图案名称等同于“任何模型、任何层都只有一条迹”。不提高连接上限来强行跨越远距离。上述结果仅针对该复现模型，不代表所有几何均能全程一笔打印。

验证记录：

- `libslic3r_tests.exe "[ContinuousPrint]"`：11 个测试、582 条断言通过，覆盖从墙/填充两种方向进入、1/2/3/4/6 圈墙、三种填充角色、轮廓周长及最终层不重复发射。
- `ctest -C Release --test-dir build/tests/libslic3r --output-on-failure`：126/127 通过；唯一失败仍为此前已记录的 `Placeholder parser coFloatsOrPercents vector access` SEGFAULT。
- 四圈墙输出通过 `tools/continuous_print_check.py`：连续区 49/49 层零空驶、零断链、零 Z 回退；首层模型单链单独验证通过。
- 关闭功能后，1/2/3/4/6 圈墙的修复前后输出，除生成时间与运行时对象 ID 注释外逐行一致。
- 本地复现脚本及产物：`sandboxes/continuous_print/verify/check_issues.py`、`issues_before*`、`issues_final*`；完整构建/测试日志在 `issues_baseline/`（均未跟踪）。

---

## 0. 环境构建(一次性)

执行 `build_release_vs2022.bat` 会自动完成 deps 与主工程的构建:

- [X] 0.1 安装前置:Visual Studio 2022(含 C++ 桌面开发负载)、CMake、git
- [X] 0.2 首次构建(含全部 deps,**预计数小时**):

  ```bat
  :: 在仓库根目录执行;默认 Release / x64
  build_release_vs2022.bat

  :: 开发期建议用 RelWithDebInfo(带调试信息,输出到 build-dbginfo/):
  build_release_vs2022.bat debuginfo
  ```

  脚本流程:`deps/build*` 下构建依赖 → 主工程 `build*/` 下 `ALL_BUILD` → 安装到 `build*/OrcaSlicer`。
- [X] 0.3 后续增量构建(已构建过 deps 后,只编主工程):

  ```bat
  cd build-dbginfo
  cmake --build . --config RelWithDebInfo --target ALL_BUILD -- -m
  ```
- [X] 0.4 验证测试基建可用(注意:首次配置需 `cmake .. -DBUILD_TESTS=ON` 才会生成测试目标):

  ```bat
  cd build-dbginfo
  ctest -C RelWithDebInfo --test-dir ./tests/libslic3r --output-on-failure
  ```

  PS: `debuginfo`模式下可以无报错编译成功,但产出的orca-slicer.exe无法运行,运行时无报错直接退出,无任何输出。
  PS2(2026-09-08 更新):`build/` 目录的 **Release 构建可正常运行**(GUI 子系统应用控制台无输出属正常),且已成功用于 CLI 切片验证(见 M2);`build/` 目录亦可以 `--config RelWithDebInfo` 编译并运行单测。

---

## 1. M1:数据层算子 + 判定器 + 单测(不接管线)

设计依据:5.2、5.3。此阶段**不碰 `GCode.cpp`**,所有代码独立可测。

### 1.1 开工前已敲定的决策(实现时遵循)

- [X] 端点重合容差 ε_geo:使用 `is_approx(a, b, SCALED_EPSILON)`(`Point.hpp:390-393`),写死在算子内,v1 不暴露配置项
- [X] `ExtrusionLoop` 处理:loop 无 `start_idx` 成员,seam 由 `split_at`/`split_at_vertex` 实现,且 `can_reverse()` 恒 false;算子层面把 loop 当作"首尾同点的闭合边"参与构图(不可反转);判定器 v1 只放行两类情形——①整层恰好一个 loop(退化为 SpiralVase 场景);②开放路径组成的单链(2 奇度端点)
- [X] 补线约束按 1.1 节更新后的划界:禁止事后补线;连续填充图案(gyroid/zigzag 回折)属合法原生图案
- [X] 回退策略 v1:任一判定失败 → 整单回退常规打印(5.6)

### 1.2 精确链化算子(设计文档 5.3)

- [X] 在 `src/libslic3r/ShortestPath.hpp/.cpp` 新增:

  ```cpp
  struct ExactChainResult {
      std::vector<std::pair<size_t, bool>> order; // (实体索引, 是否反转)
      Point start;   // 链首
      Point end;     // 链尾
      bool  closed;  // 首尾重合
  };
  std::optional<ExactChainResult> chain_extrusion_entities_exact(
      const std::vector<ExtrusionEntity*> &entities,
      const Point *preferred_start = nullptr);
  ```
- [X] 实现要点:

  - [X] 端点提取用基类虚接口 `first_point()`/`last_point()`(`ExtrusionEntity.hpp:118-122`);`extrusion_entity_has_endpoints`(`ShortestPath.cpp:20-41`)为 static 私有、只判非空不提取坐标,需自行过滤零长度实体
  - [X] 端点合并为顶点(scaled 坐标,`is_approx`/`SCALED_EPSILON` 判定),统计各顶点度数
  - [X] 奇度顶点 = 0 → `closed=true`;= 2 → 开放链,链首/链尾即两个奇度端点;> 2 → 返回 `std::nullopt`
  - [X] 接续条件唯一:下一段首点在当前末点 `SCALED_EPSILON` 邻域内(`is_approx`);找不到即失败。**绝不创造新连线**
  - [X] `preferred_start` 非空时优先从距其最近的合法端点起链(供层间衔接用)

  - 备注:图连通性不做独立并查集,由 Hierholzer 结束时的"消费边数 == 实体数"校验兜底(不连通图必然消费不完)

### 1.3 判定器骨架(设计文档 5.2)

- [X] 新增 `src/libslic3r/GCode/ContinuousPrint.hpp/.cpp`(已登记 `src/libslic3r/CMakeLists.txt`):
  - [X] `enum class ContinuousPrintVerdict { Applicable, Reject };`(实际命名比原计划多了 `ContinuousPrint` 前缀)
  - [X] `struct ContinuousLayerPlan`(order / start_point / end_point / is_closed / total_length / sampling);`total_length` 已 `unscale_` 为 mm(注意 `ExtrusionEntity::length()` 返回 scaled 单位)
  - [X] `preflight_layer(entities, layer, cfg, out_plan)`:调 `chain_extrusion_entities_exact`,填充 `out_plan`;形状级判定(单对象/单材料/无支撑/单岛)留到 M3(`layer`/`cfg` 参数已预留,当前 `[[maybe_unused]]`)
- [X] 弧长采样(`sampling`):沿链按 1mm 固定步长**插值**采样 XY(非仅取顶点),供后续转移点曲线与离体判定使用
- [X] v1 判定器仅覆盖层内单链判定;转移点曲线/离体检查(5.2 第 4 步)留到 M3

### 1.4 单元测试

- [X] 新增 `tests/libslic3r/test_continuous_print.cpp`,并在 `tests/libslic3r/CMakeLists.txt` 源文件清单中登记(该文件为显式列表,不自动 glob)
- [X] 实体构造参考 `tests/fff_print/test_extrusion_entity.cpp:22-36`:**注意 `ExtrusionPath::polyline` 是 `Polyline3`(三维)**,测试点要用 `Point3` 追加,`first_point()`/`last_point()` 返回其 2D 投影;`tests/libslic3r` 下没有现成的 ExtrusionEntity 构造示例
- [X] 用例(手构 `ExtrusionEntity` 集):
  - [X] 单个 `ExtrusionLoop` → 通过,`closed=true`
  - [X] 两条平行开放线(rectilinear 示意)→ 拒绝(奇度 > 2)
  - [X] 端点相接的开放折线链 → 通过,`closed=false`,起终点为两个奇度端点
  - [X] 链中间一段反向(需 flip)→ 通过且 `order` 中对应 `bool=true`
  - [X] 端点间距 > ε_geo → 拒绝(不补线断言)
  - [X] 空实体集 / 零长度实体 / 两个分离环(不连通图)→ 拒绝且不崩溃
  - [X] `preferred_start` 选择最近奇度端点为链首
  - [X] `preflight_layer` 三组:开放链 Applicable + 计划一致性、单 loop Applicable + closed、平行线 Reject
- [X] 跑通:`ctest -C RelWithDebInfo --test-dir ./tests/libslic3r --output-on-failure`(2 个新场景 40 断言全过)

### 1.5 M1 出口标准

- [X] 全部新单测通过;既有测试无回归(注:`Placeholder parser coFloatsOrPercents` 在基线上即 SEGFAULT,为既有问题,与本次纯新增改动无关;构建需降并行 `/maxcpucount:2`,否则全并行 `-m` 会编译器堆耗尽)
- [X] 不修改 `GCode.cpp` / `PrintConfig.cpp`,主程序行为零变化

---

## 2. M2:文本过滤器原型(离线)

设计依据:5.1 路线 A、5.4。

- [X] **算子扩展(接合点拆分,设计文档 3.5)**:实现为 `split_entities_at_junctions()`(`ContinuousPrint.cpp`,独立于 M1 算子):"实体 A 端点落在实体 B 中间(ε 内)→ 拆分 B"(支持 `ExtrusionPath`/`ExtrusionLoop`,loop 在接合点处线性化),`preflight_layer` 已接入,拆分工作集由 `ContinuousLayerPlan::entities` 持有供发射用。拆分不新增几何,不违反"不补线"约束。单测:lollipop 通过、双接合点桥接通过、T 型接合(4 奇度)拒绝、无接合直通
- [X] 用真实切片数据验证连续填充图案(2026-09-08,Release 构建 CLI 切片 20mm 实心立方体,BBL X1C profile,块内纯空驶统计):

  - `sparse_infill_pattern = alignedrectilinear`:**0 / 1071**(0 次块内空驶/1071 段挤出)——一层一条迹,zigzag 端部相接
  - `top_surface_pattern = monotonic`:0 / 141;`bottom_surface_pattern = monotonic`:0 / 122
  - `internal_solid_infill_pattern = monotonic`:2 / 474(仅 2 次例外,疑窄区分片)
  - 对照:Inner wall 50 / 200(每层 2 道墙环,环间不共点,各有 1 次环间空驶,符合 lollipop 接合预期)
  - 产物留存于 `sandboxes/continuous_print/`(STL/G-code,未跟踪);CLI 用法:`orca-slicer.exe --slice 0 --outputdir <dir> --load-settings "<machine.json>;<process.json>" --load-filaments "<filament.json>" <model.stl>`(注意 `--slice` 必须带板号参数)
- [X] 在 `ContinuousPrint.hpp/.cpp` 中实现过滤器,泛化自 `SpiralVase::process_layer`(`SpiralVase.cpp:66-216`),复用四步机制:

  - [X] 首条纯 Z 移动改写(保持 Z 单调)
  - [X] Z-ramp(按弧长比例摊层高)
  - [X] XY 平滑滑移(复用 `spiral_mode_smooth` 开关;预算经 `set_max_xy_smoothing` 注入,离体检查留 M3 hook)
  - [X] 跳过 travel/回抽行

  - 注:`SpiralVaseHelpers` 已迁移至 `SpiralVase.hpp`(inline)供两个过滤器共用
- [X] 离线验证:单测自动对比——同一单层 G-code 分别喂入 SpiralVase 与 ContinuousPrint,输出**逐字节相等**;另断言零空驶(任何 XY 移动必带挤出)与 Z 单调递增至层高
- [X] 人工检查斜壁样张的转移点曲线(连续、无突变)。已在 M3 管线接入后用真实模型验证:平滑开启时层界无 Z 回退、挤出段 Z 单调升至层高(见 M4 实测)

### M2 出口标准

- [X] 闭合环输入下,过滤器输出与 SpiralVase 等价(单测逐字节断言)
- [X] 开放链输入下,Z 单调、无空驶段(单测断言;Δ 预算与离体检查的强制执行为 M3 范围)

---

## 3. M3:管线接入

设计依据:5.4、5.5。

- [X] `src/libslic3r/PrintConfig.{hpp,cpp}`:新增 `continuous_print_mode`(bool,默认 false,comAdvanced);平滑参数复用 `spiral_mode_smooth` / `spiral_mode_max_xy_smoothing`,不新增配置面。开启时在 `normalize_fdm`/`normalize_fdm_1` 中禁用 `retract_when_changing_layer`(连续打印靠挤出段焊接层界,被过滤器丢弃的回抽会留下未配对的回填)。**未加硬校验/提示**:逐层判定失败会自然回退常规打印,不阻断切片
- [X] `src/libslic3r/GCode.hpp`:`LayerResult` 扩展 `continuous_print_enable`(bool);`GCode` 新增 `m_continuous_print`/`m_continuous_prev_end`/`m_continuous_has_prev` 与 `emit_continuous_print_layer()`/`continuous_print_compatible()`
- [X] `src/libslic3r/GCode.cpp`:

  - [X] `_do_export`(2539 附近):`continuous_print_mode` 开启且结构门通过时实例化 `ContinuousPrint`(与 `spiral_mode` 互斥,spiral 优先)
  - [X] `process_layer`(发射段 5454 附近):整单结构门(`continuous_print_compatible`:单对象单实例、单材料/单喷嘴、无支撑、无擦料塔、ByLayer;skirt/brim 按层排除)+ 逐层 `preflight_layer`,命中时整层发射为单条连续链;失败层常规发射
  - [X] spiral 判定/发射管道:两处 `process_layers` 的过滤器槽位改为 `has_vase_filter = m_spiral_vase || m_continuous_print` 分派,避免两套判定冲突
  - [X] 发射分支:`emit_continuous_print_layer()` 按 `plan.order` 顺序/反转逐段 `extrude_path`;`preflight_layer` 已把 loop 线性化为开放 `ExtrusionPath`,故无需 seam/裁剪代码;连续实体首尾相接,`_extrude` 的 `travel_to`/`needs_retraction` 无可触发段
  - [X] `change_layer`/`lazy_lift`:连续模式下与 spiral 同法强制普通 Z 提升(否则过滤器会把 z-hop 的 Z 误当作层高基准,导致整层 Z 偏移)
- [X] 转移点判定(5.2 第 4 步):以「上一层终点」为 `preferred_start` 选链方向(首个连续层用当前喷嘴位置),层界由过滤器丢弃 travel 后焊接;开启 `spiral_mode_smooth` 时在发射前校验 Δ ≤ `spiral_mode_max_xy_smoothing` 预算,超预算即该层回退常规。**离体检查未做**(需把网格/层多边形引用传入,留待 M4 几何合法性)
- [X] **接合点容差与吸附(2026-09-11 关键修复)**:实测真实切片中墙与填充之间留有 `infill_wall_overlap` 级别的缝(顶/底面 ≈ 0.12mm,内部实心 ≈ 0.48mm),而原实现用 `SCALED_EPSILON`(≈1e-4mm)判定接合,导致**一直接不上、特征完全不生效**。现改为把物理容差(默认 `0.5 × 喷嘴直径`)传入 `preflight_layer`/`split_entities_at_junctions`,并在拆分目标的同时把接触端点**吸附**到接合点(位移 ≤ 容差,不新增任何线段,仍满足"不补线")。单测:`0.15mm` 缝隙在严格 ε 下 Reject、在物理容差下 Applicable 且链精确相接
- [X] **有界连接段(2026-09-11,按用户要求放宽"绝不补线")**:`preflight_layer` 改用贪心单链排序 `build_chain_with_connectors`:相邻两段间隙 > 接合容差且 ≤ `continuous_print_max_join_distance`(默认 1.0mm,可配,0=严格)时插入一段直线挤出连接(继承相邻实体属性),用于连接**墙环之间、墙↔填充/支撑**;超过上限则整层回退。发射端无需改动(连接段作为普通 `ExtrusionPath` 排在链中)。**不强制 `wall_loops=1`**(遵用户意见,多墙靠连接段串成一笔)
- [X] **多墙一笔画(2026-09-11 解决)**:排序器改为「按角色分组(外墙/内墙按面积降序 = 墙1→墙2→墙3,填充一组)+ 方向按上一层终点自动选择」;闭环置缝改为**在边上插入顶点的最近点置缝**(此前只吸附到已有顶点,墙-填充缝 0.12mm 无法精确落点,多墙因此 0/49)。实测 2 墙 + alignedrectilinear:0/49 → **42/49** 层零空驶;逐层特征序列实测为 `Outer wall → Inner wall → Sparse infill` 与 `Sparse infill → Inner wall → Outer wall` **逐层交替**,与设计目标一致
- [X] **用户提示(2026-09-11 新增)**:`GCodeProcessorResult::continuous_print_report` 携带整单结论(GUI 通知 + CLI stderr + 日志)。三种文案:结构门不通过(非单对象/单材料/有擦料塔)、无任一层可一画、部分层不可一画(N/M)。不再"静默按常规切"
- [X] **墙→顶/底面用"不完整墙壁"衔接(2026-09-11)**:排序器在"墙→非墙(顶/底/填充)"衔接处不再插入独立连接实体,而是把填充起点**追加到墙折线末尾**,即墙沿自身切向延伸到图案起点(末段 = 不完整墙壁)。墙↔墙仍用短连接段。实测覆盖率不变(质量优化),零空驶保持;1 墙 47/49、2 墙 45/49、纯薄壁 50/50
- [X] **顶/底面接入(2026-09-11)**:墙环的缝改为对齐"填充连接点"(反向逐道墙置缝),填充块按邻近排序;实测 `top_surface_pattern=bottom_surface_pattern=monotonic` 时**顶面层/底面层已一笔走完**(1 墙配置 47/49 层,仅内部实心填充层回退)。`continuous_print_max_join_distance` 默认由 1.0mm 调到 **2.0mm**(墙↔面实测缝 1.18mm)
- [ ] **仍回退:内部实心填充层**。`CP_DEBUG=1` 诊断显示这些层的填充在 `by_region` 里是 **3 个实体**(role=5,点数 119/6/4),彼此相距可达 **24mm**——这是真实几何间隔(中间为空),不是排序问题;要一画只能补 24mm 长连接,不可接受,故按层回退并在提示中体现
- [X] 诊断开关:排序器支持 `CP_DEBUG=1` 打印每层实体清单(role/闭环/点数)与失败 hop
- [X] 底实心层/顶封口:靠逐层判定天然处理(底层实心/顶封层不可一画→常规打印,其余层连续),语义等同 `transition_in`
- [X] (可选)GUI:Print 设置页「Special mode」新增 `continuous_print_mode` 勾选(默认关);`ConfigManipulation` 中 `spiral_mode_smooth`/`spiral_mode_max_xy_smoothing` 的可见性同时受 `continuous_print_mode` 控制(复用其平滑参数)

  - **坑(已修)**:新增配置项必须同时登记进 `Preset::print_options()`(`src/libslic3r/Preset.cpp` 的 `s_Preset_print_options`)。该列表决定 process preset 配置包含哪些键;**只加 def + GUI 行而不登记会点「Others」选项卡崩溃**(页面对缺失键取值为 null)。已加回归单测 `continuous_print_mode is registered as a process preset option` 守卫;同一坑适用于今后任何新增打印选项

### M3 出口标准

- [X] 开关关闭时零回归:选项缺失 vs `continuous_print_mode=0` 两次切片,除 `print_settings_id` 注释外 G-code **逐字节相等**(结构上 `m_continuous_print` 仅在该选项为真时创建,关闭路径与基线同构)
- [X] 开关开启 + 花瓶类模型:生成连续 GCode(见 M4 实测);判定失败模型(2 墙 + grid 填充实心立方)无任何 Z-ramp 行,整单回退常规,输出合法

---

## 4. M4:贯通 + 端到端验证

设计依据:6.2。

- [X] 零空驶断言工具(离线,可自动化):`tools/continuous_print_check.py`,解析输出 G-code 统计
  - [X] 层内挤出段之间"非挤出 XY 移动"段数(应为 0)
  - [X] 层内 G1 挤出是否首尾相连成单链(链断点计数)
  - [X] 层界非挤出重定位段数;挤出段 Z 单调性(含 Z 回退检测)
- [X] 端到端实测(2026-09-11,Release 构建 CLI,BBL X1C 0.4 profile):
  - **修复前**:即使用文档指定的连续图案(`wall_loops=1` + `top/bottom=monotonic` + `sparse=alignedrectilinear`),Z-ramp 行数 = 0,特征完全不生效
  - **修复后(接合点容差+吸附)**:同一模型 51 层中连续区 45/49 层零空驶单链(含外墙↔顶面、外墙↔底面、外墙↔sparse/internal bridge);仅"内部实心填充层"(与墙缝 ≈0.48mm 超容差)与支撑层回退常规
  - 纯薄壁/无填充(1 环)场景保持 PASS:50/50 层零空驶、Z 单调
  - 默认参数(2 墙 + grid 填充)不可一画 → 现在会明确提示"没有任何一层能一画",而非静默
  - 复现:`--load-settings "<machine>;sandboxes/continuous_print/profiles/cp_1.json"`(纯薄壁)或 `cp_real_on.json`(1 墙+monotonic 顶/底+alignedrectilinear 填充),再 `python tools/continuous_print_check.py <dir>/plate_1.gcode`
  - 产物:`sandboxes/continuous_print/verify/final2_cp_*`、`cp_real_on_v4`、`cp_default_on_v4`(未跟踪)
- [ ] 几何合法性:平滑滑移/层界焊接段采样点落在模型切片多边形/体积内(防穿模/悬空)。**未做**,需把层多边形或网格体积引用传入过滤器/发射器(设计文档 G4、5.4 方案 A),为下一步
- [ ] 打印实测:小尺寸薄壁花瓶(直壁 / 斜壁 / 非圆截面),目检层界焊缝、强度、挤出一致性,重点观察转移点曲线附近是否积料/凹陷(**需实机**)

---

## 5. 全程纪律(每个里程碑必做)

- [X] 遵循 AGENTS.md:C++17、PascalCase 类 / snake_case 函数、`#pragma once`、TBB 共享状态只读(连续打印状态 `m_continuous_prev_end` 仅在 `serial_in_order` 的 generator/过滤器阶段读写)
- [X] 不改动既有默认行为、profile、.3mf 兼容性(新选项默认 false,关闭路径与基线同构;未改任何既有配置默认值/迁移)
- [X] 每个里程碑提交前跑 `tests/libslic3r` 全量(37/38 通过,唯一失败为基线既有 `Placeholder parser coFloatsOrPercents` SIGSEGV)+ 基线 diff
- [X] 代码保持精简,review 前手动删减 AI 生成的冗余代码
- [ ] 开放问题 O1–O4(设计文档第 7 章)不在 v1 范围内,遇到相关需求先记录不实现

---

## 里程碑依赖关系

```text
环境构建(0) ──► M1 算子+判定器 ──► M2 过滤器原型 ──► M3 管线接入 ──► M4 端到端验证
   (可并行准备)      (纯数据层,可独立交付)
```
