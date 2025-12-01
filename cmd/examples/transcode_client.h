// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "media.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVRational;

namespace quicr {
namespace transcode {

/**
 * @brief Configuration for the transcode client
 */
struct TranscodeConfig
{
    uint32_t target_width{ 0 };
    uint32_t target_height{ 0 };
    uint32_t target_bitrate{ 0 };
    uint32_t target_fps{ 0 };
    bool debug{ true };
};

/**
 * @brief Thread-safe Ring Buffer helper class
 * Defined here to be part of the TranscodeClient memory layout without complex pointers.
 */
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);
    void Write(const uint8_t* data, size_t size);
    int Read(uint8_t* dest, int size);
    size_t Size() const;
    bool IsEmptyUnsafe() const;

private:
    void Expand(size_t needed);
    mutable std::mutex mutex_;
    std::vector<uint8_t> buffer_;
    size_t read_pos_{0};
    size_t write_pos_{0};
    size_t stored_bytes_{0};
};

/**
 * @brief CMAF Transcode Client (Simplified Implementation)
 */
class TranscodeClient
{
  public:
    using OutputInitCallback = std::function<void(const uint8_t* data, size_t size)>;
    using OutputFragmentCallback = std::function<void(MP4Chunk)>;

    explicit TranscodeClient(const TranscodeConfig& config);

    virtual ~TranscodeClient();

    void SetOutputInitCallback(OutputInitCallback callback);
    void SetOutputFragmentCallback(OutputFragmentCallback callback);

    bool PushInputInit(const uint8_t* data, size_t size);
    bool PushInputFragment(const uint8_t* data, size_t size);
    bool Flush();
    void Close();
    bool IsReady() const;
    std::string GetLastError() const;


  private:
    // Internal Helper Methods
    void TranscodeLoop();
    void ProcessPendingData();
    bool InitializeDemuxer();
    void ProcessPacket(AVPacket* packet);
    void ProcessFrame(AVFrame* frame);
    bool InitializeOutput();
    void DrainEncoder();
    void WriteOutputPacketFunc(AVPacket* packet);
    void FlushOutput(bool keyframe_flag);
    void Cleanup();
    void StopAndJoin();

    // Static callback for FFmpeg custom IO
    static int ReadBufferCallback(void* opaque, uint8_t* buf, int buf_size);

    // --- Member Variables ---

    TranscodeConfig config_;
    std::atomic<bool> has_init_segment_{ false };
    std::string last_error_;

    // Threading and Synchronization
    std::thread worker_thread_;
    std::atomic<bool> running_{ false };
    std::mutex cv_mutex_;
    std::condition_variable data_cv_;
    std::mutex callback_mutex_;

    // Input Buffer
    RingBuffer input_ring_buffer_;

    // FFmpeg Contexts
    AVFormatContext* input_fmt_ctx_{ nullptr };
    uint8_t* avio_ctx_buffer_{ nullptr };
    int video_stream_index_{ -1 };

    AVCodecContext* decoder_ctx_{ nullptr };
    int input_time_base_num_{1};
    int input_time_base_den_{1};

    SwsContext* sws_ctx_{ nullptr };
    AVFrame* scaled_frame_{ nullptr };

    AVCodecContext* encoder_ctx_{ nullptr };
    AVFormatContext* output_fmt_ctx_{ nullptr };
    int output_stream_index_{ 0 };

    std::vector<uint8_t> output_buffer_;
    bool output_initialized_{ false };

    int64_t next_pts_{ 0 };
    int64_t last_encoded_pts_{ -1 };

    // Callbacks
    OutputInitCallback output_init_cb_;
    OutputFragmentCallback output_fragment_cb_;
};

} // namespace transcode
} // namespace quicr