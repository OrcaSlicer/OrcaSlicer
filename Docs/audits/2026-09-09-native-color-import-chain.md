# 原生导入配色：链路、问题和本轮改进

2026-09-09。本记录来自当前 continuation 工作树的源码检查及开源原始资料。没有比较被禁止的旧智能切片工作区，没有调用生成服务。

## 实际链路

`Plater::priv::run_textured_mesh_import_dialog` 收集当前工程的实体耗材与虚拟叠色项 → `TextureImportDialog` → `TexturePainting` → `TextureToColor` → 面标签、耗材映射 → 用户确认后 `Plater` 写入工程。

原有导入窗口首次运行 `start_computation(true, true)`，即自适应分色，内部 `target_colors_num=0`。它没有收到生成预览中的六色色板，因此截图中的三色是新一轮自适应计算结果，不能解释为生成页面六色已经应用。4/8/16 是快捷按钮，数字输入允许其他数量。

固定数量和自适应分色都先对 RGB 去重，以唯一色值等权聚类；原生默认颜色差异采用 CIEDE2000。随后按面分配最近中心，再清理色块边界。这与生成预览的面积权重、Oklab 色相保护和用户改色分组不同。

旧的“自动合并同耗材”实际行为是：先找工程中最近颜色；当 CIEDE2000 色差大于 5 时自动提出新增实体耗材。新增项虽暂存在弹窗内，用户确认后会成为工程材料项。因此“目标六色”不等于“使用已有六卷耗材”。

CMYW 和 RYBW 按钮读取现有标准配方表，必要时原逻辑会提出新增标准基础材料并建立虚拟叠色项。CMYW 中 W 是白色，这套功能不能改名成用户所说的 CMYK 实测叠色。空间/层次效果需要实际材料、层高和打印样本标定。

## 本轮最小实现

- 普通导入保留中性默认选项，不改变自动分色与既有映射策略。
- `TextureImportOptions` 作为原生 GUI 的可选上下文，包含初始目标数、实体耗材预算、仅用已有耗材、Z-up 观察，以及两组固定色板。
- 两组固定色板分别是原始分组中心 `fixed_mapping_palette` 和编辑后的输出色 `fixed_palette`；在 Oklab 中按与生成预览相同的亮度权重 0.35 分类，再使用同索引输出色。输出色改变不会反过来移动原分组。
- 固定色板绕过二次聚类，不人为给未出现的颜色植入种子面；默认关闭二次边界清理，并避免零清理时不必要的 CGAL 半边转换。
- AI 来源仅匹配已有工程耗材，不因色差自动新增实体耗材。既有工程超过计划六色时保留全部配置并提示，不擅自删槽。
- 用可点击并保持的“原始颜色 / 目标分色 / 耗材效果”替换悬停即切换的隐式视图；新增六色快捷键；显示目标色、实体耗材、叠色配方、新增实体耗材数量。
- `Z-up` 只修改预览观察，不修改导入几何坐标。

## 开源资料与取舍

| 原始资料 | 可用方法 | 本轮取舍 |
|---|---|---|
| [BambuStudio TextureToColor](https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/TextureToColor/TextureToColor.cpp) | 纹理采样、分色、面标签与边界处理 | 复用项目已有管线，修好输入一致性；不整段替换核心 |
| [OrcaSlicer TriangleSelector](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/src/libslic3r/TriangleSelector.cpp) | 原生面涂色、区域选择、分割面表示 | 保持最终写回原生涂色数据与工程行为 |
| [Oklab 作者说明](https://bottosson.github.io/posts/oklab/) | 感知颜色空间，公开矩阵 | 统一屏幕试色和固定色板分类；当前不等于材料光学模型 |
| [libimagequant](https://github.com/ImageOptim/libimagequant)、[API 文档](https://pngquant.org/lib/) | 固定颜色、权重、重要区域、受限色板及可关闭抖动 | 可作为后续离线对照；首版不引入 Rust/C 接口和新构建依赖。图像像素抖动不直接等于可打印面分区 |
| [nQuantCpp](https://github.com/mcychan/nQuantCpp) | PNNLAB、Wu 等量化算法对照 | 适合离线质量实验；Windows 图像依赖和区域连通性不适合作为当前跨平台核心替换 |
| [Colour 的 CIEDE2000 文档](https://colour.readthedocs.io/en/develop/generated/colour.difference.delta_E_CIE2000.html)、[Sharma 测试数据](https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/) | 色差计算和标准测试对 | 项目已有 CIEDE2000，后续用标准色对交叉验证；不为现成色差再加运行时库 |

检索中 Orca main 的 `TextureToColor/TextureToColor.cpp` 路径返回 404，因此本记录不把当前分支的整个导入弹窗误称为官方 Orca 主线现有功能。

## 验证范围与限制

增加 `[ModelVertexColors][FixedPalette]` 离线测试，覆盖换色后分组不漂、未用颜色不强行出现、低面数顶点色路径、面颜色路径、无效色值和两组色板长度不一致。构建、执行结果与实际主窗口验收由本轮总体验证记录汇总。

固定目标 RGB 的一致性不等于逐像素一致：生成预览按片元采样，原生高面数模型按面颜色分配；颜色边界仍受网格密度影响。照明、抗锯齿也能让屏幕像素超过六种 RGB。实际耗材映射继续用原生 CIEDE2000，必须在“耗材效果”中比较损失。

CMYK 自动标定、识别人脸并自动保护五官、预测换色次数与打印耗时不在这些改动中冒充实现；后两项数值应来自真实试切，实物颜色应来自打印色卡。
