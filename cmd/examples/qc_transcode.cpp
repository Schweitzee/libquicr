//
// Created by schweitzer on 2025. 11. 06..
//
// Extended MoQ-Based Transcoder Client Logic
// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <nlohmann/json.hpp>
#include <oss/cxxopts.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <quicr/client.h>
#include <quicr/object.h>

#include "helper_functions.h"
#include "signal_handler.h"
#include <quicr/cache.h>
#include <quicr/defer.h>

#include <filesystem>
#include <format>
#include <fstream>

#include <quicr/publish_fetch_handler.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <termios.h>

#include "CatalogSubscribeTrackHandler.h"
#include "TranscodeRequestSubscribeHandler.h"
#include "TranscodeSubscribeTrackHandler.h"
#include "base64_tool.h"
#include "catalog.hpp"
#include "media.h"
#include "subscriber_util.h"
#include "transcode_client.h"
#include "transcode_request.h"

#include <set>
#include <iomanip>

#include <optional>

std::shared_ptr<spdlog::logger> logger;

using json = nlohmann::json; // NOLINT

/**
 * @brief Defines an object received from an announcer that lives in the cache.
 */
struct CacheObject
{
    quicr::ObjectHeaders headers;
    quicr::Bytes data;
};

/**
 * @brief Specialization of std::less for sorting CacheObjects by object ID.
 */
template<>
struct std::less<CacheObject>
{
    constexpr bool operator()(const CacheObject& lhs, const CacheObject& rhs) const noexcept
    {
        return lhs.headers.object_id < rhs.headers.object_id;
    }
};

namespace qclient_vars {
    std::optional<uint64_t> track_alias;
    std::unordered_map<quicr::messages::TrackAlias, quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>>
      cache;
    std::shared_ptr<quicr::ThreadedTickService> tick_service = std::make_shared<quicr::ThreadedTickService>();
}

/**
 * @brief Thread-safe fragment queue for transcoded data
 */
class FragmentQueue
{
  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> queue_;
    std::atomic<bool> closed_{ false };

  public:
    void Push(const uint8_t* data, size_t size)
    {
        if (closed_.load()) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        queue_.emplace_back(data, data + size);
        cv_.notify_one();
    }

    std::optional<std::vector<uint8_t>> TryPop(std::chrono::milliseconds timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (!cv_.wait_for(lock, timeout_ms, [this] { return !queue_.empty() || closed_.load(); })) {
            return std::nullopt;
        }

        if (queue_.empty()) {
            return std::nullopt;
        }

        std::vector<uint8_t> fragment = std::move(queue_.front());
        queue_.pop_front();
        return fragment;
    }

    void Close()
    {
        closed_.store(true);
        cv_.notify_all();
    }

    bool IsClosed() const { return closed_.load(); }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
};

/**
 * @brief Publish track handler for video tracks
 */
class VideoPublishTrackHandler : public quicr::PublishTrackHandler
{
  public:
    VideoPublishTrackHandler(const quicr::FullTrackName& full_track_name,
                             quicr::TrackMode track_mode,
                             uint8_t default_priority,
                             uint32_t default_ttl)
      : quicr::PublishTrackHandler(full_track_name, track_mode, default_priority, default_ttl)
    {
    }

    void StatusChanged(Status status) override
    {
        const auto alias = GetTrackAlias().value_or(0);
        switch (status) {
            case Status::kOk:
                SPDLOG_INFO("Publish track alias: {0} is ready to send", alias);
                break;
            case Status::kNoSubscribers:
                SPDLOG_INFO("Publish track alias: {0} has no subscribers", alias);
                break;
            case Status::kNewGroupRequested:
                SPDLOG_INFO("Publish track alias: {0} has new group request", alias);
                break;
            default:
                SPDLOG_INFO("Publish track alias: {0} has status {1}", alias, static_cast<int>(status));
                break;
        }
    }
};

/**
 * @brief MoQ client
 */
class MyClient : public quicr::Client
{
    MyClient(const quicr::ClientConfig& cfg, bool& stop_threads)
      : quicr::Client(cfg)
      , stop_threads_(stop_threads)
    {
    }

  public:
    static std::shared_ptr<MyClient> Create(const quicr::ClientConfig& cfg, bool& stop_threads)
    {
        return std::shared_ptr<MyClient>(new MyClient(cfg, stop_threads));
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kReady:
                SPDLOG_INFO("Connection ready");
                break;
            case Status::kConnecting:
                break;
            case Status::kPendingServerSetup:
                SPDLOG_INFO("Connection connected and now pending server setup");
                break;
            default:
                SPDLOG_INFO("Connection failed {0}", static_cast<int>(status));
                stop_threads_ = true;
                moq_example::terminate = true;
                moq_example::termination_reason = "Connection failed";
                moq_example::cv.notify_all();
                break;
        }
    }

    void PublishNamespaceReceived(const quicr::TrackNamespace& track_namespace,
                                  const quicr::PublishNamespaceAttributes&) override
    {
        auto th = quicr::TrackHash({ track_namespace, {} });
        SPDLOG_INFO("Received announce for namespace_hash: {}", th.track_namespace_hash);
    }

    void PublishNamespaceDoneReceived(const quicr::TrackNamespace& track_namespace) override
    {
        auto th = quicr::TrackHash({ track_namespace, {} });
        SPDLOG_INFO("Received unannounce for namespace_hash: {}", th.track_namespace_hash);
    }

  private:
    bool& stop_threads_;
};

/*===========================================================================*/
// Transcoding thread function
/*===========================================================================*/

/**
 * @brief Function to handle transcoding for a single request
 * @param request The transcode request
 * @param catalog The latest catalog snapshot
 * @param delta_track_handler Handler for publishing delta updates
 * @param client The MoQ client
 * @param base_namespace Base namespace for the transcoder
 */
void HandleTranscodeRequest(const TranscodeRequest& request,
                            const Catalog& catalog,
                            std::shared_ptr<quicr::PublishTrackHandler> delta_track_handler,
                            std::shared_ptr<quicr::Client> client,
                            const std::string& base_namespace)
{
    SPDLOG_INFO("Starting transcoding thread for request_id: {}", request.request_id);

    try {
        // 1. Validate request has video operations
        if (request.operations.empty()) {
            SPDLOG_ERROR("Request {} has no operations", request.request_id);
            return;
        }

        // For now, only handle video rescaling (first video operation)
        auto media_type = infer_media_type(request.operations[0].kind);
        if (media_type != InferredMediaType::Video) {
            SPDLOG_ERROR("Request {} contains non-video operations (not supported yet)", request.request_id);
            return;
        }

        // 2. Find source track in catalog
        std::string source_ns = request.source.ns.value_or(base_namespace);
        std::string source_track = request.source.track;

        SPDLOG_INFO("Looking for source track: namespace={}, name={}", source_ns, source_track);

        // Find the track entry in catalog
        auto& tracks = const_cast<Catalog&>(catalog).tracks();
        auto track_it = std::find_if(tracks.begin(), tracks.end(), [&](const CatalogTrackEntry& entry) {
            std::string entry_ns = entry.effective_src_namespace(catalog.namespace_);
            return entry_ns == source_ns && entry.name == source_track;
        });

        if (track_it == tracks.end()) {
            SPDLOG_ERROR("Source track {} not found in catalog", source_track);
            return;
        }

        SPDLOG_INFO("Found source track: {}, type: {}", track_it->name, track_it->type);

        // 3. Build transcode configuration from operations
        transcode::TranscodeConfig transcode_config;

        for (const auto& op : request.operations) {
            if (op.kind == OperationKind::VideoChangeResolution) {
                const auto& res_op = std::get<OpVideoChangeResolution>(op.data);
                transcode_config.target_width = res_op.width;
                transcode_config.target_height = res_op.height;
                SPDLOG_INFO("Transcode config: target resolution {}x{}", res_op.width, res_op.height);
            } else if (op.kind == OperationKind::VideoChangeFramerate) {
                const auto& fps_op = std::get<OpVideoChangeFramerate>(op.data);
                transcode_config.target_fps = fps_op.target_fps;
                SPDLOG_INFO("Transcode config: target FPS {}", fps_op.target_fps);
            }
        }

        // 4. Decode init segment from catalog
        std::vector<uint8_t> init_data = base64::decode_to_uint8_vec(track_it->b64_init_data);
        SPDLOG_INFO("Decoded init segment: {} bytes", init_data.size());

        // 5. Create transcoder instance
        auto transcode_client = transcode::TranscodeClient::Create(transcode_config);

        // 6. Create fragment queue
        auto fragment_queue = std::make_shared<FragmentQueue>();

        // 7. Set up init callback - publishes delta update
        transcode_client->SetOutputInitCallback([&, delta_track_handler, fragment_queue, request](
                                                  const uint8_t* data, size_t size) {
            SPDLOG_INFO("Transcoder init callback: {} bytes", size);

            // TODO: Build and publish delta update to catalog
            // For now, just log it
            std::string init_b64 = base64::encode_uint8_vec(std::vector<uint8_t>(data, data + size));
            SPDLOG_INFO("Transcoded init segment (base64 length): {}", init_b64.size());

            // In a full implementation, we'd:
            // 1. Create a CatalogTrackEntry for the transcoded stream
            // 2. Build a JSON Patch delta update
            // 3. Publish it on the delta_track_handler
        });

        // 8. Set up fragment callback - pushes to queue
        transcode_client->SetOutputFragmentCallback([fragment_queue](const uint8_t* data, size_t size) {
            SPDLOG_DEBUG("Transcoder fragment callback: {} bytes", size);
            fragment_queue->Push(data, size);
        });

        // 9. Push init segment to transcoder
        transcode_client->PushInputInit(init_data.data(), init_data.size());

        // 10. Subscribe to source track
        auto subtrack = std::make_shared<SubTrack>();
        subtrack->track_entry = *track_it;
        subtrack->namespace_ = source_ns;
        subtrack->init = init_data;

        auto source_full_track_name = quicr::example::MakeFullTrackName(source_ns, source_track);
        auto source_track_handler = std::make_shared<TranscodeSubscribeTrackHandler>(
          source_full_track_name,
          quicr::messages::FilterType::kLargestObject,
          std::nullopt,
          subtrack,
          transcode_client);

        client->SubscribeTrack(source_track_handler);

        SPDLOG_INFO("Subscribed to source track, waiting for fragments...");

        // 11. Create and publish transcoded track
        std::string output_ns = request.output.has_value() && request.output->ns.has_value()
                                  ? request.output->ns.value()
                                  : base_namespace + ".transcoded";

        std::string output_track_name =
          request.output.has_value() && request.output->track_name_hint.has_value()
            ? request.output->track_name_hint.value()
            : source_track + "_" + std::to_string(transcode_config.target_width) + "x" +
                std::to_string(transcode_config.target_height);

        auto output_full_track_name = quicr::example::MakeFullTrackName(output_ns, output_track_name);
        auto output_track_handler =
          std::make_shared<VideoPublishTrackHandler>(output_full_track_name, quicr::TrackMode::kStream, 2, 3000);

        client->PublishTrack(output_track_handler);

        SPDLOG_INFO("Publishing transcoded track: {}/{}", output_ns, output_track_name);

        // 12. Fragment publishing loop
        uint64_t group_id = 0;
        uint64_t object_id = 0;
        bool running = true;

        while (running && !moq_example::terminate) {
            // Try to get a fragment from the queue
            auto fragment_opt = fragment_queue->TryPop(std::chrono::milliseconds(100));

            if (fragment_opt.has_value()) {
                auto& fragment = fragment_opt.value();

                // Publish fragment
                quicr::ObjectHeaders obj_headers = { group_id,
                                                     object_id++,
                                                     0,
                                                     fragment.size(),
                                                     quicr::ObjectStatus::kAvailable,
                                                     2,
                                                     3000,
                                                     std::nullopt,
                                                     std::nullopt,
                                                     std::nullopt };

                if (output_track_handler->CanPublish()) {
                    auto status = output_track_handler->PublishObject(obj_headers, fragment);
                    if (status == quicr::PublishTrackHandler::PublishObjectStatus::kOk) {
                        SPDLOG_DEBUG("Published transcoded fragment: group={}, object={}, size={}",
                                     group_id,
                                     object_id - 1,
                                     fragment.size());
                    }
                }

                // Start new group every 60 objects (roughly every 2 seconds at 30fps)
                if (object_id >= 60) {
                    group_id++;
                    object_id = 0;
                }
            }

            // Check if we should stop
            if (fragment_queue->IsClosed() && fragment_queue->Empty()) {
                SPDLOG_INFO("Fragment queue closed and empty, finishing transcoding");
                running = false;
            }
        }

        // Cleanup
        transcode_client->Flush();
        transcode_client->Close();
        client->UnsubscribeTrack(source_track_handler);
        client->UnpublishTrack(output_track_handler);

        SPDLOG_INFO("Transcoding thread finished for request_id: {}", request.request_id);

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Transcoding thread error for request_id {}: {}", request.request_id, e.what());
    }
}

/*===========================================================================*/
// Subscriber thread to perform subscribe and handle transcode requests
/*===========================================================================*/

void DoSubscriber(const std::string& track_namespace,
                 const std::shared_ptr<quicr::Client>& client,
                 const bool& stop)
{
    auto sub_util = std::make_shared<SubscriberUtil>();

    // 1) Subscribe to catalog track
    auto catalog_full_track_name = quicr::example::MakeFullTrackName(track_namespace + ",catalog", "publisher");
    const auto catalog_track_handler =
      std::make_shared<CatalogSubscribeTrackHandler>(catalog_full_track_name,
                                                     quicr::messages::FilterType::kLargestObject,
                                                     std::nullopt,
                                                     sub_util);

    SPDLOG_INFO("Starting transcoder subscriber");

    if (client->GetStatus() == MyClient::Status::kReady) {
        SPDLOG_INFO("Subscribing to catalog track");
        client->SubscribeTrack(catalog_track_handler);
    } else {
        SPDLOG_ERROR("Client not ready for subscribing to catalog track");
        return;
    }

    // Wait for catalog to be ready
    SPDLOG_INFO("Waiting for initial catalog");
    if (!catalog_track_handler->WaitForCatalog(std::chrono::seconds(30))) {
        SPDLOG_ERROR("Timeout waiting for catalog");
        return;
    }

    SPDLOG_INFO("Catalog received with {} tracks", sub_util->catalog.tracks().size());

    // 2) Subscribe to transcode request track
    auto request_queue = std::make_shared<TranscodeRequestQueue>();
    auto request_track_name =
      quicr::example::MakeFullTrackName(track_namespace + ".transcode.requests", "requests");
    auto request_track_handler = std::make_shared<TranscodeRequestSubscribeHandler>(
      request_track_name, request_queue, quicr::messages::FilterType::kLargestObject);

    if (client->GetStatus() == MyClient::Status::kReady) {
        SPDLOG_INFO("Subscribing to transcode request track");
        client->SubscribeTrack(request_track_handler);
    }

    // 3) Publish delta update track
    auto delta_track_name = quicr::example::MakeFullTrackName(track_namespace + ",catalog", "delta_updates");
    auto delta_track_handler =
      std::make_shared<VideoPublishTrackHandler>(delta_track_name, quicr::TrackMode::kStream, 2, 5000);

    if (client->GetStatus() == MyClient::Status::kReady) {
        SPDLOG_INFO("Publishing delta update track");
        client->PublishTrack(delta_track_handler);
    }

    SPDLOG_INFO("Transcoder ready, waiting for transcode requests...");

    // 4) Main request processing loop
    std::vector<std::thread> transcode_threads;

    while (!stop && !moq_example::terminate) {
        // Try to get a request with timeout
        auto request_opt = request_queue->TryPop(std::chrono::milliseconds(500));

        if (request_opt.has_value()) {
            auto request = std::move(request_opt.value());

            SPDLOG_INFO("Received transcode request: request_id={}, client_id={}",
                        request.request_id,
                        request.client_id);

            // Get latest catalog snapshot
            Catalog catalog_snapshot = catalog_track_handler->GetCatalogCopy();

            // Spawn a new thread to handle this request
            transcode_threads.emplace_back(
              HandleTranscodeRequest, request, catalog_snapshot, delta_track_handler, client, track_namespace);
        }
    }

    // Cleanup: wait for all transcoding threads to finish
    SPDLOG_INFO("Shutting down, waiting for {} transcoding threads...", transcode_threads.size());
    for (auto& t : transcode_threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    client->UnsubscribeTrack(request_track_handler);
    client->UnsubscribeTrack(catalog_track_handler);
    client->UnpublishTrack(delta_track_handler);

    SPDLOG_INFO("Transcoder subscriber done");
    moq_example::terminate = true;
}

/*===========================================================================*/
// Main program
/*===========================================================================*/

quicr::ClientConfig
InitConfig(cxxopts::ParseResult& cli_opts, bool& enable_sub)
{
    quicr::ClientConfig config;

    std::string qlog_path;
    if (cli_opts.count("qlog")) {
        qlog_path = cli_opts["qlog"].as<std::string>();
    }

    if (cli_opts.count("debug") && cli_opts["debug"].as<bool>() == true) {
        SPDLOG_INFO("setting debug level");
        spdlog::set_level(spdlog::level::debug);
    }

    if (cli_opts.count("trace") && cli_opts["trace"].as<bool>() == true) {
        SPDLOG_INFO("setting trace level");
        spdlog::set_level(spdlog::level::trace);
    }

    if (cli_opts.count("version") && cli_opts["version"].as<bool>() == true) {
        SPDLOG_INFO("QuicR library version: {}", QUICR_VERSION);
        exit(0);
    }

    if (cli_opts.count("sub_namespace")) {
        enable_sub = true;
        SPDLOG_INFO("Transcoder enabled using track namespace: {0}", cli_opts["sub_namespace"].as<std::string>());
    }

    if (cli_opts.count("ssl_keylog") && cli_opts["ssl_keylog"].as<bool>() == true) {
        SPDLOG_INFO("SSL Keylog enabled");
    }

    config.endpoint_id = cli_opts["endpoint_id"].as<std::string>();
    config.connect_uri = cli_opts["url"].as<std::string>();
    config.transport_config.debug = cli_opts["debug"].as<bool>();
    config.transport_config.ssl_keylog = cli_opts["ssl_keylog"].as<bool>();

    config.transport_config.use_reset_wait_strategy = false;
    config.transport_config.time_queue_max_duration = 5000;
    config.transport_config.tls_cert_filename = "";
    config.transport_config.tls_key_filename = "";
    config.transport_config.quic_qlog_path = qlog_path;

    return config;
}

int
main(int argc, char* argv[])
{
    // Initialize logger
    logger = spdlog::stderr_color_mt("err_logger");
    spdlog::set_default_logger(logger);

    int result_code = EXIT_SUCCESS;

    cxxopts::Options options("qc_transcode",
                             std::string("MOQ Transcoder using QuicR Version: ") + std::string(QUICR_VERSION));

    // clang-format off
    options.set_width(75)
      .set_tab_expansion()
      .add_options()
        ("h,help", "Print help")
        ("d,debug", "Enable debugging")
        ("t,trace", "Enable tracing")
        ("v,version", "QuicR Version")
        ("r,url", "Relay URL", cxxopts::value<std::string>()->default_value("moq://localhost:1234"))
        ("e,endpoint_id", "This client endpoint ID", cxxopts::value<std::string>()->default_value("moq-transcoder"))
        ("q,qlog", "Enable qlog using path", cxxopts::value<std::string>())
        ("s,ssl_keylog", "Enable SSL Keylog for transport debugging");

    options.add_options("Transcoder")
        ("sub_namespace", "Track namespace to transcode", cxxopts::value<std::string>());

    // clang-format on

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help({ "", "Transcoder" }) << std::endl;
        return EXIT_SUCCESS;
    }

    // Install signal handlers
    installSignalHandlers();

    // Lock the mutex so that main can then wait on it
    std::unique_lock lock(moq_example::main_mutex);

    bool enable_sub{ false };
    quicr::ClientConfig config = InitConfig(result, enable_sub);

    try {
        bool stop_threads{ false };
        auto client = MyClient::Create(config, stop_threads);

        if (client->Connect() != quicr::Transport::Status::kConnecting) {
            SPDLOG_ERROR("Failed to connect to server due to invalid params, check URI");
            exit(-1);
        }

        while (not stop_threads) {
            if (client->GetStatus() == MyClient::Status::kReady) {
                SPDLOG_INFO("Connected to server");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::thread sub_thread;

        if (enable_sub) {
            // Subscribe to transcode namespace announces
            std::string track_ns = result["sub_namespace"].as<std::string>();
            std::string prefix_ns_str = track_ns + ".transcode.requests";
            const auto& prefix_ns = quicr::example::MakeFullTrackName(prefix_ns_str, "");

            auto th = quicr::TrackHash(prefix_ns);

            SPDLOG_INFO("Sending subscribe announces for prefix '{}' namespace_hash: {}",
                        prefix_ns_str,
                        th.track_namespace_hash);

            client->SubscribeNamespace(prefix_ns.name_space);

            // Start transcoder subscriber thread
            sub_thread = std::thread(DoSubscriber, track_ns, client, std::ref(stop_threads));
        }

        // Wait until told to terminate
        moq_example::cv.wait(lock, [&]() { return moq_example::terminate; });

        stop_threads = true;
        SPDLOG_INFO("Stopping threads...");

        if (sub_thread.joinable()) {
            sub_thread.join();
        }

        client->Disconnect();

        SPDLOG_INFO("Transcoder client done");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    } catch (const std::invalid_argument& e) {
        std::cerr << "Invalid argument: " << e.what() << std::endl;
        result_code = EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "Unexpected exception: " << e.what() << std::endl;
        result_code = EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Unexpected exception" << std::endl;
        result_code = EXIT_FAILURE;
    }

    SPDLOG_INFO("Exit");

    return result_code;
}
