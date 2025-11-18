// SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
// SPDX-License-Identifier: BSD-2-Clause

#include "transcode_client.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace transcode {

/**
 * @brief Simple CMAF/MP4 box parser for splitting output stream
 *
 * Parses fragmented MP4 to extract:
 * - Init segment (ftyp + moov)
 * - Media fragments (moof + mdat pairs)
 */
class CMAFParser
{
  public:
    CMAFParser() = default;

    /**
     * @brief Feed data into the parser
     * @param data Input data
     * @param size Size of input data
     */
    void Feed(const uint8_t* data, size_t size)
    {
        buffer_.insert(buffer_.end(), data, data + size);
        Parse();
    }

    /**
     * @brief Set callback for init segment
     */
    void SetInitCallback(TranscodeClient::OutputInitCallback cb) { init_callback_ = std::move(cb); }

    /**
     * @brief Set callback for media fragments
     */
    void SetFragmentCallback(TranscodeClient::OutputFragmentCallback cb) { fragment_callback_ = std::move(cb); }

  private:
    /**
     * @brief Read 32-bit big-endian integer
     */
    static uint32_t ReadU32BE(const uint8_t* data) { return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]; }

    /**
     * @brief Read 4-character box type
     */
    static std::string ReadBoxType(const uint8_t* data) { return std::string(reinterpret_cast<const char*>(data), 4); }

    /**
     * @brief Parse accumulated buffer for complete boxes
     */
    void Parse()
    {
        while (buffer_.size() >= 8) {
            uint32_t box_size = ReadU32BE(buffer_.data());
            std::string box_type = ReadBoxType(buffer_.data() + 4);

            // Handle extended size (size == 1)
            if (box_size == 1) {
                if (buffer_.size() < 16) {
                    return; // Need more data for extended size
                }
                // Read 64-bit size (we'll still use 32-bit for simplicity, assuming sizes fit)
                box_size = ReadU32BE(buffer_.data() + 12); // Lower 32 bits
            }

            // Check if we have the complete box
            if (box_size > buffer_.size()) {
                return; // Need more data
            }

            // Handle different box types
            if (box_type == "ftyp") {
                init_segment_.clear();
                init_segment_.insert(init_segment_.end(), buffer_.begin(), buffer_.begin() + box_size);
                buffer_.erase(buffer_.begin(), buffer_.begin() + box_size);
            } else if (box_type == "moov") {
                // Append moov to init segment
                init_segment_.insert(init_segment_.end(), buffer_.begin(), buffer_.begin() + box_size);
                buffer_.erase(buffer_.begin(), buffer_.begin() + box_size);

                // Init segment is complete, invoke callback
                if (init_callback_ && !init_segment_.empty()) {
                    init_callback_(init_segment_.data(), init_segment_.size());
                    init_sent_ = true;
                }
            } else if (box_type == "moof") {
                // Start of a new fragment
                current_fragment_.clear();
                current_fragment_.insert(current_fragment_.end(), buffer_.begin(), buffer_.begin() + box_size);
                buffer_.erase(buffer_.begin(), buffer_.begin() + box_size);
            } else if (box_type == "mdat") {
                // Complete the fragment with mdat
                current_fragment_.insert(current_fragment_.end(), buffer_.begin(), buffer_.begin() + box_size);
                buffer_.erase(buffer_.begin(), buffer_.begin() + box_size);

                // Fragment is complete (moof + mdat), invoke callback
                if (fragment_callback_ && !current_fragment_.empty() && init_sent_) {
                    fragment_callback_(current_fragment_.data(), current_fragment_.size());
                }
                current_fragment_.clear();
            } else {
                // Unknown box, skip it
                if (box_size < 8)
                    box_size = 8; // Minimum box size
                buffer_.erase(buffer_.begin(), buffer_.begin() + std::min<size_t>(box_size, buffer_.size()));
            }
        }
    }

    std::vector<uint8_t> buffer_;
    std::vector<uint8_t> init_segment_;
    std::vector<uint8_t> current_fragment_;
    TranscodeClient::OutputInitCallback init_callback_;
    TranscodeClient::OutputFragmentCallback fragment_callback_;
    bool init_sent_ = false;
};

/**
 * @brief Private implementation of TranscodeClient
 */
class TranscodeClient::Impl
{
  public:
    explicit Impl(const TranscodeConfig& config)
      : config_(config)
    {
        // Initialize GStreamer
        static std::once_flag gst_init_flag;
        std::call_once(gst_init_flag, []() {
            gst_init(nullptr, nullptr);
            std::cout << "GStreamer initialized" << std::endl;
        });

        CreatePipeline();
    }

    ~Impl()
    {
        Stop();
        if (pipeline_) {
            gst_object_unref(pipeline_);
        }
    }

    void SetOutputInitCallback(OutputInitCallback callback) { parser_.SetInitCallback(std::move(callback)); }

    void SetOutputFragmentCallback(OutputFragmentCallback callback) { parser_.SetFragmentCallback(std::move(callback)); }

    void PushInputInit(const uint8_t* data, size_t size)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_buffer_.insert(input_buffer_.end(), data, data + size);
    }

    void PushInputFragment(const uint8_t* data, size_t size)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_buffer_.insert(input_buffer_.end(), data, data + size);
        PushDataToAppsrc();
    }

    bool Start()
    {
        if (!pipeline_) {
            std::cerr << "Pipeline not created" << std::endl;
            return false;
        }

        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "Failed to set pipeline to PLAYING state" << std::endl;
            return false;
        }

        running_ = true;
        std::cout << "Transcode pipeline started" << std::endl;
        return true;
    }

    void Stop()
    {
        if (!running_)
            return;

        running_ = false;

        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }

        std::cout << "Transcode pipeline stopped" << std::endl;
    }

    void Flush()
    {
        if (appsrc_) {
            gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        }
    }

    bool IsRunning() const { return running_; }

  private:
    void CreatePipeline()
    {
        // Create pipeline
        pipeline_ = gst_pipeline_new("transcode-pipeline");
        if (!pipeline_) {
            throw std::runtime_error("Failed to create pipeline");
        }

        // Create elements
        appsrc_ = gst_element_factory_make("appsrc", "source");
        GstElement* qtdemux = gst_element_factory_make("qtdemux", "demux");
        GstElement* queue1 = gst_element_factory_make("queue", "queue1");
        GstElement* h264parse = gst_element_factory_make("h264parse", "parser1");
        GstElement* avdec = gst_element_factory_make("avdec_h264", "decoder");
        GstElement* videoconvert = gst_element_factory_make("videoconvert", "convert");
        GstElement* videoscale = gst_element_factory_make("videoscale", "scale");
        GstElement* capsfilter = gst_element_factory_make("capsfilter", "filter");
        GstElement* encoder = CreateEncoder();
        GstElement* h264parse2 = gst_element_factory_make("h264parse", "parser2");
        GstElement* mp4mux = gst_element_factory_make("mp4mux", "muxer");
        appsink_ = gst_element_factory_make("appsink", "sink");

        if (!appsrc_ || !qtdemux || !queue1 || !h264parse || !avdec || !videoconvert || !videoscale || !capsfilter ||
            !encoder || !h264parse2 || !mp4mux || !appsink_) {
            throw std::runtime_error("Failed to create pipeline elements");
        }

        // Configure appsrc for live streaming
        g_object_set(G_OBJECT(appsrc_),
                     "is-live",
                     TRUE,
                     "format",
                     GST_FORMAT_TIME,
                     "stream-type",
                     0, // GST_APP_STREAM_TYPE_STREAM
                     "max-bytes",
                     static_cast<guint64>(10 * 1024 * 1024), // 10MB buffer
                     NULL);

        // Configure caps for scaling
        GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                            "width",
                                            G_TYPE_INT,
                                            config_.target_width,
                                            "height",
                                            G_TYPE_INT,
                                            config_.target_height,
                                            NULL);
        g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
        gst_caps_unref(caps);

        // Configure mp4mux for fragmented output
        g_object_set(G_OBJECT(mp4mux),
                     "fragment-duration",
                     1000, // 1 second fragments
                     "streamable",
                     TRUE,
                     NULL);

        // Configure appsink
        g_object_set(G_OBJECT(appsink_), "emit-signals", TRUE, "sync", FALSE, NULL);

        // Connect appsink callback
        g_signal_connect(appsink_, "new-sample", G_CALLBACK(OnNewSample), this);

        // Add elements to pipeline
        gst_bin_add_many(GST_BIN(pipeline_),
                         appsrc_,
                         qtdemux,
                         queue1,
                         h264parse,
                         avdec,
                         videoconvert,
                         videoscale,
                         capsfilter,
                         encoder,
                         h264parse2,
                         mp4mux,
                         appsink_,
                         NULL);

        // Link static elements (qtdemux pads will be linked dynamically)
        if (!gst_element_link(appsrc_, qtdemux)) {
            throw std::runtime_error("Failed to link appsrc -> qtdemux");
        }

        // Connect pad-added signal for dynamic linking
        g_signal_connect(qtdemux, "pad-added", G_CALLBACK(OnPadAdded), this);

        // Link the rest of the pipeline after queue1
        if (!gst_element_link_many(
              queue1, h264parse, avdec, videoconvert, videoscale, capsfilter, encoder, h264parse2, mp4mux, appsink_, NULL)) {
            throw std::runtime_error("Failed to link pipeline elements");
        }

        std::cout << "Transcode pipeline created successfully" << std::endl;
    }

    GstElement* CreateEncoder()
    {
        GstElement* encoder = nullptr;

        if (config_.codec == "x264") {
            encoder = gst_element_factory_make("x264enc", "encoder");
            if (encoder) {
                // Configure for low latency
                g_object_set(G_OBJECT(encoder),
                             "bitrate",
                             config_.target_bitrate / 1000, // x264enc uses kbps
                             "speed-preset",
                             1, // ultrafast
                             "tune",
                             0x00000004, // zerolatency
                             "key-int-max",
                             config_.gop_size,
                             NULL);
            }
        } else if (config_.codec == "x265") {
            encoder = gst_element_factory_make("x265enc", "encoder");
            if (encoder) {
                g_object_set(G_OBJECT(encoder), "bitrate", config_.target_bitrate / 1000, "speed-preset", 1, NULL);
            }
        } else {
            // Default to x264
            encoder = gst_element_factory_make("x264enc", "encoder");
        }

        if (!encoder) {
            throw std::runtime_error("Failed to create encoder: " + config_.codec);
        }

        return encoder;
    }

    static void OnPadAdded(GstElement* element, GstPad* pad, gpointer data)
    {
        auto* self = static_cast<Impl*>(data);
        GstElement* queue1 = gst_bin_get_by_name(GST_BIN(self->pipeline_), "queue1");

        if (!queue1) {
            std::cerr << "Failed to get queue1 element" << std::endl;
            return;
        }

        GstPad* sink_pad = gst_element_get_static_pad(queue1, "sink");
        if (!sink_pad) {
            std::cerr << "Failed to get queue1 sink pad" << std::endl;
            gst_object_unref(queue1);
            return;
        }

        if (gst_pad_is_linked(sink_pad)) {
            std::cout << "Pad already linked, ignoring" << std::endl;
            gst_object_unref(sink_pad);
            gst_object_unref(queue1);
            return;
        }

        // Check pad capabilities
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) {
            caps = gst_pad_query_caps(pad, NULL);
        }

        if (caps) {
            GstStructure* structure = gst_caps_get_structure(caps, 0);
            const gchar* name = gst_structure_get_name(structure);

            std::cout << "Pad added with caps: " << name << std::endl;

            // Link if it's a video pad
            if (g_str_has_prefix(name, "video/")) {
                GstPadLinkReturn ret = gst_pad_link(pad, sink_pad);
                if (ret != GST_PAD_LINK_OK) {
                    std::cerr << "Failed to link pads: " << ret << std::endl;
                } else {
                    std::cout << "Successfully linked qtdemux video pad" << std::endl;
                }
            }

            gst_caps_unref(caps);
        }

        gst_object_unref(sink_pad);
        gst_object_unref(queue1);
    }

    static GstFlowReturn OnNewSample(GstElement* sink, gpointer data)
    {
        auto* self = static_cast<Impl*>(data);

        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample) {
            return GST_FLOW_ERROR;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (buffer) {
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                // Feed data to CMAF parser
                self->parser_.Feed(map.data, map.size);
                gst_buffer_unmap(buffer, &map);
            }
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    void PushDataToAppsrc()
    {
        if (!appsrc_ || input_buffer_.empty()) {
            return;
        }

        // Create GStreamer buffer from input data
        GstBuffer* buffer = gst_buffer_new_allocate(NULL, input_buffer_.size(), NULL);
        if (!buffer) {
            std::cerr << "Failed to allocate GStreamer buffer" << std::endl;
            return;
        }

        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            std::memcpy(map.data, input_buffer_.data(), input_buffer_.size());
            gst_buffer_unmap(buffer, &map);

            // Set timestamp (PTS)
            GST_BUFFER_PTS(buffer) = timestamp_;
            GST_BUFFER_DTS(buffer) = timestamp_;
            GST_BUFFER_DURATION(buffer) = GST_CLOCK_TIME_NONE;

            // Increment timestamp for next buffer (assuming ~30fps, 33ms per frame)
            timestamp_ += 33 * GST_MSECOND;

            // Push to appsrc
            GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
            if (ret != GST_FLOW_OK) {
                std::cerr << "Failed to push buffer to appsrc: " << ret << std::endl;
            }

            input_buffer_.clear();
        } else {
            gst_buffer_unref(buffer);
            std::cerr << "Failed to map buffer" << std::endl;
        }
    }

    TranscodeConfig config_;
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstElement* appsink_ = nullptr;
    CMAFParser parser_;
    std::vector<uint8_t> input_buffer_;
    std::mutex mutex_;
    std::atomic<bool> running_{ false };
    guint64 timestamp_ = 0;
};

// TranscodeClient public interface implementation

TranscodeClient::TranscodeClient(const TranscodeConfig& config)
  : pImpl_(std::make_unique<Impl>(config))
{
}

TranscodeClient::~TranscodeClient() = default;

void
TranscodeClient::SetOutputInitCallback(OutputInitCallback callback)
{
    pImpl_->SetOutputInitCallback(std::move(callback));
}

void
TranscodeClient::SetOutputFragmentCallback(OutputFragmentCallback callback)
{
    pImpl_->SetOutputFragmentCallback(std::move(callback));
}

void
TranscodeClient::PushInputInit(const uint8_t* data, size_t size)
{
    pImpl_->PushInputInit(data, size);
}

void
TranscodeClient::PushInputFragment(const uint8_t* data, size_t size)
{
    pImpl_->PushInputFragment(data, size);
}

bool
TranscodeClient::Start()
{
    return pImpl_->Start();
}

void
TranscodeClient::Stop()
{
    pImpl_->Stop();
}

void
TranscodeClient::Flush()
{
    pImpl_->Flush();
}

bool
TranscodeClient::IsRunning() const
{
    return pImpl_->IsRunning();
}

} // namespace transcode
