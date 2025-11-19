#include "ffmpeg_cmaf_splitter.hpp"
#include <iostream>
#include <map>
#include <stdexcept>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <unistd.h> // read()

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h> // <- ez kell az av_opt_set-hez
}

#include <cstddef>
#include <cstdint>
#include <string>

static inline uint32_t
be32(const uint8_t* b)
{
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}

static inline bool
box_fits(size_t off, size_t size, size_t len)
{
    if (size < 8)
        return false;
    return off + size <= len;
}

// ---- belső segéd: kulcskép-eldöntés a flag-ekből ----
static inline bool
sample_is_sync(uint32_t f)
{
    // ISO BMFF 8.8.3 mezők:
    // [25:24] sample_depends_on, [16] sample_is_non_sync_sample
    const bool is_non_sync = ((f >> 16) & 1) != 0;
    const uint32_t depends_on = (f >> 24) & 3;
    if (!is_non_sync)
        return true;        // gyakorlatban elég: ha nem non-sync, akkor sync
    return depends_on == 2; // extra óvatosság, ha jelölve van
}

// ---- belső segéd: egyetlen MOOF bejárása ----
static bool
moof_has_keyframe(const uint8_t* buf, size_t moof_off, size_t moof_size, size_t len)
{
    size_t off = moof_off + 8; // "moof" FullBox header nélkül (itt sima box: size+type)
    const size_t moof_end = moof_off + moof_size;

    while (off + 8 <= moof_end) {
        if (off + 8 > len)
            return false;
        uint32_t box_size = be32(buf + off);
        if (box_size < 8 || !box_fits(off, box_size, len))
            break;
        const char* typep = reinterpret_cast<const char*>(buf + off + 4);
        uint32_t type =
          (uint32_t(typep[0]) << 24) | (uint32_t(typep[1]) << 16) | (uint32_t(typep[2]) << 8) | uint32_t(typep[3]);

        // "traf"
        if (type == 0x74726166u) {
            // Első kör: tfhd kiolvasása (default_sample_flags-hez)
            uint32_t tfhd_flags = 0;
            uint32_t tfhd_default_sample_flags = 0;
            bool tfhd_has_default_flags = false;

            size_t tOff = off + 8;
            const size_t traf_end = off + box_size;

            // tfhd keresése
            while (tOff + 8 <= traf_end) {
                if (tOff + 8 > len)
                    break;
                uint32_t isz = be32(buf + tOff);
                if (isz < 8 || !box_fits(tOff, isz, len))
                    break;
                const char* itypep = reinterpret_cast<const char*>(buf + tOff + 4);
                uint32_t itype = (uint32_t(itypep[0]) << 24) | (uint32_t(itypep[1]) << 16) |
                                 (uint32_t(itypep[2]) << 8) | uint32_t(itypep[3]);

                if (itype == 0x74666864u) { // "tfhd" (FullBox)
                    size_t p = tOff + 8;
                    if (p + 4 > len)
                        break;
                    // version(1) + flags(3)
                    uint32_t vf = (uint32_t(buf[p + 1]) << 16) | (uint32_t(buf[p + 2]) << 8) | uint32_t(buf[p + 3]);
                    tfhd_flags = vf;
                    p += 4;

                    // track_ID (kötelező)
                    if (p + 4 > len)
                        break;
                    // uint32_t tfhd_track_id = be32(buf + p); // nem kell szűrni, bármelyik track jó
                    p += 4;

                    // opcionális mezők 8.8.7 szerint
                    if (tfhd_flags & 0x000001)
                        p += 8; // base_data_offset
                    if (tfhd_flags & 0x000002)
                        p += 4; // sample_description_index
                    if (tfhd_flags & 0x000008)
                        p += 4; // default_sample_duration
                    if (tfhd_flags & 0x000010)
                        p += 4;                  // default_sample_size
                    if (tfhd_flags & 0x000020) { // default_sample_flags
                        if (p + 4 > len)
                            break;
                        tfhd_default_sample_flags = be32(buf + p);
                        tfhd_has_default_flags = true;
                        p += 4;
                    }
                    // tfhd megvolt, lépünk tovább
                }

                if (isz == 0)
                    break;
                tOff += isz;
            }

            // Második kör: trun-ok feldolgozása
            tOff = off + 8;
            while (tOff + 8 <= traf_end) {
                if (tOff + 8 > len)
                    break;
                uint32_t isz = be32(buf + tOff);
                if (isz < 8 || !box_fits(tOff, isz, len))
                    break;
                const char* itypep = reinterpret_cast<const char*>(buf + tOff + 4);
                uint32_t itype = (uint32_t(itypep[0]) << 24) | (uint32_t(itypep[1]) << 16) |
                                 (uint32_t(itypep[2]) << 8) | uint32_t(itypep[3]);

                if (itype == 0x7472756Eu) { // "trun" (FullBox)
                    size_t p = tOff + 8;
                    if (p + 4 > len)
                        break;
                    // version(1) + flags(3)
                    // uint8_t version = buf[p];
                    uint32_t trun_flags =
                      (uint32_t(buf[p + 1]) << 16) | (uint32_t(buf[p + 2]) << 8) | uint32_t(buf[p + 3]);
                    p += 4;

                    if (p + 4 > len)
                        break;
                    uint32_t sample_count = be32(buf + p);
                    p += 4;

                    bool data_offset_present = (trun_flags & 0x000001) != 0;
                    bool first_sample_flags_present = (trun_flags & 0x000004) != 0;
                    bool has_sample_duration = (trun_flags & 0x000100) != 0;
                    bool has_sample_size = (trun_flags & 0x000200) != 0;
                    bool has_sample_flags = (trun_flags & 0x000400) != 0;
                    bool has_sample_cto = (trun_flags & 0x000800) != 0;

                    if (data_offset_present) {
                        if (p + 4 > len)
                            break;
                        p += 4;
                    }

                    uint32_t first_sample_flags = 0;
                    if (first_sample_flags_present) {
                        if (p + 4 > len)
                            break;
                        first_sample_flags = be32(buf + p);
                        p += 4;
                    }

                    // minták bejárása
                    size_t cur = p;
                    for (uint32_t i = 0; i < sample_count; ++i) {
                        size_t rec = cur;
                        uint32_t flags_for_sample = 0;
                        bool got_flags = false;

                        if (has_sample_duration) {
                            if (rec + 4 > len)
                                break;
                            rec += 4;
                        }
                        if (has_sample_size) {
                            if (rec + 4 > len)
                                break;
                            rec += 4;
                        }

                        if (has_sample_flags) {
                            if (rec + 4 > len)
                                break;
                            flags_for_sample = be32(buf + rec);
                            rec += 4;
                            got_flags = true;
                        } else if (i == 0 && first_sample_flags_present) {
                            flags_for_sample = first_sample_flags;
                            got_flags = true;
                        } else if (tfhd_has_default_flags) {
                            flags_for_sample = tfhd_default_sample_flags;
                            got_flags = true;
                        } else {
                            // formálisan: trex.default_sample_flags (INIT-ből)
                        }

                        if (has_sample_cto) {
                            if (rec + 4 > len)
                                break;
                            rec += 4;
                        }

                        if (got_flags && sample_is_sync(flags_for_sample)) {
                            return true; // bármelyik track-ben találtunk kulcsképet
                        }

                        // a következő rekord elejére ugrás
                        cur += (has_sample_duration ? 4 : 0) + (has_sample_size ? 4 : 0) + (has_sample_flags ? 4 : 0) +
                               (has_sample_cto ? 4 : 0);
                    }
                }

                if (isz == 0)
                    break;
                tOff += isz;
            }
        }

        if (box_size == 0)
            break;
        off += box_size;
    }
    return false;
}

// ---- publikus: teljes fragment (moof+mdat) vizsgálata ----
bool
CmafChunkHasKeyframe(const uint8_t* buf, size_t len)
{
    size_t off = 0;
    while (off + 8 <= len) {
        uint32_t box_size = be32(buf + off);
        if (box_size < 8 || !box_fits(off, box_size, len))
            break;
        const char* typep = reinterpret_cast<const char*>(buf + off + 4);
        uint32_t type =
          (uint32_t(typep[0]) << 24) | (uint32_t(typep[1]) << 16) | (uint32_t(typep[2]) << 8) | uint32_t(typep[3]);

        if (type == 0x6D6F6F66u) { // "moof"
            if (moof_has_keyframe(buf, off, box_size, len))
                return true;
        }

        if (box_size == 0)
            break;
        off += box_size;
    }
    return false;
}

FfmpegCmafSplitter::FfmpegCmafSplitter(const FfmpegCmafSplitterConfig& cfg, FfmpegToMoQAdapter& adapter)
  : cfg_(cfg)
  , adapter_(adapter)
{
    av_log_set_level(AV_LOG_QUIET);
}

FfmpegCmafSplitter::~FfmpegCmafSplitter() = default;

FfmpegCmafSplitter::TrackWriter::~TrackWriter()
{
    if (oc) {
        av_write_trailer(oc);
        avformat_free_context(oc);
    }
    if (pb) {
        if (iobuf)
            av_free(iobuf);
        avio_context_free(&pb);
    }
}

int
FfmpegCmafSplitter::mem_write(void* opaque, const uint8_t* buf, int buf_size)
{
    auto* mb = reinterpret_cast<MemBuffer*>(opaque);
    mb->data.insert(mb->data.end(), buf, buf + buf_size);
    return buf_size;
}

int64_t
FfmpegCmafSplitter::mem_seek(void*, int64_t, int)
{
    return AVERROR(ENOSYS);
}

FfmpegCmafSplitter::TrackWriter*
FfmpegCmafSplitter::make_writer_for_stream(AVStream* in_st)
{
    auto* tw = new TrackWriter();

    if (avformat_alloc_output_context2(&tw->oc, nullptr, "mp4", nullptr) < 0 || !tw->oc) {
        delete tw;
        throw std::runtime_error("alloc output ctx failed");
    }
    AVDictionary* movopts = nullptr;

    // Alap movflags
    std::string flags = "empty_moov+cmaf+separate_moof+skip_trailer+faststart";
    if (in_st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        flags += "+frag_every_frame";
        av_dict_set_int(&movopts, "video_track_timescale", 90000, 0);
    }
    av_dict_set(&movopts, "movflags", flags.c_str(), 0);

    // Flush viselkedés
    av_dict_set(&movopts, "flush_packets", "1", 0);

    // Interleave késleltetés nullázása
    av_opt_set_int(tw->oc, "max_interleave_delta", 0, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(tw->oc, "muxpreload", 0, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(tw->oc, "muxdelay", 0, AV_OPT_SEARCH_CHILDREN);

    tw->st = avformat_new_stream(tw->oc, nullptr);
    if (!tw->st) {
        av_dict_free(&movopts);
        delete tw;
        throw std::runtime_error("new_stream failed");
    }
    if (avcodec_parameters_copy(tw->st->codecpar, in_st->codecpar) < 0) {
        av_dict_free(&movopts);
        delete tw;
        throw std::runtime_error("copy codecpar failed");
    }
    tw->st->time_base = in_st->time_base;

    tw->iobuf = (uint8_t*)av_malloc(cfg_.io_buffer_kb * 1024);
    if (!tw->iobuf) {
        av_dict_free(&movopts);
        delete tw;
        throw std::runtime_error("no iobuf");
    }

    tw->pb = avio_alloc_context(tw->iobuf, cfg_.io_buffer_kb * 1024, 1, &tw->mbuf, nullptr, &mem_write, &mem_seek);
    if (!tw->pb) {
        av_free(tw->iobuf);
        av_dict_free(&movopts);
        delete tw;
        throw std::runtime_error("no avio");
    }
    tw->oc->pb = tw->pb;
    tw->oc->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_write_header(tw->oc, &movopts) < 0) {
        av_dict_free(&movopts);
        delete tw;
        throw std::runtime_error("write_header failed");
    }
    av_dict_free(&movopts);
    avio_flush(tw->oc->pb);
    tw->header_written = true;
    return tw;
}

int
FfmpegCmafSplitter::Run(const std::atomic<bool>& stop)
{
    int err;
    AVFormatContext* ic = nullptr;
    AVDictionary* opts = nullptr;

    if (!cfg_.protocol_whitelist.empty())
        av_dict_set(&opts, "protocol_whitelist", cfg_.protocol_whitelist.c_str(), 0);
    if (cfg_.probesize > 0)
        av_dict_set_int(&opts, "probesize", cfg_.probesize, 0);
    if (cfg_.analyzeduration_us > 0)
        av_dict_set_int(&opts, "analyzeduration", cfg_.analyzeduration_us, 0);
    av_dict_set(&opts, "ignore_editlist", "1", 0);
    av_dict_set(&opts, "flush_packets", "1", 0);
    av_dict_set(&opts, "fflags", "nobuffer+fastseek+flush_packets", 0);

    if (cfg_.use_custom_stdin) {
        // (ha később mégis kell cső, itt MARADHAT a működő custom-AVIO út:
        // ic = avformat_alloc_context(); ic->pb = avio; ic->flags |= AVFMT_FLAG_CUSTOM_IO;
        // err = avformat_open_input(&ic, nullptr, nullptr, &opts);
    }

    // ---- FÁJLBÓL OLVASÁS ----
    if (!ic) { // normál fájlos út
        err = avformat_open_input(&ic, cfg_.input_url.c_str(), nullptr, &opts);
    }
    av_dict_free(&opts);

    if (err < 0) {
        char e[128];
        av_strerror(err, e, sizeof(e));
        SPDLOG_ERROR("avformat_open_input('{}') failed: {} ({})", cfg_.input_url, e, err);
        return err;
    }

    if ((err = avformat_find_stream_info(ic, nullptr)) < 0) {
        avformat_close_input(&ic);
        return err;
    }

    SPDLOG_INFO("Input opened: '{}' ({} streams)", cfg_.input_url, ic->nb_streams);

    std::map<uint32_t, TrackWriter*> writers;

    std::vector<unsigned int> stream_indices_to_process;
    for (unsigned int si = 0; si < ic->nb_streams; ++si) {
        auto* st = ic->streams[si];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            stream_indices_to_process.push_back(si);
        }
    }
    SPDLOG_INFO("nb_streams = {}", ic->nb_streams);
    if (ic->nb_streams == 0) {
        SPDLOG_ERROR("No streams found in input.");
        return err;
    }

    // Create per-stream writers & emit INIT
    for (size_t i = 0; i < stream_indices_to_process.size(); ++i) {
        const unsigned int si = stream_indices_to_process[i];
        auto* st = ic->streams[si];

        auto* tw = make_writer_for_stream(st);
        writers[static_cast<int>(si)] = tw;

        // INIT
        SPDLOG_INFO("trying to call oninit {}", si);
        if (!tw->mbuf.data.empty()) {
            // Az utolsó, ha az 'i' index elérte a feldolgozandó sávok számának végét.
            bool is_last_init = (i + 1 == stream_indices_to_process.size());
            adapter_.OnInit(static_cast<int>(si), tw->mbuf.data.data(), tw->mbuf.data.size(), is_last_init);
            tw->mbuf.data.clear();
            SPDLOG_INFO("Oninit called {}", si);
        } else {
        }
        writers[static_cast<int>(si)] = tw;
    }

    using clock = std::chrono::steady_clock;

    double start_ts = NAN;
    auto wall0 = clock::now();

    auto pkt_ts_sec = [&](AVPacket* p) -> double {
        auto* st = ic->streams[p->stream_index];
        AVRational tb = st->time_base;
        int64_t t = (p->dts != AV_NOPTS_VALUE) ? p->dts : p->pts;
        if (t == AV_NOPTS_VALUE)
            return NAN;
        return t * av_q2d(tb);
    };

    AVPacket* pkt = av_packet_alloc();
    while ((err = av_read_frame(ic, pkt)) >= 0 && !stop.load(std::memory_order_relaxed)) {
        if (cfg_.realtime_pace) {
            if (double ts = pkt_ts_sec(pkt); !std::isnan(ts)) {
                if (std::isnan(start_ts))
                    start_ts = ts;
                double target_ms = (ts - start_ts) * 1000.0;
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - wall0).count();
                if (target_ms > elapsed_ms + 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds((long)(target_ms - elapsed_ms)));
                }
            }
        }

        auto it = writers.find(pkt->stream_index);
        if (it == writers.end()) {
            av_packet_unref(pkt);
            continue;
        }
        auto* tw = it->second;

        pkt->stream_index = 0; // egyetlen kimenő sáv/TrackWriter
        if ((err = av_write_frame(tw->oc, pkt)) < 0) {
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);

        auto iskeyframe = false;

        if (tw->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            iskeyframe = CmafChunkHasKeyframe(tw->mbuf.data.data(), tw->mbuf.data.size());
        } else {
            iskeyframe = true;
        }

        if (!tw->mbuf.data.empty()) {
            adapter_.OnFrag(it->first, tw->mbuf.data.data(), tw->mbuf.data.size(), iskeyframe);
            tw->mbuf.data.clear();
        }
    }

    for (auto& kv : writers)
        delete kv.second;
    avformat_close_input(&ic);
    return err == AVERROR_EOF ? 0 : err;
}
