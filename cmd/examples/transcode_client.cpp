// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "transcode_client.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
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

// ----------------------------------------------------------------------------
// RingBuffer Implementation
// ----------------------------------------------------------------------------

RingBuffer::RingBuffer(size_t capacity) : buffer_(capacity) {
    SPDLOG_DEBUG("RingBuffer initialized with capacity: {} bytes", capacity);
}

void RingBuffer::Write(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((buffer_.size() - stored_bytes_) < size) {
        Expand(size);
    }

    size_t first_chunk = std::min(size, buffer_.size() - write_pos_);
    std::memcpy(&buffer_[write_pos_], data, first_chunk);
    std::memcpy(&buffer_[0], data + first_chunk, size - first_chunk);

    write_pos_ = (write_pos_ + size) % buffer_.size();
    stored_bytes_ += size;
}

int RingBuffer::Read(uint8_t* dest, int size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stored_bytes_ == 0) return 0;

    size_t to_read = std::min(static_cast<size_t>(size), stored_bytes_);
    size_t first_chunk = std::min(to_read, buffer_.size() - read_pos_);

    std::memcpy(dest, &buffer_[read_pos_], first_chunk);
    std::memcpy(dest + first_chunk, &buffer_[0], to_read - first_chunk);

    read_pos_ = (read_pos_ + to_read) % buffer_.size();
    stored_bytes_ -= to_read;

    return static_cast<int>(to_read);
}

size_t RingBuffer::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stored_bytes_;
}

bool RingBuffer::IsEmptyUnsafe() const {
    return stored_bytes_ == 0;
}

void RingBuffer::Expand(size_t needed) {
    std::vector<uint8_t> new_buf(buffer_.size() * 2 + needed);
    size_t current_size = stored_bytes_;

    if (stored_bytes_ > 0) {
        size_t first_chunk = std::min(stored_bytes_, buffer_.size() - read_pos_);
        std::memcpy(&new_buf[0], &buffer_[read_pos_], first_chunk);
        std::memcpy(&new_buf[first_chunk], &buffer_[0], stored_bytes_ - first_chunk);
    }

    buffer_ = std::move(new_buf);
    read_pos_ = 0;
    write_pos_ = current_size;
}

namespace {
    // Helper for FFmpeg IO output
    int WriteOutputPacket(void* opaque, const uint8_t* buf, int buf_size)
    {
        auto* vec = static_cast<std::vector<uint8_t>*>(opaque);
        vec->insert(vec->end(), buf, buf + buf_size);
        return buf_size;
    }
}

// ----------------------------------------------------------------------------
// TranscodeClient Implementation
// ----------------------------------------------------------------------------

TranscodeClient::TranscodeClient(const TranscodeConfig& config) : config_(config), input_ring_buffer_(2 * 1024 * 1024)
{
    SPDLOG_INFO("TranscodeClient initialized.");
    running_ = true;
    worker_thread_ = std::thread(&TranscodeClient::TranscodeLoop, this);
}


TranscodeClient::~TranscodeClient()
{
    StopAndJoin();
    Cleanup();
}

void TranscodeClient::StopAndJoin()
{
    {
        std::lock_guard<std::mutex> lock(cv_mutex_);
        if (!running_) return; // Already stopped
        running_ = false;
    }
    data_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void TranscodeClient::Cleanup()
{
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (scaled_frame_) av_frame_free(&scaled_frame_);
    if (decoder_ctx_) avcodec_free_context(&decoder_ctx_);
    if (encoder_ctx_) avcodec_free_context(&encoder_ctx_);

    if (input_fmt_ctx_) {
        avformat_close_input(&input_fmt_ctx_);
        input_fmt_ctx_ = nullptr;
    }
    if (avio_ctx_buffer_) {
        av_free(avio_ctx_buffer_);
        avio_ctx_buffer_ = nullptr;
    }

    if (output_fmt_ctx_) {
        if (output_fmt_ctx_->pb) {
            av_freep(&output_fmt_ctx_->pb->buffer);
            avio_context_free(&output_fmt_ctx_->pb);
        }
        avformat_free_context(output_fmt_ctx_);
        output_fmt_ctx_ = nullptr;
    }
}

void TranscodeClient::Close() {
    StopAndJoin();
    Cleanup();
}

bool TranscodeClient::IsReady() const { return has_init_segment_; }

std::string TranscodeClient::GetLastError() const { return last_error_; }

void TranscodeClient::SetOutputInitCallback(OutputInitCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    output_init_cb_ = std::move(callback);
}

void TranscodeClient::SetOutputFragmentCallback(OutputFragmentCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    output_fragment_cb_ = std::move(callback);
}

bool TranscodeClient::PushInputInit(const uint8_t* data, size_t size)
{
    if (!data || size == 0) return false;
    {
        input_ring_buffer_.Write(data, size);
        has_init_segment_ = true;
    }
    data_cv_.notify_one();
    if (config_.debug) {
        SPDLOG_INFO("Buffered init segment: {} bytes", size);
    }
    return true;
}

bool TranscodeClient::PushInputFragment(const uint8_t* data, size_t size)
{
    if (!has_init_segment_) {
        last_error_ = "Client not ready - push init segment first";
        return false;
    }
    if (!data || size == 0) return false;

    input_ring_buffer_.Write(data, size);
    data_cv_.notify_one();
    return true;
}

bool TranscodeClient::Flush()
{
    // Wait for buffer to drain or stop
    while (input_ring_buffer_.Size() > 0 && running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

int TranscodeClient::ReadBufferCallback(void* opaque, uint8_t* buf, int buf_size)
{
    auto* self = static_cast<TranscodeClient*>(opaque);
    std::unique_lock<std::mutex> lock(self->cv_mutex_);

    // Wait for data or shutdown
    self->data_cv_.wait(lock, [self] {
        return (self->input_ring_buffer_.Size() > 0) || !self->running_;
    });

    if (!self->running_) {
        return AVERROR_EOF;
    }

    // Release CV lock before reading from RingBuffer (it has its own lock)
    lock.unlock();

    return self->input_ring_buffer_.Read(buf, buf_size);
}

void TranscodeClient::TranscodeLoop()
{
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            data_cv_.wait(lock, [this] {
                return (input_ring_buffer_.Size() > 0) || !running_;
            });
        }
        if (!running_) break;
        SPDLOG_DEBUG("TranscodeLoop: Processing pending data...");
        ProcessPendingData();
    }
}

void TranscodeClient::ProcessPendingData()
{

    if (!input_fmt_ctx_) {
        SPDLOG_DEBUG("Initializing demuxer...");
        if (!InitializeDemuxer()) return;
    }

    AVPacket* packet = av_packet_alloc();
    while (running_) {
        int ret = av_read_frame(input_fmt_ctx_, packet);
        if (ret == AVERROR_EOF) break;
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
             SPDLOG_ERROR("Demuxer error: {}", ret);
             break;
        }

        if (packet->stream_index == video_stream_index_) {
            ProcessPacket(packet);
        }
        av_packet_unref(packet);

        // Break if we processed everything currently in the buffer (simplification)
        // Note: FFmpeg internal buffers might mean we don't break exactly when RingBuffer is empty,
        // but the outer loop handles the wait.
        // For strictly following ring buffer:
        if (input_ring_buffer_.Size() == 0) break;
    }
    av_packet_free(&packet);
}

bool TranscodeClient::InitializeDemuxer()
{
    if (input_fmt_ctx_) return true;

    input_fmt_ctx_ = avformat_alloc_context();
    const int avio_buffer_size = 32768;
    avio_ctx_buffer_ = static_cast<uint8_t*>(av_malloc(avio_buffer_size));

    AVIOContext* avio_ctx = avio_alloc_context(
        avio_ctx_buffer_, avio_buffer_size, 0, this,
        ReadBufferCallback, nullptr, nullptr
    );

    if (!avio_ctx) return false;
    input_fmt_ctx_->pb = avio_ctx;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "probesize", "32768", 0);
    av_dict_set(&options, "analyzeduration", "0", 0);
    av_dict_set(&options, "flags", "nobuffer", 0);

    SPDLOG_INFO("Opening input stream...");
    int ret = avformat_open_input(&input_fmt_ctx_, nullptr, nullptr, &options);
    av_dict_free(&options);
    SPDLOG_DEBUG("Input stream opened.");

    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        SPDLOG_ERROR("avformat_open_input failed: {}", errbuf);
        return false;
    }

    if (avformat_find_stream_info(input_fmt_ctx_, nullptr) < 0) return false;

    for (unsigned int i = 0; i < input_fmt_ctx_->nb_streams; i++) {
        if (input_fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = i;
            // Store timebase
            input_time_base_num_ = input_fmt_ctx_->streams[i]->time_base.num;
            input_time_base_den_ = input_fmt_ctx_->streams[i]->time_base.den;

            AVCodecParameters* codecpar = input_fmt_ctx_->streams[i]->codecpar;
            const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
            if (decoder) {
                decoder_ctx_ = avcodec_alloc_context3(decoder);
                avcodec_parameters_to_context(decoder_ctx_, codecpar);
                decoder_ctx_->thread_count = 1;
                if (avcodec_open2(decoder_ctx_, decoder, nullptr) < 0) return false;
            }
            break;
        }
    }
    return (decoder_ctx_ != nullptr);
}

void TranscodeClient::ProcessPacket(AVPacket* packet)
{
    if (!decoder_ctx_) return;
    int ret = avcodec_send_packet(decoder_ctx_, packet);
    if (ret < 0) return;

    AVFrame* frame = av_frame_alloc();
    while (ret >= 0) {
        ret = avcodec_receive_frame(decoder_ctx_, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        ProcessFrame(frame);
    }
    av_frame_free(&frame);
}

void TranscodeClient::ProcessFrame(AVFrame* frame)
{
    if (!output_initialized_) {
        if (!InitializeOutput()) return;
    }

    if (!sws_ctx_ || frame->width != decoder_ctx_->width || frame->height != decoder_ctx_->height) {
        if (sws_ctx_) sws_freeContext(sws_ctx_);
        sws_ctx_ = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                  encoder_ctx_->width, encoder_ctx_->height, AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
    }

    sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height,
              scaled_frame_->data, scaled_frame_->linesize);

    AVRational in_tb;
    in_tb.num = input_time_base_num_;
    in_tb.den = input_time_base_den_;

    if (frame->pts != AV_NOPTS_VALUE) {
        scaled_frame_->pts = av_rescale_q(frame->pts, in_tb, encoder_ctx_->time_base);
    } else {
        scaled_frame_->pts = next_pts_++;
    }

    if (scaled_frame_->pts <= last_encoded_pts_) {
        scaled_frame_->pts = last_encoded_pts_ + 1;
    }
    last_encoded_pts_ = scaled_frame_->pts;

    int ret = avcodec_send_frame(encoder_ctx_, scaled_frame_);
    if (ret >= 0) DrainEncoder();
}

void TranscodeClient::DrainEncoder() {
    AVPacket* packet = av_packet_alloc();
    while (avcodec_receive_packet(encoder_ctx_, packet) >= 0) {
        bool is_keyframe = (packet->flags & AV_PKT_FLAG_KEY);
        WriteOutputPacketFunc(packet);
        FlushOutput(is_keyframe);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
}

bool TranscodeClient::InitializeOutput()
{
    int out_width = config_.target_width > 0 ? config_.target_width : decoder_ctx_->width;
    int out_height = config_.target_height > 0 ? config_.target_height : decoder_ctx_->height;
    AVCodecID out_codec_id =  AV_CODEC_ID_H264;

    const AVCodec* encoder = avcodec_find_encoder(out_codec_id);
    if (!encoder) return false;

    encoder_ctx_ = avcodec_alloc_context3(encoder);
    encoder_ctx_->width = out_width;
    encoder_ctx_->height = out_height;
    encoder_ctx_->time_base = (AVRational){1, 90000};
    encoder_ctx_->framerate = decoder_ctx_->framerate;
    encoder_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_ctx_->gop_size = 30;
    encoder_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (config_.target_bitrate > 0) encoder_ctx_->bit_rate = config_.target_bitrate;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "veryfast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    avcodec_open2(encoder_ctx_, encoder, &opts);
    av_dict_free(&opts);

    scaled_frame_ = av_frame_alloc();
    scaled_frame_->format = AV_PIX_FMT_YUV420P;
    scaled_frame_->width = out_width;
    scaled_frame_->height = out_height;
    av_frame_get_buffer(scaled_frame_, 0);

    avformat_alloc_output_context2(&output_fmt_ctx_, nullptr, "mp4", nullptr);
    AVStream* out_stream = avformat_new_stream(output_fmt_ctx_, nullptr);
    avcodec_parameters_from_context(out_stream->codecpar, encoder_ctx_);
    out_stream->time_base = encoder_ctx_->time_base;
    output_stream_index_ = out_stream->index;

    const int out_buffer_size = 32768;
    uint8_t* out_buffer = static_cast<uint8_t*>(av_malloc(out_buffer_size));
    AVIOContext* out_avio = avio_alloc_context(out_buffer, out_buffer_size, 1, &output_buffer_, nullptr, WriteOutputPacket, nullptr);
    output_fmt_ctx_->pb = out_avio;

    AVDictionary* muxer_opts = nullptr;
    av_dict_set(&muxer_opts, "movflags", "frag_every_frame+empty_moov+default_base_moof", 0);
    avformat_write_header(output_fmt_ctx_, &muxer_opts);
    av_dict_free(&muxer_opts);

    avio_flush(output_fmt_ctx_->pb);
    if (!output_buffer_.empty()) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (output_init_cb_) output_init_cb_(output_buffer_.data(), output_buffer_.size());
        output_buffer_.clear();
    }
    output_initialized_ = true;
    return true;
}

void TranscodeClient::WriteOutputPacketFunc(AVPacket* packet)
{
    packet->stream_index = output_stream_index_;
    av_packet_rescale_ts(packet, encoder_ctx_->time_base, output_fmt_ctx_->streams[output_stream_index_]->time_base);
    av_interleaved_write_frame(output_fmt_ctx_, packet);
}

void TranscodeClient::FlushOutput(bool keyframe_flag)
{
    if (output_fmt_ctx_ && output_fmt_ctx_->pb) avio_flush(output_fmt_ctx_->pb);

    if (!output_buffer_.empty()) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (output_fragment_cb_) {
            MP4Chunk chunk;
            chunk.whole_chunk.data = output_buffer_; // Copy
            chunk.whole_chunk.size = output_buffer_.size();
            chunk.has_keyframe = keyframe_flag;
            output_fragment_cb_(chunk);
        }
        output_buffer_.clear();
    }
}

} // namespace transcode
} // namespace quicr