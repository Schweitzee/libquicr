// catalog.cpp - nlohmann/json kézi építéssel

#pragma once

#include "quicr/track_name.h"
#include "spdlog/spdlog.h"

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

struct CatalogTrackEntry {

    std::string name;
    std::string track_namespace_;

    std::string type;
    int idx;

    std::string b64_init_data;
    size_t init_binary_size;
    std::string init_encoding = "base64";


    std::optional<int> alt_group;
    std::optional<std::string> codec;
    std::optional<std::string> mime_type;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<double> framerate;
    std::optional<int> bitrate;
    std::optional<int> sample_rate;
    std::optional<int> channels;
    std::optional<std::string> language;
    std::optional<std::string> label;

    static std::string lowercase(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return out;
    }

    void validate() const {
        if (name.empty())
            throw std::invalid_argument("TrackEntry: name is empty");
        if (type.empty())
            throw std::invalid_argument("TrackEntry: type is empty");
        std::string t = lowercase(type);
        if (t == "video") {
            if (!width || !height || *width <= 0 || *height <= 0)
                throw std::invalid_argument("Video track needs positive width/height");
        } else if (t == "audio") {
            if (!language || language->empty())
                throw std::invalid_argument("Audio track needs non-empty language");
        }
    }

    std::string effective_src_namespace(const std::string& catalog_ns) const {
        return track_namespace_.empty() ? catalog_ns : track_namespace_;
    }
};

class Catalog {
public:
    std::string namespace_; //The whole media's namespace
private:
    std::string catalog_version = "1";
    std::string streaming_format = "WARP";
    std::string streaming_format_version = "1";
    bool supports_delta_updates = true;

    uint16_t update_sequence = 1;
    std::vector<CatalogTrackEntry> tracks_;

public:

    void clear() { tracks_.clear(); }
    void setUpdateSequence(uint16_t v) { update_sequence = v; }
    uint16_t updateSequence() const { return update_sequence; }
    std::vector<CatalogTrackEntry>& tracks() { return tracks_; }

    // Új track hozzáadása különböző típusokhoz
    void addTrack(const CatalogTrackEntry& entry) {
        CatalogTrackEntry e = entry;
        e.validate();
        // Duplikált track név ellenőrzése (név + típuskategória egyedisége)
        for (const auto& x : tracks_) {
            if (CatalogTrackEntry::lowercase(x.type) == CatalogTrackEntry::lowercase(e.type) &&
                x.name == e.name) {
                throw std::invalid_argument("Duplicate track: " + e.name + " (" + e.type + ")");
            }
        }
        tracks_.push_back(std::move(e));
    }

    void add_video(const std::string& name, int index, const std::string& init_b64, int init_len,
                   int w, int h, const std::string& codec = "", double fps = 0, int bitrate = 0, int alt_group = 0) {
        CatalogTrackEntry e;
        e.name = name;
        e.type = "video";
        e.idx = index;
        e.b64_init_data = init_b64;
        e.init_binary_size = init_len;
        e.width = w;
        e.height = h;
        if (alt_group > 0) e.alt_group = alt_group;
        if (!codec.empty()) e.codec = codec;
        if (fps > 0) e.framerate = fps;
        if (bitrate > 0) e.bitrate = bitrate;
        e.label = "video_" + std::to_string(h);
        addTrack(e);
    }
    void add_audio(const std::string& name, int index, const std::string& init_b64, int init_len,
                   const std::string& lang, const std::string& codec = "", int sr = 0, int ch = 0, int alt_group = 0) {
        CatalogTrackEntry e;
        e.name = name;
        e.type = "audio";
        e.idx = index;
        e.b64_init_data = init_b64;
        e.init_binary_size = init_len;
        e.language = lang;
        if (!codec.empty()) e.codec = codec;
        if (sr > 0) e.sample_rate = sr;
        if (ch > 0) e.channels = ch;
        e.label = "audio_" + lang;
        addTrack(e);
    }

    std::string to_json(bool pretty = false) const {
        nlohmann::json j;
        j["version"] = catalog_version;
        j["streamingFormat"] = streaming_format;
        j["streamingFormatVersion"] = streaming_format_version;
        if (supports_delta_updates) {
            j["supportsDeltaUpdates"] = true;
        }
        j["namespace"] = namespace_;
        // Track-ek tömbje
        nlohmann::json tracks_array = nlohmann::json::array();
        for (const auto& e : tracks_) {
            nlohmann::json jt;
            jt["name"]     = e.name;
            if (!e.track_namespace_.empty())
                jt["namespace"] = e.track_namespace_;
            jt["type"]     = CatalogTrackEntry::lowercase(e.type);
            jt["index"]    = e.idx;
            jt["init_data"] = e.b64_init_data;
            jt["init_len"]  = e.init_binary_size;
            jt["encoding"]  = e.init_encoding;

            if (e.alt_group)   jt["altGroup"]  = *e.alt_group;
            if (e.codec)       jt["codec"]    = *e.codec;
            if (e.mime_type)   jt["mimeType"] = *e.mime_type;
            if (e.width)       jt["width"]    = *e.width;
            if (e.height)      jt["height"]   = *e.height;
            if (e.framerate)   jt["framerate"] = *e.framerate;
            if (e.bitrate)     jt["bitrate"]   = *e.bitrate;
            if (e.sample_rate) jt["sampleRate"] = *e.sample_rate;
            if (e.channels)    jt["channels"]  = *e.channels;
            if (e.language)    jt["lang"]      = *e.language;
            if (e.label)       jt["label"]     = *e.label;
            tracks_array.push_back(jt);
        }
        j["tracks"] = tracks_array;
        return pretty ? j.dump(2) : j.dump();
    }

    // JSON deszerializálás a katalógus frissítésére (teljes csere)
    void from_json(const std::string& json_str) {
        nlohmann::json j = nlohmann::json::parse(json_str);
        // Kötelező mezők ellenőrzése
        catalog_version = j.value("version", std::string("1"));
        streaming_format = j.value("streamingFormat", std::string("WARP"));
        streaming_format_version = j.value("streamingFormatVersion", std::string("1"));
        supports_delta_updates = j.value("supportsDeltaUpdates", false);
        namespace_ = j.at("namespace").get<std::string>();
        if (!j.contains("tracks") || !j["tracks"].is_array()) {
            throw std::invalid_argument("Catalog JSON missing 'tracks' array");
        }
        tracks_.clear();
        for (const auto& jt : j["tracks"]) {
            CatalogTrackEntry e;
            e.name = jt.at("name").get<std::string>();
            if (jt.contains("namespace"))
                e.track_namespace_ = jt.at("namespace").get<std::string>();
            e.type = jt.at("type").get<std::string>();
            e.idx  = jt.at("index").get<int>();
            e.b64_init_data = jt.at("init_data").get<std::string>();
            e.init_binary_size = jt.at("init_len").get<int>();
            e.init_encoding = jt.value("encoding", std::string("base64"));

            if (jt.contains("altGroup"))   e.alt_group = jt.at("altGroup").get<int>();
            if (jt.contains("codec"))      e.codec = jt.at("codec").get<std::string>();
            if (jt.contains("mimeType"))   e.mime_type = jt.at("mimeType").get<std::string>();
            if (jt.contains("width"))      e.width = jt.at("width").get<int>();
            if (jt.contains("height"))     e.height = jt.at("height").get<int>();
            if (jt.contains("framerate"))  e.framerate = jt.at("framerate").get<double>();
            if (jt.contains("bitrate"))    e.bitrate = jt.at("bitrate").get<int>();
            if (jt.contains("sampleRate")) e.sample_rate = jt.at("sampleRate").get<int>();
            if (jt.contains("channels"))   e.channels = jt.at("channels").get<int>();
            if (jt.contains("lang"))       e.language = jt.at("lang").get<std::string>();
            if (jt.contains("label"))      e.label = jt.at("label").get<std::string>();
            e.validate();
            // Duplikáció ellenőrzése (név + típus)
            for (const auto& x : tracks_) {
                if (CatalogTrackEntry::lowercase(x.type) == CatalogTrackEntry::lowercase(e.type) &&
                    x.name == e.name) {
                    throw std::invalid_argument("Duplicate in input JSON: " + e.name + " (" + e.type + ")");
                }
            }
            tracks_.push_back(std::move(e));
        }
    }

    // **Új funkció**: JSON delta patch alkalmazása a katalógusra
    // CSAK track hozzáadást vagy eltávolítást engedélyez
    void applyDeltaUpdate(const std::string& patch_json) {
        nlohmann::json patch = nlohmann::json::parse(patch_json);

        if (!patch.is_array()) {
            throw std::invalid_argument("Patch must be a JSON array");
        }

        for (const auto& operation : patch) {
            if (!operation.contains("op") || !operation.contains("path")) {
                throw std::invalid_argument("Invalid patch operation: missing 'op' or 'path'");
            }

            std::string op = operation.at("op").get<std::string>();
            std::string path = operation.at("path").get<std::string>();

            // Csak /tracks/-hez való hozzáadást vagy /tracks/N eltávolítást engedélyezünk
            if (op == "add" && path == "/tracks/-") {
                // Track hozzáadása a végére
                if (!operation.contains("value")) {
                    throw std::invalid_argument("Add operation requires 'value'");
                }

                const nlohmann::json& jt = operation.at("value");
                CatalogTrackEntry e;
                e.name = jt.at("name").get<std::string>();
                if (jt.contains("namespace"))
                    e.track_namespace_ = jt.at("namespace").get<std::string>();
                e.type = jt.at("type").get<std::string>();
                e.idx  = jt.at("index").get<int>();
                e.b64_init_data = jt.at("init_data").get<std::string>();
                e.init_binary_size = jt.at("init_len").get<int>();
                e.init_encoding = jt.value("encoding", std::string("base64"));

                if (jt.contains("altGroup"))   e.alt_group = jt.at("altGroup").get<int>();
                if (jt.contains("codec"))      e.codec = jt.at("codec").get<std::string>();
                if (jt.contains("mimeType"))   e.mime_type = jt.at("mimeType").get<std::string>();
                if (jt.contains("width"))      e.width = jt.at("width").get<int>();
                if (jt.contains("height"))     e.height = jt.at("height").get<int>();
                if (jt.contains("framerate"))  e.framerate = jt.at("framerate").get<double>();
                if (jt.contains("bitrate"))    e.bitrate = jt.at("bitrate").get<int>();
                if (jt.contains("sampleRate")) e.sample_rate = jt.at("sampleRate").get<int>();
                if (jt.contains("channels"))   e.channels = jt.at("channels").get<int>();
                if (jt.contains("lang"))       e.language = jt.at("lang").get<std::string>();
                if (jt.contains("label"))      e.label = jt.at("label").get<std::string>();

                e.validate();
                // Ellenőrizzük a duplikációt
                for (const auto& x : tracks_) {
                    if (CatalogTrackEntry::lowercase(x.type) == CatalogTrackEntry::lowercase(e.type) &&
                        x.name == e.name) {
                        throw std::invalid_argument("Duplicate track in delta: " + e.name + " (" + e.type + ")");
                    }
                }
                tracks_.push_back(std::move(e));

            } else if (op == "remove" && path.rfind("/tracks/", 0) == 0) {
                // Track eltávolítása index alapján
                std::string index_str = path.substr(8); // "/tracks/" után
                try {
                    size_t index = std::stoul(index_str);
                    if (index < tracks_.size()) {
                        tracks_.erase(tracks_.begin() + index);
                    } else {
                        throw std::invalid_argument("Invalid track index in remove operation: " + index_str);
                    }
                } catch (const std::exception&) {
                    throw std::invalid_argument("Invalid track index in remove operation: " + index_str);
                }

            } else {
                throw std::invalid_argument("Delta update only supports adding tracks (op=add, path=/tracks/-) or removing tracks (op=remove, path=/tracks/N). Got: op=" + op + ", path=" + path);
            }
        }
    }

    // **Új funkció**: Katalógus patch készítése egy track entry-ből (track hozzáadásához)
    static std::string makeCatalogPatch(const CatalogTrackEntry& entry, bool remove = false, int remove_index = -1) {
        nlohmann::json patch = nlohmann::json::array();

        if (remove) {
            // Track eltávolítása
            if (remove_index < 0) {
                throw std::invalid_argument("makeCatalogPatch: remove_index must be >= 0 when remove=true");
            }
            nlohmann::json op;
            op["op"] = "remove";
            op["path"] = "/tracks/" + std::to_string(remove_index);
            patch.push_back(op);

        } else {
            // Track hozzáadása
            nlohmann::json op;
            op["op"] = "add";
            op["path"] = "/tracks/-";

            nlohmann::json value;
            value["name"] = entry.name;
            if (!entry.track_namespace_.empty())
                value["namespace"] = entry.track_namespace_;
            value["type"] = CatalogTrackEntry::lowercase(entry.type);
            value["index"] = entry.idx;
            value["init_data"] = entry.b64_init_data;
            value["init_len"] = entry.init_binary_size;
            value["encoding"] = entry.init_encoding;

            if (entry.alt_group)   value["altGroup"]  = *entry.alt_group;
            if (entry.codec)       value["codec"]     = *entry.codec;
            if (entry.mime_type)   value["mimeType"]  = *entry.mime_type;
            if (entry.width)       value["width"]     = *entry.width;
            if (entry.height)      value["height"]    = *entry.height;
            if (entry.framerate)   value["framerate"] = *entry.framerate;
            if (entry.bitrate)     value["bitrate"]   = *entry.bitrate;
            if (entry.sample_rate) value["sampleRate"] = *entry.sample_rate;
            if (entry.channels)    value["channels"]  = *entry.channels;
            if (entry.language)    value["lang"]      = *entry.language;
            if (entry.label)       value["label"]     = *entry.label;

            op["value"] = value;
            patch.push_back(op);
        }

        return patch.dump();
    }

    // **Új funkció**: Track keresése név és típus alapján, visszaadja az indexet
    std::optional<size_t> findTrackIndex(const std::string& name, const std::string& type) const {
        std::string type_lower = CatalogTrackEntry::lowercase(type);
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (CatalogTrackEntry::lowercase(tracks_[i].type) == type_lower &&
                tracks_[i].name == name) {
                return i;
            }
        }
        return std::nullopt;
    }
};
