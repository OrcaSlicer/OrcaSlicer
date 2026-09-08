# 常见功能实施手册

本文提供端到端修改清单。使用时只选择与当前任务有关的章节。

## 1. 新增一个 FFF 配置项

### 决策

先确定作用域：

- 打印机能力：喷嘴、机型、固件等，进入 printer preset。
- 工艺参数：层高、墙、填充、支撑等，进入 print preset。
- 耗材属性：温度、流量、冷却、材料等，进入 filament preset。
- 对象/区域参数：必须属于 `PrintObjectConfig` 或 `PrintRegionConfig` 可覆盖集合。
- 盘级参数：确认 `PartPlate::config()` 的覆盖语义。

### 修改闭环

1. 在 `PrintConfigDef::init_common_params/init_fff_params` 定义 option。
2. 在 `PrintConfig.hpp` 将 option 加入正确静态配置类。
3. 若是枚举，补齐 enum、字符串映射、默认值和反序列化处理。
4. 在 `Tab.cpp`/`ParamsPanel.cpp` 中加入现有页面和分组。
5. 在 `Print.cpp` 或 `PrintObject.cpp` 配置失效映射中添加键。
6. 在算法中读取最终强类型配置，避免再次解析字符串。
7. 如 profile 需要默认值，再更新 `resources/profiles/`。
8. 如涉及旧键或语义迁移，更新 `PrintConfigDef::handle_legacy*`。

### 完成条件

- 未设置新键的旧 `.3mf` 使用稳定默认值。
- 功能关闭时结果与原路径一致。
- 用户修改后会重算正确步骤，不需要重启应用。
- 对象级覆盖、全局配置和多耗材数组语义清楚。

## 2. 新增受开关控制的切片算法

推荐结构：

```text
读取强类型配置
  -> 开关关闭：进入原有路径
  -> 开关开启：进入新算法
  -> 两条路径输出同一种已有领域类型
  -> 下游流程保持共用
```

实施要点：

- 新算法优先输出 `ExPolygons`、`ExtrusionEntityCollection`、`SurfaceCollection` 等已有类型。
- 不要为新路径复制完整 `PrintObject::process` 阶段。
- 将纯计算从 GUI 分离，放到相关 `libslic3r` 模块。
- 循环中使用已有 `throw_if_canceled`/取消回调。
- 明确是否可参与共享对象缓存和 TBB 并行。
- 把开关映射到最早真正受影响的 `PrintObjectStep`，下游由依赖失效。

## 3. 新增或修改 GUI 操作

标准动作顺序：

```text
检查当前模式/选择是否合法
  -> 建立 Undo/Redo snapshot
  -> 修改 Model 或配置唯一真相
  -> 更新 plate/object list/scene 派生状态
  -> 标记 project/preset dirty
  -> schedule_background_process
  -> 刷新必要 UI
```

注意：

- 不直接修改 `GLVolume` 来代表永久模型变化；GL 数据是派生显示状态。
- 不自行维护另一份 selection。
- 事件回调中避免持有已可能销毁窗口的裸指针执行异步任务。
- 用户可见字符串使用项目现有 `L()`/`_L()` 翻译宏风格。
- 新 `.cpp` 记得加入 `src/slic3r/CMakeLists.txt`。

## 4. 修改后台切片触发逻辑

先阅读：

- `Plater::priv::schedule_background_process()`
- `Plater::priv::update_background_process()`
- `Plater::priv::restart_background_process()`
- `BackgroundSlicingProcess::apply/start/stop`

修改原则：

- 只需延迟合并频繁 UI 更新时使用现有 timer，不新增第二个切片调度器。
- `apply()` 的返回状态决定是否需要重启和清空预览。
- 切盘时先更新 `BackgroundSlicingProcess` 的当前 `PartPlate` 和对应结果对象。
- 取消是异步生命周期的一部分；不要把 canceled 当成必须弹窗的异常。
- UI 完成事件可能在工程切换后到达，处理前验证当前上下文。

## 5. 新增 `.3mf` 工程字段

### 字段设计

- 字段必须可选。
- 缺失时给出旧版本行为，而不是报错。
- 明确属于 model、object、volume、instance、plate、project config 还是 preset。
- 若可由其他数据稳定重建，优先不持久化。

### 修改闭环

1. 在 `bbs_3mf` 导出器写入字段。
2. 在导入器解析字段，接受缺失和未知值。
3. 将解析值应用到正确的所有者，而非临时解析对象。
4. 检查 `LoadStrategy`：完整工程、仅模型、仅配置是否都符合预期。
5. 检查 `SaveStrategy`：普通保存、备份、发送临时包、含 G-code 包。
6. 检查普通/Prusa/Bambu/Orca 3MF 分支。
7. 如语义替换旧字段，保留旧字段读取和迁移。

## 6. 修改系统 Profile

操作顺序：

1. 从 `resources/profiles/<vendor>.json` 找到厂商入口。
2. 沿 `inherits` 追完整继承链。
3. 确认修改的是 printer、process 还是 filament 配置。
4. 搜索同名 key 在其他厂商和模板中的用法。
5. 检查 `compatible_printers`/条件表达式。
6. 检查数组参数长度是否与喷嘴/挤出机/耗材数量一致。

不要仅因某台机器需要一个值就修改公共父预设；这会改变所有子预设。

## 7. 修复 G-code 或预览问题

先划分责任：

| 证据 | 修改层 |
| --- | --- |
| 导出的文本指令本身错误 | `GCode` / `GCodeWriter` / 后处理链 |
| 文本正确，统计时间或耗材错误 | `GCodeProcessor` |
| 处理结果正确，颜色/轨迹分类错误 | `GUI_Preview` / `LibVGCodeWrapper` |
| 数据正确，画面裁切/深度/着色错误 | `libvgcode` / OpenGL / shader |

修复时保留“实际 G-code 是真相”的方向，不在渲染层伪造新的打印语义。

## 8. 新增网络打印机或上传协议

1. 判断属于通用 `PrintHost` 上传，还是需要长连接/设备状态的 `IPrinterAgent`。
2. 把认证、协议和错误转换放 Utils/Agent 实现，不放 Dialog。
3. 复用 `SendJob`/`PrintJob` 的进度和取消模型。
4. 所有非 UI 线程回调通过 wx 事件或 `CallAfter` 更新页面。
5. 退出、注销、切换 Agent 时先停订阅和线程，再释放回调目标。
6. 日志不得输出 token、密码、完整认证头或用户敏感信息。

## 9. 修改完成后的静态自查

- 是否建立了重复状态？
- 是否绕过 `Print::apply()` 或后台调度？
- 是否漏了配置失效映射？
- 是否影响旧工程/Profile 的读取？
- 是否从工作线程碰了 GUI？
- 是否混用了毫米和 scaled coordinate？
- 是否只处理当前盘却影响了全部盘，或反之？
- 是否给新源文件补了 CMake 列表？
- 是否保持功能关闭时的旧行为？

