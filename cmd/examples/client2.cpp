// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include <nlohmann/json.hpp>
#include <oss/cxxopts.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <quicr/client.h>
#include <quicr/object.h>

#include "helper_functions.h"
#include "signal_handler.h"

#include <filesystem>
#include <format>
#include <fstream>

#include <quicr/publish_fetch_handler.h>

#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>
#include <unordered_map>

#include <termios.h>

#include "media.h"
#include "subscriber_util.h"

#include <optional>

std::shared_ptr<spdlog::logger> logger;

using json = nlohmann::json; // NOLINT

namespace qclient_vars {
    bool publish_clock{ false };
    bool video{ false };                 /// Video publish
    std::optional<uint64_t> track_alias; /// Track alias to use for subscribe
    bool record = false;
    bool playback = false;
    bool new_group = false;
    bool add_gaps = false;
    bool req_track_status = false;
    std::chrono::milliseconds playback_speed_ms(20);
}

namespace qclient_consts {
    const std::filesystem::path kMoqDataDir = std::filesystem::current_path() / "moq_data";
}

namespace {
    // TODO: Escape/drop invalid filename characters.
    std::string ToString(const quicr::FullTrackName& ftn)
    {
        std::string str;
        const auto& entries = ftn.name_space.GetEntries();
        for (const auto& entry : entries) {
            str += std::string(entry.begin(), entry.end()) + '_';
        }

        str += std::string(ftn.name.begin(), ftn.name.end());

        return str;
    }
}

namespace base64 {
    // From https://gist.github.com/williamdes/308b95ac9ef1ee89ae0143529c361d37;

    constexpr std::string_view kValues = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";//=
    static std::string Encode(const std::string& in)
    {
        std::string out;

        int val = 0;
        int valb = -6;

        for (std::uint8_t c : in) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out += kValues[(val >> valb) & 0x3F];
                valb -= 6;
            }
        }

        if (valb > -6) {
            out += kValues[((val << 8) >> (valb + 8)) & 0x3F];
        }

        while (out.size() % 4) {
            out += '=';
        }

        return out;
    }

    [[maybe_unused]] static std::string Decode(const std::string& in)
    {
        std::string out;

        std::vector<int> values(256, -1);
        for (int i = 0; i < 64; i++) {
            values[kValues[i]] = i;
        }

        int val = 0;
        int valb = -8;

        for (std::uint8_t c : in) {
            if (values[c] == -1) {
                break;
            }

            val = (val << 6) + values[c];
            valb += 6;

            if (valb >= 0) {
                out += char((val >> valb) & 0xFF);
                valb -= 8;
            }
        }

        return out;
    }

    inline std::string Encode(const std::vector<uint8_t>& in) {
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        size_t i = 0;
        while (i + 3 <= in.size()) {
            uint32_t v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
            out.push_back(kValues[(v >> 18) & 0x3F]);
            out.push_back(kValues[(v >> 12) & 0x3F]);
            out.push_back(kValues[(v >> 6)  & 0x3F]);
            out.push_back(kValues[v & 0x3F]);
            i += 3;
        }
        if (i + 1 == in.size()) {
            uint32_t v = (in[i] << 16);
            out.push_back(kValues[(v >> 18) & 0x3F]);
            out.push_back(kValues[(v >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        } else if (i + 2 == in.size()) {
            uint32_t v = (in[i] << 16) | (in[i+1] << 8);
            out.push_back(kValues[(v >> 18) & 0x3F]);
            out.push_back(kValues[(v >> 12) & 0x3F]);
            out.push_back(kValues[(v >> 6)  & 0x3F]);
            out.push_back('=');
        }
        return out;
    }

    // Dekód táblázat (standard, + és / jelekhez)
    inline const std::array<int8_t, 256>& decode_table() {
        static std::array<int8_t, 256> D{};
        static bool init = false;
        if (!init) {
            D.fill(-1);
            for (int i = 0; i < 64; ++i) {
                D[static_cast<unsigned char>(kValues[i])] = static_cast<int8_t>(i);
            }
            init = true;
        }
        return D;
    }

    // Robusztus dekóder: kezel URL-safe-et, whitespace-et, data URI prefixet, paddinget
    inline std::vector<uint8_t> decode_to_uint8_vec(std::string_view in) {
        // 1) Ha data: URI, vágjuk le az előtagot a vesszőig
        if (in.rfind("data:", 0) == 0) {
            const size_t comma = in.find(',');
            if (comma == std::string_view::npos) {
                throw std::invalid_argument("base64: invalid data URI (missing comma)");
            }
            in = in.substr(comma + 1);
        }

        // 2) Normalizálás: minden whitespace eldobása; URL-safe (-/_) -> +/
        std::string norm;
        norm.reserve(in.size());
        bool saw_urlsafe = false;

        for (size_t i = 0; i < in.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(in[i]);

            if (std::isspace(c)) continue;           // minden ASCII whitespace törlése

            if (c == '-') { norm.push_back('+'); saw_urlsafe = true; continue; }
            if (c == '_') { norm.push_back('/'); saw_urlsafe = true; continue; }

            // Megengedett karakterek: A-Z a-z 0-9 + / =
            if ((c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '+' || c == '/' || c == '=') {
                norm.push_back(static_cast<char>(c));
            } else {
                // Adjunk értelmes hibát pozícióval és kóddal
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "base64: invalid character at pos %zu: 0x%02X ('%c')",
                              i, c, (c >= 32 && c < 127 ? c : '.'));
                throw std::invalid_argument(msg);
            }
        }

        // 3) Pótoljuk a paddinget 4-es többszörösre
        if (norm.size() % 4 != 0) {
            size_t pad = (4 - (norm.size() % 4)) % 4;
            norm.append(pad, '=');
        }

        // 4) Tényleges dekódolás
        const auto& D = decode_table();
        std::vector<uint8_t> out;
        out.reserve((norm.size() / 4) * 3);

        int val = 0;
        int valb = -8;

        for (size_t pos = 0; pos < norm.size(); ++pos) {
            unsigned char c = static_cast<unsigned char>(norm[pos]);
            if (c == '=') {
                // padding -> a hátralevő részt ignoráljuk (csak whitespace/’=’ lehet)
                break;
            }
            int8_t d = D[c];
            if (d < 0) {
                // Ez elvileg nem fordulhat elő a fenti szűrés után
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "base64: unexpected character after normalization at pos %zu: 0x%02X",
                              pos, c);
                throw std::invalid_argument(msg);
            }
            val = (val << 6) | d;
            valb += 6;
            if (valb >= 0) {
                out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }

        return out;
    }
}

/**
 * @brief  Subscribe track handler
 * @details Subscribe track handler used for the subscribe command line option.
 */
class MySubscribeTrackHandler : public quicr::SubscribeTrackHandler
{
    bool is_catalog_;
    std::shared_ptr<SubscriberUtil> util_;

  public:
    MySubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                            quicr::messages::FilterType filter_type,
                            const std::optional<JoiningFetch>& joining_fetch,
                            std::shared_ptr<SubscriberUtil> util,
                            const bool catalog = false,
                            const std::filesystem::path& dir = qclient_consts::kMoqDataDir)
      : SubscribeTrackHandler(full_track_name, 3, quicr::messages::GroupOrder::kAscending, filter_type, joining_fetch)
    {
        is_catalog_ = catalog;
        util_ = util;

        if (qclient_vars::record) {
            std::filesystem::create_directory(dir);

            const std::string name_str = ToString(full_track_name);
            data_fs_.open(dir / (name_str + ".dat"), std::ios::in | std::ios::out | std::ios::trunc);

            moq_fs_.open(dir / (name_str + ".moq"), std::ios::in | std::ios::out | std::ios::trunc);
            moq_fs_ << json::array();
        }
    }

    virtual ~MySubscribeTrackHandler()
    {
        data_fs_ << std::endl;
        data_fs_.close();

        moq_fs_ << std::endl;
        moq_fs_.close();
    }

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        if (qclient_vars::record) {
            RecordObject(GetFullTrackName(), hdr, data);
        }
        std::string s(reinterpret_cast<const char*>(GetFullTrackName().name.data()),
                      GetFullTrackName().name.size());
        SPDLOG_INFO("Received message on {0}: Group:{1}, Object:{2}", s, hdr.group_id, hdr.object_id);

        if (is_catalog_) {
            // Parse catalog and print to stdout
            try {
                std::string catalog_str(data.begin(), data.end());
                Catalog catalog;
                catalog.from_json(catalog_str);
                std::vector<uint8_t> init_data = base64::decode_to_uint8_vec(catalog.init_data_);
                std::cout.write(reinterpret_cast<const char*>(init_data.data()), init_data.size());
                for (const auto& track : catalog.tracks()) {
                    auto s_track = std::make_shared<SubTrack>();
                    s_track->track_entry = track;
                    util_->sub_tracks.push_back(s_track);
                }
                util_->catalog_read = true;
                SPDLOG_INFO( "Catalog parsed with {0} tracks", catalog.tracks().size());


            } catch (const std::exception& e) {
                SPDLOG_ERROR("Failed to parse catalog JSON: {0}", e.what());
                std::cerr << fmt::format("Failed to parse catalog JSON: {0}", e.what()) << std::endl;
            }
            return;
        }

        // std::cout << msg;
        std::cout.write(reinterpret_cast<const char*>(data.data()), data.size());
        std::cout.flush();



        // std::stringstream ext;
        //
        // if (hdr.extensions) {
        //     ext << "mutable hdrs: ";
        //
        //     for (const auto& [type, value] : hdr.extensions.value()) {
        //         ext << std::hex << std::setfill('0') << std::setw(2) << type;
        //         ext << " = " << std::dec << std::setw(0) << uint64_t(quicr::UintVar(value)) << " ";
        //     }
        // }
        //
        // if (hdr.immutable_extensions) {
        //     ext << "immutable hdrs: ";
        //
        //     for (const auto& [type, value] : hdr.immutable_extensions.value()) {
        //         ext << std::hex << std::setfill('0') << std::setw(2) << type;
        //         ext << " = " << std::dec << std::setw(0) << uint64_t(quicr::UintVar(value)) << " ";
        //     }
        // }
        //
        // std::string msg(data.begin(), data.end());
        //
        // SPDLOG_INFO("Received message: {} Group:{}, Object:{} - {}", ext.str(), hdr.group_id, hdr.object_id, msg);
        //
        // if (qclient_vars::new_group && not new_group_requested_) {
        //     SPDLOG_INFO("Track alias: {} requesting new group", GetTrackAlias().value());
        //     RequestNewGroup();
        //     new_group_requested_ = true;
        // }
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kOk: {
                if (auto track_alias = GetTrackAlias(); track_alias.has_value()) {
                    SPDLOG_INFO("Track alias: {0} is ready to read", track_alias.value());
                }
            } break;

            default:
                break;
        }
    }

  private:
    void RecordObject(const quicr::FullTrackName& ftn, const quicr::ObjectHeaders& hdr, quicr::BytesSpan data)
    {
        const std::size_t data_offset = data_fs_.tellp();
        data_fs_ << std::string(data.begin(), data.end());

        std::vector<std::string> ns_entries;
        for (const auto& entry : ftn.name_space.GetEntries()) {
            ns_entries.push_back(base64::Encode(std::string{ entry.begin(), entry.end() }));
        }

        const std::string name_str = ToString(GetFullTrackName());
        const std::string data_filename = name_str + ".dat";

        json j;
        j["nameSpace"] = ns_entries;
        j["trackName"] = base64::Encode(std::string(ftn.name.begin(), ftn.name.end()));
        j["objectID"] = hdr.object_id;
        j["groupID"] = hdr.group_id;
        j["subGroup"] = hdr.subgroup_id;
        j["publisherPriority"] = hdr.priority.value();
        j["maxCacheDuration"] = 0;
        j["publisherDeliveryTimeout"] = 0;
        j["receiveTime"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
        j["dataFile"] = data_filename;
        j["dataOffset"] = data_offset;
        j["dataLength"] = hdr.payload_length;

        moq_fs_.clear();
        moq_fs_.seekg(0);
        json moq_j = json::parse(moq_fs_);
        moq_j.push_back(j);

        moq_fs_.clear();
        moq_fs_.seekg(0);
        moq_fs_ << moq_j.dump();
    }

  private:
    std::ofstream data_fs_;
    std::fstream moq_fs_;
    bool new_group_requested_ = false;
};

/**
 * @brief Publish track handler
 * @details Publish track handler used for the publish command line option
 */
class MyPublishTrackHandler : public quicr::PublishTrackHandler
{
  public:
    MyPublishTrackHandler(const quicr::FullTrackName& full_track_name,
                          quicr::TrackMode track_mode,
                          uint8_t default_priority,
                          uint32_t default_ttl)
      : quicr::PublishTrackHandler(full_track_name, track_mode, default_priority, default_ttl)
    {
    }

    void StatusChanged(Status status) override
    {
        const auto alias = GetTrackAlias().value();
        switch (status) {
            case Status::kOk: {
                SPDLOG_INFO("Publish track alias: {0} is ready to send", alias);
                break;
            }
            case Status::kNoSubscribers: {
                SPDLOG_INFO("Publish track alias: {0} has no subscribers", alias);
                break;
            }
            case Status::kNewGroupRequested: {
                SPDLOG_INFO("Publish track alias: {0} has new group request", alias);
                break;
            }
            case Status::kSubscriptionUpdated: {
                SPDLOG_INFO("Publish track alias: {0} has updated subscription", alias);
                break;
            }
            case Status::kPaused: {
                SPDLOG_INFO("Publish track alias: {0} is paused", alias);
                break;
            }
            case Status::kPendingPublishOk: {
                SPDLOG_INFO("Publish track alias: {0} is pending publish ok", alias);
                break;
            }

            default:
                SPDLOG_INFO("Publish track alias: {0} has status {1}", alias, static_cast<int>(status));
                break;
        }
    }
};

class MyFetchTrackHandler : public quicr::FetchTrackHandler
{
    MyFetchTrackHandler(const quicr::FullTrackName& full_track_name,
                        uint64_t start_group,
                        uint64_t start_object,
                        uint64_t end_group,
                        uint64_t end_object)
      : FetchTrackHandler(full_track_name,
                          3,
                          quicr::messages::GroupOrder::kAscending,
                          start_group,
                          end_group,
                          start_object,
                          end_object)
    {
    }

  public:
    static auto Create(const quicr::FullTrackName& full_track_name,
                       uint64_t start_group,
                       uint64_t start_object,
                       uint64_t end_group,
                       uint64_t end_object)
    {
        return std::shared_ptr<MyFetchTrackHandler>(
          new MyFetchTrackHandler(full_track_name, start_group, end_group, start_object, end_object));
    }

    void ObjectReceived(const quicr::ObjectHeaders& headers, quicr::BytesSpan data) override
    {
        std::string msg(data.begin(), data.end());
        SPDLOG_INFO(
          "Received fetched object group_id: {} object_id: {} value: {}", headers.group_id, headers.object_id, msg);
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
                SPDLOG_INFO("Fetch failed");
                break;
            }
            default:
                break;
        }
    }
};

/**
 * @brief MoQ client
 * @details Implementation of the MoQ Client
 */
class MyClient : public quicr::Client
{
    MyClient(const quicr::ClientConfig& cfg, bool& stop_threads)
      : quicr::Client(cfg)
      , stop_threads_(stop_threads)
    {
    }

  public:
    static std::shared_ptr<MyClient> Create(const quicr::ClientConfig& cfg, bool& stop_threads)
    {
        return std::shared_ptr<MyClient>(new MyClient(cfg, stop_threads));
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kReady:
                SPDLOG_INFO("Connection ready");
                break;
            case Status::kConnecting:
                break;
            case Status::kPendingServerSetup:
                SPDLOG_INFO("Connection connected and now pending server setup");
                break;
            default:
                SPDLOG_INFO("Connection failed {0}", static_cast<int>(status));
                stop_threads_ = true;
                moq_example::terminate = true;
                moq_example::termination_reason = "Connection failed";
                moq_example::cv.notify_all();
                break;
        }
    }

    void AnnounceReceived(const quicr::TrackNamespace& track_namespace,
                          const quicr::PublishAnnounceAttributes&) override
    {
        auto th = quicr::TrackHash({ track_namespace, {} });
        SPDLOG_INFO("Received announce for namespace_hash: {}", th.track_namespace_hash);
    }

    void UnannounceReceived(const quicr::TrackNamespace& track_namespace) override
    {
        auto th = quicr::TrackHash({ track_namespace, {} });
        SPDLOG_INFO("Received unannounce for namespace_hash: {}", th.track_namespace_hash);
    }

    void SubscribeAnnouncesStatusChanged(const quicr::TrackNamespace& track_namespace,
                                         std::optional<quicr::messages::SubscribeNamespaceErrorCode> error_code,
                                         std::optional<quicr::messages::ReasonPhrase> reason) override
    {
        auto th = quicr::TrackHash({ track_namespace, {} });
        if (!error_code.has_value()) {
            SPDLOG_INFO("Subscribe announces namespace_hash: {} status changed to OK", th.track_namespace_hash);
            return;
        }

        std::string reason_str;
        if (reason.has_value()) {
            reason_str.assign(reason.value().begin(), reason.value().end());
        }

        SPDLOG_WARN("Subscribe announces to namespace_hash: {} has error {} with reason: {}",
                    th.track_namespace_hash,
                    static_cast<uint64_t>(error_code.value()),
                    reason_str);
    }

    bool FetchReceived(quicr::ConnectionHandle connection_handle,
                       uint64_t request_id,
                       const quicr::FullTrackName& track_full_name,
                       const quicr::messages::FetchAttributes& attributes) override
    {
        auto pub_fetch_h = quicr::PublishFetchHandler::Create(
          track_full_name, attributes.priority, request_id, attributes.group_order, 50000);
        BindFetchTrack(connection_handle, pub_fetch_h);

        for (uint64_t pub_group_number = attributes.start_location.group; pub_group_number < attributes.end_group;
             ++pub_group_number) {
            quicr::ObjectHeaders headers{ .group_id = pub_group_number,
                                          .object_id = 0,
                                          .subgroup_id = 0,
                                          .payload_length = 0,
                                          .status = quicr::ObjectStatus::kAvailable,
                                          .priority = attributes.priority,
                                          .ttl = 3000, // in milliseconds
                                          .track_mode = std::nullopt,
                                          .extensions = std::nullopt,
                                          .immutable_extensions = std::nullopt };

            std::string hello = "Hello:" + std::to_string(pub_group_number);
            std::vector<uint8_t> data_vec(hello.begin(), hello.end());
            quicr::BytesSpan data{ data_vec.data(), data_vec.size() };
            pub_fetch_h->PublishObject(headers, data);
        }

        return true;
    }

    void TrackStatusResponseReceived(quicr::ConnectionHandle,
                                     uint64_t request_id,
                                     const quicr::SubscribeResponse& response) override
    {
        switch (response.reason_code) {
            case quicr::SubscribeResponse::ReasonCode::kOk:
                SPDLOG_INFO("Request track status OK response request_id: {} largest group: {} object: {}",
                            request_id,
                            response.largest_location.has_value() ? response.largest_location->group : 0,
                            response.largest_location.has_value() ? response.largest_location->object : 0);
                break;
            default:
                SPDLOG_INFO("Request track status response ERROR request_id: {} error: {} reason: {}",
                            request_id,
                            static_cast<int>(response.reason_code),
                            response.error_reason.has_value() ? response.error_reason.value() : "");
                break;
        }
    }

  private:
    bool& stop_threads_;
};

struct TrackPublishData
{
    int track_id{ -1 };
    std::string track_type;
    std::shared_ptr<MyPublishTrackHandler> track_handler;

    uint64_t group_id{ 0 };
    uint64_t object_id{ 0 };
    uint64_t subgroup_id{ 0 };
};

/*===========================================================================*/
// INIT publisher for a track
/*===========================================================================*/

quicr::PublishTrackHandler::PublishObjectStatus
PublishCatalog(std::shared_ptr<TrackPublishData>& Catalog_TrackHandler_Data,
            std::shared_ptr<PublisherSharedState> shared_state,
            bool& catalog_published)
{
    switch (Catalog_TrackHandler_Data->track_handler->GetStatus()) {
        case MyPublishTrackHandler::Status::kOk:
            break;
        case MyPublishTrackHandler::Status::kNewGroupRequested:
            if (Catalog_TrackHandler_Data->object_id) {
                Catalog_TrackHandler_Data->group_id++;
                Catalog_TrackHandler_Data->object_id = 0;
                Catalog_TrackHandler_Data->subgroup_id = 0;
            }
            SPDLOG_INFO("New Group Requested: Now using group {0}", Catalog_TrackHandler_Data->group_id);

            break;
        case MyPublishTrackHandler::Status::kSubscriptionUpdated:
            SPDLOG_INFO("subscribe updated");
            break;
        case MyPublishTrackHandler::Status::kNoSubscribers:
            // Start a new group when a subscriber joins
            if (Catalog_TrackHandler_Data->object_id) {
                Catalog_TrackHandler_Data->group_id++;
                Catalog_TrackHandler_Data->object_id = 0;
                Catalog_TrackHandler_Data->subgroup_id = 0;
            }
            [[fallthrough]];
        default:
            SPDLOG_WARN("Catalog publish status not ok: {0}", static_cast<int>(Catalog_TrackHandler_Data->track_handler->GetStatus()));
            return MyPublishTrackHandler::PublishObjectStatus::kInternalError;
    }


    std::vector<uint8_t> catalog_data;

    {
        auto ftyp = shared_state->getFtyp();
        auto moov = shared_state->getMoov();

        if (ftyp.data.empty() || moov.data.empty()) {
            SPDLOG_ERROR("No ftyp or moov atom available yet");
            return quicr::PublishTrackHandler::PublishObjectStatus::kObjectDataIncomplete;
        }

        catalog_data.insert(catalog_data.end(),
            ftyp.data.begin(), ftyp.data.end());
        catalog_data.insert(catalog_data.end(),
            moov.data.begin(), moov.data.end());
    }


    auto str = base64::Encode(catalog_data);

    shared_state->catalog.init_data_ = str;
    shared_state->catalog.binary_size = catalog_data.size();
    std::string catalog_str = shared_state->catalog.to_json(true);

    std::cout << "Catalog JSON: " << catalog_str << std::endl;

    quicr::BytesSpan catalog_bytespan{ reinterpret_cast<const uint8_t*>(catalog_str.data()), catalog_str.size() };

    quicr::ObjectHeaders obj_headers = {
        Catalog_TrackHandler_Data->group_id,
        Catalog_TrackHandler_Data->object_id,
        Catalog_TrackHandler_Data->subgroup_id,
        catalog_bytespan.size(),
        quicr::ObjectStatus::kAvailable,
        2 /*priority*/,
        5000 /* ttl */,
        std::nullopt,
        std::nullopt
    };

    try {
        auto status = Catalog_TrackHandler_Data->track_handler->PublishObject(obj_headers, catalog_bytespan);
        if (status == decltype(status)::kPaused) {
            SPDLOG_INFO("Publish is paused");
        } else if (status == decltype(status)::kNoSubscribers) {
            SPDLOG_INFO("Publish has no subscribers");
        } else if (status != decltype(status)::kOk) {
            throw std::runtime_error("PublishObject returned status=" + std::to_string(static_cast<int>(status)));
        } else if (status == decltype(status)::kOk) {
            catalog_published = true;
            SPDLOG_INFO("Catalog published: {0}, group id: {1}, obj. id: {2}",
                static_cast<int>(catalog_published), Catalog_TrackHandler_Data->group_id, Catalog_TrackHandler_Data->object_id++);
            return status;
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Caught exception trying to publish catalog. (error={})", e.what());
    }
    return MyPublishTrackHandler::PublishObjectStatus::kInternalError;
}

/*===========================================================================*/
// Chunk publisher for a track
/*===========================================================================*/

quicr::PublishTrackHandler::PublishObjectStatus
PublishChunk(std::shared_ptr<TrackPublishData> & TrackHandler_Data,
             std::shared_ptr<MP4Chunk> chunk)
{
    if (chunk->has_keyframe) { // TrackHandler_Data->track_type == "video" &&
        TrackHandler_Data->group_id++;
        TrackHandler_Data->object_id = 0;
        TrackHandler_Data->subgroup_id = 0;
    }

    switch (TrackHandler_Data->track_handler->GetStatus()) {
        case MyPublishTrackHandler::Status::kOk:
            break;
        case MyPublishTrackHandler::Status::kNewGroupRequested:
            if (TrackHandler_Data->object_id) {
                TrackHandler_Data->group_id++;
                TrackHandler_Data->object_id = 0;
                TrackHandler_Data->subgroup_id = 0;
            }
            SPDLOG_INFO("New Group Requested: Now using group {0}", TrackHandler_Data->group_id);

            break;
        case MyPublishTrackHandler::Status::kSubscriptionUpdated:
            SPDLOG_INFO("subscribe updated");
            break;
        case MyPublishTrackHandler::Status::kNoSubscribers:
            // Start a new group when a subscriber joins
            if (TrackHandler_Data->object_id) {
                TrackHandler_Data->group_id++;
                TrackHandler_Data->object_id = 0;
                TrackHandler_Data->subgroup_id = 0;
            }
            [[fallthrough]];
        default:
            return MyPublishTrackHandler::PublishObjectStatus::kNoSubscribers;
    }

    quicr::ObjectHeaders obj_headers = {
        TrackHandler_Data->group_id,
        TrackHandler_Data->object_id,
        TrackHandler_Data->subgroup_id,
        chunk->moof.data.size() + chunk->mdat.data.size(),
        quicr::ObjectStatus::kAvailable,
        2 /*priority*/,
        5000 /* ttl */,
        std::nullopt,
        std::nullopt
    };

    std::vector<uint8_t> data_to_publish;
    data_to_publish.insert(data_to_publish.end(), chunk->moof.data.begin(), chunk->moof.data.end());
    data_to_publish.insert(data_to_publish.end(), chunk->mdat.data.begin(), chunk->mdat.data.end());

    try {
        auto status = TrackHandler_Data->track_handler->PublishObject(obj_headers, data_to_publish);
        if (status == decltype(status)::kPaused) {
            SPDLOG_INFO("Publish is paused");
        } else if (status == decltype(status)::kNoSubscribers) {
            SPDLOG_INFO("Publish has no subscribers");
        } else if (status != decltype(status)::kOk) {
            throw std::runtime_error("PublishObject returned status=" + std::to_string(static_cast<int>(status)));
        } else if (status == decltype(status)::kOk) {
            SPDLOG_INFO("Published CHUNK, rval: {0}, track_idx: {1}, group id: {2}, obj. id: {3}",
                static_cast<int>(status),
                TrackHandler_Data->track_id,
                TrackHandler_Data->group_id,
                TrackHandler_Data->object_id++);
        }
        return status;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Caught exception trying to publish chunk. (error={})", e.what());
    }
    return MyPublishTrackHandler::PublishObjectStatus::kInternalError;
}


/*===========================================================================*/
// Video Publisher Thread to perform publishing
/*===========================================================================*/

void
DoPublisher2(std::shared_ptr<PublisherSharedState> shared_state, const std::shared_ptr<quicr::Client>& client, bool use_announce, const bool& stop)
{
    std::vector<std::shared_ptr<TrackPublishData>> tpd_list;

    if (shared_state->catalog.tracks().empty()) {
        SPDLOG_WARN("No track names found in shared state");
        return;
    }
    {
        quicr::FullTrackName full_track_name = quicr::example::MakeFullTrackName(shared_state->catalog.namespace_, "catalog");

        auto th = std::make_shared<MyPublishTrackHandler>(
        full_track_name, quicr::TrackMode::kStream, 2, 3000);
        th->SetUseAnnounce(true); //TODO True vagy false mit tesz
        th->SetTrackAlias(0);

        auto tpd = std::make_shared<TrackPublishData>();
        tpd->track_id = 0;
        tpd->track_type = "catalog";
        tpd->track_handler = th;
        tpd_list.push_back(tpd);

        client->PublishTrack(th);
    }
    for (auto track : shared_state->catalog.tracks()) {

        quicr::FullTrackName full_track_name = quicr::example::MakeFullTrackName(shared_state->catalog.namespace_, track.name);

        auto th = std::make_shared<MyPublishTrackHandler>(
            full_track_name, quicr::TrackMode::kStream, 2, 3000);
        th->SetUseAnnounce(false);
        th->SetTrackAlias(track.idx);

        auto tpd = std::make_shared<TrackPublishData>();
        tpd->track_id = track.idx;
        tpd->track_type = track.type;
        tpd->track_handler = th;
        tpd_list.push_back(tpd);
        client->PublishTrack(th);
    }

    auto get_catalog_handler_data = [&]() -> std::shared_ptr<TrackPublishData> {
        for (const auto& tpd : tpd_list) {
            if (tpd->track_id == 0) {
                return tpd;
            }
        }
        return nullptr;
    };

    SPDLOG_INFO("Started publisher track");

    //bool published_tracks{ false };
    bool catalog_published{ false };

    while (!stop) {


        // if ((!published_tracks) && (client->GetStatus() == MyClient::Status::kReady)) {
        //     SPDLOG_INFO("Publish track");
        //     for (auto track_publish_data : tpd_list) {
        //         client->PublishTrack(track_publish_data->track_handler);
        //     }
        //     published_tracks = true;
        // }

    if (!catalog_published) {
            auto catalog_handler_data = get_catalog_handler_data();
            PublishCatalog(catalog_handler_data, shared_state, std::ref(catalog_published));
            if (!catalog_published) {
                std::this_thread::yield();
                SPDLOG_ERROR("Catalog not yet published, waiting...");
                continue;
            }
            SPDLOG_INFO("Published catalog");
        }


        //std::shared_ptr<MP4Chunk> chunk = shared_state->GetChunk(stop);
        if (auto ch = shared_state->GetChunk(stop)) {
                      // (opcionális) pacing itt, ha kell
            if (ch->track_id == -1) {
                SPDLOG_ERROR( "Track ID '-1', returning.");
                continue;
            }

            for (auto tpd : tpd_list) {
                if (tpd->track_id == ch->track_id) {
                    PublishChunk(tpd, ch);
                    break;
                }
            }
        } else {
                          // nincs adat most — adjunk CPU-t másnak
            std::this_thread::yield();
        }

        // if (chunk->track_id == -1) {
        //     SPDLOG_ERROR( "Track ID '-1', returning.");
        //     continue;
        // }
        //
        // for (auto tpd : tpd_list) {
        //     if (tpd->track_id == chunk->track_id) {
        //         PublishChunk(tpd, chunk);
        //         break;
        //     }
        // }
        // //std::this_thread::sleep_for(std::chrono::milliseconds(15c));
        // chunk.reset(); // Reset the chunk after publishing
    }

    for (auto track_publish_data : tpd_list) {
        client->UnpublishTrack(track_publish_data->track_handler);
    }

    SPDLOG_INFO("Publisher done track");
    moq_example::terminate = true;
}

/*===========================================================================*/
// Publisher Thread to perform publishing
/*===========================================================================*/

void
DoPublisher(const quicr::FullTrackName& full_track_name,
            const std::shared_ptr<quicr::Client>& client,
            bool use_announce,
            bool& stop)
{
    auto track_handler = std::make_shared<MyPublishTrackHandler>(
      full_track_name, quicr::TrackMode::kStream /*mode*/, 2 /*priority*/, 3000 /*ttl*/);

    track_handler->SetUseAnnounce(use_announce);

    if (qclient_vars::track_alias.has_value()) {
        track_handler->SetTrackAlias(*qclient_vars::track_alias);
    }

    SPDLOG_INFO("Started publisher track");

    bool published_track{ false };
    bool sending{ false };
    uint64_t group_id{ 0 };
    uint64_t object_id{ 0 };
    uint64_t subgroup_id{ 0 };

    std::ifstream moq_fs;
    std::ifstream data_fs;

    std::deque<std::pair<quicr::ObjectHeaders, quicr::Bytes>> messages;
    if (qclient_vars::playback) {
        const std::string name_str = ToString(full_track_name);
        moq_fs.open(qclient_consts::kMoqDataDir / (name_str + ".moq"), std::ios::in);
        data_fs.open(qclient_consts::kMoqDataDir / (name_str + ".dat"), std::ios::in);

        std::string data;
        std::getline(data_fs, data);

        json moq_arr_j = json::parse(moq_fs);

        for (const auto& moq_j : moq_arr_j) {
            quicr::ObjectHeaders hdr{
                .group_id = moq_j["groupID"],
                .object_id = moq_j["objectID"],
                .subgroup_id = moq_j["subGroup"],
                .payload_length = moq_j["dataLength"],
                .status = quicr::ObjectStatus::kAvailable,
                .priority = moq_j["publisherPriority"],
                .ttl = std::nullopt,
                .track_mode = std::nullopt,
                .extensions = std::nullopt,
                .immutable_extensions = std::nullopt,
            };

            std::size_t data_offset = moq_j["dataOffset"];

            auto& msg = messages.emplace_back(std::make_pair(hdr, quicr::Bytes{}));
            msg.second.assign(std::next(data.begin(), data_offset),
                              std::next(data.begin(), data_offset + hdr.payload_length));
        }
    }

    while (not stop) {
        if ((!published_track) && (client->GetStatus() == MyClient::Status::kReady)) {
            SPDLOG_INFO("Publish track ");
            client->PublishTrack(track_handler);
            published_track = true;
        }

        switch (track_handler->GetStatus()) {
            case MyPublishTrackHandler::Status::kOk:
                break;
            case MyPublishTrackHandler::Status::kNewGroupRequested:
                if (object_id) {
                    group_id++;
                    object_id = 0;
                    subgroup_id = 0;
                }
                SPDLOG_INFO("New Group Requested: Now using group {0}", group_id);

                break;
            case MyPublishTrackHandler::Status::kSubscriptionUpdated:
                SPDLOG_INFO("subscribe updated");
                break;
            case MyPublishTrackHandler::Status::kNoSubscribers:
                // Start a new group when a subscriber joins
                if (object_id) {
                    group_id++;
                    object_id = 0;
                    subgroup_id = 0;
                }
                [[fallthrough]];
            default:
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
        }

        if (!sending) {
            SPDLOG_INFO("--------------------------------------------------------------------------");

            if (qclient_vars::publish_clock) {
                SPDLOG_INFO(" Publishing clock timestamp every second");
            } else {
                SPDLOG_INFO(" Type message and press enter to send");
            }

            SPDLOG_INFO("--------------------------------------------------------------------------");
            sending = true;
        }

        if (qclient_vars::playback) {
            const auto [hdr, msg] = messages.front();
            messages.pop_front();

            SPDLOG_INFO("Send message: {0}", std::string(msg.begin(), msg.end()));

            try {
                auto status = track_handler->PublishObject(hdr, msg);
                if (status != decltype(status)::kOk) {
                    throw std::runtime_error("PublishObject returned status=" +
                                             std::to_string(static_cast<int>(status)));
                }
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Caught exception trying to publish. (error={})", e.what());
            }

            std::this_thread::sleep_for(qclient_vars::playback_speed_ms);

            if (messages.empty()) {
                break;
            }

            continue;
        }

        if (object_id && object_id % 15 == 0) { // Set new group
            object_id = 0;
            subgroup_id = 0;
            group_id++;
        }

        if (qclient_vars::add_gaps && group_id && group_id % 4 == 0) {
            group_id += 1;
        }

        if (qclient_vars::add_gaps && object_id && object_id % 8 == 0) {
            object_id += 2;
        }

        std::string msg;
        if (qclient_vars::publish_clock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(999));
            msg = quicr::example::GetTimeStr();
            SPDLOG_INFO("Group:{0} Object:{1}, Msg:{2}", group_id, object_id, msg);
        } else { // stdin
            getline(std::cin, msg);
            SPDLOG_INFO("Send message: {0}", msg);
        }

        quicr::ObjectHeaders obj_headers = {
            group_id,       object_id++,    subgroup_id,  msg.size(),   quicr::ObjectStatus::kAvailable,
            2 /*priority*/, 3000 /* ttl */, std::nullopt, std::nullopt, std::nullopt
        };

        try {
            if (track_handler->CanPublish()) {
                auto status =
                  track_handler->PublishObject(obj_headers, { reinterpret_cast<uint8_t*>(msg.data()), msg.size() });

                if (status == decltype(status)::kPaused) {
                    SPDLOG_INFO("Publish is paused");
                } else if (status == decltype(status)::kNoSubscribers) {
                    SPDLOG_INFO("Publish has no subscribers");
                } else if (status != decltype(status)::kOk) {
                    throw std::runtime_error("PublishObject returned status=" +
                                             std::to_string(static_cast<int>(status)));
                }
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Caught exception trying to publish. (error={})", e.what());
        }
    }

    client->UnpublishTrack(track_handler);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SPDLOG_INFO("Publisher done track");
    moq_example::terminate = true;
}

struct SubTrackHandlerStruct
{
    std::shared_ptr<MySubscribeTrackHandler> track_handler;
    SubTrack& trackDetails;
};

// ---- Video track toggle helpers ----
struct VideoToggleContext {
    std::vector<std::string> video_names; // pl. {"video1", "video2"}
    std::unordered_map<std::string, std::shared_ptr<MySubscribeTrackHandler>> handler_by_name;
    int active_idx = -1; // melyik video aktív
};

static bool StartsWith(const std::string& s, const std::string& pref) {
    return s.size() >= pref.size() && std::equal(pref.begin(), pref.end(), s.begin());
}
static bool IsVideoTrackName(const std::string& name) {
    // nálad: "video1", "video2", ... vs "sound3" stb.
    return StartsWith(name, "video");
}

// /dev/tty raw mód be/ki (nem a stdin!)
static termios SetRawInputFD(int fd) {
    termios oldt;
    if (tcgetattr(fd, &oldt) != 0) return oldt;
    termios newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(fd, TCSANOW, &newt);
    int oldfl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, oldfl | O_NONBLOCK);
    return oldt;
}


/*===========================================================================*/
// Subscriber thread to perform subscribe
/*===========================================================================*/

void
DoSubscriber(const std::string& track_namespace,
             const std::shared_ptr<quicr::Client>& client,
             quicr::messages::FilterType filter_type,
             const bool& stop,
             const std::optional<std::uint64_t> join_fetch,
             const bool absolute)
{
    using Fetch = quicr::SubscribeTrackHandler::JoiningFetch;
    const auto joining_fetch = join_fetch.has_value()
        ? Fetch{ 4, quicr::messages::GroupOrder::kAscending, {}, *join_fetch, absolute }
        : std::optional<Fetch>(std::nullopt);

    std::vector<SubTrackHandlerStruct> sub_track_handlers; // itt TOVÁBBRA IS csak non-video-k legyenek

    // ---- Perzisztens (statikus) állapotok a teljes függvény-élettartamra ----
    static VideoToggleContext s_vctx;       // videós handlerek + aktív index
    static std::atomic_bool s_keyloop_running{false};
    static std::thread       s_keyloop;
    static std::atomic_bool  s_toggle_requested{false}; // keyloop állítja, főciklus kezeli
    static std::mutex        s_vctx_mu;     // videós kontextus védelme

    auto sub_util = std::make_shared<SubscriberUtil>();

    // 1) KATALÓGUS FELIRATKOZÁS
    auto catalog_full_track_name = quicr::example::MakeFullTrackName(track_namespace, "catalog");
    const auto catalog_track_handler =
        std::make_shared<MySubscribeTrackHandler>(catalog_full_track_name, filter_type, joining_fetch, sub_util, true);

    SPDLOG_INFO("Started subscriber");

    if (client->GetStatus() == MyClient::Status::kReady) {
        std::cerr << "Subscribing to catalog track" << std::endl;
        client->SubscribeTrack(catalog_track_handler);
    } else {
        // kis várakozás míg Ready lesz
        while (!stop && client->GetStatus() != MyClient::Status::kReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!stop) {
            client->SubscribeTrack(catalog_track_handler);
        }
    }

    // 2) Várunk, míg a catalog bejön és a track lista feltöltődik
    while (!stop && sub_util->catalog_read == false) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SPDLOG_INFO("Waiting for catalog to be ready");
        if (moq_example::terminate) break;
    }

    // 3) Feliratkozás trackekre: non-video azonnal; video csak EGY kezdetben
    if (!stop && sub_util->subscribed == false) {
        SPDLOG_INFO("Subscribing to tracks based on ready catalog");

        // vctx ürítése (ha újraindulna)
        {
            std::lock_guard<std::mutex> lk(s_vctx_mu);
            s_vctx.video_names.clear();
            s_vctx.handler_by_name.clear();
            s_vctx.active_idx = -1;
        }

        for (auto track : sub_util->sub_tracks) {
            track->namespace_ = track_namespace;
            std::string name = track->track_entry.name;

            auto track_full_name = quicr::example::MakeFullTrackName(track_namespace, name);
            auto track_handler   = std::make_shared<MySubscribeTrackHandler>(track_full_name, filter_type, joining_fetch, sub_util, false);

            if (IsVideoTrackName(name)) {
                // csak ELTÁROLJUK; feliratkozás majd lejjebb
                std::lock_guard<std::mutex> lk(s_vctx_mu);
                if (s_vctx.handler_by_name.find(name) == s_vctx.handler_by_name.end()) {
                    s_vctx.handler_by_name[name] = track_handler;
                    // ne duplikáljunk neveket
                    if (std::find(s_vctx.video_names.begin(), s_vctx.video_names.end(), name) == s_vctx.video_names.end())
                        s_vctx.video_names.push_back(name);
                }
            } else {
                // NON-VIDEO: azonnal subscribe, és eltesszük az unsubscribe-hoz
                client->SubscribeTrack(track_handler);
                SubTrackHandlerStruct track_handler_struct{track_handler, *track};
                sub_track_handlers.push_back(track_handler_struct);
                SPDLOG_INFO("Subscribed (non-video) track name: {}", name);
            }
        }

        // Kezdeti video feliratkozás: az elsőre
        {
            std::lock_guard<std::mutex> lk(s_vctx_mu);
            if (!s_vctx.video_names.empty()) {
                s_vctx.active_idx = 0;
                const std::string& first = s_vctx.video_names[s_vctx.active_idx];
                auto it = s_vctx.handler_by_name.find(first);
                if (it != s_vctx.handler_by_name.end() && it->second) {
                    client->SubscribeTrack(it->second);
                    SPDLOG_INFO("Subscribed initial VIDEO track: {}", first);
                } else {
                    SPDLOG_WARN("Initial VIDEO handler not found for name={}", first);
                }
            } else {
                SPDLOG_INFO("No VIDEO tracks found to subscribe initially.");
            }
        }

        // Billentyűfigyelő szál: CSAK jelez (flag), nem hív Client-et!
        if (!s_keyloop_running.exchange(true)) {
            if (s_keyloop.joinable()) s_keyloop.join(); // elvileg nem szükséges, védekezés
            s_keyloop = std::thread([]() {
                int tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
                if (tty_fd < 0) {
                    fprintf(stderr, "[toggle] /dev/tty not available; key toggle disabled.\n");
                    s_keyloop_running = false;
                    return;
                }
                termios oldt = SetRawInputFD(tty_fd);
                auto restore = [&]() { tcsetattr(tty_fd, TCSANOW, &oldt); close(tty_fd); };

                while (s_keyloop_running.load(std::memory_order_relaxed)) {
                    int c;
                    char ch;
                    int r = ::read(tty_fd, &ch, 1);
                    if (r <= 0) { c = EOF; } else { c = (unsigned char)ch; }

                    if (c == EOF) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                        continue;
                    }
                    if (c == 'v' || c == 'V') {
                        s_toggle_requested = true; // csak jelzés, a váltást a subscriber szál végzi!
                    }
                }
                restore();
            });
        }

        sub_util->subscribed = true;
    }

    // 4) FŐ CIKLUS — itt fut le a váltás (thread-safe, nincs Client hívás másik szálról)
    while (not stop) {
        if (moq_example::terminate) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Pending toggle?
        if (s_toggle_requested.load(std::memory_order_relaxed)) {
            // egy csomagban intézzük az Unsubscribe->Subscribe sorrendet
            std::string cur, next;
            std::shared_ptr<MySubscribeTrackHandler> cur_h, next_h;

            { // csak olvasáshoz/íráshoz védjük a contextet
                std::lock_guard<std::mutex> lk(s_vctx_mu);
                s_toggle_requested = false;

                if (s_vctx.video_names.size() >= 2 && s_vctx.active_idx >= 0) {
                    cur = s_vctx.video_names[s_vctx.active_idx];
                    auto cur_it = s_vctx.handler_by_name.find(cur);
                    if (cur_it != s_vctx.handler_by_name.end()) {
                        cur_h = cur_it->second;
                    }

                    s_vctx.active_idx = (s_vctx.active_idx + 1) % (int)s_vctx.video_names.size();
                    next = s_vctx.video_names[s_vctx.active_idx];
                    auto next_it = s_vctx.handler_by_name.find(next);
                    if (next_it != s_vctx.handler_by_name.end()) {
                        next_h = next_it->second;
                    }
                } else {
                    fprintf(stderr, "[toggle] Not enough video tracks or invalid active index.\n");
                }
            }

            // A tényleges Client-hívások a subscriber szálon:
            if (cur_h) {
                try { client->UnsubscribeTrack(cur_h); } catch (...) {}
            }
            if (next_h) {
                try { client->SubscribeTrack(next_h); } catch (...) {}
                fprintf(stderr, "[toggle] Switched VIDEO to: %s\n", next.c_str());
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 5) LEZÁRÁS: keyloop leállítás, unsubscribe
    s_keyloop_running = false;
    if (s_keyloop.joinable()) s_keyloop.join();

    // catalog le
    client->UnsubscribeTrack(catalog_track_handler);

    // non-video le
    for (auto [track_handler, trackDetails] : sub_track_handlers) {
        client->UnsubscribeTrack(track_handler);
    }

    // video le (bár a relay oldali unannounce kezelheti, explicit rendbetesszük)
    {
        std::lock_guard<std::mutex> lk(s_vctx_mu);
        for (auto& kv : s_vctx.handler_by_name) {
            if (kv.second) {
                try { client->UnsubscribeTrack(kv.second); } catch (...) {}
            }
        }
        s_vctx.handler_by_name.clear();
        s_vctx.video_names.clear();
        s_vctx.active_idx = -1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SPDLOG_INFO("Subscriber done track");
    moq_example::terminate = true;
}


/*===========================================================================*/
// Fetch thread to perform fetch
/*===========================================================================*/

struct Range
{
    uint64_t start;
    uint64_t end;
};

void
DoFetch(const quicr::FullTrackName& full_track_name,
        const Range& group_range,
        const Range& object_range,
        const std::shared_ptr<quicr::Client>& client,
        const bool& stop)
{
    auto track_handler = MyFetchTrackHandler::Create(
      full_track_name, group_range.start, object_range.start, group_range.end, object_range.end);

    SPDLOG_INFO("Started fetch");

    bool fetch_track{ false };

    while (not stop) {
        if ((!fetch_track) && (client->GetStatus() == MyClient::Status::kReady)) {
            SPDLOG_INFO("Fetching track");
            client->FetchTrack(track_handler);
            fetch_track = true;
        }

        if (track_handler->GetStatus() == quicr::FetchTrackHandler::Status::kPendingResponse) {
            // do nothing...
        } else if (!fetch_track || (track_handler->GetStatus() != quicr::FetchTrackHandler::Status::kOk)) {
            SPDLOG_INFO("GetStatus() != quicr::FetchTrackHandler::Status::kOk {}", (int)track_handler->GetStatus());
            moq_example::terminate = true;
            moq_example::cv.notify_all();
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    client->CancelFetchTrack(track_handler);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    moq_example::terminate = true;
}

/*===========================================================================*/
// Main program
/*===========================================================================*/

quicr::ClientConfig
InitConfig(cxxopts::ParseResult& cli_opts, bool& enable_pub, bool& enable_sub, bool& enable_fetch, bool& use_announce)
{
    quicr::ClientConfig config;

    std::string qlog_path;
    if (cli_opts.count("qlog")) {
        qlog_path = cli_opts["qlog"].as<std::string>();
    }

    if (cli_opts.count("debug") && cli_opts["debug"].as<bool>() == true) {
        SPDLOG_INFO("setting debug level");
        spdlog::set_level(spdlog::level::debug);
    }

    if (cli_opts.count("trace") && cli_opts["trace"].as<bool>() == true) {
        SPDLOG_INFO("setting trace level");
        spdlog::set_level(spdlog::level::trace);
    }

    if (cli_opts.count("version") && cli_opts["version"].as<bool>() == true) {
        SPDLOG_INFO("QuicR library version: {}", QUICR_VERSION);
        exit(0);
    }

    if (cli_opts.count("pub_namespace") && cli_opts.count("pub_name")) {
        enable_pub = true;
        SPDLOG_INFO("Publisher enabled using track namespace: {0} name: {1}",
                    cli_opts["pub_namespace"].as<std::string>(),
                    cli_opts["pub_name"].as<std::string>());
    }

    if (cli_opts.count("use_announce")) {
        use_announce = true;
        SPDLOG_INFO("Publisher will use announce flow");
    }

    if (cli_opts.count("clock") && cli_opts["clock"].as<bool>() == true) {
        SPDLOG_INFO("Running in clock publish mode");
        qclient_vars::publish_clock = true;
    }

    if (cli_opts.count("video") && cli_opts["video"].as<bool>() == true) {
        // SPDLOG_INFO("Running in video publish mode");
        std::cerr << "Running in video publish mode" << std::endl;
        qclient_vars::video = true;
    }

    if (cli_opts.count("sub_namespace") && cli_opts.count("sub_name")) {
        enable_sub = true;
        SPDLOG_INFO("Subscriber enabled using track namespace: {0} name: {1}",
                    cli_opts["sub_namespace"].as<std::string>(),
                    cli_opts["sub_name"].as<std::string>());
    }

    if (cli_opts.count("fetch_namespace") && cli_opts.count("fetch_name")) {
        enable_fetch = true;
        SPDLOG_INFO("Subscriber enabled using track namespace: {0} name: {1}",
                    cli_opts["fetch_namespace"].as<std::string>(),
                    cli_opts["fetch_name"].as<std::string>());
    }

    if (cli_opts.count("track_alias")) {
        qclient_vars::track_alias = cli_opts["track_alias"].as<uint64_t>();
    }

    if (cli_opts.count("record")) {
        qclient_vars::record = true;
    }

    if (cli_opts.count("playback")) {
        qclient_vars::playback = true;
    }

    if (cli_opts.count("gaps") && cli_opts["gaps"].as<bool>() == true) {
        SPDLOG_INFO("Adding gaps to group and objects");
        qclient_vars::add_gaps = true;
    }

    if (cli_opts.count("new_group")) {
        qclient_vars::new_group = true;
    }

    if (cli_opts.count("track_status")) {
        qclient_vars::req_track_status = true;
    }

    if (cli_opts.count("playback_speed_ms")) {
        qclient_vars::playback_speed_ms = std::chrono::milliseconds(cli_opts["playback_speed_ms"].as<uint64_t>());
    }

    if (cli_opts.count("ssl_keylog") && cli_opts["ssl_keylog"].as<bool>() == true) {
        SPDLOG_INFO("SSL Keylog enabled");
    }

    config.endpoint_id = cli_opts["endpoint_id"].as<std::string>();
    config.connect_uri = cli_opts["url"].as<std::string>();
    config.transport_config.debug = cli_opts["debug"].as<bool>();
    config.transport_config.ssl_keylog = cli_opts["ssl_keylog"].as<bool>();

    config.transport_config.use_reset_wait_strategy = false;
    config.transport_config.time_queue_max_duration = 5000;
    config.transport_config.tls_cert_filename = "";
    config.transport_config.tls_key_filename = "";
    config.transport_config.quic_qlog_path = qlog_path;

    return config;
}

int
main(int argc, char* argv[])
{
    // Initialize logger inside a function
    logger = spdlog::stderr_color_mt("err_logger");
    spdlog::set_default_logger(logger);

    int result_code = EXIT_SUCCESS;

    cxxopts::Options options("qclient",
                             std::string("MOQ Example Client using QuicR Version: ") + std::string(QUICR_VERSION));

    // clang-format off
    options.set_width(75)
      .set_tab_expansion()
      //.allow_unrecognised_options()
      .add_options()
        ("h,help", "Print help")
        ("d,debug", "Enable debugging") // a bool parameter
        ("t,trace", "Enable tracing") // a bool parameter
        ("v,version", "QuicR Version")                                        // a bool parameter
        ("r,url", "Relay URL", cxxopts::value<std::string>()->default_value("moq://localhost:1234"))
        ("e,endpoint_id", "This client endpoint ID", cxxopts::value<std::string>()->default_value("moq-client"))
        ("q,qlog", "Enable qlog using path", cxxopts::value<std::string>())
        ("s,ssl_keylog", "Enable SSL Keylog for transport debugging");

    options.add_options("Publisher")
        ("use_announce", "Use Announce flow instead of publish flow", cxxopts::value<bool>())
        ("track_alias", "Track alias to use", cxxopts::value<uint64_t>())
        ("pub_namespace", "Track namespace", cxxopts::value<std::string>())
        ("pub_name", "Track name", cxxopts::value<std::string>())
        ("clock", "Publish clock timestamp every second instead of using STDIN chat")
        ("playback", "Playback recorded data from moq and dat files", cxxopts::value<bool>())
        ("playback_speed_ms", "Playback speed in ms", cxxopts::value<std::uint64_t>())
        ("video", "Input MP4 video file (for publisher mode)")
        ("gaps", "Add gaps to groups and objects");

    options.add_options("Subscriber")
        ("sub_namespace", "Track namespace", cxxopts::value<std::string>())
        ("sub_name", "Track name", cxxopts::value<std::string>())
        ("start_point", "Start point for Subscription - 0 for from the beginning, 1 from the latest object", cxxopts::value<uint64_t>())
        ("sub_announces", "Prefix namespace to subscribe announces to", cxxopts::value<std::string>())
        ("record", "Record incoming data to moq and dat files", cxxopts::value<bool>())
        ("new_group", "Request new group on subscribe", cxxopts::value<bool>())
        ("joining_fetch", "Subscribe with a joining fetch using this joining start", cxxopts::value<std::uint64_t>())
        ("absolute", "Joining fetch will be absolute not relative", cxxopts::value<bool>())
        ("track_status", "Request track status using sub_namespace and sub_name options", cxxopts::value<bool>());

    options.add_options("Fetcher")
        ("fetch_namespace", "Track namespace", cxxopts::value<std::string>())
        ("fetch_name", "Track name", cxxopts::value<std::string>())
        ("start_group", "Starting group ID", cxxopts::value<uint64_t>())
        ("end_group", "One past the final group ID", cxxopts::value<uint64_t>())
        ("start_object", "The starting object ID within the group", cxxopts::value<uint64_t>())
        ("end_object", "One past the final object ID in the group", cxxopts::value<uint64_t>());

    // clang-format on

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help({ "", "Publisher", "Subscriber", "Fetcher" }) << std::endl;
        return EXIT_SUCCESS;
    }

    // Install a signal handlers to catch operating system signals
    installSignalHandlers();

    // Lock the mutex so that main can then wait on it
    std::unique_lock lock(moq_example::main_mutex);

    bool enable_pub{ false };
    bool enable_sub{ false };
    bool enable_fetch{ false };
    bool use_announce{ false };
    quicr::ClientConfig config = InitConfig(result, enable_pub, enable_sub, enable_fetch, use_announce);

    try {
        bool stop_threads{ false };
        auto client = MyClient::Create(config, stop_threads);

        if (client->Connect() != quicr::Transport::Status::kConnecting) {
            SPDLOG_ERROR("Failed to connect to server due to invalid params, check URI");
            exit(-1);
        }

        while (not stop_threads) {
            if (client->GetStatus() == MyClient::Status::kReady) {
                SPDLOG_INFO("Connected to server");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::thread pub_thread;
        std::thread sub_thread;
        std::thread fetch_thread;
        std::thread parse_thread;

        if (result.count("sub_announces")) {
            const auto& prefix_ns = quicr::example::MakeFullTrackName(result["sub_announces"].as<std::string>(), "");

            auto th = quicr::TrackHash(prefix_ns);

            SPDLOG_INFO("Sending subscribe announces for prefix '{}' namespace_hash: {}",
                         result["sub_announces"].as<std::string>(),
                         th.track_namespace_hash);

            client->SubscribeAnnounces(prefix_ns.name_space);
        }

        if (enable_pub) {
            if (!qclient_vars::video){
                const auto& pub_track_name = quicr::example::MakeFullTrackName(result["pub_namespace"].as<std::string>(),
                                                               result["pub_name"].as<std::string>());

                pub_thread = std::thread(DoPublisher, pub_track_name, client, use_announce, std::ref(stop_threads));
            } else {
                auto shared_state = std::make_shared<PublisherSharedState>();

                bool stop_parse{ false };

                parse_thread = std::thread(DoParse, shared_state, std::ref(stop_parse));

                shared_state->WaitForCatalogReady(stop_threads);

                if (result["pub_namespace"].as<std::string>() != "" ) {
                    shared_state->catalog.namespace_ = result["pub_namespace"].as<std::string>();
                } else {
                        SPDLOG_ERROR("No namespace specified for video publishing");
                        return EXIT_FAILURE;
                }

                pub_thread = std::thread(DoPublisher2, shared_state, client, use_announce, std::ref(stop_threads));
            }
        }
        if (enable_sub) {
            auto filter_type = quicr::messages::FilterType::kLargestObject;
            if (result.count("start_point")) {
                if (result["start_point"].as<uint64_t>() == 0) {
                    filter_type = quicr::messages::FilterType::kNextGroupStart;
                    SPDLOG_INFO("Setting subscription filter to Next Group Start");
                }
            }
            std::optional<std::uint64_t> joining_fetch;
            if (result.count("joining_fetch")) {
                joining_fetch = result["joining_fetch"].as<uint64_t>();
            }
            bool absolute = result.count("absolute") && result["absolute"].as<bool>();

            const auto& sub_track_name = quicr::example::MakeFullTrackName(result["sub_namespace"].as<std::string>(),
                                                                           result["sub_name"].as<std::string>());

            if (qclient_vars::req_track_status) {
                client->RequestTrackStatus(sub_track_name);
            }

            sub_thread = std::thread(
              DoSubscriber, result["sub_namespace"].as<std::string>(), client, filter_type, std::ref(stop_threads), joining_fetch, absolute);
        }
        if (enable_fetch) {
            const auto& fetch_track_name = quicr::example::MakeFullTrackName(
              result["fetch_namespace"].as<std::string>(), result["fetch_name"].as<std::string>());

            fetch_thread =
              std::thread(DoFetch,
                          fetch_track_name,
                          Range{ result["start_group"].as<uint64_t>(), result["end_group"].as<uint64_t>() },
                          Range{ result["start_object"].as<uint64_t>(), result["end_object"].as<uint64_t>() },
                          client,
                          std::ref(stop_threads));
        }

        // Wait until told to terminate
        moq_example::cv.wait(lock, [&]() { return moq_example::terminate; });

        stop_threads = true;
        SPDLOG_ERROR("Stopping threads...");
        if (parse_thread.joinable()) {
            parse_thread.join();
        }

        if (pub_thread.joinable()) {
            pub_thread.join();
        }

        if (sub_thread.joinable()) {
            sub_thread.join();
        }

        if (fetch_thread.joinable()) {
            fetch_thread.join();
        }

        client->Disconnect();

        SPDLOG_ERROR("Client done");
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    } catch (const std::invalid_argument& e) {
        std::cerr << "Invalid argument: " << e.what() << std::endl;
        result_code = EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "Unexpected exception: " << e.what() << std::endl;
        result_code = EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Unexpected exception" << std::endl;
        result_code = EXIT_FAILURE;
    }

    SPDLOG_INFO("Exit");

    return result_code;
}
