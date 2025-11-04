//
// Minimal, self-contained GStreamer subscriber utility
// Implements: fixed video+audio pipelines, track init caching,
// and init-only switching on select.
//
// Created by ChatGPT on 2025-10-28
//
#include "subscriber_util.h"

#include <cstring>

static bool
caps_starts_with(GstCaps* caps, const char* prefix)
{
    if (!caps)
        return false;
    GstStructure* s = gst_caps_get_structure(caps, 0);
    if (!s)
        return false;
    const gchar* name = gst_structure_get_name(s);
    return name && g_str_has_prefix(name, prefix);
}

// -------------- BuildPipelines --------------
bool
SubscriberGst::BuildPipelines(const char* video_sink, const char* audio_sink)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!BuildVideoPipeline_(video_sink))
        return false;
    if (!BuildAudioPipeline_(audio_sink))
        return false;
    return true;
}

bool
SubscriberGst::StartPipelines(GstState s)
{
    std::lock_guard<std::mutex> lk(mtx_);
    bool ok = true;
    if (vpipe_)
        ok &= (gst_element_set_state(vpipe_, s) != GST_STATE_CHANGE_FAILURE);
    if (apipe_)
        ok &= (gst_element_set_state(apipe_, s) != GST_STATE_CHANGE_FAILURE);
    return ok;
}
bool
SubscriberGst::SetVideoState(GstState s)
{
    std::lock_guard<std::mutex> lk(mtx_);
    return vpipe_ && (gst_element_set_state(vpipe_, s) != GST_STATE_CHANGE_FAILURE);
}
bool
SubscriberGst::SetAudioState(GstState s)
{
    std::lock_guard<std::mutex> lk(mtx_);
    return apipe_ && (gst_element_set_state(apipe_, s) != GST_STATE_CHANGE_FAILURE);
}

// -------------- Video pipeline --------------
bool
SubscriberGst::BuildVideoPipeline_(const char* video_sink)
{
    vpipe_ = gst_pipeline_new("moq-vpipe");
    if (!vpipe_) {
        g_printerr("Failed to create vpipe\n");
        return false;
    }

    video_src_ = Make("appsrc", "video_src");
    vdemux_ = Make("qtdemux", "vdemux");
    vqueue_ = Make("queue", "vqueue");
    vdec_ = Make("decodebin", "vdec");
    vconv_ = Make("videoconvert", "vconv");
    vscale_ = Make("videoscale", "vscale");
    vsink_ = MakeOr(video_sink ? video_sink : "autovideosink", "vsink", "fakesink");

    if (!video_src_ || !vdemux_ || !vqueue_ || !vdec_ || !vconv_ || !vscale_ || !vsink_) {
        g_printerr("Missing video element(s)\n");
        return false;
    }

    g_object_set(video_src_, "is-live", TRUE, "format", GST_FORMAT_BYTES, "block", TRUE, NULL);
    g_object_set(vqueue_, "max-size-buffers", 0, "max-size-bytes", 0, "max-size-time", (gint64)2 * GST_SECOND, NULL);
    g_object_set(vsink_, "sync", FALSE, NULL);

    gst_bin_add_many(GST_BIN(vpipe_), video_src_, vdemux_, vqueue_, vdec_, vconv_, vscale_, vsink_, NULL);

    // Static links
    if (!gst_element_link(video_src_, vdemux_)) {
        g_printerr("link video_src->vdemux failed\n");
        return false;
    }
    if (!gst_element_link(vqueue_, vdec_)) {
        g_printerr("link vqueue->vdec failed\n");
        return false;
    }
    if (!gst_element_link_many(vconv_, vscale_, vsink_, NULL)) {
        g_printerr("link vconv->vscale->vsink failed\n");
        return false;
    }

    // Demux pad-added: link the first pad to vqueue sink (no caps filtering)
    g_signal_connect(vdemux_,
                     "pad-added",
                     G_CALLBACK(+[](GstElement* demux, GstPad* new_pad, gpointer user_data) {
                         GstElement* vqueue = GST_ELEMENT(user_data);
                         GstPad* sinkpad = gst_element_get_static_pad(vqueue, "sink");
                         if (!gst_pad_is_linked(sinkpad)) {
                             if (gst_pad_link(new_pad, sinkpad) != GST_PAD_LINK_OK) {
                                 g_printerr("vdemux pad link to vqueue failed\n");
                             }
                         }
                         gst_object_unref(sinkpad);
                     }),
                     vqueue_);

    // Decodebin pad-added: try to link to vconv; ignore failure (user ensures correct type)
    g_signal_connect(vdec_,
                     "pad-added",
                     G_CALLBACK(+[](GstElement* decodebin, GstPad* new_pad, gpointer user_data) {
                         GstElement* vconv = GST_ELEMENT(user_data);
                         GstPad* sinkpad = gst_element_get_static_pad(vconv, "sink");
                         if (!gst_pad_is_linked(sinkpad)) {
                             if (gst_pad_link(new_pad, sinkpad) != GST_PAD_LINK_OK) {
                                 g_printerr("vdec pad link to vconv failed (wrong media type?)\n");
                             }
                         }
                         gst_object_unref(sinkpad);
                     }),
                     vconv_);

    return true;
}

// -------------- Audio pipeline --------------
bool
SubscriberGst::BuildAudioPipeline_(const char* audio_sink)
{
    apipe_ = gst_pipeline_new("moq-apipe");
    if (!apipe_) {
        g_printerr("Failed to create apipe\n");
        return false;
    }

    audio_src_ = Make("appsrc", "audio_src");
    ademux_ = Make("qtdemux", "ademux");
    aqueue_ = Make("queue", "aqueue");
    adec_ = Make("decodebin", "adec");
    aconv_ = Make("audioconvert", "aconv");
    ares_ = Make("audioresample", "ares");
    asink_ = MakeOr(audio_sink ? audio_sink : "autoaudiosink", "asink", "autoaudiosink");

    if (!audio_src_ || !ademux_ || !aqueue_ || !adec_ || !aconv_ || !ares_ || !asink_) {
        g_printerr("Missing audio element(s)\n");
        return false;
    }

    g_object_set(audio_src_, "is-live", TRUE, "format", GST_FORMAT_BYTES, "block", TRUE, NULL);
    g_object_set(aqueue_, "max-size-buffers", 0, "max-size-bytes", 0, "max-size-time", (gint64)2 * GST_SECOND, NULL);

    gst_bin_add_many(GST_BIN(apipe_), audio_src_, ademux_, aqueue_, adec_, aconv_, ares_, asink_, NULL);

    if (!gst_element_link(audio_src_, ademux_)) {
        g_printerr("link audio_src->ademux failed\n");
        return false;
    }
    if (!gst_element_link(aqueue_, adec_)) {
        g_printerr("link aqueue->adec failed\n");
        return false;
    }
    if (!gst_element_link_many(aconv_, ares_, asink_, NULL)) {
        g_printerr("link aconv->ares->asink failed\n");
        return false;
    }

    // Demux pad-added: link first pad to aqueue
    g_signal_connect(ademux_,
                     "pad-added",
                     G_CALLBACK(+[](GstElement* demux, GstPad* new_pad, gpointer user_data) {
                         GstElement* aqueue = GST_ELEMENT(user_data);
                         GstPad* sinkpad = gst_element_get_static_pad(aqueue, "sink");
                         if (!gst_pad_is_linked(sinkpad)) {
                             if (gst_pad_link(new_pad, sinkpad) != GST_PAD_LINK_OK) {
                                 g_printerr("ademux pad link to aqueue failed\n");
                             }
                         }
                         gst_object_unref(sinkpad);
                     }),
                     aqueue_);

    // Decodebin pad-added: try to link to aconv; ignore failure
    g_signal_connect(adec_,
                     "pad-added",
                     G_CALLBACK(+[](GstElement* decodebin, GstPad* new_pad, gpointer user_data) {
                         GstElement* aconv = GST_ELEMENT(user_data);
                         GstPad* sinkpad = gst_element_get_static_pad(aconv, "sink");
                         if (!gst_pad_is_linked(sinkpad)) {
                             if (gst_pad_link(new_pad, sinkpad) != GST_PAD_LINK_OK) {
                                 g_printerr("adec pad link to aconv failed (wrong media type?)\n");
                             }
                         }
                         gst_object_unref(sinkpad);
                     }),
                     aconv_);

    return true;
}

// -------------- Register init --------------
bool
SubscriberGst::RegisterTrackInit(const std::string& track_name, bool is_video, const uint8_t* init, size_t init_len)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!init || init_len == 0)
        return false;
    auto& e = init_by_name_[track_name];
    e.bytes.assign(init, init + init_len);
    e.is_video = is_video;
    return true;
}

// -------------- Select (push only init) --------------
bool
SubscriberGst::SelectVideo(const std::string& track_name)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = init_by_name_.find(track_name);
    if (it == init_by_name_.end() || !it->second.is_video || !video_src_)
        return false;
    current_video_ = track_name;
    need_discont_video_ = true;
    GstBuffer* b = gst_buffer_new_allocate(NULL, it->second.bytes.size(), NULL);
    GstMapInfo mi;
    gst_buffer_map(b, &mi, GST_MAP_WRITE);
    std::memcpy(mi.data, it->second.bytes.data(), it->second.bytes.size());
    gst_buffer_unmap(b, &mi);
    return gst_app_src_push_buffer(GST_APP_SRC(video_src_), b) == GST_FLOW_OK;
}

bool
SubscriberGst::SelectAudio(const std::string& track_name)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = init_by_name_.find(track_name);
    if (it == init_by_name_.end() || it->second.is_video || !audio_src_)
        return false;
    current_audio_ = track_name;
    need_discont_audio_ = true;
    GstBuffer* b = gst_buffer_new_allocate(NULL, it->second.bytes.size(), NULL);
    GstMapInfo mi;
    gst_buffer_map(b, &mi, GST_MAP_WRITE);
    std::memcpy(mi.data, it->second.bytes.data(), it->second.bytes.size());
    gst_buffer_unmap(b, &mi);
    return gst_app_src_push_buffer(GST_APP_SRC(audio_src_), b) == GST_FLOW_OK;
}

// =============== Public: Push fragments ===============
bool
SubscriberGst::VideoPushFragment(const uint8_t* data, size_t len, bool is_rap)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!video_src_ || !data || len == 0)
        return false;

    GstBuffer* b = gst_buffer_new_allocate(NULL, len, NULL);
    GstMapInfo mi;
    gst_buffer_map(b, &mi, GST_MAP_WRITE);
    std::memcpy(mi.data, data, len);
    gst_buffer_unmap(b, &mi);

    bool dis = need_discont_video_;
    if (dis)
        GST_BUFFER_FLAG_SET(b, GST_BUFFER_FLAG_DISCONT);
    need_discont_video_ = false;
    if (!is_rap)
        GST_BUFFER_FLAG_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);

    return gst_app_src_push_buffer(GST_APP_SRC(video_src_), b) == GST_FLOW_OK;
}

bool
SubscriberGst::AudioPushFragment(const uint8_t* data, size_t len, bool is_rap)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!audio_src_ || !data || len == 0)
        return false;

    GstBuffer* b = gst_buffer_new_allocate(NULL, len, NULL);
    GstMapInfo mi;
    gst_buffer_map(b, &mi, GST_MAP_WRITE);
    std::memcpy(mi.data, data, len);
    gst_buffer_unmap(b, &mi);

    bool dis = need_discont_audio_;
    if (dis)
        GST_BUFFER_FLAG_SET(b, GST_BUFFER_FLAG_DISCONT);
    need_discont_audio_ = false;
    if (!is_rap)
        GST_BUFFER_FLAG_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);

    return gst_app_src_push_buffer(GST_APP_SRC(audio_src_), b) == GST_FLOW_OK;
}

// =============== Optional: explicit init push ===============
bool
SubscriberGst::VideoPushInit(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!video_src_ || !data || len == 0)
        return false;
    GstBuffer* b = gst_buffer_new_allocate(NULL, len, NULL);
    GstMapInfo mi;
    gst_buffer_map(b, &mi, GST_MAP_WRITE);
    std::memcpy(mi.data, data, len);
    gst_buffer_unmap(b, &mi);
    return gst_app_src_push_buffer(GST_APP_SRC(video_src_), b) == GST_FLOW_OK;
}

bool
SubscriberGst::AudioPushInit(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!audio_src_ || !data || len == 0)
        return false;
    GstBuffer* b = gst_buffer_new_allocate(NULL, len, NULL);
    GstMapInfo mi;
    gst_buffer_map(b, &mi, GST_MAP_WRITE);
    std::memcpy(mi.data, data, len);
    gst_buffer_unmap(b, &mi);
    return gst_app_src_push_buffer(GST_APP_SRC(audio_src_), b) == GST_FLOW_OK;
}

// =============== Accessors ===============
std::string
SubscriberGst::CurrentVideo() const
{
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mtx_));
    return current_video_;
}
std::string
SubscriberGst::CurrentAudio() const
{
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mtx_));
    return current_audio_;
}
