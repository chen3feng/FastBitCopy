<!-- Thanks for the PR! Please fill in the sections below. -->

## What / Why

<!-- One-paragraph description of what this change does and why. -->

## How to test

<!-- Steps a reviewer can follow to verify. If a new Automation test was
     added, mention its name. -->

## Checklist

- [ ] CI is green (`CI / Standalone test` on all 3 OSes + `arm64 cross-build`).
- [ ] If the bit-copy algorithm changed, the correctness harness in `CI/` still passes.
- [ ] Tested in the `TestHost/` project with `Window → Developer Tools → Session Frontend → Automation → FastBitCopy.*`.
- [ ] No UE version-specific headers were introduced that break the supported UE range.
- [ ] `README.md` / `README_CN.md` updated if behavior or public API changed.

## Related issues

Closes #
