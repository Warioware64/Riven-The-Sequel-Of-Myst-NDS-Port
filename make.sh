#!/usr/bin/env bash
# Build the NDS ROM by running build.py inside the project's Python venv.
# Forwards any extra arguments to build.py (e.g. ./make.sh --ninja, --graph).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${SCRIPT_DIR}/env"

if [ ! -f "${VENV_DIR}/bin/activate" ]; then
    echo "ERROR: Python venv not found at ${VENV_DIR}." >&2
    echo "Run ./setup-env.sh first." >&2
    exit 1
fi

# shellcheck disable=SC1091
source "${VENV_DIR}/bin/activate"

# BlocksDS lives under the Wonderful toolchain. Export the toolchain env here
# (with fallbacks) so the build works even from shells that never sourced
# ~/.bashrc -- e.g. IDE terminals / task runners. Without BLOCKSDS, architectds
# silently defaults to /opt/blocksds/core and the ARM7 core ELF
# (arm7_dswifi_maxmod.elf) can't be found. ${VAR:-default} keeps any value an
# already-configured shell provides.
export WONDERFUL_TOOLCHAIN="${WONDERFUL_TOOLCHAIN:-/opt/wonderful}"
export BLOCKSDS="${BLOCKSDS:-${WONDERFUL_TOOLCHAIN}/thirdparty/blocksds/core}"
export BLOCKSDSEXT="${BLOCKSDSEXT:-${WONDERFUL_TOOLCHAIN}/thirdparty/blocksds/external}"
export PATH="${WONDERFUL_TOOLCHAIN}/bin:${PATH}"

cd "${SCRIPT_DIR}"
exec python build.py "$@"
