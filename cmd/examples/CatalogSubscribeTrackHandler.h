//
// Created by schweitzer on 2025. 11. 16..
//

#ifndef QUICR_CATALOGSUBSCRIBETRACKHANDLER_H
#define QUICR_CATALOGSUBSCRIBETRACKHANDLER_H

#pragma once
#include <nlohmann/json.hpp>
#include <oss/cxxopts.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <quicr/client.h>
#include <quicr/object.h>

#include "helper_functions.h"

#include <filesystem>
#include <fstream>

#include <quicr/publish_fetch_handler.h>

#include <atomic>
#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "base64_tool.h"
#include "catalog.hpp"
#include "media.h"
#include "subscriber_util.h"

#include <optional>

using namespace quicr;

/**
 * @brief  Subscribe track handler for catalog with delta update support
 * @details This handler processes both initial catalog (JSON) and delta updates (JSON Patch).
 *          It provides thread-safe access to the catalog and notifies waiting threads of updates.
 */
class CatalogSubscribeTrackHandler : public quicr::SubscribeTrackHandler
{
  private:
    std::shared_ptr<SubscriberUtil> util_;

    // Thread synchronization for catalog updates
    mutable std::mutex catalog_mutex_;
    std::condition_variable catalog_cv_;
    std::atomic<bool> catalog_updated_{ false };

    // Track first catalog vs delta updates
    bool initial_catalog_received_{ false };

  public:
    CatalogSubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                                 quicr::messages::FilterType filter_type,
                                 const std::optional<JoiningFetch>& joining_fetch,
                                 std::shared_ptr<SubscriberUtil> util,
                                 bool publisher_initiated = false)
      : SubscribeTrackHandler(full_track_name,
                              3,
                              quicr::messages::GroupOrder::kAscending,
                              filter_type,
                              joining_fetch,
                              publisher_initiated)
      , util_(util)
    {
    }

    ~CatalogSubscribeTrackHandler() override {}

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        std::string s(reinterpret_cast<const char*>(GetFullTrackName().name.data()), GetFullTrackName().name.size());
        SPDLOG_INFO(
          "Received catalog object on {0}: Group:{1}, Object:{2}, Size:{3} bytes", s, hdr.group_id, hdr.object_id, data.size());

        try {
            std::string catalog_str(data.begin(), data.end());

            std::lock_guard<std::mutex> lock(catalog_mutex_);

            if (!initial_catalog_received_) {
                // First object is the full catalog
                SPDLOG_INFO("Processing initial catalog");
                ProcessInitialCatalog(catalog_str);
                initial_catalog_received_ = true;
                util_->catalog_read = true;
                catalog_updated_.store(true);
                catalog_cv_.notify_all();
            } else {
                // Subsequent objects are delta updates (JSON Patch)
                SPDLOG_INFO("Processing catalog delta update");
                ProcessDeltaUpdate(catalog_str);
                catalog_updated_.store(true);
                catalog_cv_.notify_all();
            }

        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to process catalog data: {0}", e.what());
            std::cerr << fmt::format("Failed to process catalog data: {0}", e.what()) << std::endl;
        }
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kOk: {
                if (auto track_alias = GetTrackAlias(); track_alias.has_value()) {
                    SPDLOG_INFO("Catalog track alias: {0} is ready to read", track_alias.value());
                }
            } break;
            case Status::kError: {
                SPDLOG_ERROR("Catalog track error");
            } break;
            case Status::kPaused: {
                SPDLOG_WARN("Catalog track paused");
            } break;

            default:
                break;
        }
    }

    /**
     * @brief Wait for catalog to be ready (initial catalog received)
     * @param timeout_ms Maximum time to wait in milliseconds (0 = wait forever)
     * @return true if catalog is ready, false if timeout
     */
    bool WaitForCatalog(std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(0))
    {
        if (util_->catalog_read) {
            return true;
        }

        std::unique_lock<std::mutex> lock(catalog_mutex_);

        if (timeout_ms.count() == 0) {
            catalog_cv_.wait(lock, [this] { return util_->catalog_read.load(); });
            return true;
        } else {
            return catalog_cv_.wait_for(lock, timeout_ms, [this] { return util_->catalog_read.load(); });
        }
    }

    /**
     * @brief Wait for a catalog update
     * @return true if an update was received
     */
    bool WaitForUpdate()
    {
        std::unique_lock<std::mutex> lock(catalog_mutex_);
        catalog_updated_.store(false);
        catalog_cv_.wait(lock, [this] { return catalog_updated_.load(); });
        return true;
    }

    /**
     * @brief Get a thread-safe copy of the catalog
     * @return Copy of the current catalog
     */
    Catalog GetCatalogCopy() const
    {
        std::lock_guard<std::mutex> lock(catalog_mutex_);
        return util_->catalog;
    }

  private:
    void ProcessInitialCatalog(const std::string& catalog_json)
    {
        // Debug output
        std::cerr << "Initial Catalog JSON:" << std::endl;
        std::cerr << catalog_json << std::endl;

        Catalog catalog;
        catalog.from_json(catalog_json);

        SPDLOG_INFO("Initial catalog read with {0} tracks", catalog.tracks().size());

        util_->catalog = std::move(catalog);
    }

    void ProcessDeltaUpdate(const std::string& patch_json)
    {
        // Debug output
        std::cerr << "Delta Update JSON:" << std::endl;
        std::cerr << patch_json << std::endl;

        // Apply the delta patch to the current catalog
        util_->catalog.applyDeltaUpdate(patch_json);

        SPDLOG_INFO("Delta update applied, catalog now has {0} tracks", util_->catalog.tracks().size());
    }
};

#endif // QUICR_CATALOGSUBSCRIBETRACKHANDLER_H
