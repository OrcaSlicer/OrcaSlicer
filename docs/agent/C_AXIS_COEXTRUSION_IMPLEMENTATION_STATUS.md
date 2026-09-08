# 多色共挤与 C 轴开发状态

本文记录代码实际落地状态。后续 Agent 应先阅读本文件，再阅读
[`C_AXIS_COEXTRUSION_DEVELOPMENT_PLAN.md`](C_AXIS_COEXTRUSION_DEVELOPMENT_PLAN.md)，不要把规划中的设计误认为已经实现。

## 当前里程碑

状态：**从表面颜色注释、路径来源恢复、C 轴解算、延迟前馈、运动约束、G-code 写入，到解析器与目标色预览的 MVP 链路已接通**。

所有功能开关默认关闭。只有打印机 C 轴和工艺表面控制同时开启，且路径满足当前支持条件时，才会改变外墙 G-code 输出。

## 已实现

### 1. 共挤核心类型

目录：`src/libslic3r/CoExtrusion/`

- `CoExtrusionTypes.hpp/.cpp`
  - `SurfaceColorId`
  - `ColorSector`
  - `Profile`
  - C 轴旋转模式、延迟模型和顶底面策略枚举
  - 角度归一化、最短角距离、连续角展开工具
  - 色区配置解析、序列化和校验

耗材色区使用单字符串版本化格式：

```text
v1;<color-id>,<#RRGGBB[AA]>,<center-deg>,<width-deg>;...
```

示例：

```text
v1;1,#FF0000,0,120;2,#00FF00,120,120;3,#0000FF,240,120
```

一个 `ConfigOptionStrings` 元素代表一根耗材；元素内部可以定义多个色区。这避免引入含义不清的二维配置数组，并保持 Orca 多耗材配置的现有扩缩容行为。

### 2. 类型化配置

已在 `PrintConfig.hpp/.cpp` 中增加三类配置：

- printer preset：C 轴开关、轴字母、方向、零点、旋转模式、软件限位、速度/加速度/jerk、启停 G-code。
- filament preset：色区 profile、装配标定角、延迟模型、响应时间、输运体积。
- print/object preset：表面控制开关、微段长度、角度容差、色区角度安全余量、法线投影阈值、顶底面策略、大角度回退策略。

相关 key 已加入 `Preset.cpp` 的 preset option 集合；filament key 已加入耗材向量扩缩容列表。

### 3. 失效传播

- 打印机、耗材以及角度/延迟策略变化：失效到 `psGCodeExport`。
- `coextrusion_surface_control` 或三角面表面颜色变化：失效到 `posSlice`，重建逐层来源 sidecar。

### 4. 配置校验

已接入 `validate(const FullPrintConfig&)`：

- 开启表面控制时必须开启 C 轴。
- 轴方向只能为 `-1` 或 `1`。
- C 轴字母必须为一个未占用的字母，不能覆盖 `X/Y/Z/E`。
- 校验旋转模式、有限角范围和各策略字符串。
- 校验每根耗材的色区序列化格式。
- 校验延迟模型字符串。

### 5. 独立表面颜色注释

已新增 `CoExtrusion/SurfaceColorAnnotation.hpp/.cpp`：

- 使用原始三角面索引保存稳定的 `SurfaceColorId`。
- `UINT32_MAX` 作为内部“未标注”值，颜色 ID `0` 仍然合法。
- 支持单面查询、设置、清除、整体重置和时间戳比较。
- 不使用 `TriangleSelector` 细分位流，不改变 `mmu_segmentation_facets`。
- 已接入 `ModelVolume` 的复制、唯一 ID、Undo/Redo cereal 状态、清除和多 Volume 拆分复制链路。
- 网格拓扑发生替换且无法可靠映射时会随其他附加 facet 数据一起清除，不进行错误的索引复用。

### 6. 3MF 可选字段

每个三角面可保存一个可选颜色 ID：

```xml
<!-- 兼容 3MF 写入链路 -->
<triangle ... slic3rpe:coextrusion_color_id="2"/>

<!-- Orca/BBS 3MF 写入链路 -->
<triangle ... coextrusion_color_id="2"/>
```

- 两条 3MF 导入、导出链路均已接入。
- 字段缺失时保持未标注，旧工程行为不变。
- 非法、超范围或保留值不会写入颜色注释。
- BBS 共享网格判断已加入颜色注释比较，避免几何相同但表面颜色不同的 Volume 被错误合并。
- 表面颜色变化会同步到后台模型并失效 `posSlice`。

### 7. 表面 provenance 查询与路径采样

已新增 `CoExtrusion/SurfaceProvenance.hpp/.cpp`：

- `SurfaceProvenance` 统一携带 Volume ID、原始 face index、颜色 ID、表面点、对象法线、距离和来源类型。
- `provenance_for_face()` 接收切片器直接传递的精确 face index，标记为 `SliceIntersection`。
- `nearest_provenance()` 使用变换后网格的 AABB 树提供有最大距离约束的回退查询。
- 构建 AABB 前直接把网格变换到查询坐标系，正确处理非均匀缩放和镜像，且保持 face 顺序稳定。
- `sample_path()` 按最大微段长度采样 `Polyline3`，输出累计路径距离以及每个采样点的 provenance。
- `ObjectSurfaceProvenanceResolver` 聚合一个 `ModelObject` 中所有带共挤注释的实体 Volume，并支持精确 Volume/face 查询或跨 Volume 最近面回退。
- 构造器显式接受 `model_to_query` 变换，后续调用者必须明确切片对象坐标和 G-code 居中坐标之间的关系。

当前 AABB 查询只作为布尔合并或 Arachne 导致来源丢失时的回退，不应替代切片交线产生时的精确 face provenance。

### 8. 精确切片交线来源

`TriangleMeshSlicer` 已新增非破坏性接口：

```cpp
std::vector<MeshSliceLines> slice_mesh_with_face_ids(...);
```

每条 `MeshSliceLine` 保存：

- 按现有切片规则生成并定向的二维 `Line`；
- 产生该交线的原始 `face_index`。

该接口复用现有三角面求交代码，与 `slice_mesh()` 并行存在，不改变普通 Polygon/ExPolygon 输出，也不会影响功能关闭时的切片流程。调用方知道当前 `ModelVolume`，因此可组成稳定的 `(volume_id, face_index)` 精确来源键。

注意：该 sidecar 目前停留在原始切片交线层，还没有跨越 Clipper 布尔运算和 Arachne 外墙重建；这些步骤丢失来源时必须使用空间匹配，并保留 `NearestSurfaceFallback` 标记。

### 9. PrintObject 按层缓存与外墙匹配

已新增：

- `CoExtrusion/SurfaceSliceSidecar.hpp/.cpp`
- `CoExtrusion/SurfacePathMatcher.hpp/.cpp`

实现内容：

- 当 `coextrusion_surface_control` 开启时，`PrintObject::slice_volumes()` 为带表面颜色的实体 Volume 生成逐层原始交线 sidecar。
- 每条来源记录保存 `volume_id`、原始 `face_index` 和可选 `SurfaceColorId`。
- 每层建立二维 `AABBTreeLines`，支持最终路径点和微段的快速最近来源查询。
- 微段匹配同时考虑距离和方向一致性；尖角处没有合格方向候选时回退到最近线。
- `SurfacePathMatcher` 只接受外墙/悬垂外墙，将最终 `ExtrusionPath` 按最大微段长度拆分并返回累计路径距离和来源匹配。
- 缓存使用 `shared_ptr`，共享切片对象复用同一 sidecar；清除/重新切片时同步释放。
- `coextrusion_surface_control` 变化和三角面表面颜色变化现在失效 `posSlice`；角度、延迟与机械参数仍只失效 G-code。

该实现不要求 Clipper 或 Arachne 原生保存任意属性：最终外墙通过同层二维来源线恢复 `(volume_id, face_index)`，匹配不到时再使用三维 `ObjectSurfaceProvenanceResolver` 回退。

### 10. 法线投影与有效 C 角区间

已新增：

- `CoExtrusion/SurfaceDirectionResolver.hpp/.cpp`
- `CoExtrusion/CAxisIntentBuilder.hpp/.cpp`

`SurfaceDirectionResolver` 已实现：

- 将归一化三维表面法线投影到 XY，并生成 `[0, 360)` 表面方位角。
- 按 `normal_xy_threshold` 判断顶底面等退化方向。
- 支持 `hold_last`、`primary_color`、`tangent_follow` 和 `disable_control` 四种退化面策略。
- 使用以下明确约定求解命令角：

```text
物理色区方位
  = 色区中心角
  + 耗材装配标定角
  + 机械 C 零点偏移
  + C轴方向 × C命令角
```

- 将色区有效角宽转换成周期性 `AngularInterval`，并扣除可配置角度安全余量；有效余量不会小于运动规划器的合并容差，避免抗抖保持越过物理色区边界。
- `nearest_angle(reference)` 会展开到参考角附近，并在有效区间内选择旋转量最小的角度；色区宽度为 360° 时保持参考角不动。
- 输出保留法线投影长度、表面方位、有效角区间和候选目标角，几何层不直接写 G-code。

`CAxisIntentBuilder` 已把外墙匹配微段与精确 face 法线连接起来：

- 对每个微段恢复 `volume_id/face_index/color_id/normal`。
- 根据耗材 `Profile` 查找对应色区并生成 `DirectionResolution`。
- 输出 `CAxisPathIntent`，明确区分无来源、未着色、缺失 face、方向不可用和已解算状态。
- 暂存连续参考角用于减少同一输入序列内的抖动；最终打印顺序确定后，运动规划器必须根据有效角区间重新做一次连续角选择。

### 11. C 轴连续角与机械约束规划

已新增：

- `CoExtrusion/CAxisMotionPlanner.hpp/.cpp`

`CAxisMotionPlanner` 消费最终顺序的 `CAxisPathIntent` 和每段名义运动时间，输出结构化 `CAxisMotionPlan`：

- `shortest_path`：在周期等效有效区间中选择相对历史角位移最小的连续角。
- `positive_only`：只选择不小于历史角的下一个可行角，不产生反向旋转。
- `limited_range`：枚举与软限位相交的周期区间，只在 `[min, max]` 内选择目标角；无交集时显式返回 `UnreachableAngle`。
- 当前角已经位于有效区间时保持不动；距离边界不超过 `coextrusion_angle_tolerance` 时允许保持并设置 `held_by_tolerance`，抑制微段抖动。
- 没有历史角时输出 `initial_positioning_angle_deg`，不假定打印机当前 C 轴位置。
- 使用保守的静止到静止梯形/三角速度模型计算 `minimum_rotation_duration_s`，分别记录速度、加速度和 jerk 限制。
- 同步时间不足时按配置输出 `SlowDownRequired`、`PrepositionRequired` 或 `IndependentRotationRequired`，其中降速策略同时给出 `xyz_speed_scale` 和计划时长。
- 几何不可达、时间非法、缺少初始角、无需控制均为独立状态；规划器不直接修改 XYZ 路径或输出 G-code。

`CoExtrusionTypes` 已增加旋转模式和大角度策略的字符串解析函数，`PrintConfig` 校验复用同一解析入口，避免配置层与规划器枚举漂移。

### 12. 最终外墙路径接入

已新增：

- `CoExtrusion/CoExtrusionPathPlanning.hpp/.cpp`

并修改 `GCode.hpp/.cpp`，在 `GCode::_extrude()` 完成基础速度、体积流量限制和动态悬垂速度计算之后建立共挤计划：

- 仅当 C 轴和表面控制同时开启，且当前路径为外墙或悬垂外墙时进入新链路；关闭功能时不构造 resolver，也不执行额外路径匹配。
- 使用当前 `Layer` 的 `PrintObject`、layer ID 和 `slice_z`，直接消费对象局部坐标的最终 `ExtrusionPath`。
- 对普通恒速路径按原始三维线段建立时间区间；对动态悬垂变速路径按 `ProcessedPoint` 的实际逐段速度建立时间区间。
- 时间区间以累计 XY 距离与 matcher 对齐，但时长使用三维移动长度，兼容 Z-contoured 路径。
- 按当前实际 filament ID 读取色区 profile 和标定角，然后依次调用 `SurfacePathMatcher`、`CAxisIntentBuilder` 和 `CAxisMotionPlanner`。
- `ObjectSurfaceProvenanceResolver` 按当前 `PrintObject` 缓存，避免每条外墙重复变换网格和重建 AABB；切换对象或开始新导出时重建/清除。
- 连续 C 轴计划角保存在 `GCode` 独立状态中，跨同一路径后的相邻外墙继续展开；新导出开始时重置。
- 输出结果已经交给 `GCodeWriter`；恒速、动态悬垂变速、Z-contoured 与 sloped 受控外墙都使用微段写入，按路径类型输出 XYEC 或 XYZEC。
- `CoExtrusionPathPlanning` 已拆分为 `prepare()`、`compensate_delay_sequence()` 和 `finalize()` 三阶段；原 `plan()` 保留为单路径兼容封装，为 G-code 路径组在写出前统一补偿提供结构化入口。
- `finalize_sequence()` 会按路径组顺序连续传递最终 C 角；`GCode::_extrude()` 已可直接消费完成跨路径补偿的 `PreparedCoExtrusionPath`，并跳过单路径二次补偿。
- `GCode::extrude_loop()`、斜接缝环和 `extrude_multi_path()` 已在任何路径写出前预计算整组的有效流量、最终速度、动态悬垂采样和 C 轴 intent；随后对连续受控路径执行一次跨路径延迟补偿，再按最终顺序逐路径完成运动规划和写出。
- 路径组构建时连续传递方向参考角；没有共挤计划的路径作为显式断点，延迟窗口不会越过未受控路径或隐式穿过 travel/tool-change 边界。

### 13. 延迟前馈补偿

已新增 `CoExtrusion/DelayCompensator.hpp/.cpp`：

- `fixed_time` 按每个微段的实际计划时长累计，提前取未来目标方向。
- `transport_volume` 按 `segment_length * effective_mm3_per_mm` 累计，提前取达到配置输运体积后的目标方向。
- 补偿发生在方向 intent 与运动规划之间，几何位置不移动，只把未来控制目标映射到当前触发微段。
- `DelayCompensator` 已增加按最终打印顺序展平多个 `ExtrusionPath` 的连续队列接口，固定时间和输运体积窗口均可跨路径获取未来 intent；`GCode` 生成器已经在 loop、sloped loop 和 multipath 的实际输出中接入该队列。

### 14. C 轴 G-code 写入与协议元数据

`GCodeWriter` 已增加同步 `XYEC`、`XYZEC` 和独立旋转接口，并按打印机配置输出可变轴字母。协议约定为连续绝对角，文件头写入：

```gcode
; coextrusion_version = 1
; coextrusion_axis = C
; coextrusion_angle_mode = continuous_absolute
; coextrusion_profile_0 = v1;...
```

- 正常同步微段输出 `G1 X... Y... E... C...`。
- `SlowDownRequired` 降低对应 XYZ 微段进给速度。
- `PrepositionRequired` 在挤出微段前输出非挤出 C 轴定位。
- `IndependentRotationRequired` 执行回抽、独立 C 轴旋转、恢复挤出。
- 每个受控微段写入 `coextrusion_color_id` 注释，供预览恢复目标色。
- 角度不可达、时间非法或缺少初始角时写入 `coextrusion_warning`，解析结果会去重保存警告类型。
- 打印机配置中的 C 轴 start/end G-code 已进入文件头尾。

### 15. G-code 解析与旋转截面预览

- `GCodeProcessor` 从配置的动态轴字母读取连续 C 角，并把角度附加到后续移动顶点。
- 解析版本、轴字母、角度模式、耗材色区 profile 和逐段目标颜色元数据。
- C 轴独立旋转只更新状态、不生成虚假 XYZ 几何。
- `libvgcode::PathVertex` 已携带 C 角、最多四个色区的连续物理中心角、角宽和预览色。
- G-code 文件头额外保存轴方向、机械零偏和逐耗材安装校准偏角；GUI wrapper 按统一物理角公式还原色区。
- libvgcode 桌面 OpenGL 与 OpenGL ES 管线均增加三张 RGBA 端点纹理。片元着色器根据椭圆截面局部方位角选择色区，并沿线段插值连续 C 角。
- 真实旋转截面只在 `ColorPrint` 视图启用；其他着色模式继续使用原有颜色。当前 shader 显示 profile 的前四个色区。

### 16. 配置 UI

`Tab.cpp` 已加入三个入口，均位于高级参数：

- 工艺 / Quality / Multi-color co-extrusion：表面控制、分段和角度策略。
- 耗材 / Filament / Multi-color co-extrusion：色区 profile、标定角和延迟模型。
- 打印机 / Multi-color co-extrusion：C 轴协议、限位、动力学及启停 G-code。

耗材页保留版本化文本字段，同时新增 `Open cross-section editor and calibration`。其中可编辑色区并即时查看截面，也可计算安装校准偏角、固定响应时间和等效输运体积；确认后一次写回既有五项耗材配置。

对象列表的 `Add settings / Multi-color co-extrusion` 已增加 `Co-extrusion surface color ID`：

- 可对整个对象或单个实体零件指定默认颜色 ID，并随 3MF 的对象/Volume 配置保存。
- 解析优先级为逐三角面注释、零件级设置、对象级设置。
- `-1` 表示当前层级无默认颜色；零件显式设置 `-1` 可阻止继承对象颜色。
- fallback 颜色已同时接入逐层切片交线 sidecar 和三维 `ObjectSurfaceProvenanceResolver`，会实际参与法线与 C 轴角度解算。

原有 `Color Painting` Gizmo 已增加共挤表面模式：

- 打印机 C 轴开启且耗材共挤 profile 有效时可用；单耗材工程自动进入共挤模式，多耗材工程可在面板顶部切换。
- 调色板解析当前实体所用耗材的 profile，UI 状态索引在写回时映射为稳定的 `SurfaceColorId`。
- 强制关闭三角细分，以原始 face index 为最小涂色单元；新增 `TriangleSelector::facet_state()` 提供无细分状态导出。
- 批量写入 `SurfaceColorAnnotation::set_triangle_colors()`，一次更新时间戳并触发后台重新切片。
- 普通模式仍只读写 `mmu_segmentation_facets`；共挤模式只读写 `coextrusion_surface_colors`。
- profile 中缺失但模型已有的颜色 ID 会追加为洋红色保留项，避免往返编辑造成静默数据丢失。

## 尚未实现

- 独立工具栏图标和更完整的共挤专用涂色交互；当前已在现有 `Color Painting` Gizmo 中提供可用模式。
- 超过四个色区的完整 GPU 预览，以及更圆滑的高边数截面网格；当前切片/profile 本身不受四区显示上限影响。
- 对具体目标固件的命令协议、回零流程和相对/绝对模式实机确认。
- 端到端 3MF、G-code 导出/解析和预览测试；纯 profile、轴字母和延迟补偿测试已加入。

## 下一开发步骤

下一步优先补齐端到端 3MF、G-code 导出/解析和 Preview 自动化用例；随后再把当前四区截面 shader 扩展到任意色区数量和更圆滑的截面网格。仍应直接消费结构化 provenance、intent 和 motion plan，不要从 G-code 文本反向猜测表面颜色。

## 兼容性约束

- 所有新功能默认关闭；关闭时现有路径、G-code 和预览必须保持不变。
- 不改变 `mmu_segmentation_facets` 的语义。
- 新的 3MF 字段必须是可选字段，旧工程缺失时按“无共挤颜色注释”处理。
- profile/3MF 格式一旦发布，只能通过版本迁移扩展，不能静默改变字段含义。
- 当前导出协议固定为连续绝对角，但具体固件是否让自定义旋转轴受 `G90/G91` 影响仍需通过 start G-code 或固件适配层明确。

## 本阶段修改文件

```text
src/libslic3r/CoExtrusion/CoExtrusionTypes.hpp
src/libslic3r/CoExtrusion/CoExtrusionTypes.cpp
src/libslic3r/CoExtrusion/SurfaceColorAnnotation.hpp
src/libslic3r/CoExtrusion/SurfaceColorAnnotation.cpp
src/libslic3r/CoExtrusion/SurfaceProvenance.hpp
src/libslic3r/CoExtrusion/SurfaceProvenance.cpp
src/libslic3r/CoExtrusion/SurfaceSliceSidecar.hpp
src/libslic3r/CoExtrusion/SurfaceSliceSidecar.cpp
src/libslic3r/CoExtrusion/SurfacePathMatcher.hpp
src/libslic3r/CoExtrusion/SurfacePathMatcher.cpp
src/libslic3r/CoExtrusion/SurfaceDirectionResolver.hpp
src/libslic3r/CoExtrusion/SurfaceDirectionResolver.cpp
src/libslic3r/CoExtrusion/CAxisIntentBuilder.hpp
src/libslic3r/CoExtrusion/CAxisIntentBuilder.cpp
src/libslic3r/CoExtrusion/CAxisMotionPlanner.hpp
src/libslic3r/CoExtrusion/CAxisMotionPlanner.cpp
src/libslic3r/CoExtrusion/DelayCompensator.hpp
src/libslic3r/CoExtrusion/DelayCompensator.cpp
src/libslic3r/CoExtrusion/CoExtrusionPathPlanning.hpp
src/libslic3r/CoExtrusion/CoExtrusionPathPlanning.cpp
src/libslic3r/CMakeLists.txt
src/libslic3r/Model.hpp
src/libslic3r/Model.cpp
src/libslic3r/PrintApply.cpp
src/libslic3r/Format/3mf.cpp
src/libslic3r/Format/bbs_3mf.cpp
src/libslic3r/TriangleMeshSlicer.hpp
src/libslic3r/TriangleMeshSlicer.cpp
src/libslic3r/TriangleSelector.hpp
src/libslic3r/TriangleSelector.cpp
src/libslic3r/PrintConfig.hpp
src/libslic3r/PrintConfig.cpp
src/libslic3r/Preset.cpp
src/libslic3r/Print.cpp
src/libslic3r/PrintObject.cpp
src/libslic3r/GCode.hpp
src/libslic3r/GCode.cpp
src/libslic3r/GCodeWriter.hpp
src/libslic3r/GCodeWriter.cpp
src/libslic3r/GCode/GCodeProcessor.hpp
src/libslic3r/GCode/GCodeProcessor.cpp
src/libvgcode/include/PathVertex.hpp
src/libvgcode/src/ViewerImpl.hpp
src/libvgcode/src/ViewerImpl.cpp
src/libvgcode/src/Shaders.hpp
src/libvgcode/src/ShadersES.hpp
src/slic3r/GUI/LibVGCode/LibVGCodeWrapper.cpp
src/slic3r/GUI/Tab.cpp
src/slic3r/GUI/CoExtrusionProfileDialog.hpp
src/slic3r/GUI/CoExtrusionProfileDialog.cpp
src/slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.hpp
src/slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.cpp
tests/libslic3r/test_coextrusion.cpp
tests/libslic3r/CMakeLists.txt
```

按仓库 `AGENTS.md` 的要求，本阶段未执行编译和测试。
