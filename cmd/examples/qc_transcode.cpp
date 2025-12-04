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
#include "signal_handler.h"
#include <quicr/cache.h>
#include <quicr/defer.h>

#include <filesystem>
#include <fstream>

#include <quicr/publish_fetch_handler.h>

#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>


#include "CatalogSubscribeTrackHandler.h"
#include "TranscodeRequestSubscribeTrackHandler.h"
#include "TranscodeSubscribeTrackHandler.h"
#include "base64_tool.h"
#include "media.h"
#include "subscriber_util.h"
#include "transcode_client.h"

#include "transcode_request.h"

#include <set>

#include <iomanip>

#include <optional>

std::shared_ptr<spdlog::logger> logger;

using json = nlohmann::json; // NOLINT

class MyClient;

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

    std::mutex cache_mutex;

    std::unordered_map<quicr::messages::TrackAlias, quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>>
      cache;
    std::shared_ptr<quicr::ThreadedTickService> tick_service = std::make_shared<quicr::ThreadedTickService>();

}

namespace qclient_consts {
    const std::filesystem::path kMoqDataDir = std::filesystem::current_path() / "moq_data";
}

/**
 * @brief Thread-safe fragment queue for transcoded data
 */
class FragmentQueue
{
private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<MP4Chunk> queue_;
    std::atomic<bool> closed_{ false };

public:
    void Push(MP4Chunk chunk)
    {
        if (closed_.load()) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        queue_.emplace_back(chunk);
        cv_.notify_one();
    }

    std::optional<MP4Chunk> TryPop(std::chrono::milliseconds timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (!cv_.wait_for(lock, timeout_ms, [this] { return !queue_.empty() || closed_.load(); })) {
            return std::nullopt;
        }

        if (queue_.empty()) {
            return std::nullopt;
        }

        auto fragment = std::move(queue_.front());
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
class SmartDeltaPublishTrackHandler : public quicr::PublishTrackHandler
{
    int group_id_{ 0 };
    int object_id_{ 0 };
    std::mutex mutex_;

  public:
    SmartDeltaPublishTrackHandler(const quicr::FullTrackName& full_track_name,
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

    PublishObjectStatus PublishObject(const quicr::ObjectHeaders& object_headers_original, quicr::BytesSpan data) override
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto track_alias = GetTrackAlias();

        auto object_headers = object_headers_original;
        object_headers.group_id = group_id_;
        object_headers.object_id = object_id_;

        {
            std::lock_guard<std::mutex> cache_lock(qclient_vars::cache_mutex);

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
        }

        group_id_++;

        return quicr::PublishTrackHandler::PublishObject(object_headers, data);
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

        {
            std::lock_guard<std::mutex> lock(qclient_vars::cache_mutex);

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
private:
    ClientConfig client_config_;
    bool& stop_threads_;

    // Queue to store incoming requests found via announcements
    std::shared_ptr<TranscodeRequestQueue> request_queue_;
    quicr::TrackNamespace requests_ns_prefix_;

    std::mutex handlers_mutex_;
    std::vector<std::shared_ptr<TranscodeRequestSubscribeHandler>> active_request_handlers_;

    std::mutex source_mutex_;
    std::map<std::string, std::shared_ptr<TranscodeSubscribeTrackHandler>> active_source_handlers_;

    std::string MakeSourceKey(const std::string& ns, const std::string& name) {
        return ns + "::" + name;
    }

    MyClient(const quicr::ClientConfig& cfg, bool& stop_threads)
      : quicr::Client(cfg)
      , client_config_(cfg)
      , stop_threads_(stop_threads)
    {
    }

  public:
    static std::shared_ptr<MyClient> Create(const quicr::ClientConfig& cfg, bool& stop_threads)
    {
        return std::shared_ptr<MyClient>(new MyClient(cfg, stop_threads));
    }

    void SetRequestQueue(std::shared_ptr<TranscodeRequestQueue> queue) {
        request_queue_ = queue;
    }

    void SetRequestsNamespacePrefix(const quicr::TrackNamespace ns) {
        requests_ns_prefix_ = ns;
    }

    std::shared_ptr<TranscodeSubscribeTrackHandler> GetOrSubscribeSource(
        const std::string& ns,
        const std::string& name,
        std::shared_ptr<SubTrack> subtrack_data)
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        std::string key = MakeSourceKey(ns, name);

        if (active_source_handlers_.contains(key)) {
            SPDLOG_INFO("REUSING source subscription: {}-{}", ns, name);
            return active_source_handlers_.at(key);
        }

        SPDLOG_INFO("NEW source subscription: {}-{}", ns, name);
        auto ftn = quicr::example::MakeFullTrackName(ns, name);

        auto handler = std::make_shared<TranscodeSubscribeTrackHandler>(
            ftn, quicr::messages::FilterType::kLargestObject, std::nullopt, subtrack_data
        );

        SubscribeTrack(handler);
        active_source_handlers_.try_emplace(key, handler);
        return handler;
    }

    void ReleaseSource(const std::string& ns, const std::string& name, std::shared_ptr<transcode::TranscodeClient> client)
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        std::string key = MakeSourceKey(ns, name);

        if (active_source_handlers_.contains(key)) {
            auto handler = active_source_handlers_[key];
            if (handler->RemoveTranscodeClient(client)) {
                SPDLOG_INFO("Source {}-{} empty. Unsubscribing.", ns, name);
                UnsubscribeTrack(handler);
                active_source_handlers_.erase(key);
            }
        }
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

    /**
     * @brief Checks active handlers and unsubscribes from finished ones.
     * Called from the main DoSubscriber loop.
     */
    void CleanupFinishedRequestHandlers()
    {
        try
        {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        if (active_request_handlers_.empty()) return;

        auto it = active_request_handlers_.begin();
        while (it != active_request_handlers_.end()) {
            if ((*it)->IsDone()) {
                SPDLOG_INFO("Handler finished, unsubscribing from request track");
                UnsubscribeTrack(*it);
                it = active_request_handlers_.erase(it);
            } else {
                ++it;
            }
        }
        }
        catch (const std::exception& ex)
        {
            SPDLOG_ERROR("Exception in CleanupFinishedRequestHandlers: {}", ex.what());
        }
    }

    std::string getEndpointID() const
    {
        return client_config_.endpoint_id;
    }

    void PublishNamespaceReceived(const quicr::TrackNamespace& track_namespace,
                                          const quicr::PublishNamespaceAttributes&) override
    {
        if (!request_queue_ || requests_ns_prefix_.empty()) return;

        std::string ns_str = track_namespace.ToString();
        std::string prefix_str = requests_ns_prefix_.ToString();

        if (ns_str.find(prefix_str) == 0) {
            if (ns_str.length() > prefix_str.length()) {

                SPDLOG_INFO("Detected Request Announce: {}", ns_str);

                auto ftn = quicr::example::MakeFullTrackName(ns_str, "data");

                auto track_handler = std::make_shared<TranscodeRequestSubscribeHandler>(
                    ftn, request_queue_, false
                );

                SubscribeTrack(track_handler);

                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    active_request_handlers_.push_back(track_handler);
                }
            }
        }
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
        std::lock_guard<std::mutex> lock(qclient_vars::cache_mutex);

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

    /* This function in this client handles incoming transcode request tracks, so on each call of this function we can subscribe to the track
     */
void PublishReceived(quicr::ConnectionHandle connection_handle, uint64_t request_id, const quicr::messages::PublishAttributes& publish_attributes) override
    {
        auto th = quicr::TrackHash(publish_attributes.track_full_name);

        if (!request_queue_ || requests_ns_prefix_.empty()) {
            SPDLOG_ERROR("Request queue or requests namespace prefix not initialized.");
            ResolvePublish(connection_handle, request_id, publish_attributes, { .reason_code = quicr::PublishResponse::ReasonCode::kNotSupported });
            return;
        }

        std::string incoming_ns = publish_attributes.track_full_name.name_space.ToString();
        std::string expected_prefix = requests_ns_prefix_.ToString();
        std::string inc_track_name ={publish_attributes.track_full_name.name.begin(), publish_attributes.track_full_name.name.end()};
        if (incoming_ns.find(expected_prefix) != 0) {
            SPDLOG_ERROR("PUBLISH track {}-{} does not match expected prefix {}.",
                         incoming_ns,
                         inc_track_name,
                         expected_prefix);
             ResolvePublish(connection_handle, request_id, publish_attributes, { .reason_code = quicr::PublishResponse::ReasonCode::kNotSupported });
            return;
        }

        SPDLOG_INFO("Received Transcode Request PUBLISH: request_id: {}, track alias: {}", request_id, publish_attributes.track_alias);

        const auto track_handler = std::make_shared<TranscodeRequestSubscribeHandler>(publish_attributes.track_full_name, request_queue_, true);
        track_handler->SetRequestId(request_id);
        track_handler->SetReceivedTrackAlias(publish_attributes.track_alias);
        track_handler->SetPriority(publish_attributes.priority);
        track_handler->SetDeliveryTimeout(publish_attributes.delivery_timeout);
        track_handler->SupportNewGroupRequest(publish_attributes.new_group_request_id.has_value());

        //SubscribeTrack(track_handler);
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            active_request_handlers_.push_back(track_handler);
        }

        ResolvePublish(connection_handle, request_id, publish_attributes, { .reason_code = quicr::PublishResponse::ReasonCode::kNotSupported });
    }
};


/*===========================================================================*/
// Transcoding thread function
/*===========================================================================*/

/*
 * @brief Function to handle transcoding for a single request
 * @details Setup is synchronous, publishing loop is asynchronous.
 */
void HandleTranscodeRequest(const TranscodeRequest& request,
                            const Catalog& catalog,
                            std::shared_ptr<quicr::PublishTrackHandler> delta_track_handler,
                            std::shared_ptr<MyClient> client,
                            const std::string& base_namespace)
{
    std::string req_id_str = request.request_id;

    SPDLOG_INFO("Starting transcoding setup (SYNC) for request_id: {}", req_id_str);

    try {
        // 1. Validate request has video operations
        if (request.operations.empty()) {
            SPDLOG_ERROR("Request {} has no operations", req_id_str);
            return;
        }

        auto media_type = infer_media_type(request.operations[0].kind);
        if (media_type != InferredMediaType::Video || request.operations[0].kind != OperationKind::VideoChangeResolution) {
            SPDLOG_ERROR("Request {} contains non-video operations or not VideoChangeResolution", req_id_str);
            return;
        }

        // 2. Find source track in catalog
        std::string source_ns = request.source.ns.value_or(base_namespace);
        std::string source_track_name = request.source.track;

        SPDLOG_INFO("Looking for source track: namespace={}, name={}", source_ns, source_track_name);

        auto& tracks = const_cast<Catalog&>(catalog).tracks();
        auto track_it = std::find_if(tracks.begin(), tracks.end(), [&](const CatalogTrackEntry& entry) {
            std::string entry_ns = entry.effective_src_namespace(catalog.namespace_);
            return entry_ns == source_ns && entry.name == source_track_name;
        });

        if (track_it == tracks.end()) {
            SPDLOG_ERROR("Source track {} not found in catalog", source_track_name);
            return;
        }

        // 3. Build transcode configuration
        transcode::TranscodeConfig transcode_config;
        for (const auto& op : request.operations) {
            if (op.kind == OperationKind::VideoChangeResolution) {
                const auto& res_op = std::get<OpVideoChangeResolution>(op.data);
                transcode_config.target_width = res_op.width;
                transcode_config.target_height = res_op.height;
                transcode_config.debug = true;
            }
        }

        // 4. Create Transcode Client and Queue
        std::shared_ptr<transcode::TranscodeClient> transcode_client = std::make_shared<transcode::TranscodeClient>(transcode_config);
        auto fragment_queue = std::make_shared<FragmentQueue>();

        std::string output_ns;
        if (request.output.has_value() && request.output->ns.has_value()) {
            output_ns = request.output->ns.value();
        } else {
            output_ns = "out," + base_namespace;
        }

        std::string output_track_name =
          request.output.has_value() && request.output->track_name_hint.has_value()
            ? request.output->track_name_hint.value()
            : "tran_" + client->getEndpointID() + "_" + source_track_name + "_" + std::to_string(transcode_config.target_height) + "p";

        // 5. Init Callback -> Delta Update
        transcode_client->SetOutputInitCallback([=](const uint8_t* data, size_t size) {
                try {
                    if (size == 0 || data == nullptr) {
                        SPDLOG_WARN("Transcoder Init Callback called with empty data");
                        return;
                    }

                    SPDLOG_INFO("Transcoder Init Ready. Publishing Delta Update. Size: {}", size);

                    CatalogTrackEntry new_entry;
                    new_entry.name = output_track_name;
                    new_entry.track_namespace_ = output_ns;
                    new_entry.type = "video";

                    // Extra védelem a vektor létrehozásnál
                    std::vector<uint8_t> init_vec;
                    init_vec.assign(data, data + size);
                    new_entry.b64_init_data = base64::Encode(init_vec);

                    new_entry.init_binary_size = size;
                    new_entry.width = transcode_config.target_width;
                    new_entry.height = transcode_config.target_height;
                    new_entry.idx = 4000 + std::hash<std::string>{}(req_id_str) % 1000;
                    new_entry.label = output_track_name;
                    if (track_it->alt_group.has_value()) new_entry.alt_group = track_it->alt_group;

                    std::string patch_json = Catalog::makeCatalogPatch(new_entry, false);

                    quicr::ObjectHeaders headers;
                    headers.payload_length = patch_json.size();

                    if (delta_track_handler) {
                        delta_track_handler->PublishObject(headers,
                            quicr::BytesSpan(reinterpret_cast<const uint8_t*>(patch_json.data()), patch_json.size()));
                    }
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("CRITICAL ERROR inside SetOutputInitCallback: {}", e.what());
                } catch (...) {
                    SPDLOG_ERROR("Unknown CRITICAL ERROR inside SetOutputInitCallback");
                }
        });

        // 6. Fragment callback
        transcode_client->SetOutputFragmentCallback([fragment_queue](MP4Chunk chunk) {
            fragment_queue->Push(chunk);
        });

        // 7. Push Input Init
        std::vector<uint8_t> init_data = base64::decode_to_uint8_vec(track_it->b64_init_data);
        transcode_client->PushInputInit(init_data.data(), init_data.size());

        // 9. Publish Output Track
        auto output_full_track_name = quicr::example::MakeFullTrackName(output_ns, output_track_name);
        auto output_track_handler =
          std::make_shared<VideoPublishTrackHandler>(output_full_track_name, quicr::TrackMode::kStream, 2, 3000);
        output_track_handler->SetTrackAlias(4000 + std::hash<std::string>{}(req_id_str) % 1000);

        client->PublishTrack(output_track_handler);
        SPDLOG_INFO("Publishing transcoded track: {}-{}", output_ns, output_track_name);

        auto subtrack = std::make_shared<SubTrack>();
        subtrack->track_entry = *track_it;
        subtrack->namespace_ = source_ns;
        subtrack->init = init_data;

        auto source_track_handler = client->GetOrSubscribeSource(source_ns, source_track_name, subtrack);
        source_track_handler->AddTranscodeClient(transcode_client);

        SPDLOG_INFO("Subscribed to source track (SYNC setup complete).");


  std::thread publishing_thread([
            fragment_queue,
            output_track_handler,
            source_track_handler,
            transcode_client,
            client,
            req_id_str,
            source_ns,
            source_track_name
        ]() {
            try {
                SPDLOG_INFO("ASYNC thread STARTED for request_id: {}", req_id_str);

                uint64_t group_id = 0;
                uint64_t object_id = 0;
                bool running = true;

                while (running && !moq_example::terminate) {
                    auto fragment_opt = fragment_queue->TryPop(std::chrono::milliseconds(100));

                    if (fragment_opt.has_value()) {
                        auto& fragment = fragment_opt.value();

                        if (fragment.has_keyframe && object_id != 0) {
                            group_id++;
                            object_id = 0;
                        }

                        quicr::ObjectHeaders obj_headers = { group_id,
                                                             object_id,
                                                             0,
                                                             fragment.whole_chunk.data.size(),
                                                             quicr::ObjectStatus::kAvailable,
                                                             2,
                                                             3000,
                                                             std::nullopt,
                                                             std::nullopt,
                                                             std::nullopt};

                        if (output_track_handler->CanPublish()) {
                            auto status = output_track_handler->PublishObject(obj_headers, fragment.whole_chunk.data);
                            if (status == quicr::PublishTrackHandler::PublishObjectStatus::kOk) {
                                SPDLOG_DEBUG("Published transcoded fragment: group={}, object={} on track alias={}",
                                             group_id, object_id++, fragment.whole_chunk.data.size(), output_track_handler->GetTrackAlias().value());
                            }
                        }
                    }

                    if (fragment_queue->IsClosed() && fragment_queue->Empty()) {
                        SPDLOG_INFO("Fragment queue closed and empty, finishing transcoding");
                        running = false;
                    }
                }

                // Cleanup
                SPDLOG_INFO("Cleaning up thread resources for {}", req_id_str);
                if (transcode_client) {
                    transcode_client->Flush();
                    transcode_client->Close();
                }
                if (client) {
                    client->ReleaseSource(source_ns, source_track_name, transcode_client);
                    client->UnpublishTrack(output_track_handler);
                }

                SPDLOG_INFO("Transcoding background thread finished for request_id: {}", req_id_str);

            } catch (const std::exception& e) {
                SPDLOG_ERROR("CRITICAL ERROR in ASYNC thread for request {}: {}", req_id_str, e.what());
            } catch (...) {
                SPDLOG_ERROR("Unknown CRITICAL ERROR in ASYNC thread for request {}", req_id_str);
            }
        });

        publishing_thread.detach();

    } catch (const std::exception& e) {
        SPDLOG_ERROR("Transcoding setup error for request_id {}: {}", req_id_str, e.what());
    }
}


/*===========================================================================*/
// Subscriber thread to perform subscribe
/*===========================================================================*/

void
DoSubscriber(const std::string& root_namespace,
             const std::shared_ptr<MyClient>& client,
             quicr::messages::FilterType filter_type,
             const bool& stop,
             const std::optional<std::uint64_t> join_fetch,
             const bool absolute)
{
    using Fetch = quicr::SubscribeTrackHandler::JoiningFetch;
    const auto joining_fetch = Fetch{ 4, quicr::messages::GroupOrder::kAscending, {}, 0, absolute };

    auto sub_util = std::make_shared<SubscriberUtil>();

    auto cat_ftn = quicr::example::MakeFullTrackName("svc,"+root_namespace, "catalog");

    const auto catalog_track_handler = std::make_shared<CatalogSubscribeTrackHandler>(
      cat_ftn, messages::FilterType::kLargestObject, joining_fetch, sub_util);

    if (client->GetStatus() == MyClient::Status::kReady) {
        SPDLOG_INFO("Subscribing to catalog track");
        client->SubscribeTrack(catalog_track_handler);
    } else {
        SPDLOG_ERROR("Client not ready for subscribing to catalog track");
        return;
    }

    if (!catalog_track_handler->WaitForCatalog(std::chrono::seconds(10))) {
        SPDLOG_ERROR("Catalog timeout");
        return;
    }
    SPDLOG_INFO("Catalog loaded.");

    std::string delta_ns = "svc,"+root_namespace + ",delta," + client->getEndpointID();
    std::string delta_track_name = "data";
    auto delta_ftn = quicr::example::MakeFullTrackName(delta_ns, delta_track_name);

    auto delta_track_handler = std::make_shared<SmartDeltaPublishTrackHandler>(
      delta_ftn, quicr::TrackMode::kStream, 2, 3000);
    delta_track_handler->SetUseAnnounce(true);
    delta_track_handler->SetTrackAlias(5000);
    client->PublishTrack(delta_track_handler);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    client->PublishNamespace(delta_ftn.name_space);


    auto req_ns_prefix = quicr::example::MakeFullTrackName("req,"+root_namespace, "");
    auto request_queue = std::make_shared<TranscodeRequestQueue>();
    client->SetRequestQueue(request_queue);
    client->SetRequestsNamespacePrefix(req_ns_prefix.name_space);

    client->SubscribeNamespace(req_ns_prefix.name_space);

    SPDLOG_INFO("Listening for PUBLISH requests on the moon");

    // 4. Loop to process requests
    while (!stop) {

        try {
            client->CleanupFinishedRequestHandlers();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Error during cleanup of finished request handlers: {}", e.what());
        }

        auto request_opt = request_queue->TryPop(std::chrono::milliseconds(250));

        if (request_opt.has_value()) {
            TranscodeRequest req = std::move(request_opt.value());
            SPDLOG_INFO("Dispatcher: New Request {} - Starting setup synchronously", req.request_id);

            Catalog catalog_snapshot = catalog_track_handler->GetCatalogCopy();

            HandleTranscodeRequest(req,
                                   catalog_snapshot,
                                   delta_track_handler,
                                   client,
                                   root_namespace);
        }
    }

    client->UnsubscribeTrack(catalog_track_handler);
    client->UnpublishTrack(delta_track_handler);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SPDLOG_INFO("Subscriber done track");
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

    if (cli_opts.count("sub_namespace")) {
        enable_sub = true;
        SPDLOG_INFO("Transcoder enabled using track namespace: {0}",
                    cli_opts["sub_namespace"].as<std::string>());
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
    logger = spdlog::stderr_color_mt("console");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::trace);

    SPDLOG_INFO("INFO");
    SPDLOG_WARN("WARN");
    SPDLOG_ERROR("ERROR");
    SPDLOG_DEBUG("DEBUG");
    SPDLOG_TRACE("TRACE");

    int result_code = EXIT_SUCCESS;

    cxxopts::Options options("qclient",
                             std::string("MOQ Example Client using QuicR Version: ") + std::string(QUICR_VERSION));

    options.set_width(75)
      .set_tab_expansion()
      .add_options()
        ("h,help", "Print help")
        ("d,debug", "Enable debugging")
        ("t,trace", "Enable tracing")
        ("v,version", "QuicR Version")
        ("r,url", "Relay URL", cxxopts::value<std::string>()->default_value("moq://localhost:1234"))
        ("e,endpoint_id", "This client endpoint ID", cxxopts::value<std::string>()->default_value("moq-client"))
        ("q,qlog", "Enable qlog using path", cxxopts::value<std::string>())
        ("s,ssl_keylog", "Enable SSL Keylog for transport debugging");

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


    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help({ "", "Publisher", "Subscriber", "Fetcher" }) << std::endl;
        return EXIT_SUCCESS;
    }

    installSignalHandlers();

    std::unique_lock lock(moq_example::main_mutex);

    bool enable_pub{ false };
    bool enable_sub{ false };
    bool enable_fetch{ false };
    bool use_announce{ false };
    quicr::ClientConfig config = InitConfig(result, enable_pub, enable_sub, enable_fetch, use_announce);

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

            sub_thread = std::thread(DoSubscriber,
                                     result["sub_namespace"].as<std::string>(),
                                     client,
                                     filter_type,
                                     std::ref(stop_threads),
                                     joining_fetch,
                                     absolute);
        }

        // Wait until told to terminate
        moq_example::cv.wait(lock, [&]() { return moq_example::terminate; });

        stop_threads = true;
        SPDLOG_ERROR("Stopping threads...");

        if (sub_thread.joinable()) {
            sub_thread.join();
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
