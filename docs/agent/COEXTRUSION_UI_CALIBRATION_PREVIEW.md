# 共挤截面编辑、校准与预览开发说明

本文供后续 Agent 快速定位本阶段代码，并明确 UI 数值如何进入真实旋转截面预览。

## 用户入口

高级模式下打开：`Filament > Multi-color co-extrusion > Open cross-section editor and calibration`。

对话框提供三组功能：

1. 色区表格与截面饼图：编辑 `color_id`、`preview_color`、`center_angle_deg`、`angular_width_deg`。
2. 安装对齐：选择观测色区，输入测试时的 C 命令角和实测物理方位角，计算 `filament_coextrusion_calibration_offset`。
3. 延迟校准：输入颜色边界错位距离、测试速度、线宽、层高，同时计算固定响应时间和等效输运体积。

确认对话框后，五个既有耗材配置项会一次写回，不引入新的 profile 格式。空 profile 会初始化为红、绿、蓝、黄四个 90 度色区。

## 角度约定

校准和预览共用同一物理角约定：

```text
physical_sector_azimuth
  = sector_center
  + filament_calibration_offset
  + machine_zero_offset
  + axis_direction * commanded_C_angle
```

不要在 G-code 解析后把连续 C 角归一化到 `[0, 360)`；跨零点的连续角用于保证 GPU 沿线段插值时不反向旋转。

## 延迟估算

```text
response_delay_s = boundary_shift_mm / path_speed_mm_s
transport_volume_mm3 = boundary_shift_mm * line_width_mm * layer_height_mm
```

这两个结果分别供 `fixed_time` 和 `transport_volume` 模型使用。向导只做测量换算，不替代实机多轮标定。

## 预览数据流

```text
G-code header/profile + C commands
  -> GCodeProcessorResult
  -> LibVGCodeWrapper 计算连续物理色区中心
  -> PathVertex 固定四区数据
  -> centers/widths/colors 三张 RGBA 端点纹理
  -> segment vertex shader 生成椭圆截面局部坐标
  -> fragment shader 按截面方位角选择色区颜色
```

预览只在 libvgcode 的 `ColorPrint` 视图启用，从而不覆盖速度、流量、层高等诊断着色。无 profile、无 C 数据或旧 G-code 会自动回退到原有单色路径。

## 当前边界

- 切片 profile 可包含任意数量色区，但 GPU 预览仅发送前四个。
- 现有 segment template 是低面数椭圆/盒状近似；色区边界已在片元阶段计算，但轮廓圆滑度仍受模板限制。
- 外部旧 G-code 没有方向、零偏和校准元数据时使用 `direction=1`、其余偏角为 `0`。
- 当前延迟前馈仍局限于单条 `ExtrusionPath`，尚未跨接缝做全局 look-ahead。

## 主要文件

- `src/slic3r/GUI/CoExtrusionProfileDialog.hpp/.cpp`
- `src/slic3r/GUI/Tab.cpp`
- `src/libslic3r/CoExtrusion/CoExtrusionTypes.hpp/.cpp`
- `src/libslic3r/GCode.cpp`
- `src/libslic3r/GCode/GCodeProcessor.hpp/.cpp`
- `src/slic3r/GUI/LibVGCode/LibVGCodeWrapper.cpp`
- `src/libvgcode/include/PathVertex.hpp`
- `src/libvgcode/src/ViewerImpl.hpp/.cpp`
- `src/libvgcode/src/Shaders.hpp`
- `src/libvgcode/src/ShadersES.hpp`

按仓库 `AGENTS.md` 的当前要求，本阶段仅提交代码与文档修改，不执行编译或测试。
