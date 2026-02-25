#include "GxIAPI.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

namespace {

struct ProbeOptions {
    int min_cameras = 2;
    uint32_t timeout_ms = 1500;
    int retries = 3;
    uint32_t retry_sleep_ms = 500;
    std::string sdk_root;
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s [--min N] [--timeout-ms MS] [--retries N] [--retry-sleep-ms MS] "
                 "[--sdk-root PATH]\n",
                 argv0);
}

bool ParseIntArg(const char* text, int min_value, int* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    if (value < min_value || value > 1000000L) {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool ParseArgs(int argc, char** argv, ProbeOptions* opt) {
    if (opt == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--min") == 0) {
            if (i + 1 >= argc || !ParseIntArg(argv[++i], 1, &opt->min_cameras)) {
                return false;
            }
        } else if (std::strcmp(arg, "--timeout-ms") == 0) {
            int value = 0;
            if (i + 1 >= argc || !ParseIntArg(argv[++i], 1, &value)) {
                return false;
            }
            opt->timeout_ms = static_cast<uint32_t>(value);
        } else if (std::strcmp(arg, "--retries") == 0) {
            if (i + 1 >= argc || !ParseIntArg(argv[++i], 1, &opt->retries)) {
                return false;
            }
        } else if (std::strcmp(arg, "--retry-sleep-ms") == 0) {
            int value = 0;
            if (i + 1 >= argc || !ParseIntArg(argv[++i], 0, &value)) {
                return false;
            }
            opt->retry_sleep_ms = static_cast<uint32_t>(value);
        } else if (std::strcmp(arg, "--sdk-root") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            opt->sdk_root = argv[++i];
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            return false;
        }
    }
    return true;
}

void PrintGxError(const char* msg, GX_STATUS status) {
    std::fprintf(stderr, "%s (GX_STATUS=%d)\n", msg, status);
    size_t sz = 0;
    GX_STATUS st = GXGetLastError(&status, nullptr, &sz);
    if (st != GX_STATUS_SUCCESS || sz == 0) {
        return;
    }
    std::string buf(sz, '\0');
    st = GXGetLastError(&status, &buf[0], &sz);
    if (st == GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "%s\n", buf.c_str());
    }
}

const char* NonEmpty(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return "-";
    }
    return value;
}

const char* DeviceTypeToString(GX_DEVICE_CLASS type) {
    switch (type) {
        case GX_DEVICE_CLASS_USB2:
            return "USB2";
        case GX_DEVICE_CLASS_U3V:
            return "U3V";
        case GX_DEVICE_CLASS_GEV:
            return "GEV";
        case GX_DEVICE_CLASS_CXP:
            return "CXP";
        case GX_DEVICE_CLASS_SMART:
            return "SMART";
        default:
            return "UNKNOWN";
    }
}

const char* DeviceSN(const GX_DEVICE_INFO& info) {
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
            return "";
    }
}

const char* DeviceModel(const GX_DEVICE_INFO& info) {
    switch (info.emDevType) {
        case GX_DEVICE_CLASS_GEV:
            return reinterpret_cast<const char*>(info.DevInfo.stGEVDevInfo.chModelName);
        case GX_DEVICE_CLASS_U3V:
            return reinterpret_cast<const char*>(info.DevInfo.stU3VDevInfo.chModelName);
        case GX_DEVICE_CLASS_USB2:
            return reinterpret_cast<const char*>(info.DevInfo.stUSBDevInfo.chModelName);
        case GX_DEVICE_CLASS_CXP:
            return reinterpret_cast<const char*>(info.DevInfo.stCXPDevInfo.chModelName);
        default:
            return "";
    }
}

const char* DeviceVendor(const GX_DEVICE_INFO& info) {
    switch (info.emDevType) {
        case GX_DEVICE_CLASS_GEV:
            return reinterpret_cast<const char*>(info.DevInfo.stGEVDevInfo.chVendorName);
        case GX_DEVICE_CLASS_U3V:
            return reinterpret_cast<const char*>(info.DevInfo.stU3VDevInfo.chVendorName);
        case GX_DEVICE_CLASS_USB2:
            return reinterpret_cast<const char*>(info.DevInfo.stUSBDevInfo.chVendorName);
        case GX_DEVICE_CLASS_CXP:
            return reinterpret_cast<const char*>(info.DevInfo.stCXPDevInfo.chVendorName);
        default:
            return "";
    }
}

std::string IPv4ToString(uint32_t ip) {
    struct in_addr addr;
    addr.s_addr = ip;
    char text[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &addr, text, sizeof(text)) != nullptr) {
        return std::string(text);
    }
    return "unknown";
}

bool IsJetsonKernel49() {
    struct utsname uts {};
    if (uname(&uts) != 0) {
        return false;
    }
    if (std::strcmp(uts.machine, "aarch64") != 0) {
        return false;
    }
    return std::strncmp(uts.release, "4.9", 3) == 0;
}

bool FileExistsReadable(const char* path) {
    return access(path, R_OK) == 0;
}

void PrintTroubleshooting(const ProbeOptions& opt, int gev_count, int u3v_count, int usb2_count) {
    std::fprintf(stderr, "Troubleshooting:\n");
    if (!FileExistsReadable("/etc/Galaxy/cfg/log4cplus.properties")) {
        std::fprintf(stderr, "  - Galaxy system config not found (/etc/Galaxy/cfg/log4cplus.properties).\n");
        if (!opt.sdk_root.empty()) {
            std::fprintf(stderr, "    Run once and reboot:\n");
            std::fprintf(stderr, "      sudo %s/Galaxy_camera.run\n", opt.sdk_root.c_str());
            std::fprintf(stderr, "      sudo reboot\n");
        }
    }

    const bool has_u3v_rule = FileExistsReadable("/etc/udev/rules.d/99-galaxy-u3v.rules");
    if (!has_u3v_rule) {
        std::fprintf(stderr, "  - Missing /etc/udev/rules.d/99-galaxy-u3v.rules.\n");
        if (!opt.sdk_root.empty()) {
            std::fprintf(stderr, "    Install udev rule for non-root USB camera access:\n");
            std::fprintf(stderr, "      sudo cp %s/config/99-galaxy-u3v.rules /etc/udev/rules.d/\n", opt.sdk_root.c_str());
            std::fprintf(stderr, "      sudo udevadm control --reload-rules && sudo udevadm trigger\n");
        }
    }

    if (u3v_count > 0 || usb2_count > 0 || !has_u3v_rule) {
        std::fprintf(stderr, "  - For USB cameras: unplug/replug cameras after SDK install.\n");
        std::fprintf(stderr, "  - If USB permission rules were not applied, try running with sudo.\n");
        std::fprintf(stderr, "  - For many USB cameras, run SetUSBStack.sh once as root.\n");
    }

    if (gev_count > 0 || (u3v_count == 0 && usb2_count == 0)) {
        std::fprintf(stderr, "  - For GigE cameras: disable firewall on camera NIC and check rp_filter.\n");
        std::fprintf(stderr, "  - If needed:\n");
        std::fprintf(stderr, "      sudo sysctl net.ipv4.conf.all.rp_filter=0\n");
        std::fprintf(stderr, "      sudo sysctl net.ipv4.conf.<iface>.rp_filter=0\n");
    }

    if (IsJetsonKernel49() && FileExistsReadable("/etc/security/limits.d/galaxy-limits.conf")) {
        std::fprintf(stderr, "  - Jetson kernel 4.9 detected with galaxy-limits.conf enabled.\n");
        std::fprintf(stderr, "    Linux SDK FAQ (Jetson Nano/TX2 R32.2.3) suggests removing it if thread creation fails:\n");
        std::fprintf(stderr, "      sudo rm -f /etc/security/limits.d/galaxy-limits.conf\n");
        std::fprintf(stderr, "      sudo reboot\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    ProbeOptions opt;
    if (!ParseArgs(argc, argv, &opt)) {
        PrintUsage(argv[0]);
        return 1;
    }

    GX_STATUS st = GXInitLib();
    if (st != GX_STATUS_SUCCESS) {
        PrintGxError("GXInitLib failed in camera probe", st);
        return 2;
    }

    uint32_t dev_num = 0;
    for (int attempt = 1; attempt <= opt.retries; ++attempt) {
        st = GXUpdateAllDeviceList(&dev_num, opt.timeout_ms);
        if (st != GX_STATUS_SUCCESS) {
            PrintGxError("GXUpdateAllDeviceList failed in camera probe", st);
            GXCloseLib();
            return 3;
        }
        std::fprintf(stdout,
                     "Camera probe attempt %d/%d: detected %u camera(s)\n",
                     attempt,
                     opt.retries,
                     dev_num);
        if (dev_num >= static_cast<uint32_t>(opt.min_cameras)) {
            break;
        }
        if (attempt < opt.retries && opt.retry_sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.retry_sleep_ms));
        }
    }

    int gev_count = 0;
    int u3v_count = 0;
    int usb2_count = 0;

    if (dev_num > 0) {
        std::fprintf(stdout, "Detected devices:\n");
    }

    for (uint32_t i = 1; i <= dev_num; ++i) {
        GX_DEVICE_INFO info{};
        st = GXGetDeviceInfo(i, &info);
        if (st != GX_STATUS_SUCCESS) {
            std::fprintf(stdout, "  [%u] failed to read device info\n", i);
            continue;
        }

        if (info.emDevType == GX_DEVICE_CLASS_GEV) {
            ++gev_count;
        } else if (info.emDevType == GX_DEVICE_CLASS_U3V) {
            ++u3v_count;
        } else if (info.emDevType == GX_DEVICE_CLASS_USB2) {
            ++usb2_count;
        }

        const char* type = DeviceTypeToString(info.emDevType);
        const char* sn = NonEmpty(DeviceSN(info));
        const char* model = NonEmpty(DeviceModel(info));
        const char* vendor = NonEmpty(DeviceVendor(info));
        if (info.emDevType == GX_DEVICE_CLASS_GEV) {
            const std::string ip = IPv4ToString(info.DevInfo.stGEVDevInfo.nCurrentIp);
            std::fprintf(stdout,
                         "  [%u] type=%s sn=%s model=%s vendor=%s ip=%s\n",
                         i,
                         type,
                         sn,
                         model,
                         vendor,
                         ip.c_str());
        } else {
            std::fprintf(stdout,
                         "  [%u] type=%s sn=%s model=%s vendor=%s\n",
                         i,
                         type,
                         sn,
                         model,
                         vendor);
        }
    }

    std::fprintf(stdout, "Device type summary: GEV=%d U3V=%d USB2=%d\n", gev_count, u3v_count, usb2_count);

    GXCloseLib();

    if (dev_num < static_cast<uint32_t>(opt.min_cameras)) {
        std::fprintf(stderr,
                     "Need at least %d cameras for this app, found %u\n",
                     opt.min_cameras,
                     dev_num);
        PrintTroubleshooting(opt, gev_count, u3v_count, usb2_count);
        return 4;
    }

    return 0;
}
