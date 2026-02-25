#include "camera_pipeline.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <thread>

namespace {

bool NodeAvailable(GX_PORT_HANDLE port, const char* name) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS st = GXGetNodeAccessMode(port, name, &mode);
    if (st != GX_STATUS_SUCCESS) {
        return false;
    }
    return mode == GX_NODE_ACCESS_MODE_RO ||
           mode == GX_NODE_ACCESS_MODE_WO ||
           mode == GX_NODE_ACCESS_MODE_RW;
}

bool NodeWritable(GX_PORT_HANDLE port, const char* name) {
    GX_NODE_ACCESS_MODE mode = GX_NODE_ACCESS_MODE_NI;
    GX_STATUS st = GXGetNodeAccessMode(port, name, &mode);
    if (st != GX_STATUS_SUCCESS) {
        return false;
    }
    return mode == GX_NODE_ACCESS_MODE_WO || mode == GX_NODE_ACCESS_MODE_RW;
}

bool SetEnumByString(GX_PORT_HANDLE port, const char* name, const char* value) {
    GX_STATUS st = GXSetEnumValueByString(port, name, value);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXSetEnumValueByString failed", st);
        return false;
    }
    return true;
}

bool SetEnumValueRaw(GX_PORT_HANDLE port, const char* name, int64_t value) {
    GX_STATUS st = GXSetEnumValue(port, name, value);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXSetEnumValue failed", st);
        return false;
    }
    return true;
}

bool SetIntValue(GX_PORT_HANDLE port, const char* name, int64_t value) {
    GX_STATUS st = GXSetIntValue(port, name, value);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXSetIntValue failed", st);
        return false;
    }
    return true;
}

bool SetFloatValue(GX_PORT_HANDLE port, const char* name, double value) {
    GX_STATUS st = GXSetFloatValue(port, name, value);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXSetFloatValue failed", st);
        return false;
    }
    return true;
}

bool SetBoolValue(GX_PORT_HANDLE port, const char* name, bool value) {
    GX_STATUS st = GXSetBoolValue(port, name, value);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXSetBoolValue failed", st);
        return false;
    }
    return true;
}

bool GetIntNodeValue(GX_PORT_HANDLE port, const char* name, GX_INT_VALUE* out) {
    GX_STATUS st = GXGetIntValue(port, name, out);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXGetIntValue failed", st);
        return false;
    }
    return true;
}

bool GetIntCurValue(GX_PORT_HANDLE port, const char* name, int64_t* out) {
    GX_INT_VALUE v{};
    if (!GetIntNodeValue(port, name, &v)) {
        return false;
    }
    *out = v.nCurValue;
    return true;
}

bool IntNodeAccepts(const GX_INT_VALUE& node, int64_t value) {
    if (value < node.nMin || value > node.nMax) {
        return false;
    }
    const int64_t inc = (node.nInc > 0) ? node.nInc : 1;
    return ((value - node.nMin) % inc) == 0;
}

bool FindCommonIntValue(const GX_INT_VALUE& a, const GX_INT_VALUE& b, int64_t prefer, int64_t* out) {
    const int64_t lo = (a.nMin > b.nMin) ? a.nMin : b.nMin;
    const int64_t hi = (a.nMax < b.nMax) ? a.nMax : b.nMax;
    if (lo > hi) {
        return false;
    }

    if (prefer < lo) prefer = lo;
    if (prefer > hi) prefer = hi;

    for (int64_t v = prefer; v >= lo; --v) {
        if (IntNodeAccepts(a, v) && IntNodeAccepts(b, v)) {
            *out = v;
            return true;
        }
    }
    for (int64_t v = prefer + 1; v <= hi; ++v) {
        if (IntNodeAccepts(a, v) && IntNodeAccepts(b, v)) {
            *out = v;
            return true;
        }
    }

    return false;
}

bool SetResolutionOnDevice(GX_DEV_HANDLE dev, int64_t width, int64_t height) {
    if (NodeAvailable(dev, "OffsetX")) {
        SetIntValue(dev, "OffsetX", 0);
    }
    if (NodeAvailable(dev, "OffsetY")) {
        SetIntValue(dev, "OffsetY", 0);
    }

    if (SetIntValue(dev, "Width", width) && SetIntValue(dev, "Height", height)) {
        return true;
    }
    if (SetIntValue(dev, "Height", height) && SetIntValue(dev, "Width", width)) {
        return true;
    }
    return false;
}

bool GetEnumValue(GX_PORT_HANDLE port, const char* name, GX_ENUM_VALUE* out) {
    GX_STATUS st = GXGetEnumValue(port, name, out);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXGetEnumValue failed", st);
        return false;
    }
    return true;
}

bool EnumHasSymbolic(const GX_ENUM_VALUE& v, const char* name) {
    for (uint32_t i = 0; i < v.nSupportedNum; ++i) {
        if (std::strcmp(v.nArrySupportedValue[i].strCurSymbolic, name) == 0) {
            return true;
        }
    }
    return false;
}

bool EnumHasRawValue(const GX_ENUM_VALUE& v, int64_t value) {
    for (uint32_t i = 0; i < v.nSupportedNum; ++i) {
        if (v.nArrySupportedValue[i].nCurValue == value) {
            return true;
        }
    }
    return false;
}

bool TrySetEnumPreferred(GX_PORT_HANDLE port,
                         const char* feature,
                         const std::vector<std::string>& choices,
                         std::string* chosen) {
    GX_ENUM_VALUE ev{};
    if (!GetEnumValue(port, feature, &ev)) {
        return false;
    }
    for (const auto& c : choices) {
        if (EnumHasSymbolic(ev, c.c_str())) {
            if (SetEnumByString(port, feature, c.c_str())) {
                if (chosen) {
                    *chosen = c;
                }
                return true;
            }
        }
    }
    return false;
}

bool GetFloatNodeValue(GX_PORT_HANDLE port, const char* name, GX_FLOAT_VALUE* out) {
    GX_STATUS st = GXGetFloatValue(port, name, out);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXGetFloatValue failed", st);
        return false;
    }
    return true;
}

bool GetFloatCurValue(GX_PORT_HANDLE port, const char* name, double* out) {
    GX_FLOAT_VALUE v{};
    if (!GetFloatNodeValue(port, name, &v)) {
        return false;
    }
    *out = v.dCurValue;
    return true;
}

bool TrySetEnumOff(GX_PORT_HANDLE port, const char* feature) {
    if (!NodeAvailable(port, feature) || !NodeWritable(port, feature)) {
        return true;
    }

    if (TrySetEnumPreferred(port, feature, {"Off"}, nullptr)) {
        return true;
    }

    GX_ENUM_VALUE ev{};
    if (!GetEnumValue(port, feature, &ev)) {
        return false;
    }
    if (!EnumHasRawValue(ev, 0)) {
        return false;
    }
    return SetEnumValueRaw(port, feature, 0);
}

double ClampDouble(double v, double lo, double hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

uint64_t GetTimestampLatchValue(GX_DEV_HANDLE dev) {
    GX_STATUS st = GXSetCommandValue(dev, "TimestampLatch");
    if (st != GX_STATUS_SUCCESS) {
        return 0;
    }
    int64_t ts = 0;
    if (!GetIntCurValue(dev, "TimestampLatchValue", &ts)) {
        return 0;
    }
    return static_cast<uint64_t>(ts);
}

void TryRaiseCurrentThreadPriority(bool ultra_low_latency, bool verbose, int priority, const char* tag) {
    if (!ultra_low_latency) {
        return;
    }
    sched_param sp{};
    sp.sched_priority = priority;
    const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0 && verbose) {
        std::fprintf(stderr,
                     "%s: pthread_setschedparam(SCHED_FIFO,%d) failed: %s\n",
                     tag,
                     priority,
                     std::strerror(rc));
    }
}

}  // namespace

void PrintGxError(const char* msg, GX_STATUS status) {
    std::fprintf(stderr, "%s (GX_STATUS=%d)\n", msg, status);
    size_t sz = 0;
    GX_STATUS st = GXGetLastError(&status, nullptr, &sz);
    if (st != GX_STATUS_SUCCESS || sz == 0) {
        return;
    }
    std::vector<char> buf(sz);
    st = GXGetLastError(&status, buf.data(), &sz);
    if (st == GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "%s\n", buf.data());
    }
}

std::string GetDeviceSN(const GX_DEVICE_INFO& info) {
    switch (info.emDevType) {
        case GX_DEVICE_CLASS_GEV:
            return reinterpret_cast<const char*>(info.DevInfo.stGEVDevInfo.chSerialNumber);
        case GX_DEVICE_CLASS_U3V:
            return reinterpret_cast<const char*>(info.DevInfo.stU3VDevInfo.chSerialNumber);
        case GX_DEVICE_CLASS_USB2:
            return reinterpret_cast<const char*>(info.DevInfo.stUSBDevInfo.chSerialNumber);
        case GX_DEVICE_CLASS_CXP:
            return reinterpret_cast<const char*>(info.DevInfo.stCXPDevInfo.chSerialNumber);
        default:
            return std::string();
    }
}

bool OpenDeviceBySN(const std::string& sn, GX_DEV_HANDLE* out_dev) {
    GX_OPEN_PARAM p{};
    p.openMode = GX_OPEN_SN;
    p.accessMode = GX_ACCESS_EXCLUSIVE;
    p.pszContent = const_cast<char*>(sn.c_str());
    GX_STATUS st = GXOpenDevice(&p, out_dev);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXOpenDevice by SN failed", st);
        return false;
    }
    return true;
}

bool OpenDeviceByIndex(int index, GX_DEV_HANDLE* out_dev) {
    char idx[32] = {0};
    std::snprintf(idx, sizeof(idx), "%d", index);
    GX_OPEN_PARAM p{};
    p.openMode = GX_OPEN_INDEX;
    p.accessMode = GX_ACCESS_EXCLUSIVE;
    p.pszContent = idx;
    GX_STATUS st = GXOpenDevice(&p, out_dev);
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXOpenDevice by index failed", st);
        return false;
    }
    return true;
}

bool ForceSameResolution(CamContext* cam0,
                         CamContext* cam1,
                         int preferred_width,
                         int preferred_height,
                         bool verbose) {
    GX_INT_VALUE w0{};
    GX_INT_VALUE h0{};
    GX_INT_VALUE w1{};
    GX_INT_VALUE h1{};
    if (!GetIntNodeValue(cam0->dev, "Width", &w0) ||
        !GetIntNodeValue(cam0->dev, "Height", &h0) ||
        !GetIntNodeValue(cam1->dev, "Width", &w1) ||
        !GetIntNodeValue(cam1->dev, "Height", &h1)) {
        std::fprintf(stderr, "Failed to read Width/Height capability\n");
        return false;
    }

    int64_t target_w = 0;
    int64_t target_h = 0;
    const int64_t prefer_w = std::max<int64_t>(preferred_width, 1);
    const int64_t prefer_h = std::max<int64_t>(preferred_height, 1);
    if (!FindCommonIntValue(w0, w1, prefer_w, &target_w)) {
        const int64_t fallback = (w0.nCurValue < w1.nCurValue) ? w0.nCurValue : w1.nCurValue;
        if (!FindCommonIntValue(w0, w1, fallback, &target_w)) {
            std::fprintf(stderr, "No common Width between cameras\n");
            return false;
        }
    }
    if (!FindCommonIntValue(h0, h1, prefer_h, &target_h)) {
        const int64_t fallback = (h0.nCurValue < h1.nCurValue) ? h0.nCurValue : h1.nCurValue;
        if (!FindCommonIntValue(h0, h1, fallback, &target_h)) {
            std::fprintf(stderr, "No common Height between cameras\n");
            return false;
        }
    }

    if (!SetResolutionOnDevice(cam0->dev, target_w, target_h) ||
        !SetResolutionOnDevice(cam1->dev, target_w, target_h)) {
        std::fprintf(stderr, "Failed to apply common resolution\n");
        return false;
    }

    int64_t rw0 = 0;
    int64_t rh0 = 0;
    int64_t rw1 = 0;
    int64_t rh1 = 0;
    if (!GetIntCurValue(cam0->dev, "Width", &rw0) ||
        !GetIntCurValue(cam0->dev, "Height", &rh0) ||
        !GetIntCurValue(cam1->dev, "Width", &rw1) ||
        !GetIntCurValue(cam1->dev, "Height", &rh1)) {
        return false;
    }

    if (rw0 != rw1 || rh0 != rh1) {
        std::fprintf(stderr,
                     "Could not lock same resolution (cam0=%lldx%lld cam1=%lldx%lld)\n",
                     static_cast<long long>(rw0),
                     static_cast<long long>(rh0),
                     static_cast<long long>(rw1),
                     static_cast<long long>(rh1));
        return false;
    }

    std::fprintf(stdout,
                 "Locked both cameras at %lldx%lld\n",
                 static_cast<long long>(rw0),
                 static_cast<long long>(rh0));
    if (verbose) {
        std::fflush(stdout);
    }
    return true;
}

bool ForceSameImageSettings(CamContext* cam0, CamContext* cam1, const Options& opt, bool verbose) {
    auto sync_float_feature = [&](const char* feature, double requested_target) -> bool {
        const bool has0 = NodeAvailable(cam0->dev, feature);
        const bool has1 = NodeAvailable(cam1->dev, feature);
        if (!has0 || !has1) {
            if (verbose) {
                std::fprintf(stdout,
                             "Skip %s pair-lock (cam0=%s cam1=%s)\n",
                             feature,
                             has0 ? "yes" : "no",
                             has1 ? "yes" : "no");
            }
            return true;
        }

        GX_FLOAT_VALUE fv0{};
        GX_FLOAT_VALUE fv1{};
        if (!GetFloatNodeValue(cam0->dev, feature, &fv0) || !GetFloatNodeValue(cam1->dev, feature, &fv1)) {
            std::fprintf(stderr, "Failed to read %s limits\n", feature);
            return false;
        }
        if (!NodeWritable(cam0->dev, feature) || !NodeWritable(cam1->dev, feature)) {
            std::fprintf(stderr, "%s is not writable on both cameras\n", feature);
            return false;
        }

        const double common_min = std::max(fv0.dMin, fv1.dMin);
        const double common_max = std::min(fv0.dMax, fv1.dMax);
        if (common_min > common_max) {
            std::fprintf(stderr, "No shared %s range between cameras\n", feature);
            return false;
        }

        double target = ClampDouble(requested_target, common_min, common_max);
        if (!SetFloatValue(cam0->dev, feature, target) || !SetFloatValue(cam1->dev, feature, target)) {
            std::fprintf(stderr, "Failed to set %s on both cameras\n", feature);
            return false;
        }

        double cur0 = target;
        double cur1 = target;
        if (GetFloatCurValue(cam0->dev, feature, &cur0) && GetFloatCurValue(cam1->dev, feature, &cur1)) {
            const double mismatch = std::fabs(cur0 - cur1);
            if (mismatch > 1e-6) {
                const double retry_target = std::min(cur0, cur1);
                if (std::fabs(retry_target - target) > 1e-6) {
                    SetFloatValue(cam0->dev, feature, retry_target);
                    SetFloatValue(cam1->dev, feature, retry_target);
                    GetFloatCurValue(cam0->dev, feature, &cur0);
                    GetFloatCurValue(cam1->dev, feature, &cur1);
                }
            }
        }

        if (verbose) {
            std::fprintf(stdout, "Pair-lock %s: cam0=%.4f cam1=%.4f\n", feature, cur0, cur1);
        }
        return true;
    };

    const double exposure_ceiling_us = std::max(200.0, 900000.0 / static_cast<double>(std::max(opt.fps, 1)));
    const double target_exposure = std::min(opt.exposure_us, exposure_ceiling_us);

    if (!TrySetEnumOff(cam0->dev, "ExposureAuto") || !TrySetEnumOff(cam1->dev, "ExposureAuto")) {
        std::fprintf(stderr, "Failed to disable ExposureAuto for pair-lock\n");
        return false;
    }
    if (!TrySetEnumOff(cam0->dev, "GainAuto") || !TrySetEnumOff(cam1->dev, "GainAuto")) {
        std::fprintf(stderr, "Failed to disable GainAuto for pair-lock\n");
        return false;
    }

    if (!sync_float_feature("ExposureTime", target_exposure)) {
        return false;
    }
    if (!sync_float_feature("Gain", opt.gain_db)) {
        return false;
    }

    auto try_pair_lock_white_balance = [&]() {
        constexpr const char* kSelector = "BalanceRatioSelector";
        constexpr const char* kRatio = "BalanceRatio";
        const bool have_nodes =
            NodeAvailable(cam0->dev, kSelector) &&
            NodeAvailable(cam1->dev, kSelector) &&
            NodeAvailable(cam0->dev, kRatio) &&
            NodeAvailable(cam1->dev, kRatio);
        if (!have_nodes) {
            if (verbose) {
                std::fprintf(stdout, "Skip BalanceRatio pair-lock (unsupported on one camera)\n");
            }
            return;
        }

        const bool writable =
            NodeWritable(cam0->dev, kSelector) &&
            NodeWritable(cam1->dev, kSelector) &&
            NodeWritable(cam0->dev, kRatio) &&
            NodeWritable(cam1->dev, kRatio);
        if (!writable) {
            if (verbose) {
                std::fprintf(stdout, "Skip BalanceRatio pair-lock (not writable)\n");
            }
            return;
        }

        const std::array<const char*, 3> labels = {"Red", "Green", "Blue"};
        const std::array<std::vector<std::string>, 3> aliases = {
            std::vector<std::string>{"Red", "R", "RedChannel"},
            std::vector<std::string>{"Green", "G", "GreenChannel"},
            std::vector<std::string>{"Blue", "B", "BlueChannel"},
        };
        const std::array<int64_t, 3> raw_selectors = {0, 1, 2};

        auto select_channel = [&](GX_DEV_HANDLE dev, int idx) -> bool {
            if (TrySetEnumPreferred(dev, kSelector, aliases[idx], nullptr)) {
                return true;
            }
            GX_ENUM_VALUE ev{};
            if (!GetEnumValue(dev, kSelector, &ev)) {
                return false;
            }
            if (!EnumHasRawValue(ev, raw_selectors[idx])) {
                return false;
            }
            return SetEnumValueRaw(dev, kSelector, raw_selectors[idx]);
        };

        std::array<double, 3> ratios0 = {};
        std::array<double, 3> ratios1 = {};
        std::array<double, 3> common_min = {};
        std::array<double, 3> common_max = {};

        for (int i = 0; i < 3; ++i) {
            if (!select_channel(cam0->dev, i) || !select_channel(cam1->dev, i)) {
                if (verbose) {
                    std::fprintf(stdout, "Skip BalanceRatio pair-lock (selector unsupported for %s)\n", labels[i]);
                }
                return;
            }

            GX_FLOAT_VALUE fv0{};
            GX_FLOAT_VALUE fv1{};
            if (!GetFloatNodeValue(cam0->dev, kRatio, &fv0) || !GetFloatNodeValue(cam1->dev, kRatio, &fv1)) {
                if (verbose) {
                    std::fprintf(stdout, "Skip BalanceRatio pair-lock (cannot read limits)\n");
                }
                return;
            }

            ratios0[i] = fv0.dCurValue;
            ratios1[i] = fv1.dCurValue;
            GetFloatCurValue(cam0->dev, kRatio, &ratios0[i]);
            GetFloatCurValue(cam1->dev, kRatio, &ratios1[i]);
            common_min[i] = std::max(fv0.dMin, fv1.dMin);
            common_max[i] = std::min(fv0.dMax, fv1.dMax);
            if (common_min[i] > common_max[i]) {
                if (verbose) {
                    std::fprintf(stdout, "Skip BalanceRatio pair-lock (no shared range for %s)\n", labels[i]);
                }
                return;
            }
        }

        auto wb_score = [](const std::array<double, 3>& r) -> double {
            const double g = (r[1] > 1e-9) ? r[1] : 1e-9;
            return std::fabs(((r[0] + r[2]) / (2.0 * g)) - 1.0);
        };

        const double score0 = wb_score(ratios0);
        const double score1 = wb_score(ratios1);
        const bool use_cam0_reference = (score0 <= score1);

        for (int i = 0; i < 3; ++i) {
            if (!select_channel(cam0->dev, i) || !select_channel(cam1->dev, i)) {
                std::fprintf(stderr, "BalanceRatio pair-lock failed while selecting %s channel\n", labels[i]);
                return;
            }

            const double ref_value = use_cam0_reference ? ratios0[i] : ratios1[i];
            const double target = ClampDouble(ref_value, common_min[i], common_max[i]);
            if (!SetFloatValue(cam0->dev, kRatio, target) || !SetFloatValue(cam1->dev, kRatio, target)) {
                std::fprintf(stderr, "BalanceRatio pair-lock failed while writing %s channel\n", labels[i]);
                return;
            }
            if (verbose) {
                std::fprintf(stdout, "Pair-lock BalanceRatio[%s]=%.4f (ref=cam%d)\n",
                             labels[i],
                             target,
                             use_cam0_reference ? 0 : 1);
            }
        }
    };

    try_pair_lock_white_balance();

    return true;
}

bool SetupCamera(CamContext* cam, const Options& opt) {
    if (!SetEnumByString(cam->dev, "AcquisitionMode", "Continuous")) {
        return false;
    }

    if (NodeAvailable(cam->dev, "TriggerSelector") && NodeWritable(cam->dev, "TriggerSelector")) {
        TrySetEnumPreferred(cam->dev, "TriggerSelector", {"FrameStart", "AcquisitionStart"}, nullptr);
    }

    auto append_unique = [](std::vector<std::string>* values, const std::string& v) {
        if (v.empty()) {
            return;
        }
        if (std::find(values->begin(), values->end(), v) == values->end()) {
            values->push_back(v);
        }
    };

    const bool master_is_free_run = (opt.sync_mode == "master" && cam->index == 0);
    const bool trigger_enabled = !(opt.sync_mode == "free" || master_is_free_run);
    std::string effective_trigger_source = opt.trigger_source;

    if (!trigger_enabled) {
        if (!SetEnumByString(cam->dev, "TriggerMode", "Off")) {
            return false;
        }
    } else {
        if (!SetEnumByString(cam->dev, "TriggerMode", "On")) {
            return false;
        }
        if (opt.sync_mode == "action" || opt.sync_mode == "scheduled") {
            if (!SetEnumByString(cam->dev, "TriggerSource", "Action0")) {
                return false;
            }
            effective_trigger_source = "Action0";
        } else {
            std::vector<std::string> trigger_choices;
            append_unique(&trigger_choices, opt.trigger_source);
            append_unique(&trigger_choices, "Line1");
            append_unique(&trigger_choices, "Line2");
            append_unique(&trigger_choices, "Line3");
            append_unique(&trigger_choices, "Line4");
            append_unique(&trigger_choices, "Software");

            std::string chosen_source;
            if (!TrySetEnumPreferred(cam->dev, "TriggerSource", trigger_choices, &chosen_source)) {
                std::fprintf(stderr, "No supported TriggerSource for camera %d\n", cam->index);
                return false;
            }
            if (!chosen_source.empty()) {
                effective_trigger_source = chosen_source;
            }
            if (opt.verbose && !effective_trigger_source.empty() &&
                effective_trigger_source != opt.trigger_source) {
                std::fprintf(stdout,
                             "Camera %d TriggerSource fallback: requested=%s selected=%s\n",
                             cam->index,
                             opt.trigger_source.c_str(),
                             effective_trigger_source.c_str());
            }
        }

        if (NodeAvailable(cam->dev, "TriggerActivation") && NodeWritable(cam->dev, "TriggerActivation")) {
            TrySetEnumPreferred(cam->dev, "TriggerActivation", {"RisingEdge", "AnyEdge", "FallingEdge"}, nullptr);
        }
        if (NodeAvailable(cam->dev, "TriggerDelay") && NodeWritable(cam->dev, "TriggerDelay")) {
            SetFloatValue(cam->dev, "TriggerDelay", 0.0);
        }
    }

    if (opt.sync_mode == "master" &&
        NodeAvailable(cam->dev, "LineSelector") &&
        NodeWritable(cam->dev, "LineSelector")) {
        std::vector<std::string> line_choices;
        append_unique(&line_choices, effective_trigger_source);
        append_unique(&line_choices, opt.trigger_source);
        append_unique(&line_choices, "Line1");
        append_unique(&line_choices, "Line2");
        append_unique(&line_choices, "Line3");
        append_unique(&line_choices, "Line4");

        std::string chosen_line;
        if (TrySetEnumPreferred(cam->dev, "LineSelector", line_choices, &chosen_line)) {
            if (opt.verbose && !chosen_line.empty() && chosen_line != opt.trigger_source) {
                std::fprintf(stdout,
                             "Camera %d LineSelector fallback: requested=%s selected=%s\n",
                             cam->index,
                             opt.trigger_source.c_str(),
                             chosen_line.c_str());
            }
            if (cam->index == 0) {
                if (NodeAvailable(cam->dev, "LineMode") && NodeWritable(cam->dev, "LineMode")) {
                    TrySetEnumPreferred(cam->dev, "LineMode", {"Output"}, nullptr);
                }
                if (NodeAvailable(cam->dev, "LineSource") && NodeWritable(cam->dev, "LineSource")) {
                    TrySetEnumPreferred(cam->dev,
                                        "LineSource",
                                        {"ExposureActive", "Strobe", "Timer1Active"},
                                        nullptr);
                }
            } else {
                if (NodeAvailable(cam->dev, "LineMode") && NodeWritable(cam->dev, "LineMode")) {
                    TrySetEnumPreferred(cam->dev, "LineMode", {"Input"}, nullptr);
                }
            }
        } else if (opt.verbose) {
            std::fprintf(stdout, "Camera %d: skip master line wiring (LineSelector unavailable values)\n", cam->index);
        }
    }

    if (opt.buffers > 0) {
        GX_STATUS st = GXSetAcqusitionBufferNumber(cam->dev, static_cast<uint64_t>(opt.buffers));
        if (st != GX_STATUS_SUCCESS) {
            PrintGxError("GXSetAcqusitionBufferNumber failed", st);
            return false;
        }
    }

    if (NodeAvailable(cam->dev, "StreamTransferSize")) {
        SetIntValue(cam->dev, "StreamTransferSize", 64 * 1024);
    }
    if (NodeAvailable(cam->dev, "StreamTransferNumberUrb")) {
        SetIntValue(cam->dev, "StreamTransferNumberUrb", 64);
    }

    if (NodeAvailable(cam->dev, "DeviceLinkThroughputLimitMode") &&
        NodeWritable(cam->dev, "DeviceLinkThroughputLimitMode")) {
        TrySetEnumPreferred(cam->dev, "DeviceLinkThroughputLimitMode", {"Off"}, nullptr);
    }

    if (NodeAvailable(cam->dev, "GevSCPSPacketSize")) {
        if (opt.packet_size > 0) {
            SetIntValue(cam->dev, "GevSCPSPacketSize", opt.packet_size);
        } else if (cam->dev_type == GX_DEVICE_CLASS_GEV) {
            uint32_t optimal = 0;
            GX_STATUS st_opt = GXGetOptimalPacketSize(cam->dev, &optimal);
            if (st_opt == GX_STATUS_SUCCESS && optimal > 0) {
                SetIntValue(cam->dev, "GevSCPSPacketSize", static_cast<int64_t>(optimal));
            }
        }
    }

    if (opt.sync_mode == "action" || opt.sync_mode == "scheduled") {
        if (NodeAvailable(cam->dev, "ActionDeviceKey")) {
            SetIntValue(cam->dev, "ActionDeviceKey", 1);
        }
        if (NodeAvailable(cam->dev, "ActionGroupKey")) {
            SetIntValue(cam->dev, "ActionGroupKey", 1);
        }
        if (NodeAvailable(cam->dev, "ActionGroupMask")) {
            SetIntValue(cam->dev, "ActionGroupMask", 0xFFFFFFFF);
        }
    }

    const bool has_color_filter = NodeAvailable(cam->dev, "PixelColorFilter");
    std::string preferred_bayer;
    if (has_color_filter && NodeAvailable(cam->dev, "PixelColorFilter")) {
        GX_ENUM_VALUE cfa{};
        if (GetEnumValue(cam->dev, "PixelColorFilter", &cfa)) {
            const char* cfa_sym = cfa.stCurValue.strCurSymbolic;
            if (std::strstr(cfa_sym, "RG")) preferred_bayer = "BayerRG8";
            else if (std::strstr(cfa_sym, "BG")) preferred_bayer = "BayerBG8";
            else if (std::strstr(cfa_sym, "GR")) preferred_bayer = "BayerGR8";
            else if (std::strstr(cfa_sym, "GB")) preferred_bayer = "BayerGB8";
        }
    }
    std::vector<std::string> prefs;
    if (has_color_filter) {
        prefs = {"BayerRG8", "BayerBG8", "BayerGR8", "BayerGB8"};
        if (!preferred_bayer.empty()) {
            prefs.insert(prefs.begin(), preferred_bayer);
        }
        prefs.push_back("BGR8");
        prefs.push_back("RGB8");
    } else {
        prefs = {"Mono8"};
    }

    std::string selected_pf;
    if (NodeAvailable(cam->dev, "PixelFormat")) {
        TrySetEnumPreferred(cam->dev, "PixelFormat", prefs, &selected_pf);
        if (opt.verbose && !selected_pf.empty()) {
            std::fprintf(stdout, "PixelFormat=%s\n", selected_pf.c_str());
        }
    }

    cam->swap_rb = false;
    if (has_color_filter) {
        cam->swap_rb = true;
        if (selected_pf == "BGR8" || selected_pf == "BGR8Packed") {
            cam->swap_rb = false;
        }
    }

    auto apply_enum_named = [&](const char* feature,
                                const std::vector<std::string>& preferred,
                                int64_t raw_fallback) -> bool {
        if (!NodeAvailable(cam->dev, feature) || !NodeWritable(cam->dev, feature)) {
            return false;
        }
        std::string chosen;
        if (TrySetEnumPreferred(cam->dev, feature, preferred, &chosen)) {
            if (opt.verbose && !chosen.empty()) {
                std::fprintf(stdout, "%s=%s\n", feature, chosen.c_str());
            }
            return true;
        }
        if (raw_fallback < 0) {
            return false;
        }
        GX_ENUM_VALUE ev{};
        if (!GetEnumValue(cam->dev, feature, &ev)) {
            return false;
        }
        if (!EnumHasRawValue(ev, raw_fallback)) {
            return false;
        }
        if (SetEnumValueRaw(cam->dev, feature, raw_fallback)) {
            if (opt.verbose) {
                std::fprintf(stdout, "%s=%lld\n", feature, static_cast<long long>(raw_fallback));
            }
            return true;
        }
        return false;
    };

    auto apply_float = [&](const char* feature, double value) {
        if (!NodeAvailable(cam->dev, feature)) {
            if (opt.verbose) {
                std::fprintf(stdout, "Skip %s (not available)\n", feature);
            }
            return;
        }
        GX_FLOAT_VALUE fv{};
        GX_STATUS st_f = GXGetFloatValue(cam->dev, feature, &fv);
        if (st_f != GX_STATUS_SUCCESS) {
            if (opt.verbose) {
                PrintGxError("GXGetFloatValue failed", st_f);
            }
            return;
        }
        double target = value;
        if (target < fv.dMin) target = fv.dMin;
        if (target > fv.dMax) target = fv.dMax;
        if (!NodeWritable(cam->dev, feature)) {
            if (opt.verbose) {
                std::fprintf(stderr, "Skip %s=%.4f (read-only now)\n", feature, target);
            }
            return;
        }
        if (SetFloatValue(cam->dev, feature, target) && opt.verbose) {
            std::fprintf(stdout, "%s=%.4f\n", feature, target);
        }
    };

    if (opt.sync_mode == "free") {
        apply_enum_named("AcquisitionFrameRateMode", {"On"}, 1);
        apply_float("AcquisitionFrameRate", static_cast<double>(std::max(opt.fps, 1)));
    } else {
        apply_enum_named("AcquisitionFrameRateMode", {"Off"}, 0);
    }

    const double exposure_ceiling_us = std::max(200.0, 900000.0 / static_cast<double>(std::max(opt.fps, 1)));
    apply_enum_named("ExposureAuto", {"Off"}, 0);
    apply_float("ExposureTime", std::min(opt.exposure_us, exposure_ceiling_us));

    apply_enum_named("GainAuto", {"Off"}, 0);
    apply_float("Gain", opt.gain_db);
    apply_float("BlackLevel", 1.0);

    if (has_color_filter) {
        apply_enum_named("LightSourcePreset", {"Auto", "Daylight", "OfficeFluorescent", "Incandescent"}, -1);
        apply_enum_named("BalanceWhiteAuto", {"Once", "Off"}, 2);
    }

    const bool enable_iq = opt.display;
    if (NodeWritable(cam->dev, "GammaEnable")) {
        SetBoolValue(cam->dev, "GammaEnable", enable_iq);
    }
    if (has_color_filter && NodeWritable(cam->dev, "ColorTransformationEnable")) {
        SetBoolValue(cam->dev, "ColorTransformationEnable", enable_iq);
    }

    uint32_t ds_num = 0;
    if (GXGetDataStreamNumFromDev(cam->dev, &ds_num) != GX_STATUS_SUCCESS || ds_num < 1) {
        std::fprintf(stderr, "No data stream found on camera\n");
        return false;
    }
    if (GXGetDataStreamHandleFromDev(cam->dev, 1, &cam->ds) != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "GXGetDataStreamHandleFromDev failed\n");
        return false;
    }

    if (NodeAvailable(cam->dev, "TimestampTickFrequency")) {
        int64_t tf = 0;
        if (GetIntCurValue(cam->dev, "TimestampTickFrequency", &tf) && tf > 0) {
            cam->tick_freq = static_cast<uint64_t>(tf);
        }
    }

    int64_t width = 0;
    int64_t height = 0;
    if (!GetIntCurValue(cam->dev, "Width", &width) || !GetIntCurValue(cam->dev, "Height", &height)) {
        std::fprintf(stderr, "Failed to get Width/Height\n");
        return false;
    }

    cam->convert_to_bgr = opt.display;
    if (cam->convert_to_bgr) {
        if (DxImageFormatConvertCreate(&cam->cvt) != DX_OK) {
            std::fprintf(stderr, "DxImageFormatConvertCreate failed\n");
            return false;
        }
        DxImageFormatConvertSetOutputPixelFormat(cam->cvt, GX_PIXEL_FORMAT_BGR8);
        const DX_BAYER_CONVERT_TYPE cvt_type = opt.ultra_low_latency ? RAW2RGB_NEIGHBOUR : RAW2RGB_ADAPTIVE;
        DxImageFormatConvertSetInterpolationType(cam->cvt, cvt_type);

        int out_bytes = 0;
        if (DxImageFormatConvertGetBufferSizeForConversion(
                cam->cvt,
                GX_PIXEL_FORMAT_BGR8,
                static_cast<VxUint32>(width),
                static_cast<VxUint32>(height),
                &out_bytes) != DX_OK) {
            out_bytes = static_cast<int>(width * height * 3);
        }
        cam->out_size = out_bytes;
    } else {
        cam->out_size = 0;
    }

    for (auto& slot : cam->slots) {
        if (cam->convert_to_bgr) {
            slot.bgr.resize(static_cast<size_t>(cam->out_size));
        } else {
            slot.bgr.clear();
            slot.bgr.shrink_to_fit();
        }
        slot.width = static_cast<int>(width);
        slot.height = static_cast<int>(height);
    }

    return true;
}

void CaptureLoop(CamContext* cam,
                 bool verbose,
                 bool ultra_low_latency,
                 const std::atomic<bool>* run_flag) {
    TryRaiseCurrentThreadPriority(ultra_low_latency, verbose, 70, "CaptureLoop");
    const uint32_t dq_timeout_ms = ultra_low_latency ? 100U : 1000U;

    while (run_flag->load(std::memory_order_relaxed)) {
        PGX_FRAME_BUFFER frame = nullptr;
        GX_STATUS st = GXDQBuf(cam->dev, &frame, dq_timeout_ms);
        if (st == GX_STATUS_TIMEOUT) {
            continue;
        }
        if (st != GX_STATUS_SUCCESS) {
            PrintGxError("GXDQBuf failed", st);
            break;
        }

        if (frame && frame->nStatus == GX_FRAME_STATUS_SUCCESS) {
            const int write_idx = (cam->latest_idx.load(std::memory_order_relaxed) + 1) & 1;
            FrameSlot& slot = cam->slots[write_idx];

            const uint32_t seq = slot.seq.load(std::memory_order_relaxed);
            slot.seq.store(seq + 1, std::memory_order_release);

            bool frame_ready = true;
            if (cam->convert_to_bgr) {
                const VxInt32 dx = DxImageFormatConvert(
                    cam->cvt,
                    frame->pImgBuf,
                    frame->nImgSize,
                    slot.bgr.data(),
                    cam->out_size,
                    static_cast<GX_PIXEL_FORMAT_ENTRY>(frame->nPixelFormat),
                    static_cast<VxUint32>(frame->nWidth),
                    static_cast<VxUint32>(frame->nHeight),
                    false);
                frame_ready = (dx == DX_OK);
                if (!frame_ready && verbose) {
                    std::fprintf(stderr, "DxImageFormatConvert failed: %d\n", dx);
                }

                if (frame_ready && cam->swap_rb) {
                    const size_t bytes =
                        static_cast<size_t>(frame->nWidth) * static_cast<size_t>(frame->nHeight) * 3U;
                    uint8_t* p = slot.bgr.data();
                    for (size_t i = 0; i + 2 < bytes; i += 3) {
                        const uint8_t t = p[i];
                        p[i] = p[i + 2];
                        p[i + 2] = t;
                    }
                }
            }

            if (frame_ready) {
                slot.width = static_cast<int>(frame->nWidth);
                slot.height = static_cast<int>(frame->nHeight);
                slot.timestamp = frame->nTimestamp;
                slot.frame_id = frame->nFrameID;
                cam->latest_idx.store(write_idx, std::memory_order_release);
            }

            slot.seq.store(seq + 2, std::memory_order_release);
        }

        if (frame) {
            GXQBuf(cam->dev, frame);
        }
    }
}

bool EnablePtpAndWait(const std::vector<CamContext*>& cams, int timeout_sec) {
    for (CamContext* cam : cams) {
        if (!NodeAvailable(cam->dev, "PtpEnable")) {
            std::fprintf(stderr, "PTP is not supported on one camera\n");
            return false;
        }
        if (!SetBoolValue(cam->dev, "PtpEnable", true)) {
            return false;
        }
    }

    for (int i = 0; i < timeout_sec; ++i) {
        GX_ENUM_VALUE ev{};
        if (!GetEnumValue(cams[0]->dev, "PtpStatus", &ev)) {
            return false;
        }
        const char* status = ev.stCurValue.strCurSymbolic;
        if (std::strcmp(status, "Master") == 0 || std::strcmp(status, "Slave") == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::fprintf(stderr, "PTP role assignment timeout\n");
    return false;
}

void ActionTriggerLoop(const Options& opt,
                       const std::vector<CamContext*>& cams,
                       bool scheduled,
                       const std::atomic<bool>* run_flag) {
    if (opt.fps <= 0) {
        return;
    }

    constexpr uint32_t kDeviceKey = 1;
    constexpr uint32_t kGroupKey = 1;
    constexpr uint32_t kGroupMask = 0xFFFFFFFF;
    const auto period = std::chrono::nanoseconds(1000000000LL / static_cast<long long>(opt.fps));
    const uint64_t lead_ns = static_cast<uint64_t>(opt.action_lead_ms) * 1000000ULL;
    auto next_fire = std::chrono::steady_clock::now();

    while (run_flag->load(std::memory_order_relaxed)) {
        GX_STATUS issue_status = GX_STATUS_SUCCESS;
        if (scheduled) {
            const uint64_t now_ns = GetTimestampLatchValue(cams[0]->dev);
            if (now_ns == 0) {
                next_fire += period;
                std::this_thread::sleep_until(next_fire);
                continue;
            }
            const uint64_t action_time = now_ns + lead_ns;
            issue_status = GXGigEIssueScheduledActionCommand(
                kDeviceKey,
                kGroupKey,
                kGroupMask,
                action_time,
                opt.broadcast_ip.c_str(),
                opt.special_ip.empty() ? "" : opt.special_ip.c_str(),
                0,
                nullptr,
                nullptr);
        } else {
            issue_status = GXGigEIssueActionCommand(
                kDeviceKey,
                kGroupKey,
                kGroupMask,
                opt.broadcast_ip.c_str(),
                opt.special_ip.empty() ? "" : opt.special_ip.c_str(),
                0,
                nullptr,
                nullptr);
        }

        if (issue_status != GX_STATUS_SUCCESS && opt.verbose) {
            PrintGxError("Issue action command failed", issue_status);
        }

        next_fire += period;
        const auto now = std::chrono::steady_clock::now();
        if (next_fire <= now) {
            next_fire = now + period;
        } else {
            std::this_thread::sleep_until(next_fire);
        }
    }
}

void StopAndClose(CamContext* cam0, CamContext* cam1) {
    if (cam0 && cam0->dev) {
        GXStreamOff(cam0->dev);
    }
    if (cam1 && cam1->dev) {
        GXStreamOff(cam1->dev);
    }

    if (cam0 && cam0->cvt) {
        DxImageFormatConvertDestroy(cam0->cvt);
        cam0->cvt = nullptr;
    }
    if (cam1 && cam1->cvt) {
        DxImageFormatConvertDestroy(cam1->cvt);
        cam1->cvt = nullptr;
    }

    if (cam0) {
        for (auto& slot : cam0->slots) {
            slot.bgr.clear();
            slot.bgr.shrink_to_fit();
        }
    }
    if (cam1) {
        for (auto& slot : cam1->slots) {
            slot.bgr.clear();
            slot.bgr.shrink_to_fit();
        }
    }

    if (cam0 && cam0->dev) {
        GXCloseDevice(cam0->dev);
        cam0->dev = nullptr;
    }
    if (cam1 && cam1->dev) {
        GXCloseDevice(cam1->dev);
        cam1->dev = nullptr;
    }
}
