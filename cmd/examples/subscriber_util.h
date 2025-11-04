//
// Minimal, self-contained GStreamer subscriber utility
// Implements: fixed video+audio pipelines, track init caching,
// and init-only switching on select.
//
// Created by ChatGPT on 2025-10-28
//
#pragma once

#ifndef QUICR_SUBSCRIBER_UTIL_H
#define QUICR_SUBSCRIBER_UTIL_H

#include "catalog.hpp"

#include <cstdint>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class MySubscribeTrackHandler;

struct SubTrack
{
    CatalogTrackEntry track_entry;
    std::string namespace_;
    std::vector<uint8_t> init;
};

class SubscriberUtil
{

  public:
    Catalog catalog;
    std::map<std::shared_ptr<MySubscribeTrackHandler>, std::shared_ptr<SubTrack>> sub_tracks;
    bool catalog_read = false;
    bool subscribed = false;

    SubscriberUtil() {};
    ~SubscriberUtil() {};
};

// --- Track cache entry ---
struct CachedInit
{
    std::vector<uint8_t> bytes;
    bool is_video{ false };
};

// --- Main class ---
class SubscriberGst
{
  public:
    SubscriberGst() = default;

    ~SubscriberGst()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (vpipe_) {
            gst_element_set_state(vpipe_, GST_STATE_NULL);
            gst_object_unref(vpipe_);
            vpipe_ = nullptr;
        }
        if (apipe_) {
            gst_element_set_state(apipe_, GST_STATE_NULL);
            gst_object_unref(apipe_);
            apipe_ = nullptr;
        }
        video_src_ = nullptr;
        audio_src_ = nullptr;
    }
    // Build independent pipelines: one for video and one for audio
    bool BuildPipelines(const char* video_sink = "autovideosink", const char* audio_sink = "autoaudiosink");

    // Optional state controls
    bool StartPipelines(GstState s = GST_STATE_PLAYING);
    bool SetVideoState(GstState s);
    bool SetAudioState(GstState s);

    // Cache track init (called when catalog arrives)
    bool RegisterTrackInit(const std::string& track_name, bool is_video, const uint8_t* init, size_t init_len);

    // Select a track: pushes only the cached init to the corresponding appsrc
    bool SelectVideo(const std::string& track_name);
    bool SelectAudio(const std::string& track_name);

    // Push media fragments (moof+mdat bytes). Use after a select.
    bool VideoPushFragment(const uint8_t* data, size_t len, bool is_rap);

    bool AudioPushFragment(const uint8_t* data, size_t len, bool is_rap);

    // (Optional helper) Explicitly push an init (rarely needed if you use Select*)
    bool VideoPushInit(const uint8_t* data, size_t len);
    bool AudioPushInit(const uint8_t* data, size_t len);

    // Current selected names (empty if none)
    std::string CurrentVideo() const;
    std::string CurrentAudio() const;

  private:
    std::mutex mtx_;

    GstElement* vpipe_{ nullptr };
    GstElement* apipe_{ nullptr };

    GstElement* video_src_{ nullptr };
    GstElement* audio_src_{ nullptr };

    // We keep references to select internal elements for debugging if needed.
    GstElement* vtypefind_{ nullptr };
    GstElement* vdemux_{ nullptr };
    GstElement* vqueue_{ nullptr };
    GstElement* vparse_{ nullptr };
    GstElement* vdec_{ nullptr };
    GstElement* vconv_{ nullptr };
    GstElement* vscale_{ nullptr };
    GstElement* vsink_{ nullptr };

    GstElement* atypefind_{ nullptr };
    GstElement* ademux_{ nullptr };
    GstElement* aqueue_{ nullptr };
    GstElement* aparse_{ nullptr };
    GstElement* adec_{ nullptr };
    GstElement* aconv_{ nullptr };
    GstElement* ares_{ nullptr };
    GstElement* asink_{ nullptr };

    // Cached inits by track name
    std::unordered_map<std::string, CachedInit> init_by_name_;

    // Currently selected tracks
    std::string current_video_;
    std::string current_audio_;

    // Next fragment after select should be DISCONT
    bool need_discont_video_{ false };
    bool need_discont_audio_{ false };

    // Internal builders
    bool BuildVideoPipeline_(const char* video_sink);
    bool BuildAudioPipeline_(const char* audio_sink);

    static inline GstElement* Make(const char* factory, const char* name)
    {
        return gst_element_factory_make(factory, name);
    }

    static inline GstElement* MakeOr(const char* primary, const char* name, const char* fallback)
    {
        GstElement* e = gst_element_factory_make(primary, name);
        if (!e)
            e = gst_element_factory_make(fallback, name);
        return e;
    }

    // Utilities
    static inline GstClockTime ticks_to_ns(uint64_t t, uint64_t timescale)
    {
        __int128 num = (__int128)t * GST_SECOND;
        return (GstClockTime)(num / (timescale ? timescale : 1));
    }
};

#endif // QUICR_SUBSCRIBER_UTIL_H
