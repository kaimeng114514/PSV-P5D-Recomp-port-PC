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
