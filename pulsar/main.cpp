#include "GxIAPI.h"
#include "DxImageProc.h"
#include "GxPixelFormat.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kTargetCameras = 2;
constexpr uint32_t kAcqBufferNum = 2;
constexpr int64_t kStreamTransferSize = 1024 * 1024;
constexpr int64_t kStreamTransferUrb = 64;
constexpr uint32_t kEnumTimeoutMs = 1000;

constexpr int kViewWidth = 1920;
constexpr int kViewHeight = 1080;
constexpr int kCanvasWidth = kViewWidth * 2;
constexpr int kCanvasHeight = kViewHeight;

constexpr uint32_t kDefaultTriggerHz = 60;
constexpr uint32_t kMinTriggerHz = 1;
constexpr uint32_t kMaxTriggerHz = 120;
constexpr double kDefaultExposureUs = 34000.0;
constexpr double kExposureGuardUs = 500.0;

constexpr double kPresetGain = 5.0;
constexpr double kPresetBlackLevel = 1.0;
constexpr double kPresetBalanceRatio = 2.0;
constexpr int64_t kPresetLightSourcePreset = 3;
constexpr int64_t kPresetBalanceWhiteAuto = 1;
constexpr int64_t kPresetExposureAuto = 0;
constexpr int64_t kPresetAcquisitionFrameRateMode = 1;

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

    std::vector<unsigned char> raw8_buffer;
    std::vector<unsigned char> rgb_buffer;
};

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

bool NodeIsWritable(GX_PORT_HANDLE handle, const char *node_name) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS) {
        return false;
    }
    return mode == GX_NODE_ACCESS_MODE_RW;
}

void TrySetIntNode(GX_PORT_HANDLE handle, const char *node_name, int64_t value) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || !IsReadableOrWritable(mode)) {
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
    if (status != GX_STATUS_SUCCESS || !IsReadableOrWritable(mode)) {
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

double RealtimeSafeExposureUs(uint32_t trigger_hz, double requested_exposure_us) {
    const double frame_period_us = 1000000.0 / static_cast<double>(trigger_hz);
    const double upper = std::max(1.0, frame_period_us - kExposureGuardUs);
    return std::max(1.0, std::min(requested_exposure_us, upper));
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

bool ConvertFrameToBgr(PGX_FRAME_BUFFER frame, CameraContext *cam, cv::Mat *out_bgr) {
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
        bgr.copyTo(*out_bgr);
        return true;
    }

    if (pixfmt == GX_PIXEL_FORMAT_RGB8) {
        cv::Mat rgb(height, width, CV_8UC3, frame->pImgBuf);
        cv::cvtColor(rgb, *out_bgr, cv::COLOR_RGB2BGR);
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
        cv::cvtColor(rgb, *out_bgr, cv::COLOR_RGB2BGR);
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
        cv::cvtColor(rgb, *out_bgr, cv::COLOR_RGB2BGR);
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

bool ConfigureCamera(CameraContext *ctx, uint32_t trigger_hz, double exposure_us) {
    ConfigureRoi1920x1080(ctx->device);

    // Prefer 8-bit output to reduce conversion cost and display latency.
    TrySetEnumNode(ctx->device, "PixelFormat", FastPixelFormatForCamera(*ctx));

    if (!SetEnumNodeStrict(ctx->device, "AcquisitionMode", "Continuous")) {
        return false;
    }
    if (!SetEnumNodeStrict(ctx->device, "TriggerMode", "On")) {
        return false;
    }
    if (!SetEnumNodeStrict(ctx->device, "TriggerSource", "Software")) {
        return false;
    }

    GX_STATUS status =
        GXSetAcqusitionBufferNumber(ctx->device, static_cast<uint64_t>(kAcqBufferNum));
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
    TrySetFloatNode(ctx->device, "Gain", kPresetGain);
    TrySetFloatNode(ctx->device, "BlackLevel", kPresetBlackLevel);
    TrySetEnumNodeByInt(ctx->device, "LightSourcePreset", kPresetLightSourcePreset);
    TrySetEnumNodeByInt(ctx->device, "BalanceWhiteAuto", kPresetBalanceWhiteAuto);
    TrySetFloatNode(ctx->device, "BalanceRatio", kPresetBalanceRatio);

    const double exposure_realtime_safe = RealtimeSafeExposureUs(trigger_hz, exposure_us);
    if (exposure_realtime_safe + 0.5 < exposure_us) {
        std::cout << "[WARN] Camera " << ctx->index << " exposure " << exposure_us
                  << " us is too long for " << trigger_hz
                  << " fps; clamped to " << exposure_realtime_safe
                  << " us for low-latency.\n";
    }
    if (NodeIsWritable(ctx->device, "ExposureTime")) {
        GX_FLOAT_VALUE exposure_range{};
        status = GXGetFloatValue(ctx->device, "ExposureTime", &exposure_range);
        if (status == GX_STATUS_SUCCESS) {
            const double fitted =
                FitFloatToNodeRange(exposure_range, exposure_realtime_safe);
            TrySetFloatNode(ctx->device, "ExposureTime", fitted);
        }
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

bool FetchLatestFrameAsBgr(CameraContext *ctx, uint32_t timeout_ms,
                           cv::Mat *frame_bgr, uint64_t *frame_id,
                           uint64_t *timestamp) {
    std::array<PGX_FRAME_BUFFER, kAcqBufferNum> frame_array{};
    uint32_t frame_count = 0;
    GX_STATUS status = GXDQAllBufs(ctx->device, frame_array.data(),
                                   static_cast<uint32_t>(frame_array.size()),
                                   &frame_count, timeout_ms);

    if (status == GX_STATUS_TIMEOUT) {
        ++ctx->timeouts;
        return false;
    }
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] GXDQAllBufs failed for camera " << ctx->index << ": "
                  << GetErrorString(status) << "\n";
        return false;
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
        if (ConvertFrameToBgr(newest, ctx, frame_bgr)) {
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
        return false;
    }

    return ok;
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

    const char *exposure_env = std::getenv("PULSAR_EXPOSURE_US");
    double exposure_us = kDefaultExposureUs;
    if (exposure_env != nullptr) {
        const double parsed = std::atof(exposure_env);
        if (parsed > 1.0) {
            exposure_us = parsed;
        }
    }

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
            !ConfigureCamera(&cameras[i], trigger_hz, exposure_us)) {
            for (auto &cam : cameras) {
                CloseCamera(&cam);
            }
            GXCloseLib();
            return 1;
        }
        std::cout << "[INFO] Camera index " << cameras[i].index
                  << " opened: model=" << cameras[i].model
                  << " serial=" << cameras[i].serial << "\n";
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

    cv::Mat canvas(kCanvasHeight, kCanvasWidth, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat left_resized(kViewHeight, kViewWidth, CV_8UC3);
    cv::Mat right_resized(kViewHeight, kViewWidth, CV_8UC3);
    cv::Mat left_bgr;
    cv::Mat right_bgr;

    const std::string window_name = "Pulsar-SideBySide";
    if (!headless) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::setWindowProperty(window_name, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
    }

    const auto period = std::chrono::microseconds(1000000 / trigger_hz);
    const uint32_t fetch_timeout_ms =
        static_cast<uint32_t>(std::max<int64_t>(8, (1000 / trigger_hz) * 2));

    std::cout << "[INFO] Fullscreen side-by-side mode: each camera 1920x1080, "
              << "trigger_hz=" << trigger_hz << ", exposure_us=" << exposure_us
              << ", headless=" << (headless ? 1 : 0) << "\n";

    if (run_seconds == 0) {
        std::cout << "[INFO] Running continuously. Press ESC/q/Ctrl+C to stop.\n";
    } else {
        std::cout << "[INFO] Running for " << run_seconds << " seconds.\n";
    }

    uint64_t cycles = 0;
    uint64_t dropped_cycles = 0;
    uint64_t prev_ok0 = 0;
    uint64_t prev_ok1 = 0;

    auto next_tick = std::chrono::steady_clock::now();
    auto report_tick = next_tick;
    const auto end_time = (run_seconds > 0)
                              ? (next_tick + std::chrono::seconds(run_seconds))
                              : std::chrono::steady_clock::time_point::max();

    bool fatal = false;
    while (!g_stop_requested && std::chrono::steady_clock::now() < end_time) {
        next_tick += period;

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

        uint64_t frame_id0 = 0;
        uint64_t frame_id1 = 0;
        uint64_t ts0 = 0;
        uint64_t ts1 = 0;

        const bool got0 =
            FetchLatestFrameAsBgr(&cameras[0], fetch_timeout_ms, &left_bgr, &frame_id0, &ts0);
        const bool got1 =
            FetchLatestFrameAsBgr(&cameras[1], fetch_timeout_ms, &right_bgr, &frame_id1, &ts1);

        ++cycles;
        if (!(got0 && got1)) {
            ++dropped_cycles;
            continue;
        }

        if (left_bgr.cols != kViewWidth || left_bgr.rows != kViewHeight) {
            cv::resize(left_bgr, left_resized, cv::Size(kViewWidth, kViewHeight), 0.0, 0.0,
                       cv::INTER_LINEAR);
        } else {
            left_bgr.copyTo(left_resized);
        }

        if (right_bgr.cols != kViewWidth || right_bgr.rows != kViewHeight) {
            cv::resize(right_bgr, right_resized, cv::Size(kViewWidth, kViewHeight), 0.0,
                       0.0, cv::INTER_LINEAR);
        } else {
            right_bgr.copyTo(right_resized);
        }

        left_resized.copyTo(canvas(cv::Rect(0, 0, kViewWidth, kViewHeight)));
        right_resized.copyTo(
            canvas(cv::Rect(kViewWidth, 0, kViewWidth, kViewHeight)));

        if (!headless) {
            cv::imshow(window_name, canvas);
            const int key = cv::waitKey(1);
            if (key == 27 || key == 'q' || key == 'Q') {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - report_tick >= std::chrono::seconds(1)) {
            const uint64_t fps0 = cameras[0].ok_frames - prev_ok0;
            const uint64_t fps1 = cameras[1].ok_frames - prev_ok1;
            prev_ok0 = cameras[0].ok_frames;
            prev_ok1 = cameras[1].ok_frames;

            std::cout << "[INFO] fps cam1=" << fps0 << " cam2=" << fps1
                      << " dropped_cycles=" << dropped_cycles
                      << " frame_id_delta="
                      << ((frame_id0 > frame_id1) ? (frame_id0 - frame_id1)
                                                  : (frame_id1 - frame_id0))
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
              << " dropped_cycles=" << dropped_cycles << "\n";
    return 0;
}
