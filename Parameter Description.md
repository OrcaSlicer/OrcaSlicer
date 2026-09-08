# OrcaSlicer Windows 编译批处理脚本参数解析
脚本文件名假设为 `build.bat`，调用格式：
```cmd
build.bat [arg1] [arg2]
```
最多接收**2个位置参数**，参数顺序不严格，部分参数可以组合使用。

## 全部可用参数列表
### 1. `pack`
- 用法：`build.bat pack`
- 功能：**仅打包依赖库，不编译代码**
  1. 进入 `deps/build` 目录
  2. 获取系统日期，生成文件名 `OrcaSlicer_dep_win64_YYYYMMDD_vs2022.zip`
  3. 调用内置7z，把 `OrcaSlicer_dep` 文件夹压缩成zip包
  4. 执行完直接退出脚本，后面所有编译逻辑不会运行

---

### 2. `debug`
- 用法：
  ```cmd
  build.bat debug
  build.bat x64 debug
  build.bat arm64 debug
  ```
- 作用：编译类型改为 **Debug**
  - 输出目录：`build-dbg`（x64） / `build-dbg-arm64`（arm64）
  - CMake `CMAKE_BUILD_TYPE=Debug`，完整调试符号，不做优化，用于调试。
> 注意：`debug` 和 `debuginfo` 互斥，`debug` 优先级更高。

### 3. `debuginfo`
- 用法：
```cmd
build.bat debuginfo
build.bat arm64 debuginfo
```
- 作用：编译类型 **RelWithDebInfo**（发布版+调试符号）
  - 输出目录：`build-dbginfo` / `build-dbginfo-arm64`
  - Release级别优化，同时生成pdb调试符号；性能接近正式版，可以调试崩溃问题。
> 如果同时传 `debug debuginfo`，debug会覆盖debuginfo。

### 4. `x64`
- 作用：强制目标架构 x64。
- 默认行为：脚本自动读取 `PROCESSOR_ARCHITECTURE`，主机是ARM64则默认编译arm64，其余默认x64。
- 示例：`build.bat x64 debuginfo`

### 5. `arm64`
- 作用：强制目标架构 ARM64。
- 示例：`build.bat arm64 release`（release不用传参数，默认就是）

### 6. `deps`
- 用法：`build.bat deps`
- 逻辑分支：只编译第三方依赖库（deps项目），**不编译 OrcaSlicer本体**
  - cmake构建deps target，构建完成后 `exit /b 0` 直接退出，不会走到 `:slicer` 切片器编译流程。

### 7. `slicer`
- 用法：`build.bat slicer`
- 逻辑：跳过deps依赖编译，**直接编译OrcaSlicer本体**。
> ⚠️ 使用前提：你已经手动编译好deps依赖，否则会cmake报错找不到依赖。

---

# 环境变量（不是命令行参数，但是会影响构建）
1. `ORCA_UPDATER_SIG_KEY`
> 环境变量，不是bat传入参数。
设置后cmake会传入 `-DORCA_UPDATER_SIG_KEY=xxx`，用于更新包签名；没设置则该cmake参数为空。

2. `CMAKE_POLICY_VERSION_MINIMUM=3.5`
脚本内部强制设置，规避新版cmake对旧策略的警告。

---

# 常用组合示例
1. 默认编译 x64 Release完整版本（deps + slicer）
```cmd
build.bat
```

2. 只编译依赖 x64
```cmd
build.bat deps x64
```

3. 已经编译完deps，只编译slicer本体 arm64 Debug
```cmd
build.bat arm64 debug slicer
```

4. 编译带调试符号的正式版本 RelWithDebInfo arm64
```cmd
build.bat arm64 debuginfo
```

5. 打包依赖压缩包
```cmd
build.bat pack
```

---

# 脚本内部逻辑小坑
1. 参数最多识别前2个，传第3个参数会被直接忽略。
> 例如：`build.bat x64 debug deps`，只会读取 `x64`、`debug`，`deps` 失效。
2. `debug` 会覆盖 `debuginfo`，两个不要一起写。
3. `slicer` 参数只是跳转标签，**不会自动编译deps**，deps没构建好会编译失败。
4. `pack` 一旦命中直接exit，后面所有参数全部无效。

## 构建目录对照表
| 参数组合 | build_type | 输出文件夹 |
|---|---|---|
| 默认 | Release | `build` |
| x64 arm64 | Release | `build-arm64` |
| debug | Debug | `build-dbg` / `build-dbg-arm64` |
| debuginfo | RelWithDebInfo | `build-dbginfo` / `build-dbginfo-arm64` |

构建完成后会执行 `cmake --build . --target install`，做安装阶段复制资源、翻译文本（调用`scripts/run_gettext.bat`）。