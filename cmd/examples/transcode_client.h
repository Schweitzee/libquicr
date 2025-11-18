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
 * @brief CMAF Transcode Client
 *
 * This class handles transcoding of fragmented CMAF (fragmented MP4) streams.
 * It accepts CMAF initialization segments and media fragments as input, and
 * produces rescaled CMAF output via callbacks.
 *
 * The client preserves timestamp information and maintains valid CMAF structure
 * in the output stream.
 *
 * Usage:
 * 1. Create a TranscodeClient with desired configuration
 * 2. Set output callbacks for init segment and fragments
 * 3. Push input init segment when received
 * 4. Push input fragments as they arrive
 * 5. Call Flush() when done
 *
 * Example:
 * @code
 *   TranscodeConfig config;
 *   config.target_width = 1280;
 *   config.target_height = 720;
 *
 *   auto client = TranscodeClient::Create(config);
 *   client->SetOutputInitCallback([](const uint8_t* data, size_t size) {
 *       // Handle output init segment
 *   });
 *   client->SetOutputFragmentCallback([](const uint8_t* data, size_t size) {
 *       // Handle output fragment
 *   });
 *
 *   client->PushInputInit(init_data, init_size);
 *   client->PushInputFragment(fragment_data, fragment_size);
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
     * @brief Push input CMAF media fragment (moof + mdat)
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
