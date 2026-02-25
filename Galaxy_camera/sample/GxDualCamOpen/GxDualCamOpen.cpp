#include "GxIAPI.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kAcqBufferNum = 8;
constexpr int64_t kStreamTransferSize = 64 * 1024;
constexpr int64_t kStreamTransferUrb = 64;
constexpr uint32_t kEnumTimeoutMs = 1000;
constexpr uint32_t kGrabTimeoutMs = 200;

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

void TrySetIntNode(GX_PORT_HANDLE handle, const char *node_name, int64_t value) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS status = GXGetNodeAccessMode(handle, node_name, &mode);
    if (status != GX_STATUS_SUCCESS || !IsReadableOrWritable(mode)) {
        return;
    }
    status = GXSetIntValue(handle, node_name, value);
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] Failed to set " << node_name << ": " << GetErrorString(status) << "\n";
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

bool SetEnumNodeStrict(GX_PORT_HANDLE handle, const char *node_name, const char *value) {
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

struct CameraContext {
    uint32_t index = 0;
    GX_DEV_HANDLE device = nullptr;
    GX_DS_HANDLE stream = nullptr;
    std::string model;
    std::string serial;
    std::atomic<uint64_t> frames_ok{0};
    std::atomic<uint64_t> frames_bad{0};
    std::atomic<GX_STATUS> grab_status{GX_STATUS_SUCCESS};
    std::thread worker;
};

void GrabLoop(CameraContext *ctx, std::atomic<bool> *stop_flag) {
    auto last_report = std::chrono::steady_clock::now();
    uint64_t local_ok_frames = 0;
    std::array<PGX_FRAME_BUFFER, kAcqBufferNum> frame_array{};

    while (!stop_flag->load()) {
        uint32_t frame_count = 0;
        GX_STATUS status = GXDQAllBufs(ctx->device, frame_array.data(),
                                       static_cast<uint32_t>(frame_array.size()),
                                       &frame_count, kGrabTimeoutMs);
        if (status == GX_STATUS_TIMEOUT) {
            continue;
        }
        if (status != GX_STATUS_SUCCESS) {
            ctx->grab_status.store(status);
            std::cerr << "[ERR ] Camera " << ctx->index << " DQAllBufs failed: "
                      << GetErrorString(status) << "\n";
            break;
        }

        uint64_t ok_count = 0;
        uint64_t bad_count = 0;
        for (uint32_t i = 0; i < frame_count; ++i) {
            PGX_FRAME_BUFFER frame = frame_array[i];
            if (frame != nullptr && frame->nStatus == GX_FRAME_STATUS_SUCCESS) {
                ++ok_count;
            } else {
                ++bad_count;
            }
        }
        local_ok_frames += ok_count;
        ctx->frames_ok.fetch_add(ok_count);
        ctx->frames_bad.fetch_add(bad_count);

        // Return all acquired buffers immediately to keep newest-frame behavior.
        status = GXQAllBufs(ctx->device);
        if (status != GX_STATUS_SUCCESS) {
            ctx->grab_status.store(status);
            std::cerr << "[ERR ] Camera " << ctx->index << " QAllBufs failed: "
                      << GetErrorString(status) << "\n";
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            std::cout << "[INFO] Camera " << ctx->index << " (" << ctx->serial
                      << ") fps~" << local_ok_frames << "\n";
            local_ok_frames = 0;
            last_report = now;
        }
    }
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

bool OpenAndConfigure(CameraContext *ctx) {
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

    ctx->model = GetStringNode(ctx->device, "DeviceModelName");
    ctx->serial = GetStringNode(ctx->device, "DeviceSerialNumber");
    if (ctx->model.empty()) {
        ctx->model = "unknown-model";
    }
    if (ctx->serial.empty()) {
        ctx->serial = "unknown-serial";
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

    if (!SetEnumNodeStrict(ctx->device, "AcquisitionMode", "Continuous")) {
        return false;
    }
    if (!SetEnumNodeStrict(ctx->device, "TriggerMode", "Off")) {
        return false;
    }

    status = GXSetAcqusitionBufferNumber(ctx->device, static_cast<uint64_t>(kAcqBufferNum));
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[ERR ] Failed to set acquisition buffer number on camera "
                  << ctx->index << ": " << GetErrorString(status) << "\n";
        return false;
    }

    TrySetIntNode(ctx->stream, "StreamTransferSize", kStreamTransferSize);
    TrySetIntNode(ctx->stream, "StreamTransferNumberUrb", kStreamTransferUrb);
    TrySetEnumNode(ctx->stream, "StreamBufferHandlingMode", "NewestOnly");
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

}  // namespace

int main(int argc, char **argv) {
    uint32_t target_cameras = 2;
    int run_seconds = 10;

    if (argc >= 2) {
        run_seconds = std::atoi(argv[1]);
        if (run_seconds <= 0) {
            std::cerr << "Usage: " << argv[0] << " [seconds]\n";
            return 1;
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
    if (device_count < target_cameras) {
        std::cerr << "[ERR ] Need at least " << target_cameras << " cameras, found "
                  << device_count << "\n";
        GXCloseLib();
        return 2;
    }

    std::vector<uint32_t> indices = SelectCameraIndices(device_count, target_cameras);
    if (indices.size() < target_cameras) {
        std::cerr << "[ERR ] Could not select " << target_cameras << " devices.\n";
        GXCloseLib();
        return 2;
    }

    std::vector<CameraContext> cameras(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        cameras[i].index = indices[i];
        if (!OpenAndConfigure(&cameras[i])) {
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

    std::atomic<bool> stop_flag{false};
    for (auto &cam : cameras) {
        cam.worker = std::thread(GrabLoop, &cam, &stop_flag);
    }

    std::cout << "[INFO] Grabbing from " << cameras.size() << " cameras for "
              << run_seconds << " seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
    stop_flag.store(true);

    for (auto &cam : cameras) {
        if (cam.worker.joinable()) {
            cam.worker.join();
        }
    }

    bool ok = true;
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
                  << cam.frames_ok.load() << " bad_frames=" << cam.frames_bad.load()
                  << "\n";
        if (cam.frames_ok.load() == 0 || cam.grab_status.load() != GX_STATUS_SUCCESS) {
            ok = false;
        }
        CloseCamera(&cam);
    }

    status = GXCloseLib();
    if (status != GX_STATUS_SUCCESS) {
        std::cerr << "[WARN] GXCloseLib failed: " << GetErrorString(status) << "\n";
        ok = false;
    }

    if (!ok) {
        std::cerr << "[ERR ] Dual camera test failed.\n";
        return 3;
    }

    std::cout << "[INFO] Dual camera test passed.\n";
    return 0;
}
