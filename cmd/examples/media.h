#pragma once

#include <deque>
#include <iostream>

namespace quicr {
    struct FullTrackName;
}


#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>


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
  bool is_keyframe = false; // Indicates if this fragment contains a keyframe
};

enum class TrackType
{
  VIDEO,
  AUDIO,
  SUBTITLE,
  ELSE,
};

class Track
{
  std::deque<MP4Chunk> chunks;
  std::mutex t_mtx;
  std::condition_variable cv;

public:
  int index;
  std::string name;
  TrackType type;
  std::string codec;

  Track(int idx, const std::string track_name)
      : index(idx), name(track_name) {}

  void PutChunk(const MP4Chunk chunk)
  {
    MP4Chunk chunk_copy = chunk;
    {
      std::lock_guard<std::mutex> lock(t_mtx);
      chunks.push_back(chunk_copy);
    }
    cv.notify_one();
  }

  MP4Chunk GetChunk(const bool& exit)
  {
    std::unique_lock<std::mutex> lock(t_mtx);
    if (!chunks.empty()) {
      MP4Chunk chunk = chunks.front();
      chunks.pop_front();
      return chunk;
    }
    cv.wait(lock, [this, exit]() { return !chunks.empty() || exit;});
    if (exit) {return MP4Chunk();} // Return empty chunk if exit is requested
    MP4Chunk chunk = chunks.front();
    chunks.pop_front();
    return chunk;
  }

  void NotifyAll()
  {
    cv.notify_all();
  }

  virtual ~Track() = default;
};

class VideoTrack : public Track
{
  public:
    int Width, Height;

    VideoTrack(int idx, const std::string track_name, int width, int height) : Track(idx,track_name), Width(width), Height(height) { type = TrackType::VIDEO; }
};

class SoundTrack : public Track
{
  public:
    SoundTrack(int idx, const std::string track_name) : Track(idx,track_name) { type = TrackType::AUDIO; }
};

struct SharedState {
  std::vector<std::shared_ptr<Track>> tracks;
  std::deque<MP4Chunk> chunks;
  std::vector<quicr::FullTrackName> track_names;
  bool catalog_ready = false;

  MP4Atom ftyp;
  MP4Atom moov;

  std::mutex s_mtx;
  std::condition_variable cv;

  void AddTrack(std::shared_ptr<Track> track)
  {
    std::lock_guard<std::mutex> lock(s_mtx);
        tracks.push_back(track);
        std::cout << "Track added to shared state, Tracks: " << tracks.size() << std::endl;
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
      catalog_ready = true;
    }
    cv.notify_all();
  }

  void PutChunk(const MP4Chunk& chunk)
  {
    {
      std::lock_guard<std::mutex> lock(s_mtx);
      chunks.push_back(chunk);
      std::cout << "Chunk added to shared state, Chunks: " << chunks.size() << std::endl;
    }
    cv.notify_one();
  }

  MP4Chunk GetChunk(const std::atomic<bool>& exit)
  {
    std::unique_lock<std::mutex> lock(s_mtx);
    if (!chunks.empty()) {
      MP4Chunk chunk = chunks.front();
      chunks.pop_front();
      std::cout << "Chunks in shared state: " << chunks.size() << std::endl;
      return chunk;
    }
    cv.wait(lock, [this, &exit]() { return !chunks.empty() || exit;});
    if (exit) {return MP4Chunk();} // Return empty chunk if exit is requested
    MP4Chunk chunk = chunks.front();
    chunks.pop_front();
    std::cout << "Chunk deleted from shared state, Chunks: " << chunks.size() << std::endl;
    return chunk;
  }

  void NotifyAll()
  {
    cv.notify_all();
  }
};

void DoParse(const std::shared_ptr<SharedState>& shared_state, const std::atomic<bool>& stop);

class CMafParser
{
  SharedState& state;
public:
  CMafParser(SharedState& shared_state) : state(shared_state) {}

  int ExtractTrackIdFromMoof(const MP4Atom& moof);
  static bool HasKeyframe(const MP4Atom& moof);
  void ParseBuffer(const uint8_t* buffer, size_t buffer_size);

private:
  void ProcessMoov(const MP4Atom& moov) const;
  void ProcessTrack(const uint8_t* trak_data, uint32_t trak_size) const;
  void ProcessFragments(const std::vector<MP4Atom>& atoms) const;
};
