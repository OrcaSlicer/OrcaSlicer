# 零空驶连续打印(一笔画)实施清单

> 配套设计文档:`CONTINUOUS_PRINT_ZERO_TRAVEL_DESIGN.md`(下称"设计文档",章节号如 5.3 均指该文档)。
> 基线:OrcaSlicer `v2.4.2`。原则:**新功能默认关闭,不改既有行为**;每个里程碑完成后先过回归再进下一步。

---

## 0. 环境构建(一次性)

执行 `build_release_vs2022.bat` 会自动完成 deps 与主工程的构建:

- [X] 0.1 安装前置:Visual Studio 2022(含 C++ 桌面开发负载)、CMake、git
- [X] 0.2 首次构建(含全部 deps,**预计数小时**):

  ```bat
  :: 在仓库根目录执行;默认 Release / x64
  build_release_vs2022.bat

  :: 开发期建议用 RelWithDebInfo(带调试信息,输出到 build-dbginfo/):
  build_release_vs2022.bat debuginfo
  ```

  脚本流程:`deps/build*` 下构建依赖 → 主工程 `build*/` 下 `ALL_BUILD` → 安装到 `build*/OrcaSlicer`。
- [X] 0.3 后续增量构建(已构建过 deps 后,只编主工程):

  ```bat
  cd build-dbginfo
  cmake --build . --config RelWithDebInfo --target ALL_BUILD -- -m
  ```
- [X] 0.4 验证测试基建可用(注意:首次配置需 `cmake .. -DBUILD_TESTS=ON` 才会生成测试目标):

  ```bat
  cd build-dbginfo
  ctest -C RelWithDebInfo --test-dir ./tests/libslic3r --output-on-failure
  ```
  PS: `debuginfo`模式下可以无报错编译成功,但产出的orca-slicer.exe无法运行,运行时无报错直接退出,无任何输出。
  PS2(2026-09-08 更新):`build/` 目录的 **Release 构建可正常运行**(GUI 子系统应用控制台无输出属正常),且已成功用于 CLI 切片验证(见 M2);`build/` 目录亦可以 `--config RelWithDebInfo` 编译并运行单测。
---

## 1. M1:数据层算子 + 判定器 + 单测(不接管线)

设计依据:5.2、5.3。此阶段**不碰 `GCode.cpp`**,所有代码独立可测。

### 1.1 开工前已敲定的决策(实现时遵循)

- [X] 端点重合容差 ε_geo:使用 `is_approx(a, b, SCALED_EPSILON)`(`Point.hpp:390-393`),写死在算子内,v1 不暴露配置项
- [X] `ExtrusionLoop` 处理:loop 无 `start_idx` 成员,seam 由 `split_at`/`split_at_vertex` 实现,且 `can_reverse()` 恒 false;算子层面把 loop 当作"首尾同点的闭合边"参与构图(不可反转);判定器 v1 只放行两类情形——①整层恰好一个 loop(退化为 SpiralVase 场景);②开放路径组成的单链(2 奇度端点)
- [X] 补线约束按 1.1 节更新后的划界:禁止事后补线;连续填充图案(gyroid/zigzag 回折)属合法原生图案
- [X] 回退策略 v1:任一判定失败 → 整单回退常规打印(5.6)

### 1.2 精确链化算子(设计文档 5.3)

- [X] 在 `src/libslic3r/ShortestPath.hpp/.cpp` 新增:

  ```cpp
  struct ExactChainResult {
      std::vector<std::pair<size_t, bool>> order; // (实体索引, 是否反转)
      Point start;   // 链首
      Point end;     // 链尾
      bool  closed;  // 首尾重合
  };
  std::optional<ExactChainResult> chain_extrusion_entities_exact(
      const std::vector<ExtrusionEntity*> &entities,
      const Point *preferred_start = nullptr);
  ```
- [X] 实现要点:

  - [X] 端点提取用基类虚接口 `first_point()`/`last_point()`(`ExtrusionEntity.hpp:118-122`);`extrusion_entity_has_endpoints`(`ShortestPath.cpp:20-41`)为 static 私有、只判非空不提取坐标,需自行过滤零长度实体
  - [X] 端点合并为顶点(scaled 坐标,`is_approx`/`SCALED_EPSILON` 判定),统计各顶点度数
  - [X] 奇度顶点 = 0 → `closed=true`;= 2 → 开放链,链首/链尾即两个奇度端点;> 2 → 返回 `std::nullopt`
  - [X] 接续条件唯一:下一段首点在当前末点 `SCALED_EPSILON` 邻域内(`is_approx`);找不到即失败。**绝不创造新连线**
  - [X] `preferred_start` 非空时优先从距其最近的合法端点起链(供层间衔接用)
  - 备注:图连通性不做独立并查集,由 Hierholzer 结束时的"消费边数 == 实体数"校验兜底(不连通图必然消费不完)

### 1.3 判定器骨架(设计文档 5.2)

- [X] 新增 `src/libslic3r/GCode/ContinuousPrint.hpp/.cpp`(已登记 `src/libslic3r/CMakeLists.txt`):
  - [X] `enum class ContinuousPrintVerdict { Applicable, Reject };`(实际命名比原计划多了 `ContinuousPrint` 前缀)
  - [X] `struct ContinuousLayerPlan`(order / start_point / end_point / is_closed / total_length / sampling);`total_length` 已 `unscale_` 为 mm(注意 `ExtrusionEntity::length()` 返回 scaled 单位)
  - [X] `preflight_layer(entities, layer, cfg, out_plan)`:调 `chain_extrusion_entities_exact`,填充 `out_plan`;形状级判定(单对象/单材料/无支撑/单岛)留到 M3(`layer`/`cfg` 参数已预留,当前 `[[maybe_unused]]`)
- [X] 弧长采样(`sampling`):沿链按 1mm 固定步长**插值**采样 XY(非仅取顶点),供后续转移点曲线与离体判定使用
- [X] v1 判定器仅覆盖层内单链判定;转移点曲线/离体检查(5.2 第 4 步)留到 M3

### 1.4 单元测试

- [X] 新增 `tests/libslic3r/test_continuous_print.cpp`,并在 `tests/libslic3r/CMakeLists.txt` 源文件清单中登记(该文件为显式列表,不自动 glob)
- [X] 实体构造参考 `tests/fff_print/test_extrusion_entity.cpp:22-36`:**注意 `ExtrusionPath::polyline` 是 `Polyline3`(三维)**,测试点要用 `Point3` 追加,`first_point()`/`last_point()` 返回其 2D 投影;`tests/libslic3r` 下没有现成的 ExtrusionEntity 构造示例
- [X] 用例(手构 `ExtrusionEntity` 集):
  - [X] 单个 `ExtrusionLoop` → 通过,`closed=true`
  - [X] 两条平行开放线(rectilinear 示意)→ 拒绝(奇度 > 2)
  - [X] 端点相接的开放折线链 → 通过,`closed=false`,起终点为两个奇度端点
  - [X] 链中间一段反向(需 flip)→ 通过且 `order` 中对应 `bool=true`
  - [X] 端点间距 > ε_geo → 拒绝(不补线断言)
  - [X] 空实体集 / 零长度实体 / 两个分离环(不连通图)→ 拒绝且不崩溃
  - [X] `preferred_start` 选择最近奇度端点为链首
  - [X] `preflight_layer` 三组:开放链 Applicable + 计划一致性、单 loop Applicable + closed、平行线 Reject
- [X] 跑通:`ctest -C RelWithDebInfo --test-dir ./tests/libslic3r --output-on-failure`(2 个新场景 40 断言全过)

### 1.5 M1 出口标准

- [X] 全部新单测通过;既有测试无回归(注:`Placeholder parser coFloatsOrPercents` 在基线上即 SEGFAULT,为既有问题,与本次纯新增改动无关;构建需降并行 `/maxcpucount:2`,否则全并行 `-m` 会编译器堆耗尽)
- [X] 不修改 `GCode.cpp` / `PrintConfig.cpp`,主程序行为零变化

---

## 2. M2:文本过滤器原型(离线)

设计依据:5.1 路线 A、5.4。

- [X] **算子扩展(接合点拆分,设计文档 3.5)**:实现为 `split_entities_at_junctions()`(`ContinuousPrint.cpp`,独立于 M1 算子):"实体 A 端点落在实体 B 中间(ε 内)→ 拆分 B"(支持 `ExtrusionPath`/`ExtrusionLoop`,loop 在接合点处线性化),`preflight_layer` 已接入,拆分工作集由 `ContinuousLayerPlan::entities` 持有供发射用。拆分不新增几何,不违反"不补线"约束。单测:lollipop 通过、双接合点桥接通过、T 型接合(4 奇度)拒绝、无接合直通
- [X] 用真实切片数据验证连续填充图案(2026-09-08,Release 构建 CLI 切片 20mm 实心立方体,BBL X1C profile,块内纯空驶统计):
  - `sparse_infill_pattern = alignedrectilinear`:**0 / 1071**(0 次块内空驶/1071 段挤出)——一层一条迹,zigzag 端部相接
  - `top_surface_pattern = monotonic`:0 / 141;`bottom_surface_pattern = monotonic`:0 / 122
  - `internal_solid_infill_pattern = monotonic`:2 / 474(仅 2 次例外,疑窄区分片)
  - 对照:Inner wall 50 / 200(每层 2 道墙环,环间不共点,各有 1 次环间空驶,符合 lollipop 接合预期)
  - 产物留存于 `sandboxes/continuous_print/`(STL/G-code,未跟踪);CLI 用法:`orca-slicer.exe --slice 0 --outputdir <dir> --load-settings "<machine.json>;<process.json>" --load-filaments "<filament.json>" <model.stl>`(注意 `--slice` 必须带板号参数)
- [X] 在 `ContinuousPrint.hpp/.cpp` 中实现过滤器,泛化自 `SpiralVase::process_layer`(`SpiralVase.cpp:66-216`),复用四步机制:
  - [X] 首条纯 Z 移动改写(保持 Z 单调)
  - [X] Z-ramp(按弧长比例摊层高)
  - [X] XY 平滑滑移(复用 `spiral_mode_smooth` 开关;预算经 `set_max_xy_smoothing` 注入,离体检查留 M3 hook)
  - [X] 跳过 travel/回抽行
  - 注:`SpiralVaseHelpers` 已迁移至 `SpiralVase.hpp`(inline)供两个过滤器共用
- [X] 离线验证:单测自动对比——同一单层 G-code 分别喂入 SpiralVase 与 ContinuousPrint,输出**逐字节相等**;另断言零空驶(任何 XY 移动必带挤出)与 Z 单调递增至层高
- [ ] 人工检查斜壁样张的转移点曲线(连续、无突变)。**已解锁**(Release 构建可 CLI 切片),建议并入 M3 管线接入后用真实模型验证

### M2 出口标准

- [X] 闭合环输入下,过滤器输出与 SpiralVase 等价(单测逐字节断言)
- [X] 开放链输入下,Z 单调、无空驶段(单测断言;Δ 预算与离体检查的强制执行为 M3 范围)

---

## 3. M3:管线接入

设计依据:5.4、5.5。

- [ ] `src/libslic3r/PrintConfig.{hpp,cpp}`:新增 `continuous_print_mode`(bool,默认 false);平滑参数复用 `spiral_mode_smooth` / `spiral_mode_max_xy_smoothing`,不新增配置面;开关开启时校验/提示填充配置须为连续图案(monotonic / alignedrectilinear,见设计文档 3.4)
- [ ] `src/libslic3r/GCode.hpp`:`LayerResult` 扩展字段(携带 `ContinuousLayerPlan` 或判定结果)
- [ ] `src/libslic3r/GCode.cpp`:
  - [ ] `process_layer`(4539 附近):启用开关时执行 preflight;**注意 preflight 需全层遍历后才能给整单 Verdict**(与 spiral 逐层判定不同),必要时前置到 `process_layers` 之前
  - [ ] spiral 判定逻辑(4596-4613)迁移/扩展为 shared 判定入口,避免与 `SpiralVase` 两套判定冲突
  - [ ] 发射分支(5432-5469):命中 continuous 模式时不逐 region 发射,按 `out_plan.order` 输出单条连续 G1 链,使 `travel_to`/`needs_retraction` 无可触发段
  - [ ] `process_layers`(3660/3762):管道装配处插入新过滤器,槽位与 spiral 相同(冷却前),开关控制是否实例化
- [ ] 转移点曲线判定(5.2 第 4 步):相邻层 `end_n == start_{n+1}`,Δ ≤ 预算,过渡段不离体(需把切片多边形/体积引用传入,注意 TBB 管道线程安全,只读共享)
- [ ] 底实心层:复用 transition_in 语义,先常规打底再单链起步;顶封口层默认常规收尾或敞口
- [ ] (可选)GUI 选项暴露:Print 设置页加 `continuous_print_mode` 勾选,默认关

### M3 出口标准

- [ ] 开关关闭时:同一工程切片输出与基线**逐字节 diff 无差异**
- [ ] 开关开启 + 花瓶类模型:生成连续 GCode;判定失败模型:整单回退常规,输出合法

---

## 4. M4:贯通 + 端到端验证

设计依据:6.2。

- [ ] 零空驶断言工具(离线,可自动化):解析输出 G-code,统计
  - [ ] 挤出段之间"E 增量≈0 且 X/Y 变化"的段数(应为 0)
  - [ ] 层内 G1 是否首尾相连成单链
  - [ ] 层界处是否有 Z 回退/回抽
- [ ] 几何合法性:平滑滑移段采样点落在模型切片多边形/体积内(防穿模/悬空)
- [ ] 回归:默认关闭时输出与基线逐字节 diff
- [ ] 打印实测:小尺寸薄壁花瓶(直壁 / 斜壁 / 非圆截面),目检层界焊缝、强度、挤出一致性,重点观察转移点曲线附近是否积料/凹陷

---

## 5. 全程纪律(每个里程碑必做)

- [ ] 遵循 AGENTS.md:C++17、PascalCase 类 / snake_case 函数、`#pragma once`、TBB 共享状态只读
- [ ] 不改动既有默认行为、profile、.3mf 兼容性
- [ ] 每个里程碑提交前跑 `tests/libslic3r` 全量 + 基线 diff
- [ ] 代码保持精简,review 前手动删减 AI 生成的冗余代码
- [ ] 开放问题 O1–O4(设计文档第 7 章)不在 v1 范围内,遇到相关需求先记录不实现

---

## 里程碑依赖关系

```text
环境构建(0) ──► M1 算子+判定器 ──► M2 过滤器原型 ──► M3 管线接入 ──► M4 端到端验证
   (可并行准备)      (纯数据层,可独立交付)
```
