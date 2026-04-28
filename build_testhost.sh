#!/usr/bin/env bash
# ============================================================
#  build_testhost.sh — Build & test FastBitCopy TestHost (Linux / macOS)
#
#  Reads ENGINE_ROOT from .env (if present), otherwise defaults
#  to ../UnrealEngine (sibling directory).
#
#  Usage:
#    ./build_testhost.sh                     — Editor (Development)
#    ./build_testhost.sh Game                — Game client (Development)
#    ./build_testhost.sh Editor Shipping     — Editor (Shipping)
#    ./build_testhost.sh Game Shipping       — Game client (Shipping)
#    ./build_testhost.sh test                — Build Editor + run automation tests
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---- Load .env if present ----
ENV_FILE="${SCRIPT_DIR}/.env"
if [[ -f "${ENV_FILE}" ]]; then
    # Source only KEY=VALUE lines, skip comments
    set -a
    # shellcheck disable=SC1090
    source <(grep -E '^\s*[A-Za-z_][A-Za-z_0-9]*=' "${ENV_FILE}" | sed 's/\s*#.*//')
    set +a
fi

# ---- Default ENGINE_ROOT to sibling UnrealEngine ----
ENGINE_ROOT="${ENGINE_ROOT:-${SCRIPT_DIR}/../UnrealEngine}"

# ---- Resolve to absolute path ----
if [[ ! -d "${ENGINE_ROOT}" ]]; then
    echo "ERROR: ENGINE_ROOT directory not found: ${ENGINE_ROOT}"
    echo ""
    echo "Set ENGINE_ROOT in .env or ensure UnrealEngine is a sibling directory."
    exit 1
fi
ENGINE_ROOT="$(cd "${ENGINE_ROOT}" && pwd)"

# ---- Detect platform ----
case "$(uname -s)" in
    Linux*)  PLATFORM="Linux";;
    Darwin*) PLATFORM="Mac";;
    *)
        echo "ERROR: Unsupported OS: $(uname -s)"
        exit 1
        ;;
esac

RUN_UBT="${ENGINE_ROOT}/Engine/Build/BatchFiles/RunUBT.sh"
if [[ ! -f "${RUN_UBT}" ]]; then
    echo "ERROR: RunUBT.sh not found at ${RUN_UBT}"
    exit 1
fi

PROJECT="${SCRIPT_DIR}/TestHost/FastBitCopyHost.uproject"
if [[ ! -f "${PROJECT}" ]]; then
    echo "ERROR: TestHost project not found at ${PROJECT}"
    exit 1
fi

# ---- Ensure .plugin_root symlink exists ----
PLUGIN_ROOT="${SCRIPT_DIR}/TestHost/.plugin_root"
if [[ ! -e "${PLUGIN_ROOT}/FastBitCopy.uplugin" ]]; then
    rm -rf "${PLUGIN_ROOT}"
    ln -s "${SCRIPT_DIR}" "${PLUGIN_ROOT}"
    echo "Created symlink: ${PLUGIN_ROOT} -> ${SCRIPT_DIR}"
fi

# ---- Parse arguments ----
TARGET_TYPE="${1:-Editor}"
CONFIG="${2:-Development}"

run_build() {
    local target_name="$1"
    local config="$2"

    echo ""
    echo "============================================================"
    echo "  ENGINE_ROOT : ${ENGINE_ROOT}"
    echo "  Target      : ${target_name}"
    echo "  Platform    : ${PLATFORM}"
    echo "  Config      : ${config}"
    echo "============================================================"
    echo ""

    bash "${RUN_UBT}" "${target_name}" "${PLATFORM}" "${config}" \
        -project="${PROJECT}"
}

if [[ "${TARGET_TYPE,,}" == "test" ]]; then
    # ---- Build Editor first ----
    echo ""
    echo "=== Step 1/2: Building Editor target ==="
    echo ""
    run_build "FastBitCopyHostEditor" "Development"

    # ---- Run automation tests ----
    echo ""
    echo "=== Step 2/2: Running automation tests ==="
    echo ""
    # On Mac, UE builds a separate UnrealEditor-Cmd binary (no .app bundle).
    # On Linux, there is no -Cmd variant; the main UnrealEditor binary serves
    # both interactive and commandlet modes.
    if [[ "${PLATFORM}" == "Mac" ]]; then
        EDITOR_CMD="${ENGINE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd"
    else
        EDITOR_CMD="${ENGINE_ROOT}/Engine/Binaries/Linux/UnrealEditor"
    fi

    if [[ ! -f "${EDITOR_CMD}" ]]; then
        echo "ERROR: UnrealEditor-Cmd not found at ${EDITOR_CMD}"
        exit 1
    fi

    "${EDITOR_CMD}" "${PROJECT}" \
        -ExecCmds="Automation RunTests FastBitCopy" \
        -unattended -NoPause -NullRHI -log

    echo ""
    echo "ALL TESTS PASSED"
    exit 0
fi

# ---- Normal build ----
if [[ "${TARGET_TYPE,,}" == "editor" ]]; then
    TARGET_NAME="FastBitCopyHostEditor"
elif [[ "${TARGET_TYPE,,}" == "game" ]]; then
    TARGET_NAME="FastBitCopyHost"
else
    echo "ERROR: Unknown target type: ${TARGET_TYPE}"
    echo "Usage: $0 [Editor|Game|test] [Development|Shipping]"
    exit 1
fi

run_build "${TARGET_NAME}" "${CONFIG}"

echo ""
echo "BUILD SUCCEEDED"
