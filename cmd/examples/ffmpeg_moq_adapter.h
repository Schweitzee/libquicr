#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
// ffmpeg_moq_adapter.h
#include "quicr/publish_track_handler.h"   // <<< EZ KELL
#include "media.h"


#include "media.h" // PublisherSharedState, MP4Chunk stb. innen jön
#include <quicr/publish_track_handler.h>

// Előre deklaráció: ne kelljen client2.cpp-t behúzni
struct TrackPublishData;

class FfmpegToMoQAdapter {
public:

    // Az adapter megkapja a shared_state-et (init szegmens tárolásához) és a két hívót.
    FfmpegToMoQAdapter(std::shared_ptr<PublisherSharedState> shared_state)
      : shared_(shared_state){}

    void OnInit(int stream_index,
                const uint8_t* init,
                size_t init_len,
                bool last_init) const;

    void OnFrag(int stream_index,
                const uint8_t* whole_chunk,
                size_t whole_chunk_len,
                bool keyframe) const;

private:
    std::shared_ptr<PublisherSharedState> shared_;


    static void ParseSingleTrackInit(const uint8_t* buffer, size_t buffer_size, CatalogTrackEntry& track);

    static void ProcessSingleTrackMoov(const MP4Atom& moov, CatalogTrackEntry& catalog_data);

    static void ProcessSingleTrack(const uint8_t* trak_data, uint32_t trak_size, CatalogTrackEntry& catalog_data);
};



