#include "FfmpegRecorder.h"
#include "SrsStreamer.h"
#include "Mp4Faststart.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace fs = std::filesystem;

FfmpegRecorder::FfmpegRecorder(const RecorderConfig& cfg)
    : cfg_(cfg)
{
    av_log_set_level(AV_LOG_WARNING);
}

FfmpegRecorder::~FfmpegRecorder()
{
    close();
}

void FfmpegRecorder::attachStreamer(std::shared_ptr<SrsStreamer> s)
{
    std::lock_guard<std::mutex> lk(mu_);
    // Re-attach means fresh streamer; force re-push of params.
    streamerParamsPushed_ = false;
    streamer_ = std::move(s);
    // If we're already open, push codec params immediately so the streamer
    // can connect. Otherwise open() will do it when streams are created.
    if (streamer_ && isOpen_) {
        if (vStream_) {
            streamer_->setVideoParams(vStream_->codecpar, vEncCtx_->time_base);
        }
        if (aStream_) {
            streamer_->setAudioParams(aStream_->codecpar, aEncCtx_->time_base);
        }
        streamerParamsPushed_ = true;
    }
}

bool FfmpegRecorder::open(const std::string& outputPath,
                          int videoWidth, int videoHeight, int fps,
                          int sampleRate, int channels,
                          bool hasAudio)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (isOpen_) return false;

    outputPath_ = outputPath;
    vWidth_ = videoWidth; vHeight_ = videoHeight; vFps_ = fps;
    aSampleRate_ = sampleRate; aChannels_ = channels;

    // Ensure output directory exists
    fs::create_directories(fs::path(outputPath).parent_path());

    // Allocate output format context (mp4)
    int ret = avformat_alloc_output_context2(&fmtCtx_, nullptr, "mp4",
                                             outputPath.c_str());
    if (ret < 0 || !fmtCtx_) {
        spdlog::error("FfmpegRecorder: avformat_alloc_output_context2 failed: {}", ret);
        return false;
    }

    // Use fragmented MP4 so the file is always playable — even if the process
    // is killed (SIGKILL) before av_write_trailer() is called.
    //   frag_keyframe     – flush a new fragment at every video keyframe; each
    //                       fragment is self-contained and decodable on its own.
    //   empty_moov        – write the 'moov' box immediately at open time with
    //                       empty sample tables; no end-of-file seek required.
    //   default_base_moof – set the defaultBaseIsMoof flag for wider player
    //                       compatibility (DASH / ISO BMFF players).
    av_opt_set(fmtCtx_->priv_data, "movflags",
               "frag_keyframe+empty_moov+default_base_moof", 0);

    if (!addVideoStream()) { close(); return false; }
    if (hasAudio) {
        if (!addAudioStream()) { close(); return false; }
    }

    vEncodeFrame_ = av_frame_alloc();
    vEncodeFrame_->format = AV_PIX_FMT_YUV420P;
    vEncodeFrame_->width = vWidth_;
    vEncodeFrame_->height = vHeight_;
    av_frame_get_buffer(vEncodeFrame_, 0);

    if (hasAudio) {
        aEncodeFrame_ = av_frame_alloc();
        aEncodeFrame_->format = aEncCtx_->sample_fmt;
        aEncodeFrame_->sample_rate = aSampleRate_;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57,28,100)
        av_channel_layout_default(&aEncodeFrame_->ch_layout, aChannels_);
#else
        aEncodeFrame_->channels = aChannels_;
        aEncodeFrame_->channel_layout = av_get_default_channel_layout(aChannels_);
#endif
        aEncodeFrame_->nb_samples = aEncCtx_->frame_size > 0 ? aEncCtx_->frame_size : 1024;
        av_frame_get_buffer(aEncodeFrame_, 0);
    }

    encodePkt_ = av_packet_alloc();

    // Open output file
    ret = avio_open(&fmtCtx_->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        spdlog::error("FfmpegRecorder: avio_open failed: {}", ret);
        close(); return false;
    }

    // Write file header
    ret = avformat_write_header(fmtCtx_, nullptr);
    if (ret < 0) {
        spdlog::error("FfmpegRecorder: avformat_write_header failed: {}", ret);
        close(); return false;
    }

    isOpen_ = true;
    spdlog::info("FfmpegRecorder: opened {}", outputPath);

    // If a streamer is already attached, give it codec params now that
    // encoder contexts exist and are open.
    if (streamer_ && !streamerParamsPushed_) {
        if (vStream_) {
            streamer_->setVideoParams(vStream_->codecpar, vEncCtx_->time_base);
        }
        if (aStream_) {
            streamer_->setAudioParams(aStream_->codecpar, aEncCtx_->time_base);
        }
        streamerParamsPushed_ = true;
    }
    return true;
}

bool FfmpegRecorder::addVideoStream()
{
    const AVCodec* codec = avcodec_find_encoder_by_name(cfg_.video_codec.c_str());
    if (!codec) {
        spdlog::error("FfmpegRecorder: video codec '{}' not found", cfg_.video_codec);
        return false;
    }

    vStream_ = avformat_new_stream(fmtCtx_, nullptr);
    if (!vStream_) return false;

    vEncCtx_ = avcodec_alloc_context3(codec);
    if (!vEncCtx_) return false;

    vEncCtx_->codec_id      = codec->id;
    vEncCtx_->codec_type    = AVMEDIA_TYPE_VIDEO;
    vEncCtx_->width         = vWidth_;
    vEncCtx_->height        = vHeight_;
    // Use millisecond precision for time_base to accurately reflect VFR/drops
    vEncCtx_->time_base     = {1, 1000};
    vEncCtx_->framerate     = {vFps_, 1};
    vEncCtx_->pix_fmt       = AV_PIX_FMT_YUV420P;
    vEncCtx_->bit_rate      = cfg_.video_bitrate;
    vEncCtx_->gop_size      = vFps_ * 2;    // keyframe every 2 s
    vEncCtx_->max_b_frames  = 0;            // no B-frames (lower latency)

    if (fmtCtx_->oformat->flags & AVFMT_GLOBALHEADER)
        vEncCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // libx264 preset
    av_opt_set(vEncCtx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(vEncCtx_->priv_data, "tune",   "zerolatency", 0);

    int ret = avcodec_open2(vEncCtx_, codec, nullptr);
    if (ret < 0) {
        spdlog::error("FfmpegRecorder: avcodec_open2 video failed: {}", ret);
        return false;
    }

    avcodec_parameters_from_context(vStream_->codecpar, vEncCtx_);
    vStream_->time_base = vEncCtx_->time_base;

    spdlog::info("FfmpegRecorder: video stream {}x{} @ {}fps codec={}",
                 vWidth_, vHeight_, vFps_, cfg_.video_codec);
    return true;
}

bool FfmpegRecorder::addAudioStream()
{
    const AVCodec* codec = avcodec_find_encoder_by_name(cfg_.audio_codec.c_str());
    if (!codec) {
        // fallback to pcm_s16le
        codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        if (!codec) {
            spdlog::error("FfmpegRecorder: audio codec '{}' not found", cfg_.audio_codec);
            return false;
        }
    }

    aStream_ = avformat_new_stream(fmtCtx_, nullptr);
    if (!aStream_) return false;

    aEncCtx_ = avcodec_alloc_context3(codec);
    if (!aEncCtx_) return false;

    aEncCtx_->codec_id      = codec->id;
    aEncCtx_->codec_type    = AVMEDIA_TYPE_AUDIO;
    aEncCtx_->sample_rate   = aSampleRate_;
    aEncCtx_->bit_rate      = cfg_.audio_bitrate;
    aEncCtx_->sample_fmt    = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57,28,100)
    av_channel_layout_default(&aEncCtx_->ch_layout, aChannels_);
#else
    aEncCtx_->channels      = aChannels_;
    aEncCtx_->channel_layout = av_get_default_channel_layout(aChannels_);
#endif
    // For audio encoders, time_base is typically 1 / sample_rate
    aEncCtx_->time_base     = {1, aSampleRate_};

    if (fmtCtx_->oformat->flags & AVFMT_GLOBALHEADER)
        aEncCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // aac encoder may require specific sample format
    if (codec->id == AV_CODEC_ID_AAC) {
        aEncCtx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }

    int ret = avcodec_open2(aEncCtx_, codec, nullptr);
    if (ret < 0) {
        spdlog::error("FfmpegRecorder: avcodec_open2 audio failed: {}", ret);
        return false;
    }

    avcodec_parameters_from_context(aStream_->codecpar, aEncCtx_);
    aStream_->time_base = {1, aSampleRate_};

    // Resampler: s16 -> fltp
    swrCtx_ = swr_alloc();
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57,28,100)
    AVChannelLayout inLayout, outLayout;
    av_channel_layout_default(&inLayout,  aChannels_);
    av_channel_layout_default(&outLayout, aChannels_);
    av_opt_set_chlayout(swrCtx_, "in_chlayout",  &inLayout,  0);
    av_opt_set_chlayout(swrCtx_, "out_chlayout", &outLayout, 0);
#else
    av_opt_set_int(swrCtx_, "in_channel_layout",  av_get_default_channel_layout(aChannels_), 0);
    av_opt_set_int(swrCtx_, "out_channel_layout", av_get_default_channel_layout(aChannels_), 0);
#endif
    av_opt_set_int(swrCtx_, "in_sample_rate",   aSampleRate_, 0);
    av_opt_set_int(swrCtx_, "out_sample_rate",  aSampleRate_, 0);
    av_opt_set_sample_fmt(swrCtx_, "in_sample_fmt",  AV_SAMPLE_FMT_S16,  0);
    av_opt_set_sample_fmt(swrCtx_, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
    swr_init(swrCtx_);

    // Audio FIFO for frame-size alignment
    aFifo_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, aChannels_,
                                  aEncCtx_->frame_size > 0 ? aEncCtx_->frame_size * 4 : 4096);

    spdlog::info("FfmpegRecorder: audio stream {}Hz {}ch codec={} bitrate={} gain={:.2f}",
                 aSampleRate_, aChannels_, cfg_.audio_codec,
                 cfg_.audio_bitrate, cfg_.audio_gain);
    return true;
}

void FfmpegRecorder::writeVideoAVFrame(const AVFrame* srcFrame, int64_t ptsMs)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!isOpen_) return;

    int width = srcFrame->width;
    int height = srcFrame->height;
    AVPixelFormat srcFmt = (AVPixelFormat)srcFrame->format;

    if (!swsCtx_ || width != srcW_ || height != srcH_) {
        if (swsCtx_) sws_freeContext(swsCtx_);
        swsCtx_ = sws_getContext(width, height, srcFmt,
                                 vWidth_, vHeight_, AV_PIX_FMT_YUV420P,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        srcW_ = width;
        srcH_ = height;
    }

    if (globalStartMs_ == -1) globalStartMs_ = ptsMs;
    int64_t newPts = ptsMs - globalStartMs_;
    
    int64_t scaledPts = av_rescale_q(newPts, {1, 1000}, vEncCtx_->time_base);
    if (scaledPts <= vPts_) scaledPts = vPts_ + 1;
    vPts_ = scaledPts;

    av_frame_make_writable(vEncodeFrame_);
    sws_scale(swsCtx_, srcFrame->data, srcFrame->linesize, 0, height,
              vEncodeFrame_->data, vEncodeFrame_->linesize);

    vEncodeFrame_->pts = scaledPts;

    int ret = avcodec_send_frame(vEncCtx_, vEncodeFrame_);
    if (ret < 0) return;

    av_packet_unref(encodePkt_);
    while (avcodec_receive_packet(vEncCtx_, encodePkt_) == 0) {
        // Stream: push BEFORE MP4 rescale (streamer expects encoder time_base)
        if (streamer_) streamer_->pushVideo(encodePkt_);
        av_packet_rescale_ts(encodePkt_, vEncCtx_->time_base, vStream_->time_base);
        encodePkt_->stream_index = vStream_->index;
        av_interleaved_write_frame(fmtCtx_, encodePkt_);
        av_packet_unref(encodePkt_);
    }
}

void FfmpegRecorder::writeVideoFrame(const uint8_t* yuv420p,
                                     int width, int height, int64_t ptsMs)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!isOpen_) return;

    // Scale if dimensions changed from encoder config
    if (!swsCtx_ || width != srcW_ || height != srcH_) {
        if (swsCtx_) sws_freeContext(swsCtx_);
        swsCtx_ = sws_getContext(width, height, AV_PIX_FMT_YUV420P,
                                  vWidth_, vHeight_, AV_PIX_FMT_YUV420P,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        srcW_ = width;
        srcH_ = height;
    }

    const uint8_t* srcData[4] = {
        yuv420p,
        yuv420p + width * height,
        yuv420p + width * height * 5 / 4,
        nullptr
    };
    int srcLinesize[4] = { width, width / 2, width / 2, 0 };

    if (globalStartMs_ == -1) globalStartMs_ = ptsMs;
    int64_t newPts = ptsMs - globalStartMs_;
    
    int64_t scaledPts = av_rescale_q(newPts, {1, 1000}, vEncCtx_->time_base);
    if (scaledPts <= vPts_) scaledPts = vPts_ + 1;
    vPts_ = scaledPts;

    av_frame_make_writable(vEncodeFrame_);
    sws_scale(swsCtx_, srcData, srcLinesize, 0, height,
              vEncodeFrame_->data, vEncodeFrame_->linesize);

    vEncodeFrame_->pts = scaledPts;

    int ret = avcodec_send_frame(vEncCtx_, vEncodeFrame_);
    if (ret < 0) return;

    av_packet_unref(encodePkt_);
    while (avcodec_receive_packet(vEncCtx_, encodePkt_) == 0) {
        if (streamer_) streamer_->pushVideo(encodePkt_);
        av_packet_rescale_ts(encodePkt_, vEncCtx_->time_base, vStream_->time_base);
        encodePkt_->stream_index = vStream_->index;
        av_interleaved_write_frame(fmtCtx_, encodePkt_);
        av_packet_unref(encodePkt_);
    }
}

void FfmpegRecorder::writeAudioSamples(const int16_t* pcm,
                                       int sampleCount, int64_t ptsMs)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!isOpen_ || !aStream_ || !aEncCtx_) return;

    if (globalStartMs_ == -1) globalStartMs_ = ptsMs;

    if (!aPtsInitialized_) {
        int64_t offsetMs = ptsMs - globalStartMs_;
        if (offsetMs > 0) {
            // Audio started after video. Advance the sample counter so the first frame
            // will have the correct PTS offset in the file.
            aPts_ = av_rescale_q(offsetMs, {1, 1000}, {1, aSampleRate_});
        }
        aPtsInitialized_ = true;
    }

    // ── Apply linear gain (config.audio_gain) with saturating clamp ──
    // gain == 1.0 → 直接用原始 pcm 指针，零拷贝；
    // gain != 1.0 → 拷贝到本地缓冲乘倍数后 clamp，避免改写 caller 的 const buffer。
    const double gain = cfg_.audio_gain;
    const int16_t* srcPcm = pcm;
    std::vector<int16_t> gainedBuf;
    if (gain != 1.0) {
        const int totalSamples = sampleCount * aChannels_;
        gainedBuf.resize(totalSamples);
        for (int i = 0; i < totalSamples; ++i) {
            int32_t s = static_cast<int32_t>(std::lround(pcm[i] * gain));
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            gainedBuf[i] = static_cast<int16_t>(s);
        }
        srcPcm = gainedBuf.data();
    }

    // Convert s16 -> fltp
    const int frameSize = aEncCtx_->frame_size > 0 ? aEncCtx_->frame_size : 1024;

    // Allocate temporary fltp buffer
    uint8_t** converted = nullptr;
    av_samples_alloc_array_and_samples(&converted, nullptr, aChannels_,
                                        sampleCount, AV_SAMPLE_FMT_FLTP, 0);
    const uint8_t* inData[1] = { reinterpret_cast<const uint8_t*>(srcPcm) };
    swr_convert(swrCtx_, converted, sampleCount, inData, sampleCount);

    // Push into FIFO
    av_audio_fifo_write(aFifo_, reinterpret_cast<void**>(converted), sampleCount);
    av_freep(&converted[0]);
    av_freep(&converted);

    // Drain FIFO in frame-sized chunks
    while (av_audio_fifo_size(aFifo_) >= frameSize) {
        av_frame_make_writable(aEncodeFrame_);
        av_audio_fifo_read(aFifo_, reinterpret_cast<void**>(aEncodeFrame_->data), frameSize);
        
        // Rescale sample count (aPts_) to the encoder's actual time_base
        aEncodeFrame_->pts = av_rescale_q(aPts_, {1, aSampleRate_}, aEncCtx_->time_base);
        aPts_ += frameSize;

        if (avcodec_send_frame(aEncCtx_, aEncodeFrame_) == 0) {
            av_packet_unref(encodePkt_);
            while (avcodec_receive_packet(aEncCtx_, encodePkt_) == 0) {
                if (streamer_) streamer_->pushAudio(encodePkt_);
                av_packet_rescale_ts(encodePkt_, aEncCtx_->time_base, aStream_->time_base);
                encodePkt_->stream_index = aStream_->index;
                av_interleaved_write_frame(fmtCtx_, encodePkt_);
                av_packet_unref(encodePkt_);
            }
        }
    }
}

bool FfmpegRecorder::writePacket(AVPacket* pkt, AVStream* st)
{
    pkt->stream_index = st->index;
    return av_interleaved_write_frame(fmtCtx_, pkt) >= 0;
}

void FfmpegRecorder::close()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!fmtCtx_) return;

    if (isOpen_) {
        AVPacket* pkt = av_packet_alloc();

        // We skip flushing (sending nullptr to avcodec_send_frame) to prevent
        // potential segfaults if the codec or its internal threads are already
        // in an unstable state due to H.323 connection termination/PChannel destruction.
        // Since we disabled B-frames for libx264, skipping flush is completely fine
        // and at most loses 1 frame of latency.
        
        av_packet_free(&pkt);

        av_write_trailer(fmtCtx_);
        isOpen_ = false;
        spdlog::info("FfmpegRecorder: closed {}", outputPath_);
    }

    if (fmtCtx_ && fmtCtx_->pb) avio_closep(&fmtCtx_->pb);
    avformat_free_context(fmtCtx_); fmtCtx_ = nullptr;

    if (vEncCtx_) { avcodec_free_context(&vEncCtx_); }
    if (aEncCtx_) { avcodec_free_context(&aEncCtx_); }
    if (swsCtx_)  { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (swrCtx_)  { swr_free(&swrCtx_); }
    if (aFifo_)   { av_audio_fifo_free(aFifo_); aFifo_ = nullptr; }
    if (vEncodeFrame_) { av_frame_free(&vEncodeFrame_); vEncodeFrame_ = nullptr; }
    if (aEncodeFrame_) { av_frame_free(&aEncodeFrame_); aEncodeFrame_ = nullptr; }
    if (encodePkt_) { av_packet_free(&encodePkt_); encodePkt_ = nullptr; }

    vStream_ = nullptr; aStream_ = nullptr;
    vPts_ = 0; aPts_ = 0;
    globalStartMs_ = -1;
    aPtsInitialized_ = false;
    srcW_ = 0; srcH_ = 0;
    // Intentionally NOT resetting streamerParamsPushed_ — codec params are
    // identical across reopen (same RecorderConfig), and resetting would
    // cause a race with the streamer thread already using cached params.
    // Intentionally NOT clearing streamer_ — owner may reopen this recorder
    // and expect the live stream to continue. Owner calls attachStreamer(nullptr)
    // to detach explicitly.

    // ── 后台 faststart 重写 ──────────────────────────────────────────
    // 已写好的 fragmented mp4 (movflags=frag_keyframe+empty_moov+...) 浏览器
    // seek 时要扫整个文件构建 sample table，体感卡顿。
    // 这里启个 detached thread 用 libav 把它重新 mux 成普通 mp4 + faststart：
    // moov atom 在文件头，浏览器 seek 即时响应。
    // - thread 内 std::system 风险：进程退出时被 cgroup KILL，tmp 文件残留
    //   → faststartRewrite 启动时会先清理同名 .faststart.tmp
    // - 重写期间原文件仍可访问（rename 是 atomic 替换）
    // - 失败原文件保持不变，下次重启或手动触发再试
    if (!outputPath_.empty()) {
        std::string p = outputPath_;
        std::thread([p]() {
            faststartRewrite(p);
        }).detach();
    }
}
