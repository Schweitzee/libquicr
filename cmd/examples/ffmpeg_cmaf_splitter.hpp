#pragma once
/**
 * FFmpeg-based CMAF per-track splitter & muxer
 *
 * Reads an input fMP4/CMAF stream (pipe, file, URL) with libavformat,
 * and for each A/V track creates an in-memory MP4 muxer configured for
 * fragmented output (ftyp+moov once, then moof+mdat fragments).
 *
 * For every track we call the provided callbacks:
 *  - on_init(track_index, buf, len)   once after write_header()
 *  - on_fragment(track_index, buf, len, is_key, group_id) on each av_write_frame()
 *
 * This is suitable for MoQ publishing where each track maps to a MoQ track.
 */
#include "ffmpeg_moq_adapter.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

extern "C" {
    #include "libavformat/avformat.h"
    }

struct FfmpegCmafSplitterConfig {
    std::string input_url = "pipe:0";
    int  io_buffer_kb = 32;
    std::string fflags = "nobuffer";
    int  probesize = 64*1024;
    int  analyzeduration_us = 0;
    std::string protocol_whitelist;

    bool use_custom_stdin = false;    // <- fájlból olvasáshoz KAPCSOLD KI

    // új: fragmentálás finomhangolás + tempózás fájlból
    bool frag_on_key = true;          // video: keyframe-szél mentén darabolás
    int  frag_duration_us = 500000;   // 500 ms
    int  min_frag_duration_us = 200000; // 200 ms
    bool realtime_pace = true;        // fájl → „életszerű” tempó

};

using OnInitFn = std::function<int(int /*stream_index*/, const uint8_t*, size_t, bool last_init)>;
using OnFragFn = std::function<int(int /*stream_index*/, const uint8_t*, size_t, bool is_key)>;

class FfmpegCmafSplitter {
public:
  explicit FfmpegCmafSplitter(const FfmpegCmafSplitterConfig& cfg, FfmpegToMoQAdapter& adapter);
  ~FfmpegCmafSplitter();

  int Run(const std::atomic<bool>& stop);

private:
  struct MemBuffer { std::vector<uint8_t> data; };
  struct TrackWriter {
    AVFormatContext* oc = nullptr;
    AVStream*        st = nullptr;
    AVIOContext*     pb = nullptr;
    uint8_t*         iobuf = nullptr;
    MemBuffer        mbuf;
    bool             header_written = false;
    ~TrackWriter();
  };

  static int mem_write(void* opaque, const uint8_t* buf, int buf_size);
  static int64_t mem_seek(void* opaque, int64_t offset, int whence);

  TrackWriter* make_writer_for_stream(AVStream* in_st);

  FfmpegCmafSplitterConfig cfg_;
  FfmpegToMoQAdapter& adapter_;
};
