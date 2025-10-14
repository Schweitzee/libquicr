
#include "ffmpeg_moq_adapter.h"

#include <stdexcept>


constexpr std::string_view kValues = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";//=
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

// --- OnInit: elmentjük az init szegmenst a shared_state-be,
// és meghívjuk a PublishCatalog-ot ---

void FfmpegToMoQAdapter::OnInit(int stream_index, const uint8_t* init, size_t init_len, bool last_init) const
{
    if (!init || !init_len) return;

    stream_index = stream_index +1; //ffmpeg counts from 0, we shall count from 1 to make space for the catalog track

    //For the catalog
    CatalogTrackEntry track;
    ParseSingleTrackInit(init, init_len,track);
    track.name = track.type + std::to_string(stream_index);
    track.idx = stream_index;

    std::vector init_vec(init, init + init_len);
    std::string b64init = Encode(init_vec);
    track.init_binary_size = init_len;
    track.b64_init_data_ = b64init;

    shared_->catalog.addTrackEntry(std::move(track));

    // for the buffer
    auto tpd = std::make_shared<TrackPublishData>();
    tpd->track_name = track.name;
    tpd->track_id = stream_index;
    shared_->tracks[stream_index] = tpd;

    if (last_init) {
        fprintf(stderr, "[PUB-DEBUG] >>> Last init segment processed. Calling SetCatalogReady().\n");
        shared_->SetCatalogReady();
    }
    SPDLOG_TRACE("[PUB-DEBUG] >>> init segment processed. - {}\n", stream_index);
};

void FfmpegToMoQAdapter::OnFrag(int stream_index,
                                const uint8_t* moof, size_t moof_len,
                                bool keyframe) const
{
    stream_index = stream_index +1;
    MP4Chunk ch;
    ch.whole_chunk.data.assign(moof, moof + moof_len);
    ch.has_keyframe = keyframe;
    try {
        shared_->tracks.at(stream_index)->PutChunk(std::move(ch)); //TODO: exception safe-é tenni, mivel ekkor még lehet nem lesz kész a map
    } catch (...) {
        SPDLOG_TRACE("gatya");
    }
};

// Parse buffer and process atoms
void FfmpegToMoQAdapter::ParseSingleTrackInit(const uint8_t* buffer, size_t buffer_size, CatalogTrackEntry& catalog_data) {

    size_t offset = 0;
    std::shared_ptr<TrackPublishData> track = std::make_shared<TrackPublishData>();


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
        } else if (atom_type == "moov") {
            ProcessSingleTrackMoov(atom,catalog_data); // Extract track info from moov
        }

        offset += atom_size;
    }
};


// Extract track information from moov atom
void FfmpegToMoQAdapter::ProcessSingleTrackMoov(const MP4Atom& moov, CatalogTrackEntry& catalog_data)
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
            ProcessSingleTrack(moov.data.data() + offset, box_size, catalog_data);
        }

        if (box_size == 0) break;
        offset += box_size;
    }
};

void FfmpegToMoQAdapter::ProcessSingleTrack(const uint8_t* trak_data, uint32_t trak_size, CatalogTrackEntry& catalog_data)
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
             catalog_data.height = height;
             catalog_data.width = width;
             catalog_data.codec = codec;
             catalog_data.type = "video";
         } else if (handler_type == "soun") {
             catalog_data.type = "audio";
             catalog_data.language = "und";
             catalog_data.codec = codec;
         } else {
            catalog_data.type = "undefined";
         }
     }
};