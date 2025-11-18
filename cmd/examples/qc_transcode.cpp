// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "helper_functions.h"
#include "signal_handler.h"
#include "transcode_client.h"

#include <nlohmann/json.hpp>
#include <oss/cxxopts.hpp>
#include <quicr/cache.h>
#include <quicr/client.h>
#include <quicr/defer.h>
#include <quicr/object.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>

using json = nlohmann::json;

namespace qtranscode_vars {
bool debug = false;
uint32_t target_width = 1280;
uint32_t target_height = 720;
uint32_t target_bitrate = 0;
std::string encoder_preset = "medium";
std::string output_file_prefix;
std::shared_ptr<quicr::ThreadedTickService> tick_service = std::make_shared<quicr::ThreadedTickService>();
} // namespace qtranscode_vars

namespace qtranscode_consts {
const std::filesystem::path kOutputDir = std::filesystem::current_path() / "transcode_output";
} // namespace qtranscode_consts

/**
 * @brief Subscribe track handler with transcoding support
 * @details This handler receives CMAF fragments, transcodes them using the
 *          TranscodeClient, and outputs the transcoded stream.
 */
class TranscodeSubscribeTrackHandler : public quicr::SubscribeTrackHandler
{
  public:
    TranscodeSubscribeTrackHandler(const quicr::FullTrackName& full_track_name,
                                   quicr::messages::FilterType filter_type,
                                   const std::optional<JoiningFetch>& joining_fetch,
                                   uint32_t target_width,
                                   uint32_t target_height,
                                   bool publisher_initiated = false)
      : SubscribeTrackHandler(full_track_name,
                              3,
                              quicr::messages::GroupOrder::kAscending,
                              filter_type,
                              joining_fetch,
                              publisher_initiated)
      , target_width_(target_width)
      , target_height_(target_height)
    {
        // Create output directory
        std::filesystem::create_directories(qtranscode_consts::kOutputDir);

        // Initialize transcode client
        quicr::transcode::TranscodeConfig config;
        config.target_width = target_width;
        config.target_height = target_height;
        config.target_bitrate = qtranscode_vars::target_bitrate;
        config.encoder_preset = qtranscode_vars::encoder_preset;
        config.debug = qtranscode_vars::debug;

        transcode_client_ = quicr::transcode::TranscodeClient::Create(config);

        // Set up output callbacks
        transcode_client_->SetOutputInitCallback(
          [this](const uint8_t* data, size_t size) { HandleOutputInit(data, size); });

        transcode_client_->SetOutputFragmentCallback(
          [this](const uint8_t* data, size_t size) { HandleOutputFragment(data, size); });

        // Open output files if prefix specified
        if (!qtranscode_vars::output_file_prefix.empty()) {
            const auto output_path = qtranscode_consts::kOutputDir / (qtranscode_vars::output_file_prefix + ".mp4");
            output_file_.open(output_path, std::ios::binary | std::ios::trunc);
            if (output_file_) {
                SPDLOG_INFO("Writing transcoded output to: {}", output_path.string());
            }
        }

        SPDLOG_INFO("TranscodeSubscribeTrackHandler initialized for {}x{}", target_width, target_height);
    }

    virtual ~TranscodeSubscribeTrackHandler()
    {
        if (transcode_client_) {
            transcode_client_->Flush();
            transcode_client_->Close();
        }

        if (output_file_) {
            output_file_.close();
        }

        SPDLOG_INFO("Transcoded {} fragments ({} bytes input, {} bytes output)",
                    fragment_count_,
                    total_input_bytes_,
                    total_output_bytes_);
    }

    void ObjectReceived(const quicr::ObjectHeaders& hdr, quicr::BytesSpan data) override
    {
        total_input_bytes_ += data.size();

        SPDLOG_DEBUG("Received object Group:{}, Object:{}, Size:{} bytes", hdr.group_id, hdr.object_id, data.size());

        // Detect if this is an init segment or a media fragment
        // CMAF init segments start with ftyp box (0x66747970)
        // Media fragments start with moof box (0x6D6F6F66)
        if (data.size() >= 8) {
            uint32_t box_type = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];

            if (box_type == 0x66747970) { // 'ftyp' - init segment
                SPDLOG_INFO("Detected CMAF init segment (Group:{}, Object:{})", hdr.group_id, hdr.object_id);

                if (!transcode_client_->PushInputInit(data.data(), data.size())) {
                    SPDLOG_ERROR("Failed to push init segment: {}", transcode_client_->GetLastError());
                    return;
                }

                init_received_ = true;

            } else if (box_type == 0x6D6F6F66) { // 'moof' - media fragment
                if (!init_received_) {
                    SPDLOG_WARN("Received fragment before init segment, buffering...");
                    // In a production system, you might want to buffer these
                    return;
                }

                SPDLOG_DEBUG("Processing CMAF fragment (Group:{}, Object:{})", hdr.group_id, hdr.object_id);

                if (!transcode_client_->PushInputFragment(data.data(), data.size())) {
                    SPDLOG_ERROR("Failed to push fragment: {}", transcode_client_->GetLastError());
                    return;
                }

                fragment_count_++;

            } else {
                SPDLOG_WARN("Unknown box type: 0x{:08x}, size: {}", box_type, data.size());
            }
        }
    }

    void StatusChanged(Status status) override
    {
        switch (status) {
            case Status::kOk: {
                if (auto track_alias = GetTrackAlias(); track_alias.has_value()) {
                    SPDLOG_INFO("Transcode track alias: {} is ready to read", track_alias.value());
                }
            } break;

            case Status::kError:
                SPDLOG_ERROR("Transcode track error");
                break;

            default:
                break;
        }
    }

  private:
    void HandleOutputInit(const uint8_t* data, size_t size)
    {
        SPDLOG_INFO("Received transcoded init segment: {} bytes", size);

        // Write to file if enabled
        if (output_file_) {
            output_file_.write(reinterpret_cast<const char*>(data), size);
            output_file_.flush();
        }

        total_output_bytes_ += size;

        // In a real application, you would forward this to the publisher or next stage
        // For example:
        // PublishTranscodedInit(data, size);
    }

    void HandleOutputFragment(const uint8_t* data, size_t size)
    {
        SPDLOG_DEBUG("Received transcoded fragment: {} bytes", size);

        // Write to file if enabled
        if (output_file_) {
            output_file_.write(reinterpret_cast<const char*>(data), size);
            output_file_.flush();
        }

        total_output_bytes_ += size;

        // In a real application, you would forward this to the publisher or next stage
        // For example:
        // PublishTranscodedFragment(data, size);
    }

  private:
    uint32_t target_width_;
    uint32_t target_height_;
    std::shared_ptr<quicr::transcode::TranscodeClient> transcode_client_;
    bool init_received_{ false };
    size_t fragment_count_{ 0 };
    size_t total_input_bytes_{ 0 };
    size_t total_output_bytes_{ 0 };
    std::ofstream output_file_;
};

/**
 * @brief MoQ client with transcoding support
 */
class TranscodeClient : public quicr::Client
{
    TranscodeClient(const quicr::ClientConfig& cfg, bool& stop_threads)
      : quicr::Client(cfg)
      , stop_threads_(stop_threads)
    {
    }

  public:
    static std::shared_ptr<TranscodeClient> Create(const quicr::ClientConfig& cfg, bool& stop_threads)
    {
        return std::shared_ptr<TranscodeClient>(new TranscodeClient(cfg, stop_threads));
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
                SPDLOG_INFO("Connection failed {}", static_cast<int>(status));
                stop_threads_ = true;
                moq_example::terminate = true;
                moq_example::termination_reason = "Connection failed";
                moq_example::cv.notify_all();
                break;
        }
    }

  private:
    bool& stop_threads_;
};

/**
 * @brief Subscriber function with transcoding
 */
void
DoSubscriber(const quicr::FullTrackName& full_track_name,
             const std::shared_ptr<quicr::Client>& client,
             quicr::messages::FilterType filter_type,
             const bool& stop,
             uint32_t target_width,
             uint32_t target_height)
{
    const auto track_handler = std::make_shared<TranscodeSubscribeTrackHandler>(
      full_track_name, filter_type, std::nullopt, target_width, target_height);

    SPDLOG_INFO("Started transcode subscriber for {}x{}", target_width, target_height);

    bool subscribe_track{ false };

    while (not stop) {
        if ((!subscribe_track) && (client->GetStatus() == TranscodeClient::Status::kReady)) {
            SPDLOG_INFO("Subscribing to track for transcoding");
            client->SubscribeTrack(track_handler);
            subscribe_track = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    client->UnsubscribeTrack(track_handler);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SPDLOG_INFO("Transcode subscriber done");
    moq_example::terminate = true;
}

/**
 * @brief Initialize configuration from command line
 */
quicr::ClientConfig
InitConfig(cxxopts::ParseResult& cli_opts)
{
    quicr::ClientConfig config;

    std::string qlog_path;
    if (cli_opts.count("qlog")) {
        qlog_path = cli_opts["qlog"].as<std::string>();
    }

    if (cli_opts.count("debug") && cli_opts["debug"].as<bool>() == true) {
        SPDLOG_INFO("Setting debug level");
        spdlog::set_level(spdlog::level::debug);
        qtranscode_vars::debug = true;
    }

    if (cli_opts.count("version") && cli_opts["version"].as<bool>() == true) {
        SPDLOG_INFO("QuicR library version: {}", QUICR_VERSION);
        exit(0);
    }

    if (cli_opts.count("width")) {
        qtranscode_vars::target_width = cli_opts["width"].as<uint32_t>();
    }

    if (cli_opts.count("height")) {
        qtranscode_vars::target_height = cli_opts["height"].as<uint32_t>();
    }

    if (cli_opts.count("bitrate")) {
        qtranscode_vars::target_bitrate = cli_opts["bitrate"].as<uint32_t>();
    }

    if (cli_opts.count("preset")) {
        qtranscode_vars::encoder_preset = cli_opts["preset"].as<std::string>();
    }

    if (cli_opts.count("output")) {
        qtranscode_vars::output_file_prefix = cli_opts["output"].as<std::string>();
    }

    config.endpoint_id = cli_opts["endpoint_id"].as<std::string>();
    config.connect_uri = cli_opts["url"].as<std::string>();
    config.transport_config.debug = cli_opts["debug"].as<bool>();
    config.transport_config.ssl_keylog = cli_opts.count("ssl_keylog") && cli_opts["ssl_keylog"].as<bool>();

    config.transport_config.use_reset_wait_strategy = false;
    config.transport_config.time_queue_max_duration = 5000;
    config.transport_config.tls_cert_filename = "";
    config.transport_config.tls_key_filename = "";
    config.transport_config.quic_qlog_path = qlog_path;

    return config;
}

/**
 * @brief Main program
 */
int
main(int argc, char* argv[])
{
    int result_code = EXIT_SUCCESS;

    cxxopts::Options options(
      "qc_transcode",
      std::string("MOQ Transcode Client using QuicR Version: ") + std::string(QUICR_VERSION));

    // clang-format off
    options.set_width(75)
      .set_tab_expansion()
      .add_options()
        ("h,help", "Print help")
        ("d,debug", "Enable debugging")
        ("v,version", "QuicR Version")
        ("r,url", "Relay URL", cxxopts::value<std::string>()->default_value("moq://localhost:1234"))
        ("e,endpoint_id", "Client endpoint ID", cxxopts::value<std::string>()->default_value("moq-transcode"))
        ("q,qlog", "Enable qlog using path", cxxopts::value<std::string>())
        ("s,ssl_keylog", "Enable SSL Keylog for transport debugging");

    options.add_options("Transcode")
        ("sub_namespace", "Track namespace to subscribe", cxxopts::value<std::string>())
        ("sub_name", "Track name to subscribe", cxxopts::value<std::string>())
        ("width", "Target output width", cxxopts::value<uint32_t>()->default_value("1280"))
        ("height", "Target output height", cxxopts::value<uint32_t>()->default_value("720"))
        ("bitrate", "Target output bitrate (0=auto)", cxxopts::value<uint32_t>()->default_value("0"))
        ("preset", "Encoder preset (ultrafast, fast, medium, slow, veryslow)",
         cxxopts::value<std::string>()->default_value("medium"))
        ("output", "Output file prefix (saved to transcode_output/)", cxxopts::value<std::string>());

    // clang-format on

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help({ "", "Transcode" }) << std::endl;
        std::cout << "\nExample usage:" << std::endl;
        std::cout << "  qc_transcode --url moq://relay:1234 \\" << std::endl;
        std::cout << "               --sub_namespace video/camera1 \\" << std::endl;
        std::cout << "               --sub_name stream \\" << std::endl;
        std::cout << "               --width 1280 --height 720 \\" << std::endl;
        std::cout << "               --output transcoded_stream" << std::endl;
        return EXIT_SUCCESS;
    }

    if (!result.count("sub_namespace") || !result.count("sub_name")) {
        std::cerr << "Error: --sub_namespace and --sub_name are required" << std::endl;
        std::cerr << "Use --help for usage information" << std::endl;
        return EXIT_FAILURE;
    }

    // Install signal handlers
    installSignalHandlers();

    std::unique_lock lock(moq_example::main_mutex);

    quicr::ClientConfig config = InitConfig(result);

    SPDLOG_INFO("Starting MOQ Transcode Client");
    SPDLOG_INFO("Target resolution: {}x{}", qtranscode_vars::target_width, qtranscode_vars::target_height);
    SPDLOG_INFO("Encoder preset: {}", qtranscode_vars::encoder_preset);

    try {
        bool stop_threads{ false };
        auto client = TranscodeClient::Create(config, stop_threads);

        if (client->Connect() != quicr::Transport::Status::kConnecting) {
            SPDLOG_ERROR("Failed to connect to server due to invalid params, check URI");
            exit(-1);
        }

        while (not stop_threads) {
            if (client->GetStatus() == TranscodeClient::Status::kReady) {
                SPDLOG_INFO("Connected to server");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        // Start subscriber with transcoding
        const auto& sub_track_name =
          quicr::example::MakeFullTrackName(result["sub_namespace"].as<std::string>(),
                                            result["sub_name"].as<std::string>());

        std::thread sub_thread(DoSubscriber,
                               sub_track_name,
                               client,
                               quicr::messages::FilterType::kNextGroupStart,
                               std::ref(stop_threads),
                               qtranscode_vars::target_width,
                               qtranscode_vars::target_height);

        // Wait until told to terminate
        moq_example::cv.wait(lock, [&]() { return moq_example::terminate; });

        stop_threads = true;
        SPDLOG_INFO("Stopping transcode client...");

        if (sub_thread.joinable()) {
            sub_thread.join();
        }

        client->Disconnect();

        SPDLOG_INFO("Transcode client done");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

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
