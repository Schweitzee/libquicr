#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "concurrentqueue.h" // moodycamel
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>

#include "catalog.hpp"
#include "spdlog/fmt/bundled/chrono.h"
#include "spdlog/spdlog.h"

class VideoPublishTrackHandler;

struct MP4Atom
{
    std::string type;
    std::vector<uint8_t> data;
    uint64_t size = 0;
    int track_id = -1; // Will be set for moof atoms
};

// Represents a complete fragment (moof+mdat pair), either moof and mdat, or whole_chunk, but both may not be empty
struct MP4Chunk
{
    MP4Atom moof;        // may be nothing
    MP4Atom mdat;        // may be nothing
    MP4Atom whole_chunk; // may be nothing, contains both moof+mdat
    int track_id = -1;
    bool has_keyframe = false; // Indicates if this fragment contains a keyframe
};

class TrackPublishData
{
    std::mutex s_mtx;
    std::atomic<uint64_t> dropped{ 0 };

    moodycamel::ConcurrentQueue<MP4Chunk> chunk_q;
    moodycamel::ProducerToken prod_tok{ chunk_q };
    moodycamel::ConsumerToken cons_tok{ chunk_q };

  public:
    int track_id{ -1 };

    std::string track_name;

    TrackType track_type;

    std::condition_variable cv;

    uint64_t group_id{ 0 };
    uint64_t object_id{ 0 };
    uint64_t subgroup_id{ 0 };

    std::shared_ptr<VideoPublishTrackHandler> Trackhandler{ nullptr };

    int ChunkQueueSize() const { return chunk_q.size_approx(); }

    explicit TrackPublishData(size_t capacity_pow2 = 1024)
      : chunk_q(capacity_pow2)
      , // kezdeti kapacitás (később dinamikusan bővíthet)
      prod_tok(chunk_q)
      , cons_tok(chunk_q)
    {
    }

    void PutChunk(MP4Chunk chunk)
    {
        if (!chunk_q.try_enqueue(prod_tok, std::move(chunk))) {
            // DROP-ON-FULL: csak ritkítva logoljunk
            auto d = ++dropped;
            if ((d & ((1u << 10) - 1)) == 0) { // minden 1024. dropnál
                // SPDLOG_DEBUG/INFO-ra állíthatod igény szerint:
                SPDLOG_WARN("PublisherSharedState: queue full, dropped ~{} chunks total", d);
            }
        }
    }
    MP4Chunk GetChunk(const bool& /*exit*/)
    {
        MP4Chunk out;
        if (chunk_q.try_dequeue(cons_tok, out))
            return out;
        return {};
    }
    void WaitForChunk()
    {
        // return if cv has notify or stop is true
        std::unique_lock<std::mutex> lock(s_mtx);
        cv.wait(lock);
    }
};

class PublisherSharedState
{
    std::atomic<uint64_t> dropped{ 0 };

    std::condition_variable cv;

    moodycamel::ConcurrentQueue<MP4Chunk> chunk_q;
    moodycamel::ProducerToken prod_tok{ chunk_q };
    moodycamel::ConsumerToken cons_tok{ chunk_q };

  public:
    Catalog catalog;
    std::mutex s_mtx;

    std::map<int, std::shared_ptr<TrackPublishData>> tracks;

    bool catalog_ready = false;
    // moodycamel::ConcurrentQueue<std::shared_ptr<MP4Chunk>> chunk_q;
    // moodycamel::ProducerToken prod_tok{chunk_q};
    // moodycamel::ConsumerToken cons_tok{chunk_q};

    PublisherSharedState() {};

    void SetCatalogReady()
    {
        {
            // A lock csak a változó írásáig kell
            std::lock_guard<std::mutex> lock(s_mtx);
            catalog_ready = true;
        }
        // A lock feloldása után értesítjük a várakozó szálat/szálakat
        cv.notify_all();
    }

    void WaitForCatalogReady(const std::atomic<bool>& stop)
    {
        std::unique_lock<std::mutex> lock(s_mtx);
        cv.wait(lock, [&]() { return catalog_ready || stop.load(std::memory_order_relaxed); });
    }

    void PutChunk(MP4Chunk chunk)
    {
        if (!chunk_q.try_enqueue(prod_tok, std::move(chunk))) {
            // DROP-ON-FULL: csak ritkítva logoljunk
            auto d = ++dropped;
            if ((d & ((1u << 10) - 1)) == 0) { // minden 1024. dropnál
                // SPDLOG_DEBUG/INFO-ra állíthatod igény szerint:
                SPDLOG_WARN("PublisherSharedState: queue full, dropped ~{} chunks total", d);
            }
        }
    }

    MP4Chunk GetChunk(const bool& /*exit*/)
    {
        MP4Chunk out;
        if (chunk_q.try_dequeue(cons_tok, out))
            return out;
        return {};
    }

    void NotifyAll() { cv.notify_all(); }
};
