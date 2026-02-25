#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${ROOT_DIR}/pulsar/main"

TARGET_ARCH="${TARGET_ARCH:-$(uname -m)}"
case "${TARGET_ARCH}" in
  aarch64|arm64)
    IS_ARM64=1
    SDK_ROOT_DEFAULT="${ROOT_DIR}/Galaxy_camera_arm64"
    SDK_LIB_SUBDIR_DEFAULT="armv8"
    ;;
  x86_64|amd64)
    IS_ARM64=0
    SDK_ROOT_DEFAULT="${ROOT_DIR}/Galaxy_camera_amd"
    SDK_LIB_SUBDIR_DEFAULT="x86_64"
    ;;
  *)
    echo "Error: unsupported architecture '${TARGET_ARCH}'. Set TARGET_ARCH or GALAXY_SDK_ROOT manually." >&2
    exit 1
    ;;
esac

SDK_ROOT="${GALAXY_SDK_ROOT:-${SDK_ROOT_DEFAULT}}"
SDK_INC="${SDK_ROOT}/inc"
if [[ -n "${GALAXY_SDK_LIB_SUBDIR:-}" ]]; then
  SDK_LIB_SUBDIR="${GALAXY_SDK_LIB_SUBDIR}"
else
  SDK_LIB_SUBDIR="${SDK_LIB_SUBDIR_DEFAULT}"
  if [[ ! -d "${SDK_ROOT}/lib/${SDK_LIB_SUBDIR}" ]]; then
    if [[ "${IS_ARM64}" -eq 1 ]]; then
      CANDIDATE_SUBDIRS=(armv8 aarch64 arm64)
    else
      CANDIDATE_SUBDIRS=(x86_64 amd64)
    fi
    for candidate in "${CANDIDATE_SUBDIRS[@]}"; do
      if [[ -d "${SDK_ROOT}/lib/${candidate}" ]]; then
        SDK_LIB_SUBDIR="${candidate}"
        break
      fi
    done
  fi
fi
SDK_LIB="${SDK_ROOT}/lib/${SDK_LIB_SUBDIR}"
SDK_CONFIG_FILE="${SDK_ROOT}/config/log4cplus.properties"

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
  echo "Error: SDK paths not found." >&2
  echo "  SDK include: ${SDK_INC}" >&2
  echo "  SDK lib:     ${SDK_LIB}" >&2
  echo "Set GALAXY_SDK_ROOT and optionally GALAXY_SDK_LIB_SUBDIR if your SDK path is different." >&2
  exit 1
fi

echo "Using camera SDK: ${SDK_ROOT}"
echo "Using camera SDK libs: ${SDK_LIB}"

if [[ -f "${SDK_CONFIG_FILE}" && -z "${LOG4CPLUS_CONFIGURATION:-}" ]]; then
  export LOG4CPLUS_CONFIGURATION="${SDK_CONFIG_FILE}"
fi

if [[ ! -f /etc/Galaxy/cfg/log4cplus.properties ]]; then
  echo "Warning: /etc/Galaxy/cfg/log4cplus.properties not found." >&2
  echo "         If camera open fails on Jetson, run installer once:" >&2
  echo "         sudo ${SDK_ROOT}/Galaxy_camera.run" >&2
fi

HAVE_OPENCV=0
OPENCV_FLAGS=()
OPENCV_PKG=""
if command -v pkg-config >/dev/null 2>&1; then
  if pkg-config --exists opencv4; then
    OPENCV_PKG="opencv4"
  elif pkg-config --exists opencv; then
    OPENCV_PKG="opencv"
  fi
fi
if [[ -n "${OPENCV_PKG}" ]]; then
  HAVE_OPENCV=1
  read -r -a OPENCV_FLAGS <<< "$(pkg-config --cflags --libs "${OPENCV_PKG}")"
fi
CPP_DEFINES=(-DPULSAR_ENABLE_OPENCV="${HAVE_OPENCV}")

CXX_FLAGS=(-O3 -DNDEBUG -std=c++17 -pthread)
if g++ -march=native -x c++ -E /dev/null >/dev/null 2>&1; then
  CXX_FLAGS+=(-march=native -mtune=native)
fi

echo "Cleaning old build ..."
rm -f "${BIN}"

echo "Building pulsar/main ..."
g++ "${CXX_FLAGS[@]}" \
  "${CPP_DEFINES[@]}" \
  "${SOURCES[@]}" \
  -isystem "${SDK_INC}" \
  -L"${SDK_LIB}" \
  -Wl,-rpath,"${SDK_LIB}" \
  "${OPENCV_FLAGS[@]}" \
  -lgxiapi \
  -o "${BIN}"

export LD_LIBRARY_PATH="${SDK_LIB}:${LD_LIBRARY_PATH:-}"

if [[ "${IS_ARM64}" -eq 1 ]]; then
  DEFAULT_RUN_ARGS=(
    --sync free
    --display-mode single
    --mon0-width 1920 --mon0-height 1080 --mon0-x 0 --mon0-y 0
    --strict-pair
    --ultra-low-latency
  )
else
  DEFAULT_RUN_ARGS=(
    --sync free
    --display-mode dual
    --mon0-width 1920 --mon0-height 1080 --mon0-x 0 --mon0-y 0
    --mon1-width 3840 --mon1-height 1080 --mon1-x 1920 --mon1-y 0
    --strict-pair
    --ultra-low-latency
  )
fi

RUN_ARGS=("${DEFAULT_RUN_ARGS[@]}" "$@")
HAS_NO_DISPLAY=0
for a in "${RUN_ARGS[@]}"; do
  if [[ "${a}" == "--no-display" ]]; then
    HAS_NO_DISPLAY=1
    break
  fi
done

if [[ "${HAVE_OPENCV}" -eq 0 && "${HAS_NO_DISPLAY}" -eq 0 ]]; then
  echo "OpenCV dev package not found via pkg-config; running with --no-display"
  RUN_ARGS+=(--no-display)
  HAS_NO_DISPLAY=1
fi

if [[ "${HAS_NO_DISPLAY}" -eq 0 && -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "No active GUI session detected (DISPLAY/WAYLAND_DISPLAY is empty); running with --no-display"
  RUN_ARGS+=(--no-display)
fi

if [[ "${IS_ARM64}" -eq 1 ]]; then
  echo "Tip (Jetson):"
  echo "  1) sudo ${SDK_ROOT}/Galaxy_camera.run"
  echo "  2) sudo nvpmodel -m 0 && sudo jetson_clocks"
  echo "  3) sudo ${SDK_ROOT}/SetUSBStack.sh   # for USB cameras"
fi

exec "${BIN}" "${RUN_ARGS[@]}"
