# FastBitCopy — Unreal Engine plugin

[![CI](https://github.com/chen3feng/FastBitCopy/actions/workflows/ci.yml/badge.svg)](https://github.com/chen3feng/FastBitCopy/actions/workflows/ci.yml)

[English](README.md) | [中文](README_CN.md)
**FastBitCopy** replaces UE's `appBitsCpy` (the bit-copy routine used by
`FBitReader` / `FBitWriter`, replication, networking, serialization, …) with
a drop-in optimized implementation **at runtime**, via a cross-platform
function hook. No engine modification required.

## Why

UE's stock `appBitsCpy` is a scalar byte-at-a-time implementation. On both
x86-64 and arm64 we can do much better with aligned 64-bit loads/stores plus
`memcpy` for middle bytes:

| Copy type   | Stock `appBitsCpy` | `appBitsCpyFastImpl` | Speed-up |
|-------------|-------------------:|---------------------:|---------:|
| Aligned     | 1743817 ns (1 MiB) |             60595 ns |   ~30×   |
| Unaligned   | 1776341 ns (1 MiB) |            325179 ns |    ~5×   |

(Numbers from the original benchmark — exact figures depend on platform.)

## How it works

1. The `FastBitCopy` runtime module loads at `PostConfigInit`, before any
   gameplay code runs.
2. It takes the address of UE's exported `appBitsCpy` symbol.
3. A tiny cross-platform **inline hook** is installed:
   * **x86-64** — 5-byte `E9 rel32` near-jump (or 14-byte absolute jump if
     the detour is farther than ±2 GiB). The displaced prologue bytes are
     decoded by a minimal length engine and relocated into a page-aligned
     executable **trampoline**, followed by an absolute jump back into the
     original function; this leaves the original callable.
   * **arm64** — 16-byte `LDR X16,#8 ; BR X16 ; <imm64>` absolute jump.
     The first 4 displaced instructions are copied to the trampoline (the
     implementation rejects any PC-relative instruction in the prologue so
     relocation is always safe) followed by the same LDR/BR back.
4. Page permissions are flipped with `VirtualProtect` on Windows and
   `mprotect` on Linux/macOS. On Apple Silicon the code uses
   `MAP_JIT` + `pthread_jit_write_protect_np` to satisfy W^X.
5. The instruction cache is flushed (`FlushInstructionCache`,
   `sys_icache_invalidate`, `__builtin___clear_cache`) on every touched page.
6. On `ShutdownModule` the original bytes are restored and the trampoline
   page is freed, so the engine is left pristine.

## Supported platforms

|            | Windows | Linux | macOS |
|------------|:-------:|:-----:|:-----:|
| **x86-64** |   ✅    |  ✅   |  ✅   |
| **arm64**  |   ✅    |  ✅   |  ✅ (Apple Silicon) |

Requires a little-endian CPU that allows unaligned loads
(`PLATFORM_LITTLE_ENDIAN && PLATFORM_SUPPORTS_UNALIGNED_LOADS`). Both x64
and arm64 qualify.

> **Cross-platform correctness.** Both the byte-aligned and bit-unaligned
> fast paths are compiled and exercised on all three host OSes in CI —
> standalone `-O2` (MSVC, GCC, Clang), an additional `-O0` sweep on
> Linux/macOS, and an UBSan+ASan run on Linux — and each covers the
> 10 000-trial randomized correctness suite plus the page-boundary /
> deterministic edge cases. The speed-up figures in the table above were
> measured on Windows (MSVC); absolute numbers on Linux/macOS will vary
> with compiler and host CPU, but the implementation path is the same.
>
> History note: issue
> [#2](https://github.com/chen3feng/FastBitCopy/issues/2) tracked an
> earlier `-O2` divergence on GCC/Clang that was resolved before the
> standalone CI job became blocking. The root causes were a
> dangling-reference bug in the CI shim's `FMath::Min`/`Max` (PR #6)
> and an alignment-UB in `CopyBitsSrcAligned` (PR #7). The per-function
> `optnone`/`optimize("O0")` override and the `noinline` attribute have
> both been removed (PRs #9 and #10). One lightweight defensive barrier
> remains — an inline-asm `"memory"` compiler barrier between the
> leading-byte `uint8*` fixup and the `uint64*` bulk copy — and will be
> removed in a follow-up PR once verified.

## Installation

The plugin folder **must** be named `FastBitCopy` (matching `FastBitCopy.uplugin`).
Pick whichever method you prefer:

### A. Plain `git clone`

```bash
cd YourGame/Plugins
git clone https://github.com/chen3feng/FastBitCopy.git FastBitCopy
```

### B. As a git submodule (recommended for teams)

```bash
cd YourGame
git submodule add https://github.com/chen3feng/FastBitCopy.git Plugins/FastBitCopy
git submodule update --init --recursive
```

### C. As a git subtree (vendored into your repo)

```bash
cd YourGame
git subtree add --prefix=Plugins/FastBitCopy \
    https://github.com/chen3feng/FastBitCopy.git main --squash
# later, to pull upstream updates:
git subtree pull --prefix=Plugins/FastBitCopy \
    https://github.com/chen3feng/FastBitCopy.git main --squash
```

### D. Manual download

Download the repo as a zip, extract, rename the top-level folder to
`FastBitCopy`, and drop it into `YourGame/Plugins/`.

Then regenerate project files and rebuild — that's it. The hook installs
itself during engine initialization; there are no APIs to call.

Repository layout:

```
FastBitCopy/
├── FastBitCopy.uplugin
├── README.md / README_CN.md
├── Source/
│   ├── FastBitCopy/              # runtime module (installs the hook)
│   │   ├── FastBitCopy.Build.cs
│   │   ├── Public/
│   │   │   ├── FastBitCopy.h
│   │   │   └── BitCopyFast.h
│   │   └── Private/
│   │       ├── FastBitCopyModule.cpp
│   │       ├── BitCopyFast.cpp
│   │       ├── FunctionHook.h
│   │       └── FunctionHook.cpp
│   └── FastBitCopyTests/         # developer-tool test module
│       ├── FastBitCopyTests.Build.cs
│       └── Private/
│           ├── FastBitCopyTestsModule.cpp
│           └── FastBitCopyTests.cpp
└── TestHost/                     # optional standalone test project
    ├── FastBitCopyHost.uproject  # uses AdditionalPluginDirectories: [".."]
    └── Source/FastBitCopyHost/
```

The `TestHost/` folder is purely for the plugin's own CI/tests — it is
**not** compiled as part of the plugin and you can ignore/delete it in a
submodule/subtree consumption.

## Verifying the hook

The plugin ships with an automation test suite. Inside the editor:

> `Window → Developer Tools → Session Frontend → Automation`

and run:

| Test                      | Purpose                                          |
|---------------------------|--------------------------------------------------|
| `FastBitCopy.HookSanity`  | Asserts the hook is installed and that the exported `appBitsCpy` now produces the same output as `appBitsCpyFastImpl`. |
| `FastBitCopy.Correctness` | Compares the fast implementation against a verbatim copy of the original across 10 000 random `(SrcBit, DstBit, BitCount)` triples. |
| `FastBitCopy.PageBound`   | Exercises the unaligned-64-bit-load path across real OS page boundaries. |
| `FastBitCopy.Speed`       | Benchmarks Original vs Hooked vs Direct-Fast across sizes 1 B – 1 KiB. |

You can also run them from the command line:

```bash
UnrealEditor-Cmd.exe <YourProject.uproject> -ExecCmds="Automation RunTests FastBitCopy" -unattended -NoPause
```

## API

The plugin also exposes the optimized primitive directly, so you can call
it even in places that don't go through `appBitsCpy`:

```cpp
#include "BitCopyFast.h"

void appBitsCpyFastImpl(uint8* Dest, int32 DestBit,
                        uint8* Src,  int32 SrcBit,
                        int32 BitCount);
```

And a query for the hook state:

```cpp
#include "FastBitCopy.h"

if (FFastBitCopyModule::IsHookInstalled()) { /* ... */ }
```

## Safety notes

* The hook only patches `appBitsCpy`. Nothing else in the engine is touched.
* If for some reason the target prologue cannot be safely relocated (e.g.
  on arm64 the very first instruction is `B` or `ADRP`, which is almost
  never the case for a leaf helper like `appBitsCpy`), installation bails
  out and the stock implementation is used — your game still runs, just
  without the speed-up.
* The trampoline is only used for benchmarking / debugging; everyday
  replication callers go straight through the patched `appBitsCpy` to
  `appBitsCpyFastImpl` with a single extra `jmp` of overhead.

## License

MIT. See the copyright headers in the source files.

