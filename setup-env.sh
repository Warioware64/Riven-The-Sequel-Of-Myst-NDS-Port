#!/usr/bin/env bash
# Create a local Python virtual environment in ./env and install the modified
# architectds package (architectds_nea_mod) into it in editable mode.
#
# Unlike the Myst port, this venv is ONLY for building the ROM. The asset
# converter is a C++ program (tools/riven-convert) built with CMake, so there
# are no Python dependencies to install for it here. It does need ffmpeg on the
# PATH to convert the movies -- a run-time tool, not a build-time one, and not
# something this script installs.
#
# Usage:
#   ./setup-env.sh
#   source env/bin/activate
#   python build.py

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${SCRIPT_DIR}/env"
PKG_DIR="${SCRIPT_DIR}/architectds_nea_mod"

if [ ! -d "${PKG_DIR}" ]; then
    echo "ERROR: architectds_nea_mod not found at ${PKG_DIR}" >&2
    exit 1
fi

if [ ! -d "${VENV_DIR}" ]; then
    echo "Creating virtual environment at ${VENV_DIR}"
    python3 -m venv "${VENV_DIR}"
fi

# shellcheck disable=SC1091
source "${VENV_DIR}/bin/activate"

python -m pip install --upgrade pip
python -m pip install -e "${PKG_DIR}"
python -m pip install ninja

# nitro-engine-advanced's img2ds tool imports Pillow. The ROM build only needs
# it if a resource actually goes through img2ds, but installing it costs little
# and turns a confusing ImportError into a non-event.
python -m pip install pillow

echo
echo "Done. Activate the environment with:"
echo "    source env/bin/activate"
echo
echo "The asset converter is built separately:"
echo "    cmake -S tools/riven-convert -B build/convert && cmake --build build/convert"
