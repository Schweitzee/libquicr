//
// Created by schweitzer on 2025. 11. 06..
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <nlohmann/json.hpp>
#include <oss/cxxopts.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <quicr/client.h>
#include <quicr/object.h>

#include "helper_functions.h"
#include <quicr/defer.h>
#include <quicr/cache.h>
#include "signal_handler.h"

#include <filesystem>
#include <format>
#include <fstream>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <quicr/publish_fetch_handler.h>

#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <termios.h>

#include "VideoSubscribeTrackHandler.h"
#include "base64_tool.h"
#include "ffmpeg_cmaf_splitter.hpp"
#include "ffmpeg_moq_adapter.h"
#include "media.h"
#include "subscriber_util.h"

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
    bool publish_clock{ false };
    std::optional<uint64_t> track_alias; /// Track alias to use for subscribe
    bool record = false;
    bool playback = false;
    std::optional<uint64_t> new_group_request_id;
    bool add_gaps = false;
    bool req_track_status = false;
    bool video = false;
    std::chrono::milliseconds playback_speed_ms(20);
    std::chrono::milliseconds cache_duration_ms(180000);
    std::unordered_map<quicr::messages::TrackAlias, quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>>
      cache;
    std::shared_ptr<quicr::ThreadedTickService> tick_service = std::make_shared<quicr::ThreadedTickService>();

}

namespace qclient_consts {
    const std::filesystem::path kMoqDataDir = std::filesystem::current_path() / "moq_data";
}


class MyFetchTrackHandler : public quicr::FetchTrackHandler
{
    MyFetchTrackHandler(const quicr::FullTrackName& full_track_name,
                        uint64_t start_group,
                        uint64_t start_object,
                        uint64_t end_group,
                        uint64_t end_object)
      : FetchTrackHandler(full_track_name,
                          3,
                          quicr::messages::GroupOrder::kAscending,
                          start_group,
                          end_group,
                          start_object,
                          end_object)
    {
    }

  public:
    static auto Create(const quicr::FullTrackName& full_track_name,
                       uint64_t start_group,
                       uint64_t start_object,
                       uint64_t end_group,
                       uint64_t end_object)
    {
        return std::shared_ptr<MyFetchTrackHandler>(
          new MyFetchTrackHandler(full_track_name, start_group, end_group, start_object, end_object));
    }

    void ObjectReceived(const quicr::ObjectHeaders& headers, quicr::BytesSpan data) override
    {
        std::string msg(data.begin(), data.end());
        SPDLOG_INFO(
          "Received fetched object group_id: {} object_id: {} value: {}", headers.group_id, headers.object_id, msg);
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kOk: {
                if (auto track_alias = GetTrackAlias(); track_alias.has_value()) {
                    SPDLOG_INFO("Track alias: {0} is ready to read", track_alias.value());
                }
            } break;

            case Status::kError: {
                SPDLOG_INFO("Fetch failed");
                break;
            }
            default:
                break;
        }
    }
};

/**
 * @brief Publish track handler
 * @details Publish track handler used for the publish command line option
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
        const auto alias = GetTrackAlias().value();
        switch (status) {
            case Status::kOk: {
                SPDLOG_INFO("Publish track alias: {0} is ready to send", alias);
                break;
            }
            case Status::kNoSubscribers: {
                SPDLOG_INFO("Publish track alias: {0} has no subscribers", alias);
                break;
            }
            case Status::kNewGroupRequested: {
                SPDLOG_INFO("Publish track alias: {0} has new group request", alias);
                break;
            }
            case Status::kSubscriptionUpdated: {
                SPDLOG_INFO("Publish track alias: {0} has updated subscription", alias);
                break;
            }
            case Status::kPaused: {
                SPDLOG_INFO("Publish track alias: {0} is paused", alias);
                break;
            }
            case Status::kPendingPublishOk: {
                SPDLOG_INFO("Publish track alias: {0} is pending publish ok", alias);
                break;
            }

            default:
                SPDLOG_INFO("Publish track alias: {0} has status {1}", alias, static_cast<int>(status));
                break;
        }
    }

    PublishObjectStatus PublishObject(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data) override
    {
        auto track_alias = GetTrackAlias();

        // Cache Object
        if (!qclient_vars::cache.contains(*track_alias)) {
            qclient_vars::cache.emplace(
              *track_alias,
              quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>{
                static_cast<std::size_t>(qclient_vars::cache_duration_ms.count()), 1000, qclient_vars::tick_service });
        }

        CacheObject object{ object_headers, { data.begin(), data.end() } };

        if (auto group = qclient_vars::cache.at(*track_alias).Get(object_headers.group_id)) {
            group->insert(std::move(object));
        } else {
            qclient_vars::cache.at(*track_alias)
              .Insert(object_headers.group_id, { std::move(object) }, qclient_vars ::cache_duration_ms.count());
        }

        return quicr::PublishTrackHandler::PublishObject(object_headers, data);
    }
};

/**
 * @brief MoQ client
 * @details Implementation of the MoQ Client
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

    void SubscribeNamespaceStatusChanged(const quicr::TrackNamespace& track_namespace,
                                         std::optional<quicr::messages::SubscribeNamespaceErrorCode> error_code,
                                         std::optional<quicr::messages::ReasonPhrase> reason) override
    {
        auto th = quicr::TrackHash({ track_namespace, {} });
        if (!error_code.has_value()) {
            SPDLOG_INFO("Subscribe announces namespace_hash: {} status changed to OK", th.track_namespace_hash);
            return;
        }

        std::string reason_str;
        if (reason.has_value()) {
            reason_str.assign(reason.value().begin(), reason.value().end());
        }

        SPDLOG_WARN("Subscribe announces to namespace_hash: {} has error {} with reason: {}",
                    th.track_namespace_hash,
                    static_cast<uint64_t>(error_code.value()),
                    reason_str);
    }

    std::optional<quicr::messages::Location> GetLargestAvailable(const quicr::FullTrackName& track_full_name)
    {
        std::optional<quicr::messages::Location> largest_location = std::nullopt;
        auto th = quicr::TrackHash(track_full_name);

        auto cache_entry_it = qclient_vars::cache.find(th.track_fullname_hash);
        if (cache_entry_it != qclient_vars::cache.end()) {
            auto& [_, cache] = *cache_entry_it;
            if (const auto& latest_group = cache.Last(); latest_group && !latest_group->empty()) {
                const auto& latest_object = std::prev(latest_group->end());
                largest_location = { latest_object->headers.group_id, latest_object->headers.object_id };
            }
        }

        return largest_location;
    }

    void FetchReceived(quicr::ConnectionHandle connection_handle,
                       uint64_t request_id,
                       const quicr::FullTrackName& track_full_name,
                       quicr::messages::SubscriberPriority priority,
                       quicr::messages::GroupOrder group_order,
                       quicr::messages::Location start,
                       std::optional<quicr::messages::Location> end)
    {
        auto reason_code = quicr::FetchResponse::ReasonCode::kOk;
        std::optional<quicr::messages::Location> largest_location = std::nullopt;
        auto th = quicr::TrackHash(track_full_name);

        auto cache_entry_it = qclient_vars::cache.find(th.track_fullname_hash);
        if (cache_entry_it != qclient_vars::cache.end()) {
            auto& [_, cache] = *cache_entry_it;
            if (const auto& latest_group = cache.Last(); latest_group && !latest_group->empty()) {
                const auto& latest_object = *std::prev(latest_group->end());
                largest_location = { latest_object.headers.group_id, latest_object.headers.object_id };
            }
        }

        if (!largest_location.has_value()) {
            // TODO: This changes to send an empty object instead of REQUEST_ERROR
            reason_code = quicr::FetchResponse::ReasonCode::kNoObjects;
        } else {
            SPDLOG_INFO("Fetch received request id: {} largest group: {} object: {}",
                        request_id,
                        largest_location.value().group,
                        largest_location.value().object);
        }

        if (start.group > end->group || largest_location.value().group < start.group) {
            reason_code = quicr::FetchResponse::ReasonCode::kInvalidRange;
        }

        const auto& cache_entries =
          cache_entry_it->second.Get(start.group, end->group != 0 ? end->group : cache_entry_it->second.Size());

        if (cache_entries.empty()) {
            reason_code = quicr::FetchResponse::ReasonCode::kInvalidRange;
        }

        ResolveFetch(connection_handle,
                     request_id,
                     priority,
                     group_order,
                     {
                       reason_code,
                       reason_code == quicr::FetchResponse::ReasonCode::kOk
                         ? std::nullopt
                         : std::make_optional("Cannot process fetch"),
                       largest_location,
                     });

        if (reason_code != quicr::FetchResponse::ReasonCode::kOk) {
            return;
        }

        // TODO: Adjust the TTL
        auto pub_fetch_h =
          quicr::PublishFetchHandler::Create(track_full_name, priority, request_id, group_order, 50000);
        BindFetchTrack(connection_handle, pub_fetch_h);

        std::thread retrieve_cache_thread([=, cache_entries = std::move(cache_entries), this] {
            defer(UnbindFetchTrack(connection_handle, pub_fetch_h));

            for (const auto& entry : cache_entries) {
                for (const auto& object : *entry) {
                    if (end->object && object.headers.group_id == end->group &&
                        object.headers.object_id >= end->object) {
                        return;
                    }

                    SPDLOG_DEBUG(
                      "Fetch sending group: {} object: {}", object.headers.group_id, object.headers.object_id);
                    pub_fetch_h->PublishObject(object.headers, object.data);
                }
            }
        });

        retrieve_cache_thread.detach();
    }

    void StandaloneFetchReceived(quicr::ConnectionHandle connection_handle,
                                 uint64_t request_id,
                                 const quicr::FullTrackName& track_full_name,
                                 const quicr::messages::StandaloneFetchAttributes& attributes)
    {
        FetchReceived(connection_handle,
                      request_id,
                      track_full_name,
                      attributes.priority,
                      attributes.group_order,
                      attributes.start_location,
                      attributes.end_location);
    }

    void JoiningFetchReceived(quicr::ConnectionHandle connection_handle,
                              uint64_t request_id,
                              const quicr::FullTrackName& track_full_name,
                              const quicr::messages::JoiningFetchAttributes& attributes)
    {
        uint64_t joining_start = 0;

        if (attributes.relative) {
            if (const auto largest = GetLargestAvailable(track_full_name)) {
                if (largest->group > attributes.joining_start)
                    joining_start = largest->group - attributes.joining_start;
            }
        } else {
            joining_start = attributes.joining_start;
        }

        FetchReceived(connection_handle,
                      request_id,
                      track_full_name,
                      attributes.priority,
                      attributes.group_order,
                      { joining_start, 0 },
                      std::nullopt);
    }

    void TrackStatusResponseReceived(quicr::ConnectionHandle,
                                     uint64_t request_id,
                                     const quicr::SubscribeResponse& response) override
    {
        switch (response.reason_code) {
            case quicr::SubscribeResponse::ReasonCode::kOk:
                SPDLOG_INFO("Request track status OK response request_id: {} largest group: {} object: {}",
                            request_id,
                            response.largest_location.has_value() ? response.largest_location->group : 0,
                            response.largest_location.has_value() ? response.largest_location->object : 0);
                break;
            default:
                SPDLOG_INFO("Request track status response ERROR request_id: {} error: {} reason: {}",
                            request_id,
                            static_cast<int>(response.reason_code),
                            response.error_reason.has_value() ? response.error_reason.value() : "");
                break;
        }
    }

  private:
    bool& stop_threads_;
};

struct TrackHandlerData
{
    int track_id{ -1 };
    std::string track_type;
    std::shared_ptr<VideoPublishTrackHandler> track_handler;

    uint64_t group_id{ 0 };
    uint64_t object_id{ 0 };
    uint64_t subgroup_id{ 0 };
};

/*===========================================================================*/
// INIT publisher for a track
/*===========================================================================*/

void
PublishCatalog(Catalog& catalog, std::shared_ptr<VideoPublishTrackHandler> TH, const std::atomic<bool>& stop)
{
    bool catalog_published = false;
    int object_id = 0;
    int group_id = 0;
    int subgroup_id = 0;

    while (!stop.load(std::memory_order_relaxed)) {
        switch (TH->GetStatus()) {
            case VideoPublishTrackHandler::Status::kOk:
                break;
            case VideoPublishTrackHandler::Status::kNewGroupRequested:
                if (object_id) {
                    group_id++;
                    object_id = 0;
                    subgroup_id = 0;
                }
                SPDLOG_INFO("New Group Requested: Now using group {0}", group_id);

                break;
            case VideoPublishTrackHandler::Status::kSubscriptionUpdated:
                SPDLOG_INFO("subscribe updated");
                break;
            case VideoPublishTrackHandler::Status::kNoSubscribers:
                // Start a new group when a subscriber joins
                if (object_id) {
                    group_id++;
                    object_id = 0;
                    subgroup_id = 0;
                }
                [[fallthrough]];
            default:
                //SPDLOG_WARN("Catalog publish status not ok: {0}", static_cast<int>(TH->GetStatus()));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
        }

        std::string catalog_str = catalog.to_json(true);

        std::cerr << "Catalog JSON: " << catalog_str << std::endl;

        quicr::BytesSpan catalog_bytespan{ reinterpret_cast<const uint8_t*>(catalog_str.data()), catalog_str.size() };

        quicr::ObjectHeaders obj_headers = { 0,
                                             0,
                                             0,
                                             catalog_bytespan.size(),
                                             quicr::ObjectStatus::kAvailable,
                                             2 /*priority*/,
                                             5000 /* ttl */,
                                             std::nullopt,
                                             std::nullopt };

        try {
            auto status = TH->PublishObject(obj_headers, catalog_bytespan);
            if (status == decltype(status)::kPaused) {
                //SPDLOG_INFO("Publish is paused");
            } else if (status == decltype(status)::kNoSubscribers) {
                //SPDLOG_INFO("Publish has no subscribers");
            } else if (status != decltype(status)::kOk) {
                throw std::runtime_error("PublishObject returned status=" + std::to_string(static_cast<int>(status)));
            } else if (status == decltype(status)::kOk) {
                catalog_published = true;
                SPDLOG_INFO("Catalog published: {0}, group id: {1}, obj. id: {2}, exiting catalog publisher",
                            static_cast<int>(catalog_published),
                            group_id,
                            object_id++);
                // return status;
                break;
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Caught exception trying to publish catalog. (error={})", e.what());
        }
    }
}

/*===========================================================================*/
// Chunk publisher for a track
/*===========================================================================*/

void
PublishChunk(std::shared_ptr<TrackPublishData> TrackPublishData,
             std::shared_ptr<VideoPublishTrackHandler> TH,
             const std::atomic<bool>& stop)
{
    while (!stop.load(std::memory_order_relaxed)) {
        switch (TH->GetStatus()) {
            case VideoPublishTrackHandler::Status::kOk:
                break;
            case VideoPublishTrackHandler::Status::kNewGroupRequested:
                SPDLOG_INFO("New Group Requested, well then wait for a new group", TrackPublishData->group_id);
                break;
            case VideoPublishTrackHandler::Status::kSubscriptionUpdated:
                //SPDLOG_INFO("Subscribe updated");
                break;
            case VideoPublishTrackHandler::Status::kNoSubscribers:
                //SPDLOG_INFO("No subscribers on track id: {0}", TrackPublishData->track_id);

                break;
            case VideoPublishTrackHandler::Status::kPaused:
                //SPDLOG_INFO("Track {} is paused", TrackPublishData->track_id);

                break;
            default:
                SPDLOG_TRACE("Publish status not ok: {0}", static_cast<int>(TH->GetStatus()));
        }

        MP4Chunk chunk = TrackPublishData->GetChunk(stop);
        if (chunk.track_id == -1) {
            TrackPublishData->WaitForChunk();
            continue;
        }

        if (chunk.has_keyframe) { //
            TrackPublishData->group_id++;
            TrackPublishData->object_id = 0;
            TrackPublishData->subgroup_id = 0;
        }

        quicr::ObjectHeaders obj_headers = { TrackPublishData->group_id,
                                             TrackPublishData->object_id,
                                             TrackPublishData->subgroup_id,
                                             chunk.whole_chunk.data.size(),
                                             quicr::ObjectStatus::kAvailable,
                                             2 /*priority*/,
                                             5000 /* ttl */,
                                             std::nullopt,
                                             std::nullopt };

        try {
            auto status = TH->PublishObject(obj_headers, chunk.whole_chunk.data);
            switch (status) {
                case PublishTrackHandler::PublishObjectStatus::kOk: {
                    SPDLOG_INFO("Published chunk OK, track_idx: {1}, group id: {2}, obj. id: {3}",
                                static_cast<int>(status),
                                TrackPublishData->track_id,
                                TrackPublishData->group_id,
                                TrackPublishData->object_id++);
                } break;
                case PublishTrackHandler::PublishObjectStatus::kPaused: {
                    // SPDLOG_INFO("Publish chunk PAUSED on track_idx: {1}, group id: {2}, obj. id: {3}",
                    //             static_cast<int>(status),
                    //             TrackPublishData->track_id,
                    //             TrackPublishData->group_id,
                    //             TrackPublishData->object_id++);
                } break;
                case PublishTrackHandler::PublishObjectStatus::kNoPreviousObject: {
                    SPDLOG_INFO("Publish problem, no prevoius objects on track_idx: {1}, group id: {2}, obj. id: {3}",
                                static_cast<int>(status),
                                TrackPublishData->track_id,
                                TrackPublishData->group_id,
                                TrackPublishData->object_id++);
                } break;
                case PublishTrackHandler::PublishObjectStatus::kNoSubscribers: {
                    // SPDLOG_INFO("Publish problem, no subscribers on track_idx: {1}, group id: {2}, obj. id: {3}",
                    //             static_cast<int>(status),
                    //             TrackPublishData->track_id,
                    //             TrackPublishData->group_id,
                    //             TrackPublishData->object_id++);
                } break;
                default:
                    throw std::runtime_error("PublishObject returned weird status=" +
                                             std::to_string(static_cast<int>(status)));
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Caught exception trying to publish chunk. (error={})", e.what());
        }
    }
}

/*===========================================================================*/
// Video Publisher Thread to perform publishing
/*===========================================================================*/

void
DoPublisher2(std::shared_ptr<PublisherSharedState> shared_state,
             const std::shared_ptr<quicr::Client>& client,
             bool use_announce,
             const std::atomic<bool>& stop)
{
    std::vector<std::thread> track_threads;
    std::vector<std::shared_ptr<VideoPublishTrackHandler>> TrackHandlers;

    FfmpegCmafSplitterConfig cfg;
    cfg.io_buffer_kb = 64;
    cfg.use_custom_stdin = false; // <- fájlból olvasunk
    // ABSZOLÚT útvonal: így nem számít, honnan indítod a scriptet
    std::filesystem::path in = std::filesystem::absolute("multi_frag.mp4");
    // SPDLOG_INFO("Opening input: {}", in.string());
    //  if (!std::filesystem::exists(in)) {
    //      SPDLOG_ERROR("Input not found: {} ; CWD = {}", in.string(), std::filesystem::current_path().string());
    //      return; // vagy állíts be hibajelet, hogy a controller le tudja állítani a publishert
    //  }
    cfg.input_url = ("pipe:0"); // in.string();

    // fragmentálás finomhangolás (lásd lent)
    cfg.frag_on_key = true;        // maradhat, ha kicsi a GOP
    cfg.frag_duration_us = 250000; // 250 ms: gyakoribb moof-ok
    cfg.min_frag_duration_us = 200000;
    cfg.realtime_pace = true;
    cfg.protocol_whitelist = "file,pipe,data,crypto,subfile";

    FfmpegToMoQAdapter adapter(shared_state);
    FfmpegCmafSplitter splitter(cfg, adapter);

    // lambda that calls the adapters on_init with the signature required

    std::thread splitter_thread([&splitter, &stop]() { splitter.Run(stop); });

    fprintf(stderr, "[PUB-DEBUG] >>> DoPublisher2: Splitter thread started. Now waiting for catalog...\n");

    shared_state->WaitForCatalogReady(stop);
    fprintf(stderr,
            "[PUB-DEBUG] >>> DoPublisher2: Wait for catalog finished. Catalog ready: %d\n",
            shared_state->catalog_ready);

    if (shared_state->catalog_ready == false) {
        moq_example::terminate = true;
        splitter_thread.join();
        return;
    }

    if (shared_state->catalog.tracks().empty()) {
        SPDLOG_WARN("No track names found in catalog");
        moq_example::terminate = true;
        splitter_thread.join();
        return;
    }
    {
        quicr::FullTrackName full_track_name =
          quicr::example::MakeFullTrackName(shared_state->catalog.namespace_, "catalog");

        auto th = std::make_shared<VideoPublishTrackHandler>(full_track_name, quicr::TrackMode::kStream, 1, 3000);
        TrackHandlers.push_back(th);
        th->SetUseAnnounce(true);
        th->SetTrackAlias(0);

        client->PublishTrack(th);
        std::thread catalog_thread(PublishCatalog, std::ref(shared_state->catalog), th, std::cref(stop));
        track_threads.push_back(std::move(catalog_thread));
    }
    for (CatalogTrackEntry track : shared_state->catalog.tracks()) {

        quicr::FullTrackName full_track_name =
          quicr::example::MakeFullTrackName(shared_state->catalog.namespace_, track.name);

        auto th = std::make_shared<VideoPublishTrackHandler>(full_track_name, quicr::TrackMode::kStream, 2, 3000);
        TrackHandlers.push_back(th);
        th->SetUseAnnounce(false);
        th->SetTrackAlias(track.idx);

        client->PublishTrack(th);

        std::shared_ptr<TrackPublishData> tpd;
        {
            std::lock_guard<std::mutex> lk(shared_state->s_mtx);
            auto it = shared_state->tracks.find(track.idx);
            if (it != shared_state->tracks.end())
                tpd = it->second;
        }
        if (!tpd) {
            SPDLOG_ERROR("No TrackPublishData for catalog idx={} (skipping thread)", track.idx);
            continue;
        }
        tpd->Trackhandler = th;

        std::thread track_thread(PublishChunk, std::ref(tpd), th, std::ref(stop));
        track_threads.push_back(std::move(track_thread));
    }
    // std::thread publisher_thread(UnifiedMediaPublisher, shared_state, std::cref(stop));

    while (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    if (splitter_thread.joinable()) {
        splitter_thread.join();
    }

    for (auto val : shared_state->tracks | std::views::values) {
        val->cv.notify_one();
    }
    for (auto& t : track_threads) {
        try {
            t.join();
        } catch (...) {
            SPDLOG_WARN("Exception while joining a track thread (ignored)");
        }
    }

    // if (publisher_thread.joinable()) {
    //     publisher_thread.join();
    // }

    for (auto& th : TrackHandlers) {
        try {
            client->UnpublishTrack(th);
        } catch (...) {
            SPDLOG_WARN("Exception while unpublishing a track handler (ignored)");
        }
    }

    SPDLOG_INFO("Publisher done track");
    moq_example::terminate = true;
}

struct SubTrackHandlerStruct
{
    std::shared_ptr<VideoSubscribeTrackHandler> track_handler;
    SubTrack& trackDetails;
};

// ---- Video track toggle helpers ----
struct VideoToggleContext
{
    std::vector<std::string> video_names; // pl. {"video1","video2"}
    std::unordered_map<std::string, std::shared_ptr<VideoSubscribeTrackHandler>> handler_by_name;
    int active_idx = -1; // melyik video aktív
};

static VideoToggleContext s_vctx;
static std::mutex s_vctx_mu;
static std::atomic<bool> s_toggle_requested{ false };
static std::atomic<bool> s_keyloop_running{ false };
static std::thread s_keyloop;

/*===========================================================================*/
// Subscriber thread to perform subscribe
/*===========================================================================*/

void
DoSubscriber(const std::string& track_namespace,
             const std::shared_ptr<quicr::Client>& client,
             quicr::messages::FilterType filter_type,
             const bool& stop,
             const std::optional<std::uint64_t> join_fetch,
             const bool absolute)
{
    using Fetch = quicr::SubscribeTrackHandler::JoiningFetch;
    const auto joining_fetch = join_fetch.has_value()
                                 ? Fetch{ 4, quicr::messages::GroupOrder::kAscending, {}, *join_fetch, absolute }
                                 : std::optional<Fetch>(std::nullopt);

    std::vector<SubTrackHandlerStruct> sub_track_handlers; // itt TOVÁBBRA IS csak non-video-k legyenek

    static VideoToggleContext s_vctx;
    static std::atomic_bool s_keyloop_running{ false };
    static std::thread s_keyloop;
    static std::atomic_bool s_toggle_requested{ false };
    static std::mutex s_vctx_mu;

    auto sub_util = std::make_shared<SubscriberUtil>();

    // 1) KATALÓGUS FELIRATKOZÁS
    auto catalog_full_track_name = quicr::example::MakeFullTrackName(track_namespace, "catalog");
    const auto catalog_track_handler = std::make_shared<VideoSubscribeTrackHandler>(
      catalog_full_track_name, messages::FilterType::kLargestObject, joining_fetch, true);
    catalog_track_handler->InitCatalogTrack(sub_util);

    SPDLOG_INFO("Started subscriber");

    if (client->GetStatus() == MyClient::Status::kReady) {
        SPDLOG_INFO("Subscribing to catalog track");
        client->SubscribeTrack(catalog_track_handler);
    } else {
        // kis várakozás míg Ready lesz
        while (!stop && client->GetStatus() != MyClient::Status::kReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!stop) {
            client->SubscribeTrack(catalog_track_handler);
        }
    }

    // video le (bár a relay oldali unannounce kezelheti, explicit rendbetesszük)
    {
        std::lock_guard<std::mutex> lk(s_vctx_mu);
        for (auto& kv : s_vctx.handler_by_name) {
            if (kv.second) {
                try {
                    client->UnsubscribeTrack(kv.second);
                } catch (...) {
                }
            }
        }
        s_vctx.handler_by_name.clear();
        s_vctx.video_names.clear();
        s_vctx.active_idx = -1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SPDLOG_INFO("Subscriber done track");
    moq_example::terminate = true;
}

/*===========================================================================*/
// Fetch thread to perform fetch
/*===========================================================================*/

struct Range
{
    uint64_t start;
    uint64_t end;
};

void
DoFetch(const quicr::FullTrackName& full_track_name,
        const Range& group_range,
        const Range& object_range,
        const std::shared_ptr<quicr::Client>& client,
        const bool& stop)
{
    auto track_handler = MyFetchTrackHandler::Create(
      full_track_name, group_range.start, object_range.start, group_range.end, object_range.end);

    SPDLOG_INFO("Started fetch");

    bool fetch_track{ false };

    while (not stop) {
        if ((!fetch_track) && (client->GetStatus() == MyClient::Status::kReady)) {
            SPDLOG_INFO("Fetching track");
            client->FetchTrack(track_handler);
            fetch_track = true;
        }

        if (track_handler->GetStatus() == quicr::FetchTrackHandler::Status::kPendingResponse) {
            // do nothing...
        } else if (!fetch_track || (track_handler->GetStatus() != quicr::FetchTrackHandler::Status::kOk)) {
            SPDLOG_INFO("GetStatus() != quicr::FetchTrackHandler::Status::kOk {}", (int)track_handler->GetStatus());
            moq_example::terminate = true;
            moq_example::cv.notify_all();
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    client->CancelFetchTrack(track_handler);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    moq_example::terminate = true;
}

/*===========================================================================*/
// Main program
/*===========================================================================*/

quicr::ClientConfig
InitConfig(cxxopts::ParseResult& cli_opts, bool& enable_pub, bool& enable_sub, bool& enable_fetch, bool& use_announce)
{
    quicr::ClientConfig config;

    std::string qlog_path;
    if (cli_opts.count("qlog")) {
        qlog_path = cli_opts["qlog"].as<std::string>();
    }

    if (cli_opts.count("debug") && cli_opts["debug"].as<bool>() == true) {
        SPDLOG_INFO("setting debug level");
        spdlog::set_level(spdlog::level::trace);
    }

    if (cli_opts.count("trace") && cli_opts["trace"].as<bool>() == true) {
        SPDLOG_INFO("setting trace level");
        spdlog::set_level(spdlog::level::trace);
    }

    if (cli_opts.count("version") && cli_opts["version"].as<bool>() == true) {
        SPDLOG_INFO("QuicR library version: {}", QUICR_VERSION);
        exit(0);
    }

    if (cli_opts.count("pub_namespace") && cli_opts.count("pub_name")) {
        enable_pub = true;
        SPDLOG_INFO("Publisher enabled using track namespace: {0} name: {1}",
                    cli_opts["pub_namespace"].as<std::string>(),
                    cli_opts["pub_name"].as<std::string>());
    }

    if (cli_opts.count("use_announce")) {
        use_announce = true;
        SPDLOG_INFO("Publisher will use announce flow");
    }

    if (cli_opts.count("clock") && cli_opts["clock"].as<bool>() == true) {
        SPDLOG_INFO("Running in clock publish mode");
        qclient_vars::publish_clock = true;
    }


    if (cli_opts.count("sub_namespace") && cli_opts.count("sub_name")) {
        enable_sub = true;
        SPDLOG_INFO("Subscriber enabled using track namespace: {0} name: {1}",
                    cli_opts["sub_namespace"].as<std::string>(),
                    cli_opts["sub_name"].as<std::string>());
    }

    if (cli_opts.count("fetch_namespace") && cli_opts.count("fetch_name")) {
        enable_fetch = true;
        SPDLOG_INFO("Subscriber enabled using track namespace: {0} name: {1}",
                    cli_opts["fetch_namespace"].as<std::string>(),
                    cli_opts["fetch_name"].as<std::string>());
    }

    if (cli_opts.count("track_alias")) {
        qclient_vars::track_alias = cli_opts["track_alias"].as<uint64_t>();
    }

    if (cli_opts.count("record")) {
        qclient_vars::record = true;
    }

    if (cli_opts.count("playback")) {
        qclient_vars::playback = true;
    }

    if (cli_opts.count("gaps") && cli_opts["gaps"].as<bool>() == true) {
        SPDLOG_INFO("Adding gaps to group and objects");
        qclient_vars::add_gaps = true;
    }

    if (cli_opts.count("new_group")) {
        qclient_vars::new_group_request_id = cli_opts["new_group"].as<uint64_t>();
    }

    if (cli_opts.count("track_status")) {
        qclient_vars::req_track_status = true;
    }

    if (cli_opts.count("playback_speed_ms")) {
        qclient_vars::playback_speed_ms = std::chrono::milliseconds(cli_opts["playback_speed_ms"].as<uint64_t>());
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
    // Initialize logger inside a function
    // logger = spdlog::stderr_color_mt("err_logger");
    // spdlog::set_default_logger(logger);

    int result_code = EXIT_SUCCESS;

    cxxopts::Options options("qclient",
                             std::string("MOQ Example Client using QuicR Version: ") + std::string(QUICR_VERSION));

    // clang-format off
    options.set_width(75)
      .set_tab_expansion()
      //.allow_unrecognised_options()
      .add_options()
        ("h,help", "Print help")
        ("d,debug", "Enable debugging") // a bool parameter
        ("t,trace", "Enable tracing") // a bool parameter
        ("v,version", "QuicR Version")                                        // a bool parameter
        ("r,url", "Relay URL", cxxopts::value<std::string>()->default_value("moq://localhost:1234"))
        ("e,endpoint_id", "This client endpoint ID", cxxopts::value<std::string>()->default_value("moq-client"))
        ("q,qlog", "Enable qlog using path", cxxopts::value<std::string>())
        ("s,ssl_keylog", "Enable SSL Keylog for transport debugging");

    options.add_options("Publisher")
        ("use_announce", "Use Announce flow instead of publish flow", cxxopts::value<bool>())
        ("track_alias", "Track alias to use", cxxopts::value<uint64_t>())
        ("pub_namespace", "Track namespace", cxxopts::value<std::string>())
        ("pub_name", "Track name", cxxopts::value<std::string>())
        ("clock", "Publish clock timestamp every second instead of using STDIN chat")
        ("playback", "Playback recorded data from moq and dat files", cxxopts::value<bool>())
        ("playback_speed_ms", "Playback speed in ms", cxxopts::value<std::uint64_t>())
        ("video", "Input MP4 video file (for publisher mode)")
        ("gaps", "Add gaps to groups and objects");

    options.add_options("Subscriber")
        ("sub_namespace", "Track namespace", cxxopts::value<std::string>())
        ("sub_name", "Track name", cxxopts::value<std::string>())
        ("start_point", "Start point for Subscription - 0 for from the beginning, 1 from the latest object", cxxopts::value<uint64_t>())
        ("sub_announces", "Prefix namespace to subscribe announces to", cxxopts::value<std::string>())
        ("record", "Record incoming data to moq and dat files", cxxopts::value<bool>())
        ("new_group", "Request new group on subscribe", cxxopts::value<bool>())
        ("joining_fetch", "Subscribe with a joining fetch using this joining start", cxxopts::value<std::uint64_t>())
        ("absolute", "Joining fetch will be absolute not relative", cxxopts::value<bool>())
        ("track_status", "Request track status using sub_namespace and sub_name options", cxxopts::value<bool>());

    options.add_options("Fetcher")
        ("fetch_namespace", "Track namespace", cxxopts::value<std::string>())
        ("fetch_name", "Track name", cxxopts::value<std::string>())
        ("start_group", "Starting group ID", cxxopts::value<uint64_t>())
        ("end_group", "One past the final group ID", cxxopts::value<uint64_t>())
        ("start_object", "The starting object ID within the group", cxxopts::value<uint64_t>())
        ("end_object", "One past the final object ID in the group", cxxopts::value<uint64_t>());

    // clang-format on

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help({ "", "Publisher", "Subscriber", "Fetcher" }) << std::endl;
        return EXIT_SUCCESS;
    }

    // Install a signal handlers to catch operating system signals
    installSignalHandlers();

    // Lock the mutex so that main can then wait on it
    std::unique_lock lock(moq_example::main_mutex);

    bool enable_pub{ false };
    bool enable_sub{ false };
    bool enable_fetch{ false };
    bool use_announce{ false };
    quicr::ClientConfig config = InitConfig(result, enable_pub, enable_sub, enable_fetch, use_announce);

    SPDLOG_INFO("INFO");
    SPDLOG_WARN("WARN");
    SPDLOG_ERROR("ERROR");
    SPDLOG_DEBUG("DEBUG");
    SPDLOG_TRACE("TRACE");

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

        std::thread pub_thread;
        std::thread sub_thread;
        std::thread fetch_thread;
        std::thread parse_thread;

        if (result.count("sub_announces")) {
            const auto& prefix_ns = quicr::example::MakeFullTrackName(result["sub_announces"].as<std::string>(), "");

            auto th = quicr::TrackHash(prefix_ns);

            SPDLOG_INFO("Sending subscribe announces for prefix '{}' namespace_hash: {}",
                        result["sub_announces"].as<std::string>(),
                        th.track_namespace_hash);

            client->SubscribeNamespace(prefix_ns.name_space);
        }

        if (enable_pub) {
            if (!qclient_vars::video) {
                SPDLOG_INFO("Not today...");
            } else {
                auto shared_state = std::make_shared<PublisherSharedState>();

                bool stop_parse{ false };

                // parse_thread = std::thread(DoParse, shared_state, std::ref(stop_parse));

                // shared_state->WaitForCatalogReady(stop_threads);

                if (result["pub_namespace"].as<std::string>() != "") {
                    shared_state->catalog.namespace_ = result["pub_namespace"].as<std::string>();
                } else {
                    SPDLOG_ERROR("No namespace specified for video publishing");
                    return EXIT_FAILURE;
                }

                pub_thread = std::thread(DoPublisher2, shared_state, client, use_announce, stop_threads);
            }
        }
        if (enable_sub) {
            auto filter_type = quicr::messages::FilterType::kLargestObject;
            if (result.count("start_point")) {
                if (result["start_point"].as<uint64_t>() == 0) {
                    filter_type = quicr::messages::FilterType::kNextGroupStart;
                    SPDLOG_INFO("Setting subscription filter to Next Group Start");
                }
            }
            std::optional<std::uint64_t> joining_fetch;
            if (result.count("joining_fetch")) {
                joining_fetch = result["joining_fetch"].as<uint64_t>();
            }
            bool absolute = result.count("absolute") && result["absolute"].as<bool>();

            const auto& sub_track_name = quicr::example::MakeFullTrackName(result["sub_namespace"].as<std::string>(),
                                                                           result["sub_name"].as<std::string>());

            if (qclient_vars::req_track_status) {
                client->RequestTrackStatus(sub_track_name);
            }

            sub_thread = std::thread(DoSubscriber,
                                     result["sub_namespace"].as<std::string>(),
                                     client,
                                     filter_type,
                                     std::ref(stop_threads),
                                     joining_fetch,
                                     absolute);
        }
        if (enable_fetch) {
            const auto& fetch_track_name = quicr::example::MakeFullTrackName(
              result["fetch_namespace"].as<std::string>(), result["fetch_name"].as<std::string>());

            fetch_thread =
              std::thread(DoFetch,
                          fetch_track_name,
                          Range{ result["start_group"].as<uint64_t>(), result["end_group"].as<uint64_t>() },
                          Range{ result["start_object"].as<uint64_t>(), result["end_object"].as<uint64_t>() },
                          client,
                          std::ref(stop_threads));
        }

        // Wait until told to terminate
        moq_example::cv.wait(lock, [&]() { return moq_example::terminate; });

        stop_threads = true;
        SPDLOG_ERROR("Stopping threads...");
        // if (parse_thread.joinable()) {
        //     parse_thread.join();
        // }

        if (pub_thread.joinable()) {
            pub_thread.join();
        }

        if (sub_thread.joinable()) {
            sub_thread.join();
        }

        if (fetch_thread.joinable()) {
            fetch_thread.join();
        }

        client->Disconnect();

        SPDLOG_ERROR("Client done");
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

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
