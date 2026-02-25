#pragma once

#include <cstdint>
#include <string>

struct Options {
    std::string sn0;
    std::string sn1;
    std::string sync_mode = "free";  // free | external | master | action | scheduled
    std::string trigger_source = "Line1";
    std::string broadcast_ip = "255.255.255.255";
    std::string special_ip;
    int buffers = 2;
    int fps = 30;
    int max_delta_us = 2000;
    int action_lead_ms = 5;
    int packet_size = 0;  // GEV only
    double exposure_us = 34000.0;
    double gain_db = 5.0;
    std::string display_mode = "dual";  // dual | single
    int mon0_width = 1920;
    int mon0_height = 1080;
    int mon0_x = 0;
    int mon0_y = 0;
    int mon1_width = 3840;
    int mon1_height = 1080;
    int mon1_x = 1920;
    int mon1_y = 0;
    bool strict_pair = true;
    bool ultra_low_latency = true;
    bool display = true;
    bool verbose = false;
};

void PrintUsage(const char* argv0);
bool ParseArgs(int argc, char** argv, Options* opt);
bool ValidateOptions(Options* opt, std::string* error);
bool AutoTuneForSystemMemory(Options* opt, uint64_t* total_mem_mb);
