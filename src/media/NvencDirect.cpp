/* MooSwitcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * MooSwitcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), and the OMT (libomt / libvmx)
 * runtime, and distribute the combined work. See EXCEPTIONS.md for the
 * full exception text. */
#include "media/NvencDirect.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstring>

#include "core/Log.h"

extern "C" {
#include <libavutil/mem.h>
}

namespace moo::media {
namespace {

// The pack ring is fixed-size, so registrations are too; more than this means
// buffers are being handed to us from somewhere unexpected.
constexpr size_t kMaxRegistrations = 8;
// Output slots. One would do (we lock synchronously per picture), but a small
// ring keeps the driver from serializing on a buffer still being copied out.
constexpr int kBitstreamBuffers = 4;

// libnvidia-encode, loaded once per process and never unloaded: the driver
// library is not designed to be torn down and re-initialized under us.
struct NvencLib {
    NVENCSTATUS(NVENCAPI* createInstance)(NV_ENCODE_API_FUNCTION_LIST*) = nullptr;
    NVENCSTATUS(NVENCAPI* maxVersion)(uint32_t*) = nullptr;
};

const NvencLib& nvencLib() {
    static const NvencLib lib = [] {
        NvencLib l;
        void* h = dlopen("libnvidia-encode.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!h) {
            MOO_LOGW("nvenc-direct: %s", dlerror());
            return l;
        }
        l.createInstance = reinterpret_cast<decltype(l.createInstance)>(
            dlsym(h, "NvEncodeAPICreateInstance"));
        l.maxVersion = reinterpret_cast<decltype(l.maxVersion)>(
            dlsym(h, "NvEncodeAPIGetMaxSupportedVersion"));
        if (!l.createInstance || !l.maxVersion)
            MOO_LOGW("nvenc-direct: libnvidia-encode is missing API entry points");
        return l;
    }();
    return lib;
}

GUID nvencPresetGuid(EncoderPreset preset) {
    switch (preset) {
        case EncoderPreset::P1: return NV_ENC_PRESET_P1_GUID;
        case EncoderPreset::P2: return NV_ENC_PRESET_P2_GUID;
        case EncoderPreset::P3: return NV_ENC_PRESET_P3_GUID;
        case EncoderPreset::P5: return NV_ENC_PRESET_P5_GUID;
        case EncoderPreset::P6: return NV_ENC_PRESET_P6_GUID;
        case EncoderPreset::P7: return NV_ENC_PRESET_P7_GUID;
        default: return NV_ENC_PRESET_P4_GUID;
    }
}

}  // namespace

bool NvencDirect::open(CudaCtx& cuda, const VideoFormatDesc& show,
                       const EncoderConfig& cfg) {
    close();
    int bitrateKbps = cfg.bitrateKbps;
    const bool globalHeader = cfg.globalHeader;
    const EncoderPreset preset = resolveEncoderPreset(cfg.preset, show);
    const GUID presetGuid = nvencPresetGuid(preset);
    const NvencLib& lib = nvencLib();
    if (!lib.createInstance) return false;
    if (!cuda.ok()) {
        MOO_LOGE("nvenc-direct: no CUDA context");
        return false;
    }

    cuda_ = &cuda;
    w_ = show.width;
    h_ = show.height;
    // NVENC takes the pack buffer as-is: tight pitch, chroma implicitly at
    // pitch*height. Pitch must be a multiple of 4 and the frame dimensions
    // even; every show format we accept already is.
    if (w_ <= 0 || h_ <= 0 || w_ % 4 != 0 || h_ % 2 != 0) {
        MOO_LOGE("nvenc-direct: %dx%d unsupported (need pitch %%4, even height)",
                 w_, h_);
        return false;
    }

    const double fps = double(show.fpsN) / double(show.fpsD);
    if (bitrateKbps <= 0)
        bitrateKbps = std::max(8000, int(double(w_) * h_ * fps * 0.04 / 1000.0));
    bitrate_ = int64_t(bitrateKbps) * 1000;
    timeBase_ = {int(show.fpsD), int(show.fpsN)};

    uint32_t driverVersion = 0;
    const uint32_t needVersion =
        (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (lib.maxVersion(&driverVersion) != NV_ENC_SUCCESS ||
        driverVersion < needVersion) {
        MOO_LOGE("nvenc-direct: driver NVENC API %u.%u < headers %d.%d",
                 driverVersion >> 4, driverVersion & 0xf,
                 NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        return false;
    }

    api_ = {};
    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (const NVENCSTATUS s = lib.createInstance(&api_); s != NV_ENC_SUCCESS) {
        MOO_LOGE("nvenc-direct: NvEncodeAPICreateInstance failed (%d)", int(s));
        return false;
    }

    cuda.makeCurrent();
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    session.device = cuda.ctx();
    session.apiVersion = NVENCAPI_VERSION;
    if (const NVENCSTATUS s = api_.nvEncOpenEncodeSessionEx(&session, &enc_);
        s != NV_ENC_SUCCESS) {
        MOO_LOGE("nvenc-direct: session open failed (%d)", int(s));
        enc_ = nullptr;
        return false;
    }

    NV_ENC_PRESET_CONFIG presetCfg{};
    presetCfg.version = NV_ENC_PRESET_CONFIG_VER;
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;
    if (const NVENCSTATUS s = api_.nvEncGetEncodePresetConfigEx(
            enc_, NV_ENC_CODEC_HEVC_GUID, presetGuid,
            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetCfg);
        s != NV_ENC_SUCCESS) {
        logLast("preset config", s);
        close();
        return false;
    }

    // Same shape as the FFmpeg backend's options (preset/ull, CBR,
    // single-frame VBV, IPP, IDR ~2 s) so the two paths are swappable mid-show.
    cfg_ = presetCfg.presetCfg;
    cfg_.version = NV_ENC_CONFIG_VER;
    cfg_.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    cfg_.gopLength = uint32_t(std::max(1, int(fps * 2)));
    cfg_.frameIntervalP = 1;
    cfg_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg_.rcParams.averageBitRate = uint32_t(bitrate_);
    cfg_.rcParams.maxBitRate = uint32_t(bitrate_);
    cfg_.rcParams.vbvBufferSize = uint32_t(bitrate_ * show.fpsD / show.fpsN);
    cfg_.rcParams.vbvInitialDelay = cfg_.rcParams.vbvBufferSize;
    cfg_.rcParams.enableLookahead = 0;
    cfg_.rcParams.zeroReorderDelay = 1;

    NV_ENC_CONFIG_HEVC& hevc = cfg_.encodeCodecConfig.hevcConfig;
    hevc.idrPeriod = cfg_.gopLength;
    hevc.chromaFormatIDC = 1;
    hevc.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    hevc.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    // MPEG-TS wants parameter sets in band on every IDR; Matroska takes them
    // once as extradata, which is what globalHeader asks for.
    hevc.repeatSPSPPS = globalHeader ? 0u : 1u;
    hevc.disableSPSPPS = globalHeader ? 1u : 0u;

    init_ = {};
    init_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    init_.presetGUID = presetGuid;
    init_.encodeWidth = uint32_t(w_);
    init_.encodeHeight = uint32_t(h_);
    init_.darWidth = uint32_t(w_);
    init_.darHeight = uint32_t(h_);
    init_.frameRateNum = uint32_t(show.fpsN);
    init_.frameRateDen = uint32_t(show.fpsD);
    init_.enablePTD = 1;  // let NVENC pick picture types (IDR at GOP starts)
    init_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init_.encodeConfig = &cfg_;
    if (const NVENCSTATUS s = api_.nvEncInitializeEncoder(enc_, &init_);
        s != NV_ENC_SUCCESS) {
        logLast("initialize encoder", s);
        close();
        return false;
    }

    for (int i = 0; i < kBitstreamBuffers; ++i) {
        NV_ENC_CREATE_BITSTREAM_BUFFER create{};
        create.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (const NVENCSTATUS s = api_.nvEncCreateBitstreamBuffer(enc_, &create);
            s != NV_ENC_SUCCESS) {
            logLast("create bitstream buffer", s);
            close();
            return false;
        }
        bitstreams_.push_back(create.bitstreamBuffer);
    }

    if (!fetchExtradata()) {
        close();
        return false;
    }

    MOO_LOGI("nvenc-direct: hevc %dx%d @ %.3f fps, %d kbps CBR (%s/ull)", w_, h_,
             fps, bitrateKbps, encoderPresetName(preset));
    return true;
}

bool NvencDirect::fetchExtradata() {
    uint8_t header[NV_MAX_SEQ_HDR_LEN] = {};
    uint32_t size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = sizeof header;
    payload.spsppsBuffer = header;
    payload.outSPSPPSPayloadSize = &size;
    if (const NVENCSTATUS s = api_.nvEncGetSequenceParams(enc_, &payload);
        s != NV_ENC_SUCCESS) {
        logLast("get sequence params", s);
        return false;
    }
    extradata_.assign(header, header + size);
    return true;
}

void NvencDirect::close() {
    if (enc_) {
        if (cuda_) cuda_->makeCurrent();
        if (!eosSent_) {
            std::vector<AVPacket*> unused;
            drain(unused);
            for (auto* pkt : unused) av_packet_free(&pkt);
        }
        for (const Registration& r : regs_)
            if (r.handle) api_.nvEncUnregisterResource(enc_, r.handle);
        for (NV_ENC_OUTPUT_PTR bs : bitstreams_)
            if (bs) api_.nvEncDestroyBitstreamBuffer(enc_, bs);
        api_.nvEncDestroyEncoder(enc_);
        enc_ = nullptr;
    }
    regs_.clear();
    bitstreams_.clear();
    extradata_.clear();
    nextBitstream_ = 0;
    frameIdx_ = 0;
    eosSent_ = false;
}

bool NvencDirect::fillCodecpar(AVCodecParameters* par) const {
    if (!enc_) return false;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_HEVC;
    par->codec_tag = 0;
    par->width = w_;
    par->height = h_;
    par->format = AV_PIX_FMT_NV12;
    par->profile = AV_PROFILE_HEVC_MAIN;
    par->bit_rate = bitrate_;
    if (par->extradata) av_freep(&par->extradata);
    par->extradata_size = 0;
    if (extradata_.empty()) return true;

    par->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata_.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!par->extradata) return false;
    memcpy(par->extradata, extradata_.data(), extradata_.size());
    par->extradata_size = int(extradata_.size());
    return true;
}

NV_ENC_REGISTERED_PTR NvencDirect::registrationFor(CUdeviceptr src) {
    for (const Registration& r : regs_)
        if (r.ptr == src) return r.handle;

    if (regs_.size() >= kMaxRegistrations) {
        MOO_LOGE("nvenc-direct: too many distinct input buffers (%zu)",
                 regs_.size());
        return nullptr;
    }
    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.width = uint32_t(w_);
    reg.height = uint32_t(h_);
    reg.pitch = uint32_t(w_);
    reg.resourceToRegister = reinterpret_cast<void*>(src);
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (const NVENCSTATUS s = api_.nvEncRegisterResource(enc_, &reg);
        s != NV_ENC_SUCCESS) {
        logLast("register resource", s);
        return nullptr;
    }
    regs_.push_back({src, reg.registeredResource});
    return reg.registeredResource;
}

bool NvencDirect::encode(CUdeviceptr src, int64_t pts,
                         std::vector<AVPacket*>& out) {
    if (!enc_) return false;
    cuda_->makeCurrent();

    NV_ENC_REGISTERED_PTR registered = registrationFor(src);
    if (!registered) return false;

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered;
    if (const NVENCSTATUS s = api_.nvEncMapInputResource(enc_, &map);
        s != NV_ENC_SUCCESS) {
        logLast("map input resource", s);
        return false;
    }

    NV_ENC_OUTPUT_PTR bitstream = bitstreams_[nextBitstream_];
    nextBitstream_ = (nextBitstream_ + 1) % bitstreams_.size();

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = uint32_t(w_);
    pic.inputHeight = uint32_t(h_);
    pic.inputPitch = uint32_t(w_);
    pic.outputBitstream = bitstream;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = uint64_t(pts);
    pic.frameIdx = frameIdx_++;

    // No B-frames and no lookahead, so every picture completes here:
    // NEED_MORE_INPUT would mean the encoder kept `src`, which would break the
    // render thread's recycle handshake.
    const NVENCSTATUS picStatus = api_.nvEncEncodePicture(enc_, &pic);
    bool encoded = picStatus == NV_ENC_SUCCESS;
    if (!encoded) {
        logLast("encode picture", picStatus);
    } else {
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = bitstream;
        if (const NVENCSTATUS s = api_.nvEncLockBitstream(enc_, &lock);
            s != NV_ENC_SUCCESS) {
            logLast("lock bitstream", s);
            encoded = false;
        } else {
            AVPacket* pkt = av_packet_alloc();
            if (pkt && av_new_packet(pkt, int(lock.bitstreamSizeInBytes)) == 0) {
                memcpy(pkt->data, lock.bitstreamBufferPtr,
                       lock.bitstreamSizeInBytes);
                // No reordering: dts == pts.
                pkt->pts = pkt->dts = int64_t(lock.outputTimeStamp);
                if (lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                    lock.pictureType == NV_ENC_PIC_TYPE_I)
                    pkt->flags |= AV_PKT_FLAG_KEY;
                out.push_back(pkt);
            } else {
                av_packet_free(&pkt);
                encoded = false;
            }
            api_.nvEncUnlockBitstream(enc_, bitstream);
        }
    }

    // The lock above waited for completion, so `src` is free again.
    api_.nvEncUnmapInputResource(enc_, map.mappedResource);
    return encoded;
}

bool NvencDirect::drain(std::vector<AVPacket*>&) {
    if (!enc_ || eosSent_) return true;
    // Synchronous session with no reordering: nothing is pending, so EOS only
    // tells the encoder the stream ended before we tear it down.
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    eosSent_ = true;
    return api_.nvEncEncodePicture(enc_, &pic) == NV_ENC_SUCCESS;
}

void NvencDirect::logLast(const char* what, NVENCSTATUS status) const {
    const char* detail = enc_ ? api_.nvEncGetLastErrorString(enc_) : nullptr;
    MOO_LOGE("nvenc-direct: %s failed (%d)%s%s", what, int(status),
             detail ? ": " : "", detail ? detail : "");
}

}  // namespace moo::media
