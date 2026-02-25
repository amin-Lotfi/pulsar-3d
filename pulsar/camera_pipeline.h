#pragma once

#include "settings.h"

#include "DxImageProc.h"
#include "GxIAPI.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

struct FrameSlot {
    std::atomic<uint32_t> seq{0};
    std::vector<uint8_t> bgr;
    int width = 0;
    int height = 0;
    uint64_t timestamp = 0;
    uint64_t frame_id = 0;
};

struct CamContext {
    GX_DEV_HANDLE dev = nullptr;
    GX_DS_HANDLE ds = nullptr;
    GX_DEVICE_CLASS dev_type = GX_DEVICE_CLASS_UNKNOWN;
    DX_IMAGE_FORMAT_CONVERT_HANDLE cvt = nullptr;
    int out_size = 0;
    uint64_t tick_freq = 0;
    bool convert_to_bgr = true;
    bool swap_rb = false;
    int index = -1;
    std::string sn;
    FrameSlot slots[2];
    std::atomic<int> latest_idx{-1};
    std::thread th;
};

struct FrameMeta {
    int width = 0;
    int height = 0;
    uint64_t timestamp = 0;
    uint64_t frame_id = 0;
};

void PrintGxError(const char* msg, GX_STATUS status);

std::string GetDeviceSN(const GX_DEVICE_INFO& info);
bool OpenDeviceBySN(const std::string& sn, GX_DEV_HANDLE* out_dev);
bool OpenDeviceByIndex(int index, GX_DEV_HANDLE* out_dev);

bool ForceSameResolution(CamContext* cam0,
                         CamContext* cam1,
                         int preferred_width,
                         int preferred_height,
                         bool verbose);
bool ForceSameImageSettings(CamContext* cam0, CamContext* cam1, const Options& opt, bool verbose);
bool SetupCamera(CamContext* cam, const Options& opt);
void CaptureLoop(CamContext* cam,
                 bool verbose,
                 bool ultra_low_latency,
                 const std::atomic<bool>* run_flag);

bool EnablePtpAndWait(const std::vector<CamContext*>& cams, int timeout_sec);
void ActionTriggerLoop(const Options& opt,
                       const std::vector<CamContext*>& cams,
                       bool scheduled,
                       const std::atomic<bool>* run_flag);

void StopAndClose(CamContext* cam0, CamContext* cam1);
