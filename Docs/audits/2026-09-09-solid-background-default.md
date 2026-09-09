# 后续设计图默认纯色背景

2026-09-09，`codex/continue`，基于 HEAD `ad4f74b97ae89d3cd9371d0c3214cd31f3b6269f` 的已有未提交工作树。

用户在确认棋盘格实际画在 RGB 图片里后要求“之后都去掉”。生产入口 `generate_image`、`generate_geometry_reference_image`、`preprocess_image` 现在统一发送 `background=opaque`，提示词要求均匀不透明纯色背景，优先中灰色，必要时更换背景色以区别白衣、头发、底座；禁止棋盘格、网格、背景纹理、渐变、阴影和光晕，保留完整主体和留白。文字、图片和组合输入沿用相同规则。三个主风格保持原有颜色和身份要求。

不对历史图像或原始用户图像做阈值抠图、覆盖、自动重生成，也不新增失败后的第二次付费请求。内部旧限色/多视图的真实 alpha 能力保留；此次调整覆盖当前不限色设计图生产主路径。

## 验证

- 三个入口 × 单色写实/多色写实/卡通共 9 组离线请求捕获：提示词没有请求透明背景，JSON 或 multipart 均为 opaque，每次只有一次请求，源图字节不变。
- 更新既有人像测试中与新背景策略对应的断言，继续检查 RGB/RGBA 返回、人脸检测证据及几何副本不回贴照片、不改写供应商原始返回。
- 系统 Python 和随程序打包的 Python 分别运行 `test_openai_preprocessor`、`test_unrestricted_creation`、`test_printable_sidecar_pipeline`，各 76 项中 75 项通过；剩余一项是已有的 `test_legacy_create_routes_ignore_palette_fields_including_unsupported_counts`，离线环境缺少文字预处理配置时返回 feature_unavailable。与前次完整回归的失败标识相同，无新增失败；`git diff --check` 通过。
- 集成检查保留全部四项已有错误：`release/test_verify_package_contents.py:39` 和 `tools/ai/test_diagnostic_failure_flow.py:106` 固定模拟 TRIPO_API_KEY 字面量，以及缺少 `codex/model-generation`、`codex/smart-slicing` 本地来源 ref。未跳过 Git 检查或削弱规则。
- 仅修改 Python 提示词和请求参数，不涉及 C++、界面或项目格式。本轮没有收费图片/3D 调用，不能据此保证真实供应商一定遵守背景指令。

## 本机生效时点

更新已复制至 `build/ux-preview/resources/tools/ai/openai_preprocessor.py`，原运行文件备份至 `.tmp/solid-background-20260909/previous-runtime-openai_preprocessor.py`，源码/运行文件 SHA-256 一致。

实际主窗口检查发现用户当前 3D 任务已在运行（77%），因此未关闭应用、重启服务或更换已提交图片。当前进程仍使用已加载的旧模块；本轮任务完成后重启 Orca，新生成的设计图才使用新默认要求。棋盘格旧图不会因更新自行变成纯色图。

证据目录：`.tmp/solid-background-20260909/`；离线测试结构化结果保存在 `.tmp/release-readiness-20260909/python-solid-background*-test-rerun-summary.json`。
