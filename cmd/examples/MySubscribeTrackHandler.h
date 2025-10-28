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

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <quicr/publish_fetch_handler.h>

#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../qperf2/inicpp.h"
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
class MySubscribeTrackHandler : public quicr::SubscribeTrackHandler
{

    bool is_catalog_;
    std::shared_ptr<SubscriberUtil> util_; //either util or track has to be set for InitCatalogTrack if "catalog == true"
    std::shared_ptr<SubTrack> track_; //this has to be set with InitMediaTrack for media tracks if "catalog == false"

    std::shared_ptr<SubscriberGst> gst_callback_;

    uint64_t waitfor_Group = 0;


    bool stop_at_newGroup = false;
    uint64_t stop_Group_after = 0;
    bool video_started_ = false;
    std::shared_ptr<std::atomic_bool> unsub_;


  public:

    MySubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                            quicr::messages::FilterType filter_type,
                            const std::optional<JoiningFetch>& joining_fetch,
                            const bool catalog = false)
      : SubscribeTrackHandler(full_track_name, 3, quicr::messages::GroupOrder::kAscending, filter_type, joining_fetch)
    {
        is_catalog_ = catalog;
    }


    uint64_t GetCurrentGroup() const { return current_group_id_;}

    uint64_t StopAtNewGroup(std::shared_ptr<std::atomic_bool> unsub)
    {
        stop_at_newGroup = true;
        stop_Group_after = current_group_id_;
        unsub_ = unsub;
        return current_group_id_;
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
        //setting the gstreamer callback handler
        gst_callback_ = gst;
    }

    void SetWaitForGroup(uint64_t group_after = 0)
    {
        waitfor_Group = group_after;
        stop_at_newGroup = false;
        stop_Group_after = -1;
    }

    ~MySubscribeTrackHandler() override
    {
        data_fs_ << std::endl;
        data_fs_.close();

        moq_fs_ << std::endl;
        moq_fs_.close();
    }

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        std::string s(reinterpret_cast<const char*>(GetFullTrackName().name.data()),
                      GetFullTrackName().name.size());
        //SPDLOG_INFO("Received message on {0}: Group:{1}, Object:{2}", s, hdr.group_id, hdr.object_id);

        if (is_catalog_) {
            // Parse catalog and print to stdout
            try {
                std::string catalog_str(data.begin(), data.end());
                std::cerr << catalog_str << std::endl;
                Catalog catalog;
                catalog.from_json(catalog_str);

                SPDLOG_INFO( "Catalog read with {0} tracks", catalog.tracks().size());
                util_->catalog = std::move(catalog);
                util_->catalog_read = true;
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Failed to parse catalog JSON: {0}", e.what());
                std::cerr << fmt::format("Failed to parse catalog JSON: {0}", e.what()) << std::endl;
            }
            return;
        }

        if (waitfor_Group != 0  && hdr.object_id != 0) {
            SPDLOG_INFO( "Waiting for group after {0}", waitfor_Group);
            return;
        }
        if (waitfor_Group != 0 && hdr.object_id == 0 && hdr.group_id < waitfor_Group) {
                SPDLOG_INFO( "Waiting for group after {0}, current group: {1}", waitfor_Group, hdr.group_id);
                return;
        }
        if (waitfor_Group <= hdr.group_id && hdr.object_id == 0) {
            waitfor_Group = 0;
            SPDLOG_INFO("New group started for track: {0}", track_->track_entry.name);
            if (track_->track_entry.type == "video") {
                gst_callback_->SelectVideo(track_->track_entry.name);
            }
            if (track_->track_entry.type == "audio") {
                gst_callback_->SelectAudio(track_->track_entry.name);
            }
            //gst_callback_->PushInit(track_->track_entry.name, track_->init.data(), track_->init.size());
            SPDLOG_INFO("Pushed init segment for gstream track: {0}", track_->track_entry.name);
        }
        if (stop_at_newGroup == true && hdr.group_id > stop_Group_after) {
            SPDLOG_INFO ("Stopping at group: {0} on track: {1}", stop_Group_after, track_->track_entry.name);
            if (unsub_ != nullptr) {
                unsub_->store(true);
                unsub_->notify_all();
                unsub_ = nullptr;
            }
            return;
        }
        if (track_->track_entry.type == "video") {
            if (!video_started_) {
                // addig dobjuk, amíg nem jön el egy group 0. object-je
                if (hdr.object_id != 0) {
                    return;
                }
                video_started_ = true;
            }
        }

        if (track_->track_entry.type == "video") {
            gst_callback_->VideoPushFragment(data.data(), data.size(), hdr.object_id == 0);
        }
        if (track_->track_entry.type == "audio") {
            gst_callback_->AudioPushFragment(data.data(), data.size(), hdr.object_id == 0);
        }


        {
            std::ofstream out(track_->track_entry.name + ".txt", std::ios::app);
            out << "group: " << hdr.group_id << ", object: " << hdr.object_id << ", data size:" << std::endl << data.size() << std::endl;
        }

        SPDLOG_INFO("Pushed fragment for gstream track: {0}, Group:{1}, Object:{2}", track_->track_entry.name, hdr.group_id, hdr.object_id);
        //std::cout << "Fragment received on "  << track_->track_entry.name << ", group: " << hdr.group_id << ", object: " << hdr.object_id << std::endl;
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kOk: {
                if (auto track_alias = GetTrackAlias(); track_alias.has_value()) {
                    SPDLOG_INFO("Track alias: {0} is ready to read", track_alias.value());
                }
            } break;
            case Status::kError: {}break;
            case Status::kPaused: {}break;

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
