#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
SECONDS_TO_RUN="${1:-0}"
TRIGGER_HZ="${2:-60}"
ARCH="$(uname -m)"

if [[ "${FORCE_NON_ARM:-0}" != "1" && "$ARCH" != "aarch64" ]]; then
  echo "[ERR] This project must run on ARM64 Jetson (current: $ARCH)."
  echo "[ERR] Run it on Jetson. To bypass: FORCE_NON_ARM=1 ./run.sh $SECONDS_TO_RUN $TRIGGER_HZ"
  exit 1
fi

if [[ -z "${DISPLAY:-}" && -n "${PULSAR_DISPLAY:-}" ]]; then
  export DISPLAY="$PULSAR_DISPLAY"
elif [[ -z "${DISPLAY:-}" ]]; then
  export DISPLAY=:0
fi

cd "$ROOT_DIR/pulsar"
make -j"$(nproc)"

export LD_LIBRARY_PATH="$ROOT_DIR/Galaxy_camera/lib/armv8:${LD_LIBRARY_PATH:-}"
exec ./pulsar_rt "$SECONDS_TO_RUN" "$TRIGGER_HZ"
