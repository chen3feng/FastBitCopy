#!/usr/bin/env bash
# ============================================================
#  run_testhost.sh — Build & run FastBitCopy TestHost tests (Linux / macOS)
#
#  Covers both Editor (dynamic linking) and Game (static linking) targets.
#
#  Reads ENGINE_ROOT from .env (if present), otherwise defaults
#  to ../UnrealEngine (sibling directory).
#
#  Usage:
#    ./run_testhost.sh              — Run all (Editor build+test, Game build)
#    ./run_testhost.sh editor       — Editor only: build + automation tests
#    ./run_testhost.sh game         — Game only: build (link verification)
#    ./run_testhost.sh --no-build   — Skip build, run Editor tests only
# ============================================================
set -uo pipefail
# Note: we do NOT use `set -e` because we want to continue after individual
# step failures and report a summary at the end.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---- Load .env if present ----
ENV_FILE="${SCRIPT_DIR}/.env"
if [[ -f "${ENV_FILE}" ]]; then
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

# ---- Editor command path ----
# On Mac, UE builds a separate UnrealEditor-Cmd binary (no .app bundle).
# On Linux, there is no -Cmd variant; the main UnrealEditor binary serves
# both interactive and commandlet modes.
if [[ "${PLATFORM}" == "Mac" ]]; then
    EDITOR_CMD="${ENGINE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd"
else
    EDITOR_CMD="${ENGINE_ROOT}/Engine/Binaries/Linux/UnrealEditor"
fi

# ---- Helpers ----
FAILURES=0

run_build() {
    local target_name="$1"
    local config="$2"
    local link_desc="$3"

    echo ""
    echo "============================================================"
    echo "  ENGINE_ROOT : ${ENGINE_ROOT}"
    echo "  Target      : ${target_name}"
    echo "  Platform    : ${PLATFORM}"
    echo "  Config      : ${config}"
    echo "  Link mode   : ${link_desc}"
    echo "============================================================"
    echo ""

    if bash "${RUN_UBT}" "${target_name}" "${PLATFORM}" "${config}" \
        -project="${PROJECT}"; then
        echo ""
        echo "[PASS] ${target_name} build succeeded"
        return 0
    else
        echo ""
        echo "[FAIL] ${target_name} build failed"
        FAILURES=$((FAILURES + 1))
        return 1
    fi
}

run_editor_tests() {
    if [[ ! -f "${EDITOR_CMD}" ]]; then
        echo "ERROR: UnrealEditor-Cmd not found at ${EDITOR_CMD}"
        FAILURES=$((FAILURES + 1))
        return 1
    fi

    echo ""
    echo "Running: ${EDITOR_CMD}"
    echo "  Project: ${PROJECT}"
    echo "  Filter:  FastBitCopy"
    echo ""

    if "${EDITOR_CMD}" "${PROJECT}" \
        -ExecCmds="Automation RunTests FastBitCopy" \
        -unattended -NoPause -NullRHI -log; then
        echo ""
        echo "[PASS] Editor automation tests passed"
        return 0
    else
        echo ""
        echo "[FAIL] Editor automation tests failed"
        FAILURES=$((FAILURES + 1))
        return 1
    fi
}

print_summary() {
    echo ""
    echo "############################################################"
    if [[ ${FAILURES} -eq 0 ]]; then
        echo "#  ALL PASSED"
    else
        echo "#  ${FAILURES} FAILURE(S)"
    fi
    echo "############################################################"
    echo ""
    exit "${FAILURES}"
}

# ---- Parse arguments ----
MODE="${1:-all}"
MODE="$(echo "${MODE}" | tr '[:upper:]' '[:lower:]')"  # lowercase

case "${MODE}" in
    all)
        echo ""
        echo "############################################################"
        echo "#  FastBitCopy TestHost — Full Test Suite (${PLATFORM})"
        echo "#  ENGINE_ROOT: ${ENGINE_ROOT}"
        echo "############################################################"

        # Step 1: Build Editor (dynamic linking)
        echo ""
        echo "=== [1/3] Building Editor target (dynamic linking) ==="
        if run_build "FastBitCopyHostEditor" "Development" "dynamic (modular)"; then
            # Step 2: Run Editor automation tests
            echo ""
            echo "=== [2/3] Running Editor automation tests ==="
            run_editor_tests
        else
            echo ""
            echo "[SKIP] Skipping Editor tests due to build failure"
        fi

        # Step 3: Build Game (static linking)
        echo ""
        echo "=== [3/3] Building Game target (static linking) ==="
        run_build "FastBitCopyHost" "Development" "static (monolithic)"

        print_summary
        ;;

    editor)
        echo ""
        echo "############################################################"
        echo "#  FastBitCopy TestHost — Editor Tests (${PLATFORM})"
        echo "#  ENGINE_ROOT: ${ENGINE_ROOT}"
        echo "############################################################"

        echo ""
        echo "=== [1/2] Building Editor target (dynamic linking) ==="
        if ! run_build "FastBitCopyHostEditor" "Development" "dynamic (modular)"; then
            echo ""
            echo "[FAIL] Editor build failed — cannot run tests."
            print_summary
        fi

        echo ""
        echo "=== [2/2] Running Editor automation tests ==="
        run_editor_tests

        print_summary
        ;;

    game)
        echo ""
        echo "############################################################"
        echo "#  FastBitCopy TestHost — Game Build (${PLATFORM})"
        echo "#  ENGINE_ROOT: ${ENGINE_ROOT}"
        echo "############################################################"

        echo ""
        echo "=== [1/1] Building Game target (static linking) ==="
        run_build "FastBitCopyHost" "Development" "static (monolithic)"

        print_summary
        ;;

    --no-build)
        echo ""
        echo "############################################################"
        echo "#  FastBitCopy TestHost — Run Tests Only (${PLATFORM})"
        echo "#  ENGINE_ROOT: ${ENGINE_ROOT}"
        echo "############################################################"

        echo ""
        echo "=== [1/1] Running Editor automation tests (skip build) ==="
        run_editor_tests

        print_summary
        ;;

    *)
        echo "ERROR: Unknown mode: ${MODE}"
        echo "Usage: $0 [all|editor|game|--no-build]"
        exit 1
        ;;
esac
