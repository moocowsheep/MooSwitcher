/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "omt/OmtInput.h"

#include <cstring>

#include <libomt.h>

#include "audio/AudioEngine.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/Stats.h"

namespace moo {

OmtInput::OmtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue, std::string ref,
                   int index, int syncFrames)
    : eng_(eng),
      queue_(uploadQueue),
      ref_(std::move(ref)),
      index_(index),
      syncFrames_(syncFrames) {
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

OmtInput::~OmtInput() {
    thread_ = {};  // stop + join (receive timeout bounds the wait)
    if (recv_) omt_receive_destroy(recv_);
}

OmtInput::Status OmtInput::status() const {
    Status s;
    s.connected = connected_.load(std::memory_order_relaxed);
    s.frames = frames_.load(std::memory_order_relaxed);
    s.drops = drops_.load(std::memory_order_relaxed);
    std::lock_guard lk(descM_);
    s.desc = desc_;
    return s;
}

void OmtInput::run(std::stop_token st) {
    auto& dropCtr = Stats::counter("in" + std::to_string(index_) + ".drops");
    auto& frameCtr = Stats::counter("in" + std::to_string(index_) + ".frames");
    auto& reconnCtr =
        Stats::counter("in" + std::to_string(index_) + ".reconnects");
    auto& synthCtr =
        Stats::counter("in" + std::to_string(index_) + ".sync.ptsSynth");
    auto& feedDropCtr =
        Stats::counter("in" + std::to_string(index_) + ".sync.feedDrops");

    int64_t lastVideoNs = MediaClock::nowNs();
    bool everConnected = false;
    // Sticky-UYVA eligibility is per connection (see NdiReceiver).
    bool alphaThisConn = false;

    // Frame-sync pts state, same policy as NdiReceiver: sender timestamps
    // (100 ns units) are the pts source of truth, deltas only; absent or
    // cadence-breaking timestamps flip to a synthesized arrival grid,
    // sticky until reconnect.
    uint64_t pubSeq = 0;
    bool ptsSynth = false;
    int badCadence = 0;
    bool haveSrcTs = false;
    int64_t lastSrcTsNs = 0;
    int64_t synthBaseNs = 0, synthK = 0, synthLastArrNs = 0;

    auto pushAudio = [this](const OMTMediaFrame& a) {
        auto* ch = audioSink_.load(std::memory_order_acquire);
        if (!ch || a.Codec != OMTCodec_FPA1 || a.Channels <= 0 || !a.Data ||
            a.SamplesPerChannel <= 0)
            return;
        const float* l = static_cast<const float*>(a.Data);
        const float* r =
            a.Channels > 1 ? l + a.SamplesPerChannel : l;  // mono: dup plane
        // Same sender clock as video timestamps; first-sample time, exactly
        // like the NDI reading (docs/bench-framesync.md).
        const int64_t pts =
            a.Timestamp >= 0 ? a.Timestamp * 100 : audio::InputChannel::kNoPts;
        ch->pushPlanar(l, r, a.SamplesPerChannel, a.SampleRate, pts);
    };

    while (!st.stop_requested()) {
        if (!recv_) {
            recv_ = omt_receive_create(
                ref_.c_str(), OMTFrameType(OMTFrameType_Video | OMTFrameType_Audio),
                OMTPreferredVideoFormat_UYVYorUYVA, OMTReceiveFlags_None);
            if (!recv_) {
                MOO_LOGE("in%d(omt): receive_create('%s') failed", index_,
                         ref_.c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            lastVideoNs = MediaClock::nowNs();
            if (everConnected) reconnCtr.add();
            appliedTally_ = 0xFF;  // re-send tally on the new connection
            ptsSynth = false;      // re-judge the new connection's timestamps
            badCadence = 0;
            haveSrcTs = false;
            synthK = 0;
            alphaThisConn = false;
            MOO_LOGI("in%d(omt): connecting to '%s'", index_, ref_.c_str());
        }

        if (const uint8_t want = tally_.load(std::memory_order_relaxed);
            want != appliedTally_) {
            OMTTally t{};
            t.program = want & 1;
            t.preview = (want >> 1) & 1;
            omt_receive_settally(recv_, &t);
            appliedTally_ = want;
        }

        OMTMediaFrame* fr = omt_receive(
            recv_, OMTFrameType(OMTFrameType_Video | OMTFrameType_Audio), 500);

        // An audio frame must be consumed before the drain below -- each
        // audio-typed receive frees the previous audio frame (per-type
        // last-frame buffers in libomt). Video pointers survive audio calls.
        if (fr && fr->Type == OMTFrameType_Audio) pushAudio(*fr);

        // Drain queued audio greedily every pass (NDI gotcha applied on
        // principle: a startup backlog must never park in the transport).
        {
            OMTMediaFrame* extra;
            while ((extra = omt_receive(recv_, OMTFrameType_Audio, 0)) != nullptr)
                pushAudio(*extra);
        }

        if (!fr) {
            // No frame: if the source went quiet for 3 s, recreate the
            // receiver (the sender may have restarted under a new port).
            if (MediaClock::nowNs() - lastVideoNs > 3'000'000'000LL) {
                connected_.store(false, std::memory_order_relaxed);
                omt_receive_destroy(recv_);
                recv_ = nullptr;
                lastVideoNs = MediaClock::nowNs();
            }
            continue;
        }

        if (fr->Type != OMTFrameType_Video) continue;  // audio handled above

        lastVideoNs = MediaClock::nowNs();
        if ((fr->Codec != OMTCodec_UYVY && fr->Codec != OMTCodec_UYVA) ||
            fr->Width <= 0 || fr->Height <= 0 || !fr->Data)
            continue;  // preferred-format promises UYVY/UYVA
        // Defensive: a UYVA frame must actually carry both planes.
        const bool incomingAlpha =
            fr->Codec == OMTCodec_UYVA &&
            size_t(fr->DataLength) >= size_t(fr->Stride) * size_t(fr->Height) +
                                          size_t(fr->Width) * size_t(fr->Height);
        if (!everConnected || !connected_.load(std::memory_order_relaxed)) {
            connected_.store(true, std::memory_order_relaxed);
            everConnected = true;
        }

        VideoFormatDesc d;
        d.width = fr->Width;
        d.height = fr->Height;
        d.fpsN = fr->FrameRateN;
        d.fpsD = fr->FrameRateD;
        d.pixfmt = incomingAlpha ? PixFmt::UYVA8_4224 : PixFmt::UYVY8_422;
        d.colorimetry = VideoFormatDesc::colorimetryForHeight(fr->Height);

        // Sticky-UYVA: keep the alpha ring across interleaved UYVY frames
        // (same policy and rationale as NdiReceiver).
        if (incomingAlpha) alphaThisConn = true;
        if (!incomingAlpha && alphaThisConn && ring_ &&
            ring_->desc().hasAlpha()) {
            VideoFormatDesc sticky = d;
            sticky.pixfmt = PixFmt::UYVA8_4224;
            if (ring_->desc() == sticky) d = sticky;
        }

        if (!ring_ || !(ring_->desc() == d)) {
            MOO_LOGI("in%d(omt): format %dx%d @ %d/%d%s", index_, d.width,
                     d.height, d.fpsN, d.fpsD, d.hasAlpha() ? " +alpha" : "");
            const int slots = gpu::UploadRing::kSlots +
                              (syncFrames_ >= 0 ? syncFrames_ + 2 : 0);
            ring_ = std::make_shared<gpu::UploadRing>(eng_, d, queue_, slots);
            slotAlphaOpaque_.assign(size_t(slots), 0);
            std::lock_guard lk(descM_);
            desc_ = d;
        }

        const int slot = ring_->acquire();
        if (slot < 0) {
            drops_.fetch_add(1, std::memory_order_relaxed);
            dropCtr.add();
            continue;
        }

        const size_t dstStride = d.rowBytes();
        uint8_t* dst = ring_->stagingPtr(slot);
        const uint8_t* src = static_cast<const uint8_t*>(fr->Data);
        if (size_t(fr->Stride) == dstStride) {
            memcpy(dst, src, dstStride * size_t(d.height));
        } else {
            for (int y = 0; y < d.height; ++y)
                memcpy(dst + size_t(y) * dstStride,
                       src + size_t(y) * size_t(fr->Stride), dstStride);
        }
        if (d.hasAlpha()) {
            uint8_t* adst = dst + d.alphaOffset();
            if (incomingAlpha) {
                // libomt decodes UYVA at Stride = width*2 with the packed
                // alpha plane appended (OMTReceive.cs / VMX_DecodeUYVA).
                const uint8_t* asrc = src + size_t(fr->Stride) * d.height;
                const size_t aSrc = size_t(fr->Stride) / 2;
                const size_t aDst = size_t(d.width);
                if (aSrc == aDst) {
                    memcpy(adst, asrc, aDst * size_t(d.height));
                } else {
                    for (int y = 0; y < d.height; ++y)
                        memcpy(adst + size_t(y) * aDst,
                               asrc + size_t(y) * aSrc, aDst);
                }
                slotAlphaOpaque_[size_t(slot)] = 0;
            } else if (!slotAlphaOpaque_[size_t(slot)]) {
                memset(adst, 0xFF, size_t(d.width) * size_t(d.height));
                slotAlphaOpaque_[size_t(slot)] = 1;
            }
        }
        const uint64_t value = ring_->submit(slot);
        const int64_t senderTsNs = fr->Timestamp >= 0 ? fr->Timestamp * 100 : -1;

        const bool premult =
            incomingAlpha && (fr->Flags & OMTVideoFlags_PreMultiplied) != 0;
        auto frame =
            std::make_shared<const gpu::GpuFrame>(ring_, slot, value, premult);
        mailbox_.publish(frame);
        if (syncFrames_ >= 0) {
            const int64_t arrNs = lastVideoNs;  // receive-return time
            const int64_t tsNs = 1'000'000'000LL * d.fpsD / d.fpsN;
            if (!ptsSynth) {
                if (senderTsNs < 0) {
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
                                MOO_LOGW("in%d(omt): sender timestamps "
                                         "unusable, synthesizing pts", index_);
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
    }
}

}  // namespace moo
