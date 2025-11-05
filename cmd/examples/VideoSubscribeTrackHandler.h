//
// Created by schweitzer on 2025. 10. 18..
//

#ifndef QUICR_MYSUBSCRIBETRACKHANDLER_H
#define QUICR_MYSUBSCRIBETRACKHANDLER_H

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

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <quicr/publish_fetch_handler.h>

#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "base64_tool.h"
#include "ffmpeg_cmaf_splitter.hpp"
#include "media.h"
#include "subscriber_util.h"

#include <optional>

using namespace quicr;

/**
 * @brief  Subscribe track handler
 * @details Subscribe track handler used for the subscribe command line option.
 */
class VideoSubscribeTrackHandler : public quicr::SubscribeTrackHandler
{

    bool is_catalog_;
    std::shared_ptr<SubscriberUtil>
      util_;                          // either util or track has to be set for InitCatalogTrack if "catalog == true"
    std::shared_ptr<SubTrack> track_; // this has to be set with InitMediaTrack for media tracks if "catalog == false"

    std::shared_ptr<SubscriberGst> gst_callback_;

    bool need_init = false;
    std::shared_ptr<std::atomic_bool> start_notify = nullptr;
    std::shared_ptr<std::atomic_bool> stop_notify = nullptr;

  public:
    VideoSubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                            quicr::messages::FilterType filter_type,
                            const std::optional<JoiningFetch>& joining_fetch,
                            const bool catalog = false)
      : SubscribeTrackHandler(full_track_name, 3, quicr::messages::GroupOrder::kAscending, filter_type, joining_fetch)
    {
        is_catalog_ = catalog;
    }


    void InitCatalogTrack(std::shared_ptr<SubscriberUtil> util)
    {
        if (is_catalog_ == false) {
            throw std::runtime_error("InitCatalogTrack called on non-catalog track");
        }
        util_ = util;
    }

    void InitMediaTrack(std::shared_ptr<SubTrack> track)
    {
        if (is_catalog_ == true) {
            throw std::runtime_error("InitMediaTrack called on catalog track");
        }
        track_ = track;
    }

    void SetSubscribeGst(const std::shared_ptr<SubscriberGst>& gst)
    {
        // setting the gstreamer callback handler
        gst_callback_ = gst;
    }

    ~VideoSubscribeTrackHandler() override
    {
        data_fs_ << std::endl;
        data_fs_.close();

        moq_fs_ << std::endl;
        moq_fs_.close();
    }

    void SetNeedInit()
    {
        need_init = true;
    }

    void SetStartNotify(std::shared_ptr<std::atomic_bool> start)
    {
        start_notify = start;
        stop_notify = nullptr;
    }

    void SetStopNotify(std::shared_ptr<std::atomic_bool> stop)
    {
        stop_notify = stop;
        //start_notify = nullptr;
    }

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        if (stop_notify != nullptr) {
            if (stop_notify->load(std::memory_order_acquire) == true) {
                SPDLOG_INFO("Dropped fragment during track change");
                return;
            }
        }

        std::string s(reinterpret_cast<const char*>(GetFullTrackName().name.data()), GetFullTrackName().name.size());
        // SPDLOG_INFO("Received message on {0}: Group:{1}, Object:{2}", s, hdr.group_id, hdr.object_id);

        if (is_catalog_) {
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
            return;
        }

        const bool is_video = (track_->track_entry.type == "video");
        const bool is_audio = (track_->track_entry.type == "audio");
        const bool is_group0 = (hdr.object_id == 0);

        if (need_init && is_group0 && start_notify != nullptr) {
            start_notify->store(true, std::memory_order_release);
            SPDLOG_INFO("Stop notify set TRUE");
            start_notify->notify_all();
            start_notify = nullptr;
            need_init = false;
            if (is_video) {
                gst_callback_->SelectVideo(track_->track_entry.name);
            }
            if (is_audio) {
                gst_callback_->SelectAudio(track_->track_entry.name);
            }
            SPDLOG_INFO("TRACK CHANGED-------------");
        }
        if (need_init && is_group0 == false) {
            SPDLOG_INFO("Dropped fragment during track change");
            return;
        }

        if (stop_notify != nullptr) {
            if (stop_notify->load(std::memory_order_acquire) == true) {
                SPDLOG_INFO("Dropped fragment during track change");
                return;
            }
        }

        if (is_video) {
            gst_callback_->VideoPushFragment(data.data(), data.size(), is_group0);
        } else if (is_audio) {
            gst_callback_->AudioPushFragment(data.data(), data.size(), is_group0);
        }

        {
            std::ofstream out(track_->track_entry.name + ".txt", std::ios::app);
            out << "group: " << hdr.group_id << ", object: " << hdr.object_id
                << ", data size:\n" << data.size() << std::endl;
        }

        SPDLOG_INFO("Pushed fragment for gstream track: {}, Group:{}, Object:{}",
                    track_->track_entry.name, hdr.group_id, hdr.object_id);
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

#endif // QUICR_MYSUBSCRIBETRACKHANDLER_H
