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

TARGET_TYPE_LOWER="$(echo "${TARGET_TYPE}" | tr '[:upper:]' '[:lower:]')"

if [[ "${TARGET_TYPE_LOWER}" == "test" ]]; then
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

    if [[ ! -x "${EDITOR_CMD}" ]]; then
        if [[ "${PLATFORM}" == "Mac" ]]; then
            echo "ERROR: UnrealEditor-Cmd not found or not executable at ${EDITOR_CMD}"
        else
            echo "ERROR: UnrealEditor not found or not executable at ${EDITOR_CMD}"
        fi
        exit 1
    fi

    # Test report export dir (useful for CI artifacts)
    REPORT_DIR="${SCRIPT_DIR}/TestHost/Saved/Automation/Reports"
    mkdir -p "${REPORT_DIR}"

    # Common flags for headless automation.
    #
    # We run tests via the Automation *commandlet* (-Run=Automation) rather than
    # -ExecCmds=. The commandlet path drives UCommandlet::Main and never enters
    # FEngineLoop::Tick / Slate tick / the Cocoa main-menu sync loop. This is
    # what avoids the known macOS crash inside
    #   FSlateMacMenu::UpdateWithMultiBox
    # (an async NSRunLoop source0 block firing on a TSharedPtr<FMultiBox> that
    # has already been destroyed during editor shutdown). The crash is a UE
    # engine-side issue, not a FastBitCopy issue; -Run=Automation sidesteps it
    # entirely because the menu-sync path is never armed.
    #
    # -NullRHI / -nosound / -nosplash keep us off all UI/audio subsystems.
    # -nop4 / -NoSourceControl stop the editor from trying Perforce/Git login.
    # -unattended / -NoPause keep the process non-interactive on CI.
    COMMON_FLAGS=(
        -unattended
        -NoPause
        -NullRHI
        -nosound
        -nosplash
        -nop4
        -NoSourceControl
        -log
    )

    # On macOS, also skip Slate renderer construction. Combined with the
    # commandlet launch path above this gives belt-and-braces protection
    # against FSlateMacMenu touching any TSharedPtr owned by the editor UI.
    if [[ "${PLATFORM}" == "Mac" ]]; then
        COMMON_FLAGS+=( -NoSlateRenderer )
    fi

    set +e
    "${EDITOR_CMD}" "${PROJECT}" \
        -Run=Automation \
        -TestCmds="RunTests FastBitCopy" \
        -ReportExportPath="${REPORT_DIR}" \
        "${COMMON_FLAGS[@]}"
    TEST_EXIT=$?
    set -e

    if [[ ${TEST_EXIT} -ne 0 ]]; then
        echo ""
        echo "TESTS FAILED (exit code ${TEST_EXIT})"
        echo "Report dir: ${REPORT_DIR}"
        exit ${TEST_EXIT}
    fi

    echo ""
    echo "ALL TESTS PASSED"
    echo "Report dir: ${REPORT_DIR}"
    exit 0
fi

# ---- Normal build ----
if [[ "${TARGET_TYPE_LOWER}" == "editor" ]]; then
    TARGET_NAME="FastBitCopyHostEditor"
elif [[ "${TARGET_TYPE_LOWER}" == "game" ]]; then
    TARGET_NAME="FastBitCopyHost"
else
    echo "ERROR: Unknown target type: ${TARGET_TYPE}"
    echo "Usage: $0 [Editor|Game|test] [Development|Shipping]"
    exit 1
fi

run_build "${TARGET_NAME}" "${CONFIG}"

echo ""
echo "BUILD SUCCEEDED"
