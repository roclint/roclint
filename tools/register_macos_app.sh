#!/usr/bin/env bash
# Developer deployment helper for macOS. Everyday launch uses Roclint.app / Spotlight;
# this does not claim to be a signed, clean-machine release installer.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGED_BUNDLE=""
INSTALL_DIR="${HOME}/Applications"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --staged-bundle)
            STAGED_BUNDLE="$2"
            shift 2
            ;;
        --install-dir)
            INSTALL_DIR="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: register_macos_app.sh --staged-bundle <path> [--install-dir <path>]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "${STAGED_BUNDLE}" ]]; then
    echo "Error: --staged-bundle is required." >&2
    exit 1
fi

STAGED_BUNDLE="$(cd "${STAGED_BUNDLE}" && pwd)"
TARGET_APP="${INSTALL_DIR}/Roclint.app"

if [[ "${STAGED_BUNDLE}" == "${TARGET_APP}" ]]; then
    echo "Error: The staging and installation directories must differ." >&2
    exit 1
fi

# Validate staged bundle contents
if [[ ! -d "${STAGED_BUNDLE}" ]]; then
    echo "Error: Staged bundle directory does not exist: ${STAGED_BUNDLE}" >&2
    exit 1
fi

EXECUTABLE="${STAGED_BUNDLE}/Contents/MacOS/Roclint"
if [[ ! -f "${EXECUTABLE}" ]]; then
    EXECUTABLE="${STAGED_BUNDLE}/Contents/MacOS/roclint_host"
fi

if [[ ! -f "${EXECUTABLE}" ]]; then
    echo "Error: Missing main executable in ${STAGED_BUNDLE}/Contents/MacOS" >&2
    exit 1
fi

for required in "Contents/MacOS/runtime/roclint_granite_worker" "Contents/MacOS/runtime/julia_worker.py"; do
    if [[ ! -f "${STAGED_BUNDLE}/${required}" ]]; then
        echo "Error: Incomplete staging bundle: ${required} is missing." >&2
        exit 1
    fi
done

# Ensure any existing installed instance is closed
INSTALLED_EXEC="${TARGET_APP}/Contents/MacOS/Roclint"
if pgrep -f "${INSTALLED_EXEC}" >/dev/null 2>&1; then
    echo "Error: Roclint is currently running. Quit Roclint from its tray menu before updating." >&2
    exit 1
fi

# Stage application bundle
mkdir -p "${INSTALL_DIR}"
rm -rf "${TARGET_APP}"
cp -R "${STAGED_BUNDLE}" "${TARGET_APP}"

# Stage Python runtime for Julia if available
if [[ ! -d "${TARGET_APP}/Contents/MacOS/runtime/julia-python" ]]; then
    if [[ -d "${STAGED_BUNDLE}/Contents/MacOS/runtime/julia-python" ]]; then
        cp -R -c "${STAGED_BUNDLE}/Contents/MacOS/runtime/julia-python" "${TARGET_APP}/Contents/MacOS/runtime/julia-python" 2>/dev/null || \
        cp -R "${STAGED_BUNDLE}/Contents/MacOS/runtime/julia-python" "${TARGET_APP}/Contents/MacOS/runtime/julia-python"
    elif [[ -d "${SCRIPT_DIR}/../build/runtime/julia-python" ]]; then
        cp -R -c "${SCRIPT_DIR}/../build/runtime/julia-python" "${TARGET_APP}/Contents/MacOS/runtime/julia-python" 2>/dev/null || \
        cp -R "${SCRIPT_DIR}/../build/runtime/julia-python" "${TARGET_APP}/Contents/MacOS/runtime/julia-python"
    fi
fi

# Stage license files in Contents/Resources/licenses
LICENSES_DIR="${TARGET_APP}/Contents/Resources/licenses"
mkdir -p "${LICENSES_DIR}"
if [[ -f "${SCRIPT_DIR}/../app/qml/shell/assets/Inter-LICENSE.txt" ]]; then
    cp -f "${SCRIPT_DIR}/../app/qml/shell/assets/Inter-LICENSE.txt" "${LICENSES_DIR}/Inter-OFL.txt"
fi
if [[ -f "${SCRIPT_DIR}/../app/qml/shell/assets/icons/LICENSE" ]]; then
    cp -f "${SCRIPT_DIR}/../app/qml/shell/assets/icons/LICENSE" "${LICENSES_DIR}/Phosphor-MIT.txt"
fi

# Apply ad-hoc code signature
codesign --force --deep --sign - "${TARGET_APP}" >/dev/null 2>&1 || true

# Register with LaunchServices and Spotlight
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
if [[ -x "${LSREGISTER}" ]]; then
    "${LSREGISTER}" -f "${TARGET_APP}" >/dev/null 2>&1 || true
fi
if command -v mdimport >/dev/null 2>&1; then
    mdimport "${TARGET_APP}" >/dev/null 2>&1 || true
fi

cat <<EOF
{
  "Bundle": "${TARGET_APP}",
  "Executable": "${TARGET_APP}/Contents/MacOS/$(basename "${EXECUTABLE}")",
  "ReleaseInstaller": false
}
EOF
