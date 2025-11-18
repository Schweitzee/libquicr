// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Forward declare GStreamer types to avoid including GStreamer headers in public API
typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;
typedef struct _GstSample GstSample;
typedef unsigned long long guint64;

namespace transcode {

/**
 * @brief Configuration for the TranscodeClient
 */
struct TranscodeConfig
{
    uint32_t target_width = 640;
    uint32_t target_height = 480;
    uint32_t target_bitrate = 500000;  // bits per second
    std::string codec = "x264";        // "x264", "x265", "vp8", "vp9", etc.
    uint32_t gop_size = 60;            // GOP size for encoder
    bool low_latency = true;           // Enable low-latency encoding
};

/**
 * @brief GStreamer-based video transcoding client for CMAF streams
 *
 * This class implements a transcoding pipeline that:
 * - Accepts fragmented MP4/CMAF input (init segment + media fragments)
 * - Decodes, rescales, and re-encodes video
 * - Outputs fragmented MP4/CMAF format
 * - Preserves timing information through the pipeline
 */
class TranscodeClient
{
  public:
    /**
     * @brief Callback for receiving output init segment (ftyp + moov boxes)
     */
    using OutputInitCallback = std::function<void(const uint8_t* data, size_t size)>;

    /**
     * @brief Callback for receiving output media fragments (moof + mdat boxes)
     */
    using OutputFragmentCallback = std::function<void(const uint8_t* data, size_t size)>;

    /**
     * @brief Constructor
     * @param config Transcoding configuration
     */
    explicit TranscodeClient(const TranscodeConfig& config);

    /**
     * @brief Destructor
     */
    ~TranscodeClient();

    // Disable copy and move
    TranscodeClient(const TranscodeClient&) = delete;
    TranscodeClient& operator=(const TranscodeClient&) = delete;
    TranscodeClient(TranscodeClient&&) = delete;
    TranscodeClient& operator=(TranscodeClient&&) = delete;

    /**
     * @brief Set callback for output init segment
     * @param callback Function to call when init segment is ready
     */
    void SetOutputInitCallback(OutputInitCallback callback);

    /**
     * @brief Set callback for output media fragments
     * @param callback Function to call when a fragment is ready
     */
    void SetOutputFragmentCallback(OutputFragmentCallback callback);

    /**
     * @brief Push input init segment (ftyp + moov)
     * @param data Pointer to init segment data
     * @param size Size of init segment in bytes
     */
    void PushInputInit(const uint8_t* data, size_t size);

    /**
     * @brief Push input media fragment (moof + mdat)
     * @param data Pointer to fragment data
     * @param size Size of fragment in bytes
     */
    void PushInputFragment(const uint8_t* data, size_t size);

    /**
     * @brief Start the transcoding pipeline
     * @return true if successful, false otherwise
     */
    bool Start();

    /**
     * @brief Stop the transcoding pipeline
     */
    void Stop();

    /**
     * @brief Flush any pending data through the pipeline
     */
    void Flush();

    /**
     * @brief Check if the pipeline is running
     * @return true if running, false otherwise
     */
    bool IsRunning() const;

  private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace transcode
