#include "media.h"
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
        if (atom_size == 0 || offset + atom_size > buffer_size) break;

        // Process atom based on type
        MP4Atom atom;
        atom.type = atom_type;
        atom.size = atom_size;
        atom.data.assign(buffer + offset, buffer + offset + atom_size);

        if (atom_type == "ftyp") {
            state.setFtyp(atom);
        } else if (atom_type == "moov") {
            ProcessMoov(atom); // Extract track info from moov
            state.setMoov(atom);
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

    DataDump("moov" , moov.data.data(), moov.data.size());

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
        std::shared_ptr<Track> track;
        if (handler_type == "vide") {
            auto video_track = std::make_shared<VideoTrack>(track_id, "Video Track", width, height);
            video_track->codec = codec;
            track = video_track;
            std::cout << "Video track created with width: " << width << ", height: " << height << std::endl;
        } else if (handler_type == "soun") {
            auto audio_track = std::make_shared<SoundTrack>(track_id, "Audio Track");
            audio_track->codec = codec;
            track = audio_track;
                std::cout << "Audio track created" << std::endl;
        } else {
            // Generic track for other types
            track = std::make_shared<Track>(track_id, "? Track");
        }

        state.AddTrack(track);
    }
}

// Process fragments and associate them with tracks
void CMafParser::ProcessFragments(const std::vector<MP4Atom>& atoms) const
{
    if (atoms.empty()) return;

    MP4Atom last_moof;

    for (size_t i = 0; i < atoms.size(); i++) {
        if (atoms[i].type == "moof") {
            last_moof = atoms[i];
        } else if (atoms[i].type == "mdat" && last_moof.track_id != -1) {
            int track_id = last_moof.track_id;
            MP4Chunk chunk;
            chunk.moof = last_moof;
            chunk.mdat = atoms[i];
            chunk.track_id = track_id;
            chunk.is_keyframe = HasKeyframe(last_moof);

            std::cout << "Fragment for track ID: " << track_id
                << ", size: " << chunk.mdat.data.size()
                << ", keyframe: " << chunk.is_keyframe << std::endl;
            state.PutChunk(chunk);
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

void DoParse(std::shared_ptr<SharedState> shared_state, const bool& stop) {
    CMafParser parser(*shared_state);

    std::deque<uint8_t> buffer;
    const size_t read_chunk = 4096;
    std::vector<uint8_t> temp(read_chunk);

    while (!stop) {
        ssize_t bytes_read = read(STDIN_FILENO, temp.data(), read_chunk);
        if (bytes_read <= 0) {
            std::cout << "mutex lock <=0 ENDING___" << std::endl;
            shared_state->cv.notify_all();
            break;
        }
        buffer.insert(buffer.end(), temp.begin(), temp.begin() + bytes_read);

        // Process as many complete atoms as possible
        while (buffer.size() >= 8) {
            // Parse atom header
            uint32_t atom_size = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
            std::string atom_type(reinterpret_cast<const char*>(&buffer[4]), 4);

            if (atom_size == 1) {
                // Extended size not supported in this example
                std::cerr << "Extended atom size not supported" << std::endl;
                break;
            }
            if (atom_size == 0 || atom_size > buffer.size()) {
                // Not enough data for a full atom yet
                std::cerr << "Incomplete atom or invalid size: " << atom_size << std::endl;
                break;
            }

            // Special handling for moof+mdat
            if (atom_type == "moof") {
                // Check if next atom is mdat
                if (buffer.size() < atom_size + 8) break; // Not enough for moof + next header
                uint32_t next_atom_size = (buffer[atom_size] << 24) | (buffer[atom_size+1] << 16) |
                                          (buffer[atom_size+2] << 8) | buffer[atom_size+3];
                std::string next_atom_type(reinterpret_cast<const char*>(&buffer[atom_size+4]), 4);
                if (next_atom_type != "mdat" || buffer.size() < atom_size + next_atom_size) break;

                // Pass moof+mdat together
                std::vector<uint8_t> atoms(buffer.begin(), buffer.begin() + atom_size + next_atom_size);
                parser.ParseBuffer(atoms.data(), atoms.size());
                buffer.erase(buffer.begin(), buffer.begin() + atom_size + next_atom_size);
            } else {
                // Pass single atom
                std::vector<uint8_t> atom(buffer.begin(), buffer.begin() + atom_size);
                parser.ParseBuffer(atom.data(), atom.size());
                buffer.erase(buffer.begin(), buffer.begin() + atom_size);
            }
        }
    }
    std::cout << "Parsing stopped." << std::endl;
    shared_state->cv.notify_all();
}

