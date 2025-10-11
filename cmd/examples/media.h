#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <deque>

#include <memory>
#include <atomic>
#include <cstddef>
#include <thread>
#include "concurrentqueue.h" // moodycamel

#include "catalog.hpp"
#include "spdlog/spdlog.h"

struct MP4Atom {
  std::string type;
  std::vector<uint8_t> data;
  uint64_t size = 0;
  int track_id = -1;  // Will be set for moof atoms
};

// Represents a complete fragment (moof+mdat pair)
struct MP4Chunk {
  MP4Atom moof;
  MP4Atom mdat;
  int track_id = -1;
  bool has_keyframe = false; // Indicates if this fragment contains a keyframe
};

class PublisherSharedState {
  std::deque<std::shared_ptr<MP4Chunk>> chunks;

  std::atomic<uint64_t> dropped{0};



  MP4Atom ftyp;
  MP4Atom moov;

  std::mutex s_mtx;
  std::condition_variable cv;

public:
    Catalog catalog;
    bool catalog_ready = false;
    moodycamel::ConcurrentQueue<std::shared_ptr<MP4Chunk>> chunk_q;
    moodycamel::ProducerToken prod_tok{chunk_q};
    moodycamel::ConsumerToken cons_tok{chunk_q};


    explicit PublisherSharedState(size_t capacity_pow2 = 1024)
    : chunk_q(capacity_pow2) // kezdeti kapacitás (később dinamikusan bővíthet)
    , prod_tok(chunk_q)
    , cons_tok(chunk_q){}

  void WaitForCatalogReady(const bool& stop_threads)
  {
    std::unique_lock<std::mutex> lock(s_mtx);
      cv.wait(lock, [&]() {
          return catalog_ready || stop_threads;
      });
  }

  MP4Atom getFtyp() {
    std::lock_guard<std::mutex> lock(s_mtx);
    return ftyp;
  }

  MP4Atom getMoov() {
    std::lock_guard<std::mutex> lock(s_mtx);
    return moov;
  }

  void setFtyp(const MP4Atom& atom) {
    std::lock_guard<std::mutex> lock(s_mtx);
    ftyp = atom;
  }

  void setMoov(const MP4Atom& atom) {
    {
      std::lock_guard<std::mutex> lock(s_mtx);
      moov = atom;
    }
    cv.notify_all();
  }

  // void PutChunk(std::shared_ptr<MP4Chunk> chunk)
  // {
  //   {
  //     std::lock_guard<std::mutex> lock(s_mtx);
  //     chunks.push_back(chunk);
  //     //std::cout << "Chunk added to shared state, Chunks: " << chunks.size() << std::endl;
  //   }
  //   cv.notify_one();
  // }
      // Non-blocking push: true = sikerült, false = tele (vagy mem nyomás)
    void PutChunk(std::shared_ptr<MP4Chunk> chunk) {
            if (!chunk_q.try_enqueue(prod_tok, std::move(chunk))) {
                  // DROP-ON-FULL: csak ritkítva logoljunk
                  auto d = ++dropped;
                  if ((d & ((1u<<10)-1)) == 0) { // minden 1024. dropnál
                        // SPDLOG_DEBUG/INFO-ra állíthatod igény szerint:
                        SPDLOG_WARN("PublisherSharedState: queue full, dropped ~{} chunks total", d);
                      }
                }
          }

  // std::shared_ptr<MP4Chunk> GetChunk(const bool& exit)
  // {
  //     std::unique_lock<std::mutex> lock(s_mtx);
  //     while (chunks.empty() && !exit) {
  //         //std::cout << "-----Waiting for chunks in shared state..." << std::endl;
  //         cv.wait_for(lock, std::chrono::milliseconds(1000));
  //         //std::cout  << "wait returned: " << chunks.size() << std::endl;
  //     }
  //     if (exit || chunks.empty()) {
  //         return std::make_shared<MP4Chunk>(); // Return empty chunk if exit or still no chunk
  //     }
  //     std::shared_ptr<MP4Chunk> chunk = chunks.front();
  //     //std::cout << "shared pointer body count before pop: " << chunk.use_count() << std::endl;
  //     chunks.pop_front();
  //     //std::cout << "Chunk deleted from shared state, Chunks: " << chunks.size() << std::endl;
  //     return chunk;
  // }

    // Nem blokkoló: ha nincs adat, nullptr-t ad vissza (exit itt nem befolyásol)
    std::shared_ptr<MP4Chunk> GetChunk(const bool& /*exit*/) {
            std::shared_ptr<MP4Chunk> out;
            if (chunk_q.try_dequeue(cons_tok, out)) return out;
            return {};
    }

  void NotifyAll()
  {
    cv.notify_all();
  }
};

void DoParse(const std::shared_ptr<PublisherSharedState>& shared_state, const bool& stop);

class CMafParser
{
  PublisherSharedState& state;
public:
  CMafParser(PublisherSharedState& shared_state) : state(shared_state) {
  }

  static int ExtractTrackIdFromMoof(const MP4Atom& moof);
  static bool HasKeyframe(const MP4Atom& moof);
  void ParseBuffer(const uint8_t* buffer, size_t buffer_size);

private:
  void ProcessMoov(const MP4Atom& moov) const;
  void ProcessTrack(const uint8_t* trak_data, uint32_t trak_size) const;
  void ProcessFragments(const std::vector<MP4Atom>& atoms) const;
};
