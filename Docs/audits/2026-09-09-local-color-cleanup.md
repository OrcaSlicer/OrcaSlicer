# 局部去杂色与 3D 美颜选型

## 本次范围

用户发现绿色领子中混有黑色杂色，要求改善并优先复用成熟开源能力。本次在“3D 美颜工作台”增加“局部去杂色”，复用现有点选、擦除选区、后台处理、取消、前后对比、接受新版本、回退和重做。大片杂色仍可用相邻的“局部改色”统一。

自动处理只针对用户选区内、被明显主色包围的小色块。颜色分组使用现有试色色板和 Oklab 距离，清理结果取自周围已有颜色；不对全模型重新量化，不对 RGB 做模糊，不改变几何。未选区域及共享边界受保护。不能据颜色和面积自动判断眼睛、纽扣或花纹的语义，必须保留可比较、可放弃的预览。

六色分组中心用于识别颜色类别，最终保存的是局部修整后的原始顶点色模型。保存新版本后，可再到“六色试色”确认配色，再沿既有导入配色流程进入准备页。修整不会自动改变工程耗材、切片或调用生成服务。

## 开源方案核对

| 方案 | 已有能力与适用范围 | 本项目决定 |
| --- | --- | --- |
| [libigl](https://libigl.github.io/tutorial/) | 三角邻接、连通域、ARAP、表面数据处理。项目已包含 2.6.0 子集，基础文件 MPL-2.0，其他模块按文件许可区分。 | 优先沿用现有 C++ 网格基础；未来锚点拖拽形变可复用 ARAP，不另写形变求解器。 |
| [CGAL](https://doc.cgal.org/latest/Polygon_mesh_processing/index.html) | 网格修复、约束处理。项目已固定 5.6.3；[许可按包区分](https://www.cgal.org/license.html)。 | 继续承担几何可靠性，不当作人物语义美颜。 |
| [MeshLab / PyMeshLab](https://pymeshlab.readthedocs.io/en/latest/filter_list.html) | Taubin、HC、保特征平滑、选区和颜色滤镜。[PyMeshLab](https://github.com/cnr-isti-vclab/PyMeshLab) 与 [VCGlib](https://github.com/cnr-isti-vclab/vcglib) 为 GPL-3.0。 | 适合后续在 Python sidecar 中做离线算法对照；本次不新增运行时。颜色 Laplacian 会产生中间颜色，不能直接作为六色杂色清理。 |
| [Blender](https://docs.blender.org/manual/en/4.4/sculpt_paint/sculpting/tools/color_filter.html) | 成熟雕刻、遮罩、颜色填充及平滑交互；[GPL](https://www.blender.org/about/license/)。 | 参考遮罩、笔刷、前后对比的交互；嵌入整套 Blender 的打包与进程成本过高。 |
| [Open3D](https://www.open3d.org/docs/release/python_api/open3d.geometry.TriangleMesh.html) | Taubin、ARAP、几何连通面及面积统计；[MIT](https://github.com/isl-org/Open3D/blob/main/LICENSE)。 | 适合扫描点云等后续需求；与当前已有 libigl/CGAL 的能力重合，本次不重复引入。 |

核对现有原生 `TextureToColor/ColorUtils.cpp`：`smooth_region_labels` 不仅整理标签，还会转换 CGAL 网格并处理几何边界。该入口缺少当前局部美颜的选区和原始行保留约束，因此不能直接用于承诺“只改颜色”的操作。本次复用既有网格边表、色板距离与文件版本机制，只补局部小色块处理规则。

这些成熟库能提供几何、颜色和选择算法，没有直接可嵌入且已验证同时满足人物理解、六色耗材与实际打印约束的完整“3D 美图秀秀”。产品层仍需承担选区、保守参数、预览、版本恢复和导入衔接。

## 验证

验证记录位于 `.tmp/color-cleanup-20260909/`：

- Windows Release 主程序和测试程序构建成功；局部美颜 20 项 / 4,132 断言、选区 17 项 / 147 断言、试色色板 11 项 / 299 断言通过，共 48 项 / 4,578 断言。初次过滤器有两个标签未匹配，随后用实际标签分别执行并保留独立结果，未将未运行项算入通过。
- `verify_ai_integration.py --json` 最终通过，errors 为空，未跳过 Git 检查或放宽规则。`git diff --check` 通过。
- 真实人物 OBJ：953,446 顶点 / 1,906,896 面，原始 SHA256 `bbb5929a852be262457cd3527d495bb143b64df071d11f60e90a87614233e390`。主窗口点选领口 64,121 面，力度 35，清理 117 个小色块、228 个顶点；原始色值统计由 39,863 变为 39,855。
- 逐行核对几何坐标、面序、法线、UV、材质保持，未选面引用的顶点颜色变化为 0，原件 SHA256 保持。输出 SHA256 `9342a7158d9f481df1c4d716eef146f966a88c7b2d3f3a7e9064700d550481a3`。
- 相同选区离线复测 13,280 ms，包含读写与 SHA 校验，输出哈希与主窗口保存结果一致。这是单次本机测量，不是跨硬件性能承诺或完整 GUI 端到端耗时。
- 主窗口已验证点选、去杂预览、处理前对照、接受新版本、回退、重做、未选区提示、切到准备页再返回；准备页保持空板，没有自动导入、切片或修改耗材。保留原有用户工程窗口，验证使用独立 datadir。
- 界面复查修复了候选模型仍显示旧色值统计、首次前后对比因统计行数变化而跳动，以及未选区提示初次显示截断的问题。

较大且连续的领口深色区域被保留；本次不宣称自动识别五官或移除全部烘焙阴影。对明确要抹去的大片深色，使用已有“局部改色”。当前去杂入口针对具有顶点 RGB/RGBA 的三角 OBJ，纯纹理模型尚不支持。本次没有付费生成、实物打印或跨平台运行验收。

最终验证程序：`build/color-cleanup-preview/orca-slicer.exe`，库 SHA256 `c389011735bd7978d127e21723eed479871eea8f6021cbd9ec96d471f37c7932`，独立配置目录 `.tmp/color-cleanup-20260909/user-data`。这属于本机验证版本，没有发布或推送。

最终库已重新启动复查：未选区提示完整换行；同一选区再次处理得到相同输出 SHA256。处理后正确显示 39,855 原始色值，处理前显示 39,863，模型对比视口保持一致。最终截图 `final-before.png`、`final-after.png` 位于上述验证目录；新版窗口留在处理后预览，原用户工程窗口继续保留。
