// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "transcode_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

// FFmpeg includes
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace quicr {
namespace transcode {

namespace {
/**
 * @brief Custom IO buffer for feeding fragmented data to FFmpeg
 */
struct IOBuffer
{
    const uint8_t* data{ nullptr };
    size_t size{ 0 };
    size_t pos{ 0 };

    void Reset(const uint8_t* new_data, size_t new_size)
    {
        data = new_data;
        size = new_size;
        pos = 0;
    }

    size_t BytesRemaining() const { return size - pos; }
};

/**
 * @brief Custom read callback for AVIOContext
 */
int ReadPacket(void* opaque, uint8_t* buf, int buf_size)
{
    auto* io_buf = static_cast<IOBuffer*>(opaque);
    if (!io_buf || !io_buf->data) {
        return AVERROR_EOF;
    }

    const size_t to_read = std::min(static_cast<size_t>(buf_size), io_buf->BytesRemaining());
    if (to_read == 0) {
        return AVERROR_EOF;
    }

    std::memcpy(buf, io_buf->data + io_buf->pos, to_read);
    io_buf->pos += to_read;

    return static_cast<int>(to_read);
}

/**
 * @brief Custom write callback for output buffer
 */
int WritePacket(void* opaque, uint8_t* buf, int buf_size)
{
    auto* vec = static_cast<std::vector<uint8_t>*>(opaque);
    vec->insert(vec->end(), buf, buf + buf_size);
    return buf_size;
}

} // anonymous namespace

/**
 * @brief Implementation class for TranscodeClient
 */
class TranscodeClient::Impl
{
  public:
    explicit Impl(const TranscodeConfig& config)
      : config_(config)
    {
    }

    ~Impl() { Cleanup(); }

    bool PushInputInit(const uint8_t* data, size_t size)
    {
        if (!data || size == 0) {
            last_error_ = "Invalid input init segment";
            return false;
        }

        // Store init segment
        init_segment_.assign(data, data + size);

        // Initialize input context with init segment
        if (!InitializeInputContext()) {
            return false;
        }

        // Note: We delay output context initialization until we decode the first frame
        // because we need to know the pixel format from actual frame data

        ready_ = true;
        return true;
    }

    bool PushInputFragment(const uint8_t* data, size_t size)
    {
        if (!ready_) {
            last_error_ = "Client not ready - push init segment first";
            return false;
        }

        if (!data || size == 0) {
            last_error_ = "Invalid input fragment";
            return false;
        }

        return ProcessFragment(data, size);
    }

    bool Flush()
    {
        if (!ready_) {
            return true; // Nothing to flush
        }

        // Flush decoder
        if (decoder_ctx_) {
            avcodec_send_packet(decoder_ctx_, nullptr);
            DrainDecoder();
        }

        // Flush encoder
        if (encoder_ctx_) {
            avcodec_send_frame(encoder_ctx_, nullptr);
            DrainEncoder();
        }

        // Write trailer
        if (output_fmt_ctx_ && output_initialized_) {
            av_write_trailer(output_fmt_ctx_);
        }

        return true;
    }

    void Close() { Cleanup(); }

    void SetOutputInitCallback(OutputInitCallback callback) { output_init_cb_ = std::move(callback); }

    void SetOutputFragmentCallback(OutputFragmentCallback callback) { output_fragment_cb_ = std::move(callback); }

    bool IsReady() const { return ready_; }

    std::string GetLastError() const { return last_error_; }

  private:
    void Cleanup()
    {
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
            sws_ctx_ = nullptr;
        }

        if (scaled_frame_) {
            av_frame_free(&scaled_frame_);
        }

        if (decoder_ctx_) {
            avcodec_free_context(&decoder_ctx_);
        }

        if (encoder_ctx_) {
            avcodec_free_context(&encoder_ctx_);
        }

        if (input_fmt_ctx_) {
            if (input_fmt_ctx_->pb) {
                av_freep(&input_fmt_ctx_->pb->buffer);
                avio_context_free(&input_fmt_ctx_->pb);
            }
            avformat_close_input(&input_fmt_ctx_);
        }

        if (output_fmt_ctx_) {
            if (output_fmt_ctx_->pb) {
                av_freep(&output_fmt_ctx_->pb->buffer);
                avio_context_free(&output_fmt_ctx_->pb);
            }
            avformat_free_context(output_fmt_ctx_);
            output_fmt_ctx_ = nullptr;
        }

        ready_ = false;
    }

    bool InitializeInputContext()
    {
        // Create IO buffer
        input_io_buf_.Reset(init_segment_.data(), init_segment_.size());

        // Allocate IO context
        const int avio_buffer_size = 4096;
        uint8_t* avio_buffer = static_cast<uint8_t*>(av_malloc(avio_buffer_size));
        if (!avio_buffer) {
            last_error_ = "Failed to allocate AVIO buffer";
            return false;
        }

        AVIOContext* avio_ctx = avio_alloc_context(avio_buffer,
                                                    avio_buffer_size,
                                                    0, // read-only
                                                    &input_io_buf_,
                                                    ReadPacket,
                                                    nullptr,
                                                    nullptr);
        if (!avio_ctx) {
            av_free(avio_buffer);
            last_error_ = "Failed to allocate AVIO context";
            return false;
        }

        // Allocate format context
        input_fmt_ctx_ = avformat_alloc_context();
        if (!input_fmt_ctx_) {
            avio_context_free(&avio_ctx);
            last_error_ = "Failed to allocate input format context";
            return false;
        }

        input_fmt_ctx_->pb = avio_ctx;

        // Open input
        int ret = avformat_open_input(&input_fmt_ctx_, nullptr, nullptr, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            last_error_ = std::string("Failed to open input: ") + errbuf;
            return false;
        }

        // Find stream info
        ret = avformat_find_stream_info(input_fmt_ctx_, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            last_error_ = std::string("Failed to find stream info: ") + errbuf;
            return false;
        }

        // Find video stream
        for (unsigned int i = 0; i < input_fmt_ctx_->nb_streams; i++) {
            if (input_fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index_ = i;
                break;
            }
        }

        if (video_stream_index_ < 0) {
            last_error_ = "No video stream found in input";
            return false;
        }

        // Set up decoder
        AVCodecParameters* codecpar = input_fmt_ctx_->streams[video_stream_index_]->codecpar;
        const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
        if (!decoder) {
            last_error_ = "Failed to find decoder";
            return false;
        }

        decoder_ctx_ = avcodec_alloc_context3(decoder);
        if (!decoder_ctx_) {
            last_error_ = "Failed to allocate decoder context";
            return false;
        }

        ret = avcodec_parameters_to_context(decoder_ctx_, codecpar);
        if (ret < 0) {
            last_error_ = "Failed to copy codec parameters";
            return false;
        }

        ret = avcodec_open2(decoder_ctx_, decoder, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            last_error_ = std::string("Failed to open decoder: ") + errbuf;
            return false;
        }

        // Store input time base
        input_time_base_ = input_fmt_ctx_->streams[video_stream_index_]->time_base;

        if (config_.debug) {
            std::cout << "Input initialized: " << codecpar->width << "x" << codecpar->height << " codec: "
                      << avcodec_get_name(codecpar->codec_id) << std::endl;
        }

        return true;
    }

    bool InitializeOutputContext()
    {
        // Determine output dimensions
        int out_width = config_.target_width > 0 ? config_.target_width : decoder_ctx_->width;
        int out_height = config_.target_height > 0 ? config_.target_height : decoder_ctx_->height;

        // Determine output codec
        AVCodecID out_codec_id = AV_CODEC_ID_H264; // Default to H.264
        if (!config_.output_codec.empty()) {
            if (config_.output_codec == "hevc" || config_.output_codec == "h265") {
                out_codec_id = AV_CODEC_ID_HEVC;
            }
        }

        // Find encoder
        const AVCodec* encoder = avcodec_find_encoder(out_codec_id);
        if (!encoder) {
            last_error_ = "Failed to find encoder";
            return false;
        }

        encoder_ctx_ = avcodec_alloc_context3(encoder);
        if (!encoder_ctx_) {
            last_error_ = "Failed to allocate encoder context";
            return false;
        }

        // Set encoder parameters
        encoder_ctx_->width = out_width;
        encoder_ctx_->height = out_height;
        encoder_ctx_->time_base = input_time_base_;
        encoder_ctx_->framerate = decoder_ctx_->framerate;
        encoder_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

        if (config_.target_bitrate > 0) {
            encoder_ctx_->bit_rate = config_.target_bitrate;
        } else {
            // Estimate bitrate based on resolution
            encoder_ctx_->bit_rate = out_width * out_height * 2; // rough estimate
        }

        encoder_ctx_->gop_size = 30;
        encoder_ctx_->max_b_frames = 0; // No B-frames for low latency

        // Set encoder preset
        av_opt_set(encoder_ctx_->priv_data, "preset", config_.encoder_preset.c_str(), 0);
        av_opt_set(encoder_ctx_->priv_data, "tune", "zerolatency", 0);

        // Open encoder
        int ret = avcodec_open2(encoder_ctx_, encoder, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            last_error_ = std::string("Failed to open encoder: ") + errbuf;
            return false;
        }

        // Initialize scaler
        sws_ctx_ = sws_getContext(decoder_ctx_->width,
                                   decoder_ctx_->height,
                                   decoder_ctx_->pix_fmt,
                                   out_width,
                                   out_height,
                                   AV_PIX_FMT_YUV420P,
                                   SWS_BILINEAR,
                                   nullptr,
                                   nullptr,
                                   nullptr);
        if (!sws_ctx_) {
            last_error_ = "Failed to initialize scaler";
            return false;
        }

        // Allocate scaled frame
        scaled_frame_ = av_frame_alloc();
        if (!scaled_frame_) {
            last_error_ = "Failed to allocate scaled frame";
            return false;
        }

        scaled_frame_->format = AV_PIX_FMT_YUV420P;
        scaled_frame_->width = out_width;
        scaled_frame_->height = out_height;

        ret = av_frame_get_buffer(scaled_frame_, 0);
        if (ret < 0) {
            last_error_ = "Failed to allocate scaled frame buffer";
            return false;
        }

        // Create output format context for fragmented MP4
        avformat_alloc_output_context2(&output_fmt_ctx_, nullptr, "mp4", nullptr);
        if (!output_fmt_ctx_) {
            last_error_ = "Failed to allocate output format context";
            return false;
        }

        // Create output video stream
        AVStream* out_stream = avformat_new_stream(output_fmt_ctx_, nullptr);
        if (!out_stream) {
            last_error_ = "Failed to create output stream";
            return false;
        }

        ret = avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx_);
        if (ret < 0) {
            last_error_ = "Failed to copy encoder parameters to output stream";
            return false;
        }

        out_stream->time_base = encoder_ctx_->time_base;
        output_stream_index_ = out_stream->index;

        // Set up custom IO for output
        const int out_buffer_size = 4096;
        uint8_t* out_buffer = static_cast<uint8_t*>(av_malloc(out_buffer_size));
        if (!out_buffer) {
            last_error_ = "Failed to allocate output buffer";
            return false;
        }

        AVIOContext* out_avio_ctx =
          avio_alloc_context(out_buffer, out_buffer_size, 1, &output_buffer_, nullptr, WritePacket, nullptr);
        if (!out_avio_ctx) {
            av_free(out_buffer);
            last_error_ = "Failed to allocate output AVIO context";
            return false;
        }

        output_fmt_ctx_->pb = out_avio_ctx;

        // Set fragmented MP4 options
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
        av_dict_set(&opts, "frag_duration", "1000000", 0); // 1 second fragments

        // Write header
        ret = avformat_write_header(output_fmt_ctx_, &opts);
        av_dict_free(&opts);

        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            last_error_ = std::string("Failed to write output header: ") + errbuf;
            return false;
        }

        output_initialized_ = true;

        // Flush to get init segment
        avio_flush(output_fmt_ctx_->pb);

        // Send init segment via callback
        if (output_init_cb_ && !output_buffer_.empty()) {
            output_init_cb_(output_buffer_.data(), output_buffer_.size());
            init_segment_sent_ = true;
        }

        // Clear buffer for fragments
        output_buffer_.clear();

        if (config_.debug) {
            std::cout << "Output initialized: " << out_width << "x" << out_height << " codec: "
                      << avcodec_get_name(out_codec_id) << std::endl;
        }

        return true;
    }

    bool ProcessFragment(const uint8_t* data, size_t size)
    {
        // Create a temporary input context for this fragment
        IOBuffer frag_io_buf;
        frag_io_buf.Reset(data, size);

        const int avio_buffer_size = 4096;
        uint8_t* avio_buffer = static_cast<uint8_t*>(av_malloc(avio_buffer_size));
        if (!avio_buffer) {
            last_error_ = "Failed to allocate fragment AVIO buffer";
            return false;
        }

        AVIOContext* avio_ctx = avio_alloc_context(avio_buffer,
                                                    avio_buffer_size,
                                                    0, // read-only
                                                    &frag_io_buf,
                                                    ReadPacket,
                                                    nullptr,
                                                    nullptr);
        if (!avio_ctx) {
            av_free(avio_buffer);
            last_error_ = "Failed to allocate fragment AVIO context";
            return false;
        }

        AVFormatContext* frag_ctx = avformat_alloc_context();
        if (!frag_ctx) {
            avio_context_free(&avio_ctx);
            last_error_ = "Failed to allocate fragment format context";
            return false;
        }

        frag_ctx->pb = avio_ctx;

        int ret = avformat_open_input(&frag_ctx, nullptr, nullptr, nullptr);
        if (ret < 0) {
            av_freep(&avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            avformat_free_context(frag_ctx);
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            last_error_ = std::string("Failed to open fragment: ") + errbuf;
            return false;
        }

        // Read packets from fragment
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(frag_ctx, packet) >= 0) {
            if (packet->stream_index == video_stream_index_) {
                ProcessPacket(packet);
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);

        // Cleanup fragment context
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);
        avformat_close_input(&frag_ctx);

        // Send output fragment if available
        if (output_fragment_cb_ && !output_buffer_.empty()) {
            output_fragment_cb_(output_buffer_.data(), output_buffer_.size());
            output_buffer_.clear();
        }

        return true;
    }

    void ProcessPacket(AVPacket* packet)
    {
        // Send packet to decoder
        int ret = avcodec_send_packet(decoder_ctx_, packet);
        if (ret < 0) {
            return;
        }

        // Receive decoded frames
        AVFrame* frame = av_frame_alloc();
        while (ret >= 0) {
            ret = avcodec_receive_frame(decoder_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                break;
            }

            // Scale and encode frame
            ProcessFrame(frame);
        }
        av_frame_free(&frame);
    }

    void ProcessFrame(AVFrame* frame)
    {
        // Lazy initialization: set up output on first frame
        if (!output_initialized_) {
            if (!InitializeOutputContext()) {
                SPDLOG_ERROR("Failed to initialize output context: {}", last_error_);
                return;
            }
        }

        // Scale frame
        sws_scale(sws_ctx_,
                  frame->data,
                  frame->linesize,
                  0,
                  decoder_ctx_->height,
                  scaled_frame_->data,
                  scaled_frame_->linesize);

        // Copy timing information
        scaled_frame_->pts = frame->pts;
        scaled_frame_->pkt_dts = frame->pkt_dts;
        scaled_frame_->pict_type = frame->pict_type;
        scaled_frame_->key_frame = frame->key_frame;

        // Send frame to encoder
        int ret = avcodec_send_frame(encoder_ctx_, scaled_frame_);
        if (ret < 0) {
            return;
        }

        // Receive encoded packets
        EncodeAndWrite();
    }

    void DrainDecoder()
    {
        AVFrame* frame = av_frame_alloc();
        int ret;
        while ((ret = avcodec_receive_frame(decoder_ctx_, frame)) >= 0) {
            ProcessFrame(frame);
        }
        av_frame_free(&frame);
    }

    void DrainEncoder()
    {
        int ret;
        while ((ret = avcodec_receive_packet(encoder_ctx_, encoder_packet_)) >= 0) {
            WriteOutputPacket(encoder_packet_);
            av_packet_unref(encoder_packet_);
        }
    }

    void EncodeAndWrite()
    {
        if (!encoder_packet_) {
            encoder_packet_ = av_packet_alloc();
        }

        int ret;
        while ((ret = avcodec_receive_packet(encoder_ctx_, encoder_packet_)) >= 0) {
            WriteOutputPacket(encoder_packet_);
            av_packet_unref(encoder_packet_);
        }
    }

    void WriteOutputPacket(AVPacket* packet)
    {
        // Rescale timestamps
        packet->stream_index = output_stream_index_;
        av_packet_rescale_ts(packet,
                             encoder_ctx_->time_base,
                             output_fmt_ctx_->streams[output_stream_index_]->time_base);

        // Write packet
        av_interleaved_write_frame(output_fmt_ctx_, packet);
    }

  private:
    TranscodeConfig config_;
    bool ready_{ false };
    std::string last_error_;

    // Input
    std::vector<uint8_t> init_segment_;
    IOBuffer input_io_buf_;
    AVFormatContext* input_fmt_ctx_{ nullptr };
    int video_stream_index_{ -1 };
    AVCodecContext* decoder_ctx_{ nullptr };
    AVRational input_time_base_{};

    // Scaling
    SwsContext* sws_ctx_{ nullptr };
    AVFrame* scaled_frame_{ nullptr };

    // Output
    AVCodecContext* encoder_ctx_{ nullptr };
    AVFormatContext* output_fmt_ctx_{ nullptr };
    int output_stream_index_{ 0 };
    std::vector<uint8_t> output_buffer_;
    bool output_initialized_{ false };
    bool init_segment_sent_{ false };
    AVPacket* encoder_packet_{ nullptr };

    // Callbacks
    OutputInitCallback output_init_cb_;
    OutputFragmentCallback output_fragment_cb_;
};

// ============================================================================
// TranscodeClient public interface implementation
// ============================================================================

TranscodeClient::TranscodeClient(const TranscodeConfig& config)
  : impl_(std::make_unique<Impl>(config))
{
}

TranscodeClient::~TranscodeClient() = default;

std::shared_ptr<TranscodeClient>
TranscodeClient::Create(const TranscodeConfig& config)
{
    return std::shared_ptr<TranscodeClient>(new TranscodeClient(config));
}

void
TranscodeClient::SetOutputInitCallback(OutputInitCallback callback)
{
    impl_->SetOutputInitCallback(std::move(callback));
}

void
TranscodeClient::SetOutputFragmentCallback(OutputFragmentCallback callback)
{
    impl_->SetOutputFragmentCallback(std::move(callback));
}

bool
TranscodeClient::PushInputInit(const uint8_t* data, size_t size)
{
    return impl_->PushInputInit(data, size);
}

bool
TranscodeClient::PushInputFragment(const uint8_t* data, size_t size)
{
    return impl_->PushInputFragment(data, size);
}

bool
TranscodeClient::Flush()
{
    return impl_->Flush();
}

void
TranscodeClient::Close()
{
    impl_->Close();
}

bool
TranscodeClient::IsReady() const
{
    return impl_->IsReady();
}

std::string
TranscodeClient::GetLastError() const
{
    return impl_->GetLastError();
}

} // namespace transcode
} // namespace quicr
