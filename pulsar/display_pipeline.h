#pragma once

#include "camera_pipeline.h"
#include "settings.h"

#include <atomic>

bool DisplayBackendAvailable();

void RunDisplayLoop(const Options& opt,
                    CamContext* cam0,
                    CamContext* cam1,
                    std::atomic<bool>* run_flag);
