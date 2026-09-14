# 写实人像 3D 打印的特征、美感与光影研究

## 1. 研究结论

写实人像打印值得同时研究“像本人”和“本人愿意接受的美化”，但它们必须分别评价。把一张照片变漂亮、把一个数字头像渲染漂亮、把一个六色实体打印漂亮，是三个不同的问题。现有研究和产品已经提供了其中不少工具；尚缺少的是把身份保持、可控美化、几何细节和实际打印表现连起来的可靠流程。

四个方向都有技术依据，其中最有把握的组合是：保留原始照片作为身份依据，建立可编辑的脸部几何，允许轻度且可撤销的美化，去除颜色中的原始照明，再根据打印尺寸、材料和观看光照处理细节。通用图生 3D 可以继续承担整体人物、服饰和造型，但应把脸部相似度作为独立问题评估。这里提出的是技术判断和待验证假设，不代表任何候选方案已经通过实体打印测试。

| 方向 | 研究判断 | 最适合承担的任务 | 关键边界 |
| --- | --- | --- | --- |
| 先做 2D 美颜或特征增强 | 值得优先做受控对照 | 改善输入质量、明确用户喜欢的形象、突出个人特征 | 更漂亮的照片未必带来更像的几何；生成的侧面不是真实观测 |
| 阿莱光影、徕卡色彩 | 数字呈现成熟，实体迁移有条件 | 预览灯光、肤色和色调设计；实体展示灯具 | LUT 和虚拟灯光不会自动成为耗材的光学属性 |
| 凹凸与自然光影补偿有限色 | 物理依据充分，打印效果需专项验证 | 强化可打印的眼睑、鼻翼、唇线和轮廓；保留自然明暗 | 不能从单张阴影唯一反推出真实脸型，也不能保证所有光照下都更像 |
| 生成后可调的 3D 美颜 | 可控性最好，已有成熟几何工具 | 局部形变、表情、细节强度、肤色与关键色区调整 | 任意 Tripo 网格没有统一的脸部拓扑，需先解决定位和形变约束 |

值得关注的业界参照是 Headshot 3 和 FaceBuilder。前者把照片修正、面部特征滑杆、正侧面形状调整和纹理重投影串在一起；后者提供照片驱动的交互式头部拟合。它们说明“专用头部重建加人工修正”已有产品基础，但厂商展示不能替代六色打印验收。Headshot 涉及的内容授权也不能按普通软件购买许可推定。[^12][^14][^15]

本报告以普通手机照片为起点，以可选的多角度照片或短视频作为高保真输入，以最多六种物理耗材的 FDM 人像打印作为主要落地场景。其他全彩打印技术只作为对照。资料范围截至 2026 年 9 月 10 日；论文指标、作者报告、厂商功能说明和本报告的推导分别标明。

## 2. 从照片到实体：相似度损失发生在哪里

一张人像照片同时混合了脸型、表情、镜头透视、皮肤本色、妆容、光照和相机处理。图生 3D 系统需要解释这些信息，还要补全没有拍到的部位。若只用一张正面照，鼻子的突出程度、眼眶深度、耳朵位置和后脑形状仍存在歧义。增加面数主要增加几何表达容量，增加纹理分辨率主要增加颜色采样容量；两者都不能保证新增信息属于这个人。

可以用下面的关系区分数字效果与实体效果。这里是分析框架，不是拟合后的性能模型：

`屏幕图像 = 色彩与显示变换 [ 渲染 (几何、反照率、材质、灯光、相机) ]`

`实体外观 = 真实光照下的反射 (打印几何、耗材及表面、观看方向)`

屏幕里的高光、皮肤透光感、柔焦和景深可以很好看，但实体能保留的是实际打印出来的形状、颜色与材料行为。景深属于观察或拍摄条件，凹凸属于物体几何。把两者区分开，才知道应该改脸型、颜色、打印工艺，还是展示方式。

建议把相似度分为四层检查：第一层是正面与侧面的轮廓、五官位置和比例；第二层是眼睑、鼻翼、嘴角等局部形状；第三层是眉毛、瞳孔、唇色、发际线等颜色特征；第四层才是皮肤微细节、光泽和摄影风格。这是面向本产品的诊断顺序，不是通用的人脸感知定律。

数字头像还有一个容易混淆的表示差异。LAM 一类 Gaussian 头像可以迅速生成具有逼真外观的可动画头部，但其核心目标是从不同视角渲染影像。即使另有辅助网格或导出功能，也不能据此认为主要外观信息已经成为封闭、可切片的实体表面。评估此类方案时应单独检查提取网格、去掉纹理后的脸型以及制造约束。[^42]

## 3. 方向一：在 2D 输入中美化与增强特征

### 3.1 两种不同的“增强”

第一种是改善照片条件：减轻过度透视、平衡曝光、清理遮挡、降低压缩和模糊，并尽量取得自然表情。它有助于减少重建系统解释错误信息的机会。第二种是主动改变形象：磨皮、改变眼睛和鼻子的比例、增强某些面部特点、调整表情。它会改变目标本身，需要保留原图并让本人确认。

“放大特征”应理解为增强这个人与一般脸形的差异，而不是统一大眼、尖下巴、高鼻梁。一个人的辨识点可能是较宽的鼻翼、特定的下颌线、眉眼间距、嘴角走势或轻微不对称。统一套用同一套美颜规则，可能恰好抹去这些区别。

在一个已经对齐、具有可比含义的形状空间中，可以把受控夸张写成示意关系：`目标形状 = 参考均值 + (1 + α) × (本人形状 - 参考均值)`。α 为零表示保持本人形状，小幅正值表示增强差异。但参考均值、特征空间和局部掩膜的选择都会影响结果；这个式子不能直接用来拉伸原始照片，更没有证据支持给所有人设置同一个最佳 α。

心理学研究对“夸张一定更像”给出了必要的限制。Rhodes 等人在 1997 年比较线描与照片肖像时发现，夸张肖像可以至少与原肖像同样易识别，但照片夸张没有呈现线描中那种更强的“超级肖像”效应。因此，漫画领域的经验支持测试轻度特征增强，却不能直接证明写实照片转实体也会提高辨识率。[^1]

### 3.2 可利用的 2D 技术

| 技术路线 | 可以做什么 | 对本任务的意义与限制 |
| --- | --- | --- |
| 关键点或分区形变 | 对眼睛、鼻翼、嘴角、下颌做小幅明确调整 | 控制直观，便于保留身份锚点；侧面深度仍缺少约束 |
| 身份条件图像生成 | 在参考人脸基础上修改光照、风格、表情和整体形象 | 能产生审美候选，但可能重画五官；应对照原图检查，而非只看生成图是否自然 |
| 局部修饰与纹理编辑 | 调整肤色、局部瑕疵、唇色、眉毛和头发 | 更适合把几何保持不动的美化单独评估；不能修正鼻形和眼眶结构 |
| 真实多角度照片 | 补足侧面轮廓和深度约束 | 提供新增观测；采集时需要尽量保持表情与头发状态一致 |

InstantID、PhotoMaker 和 PuLID 都是值得纳入候选库的身份条件图像生成路线。它们的输出仍是图像；身份条件是保持相似的约束，不是本人几何的证明。InstantID 官方明确区分 Apache 代码与仅供研究的检查点；PhotoMaker 的许可说明为 Apache 2.0 并保留第三方组件例外；PuLID 也需要按实际使用的 SDXL 或 FLUX 版本检查基础模型与依赖。[^2][^3][^4]

尤其要把代码许可与人脸权重分开。InsightFace 官方说明：代码为 MIT，而提供的训练数据及预训练模型有非商业研究限制，部分用途提供另行许可渠道。任何依赖这类识别权重的“开源美颜”组合，都不能只看外层仓库标记。[^5]

### 3.3 进入 3D 前的合理分工

建议保留两份不同用途的参考：原图负责约束身份，经过确认的美化图负责表达美感偏好。如果当前生成接口只能接受一份参考，就应把原图方案与美化图方案作为对照分支，而不是丢弃原图后只优化生成图的外观。

生成的正面、侧面和背面可以作为一致性检查材料，但它们不是独立测量。一个系统可能生成互相一致却并不真实的鼻形和后脑。已有真实侧面照时，不应让生成的侧面覆盖它；没有真实侧面时，应明确哪些部位来自推测。

此方向最值得验证的假设是：“轻度照片整理和本人确认的美化，能否在不降低身份认可的条件下提高成品接受度？”更强的假设“夸张后一定更像”需要另做实验。对只希望更好看的用户，可以提供美化幅度更大的候选，但产品应让其清楚看到与原貌的差异。

## 4. 专用脸部重建与业界能力对照

### 4.1 学术与开源路线

MICA 着重估计较稳定的身份脸形，DECA 同时处理形状、表情、姿态及细节，SMIRK 重点改善表情重建。它们解决的问题并不相同，不能把“更会做表情”当成“更会保留本人长相”。MICA 与 DECA 的公开许可含非商业限制；SMIRK 的代码为 MIT，但 FLAME、MICA 等依赖仍需独立核对。[^6][^7][^10]

Pixel3DMM 用图像空间的 UV 对应与法线先验辅助人脸模型拟合，是更接近“把照片特征落实为几何”的候选。其论文的消融实验显示身份先验对身份与表情分离有作用，同时承认两者仍不能完美分离。论文中的先验主要按单视图学习；仓库后来提供的多图与跟踪能力，也不等于先验已经从多个视角共同消除了歧义。官方代码为 CC BY-NC 4.0，适合作为研究对照。[^8][^9]

一个会影响长期路线的新变化是 FLAME 2023 Open。官方许可页将它列为 CC BY 4.0，允许包括商业目的在内的使用并要求归属等条件；较早版本仍有另一套限制。它提供了建立可控头部表示的机会，但不是完整重建系统，也不会自动改变使用旧版 FLAME 的项目、训练权重或纹理模型的许可。[^11]

2026 年阿里团队的《High-Fidelity Single-Image Head Modeling with Industry-Grade Topology》进一步强调几何与拓扑同时优化：从语义控制到关节和顶点逐级拟合，结合曲率、角度和关键轮廓约束。作者报告了 22 位技术美术对 30 个重建人脸的评价，但这不是消费者的实体相似度测试。公开论文值得借鉴；本报告未确认可直接集成的完整代码、权重与授权。[^16]

### 4.2 业界路线

Headshot 3 的照片流程已经提供光照平衡与表情处理、58 个可调面部特征预设、正侧面分开的轮廓调整以及修改后的纹理重投影。其网格流程还可以对已有扫描或雕刻头部做模板包裹。这与“生成后还可以改”的产品方向很一致。需要逐项分清真实几何、法线贴图和纹理修正；网页里的高频皮肤效果并不全是可打印细节。[^12][^13]

FaceBuilder 可使用一张或多张照片，通过交互点位拟合头部，并从多个视角投射纹理。官方说明允许商业工作并保留创作者对结果的权利；同时，它依赖付费的 KeenTools Core。它适合作为人工修正的质量和时间基准。采购现有工具用于制作，与把其能力作为自有应用的 SDK 或服务提供，是两个需要分别确认的事项。[^14]

Reallusion 当前内容 EULA 的标准许可对由其 Content 衍生的三维实体作品仅允许个人用途，并将销售 3D 打印物、角色生成 API、软件内生成等列入限制场景；协议另列企业授权途径。这些条款针对其 Content 及衍生内容，不能笼统外推到所有纯自有资产，但足以说明：使用 CC 默认网格或组件制作商业人像打印，不能仅凭“买了 Headshot”判断授权完整。[^15]

### 4.3 对整体生成路线的启示

可以研究把头部分为“身份核心”与“外围造型”：脸部和耳部由专用重建及可控编辑负责，发型、服饰、身体与姿态由通用生成或其他资产完成。这个方向有较强的模块化价值，但接缝、比例、颈部姿态、肤色、发际线和材质需要统一处理；把两个模型拼起来并不自动得到可打印成品。

因此，应先用相同人物、相同输入条件，把专用头部方案与当前通用生成做对照。只有几何和人工修正收益明确，才有理由进一步讨论接入与融合。不能用一个厂商精修示例对比一个未经修正的通用生成结果。

## 5. 方向二：阿莱光影、徕卡色彩如何用于 3D

### 5.1 可以借鉴的是完整影像处理思想

ARRI 与 HONOR 的官方合作公告确实提出，将 ARRI Image Science 引入消费级设备。ARRI 的 REVEAL 本身包含传感器数据处理、颜色变换、广色域及 LogC4 编码等环节，并非一个可以无条件套在任意图片上的滤镜。其 Look File 文档又区分创意变换与显示变换。[^17][^18][^19]

Leica 的官方 Looks 则提供具体的色调、对比度和饱和度风格，例如强调柔和自然肤色的 Contemporary，以及不同的低饱和或黑白风格。它说明可设计一套稳定的影像审美，但没有证明这种风格会改变几何辨识度或直接适配打印耗材。[^20]

对于 3D 人像，摄影风格至少涉及三个不同位置：输入照片的处理、数字模型的展示、实体材料与展示环境。相同名字出现在这三个位置，实际可控制的变量完全不同。

| 作用位置 | 可控制的变量 | 能获得的收益 | 实体迁移条件 |
| --- | --- | --- | --- |
| 输入照片 | 曝光、白平衡、明暗、局部修饰 | 参考更清晰，用户更容易选择喜欢的形象 | 应同时保留原图，避免把风格化阴影当结构 |
| 3D 数字预览 | 主光、补光、轮廓光、环境光、材质、显示变换 | 稳定呈现肤色与体积感，改善作品展示 | 预览必须有与实际耗材和常见照明接近的模式 |
| 实体打印 | 有限色分配、表面粗糙度、几何、后处理 | 改善真实的颜色边界与明暗层次 | 需要耗材样片、打印工艺与灯光条件验证 |
| 展示底座或灯具 | 灯的位置、面积、强度及光谱 | 让实体获得可重复的柔光和轮廓 | 会增加产品成本与使用条件；普通环境仍应可接受 |

最稳妥的技术借鉴是：为预览提供自然、柔和、轮廓清晰等可比较的影像方案；对皮肤颜色先做一致性处理；为实体建立材料校准和合适的展示光照。若采用现成 LUT，输入颜色空间和编码必须匹配。把 LogC4 变换直接用于普通 sRGB 纹理，不能视为完成 ARRI 工作流。[^18][^19]

现有手机合作是否覆盖其他产品、具体算法和命名，需要按实际合作范围确认。研究阶段可以先用中性名称描述效果，明确到底需要灯光设计、色彩风格还是材质模型。它们的技术收益可以分别验证，无需先依赖品牌化表达。

### 5.2 实体最需要的往往是先去光，再用光

皮肤照片中偏暗的区域，可能来自真实肤色，也可能来自鼻翼阴影、头发遮挡或曝光。若把这些阴影作为深色耗材固定下来，实体的新光照还会再产生一层阴影。在灯光反向时，原本“立体”的暗色块可能变成不合理的污渍。因此，打印颜色通常更需要接近皮肤本色的反照率，再由几何产生主要阴影。

这并不意味着完全禁止局部明暗修饰。眉毛、瞳孔、唇线和胡须等本来就是重要颜色特征；轻微的艺术性明暗也可能增加接受度。应把它们作为单独、可降低强度的美化层，避免把某个拍摄环境的大块阴影永久写进底色。

IC-Light 是公开的图像重打光路线，适合做风格探索和输入候选。其代码为 Apache 2.0，官方特别提示默认 BRIA 去背景模型有非商业限制，商业组合需要替换或另行解决。它能改图像中的光，并不意味着已经估计出了准确的皮肤反照率和打印几何。[^21]

## 6. 人脸去光与材质恢复：2024 至 2026 年进展

ID2Reflectance 将身份条件引入反射属性重建，试图从单张照片获得可渲染的面部反射资产。其价值是把“这个人的脸”和“这张照片的照明”区分开。公开仓库说明发布的是多域反射模型实现；是否具备完整资产输出、所需权重以及各依赖的产品使用条件，仍需逐项确认。[^22][^23]

CoRA 使用暗环境中手机闪光灯主导的拍摄序列，联合重建脸部几何与外观。这是“通过额外可控观测减少歧义”的实例，而非从任意单张自拍直接得到完整皮肤材质。仓库包含采集、预处理和重建步骤，可用于研究更高保真输入的收益，但其采集要求与普通照片上传的便利性不同。[^30]

2026 年的 OpenDelight 更直接面向自然环境人脸去光：学习去除照明的先验，再用它帮助多视图外观恢复。论文同时披露了非常相关的失败情况：可能把有色痣误当作照明而去掉；存在偏向较浅肤色的现象；模板配准与整头补全仍会带来相似度损失。作者报告完整视频到可重打光资产约 30 分钟，不能把这个时间理解成单张去光推理时间，也不能当作本地实测。[^24]

OpenDelight 的公开版本也与论文描述有变化：仓库改为提供 Ava256-Scan 相关训练路线，而预训练测试说明指出，当前提供的权重仍使用 FaceOLAT 与私有 Light Stage 扫描数据训练。代码为 GPL 3.0；公开代码、公开数据路线与预训练权重的具体可用范围需要分开判断。[^25][^26]

WildCap 处理自然环境中的面部反照率恢复，对硬阴影很有针对性。但当前公开的自定义数据流程包含 SwitchLight API、人工绘制阴影区域，以及基于 STFR 的几何准备；README 中 4K 超分和整头补全仍有 TODO。STFR 又依赖 COLMAP、2DGS、FLAME 相关拟合和 Wrap 命令行配准。它们提供了可研究的工程材料，不应被描述为已经可以无缝嵌入桌面产品的完整按钮。[^27][^28][^29]

| 候选 | 更适合验证的能力 | 本阶段定位 |
| --- | --- | --- |
| IC-Light | 参考图重打光、审美候选 | 输入与展示实验；检查是否改动五官 |
| ID2Reflectance | 身份条件反射属性估计 | 材质分离的研究参照 |
| CoRA | 可控手机拍摄带来的几何与反射收益 | 高保真采集路线对照 |
| OpenDelight | 自然照片去光、保留皮肤底色 | 优先研究其独立去光环节及身份特征保留 |
| WildCap 与 STFR | 手机视频到几何及可重打光资产 | 更完整、工程依赖也更重的研究路线 |

去光不是“自动把肤色变白”。2026 年另一项虚拟人肤色研究在多种渲染照明下评估了肤色传递，报告较深肤色存在更大的色度误差。对打印项目的启示是：样本与耗材不能只覆盖单一肤色；应将输入肤色、目标肤色和实物误差分开记录。这个启示属于跨场景推导，该论文并非打印实验。[^43]

## 7. 方向三：用真实凹凸与自然光影强化有限色人像

### 7.1 物理上成立，逆推却不是唯一解

同一种颜色的表面，在不同朝向、遮挡和光照下可以产生连续明暗。因此，“六种耗材”不等于“人眼只能看见六种明暗层次”。眼眶、鼻梁、鼻翼和嘴唇的实际形状，能够在光照下提供超出底色数量的视觉信息。

但照片的明暗并不唯一确定几何。经典的广义浅浮雕歧义表明，在一定反射、投影和未知照明假设下，不同的表面与照明组合可以产生同样图像。它解释了为什么单张正面照片看起来很立体，重建后的鼻子仍可能扁平或过高。解除歧义需要额外视角、已知光照、可靠人脸先验或人工确认。[^31]

Disney Research 的感知形状损失直接利用照片与几何着色图来评价脸部重建，说明“通过明暗判断几何是否像本人”已有研究基础。Shadow Art 则展示了根据目标投影优化实体几何的可能性。前者没有验证六色打印，后者的目标是投影图形；它们分别支持评价思路与逆向设计思路，不能合并成“任意光照下的人像更像”的证据。[^32][^33]

### 7.2 哪些细节值得转成真实几何

需要先分清法线、凹凸贴图和真实位移。法线或 bump 可以改变渲染时的表面朝向感，却不改变待打印表面；真正的 displacement 需要落实为导出网格中的顶点变化。Blender 官方文档也将两者明确区分。即便使用了真实位移，仍要确认导出与切片采用的是变形后的几何。[^38]

| 面部区域 | 可考虑的几何处理 | 颜色配合 | 主要风险 |
| --- | --- | --- | --- |
| 眼睑与眼眶 | 保留上眼睑、眼角与眼球周围的层次 | 将虹膜、瞳孔和眉毛作为独立重要区域 | 统一加深眼窝会改变本人气质；眼球、眼皮可能穿插 |
| 鼻梁、鼻翼、鼻孔 | 恢复轮廓和鼻翼转折，控制可制造的鼻孔开口 | 降低鼻侧固定暗块的依赖 | 夸大鼻梁会改变身份，细孔可能被切片或支撑填掉 |
| 嘴唇、嘴角、人中 | 保存唇形、嘴角走向及适度的唇间缝 | 唇色与口腔暗部避免混为一色块 | 深槽容易显脏，张嘴会增加牙齿与口腔处理负担 |
| 眉毛、发际线、胡须 | 在必要位置形成可打印的体块 | 用清晰色区承载主要辨识信息 | 全部做成细丝可能无法制造，做得过厚会显假 |
| 面颊与细纹 | 保留本人必要转折，按尺寸选择性弱化或加强 | 肤色层次服务整体体积感 | 全局磨平会丢个性，全部刻深又可能显老 |

这里的处理是候选设计，并无统一的毫米参数。打印尺寸、喷嘴、线宽、层高、摆放方向和表面处理都会改变可见性。例如，假设一个特征在原比例下深 1 mm，整体按五分之一缩小后只剩 0.2 mm。这个比例计算说明需要按最终尺寸评估，并不说明 0.2 mm 在所有方向、所有机器上都必然消失或保留。Prusa 官方也将最小特征与喷嘴及挤出线宽联系起来。[^37]

### 7.3 把“光影增强”写成可验证的优化问题

研究上可以固定一个已确认的基础脸形，同时调整局部位移、有限颜色分配和少量表面参数。在多个视角与多组常见灯光下渲染，评价本人辨识、审美、轮廓以及颜色误差，再增加几何变化幅度、局部平滑、最小结构和自交约束。这是本报告提出的联合优化框架，不是已有库的直接功能。

目标应同时考虑平均表现和较差条件下的表现。例如，正面柔光下更像，但侧面窗光下鼻翼成为夸张深沟，就不应仅因平均分提高而采用。应保留几何变化上限和被保护的身份锚点，避免优化过程通过不合理凹凸“骗过”某个固定视角的图像指标。

可提供两种独立评价条件：普通环境中的稳健实体，以及配套展示灯下的效果实体。后者允许针对明确灯位优化，但它的结果不能当作普通环境能力。色彩和几何能提高稳定性，无法让同一被动物体在所有照明下同时呈现任意指定影像。

## 8. 六色打印的颜色组织与制造限制

### 8.1 优先保存可辨识的信息

把纹理中的每个 RGB 点独立匹配到最近耗材颜色，容易让小而关键的眼睛、眉毛和唇部在大面积皮肤与服装面前失去权重。更合适的研究方向是先识别人脸的语义区域，再按本人特征和实际色盘分配颜色预算，同时限制碎片与过小色岛。这是本报告的产品推导。

六色配色没有普适答案。对于一个面部占比较大的半身像，可以测试肤色主色、肤色辅助色、深色五官与头发、眼白或亮部、唇部以及服饰色的分配；若衣服和头发需要更多独立颜色，就要重新权衡。应让“脸部细节优先”和“全身配色优先”成为可比较的选择，而不是承诺六色可以等价还原任意纹理。

几何、眼线与颜色区域还必须在编辑后保持对齐。鼻子变宽而原纹理不重投影，鼻孔颜色可能落到鼻翼外；网格局部重建后只按旧顶点编号上色，也可能移动色区。应分别保留高质量源纹理、语义区域以及最后用于切片的材料分配，避免把六色预览误当成已完成的打印交付。

### 8.2 半色调有研究基础，但不能照搬喷墨

Brunton 等人的全彩 3D 半色调研究利用多材料喷射与半透明材料，在物体表层及内部组织颜色。它证明材料光学和颜色空间组织非常重要，但实验硬件与普通 FDM 换色系统不同，不能据此承诺相同色域、分辨率或皮肤效果。[^34]

更接近 FDM 的研究是 Kuipers 等人的 Hatching for 3D Prints：利用黑白交替层及线条可见比例产生连续灰阶观感。论文实验基于双喷头系统，并明确讨论表面坡度、视角和感知色调校准。它支持进一步探索有限耗材的明暗表达；扩展到彩色面部、单喷嘴换料成本和完整人像质量仍是待验证问题。[^35]

HueForge 通过按层换料、材料透光距离和叠层厚度生成照片式浮雕效果，是有限耗材表达丰富影像的业界参照。但它以图像到分层高度模型为核心，不等于任意曲面上的全角度人像着色。其材料校准、试片和预览思想值得借鉴，不能把平面作品的效果直接外推到 360 度人头。[^36]

因此，近期颜色实验应先比较：清晰色区、少量空间混色、适度几何增强这三种策略。任何混色方案都要统计额外换料、耗时、废料和细小色块成形情况，并检查近看是否成为噪点。看起来像连续色，不代表材料真的连续混合。

## 9. 方向四：生成后的可控 3D 美颜

### 9.1 可利用的四类技术

**参数化形变。** 用基础脸形加一组受控形变表示结果，可以把鼻翼宽度、眼睑开合、嘴角等做成连续滑杆。Blender 的 shape keys 展示了这种机制：存储并混合已有顶点的位置变化。它适合可撤销、可比较的编辑，但前提是各形变之间具有一致的顶点对应。任意生成的三角网格无法直接共享另一张脸的形变参数。[^39]

**局部几何变形。** 对没有标准拓扑的网格，可以研究关键点、控制笼、拉普拉斯或 ARAP 变形。libigl 已提供 ARAP、网格平滑和相关几何算法。它们提供的是通用几何工具，不理解“美”或“像本人”；需要应用层定义编辑区域、固定边界、身份锚点及眼皮等特殊约束。[^40]

**学习型语义编辑。** Deep Deformable 3D Caricatures 已展示语义标签、点控制和自动夸张的 3D 编辑方式。它说明可把复杂形变组织成可操作空间，但训练域是漫画化头部，代码为 AGPL 3.0，训练数据等仍有各自条件。它更适合作为可控编辑方法的参照，不宜直接承担写实人像的通用“美颜模型”。[^41]

**纹理与材料编辑。** 在保持脸形的前提下，可以独立调整肤色、局部不均、眉眼边界、唇色、胡须和头发；调整几何后再进行纹理重投影或变形传递。Headshot 的流程已经证明这类编辑可以被组织成相对明确的工具。打印应用还需要把最后结果转换成有限材料区域。[^12][^13]

### 9.2 建议的控制项与保护规则

| 用户可理解的控制 | 背后的处理 | 必须同时观察的内容 |
| --- | --- | --- |
| 保留原貌 / 轻度美化 | 多个小幅参数的组合，保存零值基准 | 本人认可、正侧面差异；避免默认统一瘦脸或放大眼睛 |
| 脸部轮廓 | 受约束的面颊、下颌、额部形变 | 耳朵与脖子的连接；轮廓变化是否破坏身份 |
| 眼神与嘴角 | 眼皮、眼球周边与唇部的有限形变 | 眼球穿插、左右不协调、口腔与牙齿暴露 |
| 皮肤细节强度 | 保持特征的平滑与分尺度位移 | 痣、疤痕、皱纹等是否为本人要求保留的特点 |
| 五官清晰度 | 几何层次与颜色边界的协同调整 | 是否变成深沟、黑线或不可打印色岛 |
| 肤色与整体风格 | 反照率调整、有限色映射、预览显示风格 | 原始肤色保留、实际耗材偏色、风格是否只影响预览 |

这些名称是待讨论的产品表达，不代表需要一次做完全部控制项。早期可以只保留少量能明确解释、可限制幅度的调整。滑杆数量多并不等于容易获得满意结果。

### 9.3 让用户不满意时可以修正

应将原始重建作为独立基准保存，调整记录可撤销，多个候选可以并排比较。局部操作应明确作用区域，并允许锁定本人在意的五官或特征。自然的不对称可以保留，左右联动应是可选项。用户确认的是整个头部在多角度下的结果，而非只确认一张最佳效果图。

编辑数据至少要区分几何、纹理、材料区域与预览灯光。改变预览灯光不能偷偷改变实体颜色；改变鼻形应同步检查纹理落点；缩小打印尺寸后，应重新评估细节和色块。导入切片时还要检查实际几何、颜色区域和打印比例的交接。

对当前通用生成网格，建议先评估“可靠定位五官和局部区域 + 少量受控变形”的难度，再判断是否需要统一拓扑。配准到标准头模有利于复用滑杆，但会引入拟合误差，尤其是眼皮、鼻翼、嘴唇、耳部和颈部。把所有模型重新拟合成一个平均模板，可能损失原本已生成正确的个体细节。

## 10. 可用组件与许可、完整性对照

下表是技术选型核查，不替代具体版本与具体使用方式的授权确认。“开源”不等于所有权重、数据和输出用途都可用；GPL 或 AGPL 也不等于禁止商业使用，而是需要遵守相应许可义务。研究原型能运行、生产环境可复用、能够出售打印实体，应分别判断。

### 10.1 输入与脸部重建

| 候选 | 公开状态与主要条件 | 选型定位 |
| --- | --- | --- |
| InstantID | Apache 代码；检查点和 InsightFace 相关权重有研究用途限制 [^2][^5] | 身份条件输入的论文与效果参照 |
| PhotoMaker | Apache 2.0，注明第三方例外；具体版本及依赖单独核对 [^3] | 2D 美化候选生成 |
| PuLID | Apache 代码；基础模型与人脸依赖另有条件 [^4][^5] | 2D 身份与可编辑性权衡 |
| MICA / DECA | 官方公开许可含非商业限制；需要脸部模型资产 [^6][^7] | 身份几何与表情细节的研究参照 |
| Pixel3DMM | CC BY-NC 4.0；依赖 FLAME、MICA 等 [^9] | 几何重建质量与误差类型对照 |
| SMIRK | MIT 代码；基础模型及其他依赖不能随之推定开放 [^10] | 表情重建候选 |
| FLAME 2023 Open | CC BY 4.0，需归属等；旧版许可不同 [^11] | 可控头部表示的候选基础 |
| Headshot 3 | 商业软件；CC 内容的打印销售、生成服务等涉及额外授权 [^12][^15] | 人工质量对照或洽谈合作对象 |
| FaceBuilder | 商业工具，闭源 Core；官方允许商业创作，集成方式需另核对 [^14] | 多图拟合与人工修正基准 |

### 10.2 去光、编辑与制造

| 候选 | 公开状态与主要条件 | 选型定位 |
| --- | --- | --- |
| IC-Light | Apache 代码；默认去背景依赖有额外限制 [^21] | 风格与照明候选实验 |
| OpenDelight | GPL 3.0；提供预训练推理，权重训练来源和数据路线有区别 [^25][^26] | 单独去光环节优先研究 |
| WildCap | GPL 3.0；公开流程含商业 API、人工步骤和未完成步骤 [^27][^28] | 完整材质恢复的研究对照 |
| STFR | GPL 3.0；涉及 2DGS、FLAME 与 Wrap 等独立依赖 [^29] | 视频几何重建与统一拓扑实验 |
| libigl | 核心为 MPL 2.0，选用的模块与依赖仍需核对 [^40] | 确定性局部几何编辑 |
| DD3C | AGPL 3.0；漫画域与数据条件另审 [^41] | 语义形变、点控制与夸张的参照 |
| LAM | Apache 代码，主要是 Gaussian 头像表示 [^42] | 数字呈现参照；需另证实体网格能力 |
| HueForge | 商业应用，按具体使用许可；基于层高与材料透光组织影像 [^36] | 耗材校准、试片与浮雕对照 |

在决定复用前，还应固定候选代码、权重、模板、输入与输出版本，记录是否需要网络服务及每次处理成本。表中没有“整套可直接商用”的泛化结论。尤其不能因为换成 FLAME Open，就认为旧模型训练权重、纹理资产或原有接口已自动完成迁移。

## 11. 后续可行性讨论所需的证据

### 11.1 小规模、分阶段的对照设计

建议先用 10 至 20 位获得照片使用授权的参与者做探索性试验，覆盖不同肤色、年龄外观、发型、胡须、眼镜和输入质量。这个规模用于发现问题和估计收益，不足以支撑面向所有用户的稳定性承诺。记录每位参与者最在意的特征，以及是否愿意接受主动美化。

第一组实验只改变 2D 输入，固定 3D 生成版本、参数、候选次数和筛选规则。对比原图、仅整理拍摄条件的图、轻度美化图，以及增强个人特征的图。对随机生成，应有相同次数的重复，并把“首个结果”和“相同预算下选出的最好结果”分别记录。

第二组实验只改变后处理，基于同一个初始 3D 结果比较：不处理、只改颜色、只做小幅几何美化，以及两者联合。这样才能看清到底是几何变好了，还是颜色暂时掩盖了脸型问题。先在统一灯光、材质与相机下检查，再加入不同展示条件。

第三组再比较重建路线：通用生成、专用脸部重建、可交互修正的商业工具，以及手机视频路线。输入数量不同本身也是变量，应分别做同输入条件的算法比较和增加真实输入的收益比较，不能把多角度采集的收益全部归功于新算法。

### 11.2 评价内容

| 评价维度 | 建议方法 | 需要避免的误判 |
| --- | --- | --- |
| 像本人 | 本人、熟悉本人的评价者，以及看参考照片的其他评价者分别做随机顺序配对比较 | 不把人脸识别分数当最终答案；不只用一个正面视角 |
| 本人喜欢 | 记录原貌认可、美化偏好、是否愿意接受成品 | 不把“普遍漂亮”替代“这个人认可” |
| 几何正确 | 去纹理的中性材质、正侧轮廓、眼鼻口局部；有扫描真值时再测几何误差 | 不把纹理清晰度或总面数当几何正确度 |
| 光照稳健 | 至少检查正面、左右斜侧及左右侧面，配柔光、侧光和顶光 | 不只报告最有利灯光和角度 |
| 颜色与材料 | 实际耗材色样、固定光照或校准测色，分别看皮肤与五官 | 不在未校准照片之间直接解释微小色差 |
| 可编辑性 | 完成指定局部调整、撤销恢复、切换候选、重新导出 | 不把算法能形变等同于普通用户能修好 |
| 制造与成本 | 切片后关键结构、换料次数、废料、支撑损伤、失败率和实物耗时 | 不把渲染图当打印通过；不只报告成功样本 |

报告结果时，应以参与者为主要统计单位，保留平局与失败，分别展示相似度和美感的变化。多视角图片或多名评价者对同一个人的评分存在相关性，不能当成大量独立样本放大结论。若使用置信区间，应按参与者进行适当聚合或重采样。

### 11.3 从小试片到完整实体

实体阶段可以先打印同一比例的脸部或半头试片，比较眼睑、鼻翼、唇线、两到三种几何强度及不同色区策略。试片用于淘汰明显无效的处理；最终仍需完整头部或半身像来验证侧面、耳朵、发型、支撑与观看效果。

对每个进入完整打印的方案，应保留源模型、实际打印比例、材料与切片配置、预览和实物的同视角照片。必须在真实切片程序里检查实际导入结果与路径。若设计依赖配套灯具，应同时记录配套灯下和普通室内光下的结果。

在上述数据出现之前，不应给出“相似度提高多少”“用户一定接受”“六色已达到照片级”等数字结论。本报告没有执行新的付费生成、模型处理实验或实体打印，也未把论文数字作为本产品实测。

## 12. 研究优先级与路线选择

### 12.1 优先获得的三类结论

第一，判断输入整理与轻度美化是否已经能提升本人接受度。这能用相对独立的对照回答，也能明确“美颜”应改变哪些信息。若美化图更好看但 3D 相似度下降，应保留身份参考，并调整美化位置或幅度。

第二，判断专用脸部几何和少量生成后控制，能否显著减少人工修正难度。这里最需要的是中性材质、多角度以及修改前后的证据。若没有正确的眼鼻口位置和深度，更复杂的颜色风格通常只能改善表现，不能解决结构问题。

第三，判断实际尺寸与六色约束下，哪些面部层次需要几何，哪些更适合颜色。先建立耗材与小试片证据，再研究更复杂的半色调和灯光联合优化。漂亮的高分辨率皮肤贴图，只有在其关键信息被工艺保留时，才对实体有价值。

### 12.2 可比较的产品路线

| 路线 | 主要价值 | 需要先回答的问题 |
| --- | --- | --- |
| 当前生成加受控 2D 美化 | 复用现有流程，较快检验审美接受度 | 输入美化是否真的传到 3D；是否损失身份 |
| 专用头部加通用身体与造型 | 独立提高脸部质量并支持局部控制 | 头身融合、纹理对齐、模板许可和配准损失 |
| 多照片或短视频的高保真模式 | 用新增观测改善真实结构与材质恢复 | 采集完成率、处理时间、依赖与失败恢复 |
| 人工辅助的精修模式 | 为高要求订单提供质量基准 | 修正时间、操作技能、单位成本及商业打印授权 |

四个最初方向可以在同一产品中互补，但目前证据不支持把它们简化成一个“人像美化滤镜”。比较有根据的长期能力是：保留身份依据、表达本人偏好、可控修改真实几何、校准有限材料，再用实际打印结果反馈。后续可行性讨论应围绕这些能力分别取证，再决定组合与投入。

## Sources

以下为直接使用的论文、作者仓库、厂商技术说明与许可页面。动态页面与仓库访问日期统一为 2026-09-10。论文年份与其网页抓取时间分别看待；论文中的最佳指标、厂商展示和本报告提出的实验条件并未视为等价证据。

1. Rhodes, G., Byatt, G., Tremewan, T., Kennedy, A. [Facial distinctiveness and the power of caricatures](https://pubmed.ncbi.nlm.nih.gov/9274754/). Perception, 1997. 用于区分线描与照片夸张的识别证据。
2. InstantX Research. [InstantID: Zero-shot Identity-Preserving Generation in Seconds](https://github.com/instantX-research/InstantID). 2024 年论文的官方仓库，参见 License 与模型说明。
3. Tencent ARC. [PhotoMaker: Customizing Realistic Human Photos via Stacked ID Embedding](https://github.com/TencentARC/PhotoMaker). 2024，含后续 V2；另见 [PhotoMaker 许可](https://github.com/TencentARC/PhotoMaker/blob/main/LICENSE)。
4. Guo, Z., Wu, Y., et al. [PuLID: Pure and Lightning ID Customization via Contrastive Alignment](https://github.com/ToTheBeginning/PuLID). NeurIPS 2024 官方仓库；另见 [PuLID 许可](https://github.com/ToTheBeginning/PuLID/blob/main/LICENSE)。
5. InsightFace. [InsightFace 官方仓库及 Licensing 说明](https://github.com/deepinsight/insightface). 包含代码、训练数据和预训练模型的不同许可说明。
6. Zielonka, W., Bolkart, T., Thies, J. [Towards Metrical Reconstruction of Human Faces](https://arxiv.org/abs/2204.06607). ECCV 2022；[MICA 官方仓库](https://github.com/Zielon/MICA)，[MICA 许可](https://github.com/Zielon/MICA/blob/master/LICENSE)。
7. Feng, Y., Feng, H., Black, M. J., Bolkart, T. [DECA: Learning an Animatable Detailed 3D Face Model from In-the-Wild Images](https://github.com/yfeng95/DECA). SIGGRAPH 2021 官方仓库，含 Non-commercial Scientific Research Purposes 说明。
8. Giebenhain, S., Kirschstein, T., Rünz, M., Agapito, L., Nießner, M. [Pixel3DMM: Versatile Screen-Space Priors for Single-Image 3D Face Reconstruction](https://arxiv.org/html/2505.00615). 2025，重点参见消融与 Limitations and Future Work。
9. Giebenhain, S., et al. [Pixel3DMM 官方代码与许可](https://github.com/SimonGiebenhain/pixel3dmm). 包含 FLAME、MICA 依赖、多图及跟踪说明。
10. Retsinas, G., et al. [SMIRK: 3D Facial Expressions through Analysis-by-Neural-Synthesis](https://github.com/georgeretsi/smirk). CVPR 2024；[SMIRK MIT 许可](https://github.com/georgeretsi/smirk/blob/main/LICENSE)。
11. Max Planck / FLAME 项目. [FLAME Model License](https://flame.is.tue.mpg.de/modellicense.html). 包含 FLAME 2023 Open 的 CC BY 4.0 与其他版本条款；另见 [FLAME 项目更新](https://flame.is.tue.mpg.de/)。
12. Reallusion. [Photo to 3D Head: Headshot 3 Image Workflow](https://www.reallusion.com/character-creator/headshot/photo-to-3d-head.html). 面部特征、轮廓、照片修正、去光与纹理重投影说明。
13. Reallusion. [3D Head to Rigged Character: Headshot 3 Mesh Workflow](https://www.reallusion.com/character-creator/headshot/3d-head-from-mesh.html). 网格包裹、细分、纹理烘焙及遮罩说明。
14. KeenTools. [FaceBuilder for Blender](https://keentools.io/products/facebuilder-for-blender). 照片拟合、商业创作、Core Library 和产品 FAQ。
15. Reallusion. [Content End User License Agreement](https://www.reallusion.com/Content/EULA/EULA.htm). 更新于 2025-08-01，重点为 2.1 Standard License 和 2.2 Enterprise License。
16. Wang, Y., Bi, Z., Cai, B., et al. [High-Fidelity Single-Image Head Modeling with Industry-Grade Topology](https://arxiv.org/html/2605.04524v1). Alibaba Group，预印本，2026-05-06。
17. ARRI. [HONOR and ARRI announce strategic technical collaboration](https://www.arri.com/en/company/press/press-releases-2026/honor-and-arri-announce-strategic-technical-collaboration). 官方公告，2026-03-01。
18. ARRI. [REVEAL Color Science](https://www.arri.com/en/learn-help/learn-help-camera-system/image-science/reveal-color-science). 技术说明，包含 ADA-7、ACE4、AWG4、LogC4 与显示 LUT。
19. ARRI. [Look Files](https://www.arri.com/en/learn-help/learn-help-camera-system/image-science/look-files). 技术说明，包含创意颜色变换和显示变换的组织方式。
20. Leica Camera. [Leica Looks](https://leica-camera.com/en-US/photography/leica-looks). 官方风格说明，包含 Contemporary、Classic、Natural 等。
21. Zhang, L., Rao, A., Agrawala, M. [IC-Light 官方实现](https://github.com/lllyasviel/IC-Light). 相关论文为 ICLR 2025；仓库包含模型差异与 BRIA 背景移除依赖的许可提示。
22. Ren, X., Deng, J., et al. [Monocular Identity-Conditioned Facial Reflectance Reconstruction](https://arxiv.org/abs/2404.00301). CVPR 2024，pp. 885-895。
23. Ren, X., et al. [ID2Reflectance 官方实现](https://github.com/xingyuren/id2reflectance). 公开实现范围、依赖与运行说明。
24. Han, Y., Ming, X., Li, T., et al. [Learning a Delighting Prior for Facial Appearance Capture in the Wild](https://arxiv.org/html/2605.05636). SIGGRAPH 2026 / ACM TOG，DOI 10.1145/3811303；特别参见 7.4 的身份、肤色与效率讨论。
25. Han, Y., et al. [OpenDelight 官方仓库](https://github.com/yxuhan/OpenDelight). 公开版本与论文版本差异；[OpenDelight GPL 3.0 许可](https://github.com/yxuhan/OpenDelight/blob/main/LICENSE)。
26. Han, Y., et al. [OpenDelight: TEST](https://github.com/yxuhan/OpenDelight/blob/main/doc/TEST.md). 预训练权重来源与推理步骤。
27. Han, Y., et al. [WildCap 官方仓库](https://github.com/yxuhan/WildCap). CVPR 2026，包含工程改动、TODO 与 GPL 3.0 许可；[论文](https://arxiv.org/html/2512.11237)。
28. Han, Y., et al. [WildCap: Processing Custom Data](https://github.com/yxuhan/WildCap/blob/main/PROCESS.md). STFR、SwitchLight API、人工阴影掩膜与初始化的具体步骤。
29. Han, Y. [STFR: Stable and Topologically-Consistent 3D Face Reconstruction from a Smartphone Video](https://github.com/yxuhan/STFR). 几何、配准、纹理重建及 Wrap 命令行依赖说明。
30. Han, Y., Lyu, J., Xu, F. [CoRA: High-Quality Facial Geometry and Appearance Capture at Home](https://github.com/yxuhan/CoRA). CVPR 2024，仓库另列 IJCV 2026 扩展；[CVPR 论文](https://openaccess.thecvf.com/content/CVPR2024/papers/Han_High-Quality_Facial_Geometry_and_Appearance_Capture_at_Home_CVPR_2024_paper.pdf)。
31. Belhumeur, P. N., Kriegman, D. J., Yuille, A. L. [The Bas-Relief Ambiguity](https://www.cs.columbia.edu/~belhumeur/journal/basrelief-ijcv.pdf). IJCV 35(1), 33-44, 1999。
32. Otto, C., Chandran, P., Zoss, G., et al. [A Perceptual Shape Loss for Monocular 3D Face Reconstruction](https://studios.disneyresearch.com/2023/10/09/a-perceptual-shape-loss-for-monocular-3d-face-reconstruction/). Disney Research Studios，Pacific Graphics，2023。
33. Mitra, N. J., Pauly, M. [Shadow Art](https://www.graphics.stanford.edu/~niloy/research/shadowArt/shadowArt_sigA_09.html). SIGGRAPH Asia 2009，作者项目页与论文。
34. Brunton, A., Arikan, C. A., Urban, P. [Pushing the Limits of 3D Color Printing: Error Diffusion with Translucent Materials](https://arxiv.org/abs/1506.02400). 2015 年预印本；研究对象为多材料喷射打印。
35. Kuipers, T., Elkhuizen, W., Verlinden, J., Doubrovski, E. [Hatching for 3D Prints: Line-Based Halftoning for Dual Extrusion Fused Deposition Modeling](https://arxiv.org/html/1805.01375). 2018，包含表面坡度、视角与色调校准讨论。
36. HueForge. [What Is HueForge?](https://shop.thehueforge.com/blogs/news/what-is-hueforge). 官方原理介绍，2023；另见 [HueForge FAQ](https://shop.thehueforge.com/pages/hueforge-faq-1)。
37. Prusa Research. [Modeling with 3D Printing in Mind](https://help.prusa3d.com/article/modeling-with-3d-printing-in-mind_164135). 官方知识库，重点为 Thin Walls and Minimum Feature Size。
38. Blender Foundation. [Displacement](https://docs.blender.org/manual/en/latest/render/materials/components/displacement.html). 官方手册，区分 Bump、Displacement 与两者组合。
39. Blender Foundation. [Shape Keys: Introduction](https://docs.blender.org/manual/en/4.5/animation/shape_keys/introduction.html). Blender 4.5 LTS 官方手册，包含工作方式、顶点对应与组合形变说明。
40. libigl. [libigl Tutorial](https://libigl.github.io/tutorial/). 官方几何处理教程，含 ARAP 与平滑；[MPL 2.0 许可](https://github.com/libigl/libigl/blob/main/LICENSE.MPL2)。
41. Jung, Y., Jang, W., Kim, S., Yang, J., Tong, X., Lee, S. [Deep Deformable 3D Caricatures with Learned Shape Control](https://github.com/ycjungSubhuman/DeepDeformable3DCaricatures). SIGGRAPH 2022 官方实现，DOI 10.1145/3528233.3530748，AGPL 3.0。
42. He, Y., Gu, X., Ye, X., et al. [LAM: Large Avatar Model for One-shot Animatable Gaussian Head](https://github.com/aigc3d/LAM). SIGGRAPH 2025 官方仓库，Apache 2.0。
43. Schneider, G. F., Menezes, E., Mecenas, R., et al. [True to Tone? Quantifying Skin Tone Fidelity and Bias in Photographic-to-Virtual Human Pipelines](https://arxiv.org/abs/2604.02055). 预印本，2026-04-02。

[^1]: Rhodes 等，照片与线描夸张研究，1997。[原文](https://pubmed.ncbi.nlm.nih.gov/9274754/)
[^2]: InstantX，InstantID 官方仓库。[原文](https://github.com/instantX-research/InstantID)
[^3]: Tencent ARC，PhotoMaker 及许可。[原文](https://github.com/TencentARC/PhotoMaker)
[^4]: Guo、Wu 等，PuLID，2024。[原文](https://github.com/ToTheBeginning/PuLID)
[^5]: InsightFace，Licensing 说明。[原文](https://github.com/deepinsight/insightface)
[^6]: Zielonka 等，MICA，2022。[原文](https://github.com/Zielon/MICA)
[^7]: Feng 等，DECA，2021。[原文](https://github.com/yfeng95/DECA)
[^8]: Giebenhain 等，Pixel3DMM 论文，2025。[原文](https://arxiv.org/html/2505.00615)
[^9]: Pixel3DMM 官方仓库及许可。[原文](https://github.com/SimonGiebenhain/pixel3dmm)
[^10]: Retsinas 等，SMIRK，2024。[原文](https://github.com/georgeretsi/smirk)
[^11]: FLAME，模型许可与版本区别。[原文](https://flame.is.tue.mpg.de/modellicense.html)
[^12]: Reallusion，Headshot 3 照片流程。[原文](https://www.reallusion.com/character-creator/headshot/photo-to-3d-head.html)
[^13]: Reallusion，Headshot 3 网格流程。[原文](https://www.reallusion.com/character-creator/headshot/3d-head-from-mesh.html)
[^14]: KeenTools，FaceBuilder 官方说明。[原文](https://keentools.io/products/facebuilder-for-blender)
[^15]: Reallusion，内容 EULA，2025-08-01。[原文](https://www.reallusion.com/Content/EULA/EULA.htm)
[^16]: Wang 等，工业拓扑头部重建，2026。[原文](https://arxiv.org/html/2605.04524v1)
[^17]: ARRI，HONOR 合作公告，2026。[原文](https://www.arri.com/en/company/press/press-releases-2026/honor-and-arri-announce-strategic-technical-collaboration)
[^18]: ARRI，REVEAL Color Science。[原文](https://www.arri.com/en/learn-help/learn-help-camera-system/image-science/reveal-color-science)
[^19]: ARRI，Look Files 技术说明。[原文](https://www.arri.com/en/learn-help/learn-help-camera-system/image-science/look-files)
[^20]: Leica Camera，Leica Looks。[原文](https://leica-camera.com/en-US/photography/leica-looks)
[^21]: Zhang 等，IC-Light 官方实现。[原文](https://github.com/lllyasviel/IC-Light)
[^22]: Ren 等，ID2Reflectance 论文，2024。[原文](https://arxiv.org/abs/2404.00301)
[^23]: ID2Reflectance 官方实现与范围。[原文](https://github.com/xingyuren/id2reflectance)
[^24]: Han 等，OpenDelight 论文，2026。[原文](https://arxiv.org/html/2605.05636)
[^25]: OpenDelight，公开版本说明与许可。[原文](https://github.com/yxuhan/OpenDelight)
[^26]: OpenDelight，预训练测试说明。[原文](https://github.com/yxuhan/OpenDelight/blob/main/doc/TEST.md)
[^27]: WildCap，官方代码与 TODO。[原文](https://github.com/yxuhan/WildCap)
[^28]: WildCap，自定义数据处理步骤。[原文](https://github.com/yxuhan/WildCap/blob/main/PROCESS.md)
[^29]: Han，STFR 官方重建流程。[原文](https://github.com/yxuhan/STFR)
[^30]: Han 等，CoRA，2024 / 2026。[原文](https://github.com/yxuhan/CoRA)
[^31]: Belhumeur 等，浅浮雕歧义，1999。[原文](https://www.cs.columbia.edu/~belhumeur/journal/basrelief-ijcv.pdf)
[^32]: Otto 等，感知形状损失，2023。[原文](https://studios.disneyresearch.com/2023/10/09/a-perceptual-shape-loss-for-monocular-3d-face-reconstruction/)
[^33]: Mitra、Pauly，Shadow Art，2009。[原文](https://www.graphics.stanford.edu/~niloy/research/shadowArt/shadowArt_sigA_09.html)
[^34]: Brunton 等，3D 颜色半色调，2015。[原文](https://arxiv.org/abs/1506.02400)
[^35]: Kuipers 等，FDM 线式半色调，2018。[原文](https://arxiv.org/html/1805.01375)
[^36]: HueForge，官方原理介绍。[原文](https://shop.thehueforge.com/blogs/news/what-is-hueforge)
[^37]: Prusa，面向打印的模型设计。[原文](https://help.prusa3d.com/article/modeling-with-3d-printing-in-mind_164135)
[^38]: Blender，Displacement 官方手册。[原文](https://docs.blender.org/manual/en/latest/render/materials/components/displacement.html)
[^39]: Blender，Shape Keys 官方手册。[原文](https://docs.blender.org/manual/en/4.5/animation/shape_keys/introduction.html)
[^40]: libigl，几何处理教程及许可。[原文](https://libigl.github.io/tutorial/)
[^41]: Jung 等，DD3C 官方实现，2022。[原文](https://github.com/ycjungSubhuman/DeepDeformable3DCaricatures)
[^42]: He 等，LAM，2025。[原文](https://github.com/aigc3d/LAM)
[^43]: Schneider 等，肤色传递研究，2026。[原文](https://arxiv.org/abs/2604.02055)
