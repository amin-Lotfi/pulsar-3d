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

RUNTIME_DIR="${ROOT_DIR}/.runtime"
RUNTIME_LOG_DIR="${RUNTIME_DIR}/logs"
RUNTIME_CACHE_DIR="${RUNTIME_DIR}/genicam-cache"
RUNTIME_LOG_CFG="${RUNTIME_DIR}/log4cplus.runtime.properties"
RUNTIME_LOG_FILE="${RUNTIME_LOG_DIR}/sdk.log"

mkdir -p "${RUNTIME_LOG_DIR}" "${RUNTIME_CACHE_DIR}"
touch "${RUNTIME_LOG_FILE}" 2>/dev/null || true

cat > "${RUNTIME_LOG_CFG}" <<EOF
log4cplus.rootLogger=ERROR, FILE
log4cplus.appender.FILE=log4cplus::RollingFileAppender
log4cplus.appender.FILE.File=${RUNTIME_LOG_FILE}
log4cplus.appender.FILE.CreateDirs=true
log4cplus.appender.FILE.MaxFileSize=20MB
log4cplus.appender.FILE.MaxBackupIndex=2
log4cplus.appender.FILE.layout=log4cplus::PatternLayout
log4cplus.appender.FILE.layout.ConversionPattern=%D{%Y-%m-%d %H:%M:%S:%q};%p;%t;%c;[%l];%m%n
EOF

if [[ -z "${LOG4CPLUS_CONFIGURATION:-}" ]]; then
  export LOG4CPLUS_CONFIGURATION="${RUNTIME_LOG_CFG}"
fi
if [[ -z "${GENICAM_CACHE_V3_0:-}" ]]; then
  export GENICAM_CACHE_V3_0="${RUNTIME_CACHE_DIR}"
fi
if [[ -z "${GENICAM_GENTL64_PATH:-}" ]]; then
  export GENICAM_GENTL64_PATH="${SDK_LIB}"
elif [[ ":${GENICAM_GENTL64_PATH}:" != *":${SDK_LIB}:"* ]]; then
  export GENICAM_GENTL64_PATH="${SDK_LIB}:${GENICAM_GENTL64_PATH}"
fi

if [[ ! -f /etc/Galaxy/cfg/log4cplus.properties ]]; then
  echo "Warning: /etc/Galaxy/cfg/log4cplus.properties not found." >&2
  echo "         If camera open fails on Jetson, run installer once:" >&2
  echo "         sudo ${SDK_ROOT}/Galaxy_camera.run" >&2
fi
if [[ ! -d /var/log/Galaxy || ! -w /var/log/Galaxy ]]; then
  echo "Info: /var/log/Galaxy is not writable for current user; using local SDK log file:"
  echo "      ${RUNTIME_LOG_FILE}"
fi

if [[ "${IS_ARM64}" -eq 1 && "${PULSAR_TUNE_LIMITS:-1}" == "1" ]]; then
  stack_soft="$(ulimit -s 2>/dev/null || true)"
  if [[ "${stack_soft}" == "unlimited" ]]; then
    ulimit -s 4096 2>/dev/null || true
  elif [[ "${stack_soft}" =~ ^[0-9]+$ ]] && (( stack_soft > 4096 )); then
    ulimit -s 4096 2>/dev/null || true
  fi

  nproc_soft="$(ulimit -u 2>/dev/null || true)"
  if [[ "${nproc_soft}" =~ ^[0-9]+$ ]] && (( nproc_soft < 1024 )); then
    ulimit -u 1024 2>/dev/null || true
  fi
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
  if [[ -S /tmp/.X11-unix/X0 ]]; then
    export DISPLAY=:0
    if [[ -z "${XAUTHORITY:-}" && -f "${HOME}/.Xauthority" ]]; then
      export XAUTHORITY="${HOME}/.Xauthority"
    fi
    echo "DISPLAY was empty; trying local X server via DISPLAY=:0"
  fi
fi

if [[ "${HAS_NO_DISPLAY}" -eq 0 && -n "${DISPLAY:-}" ]] && command -v xdpyinfo >/dev/null 2>&1; then
  if ! xdpyinfo >/dev/null 2>&1; then
    echo "Cannot access X display '${DISPLAY}'; running with --no-display"
    RUN_ARGS+=(--no-display)
    HAS_NO_DISPLAY=1
  fi
fi

if [[ "${HAS_NO_DISPLAY}" -eq 0 && -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "No active GUI session detected (DISPLAY/WAYLAND_DISPLAY is empty); running with --no-display"
  RUN_ARGS+=(--no-display)
  HAS_NO_DISPLAY=1
fi

if [[ "${IS_ARM64}" -eq 1 ]]; then
  echo "Tip (Jetson):"
  echo "  1) sudo ${SDK_ROOT}/Galaxy_camera.run"
  echo "  2) sudo nvpmodel -m 0 && sudo jetson_clocks"
  echo "  3) sudo ${SDK_ROOT}/SetUSBStack.sh   # for USB cameras"
fi

APP_STDOUT_LOG="${RUNTIME_LOG_DIR}/last-run.log"
set +e
"${BIN}" "${RUN_ARGS[@]}" 2>&1 | tee "${APP_STDOUT_LOG}"
APP_STATUS=${PIPESTATUS[0]}
set -e

if [[ "${APP_STATUS}" -ne 0 ]] && grep -q "Thread creation was not successful" "${APP_STDOUT_LOG}"; then
  echo ""
  echo "Detected Galaxy SDK init failure: 'Thread creation was not successful'."
  echo "Try one-time system setup on Jetson, then reboot:"
  echo "  sudo ${SDK_ROOT}/Galaxy_camera.run"
  echo "  sudo reboot"
  echo ""
  echo "If you are running from SSH and need display on the Jetson monitor:"
  echo "  export DISPLAY=:0"
  echo "  ./run.sh"
fi

exit "${APP_STATUS}"
