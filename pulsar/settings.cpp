#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void PrintUsage(const char* argv0) {
    std::printf("Usage: %s [options]\n", argv0);
    std::printf("\n");
    std::printf("Options:\n");
    std::printf("  --sn0 <SN>               Serial number for camera 0\n");
    std::printf("  --sn1 <SN>               Serial number for camera 1\n");
    std::printf("  --sync <mode>            free | external | master | action | scheduled (default: free)\n");
    std::printf("  --trigger-source <src>   TriggerSource line for external/master mode (default: Line1)\n");
    std::printf("  --fps <n>                Trigger FPS for action/scheduled mode (default: 30)\n");
    std::printf("  --buffers <n>            Stream buffer count (default: 2)\n");
    std::printf("  --packet-size <n>        GEV packet size, e.g. 8192 (default: 0=keep current)\n");
    std::printf("  --exposure-us <f>        Exposure time in microseconds (default: 34000)\n");
    std::printf("  --gain-db <f>            Gain in dB (default: 5)\n");
    std::printf("  --max-delta-us <n>       Max allowed timestamp delta in us (default: 2000, 0=disable)\n");
    std::printf("  --broadcast <ip>         Action command broadcast IP (default: 255.255.255.255)\n");
    std::printf("  --special-ip <ip>        Action command source IP (default: empty)\n");
    std::printf("  --action-lead-ms <n>     Lead time for scheduled action (default: 5)\n");
    std::printf("  --display-mode <mode>    dual | single (default: dual)\n");
    std::printf("  --mon0-width <n>         Monitor 0 width (default: 1920)\n");
    std::printf("  --mon0-height <n>        Monitor 0 height (default: 1080)\n");
    std::printf("  --mon0-x <n>             Monitor 0 window X position (default: 0)\n");
    std::printf("  --mon0-y <n>             Monitor 0 window Y position (default: 0)\n");
    std::printf("  --mon1-width <n>         Monitor 1 width (default: 3840)\n");
    std::printf("  --mon1-height <n>        Monitor 1 height (default: 1080)\n");
    std::printf("  --mon1-x <n>             Monitor 1 window X position (default: 1920)\n");
    std::printf("  --mon1-y <n>             Monitor 1 window Y position (default: 0)\n");
    std::printf("  --single-window          Shortcut for --display-mode single\n");
    std::printf("  --dual-window            Shortcut for --display-mode dual\n");
    std::printf("  --strict-pair            Always require both cameras to provide new frame (default: on)\n");
    std::printf("  --relaxed-pair           Allow mixed-age pairs in free mode\n");
    std::printf("  --ultra-low-latency      Favor minimum latency (default: on)\n");
    std::printf("  --balanced-latency       Favor balanced CPU/quality over minimum latency\n");
    std::printf("  --no-display             Disable GUI display\n");
    std::printf("  --verbose                Print extra diagnostics\n");
    std::printf("  --help                   Show this message\n");
    std::printf("\n");
    std::printf("Examples:\n");
    std::printf("  %s\n", argv0);
    std::printf("  %s --sync external --trigger-source Line2\n", argv0);
    std::printf("  %s --sync master --trigger-source Line2\n", argv0);
    std::printf("  %s --sync action --fps 60 --packet-size 8192\n", argv0);
    std::printf("  %s --display-mode dual --mon0-width 1920 --mon1-width 3840\n", argv0);
}

bool ParseArgs(int argc, char** argv, Options* opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--sn0") {
            const char* v = next_value("--sn0");
            if (!v) return false;
            opt->sn0 = v;
        } else if (arg == "--sn1") {
            const char* v = next_value("--sn1");
            if (!v) return false;
            opt->sn1 = v;
        } else if (arg == "--sync") {
            const char* v = next_value("--sync");
            if (!v) return false;
            opt->sync_mode = v;
        } else if (arg == "--trigger-source") {
            const char* v = next_value("--trigger-source");
            if (!v) return false;
            opt->trigger_source = v;
        } else if (arg == "--fps") {
            const char* v = next_value("--fps");
            if (!v) return false;
            opt->fps = std::atoi(v);
        } else if (arg == "--buffers") {
            const char* v = next_value("--buffers");
            if (!v) return false;
            opt->buffers = std::atoi(v);
        } else if (arg == "--packet-size") {
            const char* v = next_value("--packet-size");
            if (!v) return false;
            opt->packet_size = std::atoi(v);
        } else if (arg == "--exposure-us") {
            const char* v = next_value("--exposure-us");
            if (!v) return false;
            opt->exposure_us = std::atof(v);
        } else if (arg == "--gain-db") {
            const char* v = next_value("--gain-db");
            if (!v) return false;
            opt->gain_db = std::atof(v);
        } else if (arg == "--max-delta-us") {
            const char* v = next_value("--max-delta-us");
            if (!v) return false;
            opt->max_delta_us = std::atoi(v);
        } else if (arg == "--broadcast") {
            const char* v = next_value("--broadcast");
            if (!v) return false;
            opt->broadcast_ip = v;
        } else if (arg == "--special-ip") {
            const char* v = next_value("--special-ip");
            if (!v) return false;
            opt->special_ip = v;
        } else if (arg == "--action-lead-ms") {
            const char* v = next_value("--action-lead-ms");
            if (!v) return false;
            opt->action_lead_ms = std::atoi(v);
        } else if (arg == "--display-mode") {
            const char* v = next_value("--display-mode");
            if (!v) return false;
            opt->display_mode = v;
        } else if (arg == "--single-window") {
            opt->display_mode = "single";
        } else if (arg == "--dual-window") {
            opt->display_mode = "dual";
        } else if (arg == "--strict-pair") {
            opt->strict_pair = true;
        } else if (arg == "--relaxed-pair") {
            opt->strict_pair = false;
        } else if (arg == "--ultra-low-latency") {
            opt->ultra_low_latency = true;
        } else if (arg == "--balanced-latency") {
            opt->ultra_low_latency = false;
        } else if (arg == "--mon0-width") {
            const char* v = next_value("--mon0-width");
            if (!v) return false;
            opt->mon0_width = std::atoi(v);
        } else if (arg == "--mon0-height") {
            const char* v = next_value("--mon0-height");
            if (!v) return false;
            opt->mon0_height = std::atoi(v);
        } else if (arg == "--mon0-x") {
            const char* v = next_value("--mon0-x");
            if (!v) return false;
            opt->mon0_x = std::atoi(v);
        } else if (arg == "--mon0-y") {
            const char* v = next_value("--mon0-y");
            if (!v) return false;
            opt->mon0_y = std::atoi(v);
        } else if (arg == "--mon1-width") {
            const char* v = next_value("--mon1-width");
            if (!v) return false;
            opt->mon1_width = std::atoi(v);
        } else if (arg == "--mon1-height") {
            const char* v = next_value("--mon1-height");
            if (!v) return false;
            opt->mon1_height = std::atoi(v);
        } else if (arg == "--mon1-x") {
            const char* v = next_value("--mon1-x");
            if (!v) return false;
            opt->mon1_x = std::atoi(v);
        } else if (arg == "--mon1-y") {
            const char* v = next_value("--mon1-y");
            if (!v) return false;
            opt->mon1_y = std::atoi(v);
        } else if (arg == "--no-display") {
            opt->display = false;
        } else if (arg == "--verbose") {
            opt->verbose = true;
        } else if (arg == "--help") {
            return false;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

bool ValidateOptions(Options* opt, std::string* error) {
    if (opt->sync_mode != "free" &&
        opt->sync_mode != "external" &&
        opt->sync_mode != "master" &&
        opt->sync_mode != "action" &&
        opt->sync_mode != "scheduled") {
        if (error) {
            *error = "Invalid --sync mode: " + opt->sync_mode;
        }
        return false;
    }

    if (opt->display_mode != "dual" && opt->display_mode != "single") {
        if (error) {
            *error = "Invalid --display-mode: " + opt->display_mode;
        }
        return false;
    }

    opt->buffers = std::clamp(opt->buffers, 1, 8);
    opt->fps = std::max(opt->fps, 1);
    opt->max_delta_us = std::max(opt->max_delta_us, 0);
    opt->action_lead_ms = std::max(opt->action_lead_ms, 1);
    if (opt->exposure_us <= 0.0) {
        opt->exposure_us = 34000.0;
    }
    if (opt->gain_db < 0.0) {
        opt->gain_db = 5.0;
    }
    opt->mon0_width = std::max(opt->mon0_width, 64);
    opt->mon0_height = std::max(opt->mon0_height, 64);
    opt->mon1_width = std::max(opt->mon1_width, 64);
    opt->mon1_height = std::max(opt->mon1_height, 64);
    return true;
}

bool AutoTuneForSystemMemory(Options* opt, uint64_t* total_mem_mb) {
    if (total_mem_mb) {
        *total_mem_mb = 0;
    }

    FILE* fp = std::fopen("/proc/meminfo", "r");
    if (!fp) {
        return false;
    }

    char key[64] = {0};
    unsigned long long kb = 0;
    char unit[32] = {0};
    unsigned long long mem_kb = 0;
    while (std::fscanf(fp, "%63s %llu %31s", key, &kb, unit) == 3) {
        if (std::strcmp(key, "MemTotal:") == 0) {
            mem_kb = kb;
            break;
        }
    }
    std::fclose(fp);

    if (mem_kb == 0) {
        return false;
    }

    const uint64_t mem_mb = static_cast<uint64_t>(mem_kb / 1024ULL);
    if (total_mem_mb) {
        *total_mem_mb = mem_mb;
    }

    // Safe profile for small-memory systems (around 2 GB): keep queue short
    // to reduce RAM use and end-to-end latency.
    if (mem_mb <= 2500ULL) {
        if (opt->buffers > 2) {
            opt->buffers = 2;
        }
        if (mem_mb <= 1800ULL && opt->buffers > 1) {
            opt->buffers = 1;
        }
        return true;
    }

    return false;
}
