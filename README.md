# P5D-PC — P5D（PCSH10109）PC 原生重编译移植

把 PS Vita 版《Persona 5: Dancing in Starlight》（P5D，PCSH10109）的**原版游戏二进制原样加载**，
在 PC（后续可扩展到手机）上由我们实现的本地兼容层（HLE）驱动运行。
CPU 采用 **AOT 静态重编译**（原版 ARM/Thumb 代码一次性翻译为 x86-64 原生执行）+ 解释器 fallback；
系统 API（文件/存档、内存、线程、GXM→Vulkan、音频、输入、视频解码）全部由 HLE 接管；
最终由**原版游戏代码自己绘制**画面，目标串联 `S0_LOGO → S1_TITLE → S2_MENU → S3_SELECT → S4_GAME → RESULT` 全流程。

> 本仓库**不包含任何游戏文件、游戏密钥、固件或由游戏数据翻译出的二进制产物**。
> 要运行本项目，玩家必须使用**自己合法持有的正版游戏**从自己的游戏机提取数据（详见 [docs/RELEASE_COMPLIANCE.md](docs/RELEASE_COMPLIANCE.md)）。

---

## 当前状态（2026-11-02，里程碑 **M1123**；系统层约 89%，端到端目标约 60–70%）

**一句话**：原版 eboot 已在 PC 上原生（AOT）跑起来，S0_LOGO 完整通过；
**CRI FS 的 CPK 挂载/文件索引已完全打通**——`data.cpk`（10,555 文件）+ `vita.cpk`（531 文件）
双双挂载成功，标题素材名全部枚举并选中 `title/title_p5.bin`；当前卡点推进到**标题文件异步加载的轮询等待**。

| 模块 | 完成度 | 当前状态（M1115 实测） |
|---|---|---|
| ARM/Thumb → x86-64 **AOT 静态重编译** | ~95% | **230,252 个基本块 / 12,921 个函数 / 36 个宿主批次**；`[NOFN]=0`、`[INDIRECT-MISS]=0`、`[BADMEM]=0`；本阶段修复两个**通用翻译器 bug**：① 全局 `LDRD rX,rY,[rX,#imm]`（34 处）② 此前的 `[pc,#imm]` 字面量加载（485 处） |
| 原版 eboot/SELF **原样加载** | ~95% | ELF 段映射、导入表/NID 解析、模块启动全部可用；`module_start=0x81198464`、`IMG_VA_BASE=0x80600000`；完整 boot 链：S0_LOGO → 启动 USM 影片（guest 软件解码）→ 应用世界恢复 |
| **HLE 系统 API 接管** | ~80% | 内存/线程/fiber/信号量/事件标志/消息/文件/GXM/音频(NGS)/CRI 日志；本阶段新增：**SceNgs HLE**（`P5D_NGS=1`，修复 39MB 假尺寸）、**HLE I/O 泵**（`P5D_IOPUMP=1`，打通 criFs 读完成）、**HDRFIX**（64 位 CPK 头标准化）、动态 reader 泵、CRILOG 调用点增强 |
| **CRI FS / CPK 虚拟文件系统** | ~95% | **CPK 挂载完全打通**：`[FIOS] mounted data.cpk files=10555` / `mounted vita.cpk files=531`，`[CRILOG]=0`；标题素材名全部枚举（`title/title_p5.bin`、`title.spr4`、`title.p3/logo.tga` …）并选中 `title/title_p5.bin`；三项 HLE 变换：TOCSZPATCH（64 位变体 TocSize +0x18）+ TOCPAD（EOF 零填充）+ TOCFIX（合成 TOC 段头、UTF 后移 0x10） |
| **GXM → Vulkan** | ~60% | 真实 SPIR-V 管线（GXP→SPIR-V manifest 4331 条）创建成功，BeginScene→Draw→RT→swapchain→`vkQueuePresentKHR result=0` 全链执行；**画面内容仍为黑**（纹理 dummy / uniform 零值 / 顶点数据未校验，M940–945 遗留问题） |
| 音频（SceNgs / CRI Atom） | ~15% | SceNgs HLE 已能给出正确 work size（修复 E2009011903）；ACF/ACB 加载与真实解码/输出仍缺 |
| 输入（SceCtrl / SceTouch） | ~10% | Pad/Touch HLE 骨架可用、可注入按键；游戏侧输入流未接通 |
| 视频解码 | ~15% | 启动 USM 影片由 guest 软件解码跑通；真实 AVC/CRI Mana 解码未完成 |
| 场景链 S0→S4→RESULT | S0 ✅ / S1 进行中 | S0_LOGO 完整通过；S1_TITLE 资源加载（CPK 挂载→文件索引→title 素材）进行中；S2–S4 未到达 |
| 单 exe 打包 / 移动端 | ~40% / 0% | 目前是单一 Windows 可执行 `recomp_harness.exe`（AOT+HLE 开发态）；正式打包/移动端未开始 |

### 距离目标还有多远（诚实评估）

| 关口 | 状态 | 剩余工作 | 粗略轮数 |
|---|---|---|---|
| CPK 挂载 + 文件索引 | **80%** | UTF 字段读取修正 → TOC 解析 → data.cpk 同样处理 | 5–10 |
| criFs 流式读取 / 资源加载 | 70% | 文件索引建立后验证 title 素材真实读取 | ~10 |
| GXM 真实画面（纹理/uniform/几何/ShaderPatcher） | 60% | **黑屏修复**、纹理上传、uniform 数据、顶点格式校验、真实管线选择 | 30–60 |
| S1 标题画面（对象/动画/音效触发） | ~40% | 依赖上面两项 + 标题状态机 | 20–40 |
| S2 MENU / S3 SELECT / S4 GAME / RESULT | 10–20% | 每个场景 20–50 轮（复用引擎能力） | 80–150 |
| 音频（SceNgs/ACB 解码输出） | ~15% | ACF/ACB 加载、NGS 连接、真实输出 | 20–40 |
| 输入 / 视频真解码 | 10–15% | Pad/Touch 接通；USM/AVC 硬解或软件解码 | 10–30 |
| 单 exe 打包 / 移动端 | 40% / 0% | 发布形态、缓存、跨平台 | 10–20 |

综合判断：**"第一帧真实标题画面"约还需 50–90 轮**（按当前每轮 1–2 个里程碑的节奏）；
**完整 S0→RESULT 全流程**约还需 **200–400 轮**（主要不确定性在 GXM 渲染正确性与音频/输入接通）。
当前进度 1079/2500 轮，时间预算充足；最大风险仍是 GXM 画面内容与真实视频/音频解码。

### 最近关键里程碑（M1093–M1115，2026-11-02）

- **M1121（18.4666）**：卡点根因链闭合——游戏不发任何文件请求（[RQMGR]/[APOPEN]/[FHOPEN]=0），ACF 未注册导致 DSP Bus Setting 失败（`DAT_8153FB30+0x44==0`，DSP 名 `BusSet_xrd722v2`）
- **M1122（18.4667）**：新增 **IOPUMP_WAIT**（阻塞 DelayThread 内泵 reader，解决嵌套 fiber spin 冻结 g_blkcnt_total）；确认声音初始化真实执行（SNDINIT→ACFREG→DSPSET），ACF 读取不走主机 IO
- **M1123（18.4668）**：★★★ **ACF 注册成功（`[CRILOG]=0`）**：FIOSFILL **ACF_FILL** 从 vita.cpk 直服 `sound/common.acf`（5152B）；关键坑=第 5–8 参数是 out-param **指针**必须写穿；声音线程 `thSeqSndAdx2` 启动（pc=81242D24），boot 链越过 init sound；新卡点=HLE-BADMEM va=0x28 @ blk 81198BEE（sceFiberSwitch 路径）

- **M1119（18.4664）**：挂载后卡点初判为 CRI AtomEx（音频）初始化重试；新增 IOPUMP pstate 7\|8 完成 shim（新 reader 848E9678 完成）
- **M1120（18.4665）**：卡点再精化——5000us 轮询其实是 **Fios 工作线程空转**（`FUN_811F22B8`，pending 任务链表 `DAT_81539754+0xC` 为空）；AtomEx 初始化只调用一次、无重试（`[ATOMEXINIT] cfg=848561F8`、`[FINSPOOL] param1=36`）；下一目标=找出 title 加载任务应在何处入队

- **M1116（18.4661）**：UTF 字段误读根因——第 0 列名指针为空导致 `strcmp(null,key)==0`、所有字段读成 0x800；HLE strcmp 空指针语义修正（空指针=隐含 `"ContentOffset"`）→ TocOffset/TocSize 正确解析
- **M1117（18.4662）**：TOC 解析失败根因——vita.cpk TOC 是裸 `@UTF`，被当成 `[0x10 段头][UTF]` → `+4=0x00` 误触发 XOR（滚动密钥 0x5F、`*=0x15`）把 UTF 数据就地破坏
- **M1118（18.4663）**：★★★ **CPK 挂载完全打通**：TOCSZPATCH + TOCPAD + TOCFIX 三变换 → 分析器状态 3→4→0x10→0x11→0x14→0x16，`[FIOS] mounted data.cpk files=10555`、`mounted vita.cpk files=531`，标题素材枚举、选中 `title/title_p5.bin`；新卡点=标题文件异步加载轮询（`8125DDAE~8125DDF6`、等待参数 0x1388）

- **M1093–M1098**：`P5D_CRIFSHLE` 之外的 **游戏自身 criFs 初始化链**跑通；binder 初始化、CRI FS 保持、file I/O 走真实 HLE
- **M1104（18.4649）**：**`P5D_NGS=1` 成为必需项**——SceNgs HLE 缺失导致 NGS work size 返回 -1/39MB，触发 `E2009011903:Insufficient buffer size`，进而阻断后续流程
- **M1105（18.4650）**：用 `batch_035` 补入 Ghidra 漏掉的函数 **`0x8128A576`**（criFs user-heap malloc 回调）→ `[INDIRECT-MISS]=0`，游戏开始打开真实 `vita.cpk`（`OPEN/PREAD` 2048B），运行从 n=181,670 推进到 12,600,000+
- **M1106–M1108（18.4651–53）**：CPK 挂载卡点逐层定位——Analyzer 结构 `0x84B1B828`（`[0]=state [1]=reader [3][4]=缓冲`）；**锁不是卡点**（SWAP 原子探针证明所有锁成对）；真正卡点=**reader 状态 `+0xBC` 停在 1(loading)**，Load 状态机只被驱动 2 步
- **M1109（18.4654）**：**HLE I/O 泵 `P5D_IOPUMP=1` 打通读完成**——直接泵 `FUN_8128D502(reader=0x848FC368)`，Load 状态机 1→2→3→7，完成 shim 置 `status=2`；**CPK Analyzer 首次进入分析 `FUN_81292794`**
- **M1110（18.4655）**：缓冲 dump 证明 **HLE 读数据与真实 vita.cpk 100% 一致**；vita.cpk 是 **64 位头/变体 CPK**（`+0x08`=u64 文件大小、`+0x10`=`@UTF`）；需要解密/头变换
- **M1111（18.4656）**：解密器注册链解码——`progress[0x20]/[0x21]` 来自 `FUN_8128EFF4(progress, reader+0xF4, reader+0xF8)`；全对象写 watch 证明游戏**从未注册解密器**（`reader+0xEC/0xF0=0`）
- **M1112（18.4657）**：**HDRFIX 头变换成功**——64 位 CPK 头在内存中标准化为 32 位布局（`+0x04=0xFF`、`+0x08=UTF 表大小`、`+0x0C=0`），`E2011022806:CPK is broken` 错误**完全消失**（CRILOG=0）；发现 **CRI UTF 表字段是大端序**
- **M1113（18.4658）**：**修复 AOT 翻译器通用 bug `LDRD rX,rY,[rX,#imm]`（34 处）**——第二字误用被覆盖的基址；修复后 **BADMEM 归零**、CPK 表读取解锁（`PREAD off=2048`）、游戏开始挂载 `data.cpk`
- **M1114（18.4659）**：错误调用点全部定位——IFF 错误来自 `FUN_812B074C`（CRI UTF Retriever/VITA，期望大端 `@UTF`）；内存错误来自 CPK 分析器 `FUN_812938C6`；同时安装**通用动态 reader 泵**（扫描 `progress[0]=0x81540460` 签名）
- **M1115（18.4660）**：**CPK 头分析成功（`[CPKRESULT] ret=0`）**；但解析字段全读成 0x800（64-bit 列变体被误读）；文件级分析找到**真实 TOC = 0x813EE800，大小 0x4C94（正好到 EOF）**，头 UTF 行真实值 `ContentOffset=0x800 / TocOffset=0x813EE800 / TocSize=0x4C94 / Align=0`

### 当前阻塞（明确、可执行）

1. **Fios 任务未入队（当前卡点，M1120）**：5000us 轮询 = Fios worker 空转循环 `FUN_811F22B8`（pending 链表 `DAT_81539754+0xC` 为空）；AtomEx 初始化已一次性完成（无重试），说明卡点不在音频初始化，而在**标题加载任务应入队而未入队**
   - 下一步：探针 Fios 管理器（+0xC/+0x14/+0x18/+0xFBC/+0xFC0）、xref `FUN_811F0B82` 处理器与 fios 提交 API，找出 title 加载任务应在何处入队；检查 app 主 fiber 真实等待点（8127BC14/81283D62/8128E9DE = criFs 区）
2. **之后**：TOC 加载 → 文件索引 → `mount status=2` → data.cpk 挂载（同样处理）→ title 素材读取
3. **GXM 画面仍黑**（M940–945 遗留）：纹理全 dummy、uniform 全零、几何数据未校验；需补纹理上传/uniform/顶点通路与 ShaderPatcher
4. **音频/输入/视频**：ACF/ACB 加载与 NGS 输出、Pad/Touch 输入流、真实 AVC 解码

## 最新标准运行环境（开发调试用，全部默认关闭）

```powershell
$env:P5D_NESTED_UNTIL=1500000; $env:P5D_GXMABI=1; $env:P5D_NOSCHEDRUN=1; $env:P5D_GETSELFFIX=1
$env:P5D_FASTSLEEP=1; $env:P5D_THRYIELD=1; $env:P5D_PERIODPUMP=1; $env:P5D_VTIME=1
$env:P5D_P1TICK=1; $env:P5D_RINIT=1; $env:P5D_OTFORCE=1; $env:P5D_STEPFORCE=1
$env:P5D_NOMOVIE=1; $env:P5D_FORCETITLE=1; $env:P5D_FIOSFILL=1; $env:P5D_WAITFORCE=1
$env:P5D_SCNFORCE=1; $env:P5D_THRPUMP=1; $env:P5D_PUMPMAIN=1; $env:P5D_POSTMAIN=1
$env:P5D_NGS=1; $env:P5D_IOPUMP=1
.\recomp_harness.exe 0x81198464 300000000
```

其中本阶段新增的**必需项**：

| 变量 | 作用 |
|---|---|
| `P5D_NGS=1` | **必需**：SceNgs HLE（缺失会导致 39MB 假 work size → `E2009011903`） |
| `P5D_IOPUMP=1` | **必需**：HLE 主循环泵 criFs Load 状态机（打通读完成/多 reader 自动发现） |
| `P5D_POSTMAIN=1` / `P5D_PUMPMAIN=1` | 主线程返回后继续 pump guest 线程（CPK 挂载等待需要） |

## 开发状态开关（当前阶段，仅调研用）

| 变量 | 作用 |
|---|---|
| `P5D_GXMABI=1` | GXM 调用走 ABI 桥接层（当前主线必需） |
| `P5D_NOSCHEDRUN=1` | 关闭旧调度 hack，使用 guest 自己的双 fiber 启动链 |
| `P5D_GETSELFFIX=1` | 线程级 `sceFiberGetSelf` 返回重新武装的 dummy（调度器恢复切换） |
| `P5D_THRYIELD=1` / `P5D_PERIODPUMP=1` | guest 线程合作让出 + 周期性抢占/pump |
| `P5D_VTIME=1` | 进程时间/RTC 返回虚拟时间（配合虚拟睡眠） |
| `P5D_FASTSLEEP=1` | 虚拟睡眠加速 |
| `P5D_OTFORCE=1` / `P5D_STEPFORCE=1` / `P5D_WAITFORCE=1` | boot/page 状态机选择性强制（研究用） |
| `P5D_NOMOVIE=1` / `P5D_FORCETITLE=1` | 跳过 USM 影片/直冲标题（开发加速） |
| `P5D_FIOSFILL=1` / `P5D_SCNFORCE=1` / `P5D_THRPUMP=1` | FIOS/场景/线程泵加速开关 |
| `P5D_P1TICK=1` | 每 1M n 打印进度（ctr/seq/pc/GXM 计数） |
| `P5D_CRIFSHLE=1` | 历史开关：由 HLE 调 guest `criFs_Initialize`（现在游戏自身 init 已跑通，不再必需） |

## 仓库结构

```
docs/
  P5D_MASTER.md              权威研究/开发记录（全部里程碑、证据、结论与历史纠正，约 36MB）
  P5D_DIGEST.md              精简摘要（脚本自动生成，勿手改）
  P5D_OPERATION_CHECKLIST.md 操作/验证清单
  REUSABLE_KNOWLEDGE.md      可复用知识清单（历史成果/工具/规格/负结果，按 ROI 排序）
  COMPARISON_VITA3K.md       与 Vita3K 模拟器的区别
  GXM_ABI_TEST_REPORT.md     GXM ABI 桥接测试报告
  HLE_DIFF_REPORT.md         HLE 与真实系统的差异报告
  SCHED_SPIN_ANALYSIS.md     调度/自旋循环分析
  P5R_ANDROID_REFERENCE.md   同族引擎移植项目（P5R Android）参考研究
  RELEASE_COMPLIANCE.md      发布合规策略（红线/交付形态/本地缓存）
  _MD_INDEX.md               文档索引
tools/
  release_audit.py           发布树合规扫描（禁止游戏文件/游戏衍生数据）
  kb_audit.py                知识库锚点审计（每个里程碑必须追加锚点）
  verify_doc_integrity.ps1   文档完整性校验
  run_kb_audit.ps1 / run_all_regressions.ps1   一键校验
  aot_difftest.py            AOT 差分测试
  diag.py / hle_probe.py     诊断与 HLE 探测工具
engine/
  recompiler/_gen_split.py   ARM/Thumb → C++ AOT 重编译生成器
  recompiler/_gen_batch2.py  指令发射后端（移位/写回/字面量/LDRD 等语义修正）
  runtime/recomp_harness.cpp 静态重编译运行时 + HLE（内存映射、线程/fiber、GXM/文件/Pad、I/O 泵、HDRFIX）
  runtime/gxm_abi.cpp        GXM ABI 桥接层（导入 stub + 指针翻译）
  runtime/cpk_reader.h       CPK 虚拟文件系统 + CRILAYLA 解压（header-only）
  gxm/                       GXM → Vulkan 后端（sce_gxm、vulkan_backend、texture_format）
  mem/                       虚拟地址空间注册/翻译（客机 VA → 宿主指针）
  common/                    公共类型与日志
（开发工作区另有 AOT 生成的 batch_000..batch_035.cpp、recomp_harness.exe 等，不进仓库）
```

## 设计路线

1. **原版二进制一字不改**：直接读取玩家提供的 eboot/SELF（不修改、不重打包）。
2. **静态重编译**：启动前一次性把 ARM/Thumb 全量翻译为 x86-64（本机生成并缓存），执行路径不做运行时代码生成；解释器仅作 fallback。
3. **HLE 只替换系统边界**：游戏代码原样执行，文件/内存/线程/图形/音频/输入等系统 API 由兼容层实现。
4. **GXM→Vulkan 在 API 层**：不模拟 GPU 命令流，把 SceGxm 调用映射为 Vulkan 调用；着色器由玩家数据中的 GSC/GXP **在玩家机器上一次性翻译**为 SPIR-V 并缓存。
5. **发布形态**：`P5D_Engine.exe`（引擎，无游戏数据） + 玩家自备游戏数据 + 本地缓存（`cache/recomp`、`cache/shaders`、`cache/pipelines`）。

## 验证与审计

```powershell
py tools/release_audit.py .        # 发布树合规（PASS 才可提交）
powershell -File tools/run_kb_audit.ps1
powershell -File tools/verify_doc_integrity.ps1
powershell -File tools/run_all_regressions.ps1
```

每个里程碑要求：权威结论写入 `docs/P5D_MASTER.md` 的「ANTI-FORGETTING ENTRY」+ `tools/kb_audit.py` 追加锚点，
以上四项检查全绿后才会提交；历史记录只追加、不删改。

## 构建与运行（开发中）

当前仓库仍是**开发中间状态**，尚未提供开箱即用的构建脚本；后续会补充：

- 编译 GXM→Vulkan 后端与运行时（MinGW g++ / MSVC + Vulkan SDK）
- AOT 生成器用法（输入：玩家自备 eboot；输出：本地代码缓存）
- 首次运行 "Prepare" 流程（本地 AOT + 着色器翻译）

## 合规与免责声明

- 本仓库只包含**我们自己的引擎/工具源代码**与**研究记录**；不包含也不接受任何游戏文件、密钥、固件、由游戏数据翻译得到的二进制（如 SPIR-V 缓存）或游戏画面素材。
- 本项目与 Atlus / SEGA / Sony 无关；仅用于**互操作性研究与个人备份游玩**。请使用自己合法持有的正版游戏。
- 许可证：**MIT**（宽松许可，见 [LICENSE](LICENSE)）。本仓库中的源代码、AOT 生成器、工具与研究文档均为 MIT；第三方组件许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
- MIT 仅覆盖本仓库中我们原创的内容；游戏本体与游戏素材版权归 ATLUS / SEGA，**不在 MIT 授权范围内**，且本仓库不包含任何游戏数据。

## 相关记录

- 权威开发记录（含所有里程碑与结论）：[docs/P5D_MASTER.md](docs/P5D_MASTER.md)
- 项目摘要：[docs/P5D_DIGEST.md](docs/P5D_DIGEST.md)
- 与 Vita3K 模拟器的区别：[docs/COMPARISON_VITA3K.md](docs/COMPARISON_VITA3K.md)
- 发布合规：[docs/RELEASE_COMPLIANCE.md](docs/RELEASE_COMPLIANCE.md)
- HLE 差异报告：[docs/HLE_DIFF_REPORT.md](docs/HLE_DIFF_REPORT.md)
