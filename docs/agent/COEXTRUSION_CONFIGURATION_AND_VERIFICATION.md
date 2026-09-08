# 多色共挤配置、协议与验证手册

本文面向继续开发的 Agent 和手工联调人员。代码实际完成度以
[`C_AXIS_COEXTRUSION_IMPLEMENTATION_STATUS.md`](C_AXIS_COEXTRUSION_IMPLEMENTATION_STATUS.md) 为准。

## 1. 最小可用配置

在高级参数模式下配置以下三处。

### 打印机 / Multi-color co-extrusion

- `Enable co-extrusion C axis`：开启。
- `C-axis letter`：通常为 `C`，可选 `A/B/C/U/V/W`，必须与固件旋转轴一致。
- `C-axis direction`：先用 `1`；若实机颜色方向相反改为 `-1`。
- `C-axis zero offset`：机械零位补偿角。
- `C-axis rotation mode`：首轮建议 `shortest_path`。
- 最大速度、加速度、jerk：填写固件真实可执行上限。
- start G-code：放置使能、回零和绝对角模式命令。
- end G-code：放置释放或复位命令。

### 耗材 / Filament / Multi-color co-extrusion

点击 `Open cross-section editor and calibration` 可直接编辑色区、选择颜色并查看截面图，不再需要手写下述 profile。对话框还包含安装方位和颜色响应延迟两组校准计算。

三等分 RGB 耗材示例：

```text
v1;1,#FF0000,0,120;2,#00FF00,120,120;3,#0000FF,240,120
```

格式：

```text
v1;<颜色ID>,<#RRGGBB或#RRGGBBAA>,<色区中心角>,<有效角宽>;...
```

`calibration offset` 表示该卷耗材安装方向误差。延迟模型建议按以下顺序标定：

1. 先选 `disabled` 验证方向和零点。
2. 恒速边界测试用 `fixed_time`，填写响应秒数。
3. 变速打印优先用 `transport_volume`，填写等效熔体输运体积。

### 工艺 / Quality / Multi-color co-extrusion

- 开启 `Enable co-extrusion surface control`。
- `max segment length` 决定 C 轴控制空间分辨率；越小 G-code 越大。
- `angle tolerance` 允许当前角仍落在有效色区时保持不动。
- `normal XY threshold` 用于识别顶底退化法线。
- `top/bottom strategy` 首轮建议 `hold_last`。
- `large rotation strategy` 首轮建议 `slow_down`。

### 模型或零件 / Multi-color co-extrusion

对于一个颜色对应一个实体零件的模型，可直接在 UI 中配置，无需编辑 3MF XML：

1. 在对象列表中选中整个对象或其中一个实体零件。
2. 右键选择 `Add settings` / `Multi-color co-extrusion`。
3. 将 `Co-extrusion surface color ID` 设置为耗材 profile 中存在的颜色 ID，例如四色 profile 使用 `1`、`2`、`3`、`4`。
4. 对象级 ID 会应用到其全部实体零件；零件级 ID 会覆盖对象值；值 `-1` 表示该层级不指定颜色。

优先级为“逐三角面注释 > 零件设置 > 对象设置”。因此可先用对象/零件级配置完成多实体四色模型，再用后续的表面涂色 UI 处理同一网格上的局部颜色。仅配置耗材 profile 而没有上述颜色映射时，不会自动给模型表面分色。

### 同一网格局部表面涂色

打印机 C 轴已开启且当前耗材 profile 有效时，工具栏原有的 `Color Painting` 工具也可用于共挤表面：

1. 选中模型并打开 `Color Painting`；单耗材工程会自动进入 `Co-extrusion Surface Painting`。
2. 多耗材工程在面板顶部勾选 `Co-extrusion surface colors`，与普通 MMU 涂色模式切换。
3. 调色按钮来自当前实体所用耗材的共挤 profile，悬停可查看真实颜色 ID。
4. 左键使用选定颜色涂色，Shift+左键擦除；刷子、单三角面、高度范围和智能填充均复用现有工具。
5. 共挤模式强制以原始网格三角面为最小单元，不生成 TriangleSelector 细分数据。

该模式只修改 `coextrusion_surface_colors`，不会修改 `mmu_segmentation_facets`。如果注释中存在当前 profile 未定义的颜色 ID，UI 会用洋红色保留显示，避免打开并保存涂色器时静默丢失数据。

## 2. 导出协议

启用 C 轴后，文件头应包含：

```gcode
; coextrusion_version = 1
; coextrusion_axis = C
; coextrusion_axis_direction = 1
; coextrusion_axis_zero_offset = 0.000000
; coextrusion_angle_mode = continuous_absolute
; coextrusion_profile_0 = v1;1,#FF0000,0,120;2,#00FF00,120,120;3,#0000FF,240,120
; coextrusion_calibration_0 = 0.000000
```

同步挤出示例：

```gcode
; coextrusion_color_id = 2
G1 X45.200 Y31.850 E0.02741 C128.500
```

独立定位示例：

```gcode
G1 C128.500 F7200 ; co-extrusion C-axis preposition
```

内部角度是连续绝对角，因此可以出现 `C370` 或 `C-15`。不要在后处理器中无条件归一化到 `[0, 360)`，否则会破坏最短路径和有限角规划。

## 3. 三种大旋转处理

| 策略 | 输出行为 | 适用情况 |
| --- | --- | --- |
| `slow_down` | 降低当前 XYZ/E 微段速度，让 C 轴同步完成 | 可连续旋转、允许局部降速 |
| `preposition` | 挤出微段前先做一次非挤出 C 轴定位 | 颜色边界允许短暂停顿 |
| `independent_rotation` | 回抽、独立旋转、恢复挤出后继续 | 大角跳变且不能边挤出边转 |

`preposition` 和 `independent_rotation` 都会在模型表面形成时间停顿，需结合温度、渗料和接缝位置评估。

## 4. 手工验证清单

不连接打印机即可检查：

1. 功能关闭时，不应出现 `coextrusion_` 文件头、颜色段注释或同步 C 参数。
2. 功能开启但模型没有逐面注释、对象颜色 ID 或零件颜色 ID 时，不应凭空生成受控颜色段。
3. 对象颜色 ID 应被零件颜色 ID 覆盖，逐面注释应再覆盖零件颜色 ID。
4. 有颜色映射的普通恒速外墙应被拆为 `G1 XYEC` 微段。
5. 相邻微段 C 值应连续展开，不应在 `0/360` 附近无故反转一整圈。
6. `coextrusion_color_id` 应与预览中的目标色段一致。
7. 独立 C 行不应在预览中生成一条虚假的空间移动线。
8. 重新保存 3MF 后，三角面的 `coextrusion_color_id` 和对象/零件配置应能再次导入。
9. 在共挤涂色模式修改表面后，普通 MMU 涂色数据应保持不变；切回普通模式时共挤注释也应保持不变。

当前不应当期待：

- sloped、Z-contoured 和动态悬垂 variable-speed 外墙均已输出 C；但前两者的法线来源查询仍使用层统一 `slice_z`。
- profile 超过四个色区时全部色区都参与切片，但 GPU 截面预览当前只显示前四个。
- 延迟补偿跨越接缝或下一条外墙提前控制。

## 5. 实机前必须确认的固件契约

- 自定义旋转轴的单位是否为度。
- `G1 C... F...` 中 `F` 对独立旋转的单位和解释方式。
- C 轴是否受 `G90/G91` 影响，还是拥有独立绝对/相对模式。
- 同一行 `XYEC` 是否被固件做同步插补。
- 连续角是否允许超过 360 度和使用负值。
- C 轴回零、使能、失能、软限位命令。
- 回抽期间独立旋转会不会改变挤出机坐标模式或压力提前状态。

若任一项与当前协议不同，应优先新增固件适配层，不要把设备特例散落在角度解算和路径匹配模块中。

## 6. 后续 Agent 的优先任务

1. 让 sloped 与 Z-contoured 外墙按微段实际 Z 查询 provenance。
2. 建立最终打印顺序上的全局 look-ahead，解决跨路径延迟前馈。
3. 为 `PathVertex::coextrusion_c_angle_deg` 增加旋转截面 mesh/shader。
4. 新增耗材截面图形编辑器和模型共挤表面涂色工具。
5. 为角度区间、延迟补偿、G-code parser 和 3MF round-trip 增加定向测试。
