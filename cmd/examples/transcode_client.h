// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace quicr {
namespace transcode {

/**
 * @brief Configuration for the transcode client
 */
struct TranscodeConfig
{
    /// Target output width in pixels
    uint32_t target_width{ 0 };

    /// Target output height in pixels
    uint32_t target_height{ 0 };

    /// Target output bitrate in bits per second (0 = auto)
    uint32_t target_bitrate{ 0 };

    /// Target framerate (0 = same as input)
    uint32_t target_fps{ 0 };

    /// Codec to use for output (e.g., "h264", "hevc"). Empty = same as input
    std::string output_codec;

    /// Quality preset ("ultrafast", "fast", "medium", "slow", "veryslow")
    std::string encoder_preset{ "medium" };

    /// Enable debug logging
    bool debug{ false };
};

/**
 * @brief CMAF Transcode Client (Continuous Streaming)
 *
 * This class handles transcoding of fragmented CMAF streams using a continuous
 * processing pipeline. Data is buffered and processed through persistent
 * decoder/encoder contexts for efficiency.
 *
 * Architecture:
 * - PushInputInit/Fragment() buffer data into input stream
 * - Single persistent AVFormatContext reads from buffer via custom AVIO
 * - Single decoder/encoder contexts maintained throughout stream
 * - Processed frames output via callbacks
 *
 * Usage:
 * @code
 *   auto client = TranscodeClient::Create(config);
 *   client->SetOutputInitCallback([](const uint8_t* data, size_t size) { ... });
 *   client->SetOutputFragmentCallback([](const uint8_t* data, size_t size) { ... });
 *   client->PushInputInit(init_data, init_size);
 *   client->PushInputFragment(fragment_data, fragment_size); // Processes continuously
 *   client->Flush();
 * @endcode
 */
class TranscodeClient
{
  public:
    /// Callback type for output init segment
    using OutputInitCallback = std::function<void(const uint8_t* data, size_t size)>;

    /// Callback type for output media fragments
    using OutputFragmentCallback = std::function<void(const uint8_t* data, size_t size)>;

    /**
     * @brief Create a transcode client
     * @param config Configuration for transcoding
     * @return Shared pointer to TranscodeClient instance
     */
    static std::shared_ptr<TranscodeClient> Create(const TranscodeConfig& config);

    /**
     * @brief Destructor
     */
    virtual ~TranscodeClient();

    /**
     * @brief Set callback for output initialization segment
     * @param callback Function to call when output init segment is ready
     */
    void SetOutputInitCallback(OutputInitCallback callback);

    /**
     * @brief Set callback for output media fragments
     * @param callback Function to call when output fragment is ready
     */
    void SetOutputFragmentCallback(OutputFragmentCallback callback);

    /**
     * @brief Push input CMAF initialization segment
     * @param data Pointer to init segment data
     * @param size Size of init segment in bytes
     * @return true on success, false on error
     */
    bool PushInputInit(const uint8_t* data, size_t size);

    /**
     * @brief Push input CMAF media fragment
     *
     * Appends fragment to input buffer and processes available data through
     * the continuous decode/encode pipeline.
     *
     * @param data Pointer to fragment data
     * @param size Size of fragment in bytes
     * @return true on success, false on error
     */
    bool PushInputFragment(const uint8_t* data, size_t size);

    /**
     * @brief Flush any remaining data and finalize output
     * @return true on success, false on error
     */
    bool Flush();

    /**
     * @brief Close the transcode client and release resources
     */
    void Close();

    /**
     * @brief Check if the client is ready to accept input
     * @return true if ready, false otherwise
     */
    bool IsReady() const;

    /**
     * @brief Get the last error message
     * @return Error message string, empty if no error
     */
    std::string GetLastError() const;

  protected:
    TranscodeClient(const TranscodeConfig& config);

  private:
    // Forward declaration of implementation details
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace transcode
} // namespace quicr
