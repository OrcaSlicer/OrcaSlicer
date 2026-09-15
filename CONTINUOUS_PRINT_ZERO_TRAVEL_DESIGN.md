# 零空驶连续打印(一笔画)研究报告与可行性设计

| 项       | 值                                                         |
| -------- | ---------------------------------------------------------- |
| 基线版本 | OrcaSlicer`v2.4.2`(本工作区 HEAD 快照)                   |
| 文档类型 | 设计 / 可行性分析(不含功能实现代码)                        |
| 日期     | 2026-09-07                                                 |
| 范围     | 层内一笔画 + 层间 Z 连续、零空驶、**不补任何连接线** |

---

## 2026-09-15 最新约束：固定 Z 的连续打印

用户进一步明确：每层固定 Z，换层时保持上一层终点的 XY 不动，只抬升一个层高。因此后文历史设计中的连续 Z、Z-ramp、螺旋渐入减流和平滑插值不再适用于 `continuous_print_mode`；独立的 `spiral_mode` 行为保持不变。

层计划的终点必须是真实最后一笔的终点，不为保持旧输出而额外收尾到外墙。下一层从该 XY 开始；较远的填充入口在新层中沿轮廓接入并局部预留空间，短连接明确进入几何计划。不得删除定位空驶后把下一条挤出误当成合法连接。普通稀疏图案与排序分支保持原样。

当前实现移除了连续打印专用螺旋过滤器；所有模型层的固定高度、层内及层间 XY 连通性均参与审计。六棱柱与圆柱体复验及相关限制见实施清单“真实层界终点与固定层高”一节。以下内容作为设计演进记录保留，与本节不一致处以本节及用户最新要求为准。

---

## 2026-09-15 实现范围更新

下文保留 09-07 的严格不补线研究前提。后续用户已授权有界挤出连接；最新要求进一步明确：实心层墙与填充之间应沿轮廓延伸一段额外墙，并局部收窄填充以避让，不能用跨越实心区域的长直线替代沿墙行进。`continuous_print_max_join_distance=0` 的严格模式继续保留。

当前实心层实现按实际喷嘴位置选择墙/填充入口；外墙入口按外到内逐圈打印，随后沿最内墙内侧的等距轮廓接入填充。额外墙与填充按线宽预留局部空间，对裁剪与重接后的整条线段验证几何合法性。含稀疏填充的层继续使用原规划分支。

验收必须同时检查沿墙形状、填充避让、层界起点和实际无挤出移动；仅有“零空驶”计数不代表几何正确。用户圆柱体第 4–125 层的 XY/Z/E/角色逐段保持不变，8 个实心层的图形与测试记录见 `CONTINUOUS_PRINT_IMPLEMENTATION_CHECKLIST.md` 的 2026-09-15 节。任意模型适用性与实机打印质量仍需分别验证。

---

## 目录

- [1. 问题定义与研究目标](#1-问题定义与研究目标)
- [2. 现状机制代码地图(研究基础)](#2-现状机制代码地图)
- [3. 数学本质与可行性边界](#3-数学本质与可行性边界)
- [4. 现有代码的缺口清单](#4-现有代码的缺口清单)
- [5. 接入点与改造方案](#5-接入点与改造方案)
- [6. 里程碑与验证方案](#6-里程碑与验证方案)
- [7. 风险与开放问题](#7-风险与开放问题)
- [8. 结论](#8-结论)
- [附录 A. 代码引用表](#附录-a-代码引用表)
- [附录 B. 相关配置项](#附录-b-相关配置项)
- [附录 C. 术语表](#附录-c-术语表)

---

## 1. 问题定义与研究目标

### 1.1 目标定义

> 让"合适的模型"在打印时,整层(乃至整个模型)只存在**一条连续挤出轨迹**:
> 每一条挤出路径的首尾与其前后路径首尾相接,整层内**零空驶**(不存在不挤出的 XY 移动);
> 且**层间 Z 连续**——第 n 层轨迹终点即第 n+1 层轨迹起点,挤出过程连续跨层进行。

约束与边界(与讨论确认一致):

1. **不补任何连接线**:为了衔接路径而在模型内部额外补画一段"装饰线"(即使藏在内侧)是被禁止的。只能在几何**天然可一笔画**的模型上生效,否则放弃该层/该模式。
   - **划界标准**:禁止的是**事后补线**(对已有路径做欧拉化修补,如在断线填充的端点之间补一段);允许的是**原生连续图案**(fill 生成器直接输出一条连续迹,如 gyroid、连续 zigzag——其端部回折属于图案本体,不算补线)。约束应施加在路径规划/后处理阶段,而非限制填充图案的生成方式。
   - **岛间补线无条件禁止**:两岛之间的连线跨越空气,是不在模型几何内的悬空挤出珠,违反几何正确性(多出实体材料、无支撑悬空),无任何放松余地。
   - **填充内补线不纳入 v1**:其效果与"选择连续填充图案"高度重叠,仅边际扩充填充图案选择而不扩大模型几何适用域,且引入过挤出、局部密度变化、表面透印等质量风险;作为有界放松项列为开放问题(见 7 节 O4)。
2. **空驶判定**:以 GCode 中"无挤出 E 变化的 XY 移动"为零空驶的定义;允许 G1 连续挤出段在层界处呈轻微斜向(处理斜壁,见 3.3)。
3. 交付物为**设计文档**(本文件),不含可编译功能代码。

### 1.2 结论摘要(先读)

1. **层内":一笔画覆盖任意填充图案"在"不补线 + 零空驶"约束下几乎必然不可行**。数学根因见 [3.2];中国邮路/欧拉化的常用补救(=把奇点成对用重复边连起来)在 3D 打印语境里等价于"重复挤出某段路径或补连接线",被约束 1 禁止。
2. **天然可行的模型类 = "逐层单条连续迹(单链)的连续形变体"**,即花瓶类(spiral vase)的推广:**允许非圆截面、允许斜壁、允许截面沿高度连续形变;也允许层内是开放路径(起点≠终点)**,只要该层所有挤出段能首尾相接成一条链。`SpiralVase` 是其中"闭合单环"的特例。
3. 因此本报告的改造主线是:**把现有 SpiralVase 管道从"单外周长"泛化为"任意单链连续迹(开放或闭合)"**,并在层发射前做两项判定/构建:
   - **层内判定**:该层所有挤出是否能在不产生空驶的前提下连成一条自洽连续迹(欧拉路径,允许 0 或 2 个奇度端点);
   - **层间转移判定**:相邻层之间满足 **上一层终点 = 本层起点**,该转移点可以在外壁、内壁或填充端,并允许逐层反向运动。
4. 层内多周回、**常规 rectilinear/平行线填充**、悬垂件、支撑、多岛在**不补线**前提下皆不在适用范围内,判定器应直接拒绝而非尽力打印;**实心顶/底与稀疏填充层**在使用 monotonic / alignedrectilinear 等连续图案且墙-填充存在共点接合时可纳入(接合点拆分机制,见 3.5)。

### 1.3 阅读指引

- 第 2 章是代码现状研究结果(纯事实,附文件:行号),供后续实现与 review 复用。
- 第 3 章给出数学判定,解释"为什么只适合这类模型"。
- 第 5 章是改造接入点映射(文件/函数/新增模块)。
- 第 6~7 章是落地路径、验证手段、风险。

---

## 2. 现状机制代码地图

> 约定:行号基于本工作区 `v2.4.2` 快照。函数名随版本可能有漂移,如需精确请以源码为准;文档中的"阶段"顺序即 GCode 生成的先后。

### 2.1 GCode 生成主链路(一张图)

```text
Print::export_gcode / _do_export (GCode.cpp:2461)
   │  按层收集：collect_layers_to_print (GCode.hpp:337-338)
   ▼
process_layers  (GCode.cpp:3660 全对象 / :3762 逐对象) ── TBB 并行管道
   generator ──► [spiral filter] ──► [pressure_equalizer] ──► cooling
            ──► fan_mover ──► [PA processor] ──► output
   generator 每层回调 GCode::process_layer (GCode.cpp:4539)
   │
   ├─ ① 判定 spiral_vase_enable (GCode.cpp:4596-4613, 详见 2.5)
   ├─ ② change_layer (GCode.cpp:4663 / 实现 5685) —— 层变(Z 移动、可选回抽)
   ├─ ③ 按 extruder/对象/岛/region 重组挤出实体 (GCode.cpp:5000-5104)
   │     → ObjectByExtruder::Island::Region (GCode.hpp:436-471)
   ├─ ④ 按对象实例排序 (GCode.cpp:5124-5133)
   │     chain_print_object_instances → sort_print_object_instances
   ├─ ⑤ 逐实例逐岛发射 (GCode.cpp:5432-5469)
   │     extrude_perimeters(...)         每 Region 的 perimeter
   │     extrude_infill(...)             每 Region 的 infill(内部先链化)
   │     extrude_infill(..., ironing)
   │        └─► extrude_entity (GCode.cpp:6087)
   │              ├─ extrude_path      (6103)
   │              ├─ extrude_multi_path(6038)
   │              └─ extrude_loop      (5744)
   │                    └─► _extrude (GCode.cpp:6345) —— 单段实际输出
   └─ 层间/路径间纯移动由 travel_to (GCode.cpp:7350) 发射,
        回抽决策 needs_retraction (GCode.cpp:7528)
```

后置过滤器(spiral/PA/cooling 等)对**整层文本**后处理,见 `process_layers` 管道装配(`GCode.cpp:3693-3755` / `3794-3853`)。

### 2.2 路径实体与分层数据结构

| 类型                                 | 说明                                                                  | 关键位置                                |
| ------------------------------------ | --------------------------------------------------------------------- | --------------------------------------- |
| `ExtrusionEntity`                  | 抽象挤出实体                                                          | `ExtrusionEntity.hpp`                 |
| `ExtrusionPath`                    | 单条折线(如一段填充线、一段 wall)                                     | 同前                                    |
| `ExtrusionMultiPath`               | 一组有序路径                                                          | 同前                                    |
| `ExtrusionLoop`                    | **闭合环**,`first_point()==last_point()`;seam 通过 `split_at`/`split_at_vertex` 实现(无独立起点索引成员) | 同前(约 :444-531)                   |
| `ExtrusionEntityCollection`        | 可排序容器(内部`entities` + `no_sort` 等)                         | `ExtrusionEntityCollection.cpp:87-95` |
| `LayerRegion::perimeters / fills`  | 每区域周长/填充集合                                                   | `Layer.hpp`                           |
| `ObjectByExtruder::Island::Region` | GCode 阶段每个"岛×print_region"的`perimeters`/`infills` 指针数组 | `GCode.hpp:443-471`                   |

要点:

- **周长为闭合环**:`ExtrusionLoop` 天然首尾相接(打印时从 seam 出发绕一圈回到 seam),单环本身**零空驶**。
- **填充/支撑为开放折线**:一堆互相断开的 `ExtrusionPath`。这是"一笔画"难度的真正来源。
- `Region::append` 负责把切片阶段收集的实体填入岛;若 `eec->can_sort()==false`,整个集合被作为一个不可拆实体保序处理(`GCode.cpp:8222-8258`)。
- GCode 阶段的"岛"划分是运行时做的:遍历每 region 的每批实体,按实体起点是否落在某切片多边形(`point_inside_surface`)确定归属岛(`GCode.cpp:5055-5069`),岛本身不直接保留切片阶段的多边形拓扑。

### 2.3 层内现有排序(减少 travel 的已有机制)

现有手段都只在"某一段同类实体"内做**贪心最近邻链化**,目标是尽量少 travel、把相邻实体首尾对接近,但**从不消除** travel:

| 位置                                                                                     | 作用                                                                   |
| ---------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| `chain_and_reorder_extrusion_entities` `ShortestPath.cpp:1056-1069`                  | 核心入口:丢弃无端点实体 → 计算顺序 → 按需反转实体以减少连接空驶      |
| `chain_extrusion_paths` / `chain_segments_greedy` `ShortestPath.cpp:1071-1094`     | 贪心最近邻(带起点)排序实现                                             |
| `extrude_infill` `GCode.cpp:6150-6176`                                               | 先把本层 infill 链化(6164),再对每个集合内部`chained_path_from`(6168) |
| `extrude_support` `GCode.cpp:6178-6235`                                              | 同理链化 support(6201,`no_sort` 时保序)                              |
| `PerimeterGenerator` `PerimeterGenerator.cpp:210/500`                                | 周长多段生成时的链化                                                   |
| `TreeSupport` `Support/TreeSupport.cpp:1688`                                         | 树形支撑实体链化                                                       |
| `ExtrusionEntityCollection::chained_path_from` `ExtrusionEntityCollection.cpp:87-95` | 以当前喷嘴位置为起点再链一次                                           |

注意 `GCode.cpp:5443` 的注释:逐 region 打印时"路径未做任何优化,按定义顺序发射";`5432` 也有 `FIXME order islands?`。即**跨 region / 跨岛 / 跨实例的顺序并没有真正的空驶最小化**,travel 成本由切片阶段与上面的局部链化共同决定,架构上没有"全层统一排序"的概念。

### 2.4 空驶(travel)来自哪些点

对一个"单挤出器、无支撑、无裙边、单对象"的理想层,空驶仍然必然出现在:

1. **岛与岛之间**(同一层多个不相交切片区域);
2. **不同 print_region 之间**(每 region 有自己的 wall 数量/材料,`Region` 之间按定义顺序发射);
3. **perimeter → infill 切换**(发射 `extrude_perimeters` 完到 `extrude_infill` 时,起点必然跳变,`GCode.cpp:5452-5468`);
4. **开放折线的端点不匹配**(即使链化后,一条填充线的末端与下一条填充线的首端通常存在间隙——这本身就是"空驶 + 一次回抽"的来源);
5. **每段路径的抬升/回抽**:`travel_to`(`GCode.cpp:7350`)在起终点间生成非挤出移动;是否回抽由 `needs_retraction`(`GCode.cpp:7528`)决定;`retract_when_changing_layer` 会在层变回抽(`GCode.cpp:5206-5208`, `5541-5543`)。
6. **避障绕行**:`reduce_crossing_wall`(绕行已打印区域)会让 travel 变成多段折线(`GCode.cpp:7416-7427`)。

因此"整层零空驶"在标准管线里从未被当作目标;**每条 `ExtrusionEntity` 的发射时刻,若其起点 ≠ 当前喷嘴位置,就隐含一次 travel**,该决策逻辑位于 `_extrude`/`extrude_loop`/`extrude_path` 的起手处与 `travel_to`。

### 2.5 SpiralVase / Smooth Spiral 现状(层间连续机制)

这是全代码库中唯一"层间连续、无抬刀、不回抽"的生产实现,是本文改造的**直接母体**。

启用判定(`GCode::process_layer`, `GCode.cpp:4596-4613`):

```cpp
m_enable_loop_clipping = true;
if (m_spiral_vase && layers.size() == 1 && support_layer == nullptr) {
    bool enable = (layer.id() > 0 || !print.has_brim())
               && (layer.id() >= skirt_height && !print.has_infinite_skirt());
    if (enable)
        for (const LayerRegion *lr : layer.regions())
            if (lr->region().config().bottom_shell_layers > layer.id()   // 仍在打底层
             || lr->perimeters.items_count() > 1u                        // 多于一条周长
             || lr->fills.items_count() > 0) {                           // 存在填充
                enable = false; break;
            }
    result.spiral_vase_enable = enable;
    m_enable_loop_clipping = !enable;   // 启用时关闭 loop 裁剪,保证单环完整
}
```

即:**vase 只允许"每层恰好一条 perimeter、无填充、已完成实心底"的单对象单材料模型**。

后处理管道(`GCode.cpp:3693-3706` 与 `3794-3806`,逐对象/全对象两分支同构):

```cpp
const auto spiral_mode = tbb::make_filter<LayerResult, LayerResult>(
    [&spiral_mode = *this->m_spiral_vase.get()](LayerResult in) {
        spiral_mode.enable(in.spiral_vase_enable);          // 逐层开关
        bool last_layer = ...;
        return { spiral_mode.process_layer(std::move(in.gcode), last_layer), ... };
    });
```

`SpiralVase::process_layer`(`GCode/SpiralVase.cpp:66-216`)对**整层文本**做四件事:

1. **首条纯 Z 移动改写**(`:136-141`):把"层开始抬 Z"的命令改写为冗余移动到"上一层的 Z",从而保持 Z 单调渐进,由后续挤出段把 Z 从旧值线性推到新值。
2. **Z-ramp**(`:162-163`):对每个挤出移动按累计弧长比例 `factor = len/total_layer_length`,把本层层高 `h` 摊到整条轨迹上:`Z = z_prev + factor * h`,层内挤出连续上升。
3. **平滑螺旋 XY 滑移**(`:164-192`,仅 `spiral_mode_smooth`):把本层轨迹按弧长 `factor` 向上一层轨迹做线性插值 `P = P_prev·(1-factor) + P_cur·factor`。这使"非竖直壁"下相邻层轮廓的 XY 差被**融进整圈过渡**,而不是在接缝处一次性跳变。允许滑移距离上限为 `spiral_mode_max_xy_smoothing`。
4. **跳过 travel 与回抽行**(`:134-135`, `:196-202` 注释):回抽行、过短段直接丢弃;travel 行被跳过,等效于让"层终点→层起点"的首段挤出移动在 XY 平面上带挤出地"焊接"起来。注释明确说明:当相邻层 loop 在 XY 不对齐时,首段的平滑度决定焊缝质量。

`m_previous_layer`(`SpiralVase.hpp:47`)保存上一层采样 XY 点序列,供本层平滑插值使用;首层进入过渡(`enable()` 时 `m_transition_layer=true`,流量从 `spiral_starting_flow_ratio` 渐入,末层流量渐出,`SpiralVase.cpp:148-161`)。

> **为什么现有实现不适用于一般模型**:判定在 `GCode.cpp:4603-4605` 已经把适用范围钉死为"单 perimeter、无 fill、完成底层"。它是文本后处理器,不感知模型体积,无法判断"若把填充线也连成一条会穿模/悬空"之类问题。

### 2.6 接缝(seam)的现状

- 每个 `ExtrusionLoop` 需要选一个起点(seam)。OrcaSlicer 用 `SeamPlacer`(`GCode/SeamPlacer.cpp/.hpp`,`GCode.hpp:508` 为实例)在打印时结合几何/涂抹选择切缝,偏好隐藏边/对齐。它服务的是"单环内起终点 = seam 点",与层间连续无关。
- 在 vase 螺旋语境里,我们关心的不是单个环的 seam 好看,而是**相邻层 seam/转移点集合连成的 3D 曲线是否平滑贴合壁面**,见 3.3。

---

## 3. 数学本质与可行性边界

### 3.1 把问题形式化

把某一层(或整个模型)的挤出轨迹看作无向图:

- 顶点 = 所有路径端点(含周长环上的 seam 起点/终点同一化);
- 边 = 挤出段本身(要求打印时"边被完整走一次且只走一次、方向不限")。
- 空驶 = 在图上"没有边、却移动"的过程。

于是:

- **层内一笔(可走所有边恰好一次、无空驶)= 图存在一条欧拉迹(所有边一次经过)**。但 3D 打印还有更强的"边不可拆分、顶点无复用"要求:喷嘴从边 A 末端走到边 B 首端若恰好重合于同一顶点则无空驶;若两线端点不重合,即使在同一顶点邻域也不允许"补接线段"。
- **整层仅一条挤出轨迹 = 覆盖所有边的单条连续链/环**(无需走回起点,但如果闭合则更利于层间衔接)。

关键结论:**任何"图"模型都被两个额外现实削弱**:

1. 3D 打印的"边"是带位置坐标的短线段,不同挤出段之间**极少恰好端点重合**(切片器的平行填充线彼此分离);
2. 禁止补线 → 不许人为增加"重复/新增边"去修复奇点(经典的"中国邮路问题"解法恰恰是添加重复边)。

因此,一般实心填充/多道墙模型在约束 1 下**从根上不可行**(见 3.2),这不是算法没写好的问题,而是目标与几何的矛盾。

### 3.2 层内"一笔画"的精确条件(不补线版)

设某层的挤出实体集 `E = {e_i}`,每条 `e_i` 有首点 `s_i`、末点 `t_i`。把坐标相同的端点合并为同一个顶点后得到无向图:

- **层内零空驶一笔覆盖 ⇔ 该图存在一条欧拉迹**,即奇度顶点个数为 **0 或 2**。
  - `0` 个奇度顶点 → 一条**闭合迹**(如 spiral vase 的单环);
  - `2` 个奇度顶点 → 一条**开放迹**,链首与链尾是两个不同的端点。
- 喷嘴从边 `A` 的末端走到边 `B` 的首端时,**只有两坐标严格重合才算无空驶**;若端点不重合,任何微小位移都不是"自然挤出段",要么空驶(违规)要么补线(违规)。

**为什么普通填充仍无法一笔覆盖(以你的示意图为例)**:
图中红色 rectilinear 填充线是彼此分离的平行线段,每条线段有两个独立端点,端点之间没有和相邻填充线、也没有和外壁共点 → 图的奇度顶点远多于 2,不存在欧拉迹。因此即使层内不要求"闭合",这种填充仍然不能在不补线的前提下零空驶走完。只有当填充本身被生成为一条连续链(例如连接内外壁的连续 zig-zag、或 gyroid/monotonic 等能在层内形成连续截面的模式),或者模型没有内部填充、仅由单 wall 环组成时,才可能通过判定。

"通过把路径端点间 tiny 距离< ε 视作相接"是一种工程近似,但任何非零 ε 都会引入"极小位移的挤出",在硬约束(不允许补线、不允许无 E 移动)之间是死结:ε 段要么挤出(≈补一段短珠)要么空驶(违规)。

所以层内可一画的实用判定收敛为:

> **该层恰好可被组织成一条连续挤出迹(开放或闭合),且组织过程不产生任何额外挤出或空驶。**

对标准切片输出,绝大多数情形仍对应**"单 perimeter 薄壁环"**(这正是 vase 的现状);对"两层壁贴合打印"等,由于两条环之间没有天然端点相接,判定为不可行(拒绝),而不是强行螺旋。

### 3.3 层间连续:转移点曲线(transition curve),而非"接缝列"

早期结论曾把层间要求写成"各层 seam XY 完全一致(一条竖直 seam 列)",经讨论修正为"接缝曲线",现在**进一步放宽**:

> 层间衔接只需要 **上一层终点 = 本层起点**。这个公共点称为**转移点(transition point)**。它不一定是外壁的 seam,也可以在内壁、填充端、或任何挤出段端点;本层运动方向甚至可以与上一层相反(例如从填充终点返回外壁,再到下一层外壁起点)。所有相邻层的转移点在 3D 中连成一条**转移点曲线**。

#### 3.3.1 为什么只需要"上终=本起"

因为零空驶的定义是"任何移动都伴随挤出"。上一层结束后,喷嘴位于该层的**终点**;要开始下一层,唯一不空驶的办法就是:下一层的挤出轨迹从这个坐标开始。至于上一层起点在哪里、本层终点在哪里,都不影响"本层自身无空驶";它们只影响下一层的衔接。因此层间连续是一个**有向衔接**:

```
Layer n:   start_n ──(连续挤出)──> end_n
                                      │
                                      │ 必须重合(同 XY,下一层 Z)
                                      ▼
Layer n+1: start_{n+1} = end_n ──(连续挤出)──> end_{n+1}
```

#### 3.3.2 倾斜壁与 XY 偏移预算

若转移点落在倾斜壁上,相邻层的转移点 XY 会逐层漂移,漂移量仍满足 `Δ ≈ h·tanθ`。该偏移不能靠空驶弥补,而必须靠**转移点处的一段斜向挤出段**承担。当转移点固定在外壁时,这段斜向过渡就是 vase 模式里"首段挤出熔合 seam"的物理本质(`SpiralVase.cpp:196-202` 注释);当转移点落在填充端时,道理相同:上一层的填充终点与下一层填充起点之间的 XY 偏移必须被一段挤出段吃掉。

现有 `smooth_spiral` 机制(`SpiralVase.cpp:164-192`)把这类 XY 偏移按弧长比例 `factor` 摊到本层轨迹的前若干段(或整圈),使过渡珠不会集中在一点;其预算 `spiral_mode_max_xy_smoothing`(默认 200% 喷嘴直径,`PrintConfig.cpp:5844-5855`)仍然适用。只不过现在比较的不是"上下两层同弧长采样点",而是:

```
对每个 n:
 1) 求 Layer n 的终点 P_end(n) 与 Layer n+1 的起点 P_start(n+1);
 2) 令 Δ = |P_start(n+1) - P_end(n)|;          // 转移点 XY 漂移
 3) 要求从 P_end(n) 到 P_start(n+1) 的过渡挤出段落在模型实体体内(离体检查);
 4) (使用 smooth 时)Δ ≤ spiral_mode_max_xy_smoothing;
 5) 否则:判定该层不适合 → 整体拒绝该模式。

典型斜壁:Δ ≈ h·tanθ。h=0.2mm、θ≤45° 时 Δ≤0.2mm,
远小于默认预算(~0.8mm@0.4mm 喷嘴),即连续渐变薄壁天然通过。
```

#### 3.3.3 反向运动的合法性

若 Layer n 的方向是"外壁 → 填充",Layer n+1 可以是"填充 → 外壁"。此时 `P_start(n+1)` 就是 Layer n 的填充终点,`P_end(n+1)` 会落在 Layer n+1 的外壁某点。只要 `P_end(n+1)` 又能作为 `P_start(n+2)` 被 Layer n+2 承接,整串仍然连续。因此允许的不仅是"同方向堆叠",还有"往复扫描"式的层间衔接。

层界处唯一被禁止的是:以不挤出方式把喷嘴从上一位置搬到下一层起点(=空驶),以及为弥补该距离而凭空挤出(=补线)。允许的只有"由连续轮廓自然生成的斜向挤出过渡"。

### 3.4 "合适模型"的完整画像(可行集合)

- 几何为**沿高度连续形变的单连通薄壁壳**,任意水平截面是**单连通、无孔洞、无分离多环**;壁横截面允许从圆滑过渡到方、到异形。
- 层内挤出图满足欧拉路径条件(0 或 2 个奇度端点)。常见满足情形:
  - **单 wall 薄壁环**(闭合,0 奇度)—— 现有 `SpiralVase` 已覆盖;
  - **单 wall + 连续填充**(开放,2 奇度)—— 例如把填充线设计成一条连接内外壁的连续 zig-zag,从外壁 seam 进入、穿越填充、终点落在另一侧外壁或内壁,下一层从该点反向走出;
  - **无填充的开放/闭合壳体**(2 或 0 奇度)。
- **不允许的填充**:标准 rectilinear / grid / 星形等生成多条互不相连平行线的模式(奇度顶点>2)。
- **允许的连续填充图案**(经代码核实,`FillRectilinear.cpp` 通过沿内轮廓的连接段把平行线接成单条迹,见 `connect_monotonic_regions` 等):配合下列配置,**实心顶/底层与稀疏填充层可纳入可行域**——
  - `top_surface_pattern = monotonic`(顶面)
  - `bottom_surface_pattern = monotonic`(底面)
  - `sparse_infill_pattern = alignedrectilinear`(稀疏填充,zigzag 连续)
  - `internal_solid_infill_pattern = monotonic`(内部实心填充)
  此时每层走线为"外墙一笔 + 填充一笔",二者通过**接合点拆分**连成单层一条迹,机制见 3.5。
- **墙-填充接合(不补线的前提)**:填充迹端点必须落在墙 loop 上(ε 内);monotonic 的连接段沿内轮廓走,端点天然靠近轮廓,接合点由判定器在 ε 容差内确认,确认不了即拒绝,**绝不为接合而补画线段**。
- 打印参数:单对象、单材料/单挤出器、无支撑、裙边/底完成后进入(或把底壳纳入"可一画的实心层"特判)、单 wall 或多 wall 但 wall 环之间必须端点共点(现实中极少)。
- 特判可豁免项:底部实心层(SolidBottom)在螺旋过渡层之前允许常规分段打印(现有 `transition_in` 也这样处理,`SpiralVase.cpp:148-151`);顶部若需封口,要么不做(敞口),要么做一段"常规收尾层"(牺牲最后几层的连续,通常可接受)。
- **明确不可行**(直接拒绝):任意带**常规 rectilinear/断线填充**的层(未使用上表连续图案时)、多 wall 且环间不共点层、多岛层、支撑层、墙-填充无共点接合的实心层。若模型只有个别层违规,则"放弃该模式"整单回退常规打印(保守策略),或将来探索"违规层常规、其余层螺旋"的混合过渡(风险较高,列为开放问题)。

### 3.5 墙-填充接合机制(lollipop 扩展)

当一层同时包含墙 loop 与连续填充迹时,二者初始是**两个不连通分量**(M1 算子直接拒绝)。要在"不补线"前提下合成单层一条欧拉迹:

1. **共点接合**:填充迹的某个端点落在墙 loop 上(ε 容差内)。该接合点在 loop 中间,需把 loop 在接合点处**拆分**(seam 置于接合点,代码现成:`ExtrusionLoop::split_at` / `split_at_vertex`)。
2. **奇度配对**:拆分后接合点度数 = 3(墙进、墙出、填充),填充迹远端度数 = 1 → 恰好 **2 个奇度顶点**,存在开放欧拉迹(棒棒糖图:从填充远端 → 接合点 → 绕墙一圈回到接合点结束;或反向)。
3. **拆分 ≠ 补线**:不新增任何几何,只在已有路径上重新划分顶点,约束 1 不受影响。
4. **算子扩展(M2/M3)**:`chain_extrusion_entities_exact` 需支持**接合点拆分**——检测"实体 A 的端点落在实体 B 的中间(ε 内)",将 B 拆成两段后再做欧拉判定。拆分会使实体数增加,判定器需输出拆分后的实体序列供发射阶段使用。
5. 多墙(内外墙 loop 嵌套)同理可串联:内墙 loop 在接合点拆开后,其两端分别接外墙接合点与填充端,形成"填充 → 内墙 → 外墙"的链。多一个墙环就多一对接合点,奇度顶点数仍可保持 ≤ 2。

---

## 4. 现有代码的缺口清单

| #  | 缺口                                                                                                                     | 证据                                                                     |
| -- | ------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------ |
| G1 | 层发射顺序按 region/island/实例硬编码,层内没有一个"全实体统一排序/单链化"的机会                                          | `GCode.cpp:5432-5469`(含 `FIXME order islands?` 与 5443 注释)        |
| G2 | 空驶是"发射时刻必然行为",不存在"拒绝空驶"的断言层                                                                        | `travel_to` `GCode.cpp:7350`;`needs_retraction` `GCode.cpp:7528` |
| G3 | SpiralVase 硬绑定"单 perimeter 单环",判定写死`perimeters.items_count()<=1 && fills 空`                                 | `GCode.cpp:4596-4613`                                                  |
| G4 | SpiralVase 是文本后处理,不知道模型体积 → 无法判断"XY 滑移/过渡是否离体、是否穿模"                                       | `SpiralVase.cpp:66-216` 全部基于 G-code 行解析                         |
| G5 | 没有"层内单链化"算子:即使实体集合能连成单链,也没有代码尝试把多实体重排成一条连续迹(开放或闭合;现有链化只服务"降 travel") | 2.3 节                                                                   |
| G6 | 层界衔接依赖 seam/转移点对齐几何;没有对"转移点曲线"做连续性/离体检定                                                     | 2.5、2.6 节                                                              |

---

## 5. 接入点与改造方案

总原则:**新功能默认关闭、独立过滤器,不改变既有行为**;先做判定器与数据层算子(可单测),再接管线。

### 5.1 推荐的总体架构(两条路线)

- **路线 A(本报告推荐,MVP 最快)**:把 SpiralVase 的"层内一条轨迹"从"单 perimeter loop"泛化为"任意单链连续迹(开放或闭合)",新增一个独立后处理过滤器 `ContinuousPrint`(平行于 `SpiralVase`),复用它全部四步机制(Z-ramp、XY 滑移、travel/retract 过滤、transition in/out),仅替换两处:
  1. 前置 **层内单链化算子**(5.3),允许开放路径;
  2. 前置 **转移点曲线/离体判定器**(5.2)。
- **路线 B(远期)**:在切片/路径规划阶段就输出"一笔画路径"(每层单条折线环),把问题前移。改动面极大(影响 fill/perimeter 生成、所有 profile),不建议作为第一步。

### 5.2 判定器(Preflight)

**模块**:新文件 `src/libslic3r/GCode/ContinuousPrint.hpp/.cpp`(建议),内含:

```
enum class Verdict { Applicable, Reject };

struct ContinuousLayerPlan {
    // 层内单链结果(顺序 + 每段是否反转)
    // 可以是闭合链(is_closed=true, 首尾 XY 重合)或开放链(is_closed=false)
    std::vector<std::pair<size_t, bool>> order; // (实体索引, 是否反转)
    Point       start_point;                     // 链首 XY
    Point       end_point;                       // 链尾 XY
    bool        is_closed = false;               // true: 首尾重合
    double      total_length = 0;                // 供 Z-ramp 用
    std::vector<Point> sampling;                 // 供转移点曲线/离体判定用
};

Verdict preflight_layer(
    const std::vector<ExtrusionEntity*> &entities,
    const Layer                         *layer,       // 用于几何/离体检查
    const PrintConfig                   &cfg,
    ContinuousLayerPlan                 *out_plan);
```

判定流程:

1. **形状级**:单对象、单材料、无支撑、无多岛(可复用/扩展 `GCode.cpp:4599` 的判定入口),层内实体需来自同一拓扑岛;
2. **层内单链判定**:构建端点图,检查奇度顶点个数是否 **≤2**;若 0,`is_closed=true`;若 2,`is_closed=false`,并标记两个奇度端点分别为 `start_point` / `end_point`;把顺序/方向解出来(贪心从任一端开始,每步只能选"下一段首点 == 当前末点");
3. **与底层/顶层的关系**:底实心层未结束则该层常规打印(作为 transition 起始);顶封口层默认 Reject(或按配置走"常规收尾"策略);
4. **转移点曲线检查**:对相邻两层,检查 `Layer n.end_point == Layer n+1.start_point` 的 XY 漂移 `Δ`,并要求过渡段落在模型实体体内;若开启 smooth,`Δ ≤ spiral_mode_max_xy_smoothing`。
5. 输出 `Verdict`;任一判定不过 → 该层(或整单,取决于策略)回退常规打印。

**判定器测试建议**放在 `tests/libslic3r`,输入为手构 `ExtrusionEntity` 集,断言判定的 on/off 与生成的单链方向。

### 5.3 层内单链化算子(复用/新建)

现状的 `chain_and_reorder_extrusion_entities`(`ShortestPath.cpp:1061`)允许段与段间存在任意间隙(它只最小化 travel),**不能直接用于"零空驶"**。因此新增一个更严的算子(建议放 `ShortestPath` 旁或新模块):

```
struct ExactChainResult {
    std::vector<std::pair<size_t,bool>> order; // (实体索引, 是否反转)
    Point start;                               // 链首
    Point end;                                 // 链尾
    bool  closed;                              // 首尾重合
};

// 仅在"可首尾相接"约束下把实体排成单链;
// 失败(奇度顶点>2 或无法连完)时返回空,表示该层不可一画。
std::optional<ExactChainResult> chain_extrusion_entities_exact(
    const std::vector<ExtrusionEntity*> &entities,
    const Point *preferred_start = nullptr);
```

实现要点:

- 端点直接用基类虚接口 `first_point()`/`last_point()`(`ExtrusionEntity.hpp:118-122`)提取;注意 `extrusion_entity_has_endpoints`(`ShortestPath.cpp:20-41`)是 static 私有函数且只判非空、不提取坐标,不可直接复用;
- 以"下一段首点必须在当前末点的极小邻域内"为唯一接续条件;判定用 `is_approx(a, b, SCALED_EPSILON)`(`Point.hpp:390-393`,`SCALED_EPSILON` 定义于 `libslic3r.h:96`),找不到即返回失败;
- 统计奇度顶点:0 个 → `closed=true`;2 个 → `closed=false`,链首/链尾即这两个奇度顶点;>2 个 → 失败;
- 此算子**不会创造连线**,天然满足"不补线"约束。

> 对现状已覆盖的 vase 几何,该算子退化为"层内只有一条 loop"(`closed=true`),与 SpiralVase 等价,可保证不回归。

### 5.4 过滤器的泛化与数据接入

把 `SpiralVase::process_layer` 的输入从"解析文本、假定单 loop"升级为"解析文本 + 携带该层判定计划(顺序/起终点/闭合标志/弧长采样/上一层采样)":

- 保留文本解析骨架与 `m_previous_layer` 采样机制(`SpiralVase.cpp:111-130`),以兼容平滑插值;
- **G4 缺口**的补法(二选一,建议 A):
  - A:后处理前(在 `process_layer` 内、组装 `LayerResult` 时)就把判定计划挂到 `LayerResult` 上(`LayerResult` 定义在 `GCode.hpp:168-180`,已有 `spiral_vase_enable` 等字段,可扩展;`GCode.cpp:4579` 是其构造点),把模型体/切片多边形引用一并传入过滤器做离体检查;
  - B:纯文本方案,离体检查在判定器阶段(GCode 阶段有 `Mesh`/layer 数据)先行完成,过滤器只做几何滑移。
- 层内单链结果需要注入:方案 A 下,`process_layer` 在 ⑤ 发射阶段(5.1 小节 `GCode.cpp:5432-5469`)若命中 continuous 模式,就**不逐 region 发射**,而是:按 `out_plan.order` 把实体(各自允许翻转)逐段接到上一条的末端,输出为**一条连续 G1 链**——这也顺带让 `travel_to`/`needs_retraction` 完全没有可触发的移动段。
- 层间过渡:过滤器需要额外知道上层的 `end_point` 与本层的 `start_point`。对闭合环,二者重合,Z-ramp 与普通 SpiralVase 完全一致;对开放路径,需要在转移点附近把从 `prev_end` 到 `curr_start` 的 XY 偏移摊入本层前段(或整层)挤出,复用 `smooth_spiral` 的插值逻辑。

### 5.5 需要改动/新增的文件与函数清单

| 文件                                             | 改动性质 | 内容                                                                                             |
| ------------------------------------------------ | -------- | ------------------------------------------------------------------------------------------------ |
| `src/libslic3r/GCode/ContinuousPrint.hpp/.cpp` | 新增     | 判定器 + 后处理过滤器(泛化自 SpiralVase)                                                         |
| `src/libslic3r/ShortestPath.hpp/.cpp`          | 新增函数 | `chain_extrusion_entities_exact`(或等价算子)                                                   |
| `src/libslic3r/GCode.cpp`                      | 小改     | `process_layer`(4539):启用判定与计划挂载;⑤ 发射分支(5432-5469)命中时走单链发射                |
| `src/libslic3r/GCode.cpp`                      | 小改     | `process_layers`(3660/3762):管道装配处插入新过滤器(与 3693-3706 同构),新配置开关控制是否实例化 |
| `src/libslic3r/GCode.cpp`                      | 小改     | `spiral_vase_enable` 判定逻辑(4596-4613)迁移/扩展为 shared 判定入口,避免两套判定打架           |
| `src/libslic3r/GCode.hpp`                      | 小改     | `LayerResult` 扩展字段(计划/单链引用或拷贝)                                                    |
| `src/libslic3r/PrintConfig.{hpp,cpp}`          | 小改     | 新开关(如`continuous_print_mode`),默认 false;选项尽量复用 spiral 平滑参数,减少配置面           |
| `src/libslic3r/Print.cpp`(若做实例级预览)      | 可选     | 传参/校验                                                                                        |
| `tests/...`                                    | 新增     | 判定器与算子的单元测试                                                                           |

> 兼容性:全部改动默认关闭,不影响既有 profile、.3mf、以及非开启模式的 GCode;开启时若任一判定失败即回退常规打印,输出仍合法。

### 5.6 边界情况与回退策略

| 情形                               | 策略                                                                                                                             |
| ---------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| 中间某层违规(出现孔、多岛、加支撑) | 保守:整单回退常规(推荐 v1);进取:违规层常规、邻层螺旋过渡,但过渡层转移点曲线会断开,列为开放问题                                   |
| 底层实心                           | 复用 transition_in 语义(`SpiralVase.cpp:148-151`),先常规打底后单链起步                                                         |
| 顶层封口                           | 默认常规收尾层(最后一层或数层)或敞口;转移点曲线终结位置需可打印(端点停头抬走属常规收尾,不算"全程零空驶",但发生在打印结束,可接受) |
| 多层数巨大且离体预算超限           | 拒绝;提示用户改用 Smooth Spiral(它已内置同一套滑移)                                                                              |
| 与冷却/压力均衡/PA 过滤器的顺序    | 新过滤器置于与 spiral 相同槽位(冷却前),行为按`3693-3755` 先例                                                                  |

---

## 6. 里程碑与验证方案

### 6.1 里程碑

1. **M1 算子与判定器**(纯数据层,不接管线):
   - 实现 `chain_extrusion_entities_exact` + `preflight_layer`;
   - 单测覆盖:单 loop(应通过且闭合)、两条平行开放线(应拒绝)、斜壁两层 Δmax 计算、突变层拒绝。
2. **M2 文本过滤器原型**(离线):对"已生成的单层 GCode(常规薄壁)"喂入泛化过滤器,离线验证 Z-ramp/滑移/travel 过滤逻辑与 SpiralVase 一致性,并人工检查斜壁样张的转移点曲线。
3. **M3 管线接入**:在 `process_layers` 槽位挂上开关与过滤器,对薄壁花瓶/斜壁花瓶/非圆截面花瓶生成对比 GCode。
4. **M4 判定 → 单链 → 发射贯通 + 端到端验证**(见下),输出样件。

### 6.2 验证手段

1. **零空驶断言(离线,可自动化)**:用现有 `GCodeProcessor` 或自写小工具解析输出 G-code,统计:
   - 挤出段之间出现"非挤出 XY 移动"(E 增量≈0 且 X/Y 变化)的段数;
   - 层内 G1(挤出)是否首尾相连成单链;
   - 层界处是否存在 Z 回退/回抽。
2. **几何合法性(离线)**:对平滑滑移段,验证插值采样点仍落在模型切片多边形(或体积)内,防止穿模/悬空。
3. **回归**:默认关闭时,同一工程切片输出与基线逐字节 diff(应无差异),确认零回归。
4. **打印实测**:小尺寸薄壁花瓶(直壁、斜壁、非圆)目检层界焊缝、强度、挤出一致性;尤其观察转移点曲线附近是否积料/凹陷。

---

## 7. 风险与开放问题

1. **流量与焊缝**:层界过渡段沿斜壁的挤出珠与上下层材料搭接质量依赖速度/流量;`smooth_spiral` 已在长段上摊薄这种效应,但非圆急变截面可能仍出现可见纹路(不违背零空驶目标,属外观风险)。
2. **冷却/回缩被跳过**:整层无回抽 → 快速冷却不好时可能拉丝/垂料;需配合风扇与降速,或逐层限速。
3. **离体检查成本**:转移点曲线/滑移的几何检查需要把网格或层多边形引用传入后处理,注意 TBB 管道中的线程安全与生命周期(`3693` 起过滤器以引用捕获状态,沿用该模式时只读共享切片数据)。
4. **开放问题 O1**:能否在"个别违规层"允许常规、相邻层保持螺旋之间做质量可控的过渡?
5. **开放问题 O2**:多 perimeter 薄壁(例如壁厚为 2 条同心环且两环间隙 < 挤出宽)可否通过"环间贴合打印成一条连续迹"(外环末端折返贴内环)实现,而不违反"不补线"?(需要端点在几何上贴合,现实切片多不产生;判定器会自动给出结论。)
6. **开放问题 O3**:把"整单回退"改成"逐层回退/逐段连续"时的转移点曲线断裂策略与用户可见性提示。
7. **开放问题 O4**:是否引入"填充内补线"的有界放松项(仅限填充区内部、不穿壁、不跨岛、限长限量)?初步结论:非必要——其效果与直接选用连续填充图案高度重叠,只边际扩充填充图案选择而不扩大模型几何适用域,且引入过挤出/局部密度变化/表面透印风险;放松后"欧拉迹 ⇔ 可一笔画"的充要刻画也不再干净。v1 保持严格禁止。

---

## 8. 结论

1. "全模型零空驶一笔画"在**禁止补线**的前提下,适用域是**几何定义得很窄的一类**:截面沿高度连续形变的单连通薄壁壳(花瓶类),即对现有 Spiral/Smooth Spiral 场景的几何泛化(非圆、斜壁、连续变截面),而不是对一般模型的替代算法。
2. 层内可一画的实用判据:该层挤出图存在**欧拉迹**(奇度顶点 0 或 2),可组织成一条端点天然相接的连续迹;闭合(0 奇度)只是特例,开放路径(2 奇度)同样合法。
3. 层间连续的关键条件是 **上一层终点 = 本层起点**。该转移点可以在外壁、内壁或填充端,方向可以逐层反向;所有转移点在 3D 中连成一条**转移点曲线**。XY 漂移预算仍由 `spiral_mode_max_xy_smoothing` 与"过渡不离体"共同约束。
4. 普通 rectilinear / 平行线填充仍不可一笔覆盖(端点不共点导致奇度顶点>2),只有连续填充模式才可能被纳入。
5. 工程上,复用并泛化 `SpiralVase` 的四步机制、新增"精确链化算子(支持开放路径) + 判定器 + 单链发射分支",按 M1→M4 落地,可实现该功能且保持既有行为零回归。
6. 建议的第一步是 **M1(数据层算子 + 判定器 + 单测)**,它独立于 GCode 管线即可完整验证理论,风险最低。

---

## 附录 A. 代码引用表

(行号基于工作区 v2.4.2 快照;标注"参考"者为同一语义在别处出现)

| 主题              | 文件:行                                 | 说明                                                                       |
| ----------------- | --------------------------------------- | -------------------------------------------------------------------------- |
| 导出入口          | `GCode.cpp:2030`                      | `GCode::do_export`                                                       |
| 导出实现          | `GCode.cpp:2461`                      | `GCode::_do_export`                                                      |
| SpiralVase 创建   | `GCode.cpp:2540`                      | `m_spiral_vase = make_unique<SpiralVase>(print.config())`                |
| 层收集            | `GCode.hpp:337-338`                   | `collect_layers_to_print`(静态)                                          |
| 管道(全对象)      | `GCode.cpp:3660-3757`                 | `process_layers` + TBB 过滤器装配                                        |
| 管道(逐对象)      | `GCode.cpp:3762-3854`                 | 同上第二分支                                                               |
| 每层生成          | `GCode.cpp:4539`                      | `GCode::process_layer` 入口                                              |
| spiral 层判定     | `GCode.cpp:4596-4613`                 | 单对象/单周长/无填充判定                                                   |
| 岛/region 重组    | `GCode.cpp:5000-5104`                 | `by_extruder` 组装与 `append` 调用                                     |
| 岛归属判定        | `GCode.cpp:5055-5069`                 | `point_inside_surface`                                                   |
| 实例排序          | `GCode.cpp:5106-5136`                 | `chain_print_object_instances`/`sort_print_object_instances`           |
| 发射主循环        | `GCode.cpp:5432-5469`                 | 逐岛 perimeters→infill→perimeters(infill_first)→ironing                 |
| region 顺序注释   | `GCode.cpp:5443`                      | "path is not optimized in any way"                                         |
| result 组装       | `GCode.cpp:5499-5552`                 | 含旧#if0 注释掉的 spiral 调用点(`:5512`)                                 |
| 层变              | `GCode.cpp:5685`                      | `change_layer`                                                           |
| loop 发射         | `GCode.cpp:5744`                      | `extrude_loop`                                                           |
| multipath 发射    | `GCode.cpp:6038`                      | `extrude_multi_path`                                                     |
| 实体分发          | `GCode.cpp:6087-6101`                 | `extrude_entity`(Path/MultiPath/Loop 三分支)                             |
| path 发射         | `GCode.cpp:6103`                      | `extrude_path`                                                           |
| perimeter 发射    | `GCode.cpp:6131-6147`                 | `extrude_perimeters`                                                     |
| infill 发射+链化  | `GCode.cpp:6150-6176`                 | `extrude_infill`;`6164`/`6168`                                       |
| support 发射+链化 | `GCode.cpp:6178-6235`                 | `extrude_support`;`6201`                                               |
| 单段输出          | `GCode.cpp:6345`                      | `_extrude`                                                               |
| travel            | `GCode.cpp:7350`                      | `travel_to`;`7423` 避障绕行                                            |
| 回抽决策          | `GCode.cpp:7528`                      | `needs_retraction`                                                       |
| Region::append    | `GCode.cpp:8222-8258`                 | 填充岛 region                                                              |
| 结构声明          | `GCode.hpp:436-471`                   | `ObjectByExtruder::Island::Region`                                       |
| SeamPlacer 成员   | `GCode.hpp:508`                       | `m_seam_placer`                                                          |
| 链化算法          | `ShortestPath.cpp:1056-1069`          | `chain_and_reorder_extrusion_entities`                                   |
| 贪心排序          | `ShortestPath.cpp:1071-1094`          | `chain_extrusion_paths`/`reorder_extrusion_paths`                      |
| 实例贪心链化      | `ShortestPath.cpp:2015-2041`          | `chain_print_object_instances`                                           |
| 集合级链化        | `ExtrusionEntityCollection.cpp:87-95` | `chained_path_from`                                                      |
| 支撑链化          | `Support/TreeSupport.cpp:1688`        | 参考                                                                       |
| 周长链化          | `PerimeterGenerator.cpp:210/500`      | 参考                                                                       |
| SpiralVase 头     | `GCode/SpiralVase.hpp`                | 类定义;`m_previous_layer`/`m_smooth_spiral` 见 40-47                   |
| SpiralVase 实现   | `GCode/SpiralVase.cpp:66-216`         | `process_layer` 全文                                                     |
| Z-ramp            | `GCode/SpiralVase.cpp:162-163`        | 核心行                                                                     |
| XY 滑移           | `GCode/SpiralVase.cpp:164-192`        | smooth 插值与 E 修正                                                       |
| travel 过滤注释   | `GCode/SpiralVase.cpp:196-202`        | 跳过 travel 以融合 seam                                                    |
| 过渡流量          | `GCode/SpiralVase.cpp:148-161`        | transition in/out                                                          |
| 平滑配置          | `PrintConfig.cpp:5837-5855`           | `spiral_mode_smooth`、`spiral_mode_max_xy_smoothing`(默认 200% nozzle) |
| 流量配置          | `PrintConfig.cpp:5857-5877`           | `spiral_starting/finishing_flow_ratio`                                   |
| 配置声明          | `PrintConfig.hpp:1572-1576`           | `spiral_*` 选项                                                          |

## 附录 B. 相关配置项

| 选项                                     | 类型/默认             | 作用                            |
| ---------------------------------------- | --------------------- | ------------------------------- |
| `spiral_mode`                          | bool=false            | 启用螺旋花瓶                    |
| `spiral_mode_smooth`                   | bool=false            | 启用 XY 平滑滑移(斜壁无缝)      |
| `spiral_mode_max_xy_smoothing`         | float-or-percent=200% | 平滑滑移单点预算                |
| `spiral_starting_flow_ratio`           | float=0               | 过渡层起始流量比例              |
| `spiral_finishing_flow_ratio`          | float=0               | 收尾层结束流量比例              |
| `(建议新增)` `continuous_print_mode` | bool=false            | 本文所述连续打印总开关,默认关闭 |

## 附录 C. 术语表

| 术语                         | 定义                                                                                                     |
| ---------------------------- | -------------------------------------------------------------------------------------------------------- |
| 空驶(travel)                 | 无挤出(E 不变)的 XY 移动                                                                                 |
| 补线 / 事后补线              | 为衔接已有断线路径而额外补画的挤出段(本设计禁止);区别于填充生成器原生输出的连续图案(如 zigzag 回折,合法) |
| 一笔画                       | 一组挤出轨迹连成单条连续链/环,无空驶                                                                     |
| 连续迹(continuous trace)     | 由挤出段首尾相接形成的单层轨迹,可以是闭合(首尾 XY 重合)或开放(起点≠终点)                                |
| 闭合迹(closed trace)         | 首尾 XY 重合的连续迹;SpiralVase 的特例                                                                   |
| seam / 接缝                  | 闭合环的起点/终点标记                                                                                    |
| 转移点(transition point)     | 相邻两层共享的 XY 坐标:上一层终点 = 本层起点,可以位于外壁、内壁或填充端                                  |
| 转移点曲线(transition curve) | 各层转移点在 3D 中连成的曲线,允许非竖直、允许反向                                                        |
| Z-ramp                       | 将层高按弧长比例摊入每段挤出的 Z 连续过渡                                                                |
| XY 滑移                      | smooth 模式下把本层轨迹按弧长向上一层插值                                                                |
| 离体(off-body)               | 滑移/过渡段落到模型实体外或空洞上                                                                        |
| 单链化                       | 按"端点相接"把实体排成一条链(零空驶专用)                                                                 |
