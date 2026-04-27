# CI – Standalone correctness & micro-benchmark harness

This directory is **not** part of the Unreal Engine plugin. It exists only so
that GitHub Actions can, on every push/PR, quickly validate that
`BitCopyFast.cpp` still:

1. Produces bit-for-bit identical output to the original `appBitsCpy`, across
   20 000 random `(DestBit, SrcBit, BitCount)` combinations.
2. Is actually faster than the original (a tiny micro-benchmark is printed).

It achieves this by:

- Providing a **UE type shim** ([`ue_shim.h`](./ue_shim.h)) that defines
  `uint8/int32/FMath/FMemory/FORCEINLINE/CORE_API/…` with just `<cstdint>` +
  `<algorithm>`.
- Providing empty stub headers under [`compat/`](./compat/) so that the
  plugin source's `#include "CoreMinimal.h"` / `#include "Math/UnrealMathUtility.h"`
  resolve.
- Force-including `ue_shim.h` via compiler flags (`/FI` on MSVC,
  `-include` on GCC/Clang), so **no change to the plugin source is required**.

## Build locally

```bash
cmake -S CI -B CI/build -DCMAKE_BUILD_TYPE=Release
cmake --build CI/build --config Release
ctest --test-dir CI/build --output-on-failure
```

Typical output:

```
[ OK ] correctness: 20000 random trials passed
[bench] aligned (SrcBit=DestBit=3), 8192 bits/call, 20000 iters
  Original appBitsCpy           xxxx.x ns/op
  appBitsCpyFastImpl              yy.y ns/op
[bench] unaligned (SrcBit=1 DestBit=5), 8192 bits/call, 20000 iters
  Original appBitsCpy           xxxx.x ns/op
  appBitsCpyFastImpl             yyy.y ns/op
```

## Why not compile the real plugin in CI?

Because a full UE checkout weighs **70–100 GB** and takes an hour+ to compile,
which is well over what GitHub-hosted runners can do. This harness
verifies the algorithm – the part we actually author – on every commit,
across Windows / Linux / macOS and x64 / arm64, in a few seconds.

End-to-end plugin integration is covered by the Automation test suite in
[`Source/FastBitCopyTests/`](../Source/FastBitCopyTests/), which runs inside
the `TestHost` project on developer machines.
