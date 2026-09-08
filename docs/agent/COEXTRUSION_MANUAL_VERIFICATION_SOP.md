# 多色共挤与 C 轴功能手工验证 SOP

本文用于验证当前已经落地的 UI、模型颜色映射、切片、G-code、校准计算和旋转截面预览。建议按顺序执行；前一阶段失败时先停止，不要直接连接打印机。

## 1. 验证范围

当前可验证：

- 打印机、耗材和工艺三个层级的共挤配置 UI。
- 四色色区图形编辑器。
- 安装角与颜色延迟校准计算。
- 对象/零件级颜色 ID 和同一网格逐面涂色。
- 3MF 保存与重新载入。
- 外墙法线到连续 C 角的解算。
- `XYEC` / `XYZEC`、预定位、独立旋转和降速 G-code。
- G-code 头部共挤元数据解析。
- Filament/ColorPrint 模式下的旋转截面预览。

当前不作为通过条件：超过四个色区的完整 GPU 显示、跨外墙路径的全局延迟前馈、真实打印机固件兼容性。

## 2. 准备测试工程

1. 启动软件并切换到“高级”或“专家”模式。
2. 新建一个打印机 preset、一个耗材 preset 和一个工艺 preset，避免修改日常使用的配置。
3. 导入一个直径约 30 mm、高约 20 mm 的圆柱体。圆柱外墙会持续改变法线方向，最适合观察 C 角连续变化。
4. 暂时不要发送到打印机；本 SOP 前八阶段只检查切片结果。

通过标准：三个 preset 均可编辑并保存，圆柱体能使用普通配置正常切片。

## 3. 配置打印机 C 轴

打开 `Printer > Multi-color co-extrusion`，设置：

| 参数 | 首轮验证值 |
| --- | --- |
| Enable co-extrusion C axis | 开启 |
| C-axis letter | `C` |
| C-axis direction | `1` |
| C-axis zero offset | `0` |
| C-axis rotation mode | `shortest_path` |
| C-axis minimum angle | `-360` |
| C-axis maximum angle | `360` |
| C-axis maximum speed | `120 deg/s` |
| C-axis maximum acceleration | `360 deg/s^2` |
| C-axis maximum jerk | `0` |
| Start/End G-code | 首轮留空 |

通过标准：配置可保存，没有“surface control requires C axis”或轴方向非法提示。

## 4. 创建四色共挤耗材

1. 打开 `Filament > Multi-color co-extrusion`。
2. 点击 `Open cross-section editor and calibration`。
3. 空 profile 应自动出现四行：红、绿、蓝、黄，每区 90 度。
4. 确认表格内容如下：

| Color ID | Color | Center | Width |
| --- | --- | ---: | ---: |
| 1 | `#FF0000` | 0 | 90 |
| 2 | `#00FF00` | 90 | 90 |
| 3 | `#0000FF` | 180 | 90 |
| 4 | `#FFFF00` | 270 | 90 |

5. 修改任意颜色或角度，确认右侧截面图立即变化，然后恢复上述数值。
6. 点击 OK。

通过标准：耗材字段写回类似以下内容，关闭并重新打开编辑器后四行仍存在：

```text
v1;1,#FF0000,0,90;2,#00FF00,90,90;3,#0000FF,180,90;4,#FFFF00,270,90
```

## 5. 验证校准计算

### 5.1 安装角

在截面编辑器的安装对齐区域输入：

- Observed color sector：Color ID 1。
- Commanded C angle：`30`。
- Measured physical azimuth：`50`。
- 当前打印机方向为 `1`、零偏为 `0`。

点击 `Calculate installation offset`。

通过标准：结果为 `20.00 deg`。点击 OK 后，`Co-extrusion calibration offset` 为 `20`。

### 5.2 延迟换算

输入：

- Boundary shift：`8 mm`。
- Test speed：`40 mm/s`。
- Line width：`0.45 mm`。
- Layer height：`0.20 mm`。

点击 `Calculate delay values`。

通过标准：

```text
Response delay = 0.20 s
Transport volume = 0.72 mm^3
```

首轮切片验证建议把 Delay model 恢复为 `disabled`，避免延迟前馈干扰基础角度验证。

## 6. 配置工艺和表面颜色

1. 打开 `Process > Quality > Multi-color co-extrusion`。
2. 开启 `Enable co-extrusion surface control`。
3. 首轮使用：

| 参数 | 值 |
| --- | --- |
| Maximum co-extrusion segment length | `1.0 mm` |
| Co-extrusion angle tolerance | `2 deg` |
| Normal projection threshold | `0.05` |
| Top and bottom strategy | `hold_last` |
| Large rotation strategy | `slow_down` |

4. 在对象列表中右键圆柱，选择 `Add settings > Multi-color co-extrusion`。
5. 将 `Co-extrusion surface color ID` 设置为 `1`。

通过标准：对象级设置能显示 ID 1，切片不会报告 profile 中缺少 Color ID 1。

## 7. 验证逐面涂色和 3MF 往返

1. 选中圆柱并打开原有 `Color Painting` 工具。
2. 单耗材工程应进入 `Co-extrusion Surface Painting`；多耗材工程需勾选面板顶部的 `Co-extrusion surface colors`。
3. 调色板应显示 profile 中的四种颜色。
4. 用 Color ID 2 在圆柱侧面涂一块区域，再用 ID 3 涂另一块区域。
5. 保存为 3MF，关闭工程并重新打开。

通过标准：

- 两块涂色区域重新载入后仍存在。
- 普通 MMU 涂色数据没有被共挤模式覆盖。
- 鼠标悬停或提示中显示实际 `Color ID`。
- 重新切片后没有静默丢失涂色。

## 8. 验证基础 G-code

切片并导出 G-code，用文本编辑器搜索 `coextrusion_` 和 ` C`。

### 8.1 文件头

应存在：

```gcode
; coextrusion_version = 1
; coextrusion_axis = C
; coextrusion_axis_direction = 1
; coextrusion_axis_zero_offset = 0.000000
; coextrusion_angle_mode = continuous_absolute
; coextrusion_profile_0 = v1;...
; coextrusion_calibration_0 = 20.000000
```

### 8.2 受控外墙

应反复出现：

```gcode
; coextrusion_color_id = 1
G1 X... Y... E... C...
```

涂色区域附近应出现 ID 2 或 ID 3。圆柱一圈中的 C 值应连续变化，允许出现负角或超过 360 度，不应在 `0/360` 附近突然反转一整圈。

### 8.3 非受控路径

内墙、填充或无颜色来源的路径可以出现：

```gcode
; coextrusion_color_id = none
```

通过标准：文件头完整，外墙包含同步 `XYEC`，颜色注释与对象/涂色 ID 一致，没有大量 `unreachable_angle`、`invalid_timing` 或 `missing_initial_angle` 警告。

## 9. 验证旋转截面预览

1. 打开刚导出的 G-code 预览。
2. 将着色方式切换到 `Filament`；内部对应 libvgcode 的 `ColorPrint` 模式。
3. 放大观察外墙挤出线。
4. 沿圆柱外墙移动观察点，确认线条截面能同时看到 profile 的多个色区，并且色区方向随 C 值连续旋转。
5. 切换到 `Speed`、`Volumetric flow` 或 `Layer height`。

通过标准：

- Filament 模式显示旋转后的多色色区，而不是整条线只有一个目标色。
- C 连续变化处的色区方向也连续变化。
- 切换到速度、流量等模式后恢复对应的原有诊断颜色，不被共挤色覆盖。
- 旧 G-code 或没有共挤 profile 的 G-code 仍按原有单色方式显示。

当前限制：GPU 只显示 profile 的前四个色区；第五个及之后的色区仍参与切片，但不是本阶段预览通过条件。

## 10. 验证三种大旋转策略

为了容易触发限制，可临时把 C 最大速度设为 `5 deg/s`、最大加速度设为 `20 deg/s^2`，并保留较高外墙速度。

### slow_down

设置 `Large rotation strategy = slow_down` 后切片。

通过标准：受限微段的 `F` 值降低，但同一挤出移动仍携带 C 参数。

### preposition

设置为 `preposition` 后切片。

通过标准：外墙微段前出现：

```gcode
G1 C... F... ; co-extrusion C-axis preposition
```

### independent_rotation

设置为 `independent_rotation` 后切片。

通过标准：大旋转位置依次出现回抽、以下独立旋转行和恢复挤出：

```gcode
G1 C... F... ; co-extrusion independent C-axis rotation
```

完成后恢复真实设备的速度和加速度值。

## 11. 验证功能关闭时的兼容性

1. 关闭打印机的 `Enable co-extrusion C axis`。
2. 同时关闭工艺的 `Enable co-extrusion surface control`。
3. 重新切片并导出。

通过标准：

- 不出现 `coextrusion_version`、`coextrusion_profile` 和 `coextrusion_color_id`。
- 普通移动中不出现作为旋转轴的 C 参数。
- 普通切片路径、时间和预览行为恢复原状。

然后只开启 `surface control`、保持 C 轴关闭。

通过标准：配置校验明确提示 `Co-extrusion surface control requires the C axis to be enabled`，而不是崩溃或生成不完整 G-code。

## 12. 实机校准前检查

只有前述离线项目全部通过后再连接设备，并首先确认：

- 固件 C 轴单位确实是度。
- 固件允许连续绝对角、负角和超过 360 度的角度。
- `G1 XYEC` 会对四轴做同步插补。
- 独立 `G1 C... F...` 中 F 的单位符合当前输出约定。
- Start G-code 已完成使能、回零和绝对模式设置。
- End G-code 不会在喷头仍接触模型时突然回零。

任何一项不一致，都应先增加目标固件适配层，不要直接修改表面法线和色区解算公式。

## 13. 验证记录模板

```text
软件版本/commit：
构建类型：
测试模型：
打印机 preset：
耗材 profile：

[ ] 打印机 UI
[ ] 四色截面编辑器
[ ] 安装角计算
[ ] 延迟计算
[ ] 对象颜色 ID
[ ] 逐面涂色
[ ] 3MF 往返
[ ] G-code 头
[ ] XYEC 连续角
[ ] slow_down
[ ] preposition
[ ] independent_rotation
[ ] 旋转截面预览
[ ] 非共挤兼容性

异常现象：
最小复现工程：
相关 G-code 行：
截图/日志：
```
