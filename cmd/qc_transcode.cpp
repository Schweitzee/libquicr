// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "transcode_client.h"

#include <quicr/client.h>
#include <quicr/object.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

// Signal handler for graceful shutdown
static std::atomic<bool> g_running{ true };

void
SignalHandler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
        g_running = false;
    }
}

/**
 * @brief Track handler for subscribing to video tracks and transcoding
 *
 * This handler receives fragmented MP4/CMAF video from the MoQ layer,
 * transcodes it using the TranscodeClient, and republishes the transcoded output.
 */
class TranscodingSubscribeTrackHandler : public quicr::SubscribeTrackHandler
{
  public:
    TranscodingSubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                                     std::shared_ptr<transcode::TranscodeClient> transcode_client,
                                     std::shared_ptr<quicr::PublishTrackHandler> output_publisher = nullptr)
      : SubscribeTrackHandler(full_track_name,
                              3, // priority
                              quicr::messages::GroupOrder::kAscending,
                              quicr::messages::FilterType::kLatestGroup)
      , transcode_client_(std::move(transcode_client))
      , output_publisher_(std::move(output_publisher))
    {
        SPDLOG_INFO("Created transcoding subscriber for track");
    }

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        if (!transcode_client_) {
            SPDLOG_WARN("No transcode client available");
            return;
        }

        SPDLOG_DEBUG("Received object: group_id={}, object_id={}, size={}", hdr.group_id, hdr.object_id, data.size());

        // Determine if this is an init segment or a media fragment
        // In CMAF/fragmented MP4:
        // - Init segment contains: ftyp + moov boxes (typically object_id == 0 or has specific flag)
        // - Media fragments contain: moof + mdat boxes (subsequent objects)
        //
        // For this example, we assume:
        // - object_id == 0 in a group is the init segment
        // - object_id > 0 are media fragments

        if (hdr.object_id == 0) {
            // This is an init segment
            SPDLOG_INFO("Received init segment for group {}, size={}", hdr.group_id, data.size());
            transcode_client_->PushInputInit(data.data(), data.size());

            // Start transcoding on first init segment
            if (!transcode_started_) {
                if (transcode_client_->Start()) {
                    transcode_started_ = true;
                    SPDLOG_INFO("Transcode pipeline started");
                } else {
                    SPDLOG_ERROR("Failed to start transcode pipeline");
                }
            }
        } else {
            // This is a media fragment
            SPDLOG_DEBUG("Received fragment: group={}, object={}, size={}", hdr.group_id, hdr.object_id, data.size());
            transcode_client_->PushInputFragment(data.data(), data.size());
        }

        stats_.objects_received++;
        stats_.bytes_received += data.size();
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kOk: {
                if (auto track_alias = GetTrackAlias(); track_alias.has_value()) {
                    SPDLOG_INFO("Track alias {} is ready to receive", track_alias.value());
                }
            } break;

            case Status::kNotConnected:
                SPDLOG_WARN("Track not connected");
                break;

            case Status::kPendingSubscribeResponse:
                SPDLOG_INFO("Waiting for subscribe response");
                break;

            default:
                SPDLOG_WARN("Track status changed: {}", static_cast<int>(status));
                break;
        }
    }

    void PrintStats() const
    {
        SPDLOG_INFO("Input stats: objects={}, bytes={}", stats_.objects_received, stats_.bytes_received);
    }

  private:
    std::shared_ptr<transcode::TranscodeClient> transcode_client_;
    std::shared_ptr<quicr::PublishTrackHandler> output_publisher_;
    bool transcode_started_ = false;

    struct Stats
    {
        uint64_t objects_received = 0;
        uint64_t bytes_received = 0;
    } stats_;
};

/**
 * @brief Track handler for publishing transcoded video
 *
 * This handler publishes the transcoded video output from the TranscodeClient.
 */
class TranscodedPublishTrackHandler : public quicr::PublishTrackHandler
{
  public:
    TranscodedPublishTrackHandler(const quicr::FullTrackName& full_track_name)
      : PublishTrackHandler(full_track_name, 3) // priority = 3
    {
        SPDLOG_INFO("Created transcoded video publisher");
    }

    void PublishInit(const uint8_t* data, size_t size)
    {
        SPDLOG_INFO("Publishing transcoded init segment, size={}", size);

        quicr::ObjectHeaders hdr;
        hdr.group_id = current_group_id_;
        hdr.object_id = 0; // Init segment is always object 0
        hdr.payload_length = size;
        hdr.priority = { 3 };

        std::vector<uint8_t> data_vec(data, data + size);
        PublishObject(hdr, std::move(data_vec), {});

        stats_.objects_published++;
        stats_.bytes_published += size;
    }

    void PublishFragment(const uint8_t* data, size_t size)
    {
        SPDLOG_DEBUG("Publishing transcoded fragment, size={}", size);

        quicr::ObjectHeaders hdr;
        hdr.group_id = current_group_id_;
        hdr.object_id = ++current_object_id_;
        hdr.payload_length = size;
        hdr.priority = { 3 };

        std::vector<uint8_t> data_vec(data, data + size);
        PublishObject(hdr, std::move(data_vec), {});

        stats_.objects_published++;
        stats_.bytes_published += size;

        // Start new group every 100 objects (adjust as needed)
        if (current_object_id_ >= 100) {
            current_group_id_++;
            current_object_id_ = 0;
        }
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kOk:
                SPDLOG_INFO("Publisher ready");
                break;

            case Status::kNotConnected:
                SPDLOG_WARN("Publisher not connected");
                break;

            default:
                SPDLOG_WARN("Publisher status changed: {}", static_cast<int>(status));
                break;
        }
    }

    void PrintStats() const
    {
        SPDLOG_INFO("Output stats: objects={}, bytes={}", stats_.objects_published, stats_.bytes_published);
    }

  private:
    uint64_t current_group_id_ = 0;
    uint64_t current_object_id_ = 0;

    struct Stats
    {
        uint64_t objects_published = 0;
        uint64_t bytes_published = 0;
    } stats_;
};

/**
 * @brief DoSubscriber - Main function demonstrating transcoding integration
 *
 * This function shows how to:
 * 1. Create a TranscodeClient with target resolution
 * 2. Set up output callbacks to forward transcoded data
 * 3. Subscribe to an input video track
 * 4. Forward received CMAF data to the transcode client
 * 5. Publish the transcoded output
 */
void
DoSubscriber(quicr::Client& client,
             const quicr::FullTrackName& input_track,
             const quicr::FullTrackName& output_track,
             uint32_t target_width,
             uint32_t target_height)
{
    SPDLOG_INFO("Starting transcoding subscriber");
    SPDLOG_INFO("Input track: (namespace and name omitted for brevity)");
    SPDLOG_INFO("Output track: (namespace and name omitted for brevity)");
    SPDLOG_INFO("Target resolution: {}x{}", target_width, target_height);

    // Step 1: Create transcode client with configuration
    transcode::TranscodeConfig config;
    config.target_width = target_width;
    config.target_height = target_height;
    config.target_bitrate = 500000; // 500 kbps
    config.codec = "x264";
    config.low_latency = true;
    config.gop_size = 60;

    auto transcode_client = std::make_shared<transcode::TranscodeClient>(config);
    SPDLOG_INFO("TranscodeClient created");

    // Step 2: Create output publisher for transcoded video
    auto output_publisher = std::make_shared<TranscodedPublishTrackHandler>(output_track);

    // Step 3: Set up transcode client output callbacks
    transcode_client->SetOutputInitCallback([output_publisher](const uint8_t* data, size_t size) {
        SPDLOG_INFO("Transcode output: init segment ready, size={}", size);
        output_publisher->PublishInit(data, size);
    });

    transcode_client->SetOutputFragmentCallback([output_publisher](const uint8_t* data, size_t size) {
        SPDLOG_DEBUG("Transcode output: fragment ready, size={}", size);
        output_publisher->PublishFragment(data, size);
    });

    // Step 4: Create subscriber that feeds data into transcode client
    auto subscriber = std::make_shared<TranscodingSubscribeTrackHandler>(input_track, transcode_client, output_publisher);

    // Step 5: Announce the output track
    auto announce_result = client.Announce(output_track.name_space);
    if (announce_result != quicr::Client::AnnounceResult::kOk) {
        SPDLOG_ERROR("Failed to announce output namespace");
        return;
    }
    SPDLOG_INFO("Announced output namespace");

    // Step 6: Connect publisher
    client.Connect(output_publisher);
    SPDLOG_INFO("Connected output publisher");

    // Step 7: Subscribe to input track
    client.Connect(subscriber);
    SPDLOG_INFO("Connected input subscriber");

    // Step 8: Main loop - wait for signal
    SPDLOG_INFO("Transcoding pipeline active. Press Ctrl+C to stop.");

    uint64_t stats_counter = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Print stats every 10 seconds
        if (++stats_counter % 10 == 0) {
            subscriber->PrintStats();
            output_publisher->PrintStats();
        }
    }

    // Step 9: Cleanup
    SPDLOG_INFO("Shutting down transcoding pipeline");

    transcode_client->Flush();
    transcode_client->Stop();

    client.Disconnect(subscriber);
    client.Disconnect(output_publisher);

    SPDLOG_INFO("Transcoding complete");
}

/**
 * @brief Main entry point
 */
int
main(int argc, char* argv[])
{
    // Set up signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Initialize logging
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    SPDLOG_INFO("MoQ Transcoding Client");
    SPDLOG_INFO("======================");

    // TODO: Add command line argument parsing for:
    // - Server connection details (relay URL, endpoint)
    // - Input track name
    // - Output track name
    // - Target resolution
    // - Codec settings

    // For now, this is a minimal example showing the API usage
    // In a real application, you would:
    // 1. Parse command line arguments
    // 2. Create quicr::Client with proper configuration
    // 3. Connect to relay
    // 4. Call DoSubscriber with appropriate track names

    SPDLOG_INFO("Example usage (pseudo-code):");
    SPDLOG_INFO("  1. Create quicr::Client");
    SPDLOG_INFO("  2. Connect to relay server");
    SPDLOG_INFO("  3. Define input and output FullTrackNames");
    SPDLOG_INFO("  4. Call DoSubscriber(client, input_track, output_track, 640, 480)");
    SPDLOG_INFO("");
    SPDLOG_INFO("Integration example:");
    SPDLOG_INFO("  quicr::Client client(config);");
    SPDLOG_INFO("  client.Connect(relay_endpoint);");
    SPDLOG_INFO("  ");
    SPDLOG_INFO("  quicr::FullTrackName input_track;");
    SPDLOG_INFO("  input_track.name_space = quicr::Namespace({\"example\", \"video\", \"input\"});");
    SPDLOG_INFO("  input_track.name = {\"track1\"};");
    SPDLOG_INFO("  ");
    SPDLOG_INFO("  quicr::FullTrackName output_track;");
    SPDLOG_INFO("  output_track.name_space = quicr::Namespace({\"example\", \"video\", \"transcoded\"});");
    SPDLOG_INFO("  output_track.name = {\"track1-480p\"};");
    SPDLOG_INFO("  ");
    SPDLOG_INFO("  DoSubscriber(client, input_track, output_track, 640, 480);");

    // Minimal loop to keep program running
    SPDLOG_INFO("");
    SPDLOG_INFO("This is a reference implementation.");
    SPDLOG_INFO("Press Ctrl+C to exit.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    SPDLOG_INFO("Exiting");
    return 0;
}
