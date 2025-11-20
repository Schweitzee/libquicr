//
// Transcode Request Subscribe Handler
//

#ifndef QUICR_TRANSCODEREQUESTSUBSCRIBEHANDLER_H
#define QUICR_TRANSCODEREQUESTSUBSCRIBEHANDLER_H

#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include <nlohmann/json.hpp>
#include <quicr/client.h>
#include <quicr/object.h>
#include <spdlog/spdlog.h>

#include "transcode_request.h"

using namespace quicr;

/**
 * @brief Thread-safe queue for transcode requests
 */
class TranscodeRequestQueue
{
  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<TranscodeRequest> queue_;

  public:
    /**
     * @brief Push a request to the queue
     */
    void Push(TranscodeRequest request)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(request));
        cv_.notify_one();
    }

    /**
     * @brief Pop a request from the queue (blocking)
     * @return transcode request
     */
    TranscodeRequest Pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });

        TranscodeRequest request = std::move(queue_.front());
        queue_.pop_front();
        return request;
    }

    /**
     * @brief Try to pop a request with timeout
     * @param timeout_ms timeout in milliseconds
     * @return Optional transcode request
     */
    std::optional<TranscodeRequest> TryPop(std::chrono::milliseconds timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (!cv_.wait_for(lock, timeout_ms, [this] { return !queue_.empty(); })) {
            return std::nullopt;
        }

        TranscodeRequest request = std::move(queue_.front());
        queue_.pop_front();
        return request;
    }

    /**
     * @brief Check if queue is empty
     */
    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * @brief Get queue size
     */
    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
};

/**
 * @brief Subscribe track handler for transcode requests
 * @details This handler receives transcode request objects, parses them,
 *          and pushes them into a thread-safe queue for processing.
 */
class TranscodeRequestSubscribeHandler : public quicr::SubscribeTrackHandler
{
  private:
    std::shared_ptr<TranscodeRequestQueue> request_queue_;

  public:
    TranscodeRequestSubscribeHandler(const quicr::FullTrackName& full_track_name,
                                     std::shared_ptr<TranscodeRequestQueue> request_queue,
                                     quicr::messages::FilterType filter_type =
                                       quicr::messages::FilterType::kLargestObject,
                                     const std::optional<JoiningFetch>& joining_fetch = std::nullopt,
                                     bool publisher_initiated = false)
      : SubscribeTrackHandler(full_track_name,
                              3,
                              quicr::messages::GroupOrder::kAscending,
                              filter_type,
                              joining_fetch,
                              publisher_initiated)
      , request_queue_(request_queue)
    {
        if (!request_queue_) {
            throw std::invalid_argument("TranscodeRequestSubscribeHandler: request_queue cannot be null");
        }
    }

    ~TranscodeRequestSubscribeHandler() override {}

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        std::string track_name(reinterpret_cast<const char*>(GetFullTrackName().name.data()),
                               GetFullTrackName().name.size());

        SPDLOG_INFO("Received transcode request on {0}: Group:{1}, Object:{2}, Size:{3} bytes",
                    track_name,
                    hdr.group_id,
                    hdr.object_id,
                    data.size());

        try {
            // Parse the JSON request
            std::string request_json(data.begin(), data.end());

            // Debug output
            SPDLOG_DEBUG("Transcode request JSON: {}", request_json);

            // Parse into TranscodeRequest structure
            TranscodeRequest request = parse_transcode_request(request_json);

            SPDLOG_INFO("Parsed transcode request: request_id={}, client_id={}, source_track={}, operations={}",
                        request.request_id,
                        request.client_id,
                        request.source.track,
                        request.operations.size());

            // Push to queue
            request_queue_->Push(std::move(request));

            SPDLOG_INFO("Transcode request queued. Queue size: {}", request_queue_->Size());

        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to parse transcode request: {}", e.what());
            std::cerr << fmt::format("Failed to parse transcode request: {}", e.what()) << std::endl;
        }
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kOk: {
                if (auto track_alias = GetTrackAlias(); track_alias.has_value()) {
                    SPDLOG_INFO("Transcode request track alias: {0} is ready to read", track_alias.value());
                }
            } break;
            case Status::kError: {
                SPDLOG_ERROR("Transcode request track error");
            } break;
            case Status::kPaused: {
                SPDLOG_WARN("Transcode request track paused");
            } break;
            default:
                break;
        }
    }
};

#endif // QUICR_TRANSCODEREQUESTSUBSCRIBEHANDLER_H
