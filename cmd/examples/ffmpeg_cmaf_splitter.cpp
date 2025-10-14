#include "ffmpeg_cmaf_splitter.hpp"
#include <iostream>
#include <map>
#include <stdexcept>

extern "C" {
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <unistd.h>  // read()

FfmpegCmafSplitter::FfmpegCmafSplitter(const FfmpegCmafSplitterConfig& cfg, FfmpegToMoQAdapter& adapter) : cfg_(cfg), adapter_(adapter)
{
  av_log_set_level(AV_LOG_QUIET);
}

FfmpegCmafSplitter::~FfmpegCmafSplitter() = default;

FfmpegCmafSplitter::TrackWriter::~TrackWriter() {
  if (oc) {
    av_write_trailer(oc);
    avformat_free_context(oc);
  }
  if (pb) {
    if (iobuf) av_free(iobuf);
    avio_context_free(&pb);
  }
}

int FfmpegCmafSplitter::mem_write(void* opaque, const uint8_t* buf, int buf_size) {
  auto* mb = reinterpret_cast<MemBuffer*>(opaque);
  mb->data.insert(mb->data.end(), buf, buf + buf_size);
  return buf_size;
}

int64_t FfmpegCmafSplitter::mem_seek(void*, int64_t, int) { return AVERROR(ENOSYS); }

FfmpegCmafSplitter::TrackWriter*
FfmpegCmafSplitter::make_writer_for_stream(AVStream* in_st) {
  auto* tw = new TrackWriter();

  if (avformat_alloc_output_context2(&tw->oc, nullptr, "mp4", nullptr) < 0 || !tw->oc) {
    delete tw; throw std::runtime_error("alloc output ctx failed");
  }
  // fragmented mp4 suitable for CMAF
  AVDictionary* movopts = nullptr;
    av_dict_set(&movopts, "movflags",
        "frag_keyframe+empty_moov+separate_moof+default_base_moof+dash+cmaf", 0);
  tw->st = avformat_new_stream(tw->oc, nullptr);
  if (!tw->st) { av_dict_free(&movopts); delete tw; throw std::runtime_error("new_stream failed"); }
  if (avcodec_parameters_copy(tw->st->codecpar, in_st->codecpar) < 0) {
    av_dict_free(&movopts); delete tw; throw std::runtime_error("copy codecpar failed");
  }
  tw->st->time_base = in_st->time_base;

  tw->iobuf = (uint8_t*)av_malloc(cfg_.io_buffer_kb * 1024);
  if (!tw->iobuf) { av_dict_free(&movopts); delete tw; throw std::runtime_error("no iobuf"); }

  tw->pb = avio_alloc_context(tw->iobuf, cfg_.io_buffer_kb * 1024, 1, &tw->mbuf, nullptr, &mem_write, &mem_seek);
  if (!tw->pb) { av_free(tw->iobuf); av_dict_free(&movopts); delete tw; throw std::runtime_error("no avio"); }
  tw->oc->pb = tw->pb;
  tw->oc->flags |= AVFMT_FLAG_CUSTOM_IO;

  if (avformat_write_header(tw->oc, &movopts) < 0) {
    av_dict_free(&movopts); delete tw; throw std::runtime_error("write_header failed");
  }
  av_dict_free(&movopts);
  tw->header_written = true;
  return tw;
}

int FfmpegCmafSplitter::run(const bool& stop) {
   int err;
AVFormatContext* ic = nullptr;
AVDictionary* opts = nullptr;

// gyorsabb felismerés, kisebb késleltetés
if (cfg_.probesize > 0)        av_dict_set_int(&opts, "probesize", cfg_.probesize, 0);
if (cfg_.analyzeduration_us>0) av_dict_set_int(&opts, "analyzeduration", cfg_.analyzeduration_us, 0);

// --- CUSTOM AVIO STDIN-RE ---
auto read_stdin = [](void* /*opaque*/, uint8_t* buf, int buf_size)->int {
  int r = ::read(STDIN_FILENO, buf, buf_size);
  if (r == 0)  return AVERROR_EOF;   // pipe closed
  if (r < 0)   return -errno;        // hiba
  return r;
};

unsigned char* iobuf = nullptr;
AVIOContext* avio = nullptr;

if (cfg_.use_custom_stdin) {
  const int io_buf_size = std::max(4*1024, cfg_.io_buffer_kb * 1024);
  iobuf = (unsigned char*)av_malloc(io_buf_size);
  if (!iobuf) {
    SPDLOG_ERROR("Failed to allocate AVIO buffer ({} bytes)", io_buf_size);
    return AVERROR(ENOMEM);
  }

  avio = avio_alloc_context(
      iobuf, io_buf_size,
      /*write_flag=*/0,
      /*opaque=*/nullptr,
      /*read_packet=*/read_stdin,
      /*write_packet=*/nullptr,
      /*seek=*/nullptr
  );
  if (!avio) {
    av_free(iobuf);
    SPDLOG_ERROR("avio_alloc_context failed");
    return AVERROR(ENOMEM);
  }

  ic = avformat_alloc_context();
  if (!ic) {
    av_freep(&avio->buffer);
    avio_context_free(&avio);
    SPDLOG_ERROR("avformat_alloc_context failed");
    return AVERROR(ENOMEM);
  }

  ic->pb = avio;
  ic->flags |= AVFMT_FLAG_CUSTOM_IO;

  int err = avformat_open_input(&ic, /*url*/nullptr, /*fmt*/nullptr, &opts);
  av_dict_free(&opts);
  if (err < 0) {
    char errstr[128]; av_strerror(err, errstr, sizeof(errstr));
    SPDLOG_ERROR("avformat_open_input(custom-stdin) failed: {} ({})", errstr, err);
    if (ic) avformat_close_input(&ic);
    if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    return err;
  }
} else {
  // fallback: URL-es megnyitás (ha egyszer engedélyezve lesznek a protokollok)
  int err = avformat_open_input(&ic, cfg_.input_url.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (err < 0) {
    char errstr[128]; av_strerror(err, errstr, sizeof(errstr));
    SPDLOG_ERROR("avformat_open_input('{}') failed: {} ({})",
                 cfg_.input_url, errstr, err);
    return err;
  }
}

  if ((err = avformat_find_stream_info(ic, nullptr)) < 0) { avformat_close_input(&ic); return err; }
  SPDLOG_INFO("CCCCCC");

  std::map<int, TrackWriter*> writers;
    SPDLOG_INFO("DDDDDD");

    std::vector<unsigned int> stream_indices_to_process;
    for (unsigned int si = 0; si < ic->nb_streams; ++si) {
        auto* st = ic->streams[si];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
            st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            stream_indices_to_process.push_back(si);
            }
    }

    SPDLOG_INFO("EEEEEEE");

    // Create per-stream writers & emit INIT
    for (size_t i = 0; i < stream_indices_to_process.size(); ++i) {
        const unsigned int si = stream_indices_to_process[i];
        auto* st = ic->streams[si];

        auto* tw = make_writer_for_stream(st);
        writers[static_cast<int>(si)] = tw;

        // INIT
            SPDLOG_INFO("trying to call oninit", si);
        if (!tw->mbuf.data.empty()) {
            // Az utolsó, ha az 'i' index elérte a feldolgozandó sávok számának végét.
            bool is_last_init = (i + 1 == stream_indices_to_process.size());
            adapter_.OnInit(static_cast<int>(si), tw->mbuf.data.data(), tw->mbuf.data.size(), is_last_init);
            tw->mbuf.data.clear();
            SPDLOG_INFO("Oninit called {}", si);
        }
        else {

        }
        writers[static_cast<int>(si)] = tw;
    }

  AVPacket* pkt = av_packet_alloc();
  int group_id = 0;

  while ((err = av_read_frame(ic, pkt)) >= 0 && !stop) {
    auto it = writers.find(pkt->stream_index);
    if (it == writers.end()) { av_packet_unref(pkt); continue; }
    auto* tw = it->second;

    bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

    pkt->stream_index = 0; // one output stream in each TrackWriter
    if ((err = av_write_frame(tw->oc, pkt)) < 0) { av_packet_unref(pkt); break; }
    av_packet_unref(pkt);

    if (!tw->mbuf.data.empty()) {
      adapter_.OnFrag(it->first, tw->mbuf.data.data(), tw->mbuf.data.size(), is_key);
      tw->mbuf.data.clear();
    }
  }

  for (auto& kv : writers) delete kv.second;
  avformat_close_input(&ic);
  return err == AVERROR_EOF ? 0 : err;
}
