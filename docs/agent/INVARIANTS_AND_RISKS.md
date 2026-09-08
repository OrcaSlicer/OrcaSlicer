# 关键不变量与高风险边界

这些规则用于发现“代码能运行但架构状态已不一致”的问题。

## 1. 所有权不变量

### 编辑态与切片态分离

- `Model` 是工程几何和实例的真相。
- `Print`/`PrintObject` 是由 Model + config 派生、可失效重建的状态。
- OpenGL volume 是显示派生状态。
- `GCodeProcessorResult` 是实际 G-code 的解析派生状态。

禁止方向：从 `PrintObject` 反向偷偷修改 `Model`，或只改 `GLVolume` 形成永久编辑结果。

### 配置真相

- option 定义以 `PrintConfigDef` 为准。
- Preset 是配置来源，不是另一套 option 定义。
- `AppConfig` 保存应用偏好，不保存切片工艺参数。
- 盘级和对象级配置是覆盖，不应复制完整全局配置后独立漂移。

## 2. ID 与引用不变量

- `ObjectID` 用于识别模型对象变更和复用切片缓存。
- `ModelObject`、`ModelVolume`、`ModelInstance` 的复制方法具有特定 ID 语义。
- Undo/Redo、对象树、GL volume 和 `Print::apply()` 都可能依赖 ID 稳定性。
- 不要用容器索引长期代替 ID；删除/重排后索引会变化。
- `Print` 内许多指针非 owning，源对象生命周期必须长于切片任务。

## 3. 切片步骤与缓存不变量

配置变更必须使“最早受影响步骤及下游”失效。

| 变化类型 | 通常最早影响 |
| --- | --- |
| 网格、实例旋转、层高 | `posSlice` |
| 墙数量、线宽、接缝相关几何 | `posPerimeters` |
| 顶底层、稀疏填充、填充图案 | `posPrepareInfill`/`posInfill` |
| 熨平 | `posIroning` |
| 支撑参数/涂色 | `posSupportMaterial` 或更早 |
| 擦料塔/换料体积 | `psWipeTower` |
| skirt/brim | `psSkirtBrim` |
| 速度、温度、模板等仅输出属性 | 常见为 `psGCodeExport`，但需按实际数据依赖判断 |

风险模式：

- 首次切片正确，修改参数后二次切片错误：通常是失效不足。
- 任意小改动都全量重切：通常是失效过度。
- 相同对象复制后偶发错误：检查 shared object 缓存与可变共享数据。

## 4. 坐标与单位不变量

- 用户配置和模型外部尺寸通常使用毫米浮点值。
- 多边形算法大量使用 `coord_t` 的 scaled integer coordinate。
- 使用已有 `scale_()`、`scaled()`、`unscale()`，不要手写比例常数。
- `Transform3d` 的应用顺序不可交换；对象、volume、instance 各自有变换层级。
- 多盘全局坐标可能带 plate offset；盘内算法要显式去除偏移。
- STL 无单位，导入流程包含毫米/英寸/米启发式与用户选择，不要在底层重复转换。

## 5. 几何不变量

- `Polygon` 与 `ExPolygon` 的方向、洞和外轮廓语义必须保持。
- 布尔运算前后留意 `ApplySafetyOffset`、容差和退化小区域。
- 浮点比较使用项目已有 `EPSILON`/近似比较。
- `Flow` 同时涉及线宽、层高、间距和体积流量，不能只改其中一个值。
- `ExtrusionEntity` 的 role 会影响预览、速度、风扇、统计和后处理。

## 6. 线程不变量

| 上下文 | 可以做 | 不可以做 |
| --- | --- | --- |
| UI 线程 | wx 控件、Model 编辑编排、启动/取消任务 | 长时间阻塞算法 |
| BackgroundSlicingProcess | `Print::process`、G-code 生成 | 直接访问 wx 控件 |
| TBB worker | 对象/层级并行纯计算 | 无锁写全局容器、依赖执行顺序 |
| 网络/Job worker | 网络和耗时 I/O | 直接更新窗口、持有失效 UI 指针 |

额外规则：

- UI 更新使用 `wxQueueEvent`/`CallAfter`。
- 任务捕获对象前确认生命周期；窗口关闭后回调必须可安全丢弃。
- 取消检查应沿用已有 callback/exception 机制。
- 不要在持锁状态调用可能同步回调自身的代码。
- 状态标志不能代替互斥和生命周期协议。

## 7. 多盘不变量

- 所有盘共享一个主 `Model`。
- `PartPlate` 保存盘归属和盘级派生/覆盖数据。
- 当前编辑盘、当前切片盘、导出目标盘可能不同。
- 每个盘有自己的临时 G-code 和 `GCodeProcessorResult` 上下文。
- 切盘时预览和 `BackgroundSlicingProcess::current_plate` 必须同步切换。
- 全盘操作必须遍历有效 plate，不要只复用当前盘状态。

## 8. Preset 与兼容性不变量

- default/system/user/bundle/project-embedded preset 的可写性不同。
- Preset 继承后展示值与文件中直接存储值不同。
- compatible printer/print 条件会影响可见和可选集合。
- 多耗材数组需要 normalize，并与有效耗材数量匹配。
- 修改父 profile 会传递给所有未覆盖该 key 的子 profile。
- 保存用户 preset 时应只保存有意义的差异，不复制整个父配置。

## 9. `.3mf` 持久化不变量

- 新版本必须读旧工程。
- 新字段应可选，未知字段应尽量可忽略。
- 完整工程加载与“仅导入模型”不能产生相同副作用。
- 导入数据要落到真实所有者，不能只留在 parser 临时结构。
- 备份、发送临时包、普通保存的 `SaveStrategy` 不完全相同。
- 工程内嵌 preset 与本地用户 preset 不可无提示互相覆盖。
- 格式版本判断和 legacy migration 应集中在已有入口。

## 10. GUI 生命周期不变量

- `MainFrame`/`Plater` 销毁时仍可能有后台完成事件到达。
- 页面控件不是长期业务状态所有者。
- 模态框和 WebView 可能建立嵌套事件循环。
- 操作 Model 前后维持 Undo/Redo snapshot 和 project dirty。
- 更新对象树、3D scene 和 plate 状态时优先调用已有综合刷新方法。

## 11. 跨平台风险

- Windows 路径与进程参数使用 Unicode/nowide 现有封装。
- Linux 同时支持 Wayland/X11，避免假设只有 `DISPLAY`。
- macOS 特有实现可能在 `.mm` 文件。
- 文件名大小写在 Linux/macOS 配置下可能敏感。
- 不把平台特有 header 泄漏到公共无条件 include。
- 新依赖和系统库必须按平台分支接入 CMake。

## 12. 高风险文件

以下文件体积大且承担多个边界，修改时应尽量局部：

- `src/slic3r/GUI/Plater.cpp`
- `src/slic3r/GUI/GUI_App.cpp`
- `src/libslic3r/Print.cpp`
- `src/libslic3r/PrintObject.cpp`
- `src/libslic3r/GCode.cpp`
- `src/libslic3r/PrintConfig.cpp`
- `src/libslic3r/Format/bbs_3mf.cpp`
- `src/libslic3r/PresetBundle.cpp`

在这些文件新增 helper 前先搜索已有同义逻辑；不要顺手进行大范围格式化或无关重构。

