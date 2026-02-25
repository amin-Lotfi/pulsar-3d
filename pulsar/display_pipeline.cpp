#include "display_pipeline.h"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <vector>

#ifndef PULSAR_ENABLE_OPENCV
#if __has_include(<opencv2/opencv.hpp>)
#define PULSAR_ENABLE_OPENCV 1
#else
#define PULSAR_ENABLE_OPENCV 0
#endif
#endif

#if PULSAR_ENABLE_OPENCV
#include <opencv2/opencv.hpp>
#define HAVE_OPENCV 1
#else
#define HAVE_OPENCV 0
#endif

namespace {

constexpr const char* kWindowDualMon0 = "pulsar_monitor_0";
constexpr const char* kWindowDualMon1 = "pulsar_monitor_1";
constexpr const char* kWindowSingle = "pulsar_dual";

bool ReadSlotMeta(const FrameSlot& slot, FrameMeta* meta) {
    for (;;) {
        const uint32_t s1 = slot.seq.load(std::memory_order_acquire);
        if (s1 & 1U) {
            continue;
        }

        const int w = slot.width;
        const int h = slot.height;
        const uint64_t ts = slot.timestamp;
        const uint64_t fid = slot.frame_id;

        const uint32_t s2 = slot.seq.load(std::memory_order_acquire);
        if (s1 == s2) {
            if (w <= 0 || h <= 0) {
                return false;
            }
            if (meta) {
                meta->width = w;
                meta->height = h;
                meta->timestamp = ts;
                meta->frame_id = fid;
            }
            return true;
        }
    }
}

bool CopySlotToBuffer(const FrameSlot& slot, uint8_t* dst, int dst_stride, FrameMeta* meta) {
    for (;;) {
        const uint32_t s1 = slot.seq.load(std::memory_order_acquire);
        if (s1 & 1U) {
            continue;
        }

        const int w = slot.width;
        const int h = slot.height;
        if (w <= 0 || h <= 0) {
            return false;
        }

        const int row_bytes = w * 3;
        const uint8_t* src = slot.bgr.data();
        for (int y = 0; y < h; ++y) {
            std::memcpy(dst + y * dst_stride, src + y * row_bytes, static_cast<size_t>(row_bytes));
        }

        const uint32_t s2 = slot.seq.load(std::memory_order_acquire);
        if (s1 == s2) {
            if (meta) {
                meta->width = w;
                meta->height = h;
                meta->timestamp = slot.timestamp;
                meta->frame_id = slot.frame_id;
            }
            return true;
        }
    }
}

#if HAVE_OPENCV
int ChooseResizeInterpolation(int src_w, int src_h, int dst_w, int dst_h) {
    return (src_w > dst_w || src_h > dst_h) ? cv::INTER_AREA : cv::INTER_LINEAR;
}

void SetupWindow(const char* name, int width, int height, int x, int y) {
    cv::namedWindow(name, cv::WINDOW_NORMAL);
    cv::moveWindow(name, x, y);
    cv::resizeWindow(name, width, height);
    cv::setWindowProperty(name, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
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
#endif

}  // namespace

bool DisplayBackendAvailable() {
#if HAVE_OPENCV
    return true;
#else
    return false;
#endif
}

void RunDisplayLoop(const Options& opt,
                    CamContext* cam0,
                    CamContext* cam1,
                    std::atomic<bool>* run_flag) {
    const int w0 = cam0->slots[0].width;
    const int h0 = cam0->slots[0].height;
    const int w1 = cam1->slots[0].width;
    const int h1 = cam1->slots[0].height;
    if (w0 <= 0 || h0 <= 0 || w1 <= 0 || h1 <= 0) {
        std::fprintf(stderr, "Invalid camera resolution\n");
        run_flag->store(false, std::memory_order_relaxed);
        return;
    }
    if (w0 != w1 || h0 != h1) {
        std::fprintf(stderr,
                     "Camera resolutions are not equal after setup: left=%dx%d right=%dx%d\n",
                     w0,
                     h0,
                     w1,
                     h1);
        run_flag->store(false, std::memory_order_relaxed);
        return;
    }

    const uint64_t tick_freq = (cam0->tick_freq > 0) ? cam0->tick_freq : cam1->tick_freq;
    uint64_t max_delta_ticks = 0;
    if (tick_freq > 0 && opt.max_delta_us > 0) {
        max_delta_ticks = static_cast<uint64_t>(opt.max_delta_us) * tick_freq / 1000000ULL;
    }

    const int total_w = w0 + w1;
    const int total_h = h0;
    const bool do_display = opt.display;
    const bool dual_mode = (opt.display_mode == "dual");
    const bool require_both_new = opt.strict_pair || (opt.sync_mode != "free");
    const bool ultra = opt.ultra_low_latency;

    auto pause_brief = [&]() {
        if (ultra) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };
    auto pause_wait_frame = [&]() {
        if (ultra) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    };

#if !HAVE_OPENCV
    if (do_display) {
        std::fprintf(stderr,
                     "OpenCV not found at compile time. Rebuild with OpenCV or use --no-display.\n");
        run_flag->store(false, std::memory_order_relaxed);
        return;
    }
#endif

    const int combined_stride = total_w * 3;
    std::vector<uint8_t> combined;
    if (do_display) {
        combined.resize(static_cast<size_t>(total_w * total_h * 3), 0);
    }

#if HAVE_OPENCV
    TryRaiseCurrentThreadPriority(ultra, opt.verbose, 60, "DisplayLoop");

    if (do_display) {
        cv::setNumThreads(1);
        cv::setUseOptimized(true);
        if (dual_mode) {
            SetupWindow(kWindowDualMon0, opt.mon0_width, opt.mon0_height, opt.mon0_x, opt.mon0_y);
            SetupWindow(kWindowDualMon1, opt.mon1_width, opt.mon1_height, opt.mon1_x, opt.mon1_y);
        } else {
            SetupWindow(kWindowSingle, opt.mon0_width, opt.mon0_height, opt.mon0_x, opt.mon0_y);
        }
        cv::waitKey(1);
    }

    cv::Mat mon0_frame;
    cv::Mat mon1_frame;
    cv::Mat single_frame;
    cv::Mat mon0_blank;
    cv::Mat mon1_blank;
    cv::Mat single_blank;
    bool need_resize_mon0 = false;
    bool need_resize_mon1 = false;
    bool need_resize_single = false;
    int interp_mon0 = cv::INTER_LINEAR;
    int interp_mon1 = cv::INTER_LINEAR;
    int interp_single = cv::INTER_LINEAR;

    if (do_display) {
        if (dual_mode) {
            need_resize_mon0 = (total_w != opt.mon0_width || total_h != opt.mon0_height);
            need_resize_mon1 = (total_w != opt.mon1_width || total_h != opt.mon1_height);
            if (ultra) {
                const bool exact_half_preview =
                    (opt.mon0_width * 2 == total_w) && (opt.mon0_height == total_h);
                interp_mon0 = exact_half_preview ? cv::INTER_NEAREST : cv::INTER_LINEAR;
                interp_mon1 = cv::INTER_LINEAR;
            } else {
                interp_mon0 = ChooseResizeInterpolation(total_w, total_h, opt.mon0_width, opt.mon0_height);
                interp_mon1 = ChooseResizeInterpolation(total_w, total_h, opt.mon1_width, opt.mon1_height);
            }
            if (need_resize_mon0) {
                mon0_frame.create(opt.mon0_height, opt.mon0_width, CV_8UC3);
            }
            if (need_resize_mon1) {
                mon1_frame.create(opt.mon1_height, opt.mon1_width, CV_8UC3);
            }
        } else {
            need_resize_single = (total_w != opt.mon0_width || total_h != opt.mon0_height);
            interp_single = ultra
                                ? cv::INTER_LINEAR
                                : ChooseResizeInterpolation(total_w, total_h, opt.mon0_width, opt.mon0_height);
            if (need_resize_single) {
                single_frame.create(opt.mon0_height, opt.mon0_width, CV_8UC3);
            }
        }

        if (dual_mode) {
            mon0_blank = cv::Mat::zeros(opt.mon0_height, opt.mon0_width, CV_8UC3);
            mon1_blank = cv::Mat::zeros(opt.mon1_height, opt.mon1_width, CV_8UC3);
            cv::imshow(kWindowDualMon0, mon0_blank);
            cv::imshow(kWindowDualMon1, mon1_blank);
        } else {
            single_blank = cv::Mat::zeros(opt.mon0_height, opt.mon0_width, CV_8UC3);
            cv::imshow(kWindowSingle, single_blank);
        }
        cv::waitKey(1);
    }
#endif

    auto t0 = std::chrono::steady_clock::now();
    int true_paired = 0;
    int cam0_fps = 0;
    int cam1_fps = 0;
    int dropped_unsynced = 0;
    uint64_t pair_delta_sum_ticks = 0;
    uint64_t pair_delta_max_ticks = 0;
    uint64_t seen_cam0_frame = 0;
    uint64_t seen_cam1_frame = 0;
    bool have_seen_cam_frame = false;
    uint64_t last_frame0 = 0;
    uint64_t last_frame1 = 0;
    bool have_last_pair = false;

    while (run_flag->load(std::memory_order_relaxed)) {
        const int idx0_local = cam0->latest_idx.load(std::memory_order_acquire);
        const int idx1_local = cam1->latest_idx.load(std::memory_order_acquire);
        if (idx0_local < 0 || idx1_local < 0) {
#if HAVE_OPENCV
            if (do_display) {
                cv::waitKey(1);
            }
#endif
            pause_wait_frame();
            continue;
        }

        FrameMeta peek0{};
        FrameMeta peek1{};
        if (!ReadSlotMeta(cam0->slots[idx0_local], &peek0) ||
            !ReadSlotMeta(cam1->slots[idx1_local], &peek1)) {
#if HAVE_OPENCV
            if (do_display) {
                cv::waitKey(1);
            }
#endif
            continue;
        }

        if (!have_seen_cam_frame) {
            seen_cam0_frame = peek0.frame_id;
            seen_cam1_frame = peek1.frame_id;
            have_seen_cam_frame = true;
        } else {
            if (peek0.frame_id != seen_cam0_frame) {
                if (peek0.frame_id > seen_cam0_frame) {
                    cam0_fps += static_cast<int>(peek0.frame_id - seen_cam0_frame);
                } else {
                    ++cam0_fps;
                }
                seen_cam0_frame = peek0.frame_id;
            }
            if (peek1.frame_id != seen_cam1_frame) {
                if (peek1.frame_id > seen_cam1_frame) {
                    cam1_fps += static_cast<int>(peek1.frame_id - seen_cam1_frame);
                } else {
                    ++cam1_fps;
                }
                seen_cam1_frame = peek1.frame_id;
            }
        }

        if (have_last_pair) {
            const bool both_old = (peek0.frame_id == last_frame0 && peek1.frame_id == last_frame1);
            const bool one_old = (peek0.frame_id == last_frame0 || peek1.frame_id == last_frame1);
            if (both_old || (require_both_new && one_old)) {
                pause_brief();
                continue;
            }
        }

        if (max_delta_ticks > 0 && require_both_new) {
            const uint64_t delta = (peek0.timestamp > peek1.timestamp)
                                       ? (peek0.timestamp - peek1.timestamp)
                                       : (peek1.timestamp - peek0.timestamp);
            if (delta > max_delta_ticks) {
                ++dropped_unsynced;
                pause_brief();
                continue;
            }
        }

        FrameMeta m0 = peek0;
        FrameMeta m1 = peek1;
        if (do_display) {
            uint8_t* left = combined.data();
            uint8_t* right = combined.data() + static_cast<size_t>(w0 * 3);
            if (!CopySlotToBuffer(cam0->slots[idx0_local], left, combined_stride, &m0) ||
                !CopySlotToBuffer(cam1->slots[idx1_local], right, combined_stride, &m1)) {
                continue;
            }
        }

        if (m0.width != w0 || m0.height != h0 || m1.width != w1 || m1.height != h1 ||
            m0.width != m1.width || m0.height != m1.height) {
            if (opt.verbose) {
                std::fprintf(stderr,
                             "Runtime resolution changed: left=%dx%d right=%dx%d\n",
                             m0.width,
                             m0.height,
                             m1.width,
                             m1.height);
            }
            continue;
        }

        if (max_delta_ticks > 0 && require_both_new) {
            const uint64_t delta = (m0.timestamp > m1.timestamp)
                                       ? (m0.timestamp - m1.timestamp)
                                       : (m1.timestamp - m0.timestamp);
            if (delta > max_delta_ticks) {
                ++dropped_unsynced;
                continue;
            }
        }

        if (have_last_pair && m0.frame_id == last_frame0 && m1.frame_id == last_frame1) {
            continue;
        }
        last_frame0 = m0.frame_id;
        last_frame1 = m1.frame_id;
        have_last_pair = true;
        const uint64_t pair_delta_ticks = (m0.timestamp > m1.timestamp)
                                              ? (m0.timestamp - m1.timestamp)
                                              : (m1.timestamp - m0.timestamp);
        pair_delta_sum_ticks += pair_delta_ticks;
        if (pair_delta_ticks > pair_delta_max_ticks) {
            pair_delta_max_ticks = pair_delta_ticks;
        }

#if HAVE_OPENCV
        if (do_display) {
            cv::Mat stereo_view(total_h, total_w, CV_8UC3, combined.data());
            if (dual_mode) {
                if (need_resize_mon0) {
                    cv::resize(stereo_view, mon0_frame, cv::Size(opt.mon0_width, opt.mon0_height), 0, 0, interp_mon0);
                    cv::imshow(kWindowDualMon0, mon0_frame);
                } else {
                    cv::imshow(kWindowDualMon0, stereo_view);
                }

                if (need_resize_mon1) {
                    cv::resize(stereo_view, mon1_frame, cv::Size(opt.mon1_width, opt.mon1_height), 0, 0, interp_mon1);
                    cv::imshow(kWindowDualMon1, mon1_frame);
                } else {
                    cv::imshow(kWindowDualMon1, stereo_view);
                }
            } else {
                if (need_resize_single) {
                    cv::resize(stereo_view, single_frame, cv::Size(opt.mon0_width, opt.mon0_height), 0, 0, interp_single);
                    cv::imshow(kWindowSingle, single_frame);
                } else {
                    cv::imshow(kWindowSingle, stereo_view);
                }
            }
            const int key = cv::waitKey(1);
            if (key == 27 || key == 'q' || key == 'Q') {
                run_flag->store(false, std::memory_order_relaxed);
            }
        } else {
            pause_brief();
        }
#else
        pause_brief();
#endif

        ++true_paired;
        const auto now = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
        if (dt >= 1) {
            if (opt.verbose) {
                if (tick_freq > 0 && true_paired > 0) {
                    const double avg_pair_delta_us =
                        (static_cast<double>(pair_delta_sum_ticks) * 1000000.0) /
                        (static_cast<double>(tick_freq) * static_cast<double>(true_paired));
                    const double max_pair_delta_us =
                        (static_cast<double>(pair_delta_max_ticks) * 1000000.0) /
                        static_cast<double>(tick_freq);
                    std::fprintf(stdout,
                                 "cam0_fps=%d cam1_fps=%d true_paired_fps=%d dropped_unsynced=%d "
                                 "pair_delta_us_avg=%.1f pair_delta_us_max=%.1f\n",
                                 cam0_fps,
                                 cam1_fps,
                                 true_paired,
                                 dropped_unsynced,
                                 avg_pair_delta_us,
                                 max_pair_delta_us);
                } else {
                    std::fprintf(stdout,
                                 "cam0_fps=%d cam1_fps=%d true_paired_fps=%d dropped_unsynced=%d "
                                 "pair_delta_ticks_max=%llu\n",
                                 cam0_fps,
                                 cam1_fps,
                                 true_paired,
                                 dropped_unsynced,
                                 static_cast<unsigned long long>(pair_delta_max_ticks));
                }
                std::fflush(stdout);
            }
            true_paired = 0;
            cam0_fps = 0;
            cam1_fps = 0;
            dropped_unsynced = 0;
            pair_delta_sum_ticks = 0;
            pair_delta_max_ticks = 0;
            t0 = now;
        }
    }

#if HAVE_OPENCV
    if (do_display) {
        if (dual_mode) {
            cv::destroyWindow(kWindowDualMon0);
            cv::destroyWindow(kWindowDualMon1);
        } else {
            cv::destroyWindow(kWindowSingle);
        }
    }
#endif

    combined.clear();
    combined.shrink_to_fit();
}
