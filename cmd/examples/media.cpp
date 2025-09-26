#include "media.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <deque>
#include <iomanip>
#include <iostream>

void DataDump(std::string type, const uint8_t* data, size_t size) {
    std::cout << "Dumping " << type << " (" << size << " bytes):" << std::endl;
    const int CHARS_PER_ROW = 64;
    for (size_t i = 0; i < size; ++i) {
        char c = data[i];
        std::cout << (c >= 32 && c <= 126 ? c : '.');
        if ((i + 1) % CHARS_PER_ROW == 0) std::cout << std::endl;
    }
    if (size % CHARS_PER_ROW != 0) std::cout << std::endl;
}

static inline uint32_t ReadU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static inline uint64_t ReadU64BE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

struct BoxHeader {
    uint64_t size = 0;       // teljes box méret headert is beleértve (0: „a fájl végéig”)
    char     type[5] = {0};  // 0-terminated
    size_t   header_len = 0; // 8 vagy 16
};

// Megpróbálja "belest" a headerbe anélkül, hogy túlindexelne.
// Csak akkor ad true-t, ha a TELJES header (8/16B) megvan és a méret valid.
static inline bool PeekBoxHeader(const std::vector<uint8_t>& b, size_t off, BoxHeader& out) {
    if (b.size() < off + 8) return false; // nincs meg a minimális header
    const uint8_t* p = b.data() + off;
    uint32_t sz32 = ReadU32BE(p);
    std::memcpy(out.type, p + 4, 4);
    out.type[4] = 0;
    out.header_len = 8;
    out.size = sz32;

    if (sz32 == 1) {
        // large size
        if (b.size() < off + 16) return false;
        out.size = ReadU64BE(p + 8);
        out.header_len = 16;
    }
    // minimális érvényességi check
    if (out.size != 0 && out.size < out.header_len) {
        SPDLOG_WARN("Invalid box size for type {}: size={} < header_len={}", out.type, out.size, out.header_len);
        return false;
    }
    return true;
}

static inline bool BoxFullyAvailable(const std::vector<uint8_t>& b, size_t off, const BoxHeader& h) {
    if (h.size == 0) return false; // stream végéig tartó mdat — pipe mellett tipikusan nem praktikus
    return (b.size() >= off + h.size);
}

// opcionális: engedélyezett „közbeékelődő” boxok moof és mdat között
static inline bool IsSkippableBetweenMoofAndMdat(const char* t) {
    // gyakori „ártalmatlan” boxok: padding, időbélyeg, index
    return std::string(t) == "free"
        || std::string(t) == "skip"
        || std::string(t) == "prft"
        || std::string(t) == "sidx"
        || std::string(t) == "mfra"
        || std::string(t) == "uuid"; // UUID box-ok is előfordulhatnak
}

// Extract track ID from a moof atom
int CMafParser::ExtractTrackIdFromMoof(const MP4Atom& moof)
{
    if (moof.type != "moof" || moof.data.size() < 32) {
        return -1;
    }

    // Scan through moof box to find the traf box
    size_t offset = 8; // Skip moof header
    while (offset + 8 <= moof.data.size()) {
        uint32_t box_size = (moof.data[offset] << 24) | (moof.data[offset+1] << 16) |
                           (moof.data[offset+2] << 8) | moof.data[offset+3];
        std::string box_type(reinterpret_cast<const char*>(&moof.data[offset+4]), 4);

        if (box_type == "traf") {
            // Found traf, now look for tfhd inside it
            size_t traf_offset = offset + 8; // Skip traf header
            while (traf_offset + 8 <= moof.data.size()) {
                uint32_t tfhd_size = (moof.data[traf_offset] << 24) | (moof.data[traf_offset+1] << 16) |
                                     (moof.data[traf_offset+2] << 8) | moof.data[traf_offset+3];
                std::string tfhd_type(reinterpret_cast<const char*>(&moof.data[traf_offset+4]), 4);

                if (tfhd_type == "tfhd" && traf_offset + 16 <= moof.data.size()) {
                    // Track ID is at offset 12 in tfhd box
                    return (moof.data[traf_offset + 12] << 24) |
                           (moof.data[traf_offset + 13] << 16) |
                           (moof.data[traf_offset + 14] << 8)  |
                           (moof.data[traf_offset + 15]);
                }

                if (tfhd_size == 0) break;
                traf_offset += tfhd_size;
            }
            break;
        }

        if (box_size == 0) break;
        offset += box_size;
    }
    return -1;
}

// Parse buffer and process atoms
void CMafParser::ParseBuffer(const uint8_t* buffer, size_t buffer_size) {

    //DataDump("buffer", buffer, buffer_size);

    std::vector<MP4Atom> atoms;
    size_t offset = 0;

    while (offset + 8 <= buffer_size) {
        uint32_t atom_size = (buffer[offset] << 24) | (buffer[offset+1] << 16) |
                            (buffer[offset+2] << 8) | buffer[offset+3];
        std::string atom_type(reinterpret_cast<const char*>(&buffer[offset+4]), 4);



        // Check if we have the complete atom
        if (atom_size == 0 || offset + atom_size > buffer_size) {
            std::cout << "Incomplete atom or invalid size: " << atom_size << std::endl;
            break;
        }
        // Process atom based on type
        MP4Atom atom;
        atom.type = atom_type;
        atom.size = atom_size;
        atom.data.assign(buffer + offset, buffer + offset + atom_size);

        if (atom_type == "ftyp") {
            state.setFtyp(atom);
        } else if (atom_type == "moov") {
            state.catalog.clear();
            ProcessMoov(atom); // Extract track info from moov
            state.catalog.validate();
            state.setMoov(atom);
            state.catalog_ready = true;
        } else if (atom_type == "moof") {
            atom.track_id = ExtractTrackIdFromMoof(atom);
            atoms.push_back(atom);
        } else if (atom_type == "mdat") {
            atoms.push_back(atom);
        }

        offset += atom_size;
    }

    // Process any moof/mdat pairs
    ProcessFragments(atoms);
}

// Extract track information from moov atom
void CMafParser::ProcessMoov(const MP4Atom& moov) const
{
    if (moov.type != "moov" || moov.data.size() < 8) {
        return;
    }

    //DataDump("moov" , moov.data.data(), moov.data.size());

    // Scan through moov box to find the trak boxes
    size_t offset = 8; // Skip moov header
    while (offset + 8 <= moov.data.size()) {
        uint32_t box_size = (moov.data[offset] << 24) | (moov.data[offset+1] << 16) |
                           (moov.data[offset+2] << 8) | moov.data[offset+3];
        std::string box_type(reinterpret_cast<const char*>(&moov.data[offset+4]), 4);

        if (box_type == "trak" && box_size > 8) {
            ProcessTrack(moov.data.data() + offset, box_size);
        }

        if (box_size == 0) break;
        offset += box_size;
    }

}

void CMafParser::ProcessTrack(const uint8_t* trak_data, uint32_t trak_size) const
{
    int track_id = -1;
    std::string handler_type;
    int width = 0, height = 0;
    std::string codec;

    //DataDump("trak", trak_data, trak_size);

    // Parse track header (tkhd) for track_id, width, height
    size_t offset = 8; // Skip trak header
    while (offset + 8 <= trak_size) {
        uint32_t box_size = (trak_data[offset] << 24) | (trak_data[offset+1] << 16) |
                           (trak_data[offset+2] << 8) | trak_data[offset+3];
        std::string box_type(reinterpret_cast<const char*>(&trak_data[offset+4]), 4);

        if (box_type == "tkhd" && offset + 84 < trak_size) {
            // Extract track ID (at offset 20 in version 0, at offset 24 in version 1)
            uint8_t version = trak_data[offset+8];
            size_t id_offset = offset + (version == 0 ? 20 : 24);
            if (id_offset + 4 <= trak_size) {
                track_id = (trak_data[id_offset] << 24) | (trak_data[id_offset+1] << 16) |
                          (trak_data[id_offset+2] << 8) | trak_data[id_offset+3];
            }

            // Extract width and height (last 8 bytes of tkhd)
            size_t dim_offset = offset + box_size - 8;
            if (dim_offset + 8 <= trak_size) {
                // Width and height are 16.16 fixed point values
                width = ((trak_data[dim_offset] << 8) | trak_data[dim_offset+1]) +
                       ((trak_data[dim_offset+2] >> 7) ? 1 : 0);
                height = ((trak_data[dim_offset+4] << 8) | trak_data[dim_offset+5]) +
                        ((trak_data[dim_offset+6] >> 7) ? 1 : 0);
            }
        } else if (box_type == "mdia") {
            // Search for hdlr inside mdia
            size_t mdia_offset = offset + 8;
            while (mdia_offset + 8 <= offset + box_size) {
                uint32_t mdia_box_size = (trak_data[mdia_offset] << 24) | (trak_data[mdia_offset+1] << 16) |
                                        (trak_data[mdia_offset+2] << 8) | trak_data[mdia_offset+3];
                std::string mdia_box_type(reinterpret_cast<const char*>(&trak_data[mdia_offset+4]), 4);

                if (mdia_box_type == "hdlr" && mdia_offset + 16 <= offset + box_size) {
                    // Handler type is at offset 16 (4 bytes)
                    handler_type = std::string(reinterpret_cast<const char*>(&trak_data[mdia_offset+16]), 4);
                } else if (mdia_box_type == "minf") {
                    // Search for stbl and then stsd for codec info
                    size_t minf_offset = mdia_offset + 8;
                    while (minf_offset + 8 <= mdia_offset + mdia_box_size) {
                        uint32_t minf_box_size = (trak_data[minf_offset] << 24) | (trak_data[minf_offset+1] << 16) |
                                               (trak_data[minf_offset+2] << 8) | trak_data[minf_offset+3];
                        std::string minf_box_type(reinterpret_cast<const char*>(&trak_data[minf_offset+4]), 4);

                        if (minf_box_type == "stbl") {
                            size_t stbl_offset = minf_offset + 8;
                            while (stbl_offset + 8 <= minf_offset + minf_box_size) {
                                uint32_t stbl_box_size = (trak_data[stbl_offset] << 24) | (trak_data[stbl_offset+1] << 16) |
                                                       (trak_data[stbl_offset+2] << 8) | trak_data[stbl_offset+3];
                                std::string stbl_box_type(reinterpret_cast<const char*>(&trak_data[stbl_offset+4]), 4);

                                if (stbl_box_type == "stsd" && stbl_offset + 24 <= minf_offset + minf_box_size) {
                                    // First entry in stsd contains codec info at offset 16
                                    codec = std::string(reinterpret_cast<const char*>(&trak_data[stbl_offset+20]), 4);
                                }

                                if (stbl_box_size == 0) break;
                                stbl_offset += stbl_box_size;
                            }
                        }

                        if (minf_box_size == 0) break;
                        minf_offset += minf_box_size;
                    }
                }

                if (mdia_box_size == 0) break;
                mdia_offset += mdia_box_size;
            }
        }

        if (box_size == 0) break;
        offset += box_size;
    }

    // Create appropriate track based on handler_type
    if (track_id != -1) {
        std::cout << "track_id: " << track_id << "type: " << handler_type << std::endl;
        if (handler_type == "vide") {
            state.catalog.add_video("video"+std::to_string(track_id), track_id, width, height, codec);
        } else if (handler_type == "soun") {
            state.catalog.add_audio("sound" + std::to_string(track_id), track_id, "und", codec);
        } else {
            state.catalog.add_else("Undefined_track_"  + std::to_string(track_id), track_id);
        }
    }
}

// Process fragments and associate them with tracks
void CMafParser::ProcessFragments(const std::vector<MP4Atom>& atoms) const
{
    if (atoms.empty()) {
        std::cout << "No fragments to process." << std::endl;
        return;
    }

    MP4Atom last_moof;

    for (size_t i = 0; i < atoms.size(); i++) {
        if (atoms[i].type == "moof") {
            last_moof = atoms[i];
        } else if (atoms[i].type == "mdat" && last_moof.track_id != -1) {
            int track_id = last_moof.track_id;

            std::shared_ptr<MP4Chunk> chunk_ptr = std::make_shared<MP4Chunk>();
            chunk_ptr->moof = last_moof;
            chunk_ptr->mdat = atoms[i];
            chunk_ptr->track_id = track_id;
            chunk_ptr->is_keyframe = HasKeyframe(last_moof);

            //std::cout << "Fragment for track ID: " << track_id
            //    << ", size: " << chunk_ptr->mdat.data.size()
            //    << ", keyframe: " << chunk_ptr->is_keyframe << std::endl;
            state.PutChunk(chunk_ptr);
            last_moof = MP4Atom(); // Reset last_moof after processing
        }
    }
}

// Check if a fragment contains a keyframe
bool CMafParser::HasKeyframe(const MP4Atom& moof) {
    if (moof.type != "moof" || moof.data.size() < 32) {
        return false;
    }

    constexpr uint32_t IS_NON_SYNC_SAMPLE = 0x10000;
    constexpr uint32_t DEPENDS_YES = 0x20000;

    size_t offset = 8; // Skip moof header
    while (offset + 8 <= moof.data.size()) {
        uint32_t box_size = (moof.data[offset] << 24) | (moof.data[offset + 1] << 16) |
                            (moof.data[offset + 2] << 8) | moof.data[offset + 3];
        std::string box_type(reinterpret_cast<const char*>(&moof.data[offset + 4]), 4);

        if (box_type == "traf") {
            size_t traf_offset = offset + 8;
            while (traf_offset + 8 <= moof.data.size()) {
                uint32_t inner_size = (moof.data[traf_offset] << 24) | (moof.data[traf_offset + 1] << 16) |
                                      (moof.data[traf_offset + 2] << 8) | moof.data[traf_offset + 3];
                std::string inner_type(reinterpret_cast<const char*>(&moof.data[traf_offset + 4]), 4);

                if (inner_type == "trun" && traf_offset + 16 <= moof.data.size()) {
                    size_t pos = traf_offset + 8;
                    uint8_t version = moof.data[pos];
                    uint32_t trun_flags = (moof.data[pos] << 24) |
                                          (moof.data[pos + 1] << 16) |
                                          (moof.data[pos + 2] << 8) |
                                          moof.data[pos + 3];
                    uint32_t sample_count = (moof.data[pos + 4] << 24) |
                                            (moof.data[pos + 5] << 16) |
                                            (moof.data[pos + 6] << 8) |
                                            moof.data[pos + 7];
                    pos += 8;

                    uint32_t first_sample_flags = 0;
                    if (trun_flags & 0x000001) { // data-offset-present
                        pos += 4;
                    }
                    if (trun_flags & 0x000004) { // first-sample-flags-present
                        first_sample_flags = (moof.data[pos] << 24) |
                                             (moof.data[pos + 1] << 16) |
                                             (moof.data[pos + 2] << 8) |
                                             moof.data[pos + 3];
                        pos += 4;
                    }

                    bool has_sample_duration = trun_flags & 0x000100;
                    bool has_sample_size     = trun_flags & 0x000200;
                    bool has_sample_flags    = trun_flags & 0x000400;
                    bool has_sample_cto      = trun_flags & 0x000800;

                    for (uint32_t i = 0; i < sample_count; ++i) {
                        uint32_t sample_flags = (i == 0 && (trun_flags & 0x000004)) ? first_sample_flags : 0;
                        size_t sample_pos = pos;
                        if (has_sample_duration) sample_pos += 4;
                        if (has_sample_size)     sample_pos += 4;
                        if (has_sample_flags) {
                            if (sample_pos + 4 > moof.data.size()) break;
                            sample_flags = (moof.data[sample_pos] << 24) |
                                           (moof.data[sample_pos + 1] << 16) |
                                           (moof.data[sample_pos + 2] << 8) |
                                           moof.data[sample_pos + 3];
                            sample_pos += 4;
                        }
                        if (has_sample_cto) sample_pos += 4;

                        // For video: keyframe if neither IS_NON_SYNC_SAMPLE nor DEPENDS_YES is set
                        if ((sample_flags & (IS_NON_SYNC_SAMPLE | DEPENDS_YES)) == 0) {
                            return true;
                        }

                        // Advance to next sample
                        pos += (has_sample_duration ? 4 : 0) +
                               (has_sample_size ? 4 : 0) +
                               (has_sample_flags ? 4 : 0) +
                               (has_sample_cto ? 4 : 0);
                    }
                }

                if (inner_size == 0) break;
                traf_offset += inner_size;
            }
        }

        if (box_size == 0) break;
        offset += box_size;
    }
    return false;
}

void DoParse(const std::shared_ptr<SharedState>& shared_state,const bool& stop)
{
    CMafParser parser(*shared_state);

    // -------- helper-ek (header olvasás, méretkezelés) --------
    auto read_u32be = [](const uint8_t* p) -> uint32_t {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    auto read_u64be = [](const uint8_t* p) -> uint64_t {
        uint64_t v = 0; for (int i=0;i<8;i++) v = (v<<8) | p[i]; return v;
    };

    struct BoxHeader { uint64_t size{0}; char type[5]{0,0,0,0,0}; size_t hlen{0}; };

    auto peek_header = [&](const std::vector<uint8_t>& b, size_t off, BoxHeader& out) -> bool {
        if (b.size() < off + 8) return false;
        const uint8_t* p = b.data() + off;
        uint32_t sz32 = read_u32be(p);
        std::memcpy(out.type, p+4, 4); out.type[4]=0;
        out.hlen = 8; out.size = sz32;
        if (sz32 == 1) { // large size
            if (b.size() < off + 16) return false;
            out.size = read_u64be(p+8);
            out.hlen = 16;
        }
        if (out.size != 0 && out.size < out.hlen) return false; // hibás méret
        return true;
    };
    auto full_avail = [&](const std::vector<uint8_t>& b, size_t off, const BoxHeader& h) -> bool {
        if (h.size == 0) return false; // stream végéig tartó: pipe-nál ne vállaljuk be automatikusan
        return b.size() >= off + h.size;
    };
    auto skippable_between_moof_mdat = [&](const char* t)->bool{
        return std::string(t)=="free" || std::string(t)=="skip" ||
               std::string(t)=="prft" || std::string(t)=="sidx" ||
               std::string(t)=="mfra" || std::string(t)=="uuid";
    };

    // -------- bemeneti puffer + állapotgép --------
    std::vector<uint8_t> buf; buf.reserve(1<<20);
    size_t head = 0;
    enum class Phase { INIT, STREAM };
    Phase phase = Phase::INIT;

    size_t moof_off = 0, moof_size = 0;
    uint64_t chunk_count = 0;

    auto compact = [&](){
        if (head > (1<<20)) { buf.erase(buf.begin(), buf.begin()+head); head = 0; }
    };

    // ha a meglévő kódod nem stdin-ről olvas, ezt a blokkot nyugodtan hagyd meg,
    // csak az append útját igazítsd a te input forrásodhoz:
    constexpr size_t READ_CHUNK = 64*1024;
    std::vector<char> in(READ_CHUNK);

    auto publish_chunk = [&](const uint8_t* moof_ptr, size_t moof_len,
                             const uint8_t* mdat_ptr, size_t mdat_len){
        // Itt NE változtass a hívott függvényeken — ha van saját queue push-od, azt hívd.
        // Az alábbi „out” csak példa; cseréld a te meglévő kódodra:
        std::vector<uint8_t> out;
        out.reserve(moof_len + mdat_len);
        out.insert(out.end(), moof_ptr, moof_ptr + moof_len);
        out.insert(out.end(), mdat_ptr, mdat_ptr + mdat_len);

        // >>> KEEP YOUR EXISTING "publish to shared_state" CALL HERE <<<
        // pl.: shared_state->PushChunk(std::move(out));
        parser.ParseBuffer(out.data(), out.size());

        (void)out; // ha a fenti sort beteszed, ez törölhető
        ++chunk_count;
        //SPDLOG_INFO("Chunk ready #{} (moof {} bytes + mdat {} bytes)", chunk_count, moof_len, mdat_len);
    };

    while (!stop) {
        bool progressed = false;

        // --- próbáljunk mindent feldolgozni, ami a bufferben már bent van ---
        while (true) {
            BoxHeader h;
            if (!peek_header(buf, head, h)) break; // kell még adat a headerhez

            if (phase == Phase::INIT) {
                // 1) ftyp/moov teljes és megvan? hagyd meg a saját hívásaidat változtatás nélkül.
                if (std::string(h.type) == "ftyp") {
                    if (!full_avail(buf, head, h)) break;
                    // >>> KEEP YOUR EXISTING FTYP HANDLER HERE <<<
                    // például: ParseFtyp(&buf[head], h.size);
                    parser.ParseBuffer(&buf[head], h.size);

                    head += static_cast<size_t>(h.size);
                    progressed = true;
                    continue;
                }
                if (std::string(h.type) == "moov") {
                    if (!full_avail(buf, head, h)) break;
                    // >>> KEEP YOUR EXISTING MOOV HANDLER HERE <<<
                    // például: ParseMoov(&buf[head], h.size);
                    parser.ParseBuffer(&buf[head], h.size);

                    head += static_cast<size_t>(h.size);
                    progressed = true;
                    continue;
                }

                // 2) amikor elértünk az első moof-hoz, áttérünk STREAM módba
                if (std::string(h.type) == "moof") {
                    if (!full_avail(buf, head, h)) break;
                    moof_off = head;
                    moof_size = static_cast<size_t>(h.size);
                    head += moof_size;
                    phase = Phase::STREAM;
                    progressed = true;
                    SPDLOG_INFO("Switching to STREAM phase (first moof seen, size={})", moof_size);
                    continue;
                }

                // 3) bármi más: ha teljes, ugorjuk (free/sidx/uuid stb.), ha nem teljes: várunk
                if (!full_avail(buf, head, h)) break;
                SPDLOG_INFO("INIT: skipping box {} (size={})", h.type, h.size);
                head += static_cast<size_t>(h.size);
                progressed = true;
                continue;
            }
            else { // STREAM phase: moof + mdat párok
                if (moof_size == 0) {
                    // itt „moof” kell jöjjön
                    if (std::string(h.type) != "moof") {
                        if (!full_avail(buf, head, h)) break;
                        SPDLOG_WARN("STREAM: expected moof, got {} — skipping (size={})", h.type, h.size);
                        head += static_cast<size_t>(h.size);
                        progressed = true;
                        continue;
                    }
                    if (!full_avail(buf, head, h)) break;
                    moof_off = head;
                    moof_size = static_cast<size_t>(h.size);
                    head += moof_size;
                    progressed = true;
                    continue;
                } else {
                    // moof már megvan – keressük a hozzátartozó mdat-ot
                    if (std::string(h.type) == "mdat") {
                        if (h.size == 0) { SPDLOG_WARN("mdat size==0 in stream; waiting for end"); break; }
                        if (!full_avail(buf, head, h)) break;
                        size_t mdat_off = head;
                        size_t mdat_size = static_cast<size_t>(h.size);

                        // >>> KEEP YOUR EXISTING "MOOF+MDAT CHUNK READY" HANDLING IF YOU HAVE ONE <<<
                        publish_chunk(buf.data()+moof_off, moof_size,
                                      buf.data()+mdat_off, mdat_size);

                        head += mdat_size;
                        moof_off = 0; moof_size = 0; // új pár jöhet
                        progressed = true;
                        continue;
                    }
                    // Közbeékelődő, ártalmatlan boxok: ugorjuk
                    if (skippable_between_moof_mdat(h.type)) {
                        if (!full_avail(buf, head, h)) break;
                        SPDLOG_INFO("STREAM: skipping {} between moof and mdat (size={})", h.type, h.size);
                        head += static_cast<size_t>(h.size);
                        progressed = true;
                        continue;
                    }
                    // Váratlan box moof után, de mdat előtt: ugorjuk, hogy ne akadjon meg a parser
                    if (!full_avail(buf, head, h)) break;
                    SPDLOG_WARN("STREAM: unexpected {} between moof and mdat — skipping (size={})", h.type, h.size);
                    head += static_cast<size_t>(h.size);
                    progressed = true;
                    continue;
                }
            }
        }

        if (progressed) { compact(); continue; }

        // --- ha nem tudtunk tovább lépni, olvassunk új adatot ---
        if (stop) break;

        std::cin.read(in.data(), static_cast<std::streamsize>(in.size()));
        std::streamsize got = std::cin.gcount();
        if (got > 0) {
            size_t old = buf.size();
            buf.resize(old + static_cast<size_t>(got));
            std::memcpy(buf.data()+old, in.data(), static_cast<size_t>(got));
            continue;
        }
        if (std::cin.eof()) {
            SPDLOG_INFO("Input EOF");
            break;
        }
        // opcionális: kis pihenő, ha busy-loopolna
        // std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    SPDLOG_INFO("DoParse finished");
}

