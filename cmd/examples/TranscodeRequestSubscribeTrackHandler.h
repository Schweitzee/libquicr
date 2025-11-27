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
 * @brief Handler that receives a single request object, queues it, and unsubscribes.
 */
class TranscodeRequestSubscribeHandler : public quicr::SubscribeTrackHandler
{
  private:
    std::shared_ptr<TranscodeRequestQueue> request_queue_;
    std::atomic<bool> is_done_{ false }; // Flag to signal completion

  public:
    TranscodeRequestSubscribeHandler(const quicr::FullTrackName& full_track_name,
                                     const std::shared_ptr<TranscodeRequestQueue>& request_queue,
                                     bool publisher_initiated = true) // publisher_initiated = true fontos itt!
      : SubscribeTrackHandler(full_track_name,
                              3,
                              quicr::messages::GroupOrder::kAscending,
                              quicr::messages::FilterType::kLargestObject,
                              std::nullopt,
                              publisher_initiated)
      , request_queue_(request_queue)
    {
    }

    bool IsDone() const { return is_done_.load(); }

    ~TranscodeRequestSubscribeHandler() override {
    }

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        std::string track_name(reinterpret_cast<const char*>(GetFullTrackName().name.data()),
                               GetFullTrackName().name.size());

        SPDLOG_INFO("Received transcode request object on {0}: Size:{1} bytes", track_name, data.size());

        try {
            std::string request_json(data.begin(), data.end());
            SPDLOG_DEBUG("Request JSON: {}", request_json);

            TranscodeRequest request = parse_transcode_request(request_json);
            SPDLOG_INFO("Parsed request: id={}", request.request_id);

            request_queue_->Push(std::move(request));
            is_done_.store(true);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to parse/queue request: {}", e.what());
            is_done_.store(true);
        }
    }

    void StatusChanged(Status status) override {}
};

#endif // QUICR_TRANSCODEREQUESTSUBSCRIBEHANDLER_H