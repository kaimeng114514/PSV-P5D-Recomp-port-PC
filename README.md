# P5D-PC — P5D（PCSH10109）PC 原生重编译移植

把 PS Vita 版《Persona 5: Dancing in Starlight》（P5D，PCSH10109）的**原版游戏二进制原样加载**，
在 PC（后续可扩展到手机）上由我们实现的本地兼容层（HLE）驱动运行。
CPU 采用 **AOT 静态重编译**（原版 ARM/Thumb 代码一次性翻译为 x86-64 原生执行）+ 解释器 fallback；
系统 API（文件/存档、内存、线程、GXM→Vulkan、音频、输入、视频解码）全部由 HLE 接管；
最终由**原版游戏代码自己绘制**画面，目标串联 `S0_LOGO → S1_TITLE → S2_MENU → S3_SELECT → S4_GAME → RESULT` 全流程。

> 本仓库**不包含任何游戏文件、游戏密钥、固件或由游戏数据翻译出的二进制产物**。
> 要运行本项目，玩家必须使用**自己合法持有的正版游戏**从自己的游戏机提取数据（详见 [docs/RELEASE_COMPLIANCE.md](docs/RELEASE_COMPLIANCE.md)）。

---

## 当前状态（2026-11-01，里程碑 M1030；整体约 70–75%）

| 模块 | 完成度 | 当前状态 |
|---|---|---|
| ARM/Thumb → x86-64 **AOT 静态重编译** | 90–95% | **229,791 个基本块 / 32 个宿主批次**已翻译并原生执行，`[NOFN]=0`；修复全局性 **pc-relative 字面量加载错误**（`ldr/ldrd/vldr [pc,#imm]` 原先按 `r15` 生成 → 485 处修正、22/32 批次重编） |
| 原版 eboot/SELF **原样加载** | 85–90% | ELF 段映射、导入表、NID 解析、模块启动全部可用；默认 30M 块回归稳定 `exit=0`；**完整 boot 链已打通**：S0_LOGO → 启动 USM 影片（约 100 秒虚拟时间，guest 软件解码器 `FUN_811DF91A`）→ 应用世界在 **n≈59.5M** 恢复运行 → **S1 标题加载页（page1）开始运行** |
| **HLE 系统 API 接管** | 70–75% | 内存/线程/fiber/信号量/事件标志/消息/文件/GXM/CRI 日志骨架均工作；本阶段新增：`sceKernelCreateLwMutex` 语义修正、`sceKernelLockLwMutexCB`(0x812B2800) 别名、`sceDisplayWaitVblankStartMultiCB` 时间片模拟、`sceKernelGetProcessTimeWide` 虚拟时钟模式、CRI FS 初始化桥（`0x8128399E`）等 |
| **GXM → Vulkan** | 60–70% | **提交流程已打通、画面内容仍为黑（诚实版）**：真实 SPIR-V 管线（GXP→SPIR-V manifest 4331 条）创建成功（`[REAL-PIPE] #1 OK`），BeginScene → Draw → RT→swapchain 拷贝 → `vkQueuePresentKHR result=0` 全链执行；但实测 RT 截图 **全黑**（PIL mean=0/max=0），根因：纹理全绑 dummy（`[TEXBIND] dummy=16`）、uniform 全零（RINIT 零表兜底）、顶点/几何数据未校验；正在补渲染数据通路 |
| 资源 / 文件系统 | 65–70% | `data.cpk`（10,555 文件）+ `vita.cpk`（531 文件）已挂载；**CRI FS binder 已初始化并保持**（`binit=0x816A0BA0`，n=5000 由 HLE 直接调用 guest `criFs_Initialize`）；page1 阶段 **CRI 流已打开（`streamcnt=3`）**，sequence 控制器 `page0..page5` 回调全部注册；读完成投递链路仍在定位 |
| 线程 / fiber / 调度 | 85–90% | **guest 双 fiber 启动链完全打通**（fiber A 原生完成位 → `FUN_8120683C` 返回 → 销毁 A → 运行 fiber B）；线程级 `sceFiberGetSelf` 重新武装 dummy 使调度器恢复切换；新增**周期抢占 + 虚拟唤醒期限**（`P5D_PERIODPUMP`/`P5D_THRYIELD`），修复"抢占后 worker 永久 waiting"；调度实验早期门控 `n>=100000` 保护 CRI 初始化 |
| 音频（SceNgs / CRI Atom） | 5–10% | CRI Atom 初始化已进入；仍缺 ACF/ACB 配置与解码，音频输出为桩 |
| 输入（SceCtrl / SceTouch） | ~10% | Pad/Touch HLE 骨架可用，可注入按键；游戏侧输入流未接通 |
| 视频解码 | 10–15% | 启动 USM 影片由 guest 软件解码跑通（约 5,000 万块）；`VdecInitLibrary` 已在 page1 被调用一次，真实 AVC/CRI Mana 解码未完成 |
| 场景链 S0→S4→RESULT | **S0 ✅ / S1 加载中（page1 已出画面）** | S0_LOGO 完整通过；S1_TITLE 的 page1 现在会真实渲染并 present（首帧已上屏），但 page1 尚未结束；S2–S4 未到达 |
| 单 exe 打包 / 移动端 | ~40% / 0% | 仓库/许可证/审计/CI 在跑；单文件成品与移动端尚未开始 |

### 最新里程碑（M1000–M1030，2026-11-01）

- **M1000–M1005**：Job 完成链打通（trampoline 强制完成）→ page1 8 步初始化完成、job "title sequence" 提交、**page6 TITLE 页启动**；序列页表从 ELF 完整解析（page0 DEBUG MENU / page1 initread / page2 FREE MODE / page3 COMMU / page4 TUTORIAL / page5 MAIN MENU / **page6 TITLE start=0x81185974 update=0x81185986 end=0x81185998**）
- **M1011–M1018**：探针盲区定位（title 路径走 nested runner，job_probes 不生效）→ 所有关键探针下沉到 AOT block 内（batch 级）
- **M1015–M1016**：**AOT 内部函数覆盖率 100%**（Ghidra 12,532 函数全部命中；230,243 blocks；仅剩 14 个 HLE import + module_start 无 block）
- **M1019–M1020**：appFade 真相闭环——fade "对象"是周期 job 节点，被"全强制完成"破坏其 job arg（0xE5 毒值）
- **M1021–M1022**：改为**选择性强制**（只强制 boot init worker）→ 周期 job 全部真跑（JOBNORM 28 条），appFade 状态修复；title worker 真正运行并推进到声音对象缺口
- **M1023–M1026**：移除 page1 提前结束强制 → **真实 init 链启动**（trophy/sound/initDataRead worker 真实调度）；发现 STEPFORCE 对 nested runner 无效 → 18 个 init 检查 block 强制下沉到 AOT 内
- **M1027–M1030**：**找到 8 步 init 链驱动器 `FUN_81184FFE`（"initDataRead" worker）**：9 项函数表 + 状态机，推进条件 `FUN_8116F8D2(step_job)==0`；trophy step 已通过、卡点缩小到 **step1 "init sound" 的完成**（本轮 SSTEP/DRV 探针运行中）

### 最近关键里程碑（M869–M923）

- **M869–M871**：boot 影片阶段真实解码（n 5M→59.5M）；`ctr` 开始增长；`[SEQST]`/`[SEQREQ]`/`[SEQCB]` 页面序列控制器初始化，`page0..page5` 回调注册
- **M885（18.4430）**：**fiber RUNNING 位修复**——guest `FUN_812067A0` 只有 fiber `+0x80` bit0 置位才置完成位 bit2；HLE `fiber_load` 补置 bit0 → fiber A 完成投递打通，启动链解锁
- **M889（18.4438/18.4439）**：**线程级 `sceFiberGetSelf` 修复**——原先返回的 dummy 被清零且 guest 链表头为 0，`FUN_8120683C` 永不调用 `sceFiberSwitch`；改为每次调用**重新武装** dummy（`+0x80=1`、`+0x94` 指向存活 fiber）→ 调度器恢复切换，外层 n 从 1717 推进到 30,000,000
- **M894 / M896**：app 主循环真正运行（boot 影片解码循环 `0x811DFxxx`）；page1 活跃：`ctr` 0→174、`[SEQCB]` 179 条、`[NSET]` 14,720、`[SAVESLOT]` 4,909
- **M899–M902**：display queue 回调确认已注册；Ghidra 证明 **`FUN_81284960` 写 binder 全局、唯一调用者 `FUN_8128399E` = CRI FS 初始化入口**
- **M904（18.4449）**：**GXM ABI `sceGxmGetRenderTargetMemSize` 从不写 out 参数** → guest 拿栈垃圾 `0x846001FC` 当 memblock 大小（2.2GB）→ `[MEMA-OOM]` ×5；修复后 `size=0x33D000` 正常分配
- **M906–M917**：page1 定向探针（`[H57]`/`[H57M]`/`[HLEDUMP]`/`[PDUMP]`）；确认 page1 期间**没有任何文件 I/O API 调用**，主上下文只有 libc + `DelayThreadCB`；部分 worker 线程 `ready=1` 却从未执行
- **M911–M914**：`[PDUMP]` 证明 worker 被 `g_cur_thr` 上下文屏蔽；周期抢占判据最初用 `g_blkcnt_total`（嵌套执行冻结）→ 改用 **job_probes 调用计数**后抢占生效；**n 突破 59.8M 卡点推进到 64M**；修复"抢占后无唤醒期限 → worker 永久 waiting"；`n>=100000` 门控保护 criFs 初始化（binder 恢复）
- **M915**：门控版长跑 **CRI 资源流开始工作（`streamcnt=3`）**，n 推进到 63M，binder 持久
- **M919（18.4464）**：`[HLEDUMP]` 发现 **`sceKernelLockLwMutexCB`(0x812B2800) 有 1,740,287 次调用却无 HLE 实现**（WaitEventFlag 9.23M 比 SetEventFlag 6.98M 多 225 万次）→ 补别名
- **M921（18.4466）**：反汇编发现 **CRI 帧节拍器 `FUN_8127BB30`**：用 `sceKernelGetProcessTimeWide()/1000` 计算帧间隔并 `DelayThreadCB(sleep)`；真实墙钟 vs 虚拟时间不一致 → `P5D_VTIME=1`（进程时间/RTC 返回虚拟时间）
- **M922–M923**：`P5D_WAKE57` 周期唤醒阻塞在 eventflag `60018000/6001A000/6001C000` 的 loader worker；`[P1TICK]` 每 1M n 打印 page1 进度（`ctr`/seq/pc/GXM 计数）

### 最近关键里程碑（M935–M940）

- **M935–M937（18.4480–18.4482）**：GXP→SPIR-V 管线接线：manifest 4331 条、FNV-1a64 哈希、`select_real_pipeline`；发现 page1 走 **Precomputed** 状态 API 且 ShaderPatcher 是 STUB（program 句柄全 0）→ 实现**假句柄链**（RegisterProgram → Create*Program → Precomputed 捕获）
- **M938（18.4483）**：假句柄链全链打通（`[ABIPGID]→[ABIPGOBJ]→[ABIOBJ]→[SHADERMOD] ok`），真实 `vkCreateGraphicsPipelines` 调用；修复 "Draw" 子串误匹配 `PrecomputedDrawInit` 导致的过早建管线崩溃
- **M939（18.4484）**：**第一发真实 draw 完整执行**（`[REAL-PIPE] #1 OK`、`[FRAMEBEGIN] frame=1`、`[FUBOHOST]`、`[TEXBIND]`）；发现 app 不调用 `sceGxmEndScene`，只调用 `sceGxmDisplayQueueAddEntry` → 把两者接到后端 `end_frame()`（提交+present）
- **M940–M943（18.4485–18.4488）**：present 提交流程打通（`vkQueuePresentKHR result=0`、RT→swapchain 拷贝、RT 截图钩子）；**诚实更正（M945）**：画面内容仍为黑，纹理/uniform/几何数据通路待补
- **M944–M945（18.4489–18.4490）**：窗口"未响应"修复（Win32 消息泵）；RT 截图实测全黑（PIL mean=0/max=0）、定位纹理 dummy/uniform 零值问题，启动红色片元着色器诊断实验

### 当前阻塞（明确、可执行）

- **init 链 step1 "init sound" 的完成条件未闭合**：driver `FUN_81184FFE` 已越过 trophy（创建了 sound job，WFDBG #49 node=85B8861C），但 sound step job 的 appProcess 节点未被完成回调释放 → `FUN_8116F8D2(step_job)` 仍非 0 → driver 停在 step2 等待；SSTEP/DRV 探针（batch_012 811848D6/DA/DE、811850A6/B2/BC）正在定位
- 完成后 driver 将继续创建 gamedata → msgwindow → mainmenu → step5 → 返回 -1 → 完成回调 setSequence(6,0,0,5) → page6 TITLE 页
- 之后：title 状态机（声音对象已创建后）→ title 对象 → 引擎自绘标题画面
- 后续：S2_MENU / S3_SELECT / S4_GAME / RESULT；音频/输入/真实视频解码；单 exe 打包（当前 AOT 与 HLE 已远超 M940 状态）


## 仓库结构

```
docs/
  P5D_MASTER.md              权威研究/开发记录（全部里程碑、证据、结论与历史纠正）
  P5D_DIGEST.md              精简摘要
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
  recompiler/_gen_batch2.py  指令发射后端（移位/写回/字面量等语义修正）
  runtime/recomp_harness.cpp 静态重编译运行时 + HLE（内存映射、线程/fiber、GXM/文件/Pad 等）
  runtime/gxm_abi.cpp        GXM ABI 桥接层（导入 stub + 指针翻译）
  runtime/cpk_reader.h       CPK 虚拟文件系统 + CRILAYLA 解压（header-only）
  gxm/                       GXM → Vulkan 后端（sce_gxm、vulkan_backend、texture_format）
  mem/                       虚拟地址空间注册/翻译（客机 VA → 宿主指针）
  common/                    公共类型与日志
```

## 设计路线

1. **原版二进制一字不改**：直接读取玩家提供的 eboot/SELF（不修改、不重打包）。
2. **静态重编译**：启动前一次性把 ARM/Thumb 全量翻译为 x86-64（本机生成并缓存），执行路径不做运行时代码生成；解释器仅作 fallback。
3. **HLE 只替换系统边界**：游戏代码原样执行，文件/内存/线程/图形/音频/输入等系统 API 由兼容层实现。
4. **GXM→Vulkan 在 API 层**：不模拟 GPU 命令流，把 SceGxm 调用映射为 Vulkan 调用；着色器由玩家数据中的 GSC/GXP **在玩家机器上一次性翻译**为 SPIR-V 并缓存。
5. **发布形态**：`P5D_Engine.exe`（引擎，无游戏数据） + 玩家自备游戏数据 + 本地缓存（`cache/recomp`、`cache/shaders`、`cache/pipelines`）。

## 开发状态开关（当前阶段，仅调研用）

运行时通过环境变量开启各阶段的实验/探针（默认行为保持不变）：

| 变量 | 作用 |
|---|---|
| `P5D_GXMABI=1` | GXM 调用走 ABI 桥接层（当前主线必需） |
| `P5D_NOSCHEDRUN=1` | 关闭旧调度 hack，使用 guest 自己的双 fiber 启动链 |
| `P5D_GETSELFFIX=1` | 线程级 `sceFiberGetSelf` 返回重新武装的 dummy（调度器恢复切换） |
| `P5D_CRIFSHLE=1` + `P5D_CRIFSHLE_AT=5000` | 在 boot 初始化后由 HLE 调用 guest `criFs_Initialize`（binder 建立） |
| `P5D_THRYIELD=1` | guest 线程 DelayThread 合作让出 + 虚拟唤醒 |
| `P5D_PERIODPUMP=1` | 周期性抢占/pump guest 线程（每 8192 个执行块） |
| `P5D_VTIME=1` | 进程时间/RTC 返回虚拟时间（配合虚拟睡眠） |
| `P5D_WAKE57=1` | page1 周期唤醒 CRI loader worker（实验） |
| `P5D_HLEDUMP` / `P5D_HLEDUMP_AT` | 统计并打印 page1 期间全部 HLE stub 调用直方图 |
| `P5D_P1TICK=1` | 每 1M n 打印 page1 进度（ctr/seq/pc/GXM 计数） |
| `P5D_BT57=1` | page1 帧节拍器调用点栈回溯（定位驱动函数） |

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
