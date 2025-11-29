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
#include <algorithm>
#include <mutex>
#include <vector>
#include <memory>
#include "transcode_client.h"

using namespace quicr;

class TranscodeSubscribeTrackHandler : public quicr::SubscribeTrackHandler
{
    struct ClientContext {
        std::shared_ptr<transcode::TranscodeClient> client;
        bool waiting_for_keyframe;
        std::string id; // Debugginghoz
    };

    std::shared_ptr<SubTrack> track_; 
    
    // MÓDOSÍTÁS: Pointereket tárolunk, hogy másolható legyen a lista a mutexen kívülre
    std::vector<std::shared_ptr<ClientContext>> transcode_clients_;
    std::mutex clients_mutex_;

  public:
    TranscodeSubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                            quicr::messages::FilterType filter_type,
                            const std::optional<JoiningFetch>& joining_fetch,
                            std::shared_ptr<SubTrack> track,
                            bool publisher_initiated = false)
      : SubscribeTrackHandler(full_track_name, 3, quicr::messages::GroupOrder::kAscending, filter_type, joining_fetch, publisher_initiated), 
        track_(track)
    {
    }

    void AddTranscodeClient(std::shared_ptr<transcode::TranscodeClient> client) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto ctx = std::make_shared<ClientContext>();
        ctx->client = client;
        ctx->waiting_for_keyframe = true; // Mindig true-val kezdünk
        ctx->id = std::to_string((uintptr_t)client.get()); // Egyedi ID
        
        transcode_clients_.push_back(ctx);
        SPDLOG_INFO("Added transcode client [{}]. Total: {}", ctx->id, transcode_clients_.size());
    }

    bool RemoveTranscodeClient(std::shared_ptr<transcode::TranscodeClient> client_to_remove) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto new_end = std::remove_if(transcode_clients_.begin(), transcode_clients_.end(),
            [&](const std::shared_ptr<ClientContext>& ctx) {
                return ctx->client == client_to_remove;
            });

        if (new_end != transcode_clients_.end()) {
            transcode_clients_.erase(new_end, transcode_clients_.end());
            SPDLOG_INFO("Removed transcode client. Remaining: {}", transcode_clients_.size());
        }
        return transcode_clients_.empty();
    }

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        if (data.size() > 10 * 1024 * 1024) return;

        // MoQ konvenció: Object 0 a csoport eleje/kulcskép
        bool is_keyframe = (hdr.object_id == 0);

        // 1. LÉPÉS: SNAPSHOT KÉSZÍTÉSE (Mutex csak eddig kell)
        std::vector<std::shared_ptr<ClientContext>> active_clients_snapshot;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (transcode_clients_.empty()) return;
            active_clients_snapshot = transcode_clients_; // Gyors másolás (csak pointerek)
        } 
        // ITT MÁR NINCS MUTEX ZÁROLÁS -> Nem blokkoljuk az Add/Remove hívásokat!

        for (auto& ctx : active_clients_snapshot) {
            if (!ctx->client) continue;

            // Szinkronizáció
            if (ctx->waiting_for_keyframe) {
                if (is_keyframe) {
                    ctx->waiting_for_keyframe = false;
                    SPDLOG_INFO("Client [{}] SYNCED on Group {}, Object {}", ctx->id, hdr.group_id, hdr.object_id);
                } else {
                    // Debug logot kiveheted, ha túl zajos
                    // SPDLOG_TRACE("Client [{}] dropping packet (waiting for Keyframe)", ctx->id);
                    continue; 
                }
            }

            // Adatátadás + Hibakezelés + Időmérés (hogy lássuk, ki a lassú)
            try {
                // Opcionális: Időmérés debug célból
                // auto start = std::chrono::steady_clock::now();
                
                ctx->client->PushInputFragment(data.data(), data.size());
                
                // auto end = std::chrono::steady_clock::now();
                // auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                // if (diff > 10) SPDLOG_WARN("Slow processing on client [{}]: {}ms", ctx->id, diff);

            } catch (const std::exception& e) {
                SPDLOG_ERROR("Client [{}] Error: {}", ctx->id, e.what());
            } catch (...) {
                SPDLOG_ERROR("Client [{}] Unknown Error", ctx->id);
            }
        }
    }

    void StatusChanged(Status status) override {}
};

#endif