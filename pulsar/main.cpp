#include "GxIAPI.h"
#include "DxImageProc.h"
#include "GxPixelFormat.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kTargetCameras = 2;
constexpr uint32_t kDefaultAcqBufferNum = 8;
constexpr uint32_t kMinAcqBufferNum = 2;
constexpr uint32_t kMaxAcqBufferNum = 16;
constexpr int64_t kStreamTransferSize = 1024 * 1024;
constexpr int64_t kStreamTransferUrb = 64;
constexpr uint32_t kEnumTimeoutMs = 1000;

constexpr int kViewWidth = 1920;
constexpr int kViewHeight = 1080;
constexpr int kCanvasWidth = kViewWidth * 2;
constexpr int kCanvasHeight = kViewHeight;

constexpr uint32_t kDefaultTriggerHz = 30;
constexpr uint32_t kMinTriggerHz = 1;
constexpr uint32_t kMaxTriggerHz = 120;
constexpr double kDefaultExposureUs = 34000.0;
constexpr double kExposureGuardUs = 500.0;
constexpr uint32_t kDefaultStaleWarnMs = 120;
constexpr uint32_t kMinStaleWarnMs = 20;
constexpr uint32_t kMaxStaleWarnMs = 2000;
constexpr uint32_t kDefaultFetchTimeoutMs = 50;
constexpr uint32_t kMinFetchTimeoutMs = 1;
constexpr uint32_t kMaxFetchTimeoutMs = 200;
constexpr uint32_t kDefaultOpencvThreads = 1;
constexpr uint32_t kMinOpencvThreads = 1;
constexpr uint32_t kMaxOpencvThreads = 8;

constexpr double kPresetGain = 10.0;
constexpr double kPresetBlackLevel = 1.0;
constexpr double kPresetBalanceRatio = 2.0;
constexpr int64_t kPresetLightSourcePreset = 3;
constexpr int64_t kPresetBalanceWhiteAuto = 0;
constexpr int64_t kPresetExposureAuto = 0;
constexpr int64_t kPresetAcquisitionFrameRateMode = 1;
constexpr double kPresetAutoExposureTimeMinUs = 0.0;
constexpr double kPresetGamma = 0.75;

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int) {
    g_stop_requested = 1;
}

struct CameraContext {
    uint32_t index = 0;
    GX_DEV_HANDLE device = nullptr;
    GX_DS_HANDLE stream = nullptr;

    std::string model;
    std::string serial;

    int64_t color_filter = GX_COLOR_FILTER_NONE;
    bool is_color = false;

    uint64_t ok_frames = 0;
    uint64_t bad_frames = 0;
    uint64_t timeouts = 0;
    double applied_exposure_us = 0.0;
    double applied_frame_rate = 0.0;

    std::vector<unsigned char> raw8_buffer;
    std::vector<unsigned char> rgb_buffer;
};

enum class FetchResult {
    kOk = 0,
    kTimeout = 1,
    kNoValidFrame = 2,
    kError = 3,
};

struct FrameMailbox {
    std::mutex mutex;
    cv::Mat frame;
    uint64_t frame_id = 0;
    uint64_t timestamp = 0;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point host_time{};
};

struct CaptureThreadContext {
    CameraContext *camera = nullptr;
    FrameMailbox *mailbox = nullptr;
    uint32_t fetch_timeout_ms = kDefaultFetchTimeoutMs;
    uint32_t dq_capacity = kDefaultAcqBufferNum;
    bool swap_rb = false;
    std::atomic<bool> stop{false};
    std::atomic<bool> fatal{false};
};

uint32_t ParseUintEnvOrDefault(const char *env_name, uint32_t default_value,
                               uint32_t min_value, uint32_t max_value) {
    const char *raw = std::getenv(env_name);
    if (raw == nullptr) {
        return default_value;
    }

    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || (end != nullptr && *end != '\0')) {
        return default_value;
    }

    if (parsed < static_cast<unsigned long long>(min_value)) {
        return min_value;
    }
    if (parsed > static_cast<unsigned long long>(max_value)) {
        return max_value;
    }
    return static_cast<uint32_t>(parsed);
}

double ParseDoubleEnvOrDefault(const char *env_name, double default_value,
                               double min_value, double max_value) {
    const char *raw = std::getenv(env_name);
    if (raw == nullptr) {
        return default_value;
    }

    char *end = nullptr;
    const double parsed = std::strtod(raw, &end);
    if (end == raw || (end != nullptr && *end != '\0')) {
        return default_value;
    }

    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return parsed;
}

bool ShouldUseSoftwareTrigger() {
    const char *mode = std::getenv("PULSAR_TRIGGER_MODE");
    if (mode == nullptr) {
        return false;
    }

    std::string value(mode);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value == "1" || value == "on" || value == "sw" ||
           value == "software";
}

std::string TrimAsciiSpaces(std::string value) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

bool ParseBoolEnvOrDefault(const char *env_name, bool default_value) {
    const char *raw = std::getenv(env_name);
    if (raw == nullptr) {
        return default_value;
    }

    std::string value = TrimAsciiSpaces(std::string(raw));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value.empty()) {
        return default_value;
    }

    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        return false;
    }
    return default_value;
}

int64_t ParseInt64EnvOrDefault(const char *env_name, int64_t default_value,
                               int64_t min_value, int64_t max_value) {
    const char *raw = std::getenv(env_name);
    if (raw == nullptr) {
        return default_value;
    }

    char *end = nullptr;
    const long long parsed = std::strtoll(raw, &end, 10);
    if (end == raw || (end != nullptr && *end != '\0')) {
        return default_value;
    }

    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return static_cast<int64_t>(parsed);
}

std::array<bool, kTargetCameras> ParseSwapRedBlueConfig() {
    std::array<bool, kTargetCameras> config{};
    config.fill(true);

    const char *swap_env = std::getenv("PULSAR_SWAP_RB");
    if (swap_env == nullptr) {
        return config;
    }

    std::string raw(swap_env);
    if (raw.find(',') == std::string::npos) {
        const bool enabled = TrimAsciiSpaces(raw) != "0";
        config.fill(enabled);
        return config;
    }

    size_t token_start = 0;
    size_t token_index = 0;
    while (token_index < config.size()) {
        const size_t comma_pos = raw.find(',', token_start);
        const size_t token_len = (comma_pos == std::string::npos)
                                     ? std::string::npos
                                     : (comma_pos - token_start);
        const std::string token =
            TrimAsciiSpaces(raw.substr(token_start, token_len));
        if (!token.empty()) {
            config[token_index] = (token != "0");
        }

        ++token_index;
        if (comma_pos == std::string::npos) {
            break;
        }
        token_start = comma_pos + 1;
    }

    return config;
}

void SwapRedBlueIfNeeded(cv::Mat *bgr, bool swap_rb) {
    if (!swap_rb || bgr == nullptr || bgr->empty()) {
        return;
    }
    cv::cvtColor(*bgr, *bgr, cv::COLOR_BGR2RGB);
}

const char *FastPixelFormatForCamera(const CameraContext &cam) {
    switch (cam.color_filter) {
    case GX_COLOR_FILTER_BAYER_RG:
        return "BayerRG8";
    case GX_COLOR_FILTER_BAYER_GB:
        return "BayerGB8";
    case GX_COLOR_FILTER_BAYER_GR:
        return "BayerGR8";
    case GX_COLOR_FILTER_BAYER_BG:
        return "BayerBG8";
    case GX_COLOR_FILTER_NONE:
    default:
        return "Mono8";
    }
}

std::string GetErrorString(GX_STATUS status) {
    size_t size = 0;
    GX_STATUS api_status = GXGetLastError(&status, nullptr, &size);
    if (api_status != GX_STATUS_SUCCESS || size == 0) {
        return "Unknown error";
    }

    std::vector<char> buffer(size, 0);
    api_status = GXGetLastError(&status, buffer.data(), &size);
    if (api_status != GX_STATUS_SUCCESS) {
        return "Unknown error";
    }
    return std::string(buffer.data());
}

bool IsReadableOrWritable(GX_NODE_ACCESS_MODE mode) {
    return mode == GX_NODE_ACCESS_MODE_RO || mode == GX_NODE_ACCESS_MODE_WO ||
           mode == GX_NODE_ACCESS_MODE_RW;
}

bool IsReadable(GX_NODE_ACCESS_MODE mode) {
    return mode == GX_NODE_ACCESS_MODE_RO || mode == GX_NODE_ACCESS_MODE_RW;
}

bool IsWritable(GX_NODE_ACCESS_MODE mode) {
    return mode == GX_NODE_ACCESS_MODE_WO || mode == GX_NODE_ACCESS_MODE_RW;
}

bool NodeIsWritable(GX_PORT_HANDLE handle, const char *node_name) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS) {
        return false;
    }
    return mode == GX_NODE_ACCESS_MODE_RW;
}

bool TryReadFloatNode(GX_PORT_HANDLE handle, const char *node_name, double *out_value) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || !IsReadable(mode)) {
        return false;
    }

    GX_FLOAT_VALUE value{};
    status = GXGetFloatValue(handle, node_name, &value);
    if (status != GX_STATUS_SUCCESS) {
        return false;
    }
    *out_value = value.dCurValue;
    return true;
}

void TrySetIntNode(GX_PORT_HANDLE handle, const char *node_name, int64_t value) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || !IsWritable(mode)) {
        return;
    }
    status = GXSetIntValue(handle, node_name, value);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] Failed to set " << node_name << ": "
                  << GetErrorString(status) << "\n";
    }
}

void TrySetFloatNode(GX_PORT_HANDLE handle, const char *node_name, double value) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || !IsWritable(mode)) {
        return;
    }
    status = GXSetFloatValue(handle, node_name, value);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] Failed to set " << node_name << ": "
                  << GetErrorString(status) << "\n";
    }
}

void TrySetEnumNode(GX_PORT_HANDLE handle, const char *node_name, const char *value) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || mode != GX_NODE_ACCESS_MODE_RW) {
        return;
    }
    status = GXSetEnumValueByString(handle, node_name, value);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] Failed to set " << node_name << "=" << value << ": "
                  << GetErrorString(status) << "\n";
    }
}

void TrySetEnumNodeByInt(GX_PORT_HANDLE handle, const char *node_name, int64_t value) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || mode != GX_NODE_ACCESS_MODE_RW) {
        return;
    }
    status = GXSetEnumValue(handle, node_name, value);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] Failed to set " << node_name << "=" << value << ": "
                  << GetErrorString(status) << "\n";
    }
}

bool SetEnumNodeStrict(GX_PORT_HANDLE handle, const char *node_name,
                       const char *value) {
    GX_STATUS status = GXSetEnumValueByString(handle, node_name, value);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] Failed to set " << node_name << "=" << value << ": "
                  << GetErrorString(status) << "\n";
        return false;
    }
    return true;
}

std::string GetStringNode(GX_DEV_HANDLE handle, const char *node_name) {
    GX_STRING_VALUE value{};
    GX_STATUS status = GXGetStringValue(handle, node_name, &value);
    if (status != GX_STATUS_SUCCESS) {
        return "";
    }
    return std::string(value.strCurValue);
}

int64_t FitIntToNodeRange(const GX_INT_VALUE &range, int64_t target) {
    int64_t value = std::max(range.nMin, std::min(target, range.nMax));
    if (range.nInc > 0) {
        value = range.nMin + ((value - range.nMin) / range.nInc) * range.nInc;
    }
    return std::max(range.nMin, std::min(value, range.nMax));
}

double FitFloatToNodeRange(const GX_FLOAT_VALUE &range, double target) {
    double value = std::max(range.dMin, std::min(target, range.dMax));
    if (range.bIncIsValid && range.dInc > 0.0) {
        const double steps = static_cast<double>(static_cast<int64_t>(
            (value - range.dMin) / range.dInc));
        value = range.dMin + steps * range.dInc;
    }
    return std::max(range.dMin, std::min(value, range.dMax));
}

void TrySetFloatNearest(GX_PORT_HANDLE handle, const char *node_name, double target) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || mode != GX_NODE_ACCESS_MODE_RW) {
        return;
    }

    GX_FLOAT_VALUE range{};
    status = GXGetFloatValue(handle, node_name, &range);
    if (status != GX_STATUS_SUCCESS) {
        return;
    }

    const double fitted = FitFloatToNodeRange(range, target);
    status = GXSetFloatValue(handle, node_name, fitted);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] Failed to set " << node_name << " to " << fitted << ": "
                  << GetErrorString(status) << "\n";
    }
}

void TrySetIntNearest(GX_PORT_HANDLE handle, const char *node_name, int64_t target) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || mode != GX_NODE_ACCESS_MODE_RW) {
        return;
    }

    GX_INT_VALUE range{};
    status = GXGetIntValue(handle, node_name, &range);
    if (status != GX_STATUS_SUCCESS) {
        return;
    }

    const int64_t fitted = FitIntToNodeRange(range, target);
    status = GXSetIntValue(handle, node_name, fitted);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] Failed to set " << node_name << " to " << fitted << ": "
                  << GetErrorString(status) << "\n";
    }
}

bool IsBayer8(int32_t fmt) {
    return fmt == GX_PIXEL_FORMAT_BAYER_GR8 || fmt == GX_PIXEL_FORMAT_BAYER_RG8 ||
           fmt == GX_PIXEL_FORMAT_BAYER_GB8 || fmt == GX_PIXEL_FORMAT_BAYER_BG8;
}

bool IsBayer16Family(int32_t fmt) {
    switch (fmt) {
    case GX_PIXEL_FORMAT_BAYER_GR10:
    case GX_PIXEL_FORMAT_BAYER_RG10:
    case GX_PIXEL_FORMAT_BAYER_GB10:
    case GX_PIXEL_FORMAT_BAYER_BG10:
    case GX_PIXEL_FORMAT_BAYER_GR12:
    case GX_PIXEL_FORMAT_BAYER_RG12:
    case GX_PIXEL_FORMAT_BAYER_GB12:
    case GX_PIXEL_FORMAT_BAYER_BG12:
    case GX_PIXEL_FORMAT_BAYER_GR14:
    case GX_PIXEL_FORMAT_BAYER_RG14:
    case GX_PIXEL_FORMAT_BAYER_GB14:
    case GX_PIXEL_FORMAT_BAYER_BG14:
    case GX_PIXEL_FORMAT_BAYER_GR16:
    case GX_PIXEL_FORMAT_BAYER_RG16:
    case GX_PIXEL_FORMAT_BAYER_GB16:
    case GX_PIXEL_FORMAT_BAYER_BG16:
        return true;
    default:
        return false;
    }
}

bool IsMono16Family(int32_t fmt) {
    return fmt == GX_PIXEL_FORMAT_MONO10 || fmt == GX_PIXEL_FORMAT_MONO12 ||
           fmt == GX_PIXEL_FORMAT_MONO14 || fmt == GX_PIXEL_FORMAT_MONO16;
}

DX_VALID_BIT ValidBitsForPixelFormat(int32_t fmt) {
    switch (fmt) {
    case GX_PIXEL_FORMAT_MONO10:
    case GX_PIXEL_FORMAT_BAYER_GR10:
    case GX_PIXEL_FORMAT_BAYER_RG10:
    case GX_PIXEL_FORMAT_BAYER_GB10:
    case GX_PIXEL_FORMAT_BAYER_BG10:
        return DX_BIT_2_9;
    case GX_PIXEL_FORMAT_MONO12:
    case GX_PIXEL_FORMAT_BAYER_GR12:
    case GX_PIXEL_FORMAT_BAYER_RG12:
    case GX_PIXEL_FORMAT_BAYER_GB12:
    case GX_PIXEL_FORMAT_BAYER_BG12:
        return DX_BIT_4_11;
    case GX_PIXEL_FORMAT_MONO14:
    case GX_PIXEL_FORMAT_BAYER_GR14:
    case GX_PIXEL_FORMAT_BAYER_RG14:
    case GX_PIXEL_FORMAT_BAYER_GB14:
    case GX_PIXEL_FORMAT_BAYER_BG14:
        return DX_BIT_6_13;
    case GX_PIXEL_FORMAT_MONO16:
    case GX_PIXEL_FORMAT_BAYER_GR16:
    case GX_PIXEL_FORMAT_BAYER_RG16:
    case GX_PIXEL_FORMAT_BAYER_GB16:
    case GX_PIXEL_FORMAT_BAYER_BG16:
    default:
        return DX_BIT_8_15;
    }
}

bool ConvertFrameToBgr(PGX_FRAME_BUFFER frame, CameraContext *cam, cv::Mat *out_bgr,
                       bool swap_rb) {
    const int width = frame->nWidth;
    const int height = frame->nHeight;
    const int32_t pixfmt = frame->nPixelFormat;

    out_bgr->create(height, width, CV_8UC3);

    if (pixfmt == GX_PIXEL_FORMAT_MONO8) {
        cv::Mat gray(height, width, CV_8UC1, frame->pImgBuf);
        cv::cvtColor(gray, *out_bgr, cv::COLOR_GRAY2BGR);
        return true;
    }

    if (pixfmt == GX_PIXEL_FORMAT_BGR8) {
        cv::Mat bgr(height, width, CV_8UC3, frame->pImgBuf);
        if (swap_rb) {
            cv::cvtColor(bgr, *out_bgr, cv::COLOR_BGR2RGB);
        } else {
            bgr.copyTo(*out_bgr);
        }
        return true;
    }

    if (pixfmt == GX_PIXEL_FORMAT_RGB8) {
        cv::Mat rgb(height, width, CV_8UC3, frame->pImgBuf);
        if (swap_rb) {
            rgb.copyTo(*out_bgr);
        } else {
            cv::cvtColor(rgb, *out_bgr, cv::COLOR_RGB2BGR);
        }
        return true;
    }

    if (IsBayer8(pixfmt)) {
        cam->rgb_buffer.resize(static_cast<size_t>(width) * height * 3);
        const VxInt32 dx_status = DxRaw8toRGB24(
            frame->pImgBuf, cam->rgb_buffer.data(), width, height, RAW2RGB_NEIGHBOUR,
            static_cast<DX_PIXEL_COLOR_FILTER>(cam->color_filter), false);
        if (dx_status != DX_OK) {
            std::cerr << "[ERR ] DxRaw8toRGB24 failed: " << dx_status << "\n";
            return false;
        }
        cv::Mat rgb(height, width, CV_8UC3, cam->rgb_buffer.data());
        if (swap_rb) {
            rgb.copyTo(*out_bgr);
        } else {
            cv::cvtColor(rgb, *out_bgr, cv::COLOR_RGB2BGR);
        }
        return true;
    }

    if (IsBayer16Family(pixfmt)) {
        cam->raw8_buffer.resize(static_cast<size_t>(width) * height);
        cam->rgb_buffer.resize(static_cast<size_t>(width) * height * 3);

        VxInt32 dx_status = DxRaw16toRaw8(frame->pImgBuf, cam->raw8_buffer.data(), width,
                                          height, ValidBitsForPixelFormat(pixfmt));
        if (dx_status != DX_OK) {
            std::cerr << "[ERR ] DxRaw16toRaw8 failed: " << dx_status << "\n";
            return false;
        }

        dx_status = DxRaw8toRGB24(cam->raw8_buffer.data(), cam->rgb_buffer.data(), width,
                                  height, RAW2RGB_NEIGHBOUR,
                                  static_cast<DX_PIXEL_COLOR_FILTER>(cam->color_filter),
                                  false);
        if (dx_status != DX_OK) {
            std::cerr << "[ERR ] DxRaw8toRGB24 failed: " << dx_status << "\n";
            return false;
        }

        cv::Mat rgb(height, width, CV_8UC3, cam->rgb_buffer.data());
        if (swap_rb) {
            rgb.copyTo(*out_bgr);
        } else {
            cv::cvtColor(rgb, *out_bgr, cv::COLOR_RGB2BGR);
        }
        return true;
    }

    if (IsMono16Family(pixfmt)) {
        cam->raw8_buffer.resize(static_cast<size_t>(width) * height);
        const VxInt32 dx_status = DxRaw16toRaw8(frame->pImgBuf, cam->raw8_buffer.data(),
                                                width, height,
                                                ValidBitsForPixelFormat(pixfmt));
        if (dx_status != DX_OK) {
            std::cerr << "[ERR ] DxRaw16toRaw8 failed: " << dx_status << "\n";
            return false;
        }
        cv::Mat gray(height, width, CV_8UC1, cam->raw8_buffer.data());
        cv::cvtColor(gray, *out_bgr, cv::COLOR_GRAY2BGR);
        return true;
    }

    std::cerr << "[ERR ] Unsupported pixel format: " << pixfmt << "\n";
    return false;
}

std::vector<uint32_t> SelectCameraIndices(uint32_t total_count, uint32_t need_count) {
    std::vector<uint32_t> u3v_indices;
    std::vector<uint32_t> other_indices;

    for (uint32_t index = 1; index <= total_count; ++index) {
        GX_DEVICE_INFO info{};
        GX_STATUS status = GXGetDeviceInfo(index, &info);
        if (status != GX_STATUS_SUCCESS) {
            other_indices.push_back(index);
            continue;
        }

        if (info.emDevType == GX_DEVICE_CLASS_U3V) {
            u3v_indices.push_back(index);
        } else {
            other_indices.push_back(index);
        }
    }

    std::vector<uint32_t> selected;
    for (uint32_t idx : u3v_indices) {
        if (selected.size() >= need_count) {
            break;
        }
        selected.push_back(idx);
    }
    for (uint32_t idx : other_indices) {
        if (selected.size() >= need_count) {
            break;
        }
        selected.push_back(idx);
    }
    return selected;
}

bool OpenCamera(CameraContext *ctx) {
    std::string index_string = std::to_string(ctx->index);
    GX_OPEN_PARAM open_param{};
    open_param.pszContent = index_string.data();
    open_param.openMode = GX_OPEN_INDEX;
    open_param.accessMode = GX_ACCESS_EXCLUSIVE;

    GX_STATUS status = GXOpenDevice(&open_param, &ctx->device);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] Failed to open camera index " << ctx->index << ": "
                  << GetErrorString(status) << "\n";
        return false;
    }

    uint32_t ds_num = 0;
    status = GXGetDataStreamNumFromDev(ctx->device, &ds_num);
    if (status != GX_STATUS_SUCCESS || ds_num < 1) {
        std::cerr << "[ERR ] Failed to get stream count from camera " << ctx->index
                  << ": " << GetErrorString(status) << "\n";
        return false;
    }

    status = GXGetDataStreamHandleFromDev(ctx->device, 1, &ctx->stream);
    if (status != GX_STATUS_SUCCESS || ctx->stream == nullptr) {
        std::cerr << "[ERR ] Failed to get stream handle from camera " << ctx->index
                  << ": " << GetErrorString(status) << "\n";
        return false;
    }

    ctx->model = GetStringNode(ctx->device, "DeviceModelName");
    ctx->serial = GetStringNode(ctx->device, "DeviceSerialNumber");
    if (ctx->model.empty()) {
        ctx->model = "unknown-model";
    }
    if (ctx->serial.empty()) {
        ctx->serial = "unknown-serial";
    }

    GX_NODE_ACCESS_MODE color_mode = GX_NODE_ACCESS_MODE_NI;
    status = GXGetNodeAccessMode(ctx->device, "PixelColorFilter", &color_mode);
    if (status == GX_STATUS_SUCCESS && IsReadableOrWritable(color_mode)) {
        ctx->is_color = true;
        GX_ENUM_VALUE em_value{};
        status = GXGetEnumValue(ctx->device, "PixelColorFilter", &em_value);
        if (status == GX_STATUS_SUCCESS) {
            ctx->color_filter = em_value.stCurValue.nCurValue;
        }
    }

    return true;
}

void ConfigureRoi1920x1080(GX_DEV_HANDLE dev) {
    // Setting ROI to 1920x1080 minimizes scaling cost and latency.
    TrySetIntNearest(dev, "OffsetX", 0);
    TrySetIntNearest(dev, "OffsetY", 0);
    TrySetIntNearest(dev, "Width", kViewWidth);
    TrySetIntNearest(dev, "Height", kViewHeight);
    TrySetIntNearest(dev, "OffsetX", 0);
    TrySetIntNearest(dev, "OffsetY", 0);
}

void PreallocateCameraConversionBuffers(CameraContext *ctx) {
    const size_t pixels = static_cast<size_t>(kViewWidth) * kViewHeight;
    ctx->raw8_buffer.resize(pixels);
    ctx->rgb_buffer.resize(pixels * 3);
}

bool ConfigureCamera(CameraContext *ctx, uint32_t trigger_hz, double exposure_us,
                     bool use_software_trigger, uint32_t acq_buffer_num,
                     bool fake_profile_enabled, double fake_profile_exposure_us,
                     double manual_gain, double manual_black_level,
                     double manual_balance_ratio, int64_t light_source_preset,
                     double manual_gamma) {
    ConfigureRoi1920x1080(ctx->device);

    // Prefer 8-bit output to reduce conversion cost and display latency.
    TrySetEnumNode(ctx->device, "PixelFormat", FastPixelFormatForCamera(*ctx));

    if (!SetEnumNodeStrict(ctx->device, "AcquisitionMode", "Continuous")) {
        return false;
    }
    if (use_software_trigger) {
        if (!SetEnumNodeStrict(ctx->device, "TriggerMode", "On")) {
            return false;
        }
        if (!SetEnumNodeStrict(ctx->device, "TriggerSource", "Software")) {
            return false;
        }
    } else {
        if (!SetEnumNodeStrict(ctx->device, "TriggerMode", "Off")) {
            return false;
        }
    }

    GX_STATUS status = GXSetAcqusitionBufferNumber(ctx->device,
                                                   static_cast<uint64_t>(acq_buffer_num));
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] Failed to set acquisition buffer number on camera "
                  << ctx->index << ": " << GetErrorString(status) << "\n";
        return false;
    }

    TrySetIntNode(ctx->stream, "StreamTransferSize", kStreamTransferSize);
    TrySetIntNode(ctx->stream, "StreamTransferNumberUrb", kStreamTransferUrb);
    TrySetEnumNode(ctx->stream, "StreamBufferHandlingMode", "NewestOnly");

    TrySetEnumNodeByInt(ctx->device, "ExposureAuto", kPresetExposureAuto);
    TrySetEnumNode(ctx->device, "ExposureAuto", "Off");
    TrySetEnumNode(ctx->device, "GainAuto", "Off");
    TrySetFloatNode(ctx->device, "Gain", manual_gain);
    TrySetFloatNode(ctx->device, "BlackLevel", manual_black_level);
    TrySetEnumNodeByInt(ctx->device, "LightSourcePreset", light_source_preset);
    TrySetEnumNodeByInt(ctx->device, "BalanceWhiteAuto", kPresetBalanceWhiteAuto);
    TrySetEnumNode(ctx->device, "BalanceWhiteAuto", "Off");
    TrySetFloatNode(ctx->device, "BalanceRatio", manual_balance_ratio);
    TrySetFloatNearest(ctx->device, "AutoExposureTimeMin",
                       kPresetAutoExposureTimeMinUs);
    TrySetFloatNode(ctx->device, "Gamma", manual_gamma);

    const double exposure_profile_us =
        fake_profile_enabled ? fake_profile_exposure_us : exposure_us;
    double applied_exposure = exposure_profile_us;
    if (NodeIsWritable(ctx->device, "ExposureTime")) {
        GX_FLOAT_VALUE exposure_range{};
        status = GXGetFloatValue(ctx->device, "ExposureTime", &exposure_range);
        if (status == GX_STATUS_SUCCESS) {
            const double fitted =
                FitFloatToNodeRange(exposure_range, exposure_profile_us);
            TrySetFloatNode(ctx->device, "ExposureTime", fitted);
            applied_exposure = fitted;
            if (std::abs(fitted - exposure_profile_us) > 0.5) {
                std::cout << "[WARN] Camera " << ctx->index
                          << " requested ExposureTime=" << exposure_profile_us
                          << " but applied " << fitted
                          << " due to camera range constraints.\n";
            }
        }
    }
    ctx->applied_exposure_us = applied_exposure;
    TryReadFloatNode(ctx->device, "ExposureTime", &ctx->applied_exposure_us);
    if (fake_profile_enabled) {
        std::cout << "[INFO] Camera " << ctx->index
                  << " fake-profile exposure_us=" << exposure_profile_us
                  << " (applied_low_latency_us=" << ctx->applied_exposure_us << ")\n";
    }

    if (NodeIsWritable(ctx->device, "AcquisitionFrameRateMode")) {
        TrySetEnumNodeByInt(ctx->device, "AcquisitionFrameRateMode",
                            kPresetAcquisitionFrameRateMode);
        TrySetEnumNode(ctx->device, "AcquisitionFrameRateMode", "On");
    }
    if (NodeIsWritable(ctx->device, "AcquisitionFrameRate")) {
        TrySetFloatNode(ctx->device, "AcquisitionFrameRate",
                        static_cast<double>(trigger_hz));
    }
    ctx->applied_frame_rate = static_cast<double>(trigger_hz);
    TryReadFloatNode(ctx->device, "AcquisitionFrameRate", &ctx->applied_frame_rate);

    return true;
}

void CloseCamera(CameraContext *ctx) {
    if (ctx->device != nullptr) {
        GX_STATUS status = GXCloseDevice(ctx->device);
        if (status != GX_STATUS_SUCCESS) {
            std::cerr << "[WARN] GXCloseDevice failed for camera " << ctx->index
                      << ": " << GetErrorString(status) << "\n";
        }
        ctx->device = nullptr;
        ctx->stream = nullptr;
    }
}

bool TriggerCamera(CameraContext *ctx) {
    GX_STATUS status = GXSetCommandValue(ctx->device, "TriggerSoftware");
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] TriggerSoftware failed for camera " << ctx->index << ": "
                  << GetErrorString(status) << "\n";
        return false;
    }
    return true;
}

FetchResult FetchLatestFrameAsBgr(CameraContext *ctx, uint32_t timeout_ms,
                                  uint32_t dq_capacity, cv::Mat *frame_bgr,
                                  uint64_t *frame_id, uint64_t *timestamp,
                                  bool swap_rb) {
    std::array<PGX_FRAME_BUFFER, kMaxAcqBufferNum> frame_array{};
    uint32_t frame_count = 0;
    const uint32_t clamped_capacity =
        std::max<uint32_t>(1, std::min(dq_capacity, kMaxAcqBufferNum));
    GX_STATUS status = GXDQAllBufs(ctx->device, frame_array.data(),
                                   clamped_capacity,
                                   &frame_count, timeout_ms);

    if (status == GX_STATUS_TIMEOUT) {
        ++ctx->timeouts;
        return FetchResult::kTimeout;
    }
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] GXDQAllBufs failed for camera " << ctx->index << ": "
                  << GetErrorString(status) << "\n";
        return FetchResult::kError;
    }

    PGX_FRAME_BUFFER newest = nullptr;
    for (uint32_t i = 0; i < frame_count; ++i) {
        PGX_FRAME_BUFFER cur = frame_array[i];
        if (cur == nullptr || cur->nStatus != GX_FRAME_STATUS_SUCCESS) {
            ++ctx->bad_frames;
            continue;
        }
        if (newest == nullptr || cur->nFrameID >= newest->nFrameID) {
            newest = cur;
        }
    }

    bool ok = false;
    if (newest != nullptr) {
        if (ConvertFrameToBgr(newest, ctx, frame_bgr, swap_rb)) {
            *frame_id = newest->nFrameID;
            *timestamp = newest->nTimestamp;
            ++ctx->ok_frames;
            ok = true;
        } else {
            ++ctx->bad_frames;
        }
    }

    status = GXQAllBufs(ctx->device);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] GXQAllBufs failed for camera " << ctx->index << ": "
                  << GetErrorString(status) << "\n";
        return FetchResult::kError;
    }

    return ok ? FetchResult::kOk : FetchResult::kNoValidFrame;
}

void CaptureLoop(CaptureThreadContext *ctx) {
    cv::Mat local_bgr;
    while (!ctx->stop.load(std::memory_order_relaxed) && !g_stop_requested) {
        uint64_t frame_id = 0;
        uint64_t timestamp = 0;
        const FetchResult result = FetchLatestFrameAsBgr(
            ctx->camera, ctx->fetch_timeout_ms, ctx->dq_capacity, &local_bgr,
            &frame_id, &timestamp, ctx->swap_rb);

        if (result == FetchResult::kError) {
            ctx->fatal.store(true, std::memory_order_relaxed);
            break;
        }
        if (result != FetchResult::kOk) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(ctx->mailbox->mutex);
            std::swap(ctx->mailbox->frame, local_bgr);
            ctx->mailbox->frame_id = frame_id;
            ctx->mailbox->timestamp = timestamp;
            ctx->mailbox->host_time = now;
            ++ctx->mailbox->sequence;
        }
    }
}

bool PullLatestFrameForDisplay(FrameMailbox *mailbox, uint64_t *last_sequence,
                               cv::Mat *resized_output, const cv::Size &view_size,
                               uint64_t *frame_id,
                               std::chrono::steady_clock::time_point *host_time,
                               bool *got_new_frame) {
    std::lock_guard<std::mutex> lock(mailbox->mutex);
    if (mailbox->sequence == 0 || mailbox->frame.empty()) {
        return false;
    }

    *frame_id = mailbox->frame_id;
    *host_time = mailbox->host_time;

    if (mailbox->sequence == *last_sequence) {
        *got_new_frame = false;
        return true;
    }

    if (mailbox->frame.cols != view_size.width ||
        mailbox->frame.rows != view_size.height) {
        cv::resize(mailbox->frame, *resized_output, view_size, 0.0, 0.0,
                   cv::INTER_NEAREST);
    } else {
        mailbox->frame.copyTo(*resized_output);
    }

    *last_sequence = mailbox->sequence;
    *got_new_frame = true;
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    int run_seconds = 0;
    uint32_t trigger_hz = kDefaultTriggerHz;

    if (argc >= 2) {
        run_seconds = std::atoi(argv[1]);
        if (run_seconds < 0) {
            std::cerr << "Usage: " << argv[0] << " [seconds>=0] [trigger_hz]\n";
            return 1;
        }
    }
    if (argc >= 3) {
        const int parsed_hz = std::atoi(argv[2]);
        if (parsed_hz < static_cast<int>(kMinTriggerHz) ||
            parsed_hz > static_cast<int>(kMaxTriggerHz)) {
            std::cerr << "trigger_hz must be in [" << kMinTriggerHz << ", "
                      << kMaxTriggerHz << "]\n";
            return 1;
        }
        trigger_hz = static_cast<uint32_t>(parsed_hz);
    }

    const char *headless_env = std::getenv("PULSAR_HEADLESS");
    const bool headless = headless_env != nullptr && std::string(headless_env) == "1";
    const bool use_software_trigger = ShouldUseSoftwareTrigger();
    const std::array<bool, kTargetCameras> swap_rb = ParseSwapRedBlueConfig();
    const bool fake_profile_enabled =
        ParseBoolEnvOrDefault("PULSAR_FAKE_PROFILE", false);
    const bool sync_to_slowest =
        ParseBoolEnvOrDefault("PULSAR_SYNC_TO_SLOWEST", false);
    const bool adaptive_loop =
        ParseBoolEnvOrDefault("PULSAR_ADAPTIVE_LOOP", false);
    const bool display_require_pair =
        ParseBoolEnvOrDefault("PULSAR_DISPLAY_REQUIRE_PAIR", true);
    const uint32_t acq_buffer_num =
        ParseUintEnvOrDefault("PULSAR_ACQ_BUFFER_NUM", kDefaultAcqBufferNum,
                              kMinAcqBufferNum, kMaxAcqBufferNum);
    const uint32_t stale_warn_ms =
        ParseUintEnvOrDefault("PULSAR_STALE_WARN_MS", kDefaultStaleWarnMs,
                              kMinStaleWarnMs, kMaxStaleWarnMs);
    const uint32_t fetch_timeout_ms =
        ParseUintEnvOrDefault("PULSAR_FETCH_TIMEOUT_MS", kDefaultFetchTimeoutMs,
                              kMinFetchTimeoutMs, kMaxFetchTimeoutMs);
    const uint32_t opencv_threads =
        ParseUintEnvOrDefault("PULSAR_OPENCV_THREADS", kDefaultOpencvThreads,
                              kMinOpencvThreads, kMaxOpencvThreads);
    const double fake_profile_exposure_us = ParseDoubleEnvOrDefault(
        "PULSAR_PROFILE_EXPOSURE_US", kDefaultExposureUs, 1.0, 2000000.0);
    const double exposure_us = ParseDoubleEnvOrDefault(
        "PULSAR_EXPOSURE_US", fake_profile_exposure_us, 1.0, 2000000.0);
    const double manual_gain =
        ParseDoubleEnvOrDefault("PULSAR_GAIN", kPresetGain, 0.0, 24.0);
    const double manual_black_level =
        ParseDoubleEnvOrDefault("PULSAR_BLACK_LEVEL", kPresetBlackLevel, 0.0, 16.0);
    const double manual_balance_ratio =
        ParseDoubleEnvOrDefault("PULSAR_BALANCE_RATIO", kPresetBalanceRatio, 0.1, 8.0);
    const int64_t manual_light_source_preset =
        ParseInt64EnvOrDefault("PULSAR_LIGHT_SOURCE_PRESET", kPresetLightSourcePreset,
                               0, 10);
    const double manual_gamma =
        ParseDoubleEnvOrDefault("PULSAR_GAMMA", kPresetGamma, 0.2, 4.0);

    const uint32_t frame_period_ms =
        std::max<uint32_t>(1, (1000u + trigger_hz - 1) / trigger_hz);
    const uint32_t min_fetch_timeout_ms =
        std::min<uint32_t>(kMaxFetchTimeoutMs, frame_period_ms + 8);
    const uint32_t tuned_fetch_timeout_ms =
        std::max(fetch_timeout_ms, min_fetch_timeout_ms);

    cv::setUseOptimized(true);
    cv::setNumThreads(static_cast<int>(opencv_threads));

    std::cout << "[INFO] Initializing Galaxy SDK...\n";
    GX_STATUS status = GXInitLib();
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] GXInitLib failed: " << GetErrorString(status) << "\n";
        return 1;
    }

    uint32_t device_count = 0;
    status = GXUpdateAllDeviceList(&device_count, kEnumTimeoutMs);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] GXUpdateAllDeviceList failed: " << GetErrorString(status)
                  << "\n";
        GXCloseLib();
        return 1;
    }

    std::cout << "[INFO] Enumerated devices: " << device_count << "\n";
    if (device_count < kTargetCameras) {
        std::cerr << "[ERR ] Need at least " << kTargetCameras << " cameras, found "
                  << device_count << "\n";
        GXCloseLib();
        return 2;
    }

    std::vector<uint32_t> indices = SelectCameraIndices(device_count, kTargetCameras);
    if (indices.size() < kTargetCameras) {
        std::cerr << "[ERR ] Could not select " << kTargetCameras << " cameras.\n";
        GXCloseLib();
        return 2;
    }

    std::vector<CameraContext> cameras(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        cameras[i].index = indices[i];
        if (!OpenCamera(&cameras[i]) ||
            !ConfigureCamera(&cameras[i], trigger_hz, exposure_us,
                             use_software_trigger, acq_buffer_num,
                             fake_profile_enabled, fake_profile_exposure_us,
                             manual_gain, manual_black_level,
                             manual_balance_ratio, manual_light_source_preset,
                             manual_gamma)) {
            for (auto &cam : cameras) {
                CloseCamera(&cam);
            }
            GXCloseLib();
            return 1;
        }
        PreallocateCameraConversionBuffers(&cameras[i]);
        std::cout << "[INFO] Camera index " << cameras[i].index
                  << " opened: model=" << cameras[i].model
                  << " serial=" << cameras[i].serial
                  << " applied_fps=" << cameras[i].applied_frame_rate
                  << " applied_exposure_us=" << cameras[i].applied_exposure_us << "\n";
    }

    uint32_t loop_hz = trigger_hz;
    if (sync_to_slowest) {
        double min_camera_hz = static_cast<double>(trigger_hz);
        for (const auto &cam : cameras) {
            if (cam.applied_frame_rate > 1.0) {
                min_camera_hz = std::min(min_camera_hz, cam.applied_frame_rate);
            }
        }
        const uint32_t rounded_hz =
            static_cast<uint32_t>(std::max(1.0, std::round(min_camera_hz)));
        loop_hz = std::max<uint32_t>(kMinTriggerHz,
                                     std::min<uint32_t>(rounded_hz, trigger_hz));
        if (loop_hz < trigger_hz) {
            std::cout << "[INFO] Syncing display loop to slowest camera: requested "
                      << trigger_hz << " Hz -> loop_hz=" << loop_hz << "\n";
        }
    }

    for (auto &cam : cameras) {
        status = GXStreamOn(cam.device);
        if (status != GX_STATUS_SUCCESS) {
            std::cerr << "[ERR ] GXStreamOn failed for camera " << cam.index << ": "
                      << GetErrorString(status) << "\n";
            for (auto &to_close : cameras) {
                CloseCamera(&to_close);
            }
            GXCloseLib();
            return 1;
        }
    }

    std::array<FrameMailbox, kTargetCameras> mailboxes;
    std::array<CaptureThreadContext, kTargetCameras> workers;
    std::vector<std::thread> capture_threads;
    capture_threads.reserve(kTargetCameras);

    for (auto &mailbox : mailboxes) {
        mailbox.frame.create(kViewHeight, kViewWidth, CV_8UC3);
        mailbox.frame.setTo(cv::Scalar(0, 0, 0));
    }

    for (size_t i = 0; i < kTargetCameras; ++i) {
        workers[i].camera = &cameras[i];
        workers[i].mailbox = &mailboxes[i];
        workers[i].fetch_timeout_ms = tuned_fetch_timeout_ms;
        workers[i].dq_capacity = acq_buffer_num;
        workers[i].swap_rb = swap_rb[i];
        capture_threads.emplace_back(CaptureLoop, &workers[i]);
    }

    cv::Mat canvas(kCanvasHeight, kCanvasWidth, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat left_resized(kViewHeight, kViewWidth, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat right_resized(kViewHeight, kViewWidth, CV_8UC3, cv::Scalar(0, 0, 0));

    const std::string window_name = "Pulsar-SideBySide";
    if (!headless) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::setWindowProperty(window_name, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
    }

    uint32_t runtime_loop_hz = loop_hz;
    auto period = std::chrono::microseconds(1000000 / runtime_loop_hz);
    std::cout << "[INFO] Fullscreen side-by-side mode: each camera 1920x1080, "
              << "trigger_hz=" << trigger_hz << ", loop_hz=" << loop_hz
              << ", exposure_us=" << exposure_us
              << ", fake_profile_exposure_us=" << fake_profile_exposure_us
              << ", fake_profile=" << (fake_profile_enabled ? 1 : 0)
              << ", sync_to_slowest=" << (sync_to_slowest ? 1 : 0)
              << ", adaptive_loop=" << (adaptive_loop ? 1 : 0)
              << ", display_require_pair=" << (display_require_pair ? 1 : 0)
              << ", headless=" << (headless ? 1 : 0)
              << ", trigger_mode="
              << (use_software_trigger ? "software" : "continuous")
              << ", acq_buffer_num=" << acq_buffer_num
              << ", fetch_timeout_ms=" << tuned_fetch_timeout_ms
              << ", opencv_threads=" << opencv_threads
              << ", swap_rb=[" << (swap_rb[0] ? 1 : 0) << ","
              << (swap_rb[1] ? 1 : 0) << "]"
              << ", stale_warn_ms=" << stale_warn_ms << "\n";
    std::cout << "[INFO] Fixed profile: Gain=" << manual_gain
              << " BlackLevel=" << manual_black_level
              << " BalanceRatio=" << manual_balance_ratio
              << " LightSourcePreset=" << manual_light_source_preset
              << " BalanceWhiteAuto=" << kPresetBalanceWhiteAuto
              << " ExposureAuto=" << kPresetExposureAuto
              << " AutoExposureTimeMin=" << kPresetAutoExposureTimeMinUs
              << " Gamma=" << manual_gamma
              << " AcquisitionFrameRateMode=" << kPresetAcquisitionFrameRateMode
              << " AcquisitionFrameRate=" << trigger_hz << "\n";

    if (run_seconds == 0) {
        std::cout << "[INFO] Running continuously. Press ESC/q/Ctrl+C to stop.\n";
    } else {
        std::cout << "[INFO] Running for " << run_seconds << " seconds.\n";
    }

    uint64_t cycles = 0;
    uint64_t dropped_cycles = 0;
    uint64_t stale_cycles = 0;
    uint64_t prev_ok0 = 0;
    uint64_t prev_ok1 = 0;
    uint64_t latest_frame_id0 = 0;
    uint64_t latest_frame_id1 = 0;
    uint64_t seq0 = 0;
    uint64_t seq1 = 0;
    bool left_ready = false;
    bool right_ready = false;

    auto next_tick = std::chrono::steady_clock::now();
    auto report_tick = next_tick;
    auto last_ok0 = next_tick;
    auto last_ok1 = next_tick;
    const auto end_time = (run_seconds > 0)
                              ? (next_tick + std::chrono::seconds(run_seconds))
                              : std::chrono::steady_clock::time_point::max();

    bool fatal = false;
    while (!g_stop_requested && std::chrono::steady_clock::now() < end_time) {
        next_tick += period;

        if (use_software_trigger) {
            bool trigger_ok = true;
            for (auto &cam : cameras) {
                if (!TriggerCamera(&cam)) {
                    trigger_ok = false;
                }
            }
            if (!trigger_ok) {
                fatal = true;
                break;
            }
        }

        for (const auto &worker : workers) {
            if (worker.fatal.load(std::memory_order_relaxed)) {
                fatal = true;
            }
        }
        if (fatal) {
            break;
        }

        ++cycles;
        const auto now = std::chrono::steady_clock::now();

        uint64_t frame_id0 = latest_frame_id0;
        uint64_t frame_id1 = latest_frame_id1;
        auto host_time0 = last_ok0;
        auto host_time1 = last_ok1;
        bool has0 = false;
        bool has1 = false;
        bool new0 = false;
        bool new1 = false;

        has0 = PullLatestFrameForDisplay(&mailboxes[0], &seq0, &left_resized,
                                         cv::Size(kViewWidth, kViewHeight), &frame_id0,
                                         &host_time0, &new0);
        has1 = PullLatestFrameForDisplay(&mailboxes[1], &seq1, &right_resized,
                                         cv::Size(kViewWidth, kViewHeight), &frame_id1,
                                         &host_time1, &new1);

        if (!(new0 && new1)) {
            ++dropped_cycles;
        }

        if (has0 && new0) {
            left_ready = true;
            last_ok0 = host_time0;
            latest_frame_id0 = frame_id0;
        }
        if (has1 && new1) {
            right_ready = true;
            last_ok1 = host_time1;
            latest_frame_id1 = frame_id1;
        }

        if (!(left_ready && right_ready)) {
            const auto sleep_now = std::chrono::steady_clock::now();
            if (next_tick > sleep_now) {
                std::this_thread::sleep_until(next_tick);
            } else {
                next_tick = sleep_now;
            }
            continue;
        }

        const int64_t pending_stale0_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - last_ok0).count();
        const int64_t pending_stale1_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - last_ok1).count();
        if (display_require_pair && !(new0 && new1) &&
            pending_stale0_ms <= static_cast<int64_t>(stale_warn_ms) &&
            pending_stale1_ms <= static_cast<int64_t>(stale_warn_ms)) {
            const auto sleep_now = std::chrono::steady_clock::now();
            if (next_tick > sleep_now) {
                std::this_thread::sleep_until(next_tick);
            } else {
                next_tick = sleep_now;
            }
            continue;
        }

        left_resized.copyTo(canvas(cv::Rect(0, 0, kViewWidth, kViewHeight)));
        right_resized.copyTo(
            canvas(cv::Rect(kViewWidth, 0, kViewWidth, kViewHeight)));

        const int64_t stale0_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ok0).count();
        const int64_t stale1_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ok1).count();
        if (stale0_ms > static_cast<int64_t>(stale_warn_ms) ||
            stale1_ms > static_cast<int64_t>(stale_warn_ms)) {
            ++stale_cycles;
            std::string stale_text = "STALE cam1=" + std::to_string(stale0_ms) +
                                     "ms cam2=" + std::to_string(stale1_ms) + "ms";
            cv::putText(canvas, stale_text, cv::Point(18, 44),
                        cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2,
                        cv::LINE_AA);
        }

        if (!headless) {
            cv::imshow(window_name, canvas);
            const int key = cv::waitKey(1);
            if (key == 27 || key == 'q' || key == 'Q') {
                break;
            }
        }

        if (now - report_tick >= std::chrono::seconds(1)) {
            const uint64_t fps0 = cameras[0].ok_frames - prev_ok0;
            const uint64_t fps1 = cameras[1].ok_frames - prev_ok1;
            prev_ok0 = cameras[0].ok_frames;
            prev_ok1 = cameras[1].ok_frames;

            if (adaptive_loop) {
                const uint32_t observed_min_hz = static_cast<uint32_t>(
                    std::max<uint64_t>(1, std::min(fps0, fps1)));
                const uint32_t capped_observed =
                    std::max<uint32_t>(kMinTriggerHz,
                                       std::min<uint32_t>(observed_min_hz, loop_hz));
                uint32_t adjusted = runtime_loop_hz;
                if (capped_observed < runtime_loop_hz) {
                    adjusted = capped_observed;
                } else if (capped_observed > runtime_loop_hz + 4) {
                    adjusted = std::min<uint32_t>(
                        loop_hz, runtime_loop_hz + 2);
                }
                if (adjusted != runtime_loop_hz) {
                    runtime_loop_hz = adjusted;
                    period = std::chrono::microseconds(1000000 / runtime_loop_hz);
                    std::cout << "[INFO] Adaptive loop_hz adjusted to "
                              << runtime_loop_hz << "\n";
                }
            }

            std::cout << "[INFO] fps cam1=" << fps0 << " cam2=" << fps1
                      << " loop_hz=" << runtime_loop_hz
                      << " dropped_cycles=" << dropped_cycles
                      << " stale_cycles=" << stale_cycles
                      << " stale_ms=[" << stale0_ms << "," << stale1_ms << "]"
                      << " frame_id_delta="
                      << ((latest_frame_id0 > latest_frame_id1)
                              ? (latest_frame_id0 - latest_frame_id1)
                              : (latest_frame_id1 - latest_frame_id0))
                      << "\n";
            report_tick = now;
        }

        const auto sleep_now = std::chrono::steady_clock::now();
        if (next_tick > sleep_now) {
            std::this_thread::sleep_until(next_tick);
        } else {
            next_tick = sleep_now;
        }
    }

    for (auto &worker : workers) {
        worker.stop.store(true, std::memory_order_relaxed);
    }
    for (auto &thread : capture_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    bool ok = !fatal;
    for (auto &cam : cameras) {
        status = GXStreamOff(cam.device);
        if (status != GX_STATUS_SUCCESS) {
            std::cerr << "[WARN] GXStreamOff failed for camera " << cam.index << ": "
                      << GetErrorString(status) << "\n";
            ok = false;
        }
    }

    for (auto &cam : cameras) {
        std::cout << "[INFO] Camera " << cam.index << " summary: ok_frames="
                  << cam.ok_frames << " bad_frames=" << cam.bad_frames
                  << " timeouts=" << cam.timeouts << "\n";
        if (cam.ok_frames == 0) {
            ok = false;
        }
        CloseCamera(&cam);
    }

    status = GXCloseLib();
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] GXCloseLib failed: " << GetErrorString(status) << "\n";
        ok = false;
    }

    if (!headless) {
        cv::destroyAllWindows();
    }

    if (!ok) {
        std::cerr << "[ERR ] Pulsar side-by-side run failed.\n";
        return 3;
    }

    std::cout << "[INFO] Pulsar side-by-side run passed. cycles=" << cycles
              << " dropped_cycles=" << dropped_cycles
              << " stale_cycles=" << stale_cycles << "\n";
    return 0;
}
