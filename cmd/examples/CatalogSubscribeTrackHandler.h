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

#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "base64_tool.h"
#include "media.h"
#include "subscriber_util.h"

#include <optional>

using namespace quicr;

/**
 * @brief  Subscribe track handler
 * @details Subscribe track handler used for the subscribe command line option.
 */
class CatalogSubscribeTrackHandler : public quicr::SubscribeTrackHandler
{
    std::shared_ptr<SubscriberUtil> util_; // either util or track has to be set for InitCatalogTrack if "catalog == true"

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
                              publisher_initiated), util_(util)
    {
    }

    ~CatalogSubscribeTrackHandler() override {}

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        std::string s(reinterpret_cast<const char*>(GetFullTrackName().name.data()), GetFullTrackName().name.size());
        // SPDLOG_INFO("Received message on {0}: Group:{1}, Object:{2}", s, hdr.group_id, hdr.object_id);
        SPDLOG_INFO(
          "Received object on {0}: Group:{1}, Object:{2}, Size:{3} bytes", s, hdr.group_id, hdr.object_id, data.size());

        // Parse catalog and print to stdout
        try {
            std::string catalog_str(data.begin(), data.end());
            std::cerr << catalog_str << std::endl;
            Catalog catalog;
            catalog.from_json(catalog_str);

            SPDLOG_INFO("Catalog read with {0} tracks", catalog.tracks().size());
            util_->catalog = std::move(catalog);
            util_->catalog_read = true;
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to parse catalog JSON: {0}", e.what());
            std::cerr << fmt::format("Failed to parse catalog JSON: {0}", e.what()) << std::endl;
        }
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
            } break;
            case Status::kPaused: {
            } break;

            default:
                break;
        }
    }

  private:
    std::ofstream data_fs_;
    std::fstream moq_fs_;
    bool new_group_requested_ = false;
};

#endif // QUICR_CATALOGSUBSCRIBETRACKHANDLER_H
