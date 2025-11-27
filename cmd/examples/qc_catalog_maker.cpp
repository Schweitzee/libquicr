//
// qc_catalog_maker.cpp
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <nlohmann/json.hpp>
#include <oss/cxxopts.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <quicr/client.h>
#include <quicr/object.h>
#include <quicr/publish_fetch_handler.h>
#include <quicr/defer.h>

#include "helper_functions.h"
#include "signal_handler.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <deque>
#include <atomic>
#include <optional>
#include <string>

#include "catalog.hpp"
#include "quicr/cache.h"

#include <set>
#include <sys/stat.h>

using json = nlohmann::json;

std::shared_ptr<spdlog::logger> logger;

struct CacheObject
{
    quicr::ObjectHeaders headers;
    quicr::Bytes data;
};

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
    std::optional<uint64_t> track_alias;
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

class CatalogMakerClient;

/**
 * @brief Output Publisher
 * JAVÍTVA: Status::kOk kezelése, hogy az első feliratkozó is megkapja az adatot.
 */
class CatalogMakerCatalogPublisher : public quicr::PublishTrackHandler {
    int group_id_ = 0;
    int object_id_ = 0;
    std::mutex pub_mutex_;
    std::string last_full_catalog_;

public:
    CatalogMakerCatalogPublisher(const quicr::FullTrackName& ftn)
        : quicr::PublishTrackHandler(ftn, quicr::TrackMode::kStream, 2, 15000) {}

    void PublishData(const std::string& payload, bool is_full_snapshot) {
        std::lock_guard<std::mutex> lock(pub_mutex_);

        if (is_full_snapshot && !payload.empty()) {
            last_full_catalog_ = payload;
            object_id_ =0;
            group_id_++;
        }

        std::string data_to_send = payload;
        if (data_to_send.empty() && is_full_snapshot) {
            data_to_send = last_full_catalog_;
        }

        if (data_to_send.empty()) return;

        quicr::ObjectHeaders headers;
        headers.group_id = group_id_;
        headers.object_id = object_id_++;
        headers.payload_length = data_to_send.size();
        headers.status = quicr::ObjectStatus::kAvailable;
        headers.priority = 2;
        headers.ttl = 15000;

        PublishObject(headers, quicr::BytesSpan((uint8_t*)data_to_send.data(), data_to_send.size()));
    }

    PublishObjectStatus PublishObject(const quicr::ObjectHeaders& h, quicr::BytesSpan d) override {
        auto ta = GetTrackAlias();

        if (!qclient_vars::cache.contains(*ta)) {
            qclient_vars::cache.emplace(*ta, quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>{
                static_cast<std::size_t>(qclient_vars::cache_duration_ms.count()), 1000, qclient_vars::tick_service
            });
        }

        CacheObject o{ h, { d.begin(), d.end() } };
        if (auto g = qclient_vars::cache.at(*ta).Get(h.group_id)) {
            g->insert(std::move(o));
        } else {
            qclient_vars::cache.at(*ta).Insert(h.group_id, { std::move(o) }, qclient_vars::cache_duration_ms.count());
        }

        return quicr::PublishTrackHandler::PublishObject(h, d);
    }



    void StatusChanged(Status status) override {
        switch (status) {
            case (Status::kOk): {
                SPDLOG_INFO("CatalogMakerCatalogPublisher is ready to send");
            } break;
            case (Status::kNoSubscribers): {
                SPDLOG_INFO("CatalogMakerCatalogPublisher has no subscribers");
            } break;
            case (Status::kNewGroupRequested): {
                SPDLOG_INFO("CatalogMakerCatalogPublisher has new group request");
            } break;
            case (Status::kSubscriptionUpdated): {
                SPDLOG_INFO("CatalogMakerCatalogPublisher has updated subscription");
            } break;
            case (Status::kPaused): {
                SPDLOG_INFO("CatalogMakerCatalogPublisher is paused");
            } break;
            case (Status::kPendingPublishOk): {
                SPDLOG_INFO("CatalogMakerCatalogPublisher is pending publish ok");
            } break;
            default:
                SPDLOG_INFO("CatalogMakerCatalogPublisher has status {}", static_cast<int>(status));
                break;
        }
    }
};

class CatalogManager {
private:
    std::mutex mutex_;
    Catalog unified_catalog_;
    bool publish_full_updates_ = false;
    int next_transcode_id_ = 1000;
    std::shared_ptr<CatalogMakerCatalogPublisher> publisher_;

public:
    CatalogManager(bool full_updates) : publish_full_updates_(full_updates) {}

    void SetPublisher(std::shared_ptr<CatalogMakerCatalogPublisher> pub) {
        publisher_ = pub;
    }

    std::shared_ptr<CatalogMakerCatalogPublisher> GetPublisher() { return publisher_; }

    void ProcessOriginalCatalog(const std::string& json_content) {
        std::lock_guard<std::mutex> lock(mutex_);
        SPDLOG_INFO("Processing Original Catalog update...");

        try {
            Catalog original;
            original.from_json(json_content);

            std::vector<CatalogTrackEntry> preserved_transcoded_tracks;
            for(const auto& t : unified_catalog_.tracks()) {
                if (t.idx >= 1000) preserved_transcoded_tracks.push_back(t);
            }

            unified_catalog_ = original;

            for(const auto& t : preserved_transcoded_tracks) {
                unified_catalog_.addTrack(t);
            }
            PublishUpdate("");
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to process original catalog: {}", e.what());
        }
    }

    void ProcessTranscoderDelta(const std::string& delta_json, const std::string& source_endpoint_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        SPDLOG_INFO("Processing Delta from Transcoder: {}", source_endpoint_id);

        try {
            json patch = json::parse(delta_json);
            if (!patch.is_array()) return;
            json modified_patch = json::array();
            bool catalog_changed = false;

            for (auto& op : patch) {
                std::string operation = op.value("op", "");
                std::string path = op.value("path", "");

                if (operation == "add" && path == "/tracks/-" && op.contains("value")) {
                    json value = op["value"];
                    if (!value.contains("name") || !value.contains("type") || !value.contains("init_data")) continue;

                    int new_id = next_transcode_id_++;
                    value["index"] = new_id;
                    op["value"] = value;
                    modified_patch.push_back(op);

                    try {
                        json single_op_patch = json::array({op});
                        unified_catalog_.applyDeltaUpdate(single_op_patch.dump());
                        catalog_changed = true;
                        SPDLOG_INFO("Added transcoded track: {} (ID: {})", value["name"].get<std::string>(), new_id);
                    } catch (const std::exception& e) {
                        SPDLOG_ERROR("Failed to apply patch locally: {}", e.what());
                    }
                }
            }
            if (catalog_changed) PublishUpdate(modified_patch.dump());

        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to process delta update: {}", e.what());
        }
    }

    void ResendSnapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (publisher_) publisher_->PublishData(unified_catalog_.to_json(), true);
    }

private:
    void PublishUpdate(const std::string& patch_json) {
        if (!publisher_) return;
        if (publish_full_updates_ || patch_json.empty()) {
            std::string full_json = unified_catalog_.to_json();
            SPDLOG_INFO("Publishing FULL Catalog update (size: {})", full_json.size());
            publisher_->PublishData(full_json, true);
        } else {
            SPDLOG_INFO("Publishing DELTA catalog update (size: {})", patch_json.size());
            publisher_->PublishData(patch_json, false);
        }
    }
};

class OriginalCatalogHandler : public quicr::SubscribeTrackHandler {
    std::shared_ptr<CatalogManager> manager_;
public:
    OriginalCatalogHandler(const quicr::FullTrackName& ftn, std::shared_ptr<CatalogManager> mgr)
        : SubscribeTrackHandler(ftn, 3, quicr::messages::GroupOrder::kAscending, quicr::messages::FilterType::kLargestObject, std::nullopt, false),
          manager_(mgr) {}

    void ObjectReceived(const quicr::ObjectHeaders&, quicr::BytesSpan data) override {
        std::string payload(data.begin(), data.end());
        manager_->ProcessOriginalCatalog(payload);
    }
    void StatusChanged(Status) override {}
};

class DeltaInputHandler : public quicr::SubscribeTrackHandler {
    std::shared_ptr<CatalogManager> manager_;
    std::string endpoint_id_;
public:
    DeltaInputHandler(const quicr::FullTrackName& ftn, std::shared_ptr<CatalogManager> mgr, std::string eid)
        : SubscribeTrackHandler(ftn, 3, quicr::messages::GroupOrder::kAscending, quicr::messages::FilterType::kNextGroupStart, std::nullopt, false),
          manager_(mgr), endpoint_id_(eid) {}

    void ObjectReceived(const quicr::ObjectHeaders&, quicr::BytesSpan data) override {
        std::string payload(data.begin(), data.end());
        manager_->ProcessTranscoderDelta(payload, endpoint_id_);
    }
    void StatusChanged(Status) override {}
};

class CatalogMakerClient : public quicr::Client{
    quicr::ClientConfig config_;
    bool& stop_signal_;
    std::string root_namespace_;
    std::shared_ptr<CatalogManager> manager_;
    std::vector<std::shared_ptr<DeltaInputHandler>> delta_handlers_;
    std::mutex handlers_mutex_;

    CatalogMakerClient(const quicr::ClientConfig& cfg, bool& stop, std::string root_ns, bool full_updates)
        : quicr::Client(cfg), config_(cfg), stop_signal_(stop), root_namespace_(root_ns)
    {
        manager_ = std::make_shared<CatalogManager>(full_updates);
    }

public:
    static std::shared_ptr<CatalogMakerClient> Create(const quicr::ClientConfig& cfg, bool& stop, std::string root_ns, bool full_updates) {
        return std::shared_ptr<CatalogMakerClient>(new CatalogMakerClient(cfg, stop, root_ns, full_updates));
    }

    std::shared_ptr<CatalogManager> GetManager() {return manager_;}

    void Init() {

        auto pub_ftn = quicr::example::MakeFullTrackName("svc,"+root_namespace_, "catalog");
        auto pub_handler = std::make_shared<CatalogMakerCatalogPublisher>(pub_ftn);
        manager_->SetPublisher(pub_handler);
        PublishNamespace(pub_ftn.name_space);
        pub_handler->SetTrackAlias(1000);
        PublishTrack(pub_handler);
        pub_handler->PublishData("", true);

        SPDLOG_INFO("Publishing to: {}", pub_ftn.name_space.ToString() +"-catalog");

        // Feliratkozás az eredetire (ez marad bbb/catalog)
        auto orig_ftn = quicr::example::MakeFullTrackName(root_namespace_, "catalog");
        auto orig_handler = std::make_shared<OriginalCatalogHandler>(orig_ftn, manager_);
        SubscribeTrack(orig_handler);

        auto delta_namespace = quicr::example::MakeFullTrackName("svc,"+root_namespace_ + ",delta", "");

        SubscribeNamespace(delta_namespace.name_space);
        SPDLOG_INFO("Listening for publish on {}", pub_ftn.name_space.ToString());
    }


    void FetchReceived(quicr::ConnectionHandle connection_handle, uint64_t request_id, const quicr::FullTrackName& track_full_name,
                       quicr::messages::SubscriberPriority priority, quicr::messages::GroupOrder group_order,
                       quicr::messages::Location start, std::optional<quicr::messages::Location> end)
    {
        auto th = quicr::TrackHash(track_full_name);
        // Megnézzük, van-e adat a cache-ben
        auto cache_entry_it = qclient_vars::cache.find(th.track_fullname_hash);

        if (cache_entry_it == qclient_vars::cache.end()) {
            ResolveFetch(connection_handle, request_id, priority, group_order, { quicr::FetchResponse::ReasonCode::kNoObjects, std::nullopt, std::nullopt });
            return;
        }

        auto& [_, cache] = *cache_entry_it;
        // Lekérjük a kért tartományt
        const auto& cache_entries = cache.Get(start.group, end.has_value() && end->group != 0 ? end->group : cache.Size());

        if (cache_entries.empty()) {
             ResolveFetch(connection_handle, request_id, priority, group_order, { quicr::FetchResponse::ReasonCode::kNoObjects, std::nullopt, std::nullopt });
             return;
        }

        // Elfogadjuk a kérést (OK)
        ResolveFetch(connection_handle, request_id, priority, group_order, { quicr::FetchResponse::ReasonCode::kOk, std::nullopt, std::nullopt });

        // Külön szálon visszaküldjük a tárolt adatokat
        auto pub_fetch_h = quicr::PublishFetchHandler::Create(track_full_name, priority, request_id, group_order, 50000);
        BindFetchTrack(connection_handle, pub_fetch_h);

        std::thread retrieve_cache_thread([=, cache_entries = std::move(cache_entries), this] {
            defer(UnbindFetchTrack(connection_handle, pub_fetch_h));
            for (const auto& entry : cache_entries) {
                for (const auto& object : *entry) {
                    // Ha van végpont megadva, ellenőrizzük
                    if (end.has_value() && end->object && object.headers.group_id == end->group && object.headers.object_id >= end->object) return;

                    SPDLOG_INFO("Serving Fetch Request: Group {}, Object {}", object.headers.group_id, object.headers.object_id);
                    pub_fetch_h->PublishObject(object.headers, object.data);
                }
            }
        });
        retrieve_cache_thread.detach();
    }

    void StandaloneFetchReceived(quicr::ConnectionHandle ch, uint64_t rid, const quicr::FullTrackName& ftn, const quicr::messages::StandaloneFetchAttributes& attr) override {
        FetchReceived(ch, rid, ftn, attr.priority, attr.group_order, attr.start_location, attr.end_location);
    }

    void JoiningFetchReceived(quicr::ConnectionHandle ch, uint64_t rid, const quicr::FullTrackName& ftn, const quicr::messages::JoiningFetchAttributes& attr) override {
        // Joining fetch: A kért ponttól kezdve mindent küldünk
        FetchReceived(ch, rid, ftn, attr.priority, attr.group_order, { attr.joining_start, 0 }, std::nullopt);
    }

    void StatusChanged(Status status) override {
        if (status == Status::kReady) SPDLOG_INFO("Client Connected and Ready");
    }

    // ÚJ IMPLEMENTÁCIÓ
    void PublishNamespaceReceived(const quicr::TrackNamespace& track_namespace,
                                  const quicr::PublishNamespaceAttributes&) override
    {
        std::string ns_str = track_namespace.ToString();
        std::string my_root = "svc," + root_namespace_ + ",delta";
        // 1. Ellenőrizzük, hogy a "mi" névterünkben történt-e (bbb,catalog_maker)
        if (ns_str.find(my_root) == 0) {

            // 2. Szűrjük ki saját magunkat és a gyökeret
            // Ha a kapott névtér PONTOSAN a gyökér, azt hagyjuk (azt mi hirdettük vagy a relay visszhangozza)
            if (ns_str == my_root) return;

            // Ha ez egy al-névtér (pl. bbb,catalog_maker,transcoder_1), akkor az egy transzkódoló
            SPDLOG_INFO("New Transcoder Detected: {}", ns_str);

            // A szabály szerint a track neve mindig "data"
            auto ftn = quicr::example::MakeFullTrackName(ns_str, "data");

            // Endpoint ID kinyerése a névtér végéből (opcionális, logoláshoz)
            std::string endpoint_id = ns_str.substr(my_root.length() + 1); // +1 a vessző miatt

            auto handler = std::make_shared<DeltaInputHandler>(ftn, manager_, endpoint_id);

            // Feliratkozás a "data" trackre
            SubscribeTrack(handler);

            {
                std::lock_guard<std::mutex> lock(handlers_mutex_);
                delta_handlers_.push_back(handler);
            }
        }
    }

    // A RÉGI PublishReceived-et ürítsd ki (vagy csak hagyd meg a NotSupported választ),
    // mert már nem track push-al jön az adat.
    void PublishReceived(quicr::ConnectionHandle ch, uint64_t rid, const quicr::messages::PublishAttributes& pa) override {
        ResolvePublish(ch, rid, pa, { .reason_code = quicr::PublishResponse::ReasonCode::kNotSupported });
    }
    void PublishNamespaceDoneReceived(const quicr::TrackNamespace&) override {}
    void SubscribeNamespaceStatusChanged(const quicr::TrackNamespace&, std::optional<quicr::messages::SubscribeNamespaceErrorCode>, std::optional<quicr::messages::ReasonPhrase>) override {}
    void TrackStatusResponseReceived(quicr::ConnectionHandle, uint64_t, const quicr::SubscribeResponse&) override {}

};

quicr::ClientConfig InitConfig(cxxopts::ParseResult& cli_opts) {
    quicr::ClientConfig config;
    config.endpoint_id = cli_opts["endpoint_id"].as<std::string>();
    config.connect_uri = cli_opts["url"].as<std::string>();
    config.transport_config.debug = cli_opts.count("debug") ? cli_opts["debug"].as<bool>() : false;
    return config;
}

int main(int argc, char* argv[]) {
    logger = spdlog::stderr_color_mt("console");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::trace);

    cxxopts::Options options("catalog_maker", "MoQ Catalog Maker Service");
    options.add_options()
        ("h,help", "Print help")
        ("d,debug", "Enable debugging")
        ("r,url", "Relay URL", cxxopts::value<std::string>()->default_value("moq://localhost:1234"))
        ("e,endpoint_id", "Endpoint ID", cxxopts::value<std::string>()->default_value("catalog-maker"))
        ("n,namespace", "Root Namespace (e.g. IB027)", cxxopts::value<std::string>()->default_value("bbb"))
        ("f,full_updates", "Always publish full catalog updates instead of deltas", cxxopts::value<bool>()->default_value("false"));

    auto result = options.parse(argc, argv);
    if (result.count("help")) { std::cout << options.help() << std::endl; return 0; }

    installSignalHandlers();
    quicr::ClientConfig config = InitConfig(result);
    config.transport_config.time_queue_max_duration = 15000;
    bool stop_threads = false;
    std::string root_ns = result["namespace"].as<std::string>();
    bool full_updates = result["full_updates"].as<bool>();

    auto client = CatalogMakerClient::Create(config, stop_threads, root_ns, full_updates);
    if (client->Connect() != quicr::Transport::Status::kConnecting) return -1;

    while (!stop_threads) {
        if (client->GetStatus() == quicr::Client::Status::kReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    client->Init();
    SPDLOG_INFO("Catalog Maker Service Started");

    while(!moq_example::terminate) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    client->Disconnect();
    return 0;
}