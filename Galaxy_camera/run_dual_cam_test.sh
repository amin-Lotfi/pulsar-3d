#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SAMPLE_DIR="$ROOT_DIR/sample/GxDualCamOpen"
LIB_DIR="$ROOT_DIR/lib/armv8"

SECONDS_TO_RUN="${1:-10}"
ARCH="$(uname -m)"

if [[ "${FORCE_NON_ARM:-0}" != "1" && "$ARCH" != "aarch64" ]]; then
  echo "[ERR] This script must run on ARM64 target (current: $ARCH)."
  echo "[ERR] Run it on the camera host. To bypass: FORCE_NON_ARM=1 $0 $SECONDS_TO_RUN"
  exit 1
fi

cd "$SAMPLE_DIR"
make -j"$(nproc)"

export LD_LIBRARY_PATH="$LIB_DIR:${LD_LIBRARY_PATH:-}"
exec ./GxDualCamOpen "$SECONDS_TO_RUN"
