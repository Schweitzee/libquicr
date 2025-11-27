//
// qc_request.cpp
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

#include "helper_functions.h"
#include "signal_handler.h"

#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <optional>
#include <string>
#include <random>

#include "CatalogSubscribeTrackHandler.h"
#include "VideoSubscribeTrackHandler.h"
#include "base64_tool.h"
#include "catalog.hpp"
#include "quicr/cache.h"
#include "subscriber_util.h"
#include "transcode_request.h"

#include <set>

std::shared_ptr<spdlog::logger> logger;


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
    std::unordered_map<quicr::messages::TrackAlias, quicr::Cache<quicr::messages::GroupId, std::set<CacheObject>>>
      cache;
    std::shared_ptr<quicr::ThreadedTickService> tick_service = std::make_shared<quicr::ThreadedTickService>();

}

// Globális GStreamer példány
static std::shared_ptr<SubscriberGst> g_gst = std::make_shared<SubscriberGst>();

static void GstInitOnce() {
    gst_init(nullptr, nullptr);
    static bool inited = false;
    if (inited) return;
    if (!g_gst->BuildPipelines()) {
        std::cerr << "Failed to build base GStreamer pipeline\n";
    }
    inited = true;
}

// --- Request Publisher Handler ---
class RequestPublishHandler : public quicr::PublishTrackHandler {
public:
    RequestPublishHandler(const quicr::FullTrackName& ftn)
        : quicr::PublishTrackHandler(ftn, quicr::TrackMode::kStream, 2, 15000) {}

    void StatusChanged(Status status) override {
        SPDLOG_INFO("Request Publish Status: {}", static_cast<int>(status));
    }

    PublishObjectStatus PublishObject(const quicr::ObjectHeaders& object_headers, quicr::BytesSpan data) override
    {
        auto track_alias = GetTrackAlias();

        // Cache Object
        if (!qclient_vars::cache.contains(*track_alias)) {
            qclient_vars::cache.emplace(
              *track_alias,
              Cache<quicr::messages::GroupId, std::set<CacheObject>>{
                static_cast<std::size_t>(qclient_vars::cache_duration_ms.count()), 1000, qclient_vars::tick_service });
        }

        CacheObject object{ object_headers, { data.begin(), data.end() } };

        if (auto group = qclient_vars::cache.at(*track_alias).Get(object_headers.group_id)) {
            group->insert(std::move(object));
        } else {
            qclient_vars::cache.at(*track_alias)
              .Insert(object_headers.group_id, { std::move(object) }, qclient_vars ::cache_duration_ms.count());
        }

        return quicr::PublishTrackHandler::PublishObject(object_headers, data);
    }
};

// --- Video Track Subscriber Logic ---
// A transzkódolt videó stream fogadására
class TranscodedVideoHandler : public quicr::SubscribeTrackHandler {
    std::shared_ptr<SubscriberGst> gst_;
    std::string track_name_;
    bool is_video_;

public:
    TranscodedVideoHandler(const quicr::FullTrackName& ftn,
                           std::shared_ptr<SubscriberGst> gst,
                           const std::string& track_name,
                           bool is_video)
        : SubscribeTrackHandler(ftn, 3, quicr::messages::GroupOrder::kAscending, quicr::messages::FilterType::kNextGroupStart, std::nullopt, false),
          gst_(gst), track_name_(track_name), is_video_(is_video) {}

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override {
        bool is_rap = (hdr.object_id == 0); // Feltételezés: object_id 0 mindig kulcskép/RAP
        if (is_video_) {
            gst_->VideoPushFragment(data.data(), data.size(), is_rap);
        } else {
            gst_->AudioPushFragment(data.data(), data.size(), is_rap);
        }
        // JAVÍTÁS: Explicit méretkiírás logoláshoz
        SPDLOG_DEBUG("Received media fragment: {} bytes", (size_t)data.size());
    }

    void StatusChanged(Status status) override {
        if (status == Status::kOk) {
            SPDLOG_INFO("Subscribed to media track: {}", track_name_);
            // Amikor sikeres a feliratkozás, "kiválasztjuk" a sávot a GStreamerben
            if (is_video_) gst_->SelectVideo(track_name_);
            else gst_->SelectAudio(track_name_);
        }
    }


};

// --- Main Client ---
class RequestClient : public quicr::Client {
    quicr::ClientConfig config_;
    bool& stop_signal_;

public:
    static std::shared_ptr<RequestClient> Create(const quicr::ClientConfig& cfg, bool& stop) {
        return std::shared_ptr<RequestClient>(new RequestClient(cfg, stop));
    }

    RequestClient(const quicr::ClientConfig& cfg, bool& stop)
        : quicr::Client(cfg), config_(cfg), stop_signal_(stop) {}

    void StatusChanged(Status status) override {
        if (status == Status::kReady) SPDLOG_INFO("Client Connected");
    }

    // Kötelező üres override-ok
    void PublishNamespaceReceived(const quicr::TrackNamespace&, const quicr::PublishNamespaceAttributes&) override {}
    void PublishNamespaceDoneReceived(const quicr::TrackNamespace&) override {}
    void SubscribeNamespaceStatusChanged(const quicr::TrackNamespace&, std::optional<quicr::messages::SubscribeNamespaceErrorCode>, std::optional<quicr::messages::ReasonPhrase>) override {}
    void TrackStatusResponseReceived(quicr::ConnectionHandle, uint64_t, const quicr::SubscribeResponse&) override {}
    void PublishReceived(quicr::ConnectionHandle, uint64_t, const quicr::messages::PublishAttributes&) override {}
};

// --- Helper: Random ID ---
std::string GenerateRandomID() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(10000, 99999);
    return std::to_string(dis(gen));
}

// --- Logic: Main Flow ---
void RunRequestLogic(std::string root_ns, std::shared_ptr<RequestClient> client, bool& stop) {
    auto sub_util = std::make_shared<SubscriberUtil>();
    std::string my_id = "hallgato_" + GenerateRandomID();

    typedef quicr::SubscribeTrackHandler::JoiningFetch Fetch;
    const auto joining_fetch = Fetch{ 4, quicr::messages::GroupOrder::kAscending, {}, 0, true };

    // 1. Feliratkozás a Unified Catalog-ra
    auto catalog_ftn = quicr::example::MakeFullTrackName("svc,"+root_ns, "catalog");
    auto catalog_handler = std::make_shared<CatalogSubscribeTrackHandler>(
        catalog_ftn, quicr::messages::FilterType::kLargestObject, std::nullopt, sub_util);

    SPDLOG_INFO("Subscribing to catalog: {}-catalog", catalog_ftn.name_space.ToString());
    client->SubscribeTrack(catalog_handler);

    std::cout << "Waiting for catalog..." << std::endl;
    if (!catalog_handler->WaitForCatalog(std::chrono::seconds(15))) {
        std::cerr << "Timeout waiting for catalog! (Check Relay routing)" << std::endl;
        stop = true; return;
    }
    SPDLOG_INFO("Catalog received!");

    // 2. Interaktív menü: Videó kiválasztása
    Catalog catalog_copy = catalog_handler->GetCatalogCopy();
    std::vector<CatalogTrackEntry> video_tracks;

    std::cout << "\n--- Available Video Tracks ---" << std::endl;
    for (const auto& t : catalog_copy.tracks()) {
        if (t.type == "video") {
            video_tracks.push_back(t);
            std::cout << video_tracks.size() << ". " << t.name
                      << " (" << (t.width ? *t.width : 0) << "x" << (t.height ? *t.height : 0) << ")"
                      << " [NS: " << t.effective_src_namespace(root_ns) << "]" << std::endl;
        }
    }

    if (video_tracks.empty()) {
        std::cerr << "No video tracks found in catalog." << std::endl;
        stop = true;
        return;
    }

    int choice = 0;
    std::cout << "Select a track number: ";
    std::cin >> choice;

    if (choice < 1 || choice > (int)video_tracks.size()) {
        std::cerr << "Invalid choice." << std::endl;
        stop = true;
        return;
    }

    CatalogTrackEntry selected_track = video_tracks[choice - 1];
    std::cout << "Selected: " << selected_track.name << std::endl;

    // 3. Paraméterek bekérése
    int target_width = 0, target_height = 0;
    std::cout << "Enter target width (e.g. 640): ";
    std::cin >> target_width;
    std::cout << "Enter target height (e.g. 360): ";
    std::cin >> target_height;

    std::string request_id_str = "req_" + GenerateRandomID(); // Egyedi ID a kérésnek

    std::string req_ns_str = "req,"+ root_ns + "," + my_id + "," + request_id_str;
    std::string req_track_name = "data";
    auto req_ftn = quicr::example::MakeFullTrackName(req_ns_str, req_track_name);

    SPDLOG_INFO("Announcing request namespace: {}", req_ftn.name_space.ToString());
    client->PublishNamespace(req_ftn.name_space);

    auto req_handler = std::make_shared<RequestPublishHandler>(req_ftn);
    req_handler->SetTrackAlias(3000);
    req_handler->SetUseAnnounce(true);
    client->PublishTrack(req_handler);

    // Kis szünet az announce miatt
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    TranscodeRequest req;
    // Operation
    OpVideoChangeResolution op_res;
    op_res.width = target_width;
    op_res.height = target_height;
    req.operations.push_back(TranscodeOperation::make(op_res));

    // JSON Serialize
    json jreq;
    jreq["request_id"] = request_id_str;
    jreq["client_id"] = my_id;
    jreq["source"] = { {"track", selected_track.name} };
    jreq["source"]["namespace"] = selected_track.effective_src_namespace(root_ns);

    // Output hint (optional)
    if (FALSE && req.output) {
        jreq["output"] = json::object();
        if (req.output->track_name_hint) jreq["output"]["track_name_hint"] = *req.output->track_name_hint;
    }

    jreq["operations"] = json::array();
    json jop;
    jop["type"] = "video.change_resolution";
    jop["params"] = { {"width", target_width}, {"height", target_height} };
    jreq["operations"].push_back(jop);

    std::string req_json_str = jreq.dump();
    SPDLOG_INFO("Sending request: {}", req_json_str);

    // Publish the JSON object
    quicr::ObjectHeaders hdr;
    hdr.group_id = 0;
    hdr.object_id = 0;
    hdr.payload_length = req_json_str.size();
    hdr.priority = 2;
    hdr.ttl = 15000;



    // Wait a bit for announce to propagate before sending data (naive approach)
    std::this_thread::sleep_for(std::chrono::seconds(1));
    req_handler->PublishObject(hdr, quicr::BytesSpan((uint8_t*)req_json_str.data(), req_json_str.size()));
    SPDLOG_INFO("Request published on track: {}-{}", req_ns_str, "data");

    // 5. Várakozás az új sávra a katalógusban
    std::cout << "Request sent. Waiting for transcoded track to appear in catalog..." << std::endl;

    std::string track_name_hint = "tran_" + my_id + "_" + std::to_string(target_height) + "p";
// 5. Várakozás az új sávra
    std::cout << "Request sent. Waiting for transcoded track (" << track_name_hint << ") to appear in catalog..." << std::endl;

    bool found_new_track = false;
    CatalogTrackEntry new_track_entry;

    while (!stop && !found_new_track) {
        // Blokkolva várunk a katalógus frissítésre
        if (catalog_handler->WaitForUpdate()) {
            Catalog current = catalog_handler->GetCatalogCopy();

            for (const auto& t : current.tracks()) {
                if (t.type == "video") {
                    if (t.height && *t.height == target_height) {
                        new_track_entry = t;
                        found_new_track = true;
                        break;
                    }
                }
            }
        }
    }

    if (found_new_track) {
        SPDLOG_INFO("Found transcoded track: {}", new_track_entry.name);
        std::cout << "Transcoded track found! Initializing GStreamer..." << std::endl;

        // 6. LEJÁTSZÁS INICIALIZÁLÁSA

        // a) GStreamer indítása
        GstInitOnce();

        // b) Init adat (codec config) kinyerése és regisztrálása
        std::vector<uint8_t> init_data = base64::decode_to_uint8_vec(new_track_entry.b64_init_data);

        // Ez a hívás beleteszi a map-be a 'new_track_entry.name'-hez tartozó init adatot
        g_gst->RegisterTrackInit(new_track_entry.name, true, init_data.data(), init_data.size());

        // c) SubTrack struktúra létrehozása a VideoSubscribeTrackHandler-hez
        auto subtrack = std::make_shared<SubTrack>();
        subtrack->track_entry = new_track_entry;
        subtrack->namespace_ = new_track_entry.effective_src_namespace(sub_util->catalog.namespace_);
        subtrack->init = init_data;

        // d) Handler létrehozása (A létező osztályt használjuk)
        auto media_ftn = quicr::example::MakeFullTrackName(subtrack->namespace_, new_track_entry.name);

        auto media_handler = std::make_shared<VideoSubscribeTrackHandler>(
            media_ftn,
            quicr::messages::FilterType::kNextGroupStart, // Élő lejátszás
            std::nullopt,
            subtrack
        );

        // e) GStreamer callback beállítása a handlernek
        media_handler->SetSubscribeGst(g_gst);

        // f) Pipeline elindítása és a sáv "kiválasztása" (ez pusholja be az init adatot a pipeline-ba)
        SPDLOG_INFO("Starting GStreamer pipelines...");
        g_gst->StartPipelines(GST_STATE_PLAYING);
        g_gst->SelectVideo(new_track_entry.name);

        // g) Feliratkozás
        SPDLOG_INFO("Subscribing to media track: {}/{}", subtrack->namespace_, new_track_entry.name);
        client->SubscribeTrack(media_handler);

        std::cout << "Playing... Press Ctrl+C to stop." << std::endl;

        // Végtelen ciklus a lejátszáshoz
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        client->UnsubscribeTrack(media_handler);
    } else {
        std::cerr << "Stopping without playback." << std::endl;
    }

    // Cleanup
    client->UnpublishTrack(req_handler);
    client->UnsubscribeTrack(catalog_handler);
}

// --- Main Entry ---
int main(int argc, char* argv[]) {
    logger = spdlog::stderr_color_mt("console");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::trace);

    SPDLOG_INFO("INFO");
    SPDLOG_WARN("WARN");
    SPDLOG_ERROR("ERROR");
    SPDLOG_DEBUG("DEBUG");
    SPDLOG_TRACE("TRACE");


    cxxopts::Options options("qc_request", "MoQ Request Client");
    options.add_options()
        ("h,help", "Print help")
        ("r,url", "Relay URL", cxxopts::value<std::string>()->default_value("moq://localhost:1234"))
        ("e,endpoint_id", "Endpoint ID", cxxopts::value<std::string>()->default_value("request-client"))
        ("n,namespace", "Root Namespace (e.g. IB027)", cxxopts::value<std::string>()->default_value("IB027"));

    // JAVÍTÁS: cli_opts helyett result változó használata a logikában
    auto result = options.parse(argc, argv);
    if (result.count("help")) { std::cout << options.help() << std::endl; return 0; }

    installSignalHandlers();

    quicr::ClientConfig config;
    config.transport_config.time_queue_max_duration = 15000; // 10s

    // JAVÍTÁS: result használata
    config.endpoint_id = result["endpoint_id"].as<std::string>();
    config.connect_uri = result["url"].as<std::string>();

    bool stop_threads = false;
    auto client = RequestClient::Create(config, stop_threads);

    if (client->Connect() != quicr::Transport::Status::kConnecting) {
        SPDLOG_ERROR("Connection failed");
        return -1;
    }

    // Wait for ready
    while (!stop_threads) {
        if (client->GetStatus() == quicr::Client::Status::kReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::string root_ns = result["namespace"].as<std::string>();

    // Futtatjuk a logikát
    std::thread logic_thread(RunRequestLogic, root_ns, client, std::ref(stop_threads));

    // Várakozás kilépésre (Ctrl+C)
    std::mutex main_mtx;
    std::unique_lock lk(main_mtx);
    moq_example::cv.wait(lk, [&](){ return moq_example::terminate; });

    stop_threads = true;
    if (logic_thread.joinable()) logic_thread.join();

    client->Disconnect();
    return 0;
}