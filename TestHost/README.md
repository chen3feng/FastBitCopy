# FastBitCopyHost — standalone test project

This is a minimal UE project whose only purpose is to host the
`FastBitCopy` plugin and run its automation tests.

## How it finds the plugin

The plugin source lives at the **repository root** (one level above this
folder). `FastBitCopyHost.uproject` uses UE's built-in
`AdditionalPluginDirectories` field to pick it up automatically:

```json
"AdditionalPluginDirectories": [ ".." ]
```

No symlink, no copy — just open the `.uproject` and build.

## Build & run the tests

```bash
# generate project files + build (example on Windows)
"<UE>/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" \
    -projectfiles -project="$(pwd)/FastBitCopyHost.uproject" -game -rocket -progress

# open in the editor
"<UE>/Engine/Binaries/Win64/UnrealEditor.exe" "$(pwd)/FastBitCopyHost.uproject"
```

Then in the editor:

* `Window → Developer Tools → Session Frontend → Automation`
* filter by `FastBitCopy` and run.

Or headless:

```bash
UnrealEditor-Cmd.exe FastBitCopyHost.uproject \
    -ExecCmds="Automation RunTests FastBitCopy;quit" \
    -unattended -NoPause -NullRHI
```
