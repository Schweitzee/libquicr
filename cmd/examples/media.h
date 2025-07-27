#pragma once
#include <deque>

namespace quicr {
    struct FullTrackName;
}


#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

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
};

class Track
{
public:
  int index;
  std::string name;
  TrackType type;
  std::string codec;
  std::deque<MP4Chunk> chunks;
  mutable std::mutex mtx;
  std::condition_variable cv;

  virtual ~Track() = default;
};

struct SharedState {
  std::vector<std::shared_ptr<Track>> tracks;
  std::vector<quicr::FullTrackName> track_names;
  bool catalog_ready = false;

  MP4Atom ftyp;
  MP4Atom moov;

  std::mutex mtx;
  std::condition_variable cv;
};

class VideoTrack : public Track
{
  public:
    int width, height;

    VideoTrack() { type = TrackType::VIDEO; }
};

class SoundTrack : public Track
{
  public:
    SoundTrack() { type = TrackType::AUDIO; }
};

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

void DoParse(std::shared_ptr<SharedState> shared_state, const bool& stop);
