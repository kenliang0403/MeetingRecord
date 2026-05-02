#include "Mp4Faststart.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#include <spdlog/spdlog.h>

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {
const char* avErr(int rc, char* buf, size_t bufsz) {
    av_strerror(rc, buf, bufsz);
    return buf;
}
}  // namespace

bool faststartRewrite(const std::string& path)
{
    if (path.empty() || !fs::exists(path)) {
        spdlog::warn("faststart: file missing: {}", path);
        return false;
    }

    AVFormatContext* in_ctx  = nullptr;
    AVFormatContext* out_ctx = nullptr;
    AVDictionary*    opts    = nullptr;
    AVPacket*        pkt     = nullptr;
    bool ok = false;
    char errBuf[128];

    const std::string tmp = path + ".faststart.tmp";
    // 提前清理同路径残留 tmp（上一轮中断可能留下）
    {
        std::error_code ec;
        fs::remove(tmp, ec);
    }

    int rc = avformat_open_input(&in_ctx, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        spdlog::warn("faststart: open input failed ({}): {}",
                     avErr(rc, errBuf, sizeof errBuf), path);
        goto cleanup;
    }
    rc = avformat_find_stream_info(in_ctx, nullptr);
    if (rc < 0) {
        spdlog::warn("faststart: find_stream_info failed ({}): {}",
                     avErr(rc, errBuf, sizeof errBuf), path);
        goto cleanup;
    }

    rc = avformat_alloc_output_context2(&out_ctx, nullptr, "mp4", tmp.c_str());
    if (rc < 0 || !out_ctx) {
        spdlog::warn("faststart: alloc output ctx failed: {}", tmp);
        goto cleanup;
    }

    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        AVStream* st_in  = in_ctx->streams[i];
        AVStream* st_out = avformat_new_stream(out_ctx, nullptr);
        if (!st_out) {
            spdlog::warn("faststart: new_stream failed");
            goto cleanup;
        }
        rc = avcodec_parameters_copy(st_out->codecpar, st_in->codecpar);
        if (rc < 0) {
            spdlog::warn("faststart: codec_param_copy failed: {}",
                         avErr(rc, errBuf, sizeof errBuf));
            goto cleanup;
        }
        st_out->codecpar->codec_tag = 0;
        st_out->time_base = st_in->time_base;
    }

    rc = avio_open(&out_ctx->pb, tmp.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) {
        spdlog::warn("faststart: avio_open {} failed ({})",
                     tmp, avErr(rc, errBuf, sizeof errBuf));
        goto cleanup;
    }

    av_dict_set(&opts, "movflags", "+faststart", 0);
    rc = avformat_write_header(out_ctx, &opts);
    if (rc < 0) {
        spdlog::warn("faststart: write_header failed: {}",
                     avErr(rc, errBuf, sizeof errBuf));
        goto cleanup;
    }

    pkt = av_packet_alloc();
    if (!pkt) goto cleanup;

    while (true) {
        rc = av_read_frame(in_ctx, pkt);
        if (rc == AVERROR_EOF) break;
        if (rc < 0) {
            spdlog::warn("faststart: read_frame failed mid-stream ({}); abort {}",
                         avErr(rc, errBuf, sizeof errBuf), path);
            goto cleanup;
        }
        AVStream* st_in  = in_ctx->streams[pkt->stream_index];
        AVStream* st_out = out_ctx->streams[pkt->stream_index];
        av_packet_rescale_ts(pkt, st_in->time_base, st_out->time_base);
        pkt->pos = -1;
        rc = av_interleaved_write_frame(out_ctx, pkt);
        av_packet_unref(pkt);
        if (rc < 0) {
            spdlog::warn("faststart: write_frame failed ({}): {}",
                         avErr(rc, errBuf, sizeof errBuf), path);
            goto cleanup;
        }
    }

    rc = av_write_trailer(out_ctx);
    if (rc < 0) {
        spdlog::warn("faststart: write_trailer failed ({}): {}",
                     avErr(rc, errBuf, sizeof errBuf), path);
        goto cleanup;
    }

    {
        // atomic replace
        std::error_code ec;
        // 关闭 output 后再 rename，避免 Windows-style 占用（Linux 上其实可以同时持有）
        if (out_ctx->pb) avio_closep(&out_ctx->pb);
        fs::rename(tmp, path, ec);
        if (ec) {
            spdlog::warn("faststart: rename {} -> {} failed: {}",
                         tmp, path, ec.message());
            goto cleanup;
        }
    }
    ok = true;
    {
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        spdlog::info("faststart: rewrote {} ({}MB)", path,
                     ec ? 0 : (unsigned)(sz / (1024 * 1024)));
    }

cleanup:
    if (pkt) av_packet_free(&pkt);
    if (in_ctx) avformat_close_input(&in_ctx);
    if (out_ctx) {
        if (out_ctx->pb) avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
    }
    if (opts) av_dict_free(&opts);
    if (!ok) {
        std::error_code ec;
        fs::remove(tmp, ec);
    }
    return ok;
}
