# 从本仓库完整复现 P5D PC 移植（v0.1.1 / M1642）

本仓库的 `src/` 是移植工程源码快照（`nativ_src` 提交 `6dd5ab9`，树 `b37fbbe75324...`，428 个文件，约 20 MB）。
按本文档可以从零完成：解密 → 分析 → 重编译 → 编译 → 运行，以及构建启动器和打包单文件发布版。

> **本仓库不含任何游戏数据、游戏密钥、固件、解密产物或工具链二进制**；游戏与固件必须由你自己合法持有。
> 许可证：GPL-2（基于 Project Nativ 架构，见 `LICENSE` / `NOTICE.md` / `THIRD_PARTY.md`）。

## 0. 你需要自备

| 项目 | 说明 |
|---|---|
| 游戏 | PS Vita《Persona 5: Dancing in Starlight》**PCSH10109 亚洲版 1.00** 的拷贝（含 `eboot.bin`、`data/`；MaiDump 格式也可） |
| 系统固件 | `PSVUPDAT.PUP` **3.74**（或已解密的固件目录：`version.txt`、`os0/`、`vs0/`） |
| 操作系统 | Windows 10/11 x64；支持 Vulkan 的显卡驱动（也可用 D3D12：SDL_GPU 会自动选择） |
| 磁盘 | 构建目录建议 ≥ 10 GB 空闲 |

## 1. 获取源码

```bat
git clone https://github.com/kaimeng114514/PSV-P5D-Recomp-port-PC.git
cd PSV-P5D-Recomp-port-PC\src
```

源码就是主分支里的 **`src/`** 目录（`nativ_src` 提交 `6dd5ab9` 的快照；完整 git 开发历史保存在本地工程，仓库不发布
历史对象）。下文命令都在 `src/` 目录下执行。

## 2. 工具链：两种获得方式

### A. 最省事：从发布版启动器 exe 解包（与官方构建完全一致）

运行 `P5D Windows移植版启动器.exe`（首次运行会自解包，约 1 分钟）。解包目录在
`%LOCALAPPDATA%\P5D-Port\<版本ID>\`，里面就有：

```
<版本ID>\mingw64\bin\gcc.exe         MinGW-w64 GCC（本机验证版本 16.1.0, Brecht Sanders r4, x86_64-msvcrt-posix-seh）
<版本ID>\toolchain\SDL3\             SDL3 3.4.16 开发文件（x86_64-w64-mingw32）
<版本ID>\toolchain\zlib\             zlib 开发文件
<版本ID>\toolchain\ffmpeg\           FFmpeg 开发库
<版本ID>\python\python.exe            嵌入式 CPython 3.10.9 + site-packages（capstone 5.0.9 / pycryptodome 3.24.0 / tomli 2.5.0）
```

把这三个目录拷到你习惯的位置（或直接指向解包目录），按第 3 节设置环境变量即可——**这就是官方构建用的同一套工具链**。

### B. 自行准备

| 组件 | 版本（本机验证） | 说明 |
|---|---|---|
| MinGW-w64 GCC/G++ | 16.1.0（winlibs, x86_64, msvcrt, posix, seh） | 需要 `gcc.exe`、`g++.exe`、`windres.exe` |
| SDL3 开发包 | 3.4.16（x86_64-w64-mingw32） | 含 `include/`、`lib/libSDL3.dll.a`、`bin/SDL3.dll` |
| zlib | 1.x 开发包 | 打包自解压 stub 用 |
| FFmpeg 开发库 | nativ 的 Windows 构建 | `libavcodec`、`libavformat`、`libavutil`、`libswscale`、`libswresample`（静态库） |
| Python | 3.10.9（或 3.11+） | `pip install capstone==5.0.9 pycryptodome==3.24.0 tomli==2.5.0`；Python 3.10 需 `tomllib` shim：在 site-packages 放 `tomllib.py`，内容 `from tomli import load, loads, TOMLDecodeError` |

目录布局照 A 的 `toolchain/` 结构摆（`toolchain/SDL3`、`toolchain/zlib`、`toolchain/ffmpeg`）。

## 3. 环境变量

```bat
set ATIV_DUMP=C:\p5dwork                 :: 工作目录（放游戏解密结果与重编译产物，首次会自动建立）
set ATIV_WIN_TOOLCHAIN=C:\p5d-toolchain  :: 含 SDL3\ zlib\ ffmpeg\ 的 toolchain 目录
set RECOMP_HOST_CC=C:\p5d-toolchain-long\mingw64\bin\gcc.exe
set RECOMP_HOST_CXX=C:\p5d-toolchain-long\mingw64\bin\g++.exe
set JOBS=8                                :: 并行编译任务数
set PYTHONPATH=<python目录>\Lib\site-packages
```

（`ATIV_WIN_TOOLCHAIN`/`RECOMP_HOST_CC/CXX` 与发布版启动器的命名一致；`PYTHONPATH` 是为了让 `vitarecomp.py`
找到 capstone/pycryptodome/tomllib。）

## 4. 首次构建（在源码根目录）

```bat
python pipeline\vitarecomp.py add <游戏目录>        :: 导入并解密（识别 PCSH10109 / 1.00）
python pipeline\vitarecomp.py firmware <PSVUPDAT.PUP>  :: 安装固件（已给解密固件目录时可跳过）
python pipeline\vitarecomp.py ready PCSH10109      :: 分析 + AOT 重编译（首次 10–30 分钟）
python pipeline\vitarecomp.py build PCSH10109      :: 编译宿主程序（首次约 8–10 分钟）
python pipeline\vitarecomp.py run PCSH10109        :: 启动游戏
```

产物与日志在 `%ATIV_DUMP%\decrypted\PCSH10109\recomp\`（`game_host_min.exe`、`session.log`）。
`ready`/`build` 都是增量缓存（内容哈希），源码不变时再次执行只需几秒到 1 分钟。

## 5. 构建启动器

```bat
powershell -ExecutionPolicy Bypass -File dev\build_p5d_launcher.ps1
```

产物：`build\p5d_launcher\P5D Windows移植版启动器.exe`（另需同目录 `SDL3.dll`）。使用第 3 节的
`ATIV_WIN_TOOLCHAIN` 与 `RECOMP_HOST_CXX`。

## 6. 打包单文件发布版 exe

`dev\make_p5d_release.py` 把源码 + 工具链 + 启动器 + 嵌入式 Python 打成一个自解压 exe（约 300 MB）。
打包前改脚本顶部的路径常量：

```python
MINGW    = r"C:\p5d-toolchain-long\mingw64"
TOOLCHAIN= r"C:\p5d-toolchain"       # SDL3/zlib/ffmpeg
WORK     = r"C:\p5d-release-work"    # 临时目录
PYEMBED  = os.path.join(WORK, "pyembed")
```

`PYEMBED` 的生成：下载 Python 3.10.9 **Windows embeddable package (64-bit)** 解压到 `pyembed\`，再
`python -m pip install --target pyembed\Lib\site-packages capstone==5.0.9 pycryptodome==3.24.0 tomli==2.5.0`，
并按第 2 节放入 `tomllib.py` shim。然后：

```bat
python dev\make_p5d_release.py --out <输出目录>\P5D Windows移植版启动器.exe
```

测试自解压包：双击运行，首次解包后出现启动器，按「游戏 ROM / 系统固件 / 开始构建 / 启动游戏」走一遍。

## 6.5 图片资源（按发布红线不入库）

`src/` 不含 `.png`（发布红线）：启动器图标/背景、自动演奏横幅截图、Kenney CC0 提示图集共 24 个文件被排除，
清单见 [`src/ASSETS_EXCLUDED.txt`](../src/ASSETS_EXCLUDED.txt)。**游戏本体的构建与运行不需要它们**；构建启动器或
打发布包时，从发布版 exe 首次运行解包出的 `%LOCALAPPDATA%\P5D-Port\<版本ID>\` 里把同路径文件复制回 `src/` 即可
（解包目录即完整 payload，与官方构建完全一致）。

## 7. 验证（提交/发布前必跑）

```bat
python tools\kb_audit.py                                :: 两棵文档树各跑一次：KB_AUDIT_PASS
powershell -ExecutionPolicy Bypass -File tools\verify_doc_integrity.ps1   :: DOC_INTEGRITY_PASS
python tools\release_audit.py .                         :: release_audit: PASS
python tools\check_text_encoding.py .                   :: [ENCODING] PASS
```

游戏内检查清单（v0.1.1 基准）：标题序列 → 主菜单 → 选歌（难度、预览角色动画实时）→ 舞池 60 fps（时间轴真实、
无倍速；Q/A/Z/D/O/K/M/J/Space 按键、F5 自动演奏开关 + P5 横幅 1 秒、F12 标记）→ 暂停（Esc/右键一次关闭；设置页右键
直接退出）→ 结算；4K 选项在 2K 屏上表现为 4x 超采样（内部渲染 3840×2176，画面不超出屏幕）。

## 8. 参考数据（M1642 / v0.1.1）

| 项 | 值 |
|---|---|
| 源码提交 | `6dd5ab9250b862e45679c6ee8c8c7ae105866499`（树 `b37fbbe7532438edafedb21b9f511f16f681ad8c`，428 文件 / 20 MB） |
| 发布 exe | 300,442,296 B，payload `v0.1.1-p5d-a50c78a+81c14158c509` |
| SHA256 | `7EAC003301A66406ADBA5860078AD6B205660A2B1C618CBE15D5CA8E4E36B361` |

构建产物含时间戳与并行顺序，**不保证比特级一致**；上表用于确认版本与内容对应，不要把 SHA256 当作可复现构建的校验值
（唯一需要逐位一致的是 `version.txt` 里的 payload 版本，它由源码内容哈希得出）。

## 9. 常见问题

- `ModuleNotFoundError: No module named 'tomllib'`：Python 3.10 没有内置 tomllib，按第 2B 安装 `tomli` 并放 `tomllib.py` shim（或 `PYTHONPATH` 指向发布包里的 site-packages）。
- `Windows blocked the game: Smart App Control ...`：Windows 11 智能应用控制拦截新编译的 exe，重建一次通常可过，或在 Windows 安全中心关闭 Smart App Control。
- 启动黑屏/崩溃：确认显卡驱动支持 Vulkan（或用 `set RECOMP_GFX=gpu`），并检查 `session.log` 开头是否 `gpu: vulkan device ...`。
- 构建目录放错位置：`ATIV_DUMP` 下会生成 `decrypted/`、`firmware/`；不要把游戏原数据放进仓库目录。
- 红线：**永远不要**把游戏文件、固件、解密产物或重编译生成的游戏代码（`game_part*.c`、`game.img` 等）提交到仓库。

## 10. 许可

- 本项目：GPL-2（见 `LICENSE`）；
- 基于 [Project Nativ](https://github.com/TheLostPantheon/nativ)（GPL-2）架构与部分 GPL-2 参考；
- 第三方组件与许可见 `THIRD_PARTY_NOTICES.md` / `src/THIRD_PARTY.md`；
- 游戏画面/数据版权归 ATLUS / SEGA，本仓库仅包含移植代码。
