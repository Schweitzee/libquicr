// catalog.cpp - nlohmann/json kézi építéssel

#pragma once

#include "quicr/track_name.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

enum class TrackType
{
    VIDEO,
    AUDIO,
    SUBTITLE,
    ELSE,
  };

struct TrackEntry {
    std::string name;
    std::string type;// "video" | "audio" | "subtitle" | "else"
    int idx;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::string> language;
    std::optional<std::string> codec;

    static std::string lowercase(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return out;
    }
    void validate() const {
        if (name.empty())  throw std::invalid_argument("TrackEntry: name is empty");
        if (type.empty())  throw std::invalid_argument("TrackEntry: type is empty");
        const auto t = lowercase(type);
        if (t == "video") {
            if (!width || !height || *width <= 0 || *height <= 0)
                throw std::invalid_argument("Video track needs positive width/height");
        } else if (t == "audio") {
            if (!language || language->empty())
                throw std::invalid_argument("Audio track needs non-empty language");
        } else if (t == "subtitle" || t == "else") {
            // ok
        } else {
            throw std::invalid_argument("Unknown track type: " + type);
        }
    }
};

class Catalog {
public:

    std::string init_data_; // init data for all tracks (ftyp+moov)
    int binary_size; // size of the binary init data
    std::string namespace_;

private:
  uint16_t version_ = 1;
  std::vector<TrackEntry> tracks_;



public:
  void clear() { tracks_.clear(); }
  void set_version(uint16_t v) { version_ = v; }
  uint16_t version() const { return version_; }
  const std::vector<TrackEntry>& tracks() const { return tracks_; }

  void add_video(const std::string& name, const int index, int w, int h, const std::string& codec = {}) {
    TrackEntry e{ name, "video", index, w, h, std::nullopt, "-"};
    if (!codec.empty()) e.codec = codec;
    add_entry(std::move(e));
  }
  void add_audio(const std::string& name, const int index, const std::string& lang, const std::string& codec = {}) {
    TrackEntry e; e.name = name; e.idx = index; e.type = "audio"; e.language = lang;
    if (!codec.empty()) e.codec = codec;
    add_entry(std::move(e));
  }
  void add_subtitle(const std::string& name, const int index, const std::string& lang = {}) {
    TrackEntry e; e.name = name; e.idx = index; e.type = "subtitle";
    if (!lang.empty()) e.language = lang;
    add_entry(std::move(e));
  }
  void add_else(const std::string& name, const int index) {
    TrackEntry e; e.name = name; e.idx = index; e.type = "else";
    add_entry(std::move(e));
  }

  void validate() const {
    if (tracks_.empty()) throw std::invalid_argument("Catalog has no tracks");
    for (size_t i = 0; i < tracks_.size(); ++i) {
      tracks_[i].validate();
      for (size_t j = i + 1; j < tracks_.size(); ++j) {
        if (TrackEntry::lowercase(tracks_[i].type) == TrackEntry::lowercase(tracks_[j].type) &&
            tracks_[i].name == tracks_[j].name) {
          throw std::invalid_argument("Duplicate track: " + tracks_[i].name + " / " +
                                      TrackEntry::lowercase(tracks_[i].type));
        }
      }
    }
  }

  // ---- FIXED: kézzel épített JSON ----
  std::string to_json(bool pretty=false) const {
    nlohmann::json j;
    j["version"] = version_;
    j["init_data_b64"] = init_data_;
    j["len"] = binary_size;
    j["encoding"] = "base64";
    nlohmann::json arr = nlohmann::json::array();

    for (const auto& e : tracks_) {
      nlohmann::json jt;
      jt["name"] = e.name;
      jt["type"] = TrackEntry::lowercase(e.type);
      jt["index"] = e.idx;

      if (e.width)    jt["width"]    = *e.width;
      if (e.height)   jt["height"]   = *e.height;
      if (e.language) jt["language"] = *e.language;
      if (e.codec)    jt["codec"]    = *e.codec;
      arr.push_back(std::move(jt));
    }
    j["tracks"] = std::move(arr);
    return pretty ? j.dump(2) : j.dump();
  }

  // ---- FIXED: kézi beolvasás ----
  void from_json(const std::string& s) {
    auto j = nlohmann::json::parse(s);
    version_ = j.value("version", static_cast<uint16_t>(1));
    init_data_ = j.value("init_data_b64", init_data_);
    binary_size = j.value("len", 0);
    tracks_.clear();

    if (!j.contains("tracks") || !j["tracks"].is_array())
      throw std::invalid_argument("Catalog JSON missing 'tracks' array");

    for (const auto& jt : j["tracks"]) {
        TrackEntry e;
        e.name = jt.at("name").get<std::string>();
        e.type = jt.at("type").get<std::string>();
        e.idx  = jt.at("index").get<int>();

        if (jt.contains("width"))     e.width    = jt.at("width").get<int>();
        if (jt.contains("height"))    e.height   = jt.at("height").get<int>();
        if (jt.contains("language"))  e.language = jt.at("language").get<std::string>();
        if (jt.contains("codec"))     e.codec    = jt.at("codec").get<std::string>();

      // duplikáció-ellenőrzés (name+type)
      const auto tnorm = TrackEntry::lowercase(e.type);
      auto dup = std::find_if(tracks_.begin(), tracks_.end(), [&](const TrackEntry& x){
        return x.name == e.name && TrackEntry::lowercase(x.type) == tnorm;
      });
      if (dup != tracks_.end())
        throw std::invalid_argument("Duplicate in input JSON: " + e.name + " / " + tnorm);

      e.validate();
      tracks_.push_back(std::move(e));
    }
  }

private:
  void add_entry(TrackEntry&& e) {
    e.validate();
    const auto tnorm = TrackEntry::lowercase(e.type);
    auto dup = std::find_if(tracks_.begin(), tracks_.end(), [&](const TrackEntry& x){
      return x.name == e.name && TrackEntry::lowercase(x.type) == tnorm;
    });
    if (dup != tracks_.end())
      throw std::invalid_argument("Duplicate track: " + e.name + " / " + tnorm);
    tracks_.push_back(std::move(e));
  }
};


/*
 * Example usage:

Catalog cat;
cat.set_version(1);
cat.add_video("video_main", 1920, 1080, "h264");
cat.add_audio("audio_hu", "hu", "aac");
cat.validate();

std::string json = cat.to_json(true);
std::cout << json << "\n";

// Visszaolvasás:
Catalog cat2;
cat2.from_json(json);
cat2.validate();

 */