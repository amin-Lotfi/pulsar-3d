#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${ROOT_DIR}/pulsar/main"
SDK_INC="${ROOT_DIR}/Galaxy_camera/inc"
SDK_LIB="${ROOT_DIR}/Galaxy_camera/lib/x86_64"

SOURCES=(
  "${ROOT_DIR}/pulsar/main.cpp"
  "${ROOT_DIR}/pulsar/settings.cpp"
  "${ROOT_DIR}/pulsar/camera_pipeline.cpp"
  "${ROOT_DIR}/pulsar/display_pipeline.cpp"
)

for src in "${SOURCES[@]}"; do
  if [[ ! -f "${src}" ]]; then
    echo "Error: source file not found: ${src}" >&2
    exit 1
  fi
done

if [[ ! -d "${SDK_INC}" || ! -d "${SDK_LIB}" ]]; then
  echo "Error: SDK paths not found under Galaxy_camera/" >&2
  exit 1
fi

HAVE_OPENCV=0
OPENCV_FLAGS=()
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists opencv4; then
  HAVE_OPENCV=1
  read -r -a OPENCV_FLAGS <<< "$(pkg-config --cflags --libs opencv4)"
fi

CXX_FLAGS=(-O3 -DNDEBUG -std=c++17 -pthread)
if g++ -march=native -x c++ -E /dev/null >/dev/null 2>&1; then
  CXX_FLAGS+=(-march=native -mtune=native)
fi

echo "Cleaning old build ..."
rm -f "${BIN}"

echo "Building pulsar/main ..."
g++ "${CXX_FLAGS[@]}" \
  "${SOURCES[@]}" \
  -isystem "${SDK_INC}" \
  -L"${SDK_LIB}" \
  -Wl,-rpath,"${SDK_LIB}" \
  "${OPENCV_FLAGS[@]}" \
  -lgxiapi \
  -o "${BIN}"

export LD_LIBRARY_PATH="${SDK_LIB}:${LD_LIBRARY_PATH:-}"

DEFAULT_RUN_ARGS=(
  --sync free
  --display-mode dual
  --mon0-width 1920 --mon0-height 1080 --mon0-x 0 --mon0-y 0
  --mon1-width 3840 --mon1-height 1080 --mon1-x 1920 --mon1-y 0
  --strict-pair
  --ultra-low-latency
)

RUN_ARGS=("${DEFAULT_RUN_ARGS[@]}" "$@")
HAS_NO_DISPLAY=0
for a in "${RUN_ARGS[@]}"; do
  if [[ "${a}" == "--no-display" ]]; then
    HAS_NO_DISPLAY=1
    break
  fi
done

if [[ "${HAVE_OPENCV}" -eq 0 && "${HAS_NO_DISPLAY}" -eq 0 ]]; then
  echo "OpenCV not found; running with --no-display"
  RUN_ARGS+=(--no-display)
fi

exec "${BIN}" "${RUN_ARGS[@]}"
