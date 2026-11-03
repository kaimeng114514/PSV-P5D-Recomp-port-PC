# P5D-PC — P5D（PCSH10109）PC 原生重编译移植

> 仓库：<https://github.com/kaimeng114514/PSV-P5D-Recomp-port-PC> · 许可证：GPL-2（见 [LICENSE](LICENSE) 与 [NOTICE.md](NOTICE.md)）

把 PS Vita 版《Persona 5: Dancing in Starlight》（P5D，PCSH10109）的**原版游戏二进制原样加载**，
在 PC（后续可扩展到手机）上由我们实现的本地兼容层（HLE）驱动运行。
CPU 采用 **AOT 静态重编译**（原版 ARM/Thumb 代码一次性翻译为 x86-64 原生执行）+ 解释器 fallback；
系统 API（文件/存档、内存、线程、GXM→Vulkan、音频、输入、视频解码）全部由 HLE 接管；
最终由**原版游戏代码自己绘制**画面，目标串联 `S0_LOGO → S1_TITLE → S2_MENU → S3_SELECT → S4_GAME → RESULT` 全流程。

> 本仓库**不包含任何游戏文件、游戏密钥、固件或由游戏数据翻译出的二进制产物**。
> 要运行本项目，玩家必须使用**自己合法持有的正版游戏**从自己的游戏机提取数据（详见 [docs/RELEASE_COMPLIANCE.md](docs/RELEASE_COMPLIANCE.md)）。

---

## 当前状态（2026-11-03，里程碑 **M1180**）

**一句话**：原版 eboot 已在 PC 上原生（AOT）跑起来；S0_LOGO 已完整通过；
当前首次在**游戏真实帧流程**（BeginScene → Draw → EndScene → RT 回读 → Present）中捕获到**可见渲染内容**（960x544 诊断三角形，2 次 present），
并已越过 GFD job 销毁等待与 appSound 致命断言；下一步是把诊断绘制替换为原版真实顶点流/UBO/纹理，并逐步移除调试杠杆。

| 模块 | 完成度 | 当前状态（M1180 实测） |
|---|---|---|
| ARM/Thumb → x86-64 **AOT 静态重编译** | ~97% | **230,291 个基本块** + 自动补齐的 **1,946 个缺失续块**（`engine/recompiler/_rscan_gaps.py` / `_gen_gapfix.py`）；`[GBACK]` 从 400 降到 6（仅剩 2 字节错位类边界） |
| 原版 eboot/SELF **原样加载** | ~97% | ELF 段映射、导入表/NID 解析、module_start 全部可用；未经任何字节修改 |
| **HLE 系统 API 接管** | ~85% | 内存/线程/fiber/信号量/事件标志/文件/GXM/音频(NGS)/CRI 日志；host-thread + host-fiber 调度模型稳定运行 |
| **CRI FS / CPK 虚拟文件系统** | ~85% | `data.cpk`(10,555 文件) / `vita.cpk`(531 文件) 挂载与读取可用；**真实异步完成语义仍靠调试杠杆 `P5D_FORCEST` 强制 reader=2**（待移植成熟实现） |
| **GXM → Vulkan** | ~70% | 真实 GXM 初始化、`ShaderPatcherRegisterProgram`×121、fragment/vertex program 创建、RenderTarget/RT 回读、display queue present 全部跑通；首个真实帧流程可捕获可见内容；真实顶点流/UBO/纹理数据尚未接通 |
| 音频（SceNgs / CRI Atom） | ~20% | NGS work size / ACF 注册可用；`appSound.c:809` 的 COMSE 状态断言仍未根治（暂用 `P5D_NOABORT` 调试杠杆越过） |
| 输入（SceCtrl / SceTouch） | ~15% | Pad/Touch HLE 骨架可用、可注入按键；游戏侧输入流未接通 |
| 视频解码 | ~20% | 启动 USM 影片由 guest 软件解码跑通；真实 AVC/CRI Mana 解码未完成 |
| 场景链 S0→S4→RESULT | S0 ✅ / S1 进行中 | S0_LOGO 完整通过；S1_TITLE 资源加载进行中；S2–S4 未到达 |
| 单 exe 打包 / 移动端 | ~40% / 0% | 目前是单一 Windows 可执行 `recomp_harness.exe`（AOT+HLE 开发态）；正式打包/移动端未开始 |

### 距离目标还有多远（诚实评估）

| 关口 | 状态 | 剩余工作 | 粗略轮数 |
|---|---|---|---|
| CPK 挂载 + 文件索引 | **85%** | 真实 SceFios 异步完成语义（去掉 `P5D_FORCEST`） | 5–10 |
| GXM 真实画面（纹理/uniform/几何/ShaderPatcher） | 70% | **诊断绘制→真实顶点流/UBO/纹理**、RT/捕获时序、真实管线选择 | 10–30 |
| 首帧可见内容 | ✅ | 诊断内容已可见；待替换为原版内容 | 3–10 |
| S1 标题画面（对象/动画/音效触发） | ~40% | 依赖上面两项 + 标题状态机 | 20–40 |
| S2–S4 全流程 | 0% | 菜单/选择/游戏/结算 + 存档 + 音频 + 视频 | 100+ |
| 画面/布局/数值与原版一致 | 0% | 完成全流程后逐场景比对（Vita3K 可作为对照 oracle） | 持续 |

> **关于使用其他开源实现**：仓库许可证为 GPL-2；在保持「原版二进制不改 + AOT CPU + 我们的 HLE」前提下，允许移植/参考 nativ、Vita3K 等 GPL-2 项目的 HLE 语义与 GXM 翻译层实现（保留署名与源码开放）。当前最值得移植的是 **Vita3K 的 GXM→Vulkan 翻译层**与 **SceFios 异步语义**。

### 最近关键里程碑（M1160–M1180，2026-11-03）

- **M1180**：越过 appSound 致命断言（`P5D_NOABORT` 调试杠杆），帧循环持续推进（GFD job 提交 21→200 次、fiber 块数 61.1M→65.5M）。
- **M1179**：**首次在真实帧流程中捕获可见渲染内容**（960x544 红三角，2 次 present；RT `max=255 mean=57.79`）。
- **M1178/M1177**：GFD 销毁链语义澄清；首轮销毁等待可完成，二次销毁对已销毁队列卡死。
- **M1176**：GFD job worker 生命周期分析（队列 state=2 退出）。
- **M1175**：**系统性 AOT 缺口批量修复**：扫描 230,291 块 → 生成 1,946 个缺失续块，`[GBACK]` 400→6。
- **M1170/M1169**：FORCEST 解锁 CPK 挂载 → 真实 GXM 初始化/着色器/绘制状态构建。
- **M1163–M1165**：guest fiber 架构逆向（主线程停 Run 是正常设计，游戏主循环在 fiber#2）。
- **M1160–M1162**：host-fiber 调度器重写为严格状态机（消除 libwinpthread 崩溃），吞吐恢复 156.9M blocks。

### 当前阻塞（明确、可执行）

1. `appSound.c:809`：COMSE 声音对象 `+4` 的 bit0 未置位（HLE 音频状态语义缺失）——需补 sceAudioOut/NGS/AT9 状态或修正调用返回值；
2. GXM 真实绘制：把诊断测试三角形替换为原版真实顶点流/UBO/纹理；
3. 去掉调试杠杆：`P5D_FORCEST`（CPK reader 完成）、`P5D_NOGFDASSERT`（GFD 断言）、`P5D_NOABORT`（致命断言）、`P5D_TESTPIPE`（诊断管线）；
4. 剩余 `[GBACK]` 2 字节错位类边界收尾。

---

## 最新标准运行环境（开发调试用，全部默认关闭）

```
P5D_HOSTFIBER=1        # guest fiber = 宿主线程（P0 调度模型，隐含 P5D_HOSTTHREADS=1）
P5D_GXMABI=1           # GXM ABI 观测/翻译管线
P5D_NESTED_UNTIL=2000000
P5D_GETSELFFIX=0 P5D_FASTSLEEP=1 P5D_THRYIELD=1 P5D_PERIODPUMP=1
P5D_VTIME=1 P5D_P1TICK=1 P5D_RINIT=1 P5D_OTFORCE=1 P5D_STEPFORCE=1
P5D_NOMOVIE=1 P5D_FORCETITLE=1 P5D_FIOSFILL=1 P5D_WAITFORCE=1
P5D_SCNFORCE=1 P5D_PUMPMAIN=1 P5D_POSTMAIN=1 P5D_NGS=1 P5D_IOPUMP=1
```

调试杠杆（**仅用于定位问题，最终必须移除**）：

```
P5D_FORCEST=1        # 强制 CRI reader 状态 1→2（绕过未实现的异步完成语义）
P5D_NOGFDASSERT=1    # 跳过 GFD 断言（gfd_app_gx.c:52 等）
P5D_NOABORT=1        # 跳过致命 abort 函数（8115F9CA，appSound 断言后）
P5D_TESTPIPE=1       # 用诊断管线/测试三角形验证渲染链路
P5D_SCENECAP=1       # end_scene_pass 后捕获场景 RT
```

---

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

## 文档编码纪律（防乱码）

- 所有 `.md`/`.py`/`.ps1` 源文件必须是 **UTF-8**（建议无 BOM）；禁止用 PowerShell `>` / `Set-Content`（默认 ANSI/UTF-16）重写仓库文本文件；
- Python 写文件必须显式 `encoding="utf-8"`；提交前运行 `py tools\check_text_encoding.py`（失败即拒绝提交）；
- 发现乱码（例如 `鈥?` / `锛` / `鍙` 等 GBK 二次编码特征）时，用 git 历史找回最后一个干净版本（本仓库 README 的干净基线为提交 `a5afc85`），不要手工在乱码上继续编辑。

---

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
