# FastBitCopy — UE 插件

[![CI](https://github.com/chen3feng/FastBitCopy/actions/workflows/ci.yml/badge.svg)](https://github.com/chen3feng/FastBitCopy/actions/workflows/ci.yml)

[English](README.md) | [中文](README_CN.md)
**FastBitCopy** 通过一套跨平台的运行时函数 Hook，将 UE 中的
`appBitsCpy`（被 `FBitReader` / `FBitWriter`、网络同步、序列化等大量调用）
替换为高度优化的实现。**无需修改引擎源码，放进 `Plugins/` 即生效**。

## 为什么要做这个

UE 自带的 `appBitsCpy` 是逐字节处理的标量实现。在 x86-64 与 arm64 上我们
可以用 `memcpy` + 非对齐 64-bit 读写做到显著更快：

| 拷贝类型 | 原始 `appBitsCpy` | `appBitsCpyFastImpl` | 加速比 |
|---------|------------------:|---------------------:|-------:|
| 对齐    |  1743817 ns（1MiB）|              60595 ns | 约 30× |
| 非对齐  |  1776341 ns（1MiB）|             325179 ns |  约 5× |

（数据来自初始基准测试，实际值因平台而异。）

此处"对齐"指 `SrcBit % 8 == DstBit % 8` —— 源和目的的 bit 偏移在字节内
的位置相同，这是网络复制中最常见的情况。

## 工作原理

1. 运行时模块 `FastBitCopy` 在 `PostConfigInit` 阶段加载，早于任何游戏
   业务代码。
2. 取出 UE 导出的 `appBitsCpy` 符号地址。
3. 安装一个极小的跨平台 **inline hook**：
   * **x86-64** —— 5 字节 `E9 rel32` 近跳转；若 Detour 距离超过 ±2 GiB，
     自动回退到 14 字节绝对跳转 `FF 25 00 00 00 00 | imm64`。被占用的
     函数序言字节会由一个内置的最小反汇编长度引擎（Length
     Disassembler）解析，重定位到一块页对齐的可执行 **Trampoline**
     内存里，末尾接一条绝对跳回原函数剩余部分，从而保留原函数可调用。
   * **arm64** —— 16 字节绝对跳转序列
     `LDR X16,#8 ; BR X16 ; <imm64>`。同样把被替换的 4 条指令迁移到
     Trampoline，实现会拒绝任何 PC 相对的指令（`B`/`BL`/`B.cond`/`CBZ`/
     `TBZ`/`ADR`/`ADRP`/`LDR literal`）在函数前 16 字节中出现，保证
     重定位总是安全的。
4. 修改页权限：Windows 使用 `VirtualProtect`，Linux/macOS 使用
   `mprotect`；Apple Silicon 上遵循 W^X，Trampoline 页通过
   `MAP_JIT` + `pthread_jit_write_protect_np` 切换写入/执行。
5. 指令缓存刷新：`FlushInstructionCache` / `sys_icache_invalidate` /
   `__builtin___clear_cache`。
6. `ShutdownModule` 时会还原被修改的字节、释放 Trampoline 页，引擎状态
   恢复如初。

## 支持的平台

|            | Windows | Linux | macOS | Android | iOS |
|------------|:-------:|:-----:|:-----:|:-------:|:---:|
| **x86-64** |   ✅    |  ✅   |  ✅   |   —     |  —  |
| **arm64**  |   ✅    |  ✅   |  ✅（Apple Silicon） | ✅ | ❌ |

需要满足 `PLATFORM_LITTLE_ENDIAN`（x64、arm64 都满足）。
非对齐访问通过 `memcpy` 辅助函数处理，不依赖硬件非对齐加载支持。

**iOS / tvOS** — Apple 对所有可执行页强制执行 W^X 和代码签名验证，
运行时 inline hook 不可能实现。模块仍然编译（优雅降级为空操作），
但 hook 永远不会安装。

**Android** — Android 上的 Linux 内核允许 `mprotect(RWX)` 修改代码页，
且没有运行时代码签名强制验证，hook 正常工作。

**Big-endian 平台** — 优化实现在编译期完全排除；模块加载但不做任何
事情（不 hook、无额外开销）。

> **实际测试覆盖。** 目前已在三种操作系统（Windows / Linux / macOS）×
> 两种 CPU 架构（x86-64 / arm64）上实际测试通过。CI 在 `-O2` 下
>（MSVC / GCC / Clang）编译并运行字节对齐与位不对齐两条快速路径，
> 外加 Linux/macOS 的 `-O0` 对照扫、以及 Linux 上的一次 UBSan + ASan
> 专项，每项都覆盖 10 000 组随机 `(SrcBit, DstBit, BitCount)` 正确性
> 测试和页边界 / 确定性边界用例。上面表中的加速比是在 Windows (MSVC)
> 上测得，Linux/macOS 上的绝对数字会随编译器和宿主 CPU 不同而变化，
> 但走的是同一条优化实现路径。Android arm64 通过 NDK 交叉编译并在 CI
> 中以 QEMU 用户态模拟执行，验证了目标 ABI 上的算法正确性。

## 安装

插件文件夹的名称必须是 `FastBitCopy`（与 `FastBitCopy.uplugin` 同名）。
三种安装方式任选：

### A. 直接 `git clone`

```bash
cd YourGame/Plugins
git clone https://github.com/chen3feng/FastBitCopy.git FastBitCopy
```

### B. 作为 git submodule（团队开发推荐）

```bash
cd YourGame
git submodule add https://github.com/chen3feng/FastBitCopy.git Plugins/FastBitCopy
git submodule update --init --recursive
```

### C. 作为 git subtree（将代码嵌入你的仓库）

```bash
cd YourGame
git subtree add --prefix=Plugins/FastBitCopy \
    https://github.com/chen3feng/FastBitCopy.git main --squash
# 后续同步上游：
git subtree pull --prefix=Plugins/FastBitCopy \
    https://github.com/chen3feng/FastBitCopy.git main --squash
```

### D. 手动下载

下载仓库 zip，解压后将最外层目录重命名为 `FastBitCopy`，拷到
`YourGame/Plugins/` 下即可。

然后重新生成工程文件、重新编译。Hook 会在引擎初始化阶段自动安装，
无需调用任何 API。

## 本地构建与测试（使用真实 UE 源码构建）

仓库自带便捷脚本，可直接用本地 UE 源码构建 `TestHost` 工程：

```bash
# Windows
build_testhost.bat              # Editor（Development）
build_testhost.bat Game         # Game 客户端（Development）
build_testhost.bat test         # 编译 Editor + 运行自动化测试

# Linux / macOS
./build_testhost.sh             # Editor（Development）
./build_testhost.sh Game        # Game 客户端（Development）
./build_testhost.sh test        # 编译 Editor + 运行自动化测试
```

专用测试脚本，覆盖两种链接模式：

```bash
# Windows
run_testhost.bat               # 全套：Editor 编译+测试，Game 编译
run_testhost.bat editor        # 仅 Editor：编译 + 自动化测试
run_testhost.bat game          # 仅 Game：编译（静态链接验证）
run_testhost.bat --no-build    # 跳过编译，仅运行 Editor 测试

# Linux / macOS
./run_testhost.sh              # 全套：Editor 编译+测试，Game 编译
./run_testhost.sh editor       # 仅 Editor：编译 + 自动化测试
./run_testhost.sh game         # 仅 Game：编译（静态链接验证）
./run_testhost.sh --no-build   # 跳过编译，仅运行 Editor 测试
```

脚本默认查找 `../UnrealEngine`（同级目录）。如需自定义，在仓库根目录
创建 `.env` 文件（参考 `.env.example`）：

```
ENGINE_ROOT=E:\UnrealEngine
```

仓库目录结构：

```
FastBitCopy/
├── FastBitCopy.uplugin
├── README.md / README_CN.md
├── build_testhost.bat            # Windows 构建脚本
├── build_testhost.sh             # Linux/macOS 构建脚本
├── run_testhost.bat             # Windows 测试脚本（Editor + Game）
├── run_testhost.sh              # Linux/macOS 测试脚本（Editor + Game）
├── .env.example                  # ENGINE_ROOT 配置模板
├── Source/
│   ├── FastBitCopy/              # 运行时模块（安装 Hook）
│   │   ├── FastBitCopy.Build.cs
│   │   ├── Public/
│   │   │   ├── FastBitCopy.h
│   │   │   └── BitCopyFast.h
│   │   └── Private/
│   │       ├── FastBitCopyModule.cpp
│   │       ├── BitCopyFast.cpp
│   │       ├── FunctionHook.h
│   │       └── FunctionHook.cpp
│   └── FastBitCopyTests/         # 开发工具类测试模块
│       ├── FastBitCopyTests.Build.cs
│       └── Private/
│           ├── FastBitCopyTestsModule.cpp
│           └── FastBitCopyTests.cpp
├── CI/                           # 独立 CI 测试（无需 UE）
│   ├── CMakeLists.txt
│   ├── test_main.cpp
│   └── ue_shim.h
└── TestHost/                     # 可选的独立测试宿主工程
    ├── FastBitCopyHost.uproject  # 通过 AdditionalPluginDirectories: [".."] 加载
    └── Source/FastBitCopyHost/
```

`TestHost/` 仅用于插件自己的 CI / 自测，**不会**被插件构建系统编译；
作为 submodule / subtree 嵌入时可直接忽略或删除。

## 验证 Hook 是否生效

插件自带一套 Automation Tests。打开编辑器：

> `窗口 → 开发者工具 → Session Frontend → Automation`

运行以下测试：

| 测试用例                   | 作用                                              |
|----------------------------|---------------------------------------------------|
| `FastBitCopy.HookSanity`   | 断言 Hook 已安装，且导出的 `appBitsCpy` 与 `appBitsCpyFastImpl` 的输出字节完全一致。 |
| `FastBitCopy.Correctness`  | 在 10 000 组随机 `(SrcBit, DstBit, BitCount)` 上对比 Fast 实现与原始实现。 |
| `FastBitCopy.PageBound`    | 覆盖真实 OS 页边界，验证非对齐 64-bit 读不会越界。 |
| `FastBitCopy.Speed`        | 对 1B – 1KiB 各尺寸分别做 Original / Hooked / Fast 三者的基准测试。 |

也可以从命令行运行：

```bash
UnrealEditor-Cmd.exe <你的项目.uproject> -ExecCmds="Automation RunTests FastBitCopy" -unattended -NoPause
```

## API

即便在不经过 `appBitsCpy` 的代码路径里，也可以直接调用优化后的实现：

```cpp
#include "BitCopyFast.h"

void appBitsCpyFastImpl(uint8* Dest, int32 DestBit,
                        uint8* Src,  int32 SrcBit,
                        int32 BitCount);
```

查询 Hook 状态：

```cpp
#include "FastBitCopy.h"

if (FFastBitCopyModule::IsHookInstalled()) { /* ... */ }
```

## 安全说明

* Hook 仅修改 `appBitsCpy` 一个函数，不会触碰引擎中任何其他代码。
* 如果目标函数的序言因为任何原因无法安全重定位（arm64 上首指令为
  `B` / `ADRP` 等；对 `appBitsCpy` 这种叶子函数几乎不可能发生），
  Hook 会**拒绝安装**并记录一条 Warning，引擎回退到自带实现——
  游戏仍然可以正常运行，只是拿不到性能收益。
* 常规调用走的是 `appBitsCpy -> (jmp) -> appBitsCpyFastImpl`，额外开销
  只有一条 `jmp` 指令，对性能几乎无影响。

## 许可证

MIT，详见源码文件头部的版权声明。

