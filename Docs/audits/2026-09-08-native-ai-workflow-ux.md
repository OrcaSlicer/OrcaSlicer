# Orca 原生 AI 样板流程 UX 审查

## 基线与证据边界

当前分支 `codex/continue`，HEAD `ad4f74b97ae89d3cd9371d0c3214cd31f3b6269f` 加当前工作树修改。保留上一轮交互修复及 Python 清理。只核对当前仓库，未比较被阻塞的来源分支。

新功能是原生 wxWidgets 界面：`ModelGenerationPanel.hpp:37` 为 wxPanel，`AI/ModelGeneration/ModelPreview3D.hpp:30,624` 为 wxPanel 内 wxGLCanvas；`AI/SmartSlicing/SmartSlicingPanel.hpp` 为 wxScrolledWindow。没有用 WebView 承担这两个主要流程。

本轮开始时通过 Computer Use 列出运行窗口：唯一 Orca 窗口来自另一个工程 `10_3DMaker_GPT6`，不是本仓库的预览程序。此前本任务的隔离实例启动被工具策略拒绝；没有换工具或入口重试。因此以下初始问题是**代码确认**或**待实机验证**，不是新版真实操作结论。上一轮 64 个用例、581 个断言通过仅是本轮代码修改前的基线。

## 按影响排序的问题

以下行号是本轮修改前位置，之后以符号定位为准。

| 优先级 | 问题与用户影响 | 证据 | 状态 |
|---|---|---|---|
| P1 | “打印宽度”无法把手办调整到 120 mm，容易将设计参考误认为模型尺寸。 | `ModelGenerationPanel.cpp:658,3866` 的 `current_print_settings()` 只组装图像设置；`IModelArtifactConsumer.hpp:19` 的导入请求没有尺寸操作。 | 代码确认；误解程度待实测。 |
| P1 | 底座只能写进重新生成的提示词，不能给已满意模型直接加底座。 | `ModelGenerationPanel.cpp:3319` 的 `on_apply_model_refinement()`；`tools/ai/model_refinement.py:58` 生成建议。原生 `GUI_ObjectList.cpp:2485` 的 Cylinder 默认放在前侧，不能直接冒充底座功能。 | 代码确认能力缺口。 |
| P1 | 成功导入后清除当前任务与准备提示词，返回生成页缺少明确的当前作品继续入口。 | `ModelGenerationPanel.cpp:2670` 之后的 `import_local_artifact()` 清理及 `m_prepared_prompt->Clear()`；历史模型仍保留。 | 清理行为代码确认；返回实际布局待测。 |
| P1 | AI 页固定白底、浅色卡片，主题更新路径没有转发到 AI，且预览清屏色固定。 | `ModelGenerationPanel.cpp:92,323,351`；`ModelGenerationFeatureHost.cpp:23`；`ModelPreview3D.hpp:562`；`MainFrame.cpp:2560,2630`。 | 代码确认；深色主题实际对比待测。 |
| P1 | 正式切片完成/失败后停止监听工程变化；后续编辑仍可能显示旧结果。 | 修改前 `SmartSlicingCoordinator::refresh_revision()` 只检查 `can_cancel()` 状态，`SmartSlicingViewModel::needs_polling` 仅包含正式切片中。 | 代码确认；本轮新增终态失效回归。 |
| P1 | 原生本地检查入口受生成服务可用性控制，离线无法稳定继续准备。 | 修改前 `AIDesktopFeatureHost::apply_availability()` 仅在兼容且有 config proposal 能力时通知创建入口。 | 代码确认；本轮将原生准备入口与生成服务发现分开。 |
| P2 | 图像处理的喷嘴、线宽初始值固定，界面容易让用户重复输入设备条件。 | `ModelGenerationPanel.cpp:659–661`；当前设备能力由 Orca 适配器读取，需区分图像参考与实际打印配置。 | 代码确认。 |
| P2 | 普通提示使用模态对话框，打断操作；必要风险确认应保留。 | `ModelGenerationPanel.cpp:1609,1614,1718,1724,3937`；生成确认 `:1891`。 | 调用方式代码确认；打断频率待测。 |
| P2 | 构造时 FromDIP 不等于跨屏 DPI 更新；固定工作流列宽和 Wrap 尺寸可能挤压内容。 | `ModelGenerationPanel.cpp:350,992`；`SmartSlicingPanel.cpp:225,268,372,452`；MainFrame DPI 转发缺失。 | 固定值代码确认；截断为待测假设。 |

已有可保留的连接：模型下载与导入自动衔接，不需要手动下载；导入使用当前打印板和原生颜色匹配；SmartSlicing 的 `WorkspaceRevision` 包含对象/实例/体积变换与配置，支持变更后检查失效。固定物理耗材与虚拟叠色在 `OrcaWorkspaceAdapter::printable_palette()` 分开表达。不能将这些代码结论等同于实物颜色、供应商稳定性或完整 UX 验收。

## 最小样板方案

保留生成标签页与原生准备页。上传固定参考图，以模拟响应得到固定模型；确认后直接加入当前项目。在准备页当前模型上设置整体高度 120 mm、按需添加圆柱底座，显示当前设备，随后重新检查。用户可以跳过优化，继续原生参数与切片；没有必须逐页完成的向导。

尺寸和底座使用本地确定性操作，原始生成 OBJ 不覆盖，打印版本保留在 Orca 模型与原生撤销历史中。整体高度明确包含底座；底座不是支撑或裙边，也不是已验证的打印效果。复杂外观重设计仍使用现有描述与新版本，不新增通用自然语言编辑器。

备选方案：仅加原生工具说明最小，但不能补齐底座定位；把编辑塞进生成服务会引入重复任务与供应商依赖。本轮采用原生模型准备控件与薄适配器，只处理当前板单个选中且仅一个实例的对象，其他场景给出原生手动工具路径。

## 当前分支可复用接口

| 用途 | 当前源码证据 | 本轮使用方式与边界 |
|---|---|---|
| 字体与布局尺度 | `GUI_App.hpp:467–471` 的 `normal_font()`、`bold_font()`、`em_unit()` | 使用 Orca 字体；保留原生 sizer、AUI 和滚动面板。字体刷新不等于所有固定边距已完成跨屏验证。 |
| 主题 | `GUI_App.hpp:451–452` 的窗口/文字颜色；`MainFrame::on_sys_color_changed()` | AI 子树通过 `AIWindowAppearance.hpp` 继承主题；耗材色卡和颜色选择器标记为 `ai_content_color`，不参与界面染色。 |
| 控件和布局 | 当前两个面板中的 wxButton、wxTextCtrl、wxCheckBox、wxBoxSizer、wxScrolledWindow；SmartSlicing FeatureHost 中 wxAuiPaneInfo | 新增直接尺寸输入、底座复选框与就地反馈。没有替换 wxWidgets、全局样式或主导航框架。 |
| 非阻塞反馈 | `Plater.hpp:895–896` 的 `get_notification_manager()`；`NotificationManager.hpp:230–235` 的通知接口 | 本轮输入问题采用所属面板的文本提示，避免把局部校验变成全局通知或模态框。付费、上传与风险确认继续保留。 |
| 撤销与快捷键 | `Plater.hpp:622,766,926` 的 undo、can_undo、TakeSnapshot；`GUI_ObjectList.cpp:259` 与 `GLCanvas3D.cpp:3393` 的原生撤销入口 | 每次尺寸/底座提交建立一个原生快照；不注册覆盖 Ctrl+Z 的新快捷键。文本框自身撤销与工程撤销的焦点行为须实机检查。 |
| 几何缩放 | `ModelInstance::set_transformation()`、`ModelObject::instance_bounding_box()`、`ModelObject::add_volume()` | 直接按 mm 计算实际几何；不借用依赖 gizmo 缓存及英寸转换的 `GizmoObjectManipulation::on_change()`。ModelObject 的复制/赋值接口是 private，不改核心 API 来开放它。 |

## 修改前后与实现位置

| 修改前 | 修改后 | 实现 |
|---|---|---|
| 参考图的“打印宽度”容易被误当成模型尺寸。 | 明确标注参考图细节简化；实际总高在准备页设置，默认样板值 120 mm。 | `ModelGenerationPanel.cpp:651,669`；`OrcaModelPreparationPanel.cpp`。 |
| 加底座需要修改提示词，再次生成。 | 在当前选中对象上添加 3 mm 圆底座，总高包含底座；自动覆盖 XY 投影并留出 2 mm 边缘，底座与主体重叠 0.2 mm。 | `OrcaModelPreparation.cpp::prepare_model/apply_model_preparation`。 |
| 导入后清空准备提示词，返回没有当前工程入口。 | 保留准备提示词和生成原件；提供“返回当前工程 · 准备页”，不会再次导入。已导入结果有继续编辑提示。 | `ModelGenerationPanel.cpp:766,2685,2993`；`ModelGenerationFeatureHost.cpp:22`。 |
| 本地功能等待生成服务发现。 | 原生准备/检查在桌面 Host 启动时启用；生成服务按原路径发现和重试。 | `AIDesktopFeatureHost.cpp:42`；原 `enable_smart_slicing()` 幂等保护。 |
| 终态结果停止监测；失效时仍可能显示候选数字。 | 记录应用后的 revision，完成/失败后继续监听；后续模型或配置变化使报告、候选和专属撤销入口失效。无法读取应用后版本时明确提示“无法核实”，不谎报工程已变。 | `SmartSlicingCoordinator.cpp`、`SmartSlicingViewModel.cpp`、`SmartSlicingPanel.cpp`。 |
| 缺输入、重复颜色等普通问题弹窗打断。 | 10 处普通信息对话框改为就地提示与必要的输入焦点。 | `ModelGenerationPanel.cpp::show_input_hint` 及调用点。 |
| 固定白底和浅色预览背景。 | AI 界面继承当前字体/主题，OpenGL 清屏色随主题变化，真实耗材颜色保留。 | `AIWindowAppearance.hpp`、`ModelPreview3D.hpp`、MainFrame 的两处转发；准备面板在可见时同步主题/字体。 |

编辑仅接受当前未锁定打印板上完整选中的单实例对象，拒绝切片中、活动 gizmo、非法数值和切割对象。使用稳定 ObjectID 校验所显示对象与提交目标，快速切换模型时先刷新提示，避免改错对象。已有自动底座拒绝重复添加；只改高度可继续同比例缩放。同高度的重复提交不产生新快照。原 OBJ 不被该操作打开或重写，打印版本保存在当前 Model、原生撤销历史和用户保存的 3MF 中。

底座继承首个正体积的耗材 ID，由当前 FDM 配置解析。未新增 CMYK 路径，也没有把图片颜色或屏幕外观当作已校准的实物效果。任意模型的接触强度、支撑和打印质量仍需原生预览与实际打印验证。

## 验证证据

固定样例是测试内生成的长方体和圆柱，不冒充供应商生成的手办。未调用付费生成、上传用户照片或联系测试人员。

- `preparation-tests-pass3.log`：4 个测试、76 个断言通过，包括普通/旋转/镜像实例的 120 mm 总高、0.2 mm 接触重叠、原模型及涂色数据保持、非法输入无修改、重复底座拒绝、Orca BBS 3MF 保存重开保留尺寸和涂色，以及原生试切。
- 原生试切固定配置显式设置 100/256 mm 设备高度、0.25 mm 层高。100 mm 路径被实际 Print 校验拒绝；256 mm 路径完成隔离试切并得到非零时间、材料估算；当前工程保持原状。它证明该样例能进入原生切片引擎，不证明 GUI 全流程或实物可打印。
- `python-tests.log`：41 项现有本地模型质量/视图测试通过。
- 独立只读复核确认公开变换/add_volume 路径及原生刷新顺序；发现并修复快速重选对象错配、未知 revision 被误报为工程变化两个边界。复核没有执行 GUI。
- 最终 Release 构建于 **23:20:23** 完成，退出码 0；保留既有 `LNK4098` 链接警告。最终相关 C++ 回归于 **23:21:37** 完成：**89 个用例、851 个断言全部通过**，包含新增终态失效、未知版本提示和既有取消/重试、应用/撤销、颜色及本地编辑回归。
- `python scripts/verify_ai_integration.py --json` 退出码 1，仍为本轮开始前的 4 项发现：`release/test_verify_package_contents.py:39`、`tools/ai/test_diagnostic_failure_flow.py:106` 的测试凭据字面量，以及缺失 `codex/model-generation`、`codex/smart-slicing` 两个来源引用。没有新增架构/预算问题，没有弱化检查或重新比较被阻塞来源。
- `git diff --check` 和项目 Skill 的 `quick_validate.py` 均通过。

可复查输出：[完整 C++ 日志](../../.tmp/native-ai-ux-20260908/cpp-tests-final.log)、[构建结果](../../.tmp/native-ai-ux-20260908/build-verified-result.json)、[构建日志](../../.tmp/native-ai-ux-20260908/build-verified.log)、[Python 测试](../../.tmp/native-ai-ux-20260908/python-tests.log)、[集成检查全部发现](../../.tmp/native-ai-ux-20260908/integration-final.json)。

复现 C++ 回归时在仓库的 `resources` 目录执行，使用当前构建的测试程序：

```powershell
../build/tests/slic3rutils/Release/slic3rutils_tests.exe '[SmartSlicing],[ModelGenerationPresentation],[OrcaModelPreparation],[ModelColorImport],[VertexColorRegion]' --order rand
```

只查本次尺寸、底座、原件/3MF 和原生设备高度试切时，过滤 `[OrcaModelPreparation]`。终态 revision 的失效/未知处理另在 `[SmartSlicing]` 回归中。

本地预览已在 **23:25:31** 更新至 `build/ux-preview/orca-slicer.exe`，需保留整个目录的 DLL、resources 和 Python。EXE 与 `OrcaSlicer.dll` 均经 SHA-256 核对，与最终构建一致；自带 Python 3.12.13、Pillow 12.2.0 的独立 PNG 编解码检查通过。该目录未被启动作 GUI 验收，也未制作/发布安装包。参见 [产物校验](../../.tmp/native-ai-ux-20260908/preview-hashes.json)、[安装结果](../../.tmp/native-ai-ux-20260908/preview-install-result.json)、[运行时检查](../../.tmp/native-ai-ux-20260908/preview-runtime.json)。

## 主窗口回归步骤与未验证项

下列是待执行的实机验收清单，不是已通过的体验记录。使用本仓库产物、独立且已获准的测试数据目录；记录 EXE/DLL/模型 SHA、设备预设、主题、窗口尺寸、DPI 与操作截图。已有平台阻塞未解决前，不换工具、任务或入口重试被拒绝的启动。

| 场景 | 操作与应观察的结果 | 本轮证据层级 |
|---|---|---|
| 主任务 | 固定参考图 + 模拟生成响应 → 核对参考图/模型外观标签 → 原生颜色匹配导入 → 在智能切片面板设总高 120 → 加底座 → 检查/比较 → 跳过优化或明确应用 → 原生切片/预览。 | 几何、3MF、原生试切离线通过；整个主窗口路径未执行。 |
| 失败恢复 | 空描述、自定义风格缺失、重复颜色；非法/NaN/过小高度；导入取消；检查/试切失败、重试、取消后继续。 | 新几何失败保护与现有自动回归；新增反馈布局、焦点与导入取消实机未执行。 |
| 返回继续 | 生成中及导入后往返准备页；输入/结果仍在；返回按钮不重复导入；仅原生项目版本随编辑改变。 | 代码路径已接线，实际状态与布局未验收。 |
| 目标一致 | A 对象填值后快速选 B 并立即应用；先提示对象变化，不能修改 B；再确认后才提交。 | 有 ObjectID 保护及独立代码复核；实机时序未执行。 |
| 版本/撤销 | 撤销和重做尺寸/底座；再编辑其他对象后不误用专属候选撤销；保存重开 3MF 后总高/涂色/底座仍在。 | 3MF 往返通过；真实 GUI Undo/Redo 未执行。 |
| 结果失效 | 检查前后、候选就绪、正式切片完成/失败后，修改高度、底座、打印机或材料；旧报告/候选失效，重新检查使用新配置；自己的方案应用不立即失效。 | Coordinator 模拟版本回归及原生设备高度试切；全适配器 + GUI 定时刷新仍待实机验证。 |
| 主题与缩放 | 浅色/深色切换；1280×720、1600×900 窗口；100%/150%/200% DPI，含跨屏；检查按钮、长提示、滚动区、色卡和预览对比度。 | 字体/颜色 API 与编译证据；没有视觉验收。固定边距的跨屏表现仍需检查。 |
| 普通 Orca | 离线、本地服务不可用时，普通模型导入、手工尺寸/摆放、预设选择、保存、切片仍可用；输入框撤销与画布/对象列表工程撤销不冲突。 | 相关本地引擎回归；完整普通 GUI 操作未执行。 |

尚未实现任意自然语言的本地几何编辑、多实例批量底座、任意底座造型与尺寸编辑，也未验证人脸相似度、真实供应商稳定性、CMYK 实物颜色或打印强度。高级参考图喷嘴/线宽仍是图像简化参数；实际打印使用 Orca 当前设备与工艺，不要求用户为样板再次输入这些字段。
