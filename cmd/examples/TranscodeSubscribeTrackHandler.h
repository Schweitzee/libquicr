//
// Created by schweitzer on 2025. 11. 10..
//

#ifndef QUICR_TRANSCODESUBTRACKHANDLER_H
#define QUICR_TRANSCODESUBTRACKHANDLER_H


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
#include "transcode_client.h"

#include <optional>

using namespace quicr;

/**
 * @brief  Subscribe track handler
 * @details Subscribe track handler used for the subscribe command line option.
 */
class TranscodeSubscribeTrackHandler : public quicr::SubscribeTrackHandler
{

    std::shared_ptr<SubTrack> track_; // this has to be set with InitMediaTrack for media tracks if "catalog == false"
    std::shared_ptr<transcode::TranscodeClient> transcode_client_;


  public:
    TranscodeSubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                            quicr::messages::FilterType filter_type,
                            const std::optional<JoiningFetch>& joining_fetch,
			    std::shared_ptr<SubTrack> track,
			    std::shared_ptr<transcode::TranscodeClient> transcode_client,
                            bool publisher_initiated = false)
      : SubscribeTrackHandler(full_track_name,
                              3,
                              quicr::messages::GroupOrder::kAscending,
                              filter_type,
                              joining_fetch,
                              publisher_initiated), track_(track), transcode_client_(transcode_client)
    {
    }

    ~TranscodeSubscribeTrackHandler() override
    {
        data_fs_ << std::endl;
        data_fs_.close();

        moq_fs_ << std::endl;
        moq_fs_.close();
    }


void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        try {

            // 2. Név lekérése és ellenőrzése
            auto ftn = GetFullTrackName();
            size_t name_size = ftn.name.size();
            const uint8_t* name_ptr = ftn.name.data();


            std::string s;
            if (name_size > 1000) {
                 std::cerr << "[ERROR] Track name size is suspiciously large: " << name_size << std::endl;
                 s = "<INVALID_SIZE>";
            } else if (name_size > 0 && name_ptr != nullptr) {
                // Biztonságos másolás
                s.assign(ftn.name.begin(), ftn.name.end());
            } else {
                s = "<empty_or_null>";
            }

            // 3. Logolás
            SPDLOG_INFO("Received object on {0}: Group:{1}, Object:{2}, Size:{3} bytes",
                        s, hdr.group_id, hdr.object_id, data.size());

            // 4. Adat ellenőrzése
            std::span<const uint8_t> data_span = data;
            size_t data_len = data_span.size();

            if (data_len > 10 * 1024 * 1024) { // Pl. 10MB limit
                 SPDLOG_ERROR("Received suspiciously large data packet: {} bytes", data_len);
                 return;
            }

            if (transcode_client_) {
                // Itt is elkapjuk, ha a ringbuffer dobna hibát
                transcode_client_->PushInputFragment(data_span.data(), data_len);
            } else {
                SPDLOG_WARN("Transcode client is null in ObjectReceived");
            }

        } catch (const std::length_error& le) {
            std::cerr << "[CRITICAL] std::length_error in ObjectReceived: " << le.what() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[CRITICAL] Exception in ObjectReceived: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[CRITICAL] Unknown exception in ObjectReceived" << std::endl;
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

#endif // QUICR_MYSUBSCRIBETRACKHANDLER_H
