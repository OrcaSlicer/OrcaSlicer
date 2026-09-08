# 快速检索配方

以下命令默认在仓库根目录运行。优先使用 `rg`，并根据任务继续缩小目录。

## 1. 找符号定义和调用

```powershell
rg -n "ClassName|function_name" src tests
rg -n "ClassName::function_name" src -g "*.cpp" -g "*.hpp"
rg -n "function_name\(" src\libslic3r src\slic3r\GUI
```

不要只看第一处命中：定义、调用、事件绑定和测试通常分散在不同文件。

## 2. 从 GUI 事件追调用链

```powershell
rg -n "EVT_NAME|wxEVT_|Bind\(" src\slic3r\GUI
rg -n "on_action_|on_.*completed|schedule_background_process" src\slic3r\GUI\Plater.cpp
rg -n "EVT_SLICING|EVT_PROCESS|wxQueueEvent|CallAfter" src\slic3r\GUI
```

推荐搜索顺序：事件声明 → `wxDEFINE_EVENT` → `Bind` → handler → handler 调用的状态修改函数。

## 3. 追一个配置 key

将 `option_key` 替换为实际键：

```powershell
rg -n '"option_key"' src resources tests
rg -n "option_key" src\libslic3r\PrintConfig.cpp src\libslic3r\PrintConfig.hpp
rg -n "option_key" src\libslic3r\Print.cpp src\libslic3r\PrintObject.cpp
rg -n "option_key" src\slic3r\GUI\Tab.cpp src\slic3r\GUI\ParamsPanel.cpp
rg -n '"option_key"' resources\profiles
```

命中结果应覆盖：定义、UI、失效映射、算法读取和必要的 profile。缺失某一类命中时先判断是否合理。

## 4. 追一个 Profile 的继承链

```powershell
rg -n '"name"\s*:\s*"目标名称"|"inherits"\s*:' resources\profiles
rg -n '"目标配置键"' resources\profiles\厂商目录 resources\profiles\厂商.json
rg -n 'compatible_printers|compatible_prints|printer_model|printer_variant' resources\profiles
```

名称可能出现在入口 JSON、具体配置文件和翻译映射中，修改前确认真正被加载的文件。

## 5. 追切片步骤和缓存失效

```powershell
rg -n "PrintObjectStep|PrintStep" src\libslic3r\Print.hpp
rg -n "invalidate_state_by_config_options|invalidate_step" src\libslic3r\Print.cpp src\libslic3r\PrintObject.cpp
rg -n "set_started\(|set_done\(|is_step_done\(" src\libslic3r
rg -n "Print::apply|BackgroundSlicingProcess::apply" src
```

## 6. 追模型修改和 Undo/Redo

```powershell
rg -n "take_snapshot|Plater::TakeSnapshot|UndoRedo|snapshot" src\slic3r\GUI
rg -n "set_plater_dirty|reset_project_dirty|ProjectDirtyStateManager" src\slic3r\GUI
rg -n "ModelObject|ModelVolume|ModelInstance" src\slic3r\GUI\目标文件.cpp
```

如果某个永久编辑操作没有 snapshot 和 dirty 相关调用，应检查它是否复用了更上层的统一入口。

## 7. 追 3MF 字段

```powershell
rg -n "字段名|XML_TAG|METADATA" src\libslic3r\Format\bbs_3mf.cpp src\libslic3r\Format\bbs_3mf.hpp
rg -n "load_model_from_file|store|export_3mf|SaveStrategy|LoadStrategy" src\libslic3r\Format src\slic3r\GUI\Plater.cpp
rg -n "file_version|is_orca_3mf|is_bbl_3mf|En3mfType" src\libslic3r\Format src\slic3r\GUI\Plater.cpp
```

至少找到读取、写入、缺省处理和应用到领域对象的四个位置。

## 8. 追 G-code 输出

```powershell
rg -n "Print::export_gcode|GCode::do_export" src\libslic3r
rg -n "GCodeWriter|writer\(\)|write_" src\libslic3r\GCode.cpp src\libslic3r\GCodeWriter.cpp
rg -n "GCodeProcessorResult|GCodeProcessor" src\libslic3r src\slic3r\GUI
rg -n "PostProcessor|CoolingBuffer|PressureEqualizer|FanMover" src\libslic3r
```

## 9. 找线程和生命周期边界

```powershell
rg -n "boost::thread|std::thread|tbb::parallel|CallAfter|wxQueueEvent" src
rg -n "cancel|stop_internal|join|shutdown|OnExit|EVT_CLOSE" src\slic3r\GUI
rg -n "mutex|scoped_lock|unique_lock|condition_variable" src\目标目录
```

## 10. 找跨平台分支

```powershell
rg -n "_WIN32|__WINDOWS__|__APPLE__|__WXGTK__|WAYLAND|X11" src\目标目录
rg --files src | rg "\.mm$|Mac|Win|Linux|unix|msw"
```

## 11. 找构建接入位置

```powershell
rg -n "目标文件名|SOURCES|add_library|target_link_libraries" src\libslic3r\CMakeLists.txt src\slic3r\CMakeLists.txt CMakeLists.txt
```

## 12. 找对应测试和固定数据

```powershell
rg -n "目标类|目标函数|option_key" tests
rg --files tests | rg -i "关键词"
rg -n "TEST_CASE|SCENARIO|SECTION" tests\libslic3r tests\fff_print
```

## 13. 控制大文件阅读量

先定位符号行，再只读取附近范围：

```powershell
rg -n "目标符号" src\slic3r\GUI\Plater.cpp
$content = Get-Content src\slic3r\GUI\Plater.cpp
$content[起始行..结束行]
```

大文件中优先追踪数据成员、构造绑定、目标 handler、状态更新函数，不要从文件开头顺序阅读。

