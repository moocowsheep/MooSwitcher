#include "ndi/NdiReceiver.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "audio/AudioEngine.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/Stats.h"

namespace moo {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
}  // namespace

NdiReceiver::NdiReceiver(gpu::VkEngine& eng, gpu::Queue& uploadQueue,
                         NdiFinder& finder, std::string matchName, int index,
                         int syncFrames)
    : eng_(eng),
      queue_(uploadQueue),
      finder_(finder),
      match_(lower(matchName)),
      displayName_(std::move(matchName)),
      index_(index),
      syncFrames_(syncFrames) {
    NDIlib_recv_create_v3_t desc{};
    desc.color_format = NDIlib_recv_color_format_fastest;  // UYVY (UYVA w/ alpha)
    desc.bandwidth = NDIlib_recv_bandwidth_highest;
    desc.allow_video_fields = false;
    desc.p_ndi_recv_name = "MooSwitcher input";
    recv_ = NDIlib_recv_create_v3(&desc);
    if (!recv_) {
        MOO_LOGE("in%d: NDIlib_recv_create_v3 failed", index_);
        return;
    }
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

NdiReceiver::~NdiReceiver() {
    thread_ = {};  // stop + join (capture timeout bounds the wait)
    if (recv_) NDIlib_recv_destroy(recv_);
    // ring_/mailbox frames release on their own; UploadRing dtor waits its TL.
}

NdiReceiver::Status NdiReceiver::status() const {
    Status s;
    s.connected = connected_.load(std::memory_order_relaxed);
    s.frames = frames_.load(std::memory_order_relaxed);
    s.drops = drops_.load(std::memory_order_relaxed);
    std::lock_guard lk(descM_);
    s.desc = desc_;
    return s;
}

void NdiReceiver::run(std::stop_token st) {
    auto& dropCtr = Stats::counter("in" + std::to_string(index_) + ".drops");
    auto& frameCtr = Stats::counter("in" + std::to_string(index_) + ".frames");
    auto& reconnCtr = Stats::counter("in" + std::to_string(index_) + ".reconnects");
    auto& synthCtr =
        Stats::counter("in" + std::to_string(index_) + ".sync.ptsSynth");
    auto& feedDropCtr =
        Stats::counter("in" + std::to_string(index_) + ".sync.feedDrops");

    int64_t lastVideoNs = 0;
    bool everConnected = false;

    // Frame-sync pts state. Sender timestamps are the pts source of truth
    // (deltas only -- they ride the sender's NTP-slewed realtime clock);
    // senders that ship absent/garbage timestamps get a synthesized
    // arrival-anchored grid instead, sticky until reconnect.
    uint64_t pubSeq = 0;
    bool ptsSynth = false;
    int badCadence = 0;
    bool haveSrcTs = false;
    int64_t lastSrcTsNs = 0;
    int64_t synthBaseNs = 0, synthK = 0, synthLastArrNs = 0;

    while (!st.stop_requested()) {
        if (!connected_.load(std::memory_order_relaxed)) {
            auto src = finder_.lookup(match_);
            if (!src) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            NDIlib_source_t s{};
            s.p_ndi_name = src->name.c_str();
            s.p_url_address = src->url.empty() ? nullptr : src->url.c_str();
            NDIlib_recv_connect(recv_, &s);
            connected_.store(true, std::memory_order_relaxed);
            lastVideoNs = MediaClock::nowNs();
            if (everConnected) reconnCtr.add();
            everConnected = true;
            appliedTally_ = 0xFF;  // force tally re-send on the new connection
            ptsSynth = false;      // re-judge the new connection's timestamps
            badCadence = 0;
            haveSrcTs = false;
            synthK = 0;
            MOO_LOGI("in%d: connecting to '%s'", index_, src->name.c_str());
        }

        if (const uint8_t want = tally_.load(std::memory_order_relaxed);
            want != appliedTally_) {
            NDIlib_tally_t t{};
            t.on_program = want & 1;
            t.on_preview = (want >> 1) & 1;
            NDIlib_recv_set_tally(recv_, &t);
            appliedTally_ = want;
        }

        auto pushAudio = [this](const NDIlib_audio_frame_v3_t& a) {
            auto* ch = audioSink_.load(std::memory_order_acquire);
            if (!ch || a.FourCC != NDIlib_FourCC_audio_type_FLTP ||
                a.no_channels <= 0 || !a.p_data)
                return;
            const float* l = reinterpret_cast<const float*>(a.p_data);
            const float* r =
                a.no_channels > 1
                    ? reinterpret_cast<const float*>(a.p_data +
                                                     a.channel_stride_in_bytes)
                    : l;  // mono: duplicate the plane
            // Audio timestamps ride the same sender clock as video's -- the
            // frame-sync auto-trim compares the two (kNoPts disables it).
            // Treated as first-sample time: measured against the TS-capture
            // ground truth (docs/bench-framesync.md), trims land ~1.7 ms of
            // center with this reading -- do NOT "correct" by chunk length
            // (tried; it overshoots by a full chunk).
            const int64_t pts = a.timestamp != NDIlib_recv_timestamp_undefined
                                    ? a.timestamp * 100
                                    : audio::InputChannel::kNoPts;
            ch->pushPlanar(l, r, a.no_samples, a.sample_rate, pts);
        };

        NDIlib_video_frame_v2_t vf{};
        NDIlib_audio_frame_v3_t af{};
        const auto ft = NDIlib_recv_capture_v3(recv_, &vf, &af, nullptr, 500);

        // Drain the SDK's audio queue greedily every pass. The main capture
        // returns one frame per call at video pace while the SDK queues audio
        // without dropping, so any startup backlog would otherwise persist
        // forever (arrival and consume rates match) and play out as a fixed
        // A/V offset. Video is drop-to-latest; audio must be pulled dry.
        {
            NDIlib_audio_frame_v3_t extra{};
            while (NDIlib_recv_capture_v3(recv_, nullptr, &extra, nullptr, 0) ==
                   NDIlib_frame_type_audio) {
                pushAudio(extra);
                NDIlib_recv_free_audio_v3(recv_, &extra);
            }
        }

        if (ft == NDIlib_frame_type_video) {
            lastVideoNs = MediaClock::nowNs();

            if (vf.FourCC != NDIlib_FourCC_video_type_UYVY &&
                vf.FourCC != NDIlib_FourCC_video_type_UYVA) {
                NDIlib_recv_free_video_v2(recv_, &vf);  // fastest promises UYVY/UYVA
                continue;
            }

            VideoFormatDesc d;
            d.width = vf.xres;
            d.height = vf.yres;
            d.fpsN = vf.frame_rate_N;
            d.fpsD = vf.frame_rate_D;
            d.colorimetry = VideoFormatDesc::colorimetryForHeight(vf.yres);

            if (!ring_ || !(ring_->desc() == d)) {
                MOO_LOGI("in%d: format %dx%d @ %d/%d", index_, d.width, d.height,
                         vf.frame_rate_N, vf.frame_rate_D);
                const int slots =
                    gpu::UploadRing::kSlots +
                    (syncFrames_ >= 0 ? syncFrames_ + 2 : 0);
                ring_ = std::make_shared<gpu::UploadRing>(eng_, d, queue_, slots);
                std::lock_guard lk(descM_);
                desc_ = d;
            }

            const int slot = ring_->acquire();
            if (slot < 0) {
                drops_.fetch_add(1, std::memory_order_relaxed);
                dropCtr.add();
                NDIlib_recv_free_video_v2(recv_, &vf);
                continue;
            }

            const size_t dstStride = d.rowBytes();
            uint8_t* dst = ring_->stagingPtr(slot);
            const uint8_t* src = vf.p_data;
            if (size_t(vf.line_stride_in_bytes) == dstStride) {
                memcpy(dst, src, dstStride * size_t(d.height));  // UYVA: alpha plane ignored
            } else {
                for (int y = 0; y < d.height; ++y)
                    memcpy(dst + size_t(y) * dstStride,
                           src + size_t(y) * vf.line_stride_in_bytes, dstStride);
            }
            const uint64_t value = ring_->submit(slot);
            const int64_t senderTsNs =
                vf.timestamp != NDIlib_recv_timestamp_undefined
                    ? vf.timestamp * 100
                    : NDIlib_recv_timestamp_undefined;
            NDIlib_recv_free_video_v2(recv_, &vf);

            auto frame = std::make_shared<const gpu::GpuFrame>(ring_, slot, value);
            mailbox_.publish(frame);
            if (syncFrames_ >= 0) {
                const int64_t arrNs = lastVideoNs;  // capture-return time
                const int64_t tsNs = 1'000'000'000LL * d.fpsD / d.fpsN;
                if (!ptsSynth) {
                    if (senderTsNs == NDIlib_recv_timestamp_undefined) {
                        ptsSynth = true;
                        synthCtr.add();
                    } else {
                        if (haveSrcTs) {
                            const int64_t dts = senderTsNs - lastSrcTsNs;
                            const int64_t err = dts > tsNs ? dts - tsNs : tsNs - dts;
                            if (dts <= 0 || err > tsNs / 4) {
                                if (++badCadence > 8) {
                                    ptsSynth = true;
                                    synthCtr.add();
                                    MOO_LOGW("in%d: sender timestamps unusable, "
                                             "synthesizing pts", index_);
                                }
                            } else {
                                badCadence = 0;
                            }
                        }
                        lastSrcTsNs = senderTsNs;
                        haveSrcTs = true;
                    }
                }
                int64_t ptsNs;
                if (ptsSynth) {
                    // Arrival-anchored grid; re-base across gaps so the
                    // scheduler sees them as pts discontinuities (resync).
                    if (synthK == 0 || arrNs - synthLastArrNs > 500'000'000LL) {
                        synthBaseNs = arrNs;
                        synthK = 0;
                    }
                    ptsNs = synthBaseNs + synthK * tsNs;
                    ++synthK;
                    synthLastArrNs = arrNs;
                } else {
                    ptsNs = senderTsNs;
                }
                if (!feed_.push({frame, ++pubSeq, ptsNs, arrNs, !ptsSynth}))
                    feedDropCtr.add();
            }
            frames_.fetch_add(1, std::memory_order_relaxed);
            frameCtr.add();
        } else if (ft == NDIlib_frame_type_audio) {
            pushAudio(af);
            NDIlib_recv_free_audio_v3(recv_, &af);
        } else if (ft == NDIlib_frame_type_error) {
            connected_.store(false, std::memory_order_relaxed);
        } else {
            // No frame: if the source went quiet for 3s, retry discovery (the
            // sender may have restarted under a new URL).
            if (everConnected &&
                MediaClock::nowNs() - lastVideoNs > 3'000'000'000LL) {
                connected_.store(false, std::memory_order_relaxed);
                lastVideoNs = MediaClock::nowNs();
            }
        }
    }
}

}  // namespace moo
