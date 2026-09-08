# 任务定位与修改路由

本文解决两个问题：接到需求后先打开哪些文件，以及真正的状态所有者在哪里。

## 1. 按需求类型定位

| 需求/现象 | 第一入口 | 继续追踪 | 常见联动 |
| --- | --- | --- | --- |
| 启动失败、命令行参数 | `src/OrcaSlicer.cpp` 的 `CLI::run/setup` | `GUI_Init.cpp`, `GUI_App::on_init_inner` | 平台初始化、AppConfig |
| 主窗口、菜单、主 Tab | `GUI/MainFrame.*` | `GUI_App.*`, `Plater.*` | 菜单 enable 状态、窗口生命周期 |
| Prepare 页面交互 | `GUI/Plater.*` | `View3D.*`, `GLCanvas3D.*`, `Selection.*` | 快照、脏状态、后台切片 |
| 对象树行为 | `GUI/GUI_ObjectList.*` | `ObjectDataViewModel.*`, `Plater.*` | `ModelObject/Volume/Instance` |
| 移动/旋转/缩放/工具 | `GUI/Gizmos/` | `Selection.*`, `GLCanvas3D.*`, `Plater.*` | Undo/Redo、盘归属、包围盒 |
| 参数页显示或编辑 | `GUI/Tab.*`, `ParamsPanel.*` | `OptionsGroup.*`, `PrintConfig.*` | Preset dirty、兼容性、重切 |
| 新增切片参数 | `libslic3r/PrintConfig.cpp` | `PrintConfig.hpp`, `Print.cpp`, `PrintObject.cpp` | UI、profile、3MF、失效映射 |
| 切层或表面分类错误 | `PrintObject::make_perimeters` | `Slice.*`, `Layer.*`, `Surface.*` | 整数坐标、对象级缓存 |
| 墙/线宽问题 | `PerimeterGenerator.*` | `Arachne/WallToolPaths.*` | Flow、挤出实体、失效步骤 |
| 填充问题 | `PrintObject::infill` | `Fill/Fill.*`, 具体 `Fill*.cpp` | 稀疏/实体填充、表面类型 |
| 支撑问题 | `PrintObject::generate_support_material` | `Support/SupportMaterial.*`, `TreeSupport*` | 支撑层、桥接、多线程 |
| 裙边、Brim、首层范围 | `Print::process` 的 `psSkirtBrim` | `Brim.*`, `Print::_make_skirt` | 盘边界、冲突检测 |
| 多材料、擦料塔、换料 | `Print::_make_wipe_tower` | `GCode/ToolOrdering.*`, `WipeTower*` | 耗材映射、盘配置、G-code |
| G-code 内容错误 | `Print::export_gcode` | `GCode::do_export`, `GCodeWriter.*` | 后处理器、模板、固件差异 |
| 预览数据错误 | `GCodeProcessor.*` | `GUI_Preview.*`, `LibVGCodeWrapper.*` | 生成结果与显示结果的边界 |
| 纯预览渲染问题 | `GUI/Preview.*` | `src/libvgcode/`, shader | OpenGL 生命周期、平台差异 |
| 多色共挤/C 轴问题 | `libslic3r/CoExtrusion/` | `GCodeWriter.*`, `GCodeProcessor.*`, `LibVGCodeWrapper.*` | provenance、连续绝对角、固件轴协议 |
| 后台切片不刷新/重复切 | `Plater::priv::update_background_process` | `BackgroundSlicingProcess.*`, `Print::apply` | timer、取消、失效状态 |
| STL/OBJ/STEP 导入 | `Model::read_from_file` | `libslic3r/Format/` 对应格式 | 单位、默认实例、颜色 |
| 3MF 打开/保存 | `Plater::load_files/export_3mf` | `Format/bbs_3mf.*` | 版本、预设、多盘、缩略图 |
| 系统预设/Profile | `resources/profiles/` | `PresetBundle.*`, `Preset.*` | 继承、兼容表达式、数组长度 |
| 用户预设保存/同步 | `PresetCollection`, `PresetBundle` | `GUI_App` 同步代码 | system/user/bundle/project 权限 |
| 多盘问题 | `GUI/PartPlate.*` | `Plater.*`, `BackgroundSlicingProcess.*` | plate offset、盘级 config |
| 打印机连接/上传 | `Utils/PrintHost.*` 或 `IPrinterAgent.hpp` | 具体 Agent/Host、`Jobs/SendJob.*` | 网络线程、认证、取消 |
| 设备页/监控 | `GUI/DeviceCore/`, `DeviceTab/` | `DeviceManager`, `NetworkAgent` | WebView、订阅、线程回调 |
| SLA | `SLAPrint.*` | `SLA/`, `Format/SL1.*` | 与 FFF 配置/流程分离 |

## 2. 按状态所有者定位

同一个功能可能跨多个文件，优先找到状态所有者再修改。

| 状态 | 所有者 | 不应另存到 |
| --- | --- | --- |
| 应用偏好、窗口、主题 | `AppConfig` / `GUI_App` | `PrintConfig` |
| 打印机/耗材/工艺配置 | `PresetBundle` / `DynamicPrintConfig` | 临时 GUI 成员 |
| 工程几何与实例 | `Model` | `GLVolume` 或 `PrintObject` |
| 当前选中项 | `Selection` / 对象树模型 | 算法层 |
| 多盘归属与盘设置 | `PartPlateList` / `PartPlate` | 每个 `ModelObject` 的重复字段 |
| 可增量重建的切片结果 | `Print` / `PrintObject` | `Model` |
| G-code 统计和预览轨迹 | `GCodeProcessorResult` | GUI 自建解析结果 |
| 工程是否需保存 | `ProjectDirtyStateManager` | 单个按钮状态 |
| 后台切片执行状态 | `BackgroundSlicingProcess` | 独立布尔标志 |
| 设备和登录会话 | `NetworkAgent` / Manager | 页面控件 |

## 3. 从 UI 事件追到算法的标准路径

```text
wx Bind / EVT_* / 菜单回调
  -> MainFrame 或 Plater 公共方法
  -> Plater::priv 修改 Model/配置并记录快照
  -> schedule_background_process
  -> update_background_process
  -> BackgroundSlicingProcess::apply
  -> Print::apply
  -> Print::process / PrintObject::*
  -> Print::export_gcode / GCode::do_export
  -> EVT_SLICING_COMPLETED / EVT_PROCESS_COMPLETED
  -> Preview 加载 GCodeProcessorResult
```

若需求只改变 UI 展示，应在进入 `BackgroundSlicingProcess` 前停止；若需求改变打印结果，则必须追到 `libslic3r`，不能只在 GUI 做补偿。

## 4. 判断修改层级的快速规则

- 能在 CLI 和 GUI 共用的逻辑，放 `libslic3r` 或非 GUI Utils。
- 只表达用户意图、窗口状态或交互的逻辑，放 GUI。
- 可序列化的工程事实放 `Model`/配置/3MF，不放 OpenGL 对象。
- 可通过模型和配置重新计算的数据放 `Print`，不持久化回 `Model`。
- 预览必须尽量消费实际 G-code 处理结果，不建立第二套路径生成规则。
