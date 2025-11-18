//
// Created by schweitzer on 2025. 11. 17..
//

#ifndef QUICR_TRANSCODE_REQUEST_H
#define QUICR_TRANSCODE_REQUEST_H

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <cstdint>
#include <nlohmann/json.hpp>

enum class TranscodePriority {
    Low,
    Normal,
    High
};

/// A támogatott operációk típusai.
/// (Ha bővítesz, bővül ez az enum és az OperationVariant.)
enum class OperationKind {
    VideoChangeResolution,
    VideoChangeFramerate,
    VideoChangeCodec,
    AudioGenerateSubtitles,
    AudioChangeCodec,
    SubtitleChangeLanguage,
    SubtitleConvertFormat
};

/// Forrás track leírása (JSON: "source")
struct TranscodeSource {
    // Ha empty, akkor a kód default namespace-t használ, pl. "<root>/data".
    std::optional<std::string> ns;   // "namespace" mező
    std::string track;               // "track" mező (kötelező)
};

/// Cél track leírása (JSON: "output"), opcionális.
/// Ha az egész `output` hiányzik, akkor ns + track_név generálódik a transzkóderben.
struct TranscodeOutput {
    std::optional<std::string> ns;              // "output.namespace"
    std::optional<std::string> track_name_hint; // "output.track_name_hint"
};

/// Opcionális általános követelmények (JSON: "constraints")
struct TranscodeConstraints {
    std::optional<std::uint32_t> max_latency_ms;
    std::optional<TranscodePriority> priority;
};

/// "video.change_resolution"
struct OpVideoChangeResolution {
    int width  = 0;  // pixel
    int height = 0;  // pixel
};

/// "video.change_framerate"
struct OpVideoChangeFramerate {
    double target_fps = 0.0; // pl. 30.0
};

/// "video.change_codec"
struct OpVideoChangeCodec {
    std::string codec;       // "h264", "av1", "vp9", ...
    int bitrate_kbps = 0;    // cél bitráta kbps-ben
};

/// "audio.generate_subtitles"
struct OpAudioGenerateSubtitles {
    std::string language;    // pl. "en", "hu"
};

/// "audio.change_codec"
struct OpAudioChangeCodec {
    std::string codec;       // pl. "aac", "opus"
    int bitrate_kbps = 0;    // cél bitráta kbps-ben
};

/// "subtitle.change_language"
struct OpSubtitleChangeLanguage {
    std::string source_language; // pl. "en"
    std::string target_language; // pl. "hu"
};

/// "subtitle.convert_format"
struct OpSubtitleConvertFormat {
    std::string target_format;   // pl. "webvtt", "srt"
};

/// Az összes támogatott operáció payload típusainak uniója.
using OperationVariant = std::variant<
    OpVideoChangeResolution,
    OpVideoChangeFramerate,
    OpVideoChangeCodec,
    OpAudioGenerateSubtitles,
    OpAudioChangeCodec,
    OpSubtitleChangeLanguage,
    OpSubtitleConvertFormat
>;

/// Egyetlen operation a JSON "operations" tömbjéből.
struct TranscodeOperation {
    OperationKind kind;
    OperationVariant data;

    // Kényelmi konstruktorok
    static TranscodeOperation make(OpVideoChangeResolution op) {
        return TranscodeOperation{ OperationKind::VideoChangeResolution, std::move(op) };
    }
    static TranscodeOperation make(OpVideoChangeFramerate op) {
        return TranscodeOperation{ OperationKind::VideoChangeFramerate, std::move(op) };
    }
    static TranscodeOperation make(OpVideoChangeCodec op) {
        return TranscodeOperation{ OperationKind::VideoChangeCodec, std::move(op) };
    }
    static TranscodeOperation make(OpAudioGenerateSubtitles op) {
        return TranscodeOperation{ OperationKind::AudioGenerateSubtitles, std::move(op) };
    }
    static TranscodeOperation make(OpAudioChangeCodec op) {
        return TranscodeOperation{ OperationKind::AudioChangeCodec, std::move(op) };
    }
    static TranscodeOperation make(OpSubtitleChangeLanguage op) {
        return TranscodeOperation{ OperationKind::SubtitleChangeLanguage, std::move(op) };
    }
    static TranscodeOperation make(OpSubtitleConvertFormat op) {
        return TranscodeOperation{ OperationKind::SubtitleConvertFormat, std::move(op) };
    }
};

/// A teljes transzkód kérés reprezentációja a C++ oldalon.
struct TranscodeRequest {
    std::string request_id;   // JSON: "request_id"
    std::string client_id;    // JSON: "client_id"

    TranscodeSource source;                       // JSON: "source"
    std::optional<TranscodeOutput> output;        // JSON: "output" (opcionális)
    std::vector<TranscodeOperation> operations;   // JSON: "operations"
    std::optional<TranscodeConstraints> constraints; // JSON: "constraints"
};

TranscodeOperation parse_single_operation(const json& opj)
{
    if (!opj.contains("type") || !opj.contains("params"))
        throw std::invalid_argument("Operation missing 'type' or 'params'");

    const std::string type = opj.at("type").get<std::string>();
    const json& params = opj.at("params");

    if (type == "video.change_resolution") {
        OpVideoChangeResolution op;
        if (!params.contains("width") || !params.contains("height"))
            throw std::invalid_argument("video.change_resolution requires width & height");
        op.width  = params.at("width").get<int>();
        op.height = params.at("height").get<int>();
        if (op.width <= 0 || op.height <= 0)
            throw std::invalid_argument("video.change_resolution width/height must be > 0");
        return TranscodeOperation::make(op);
    }

    if (type == "video.change_framerate") {
        OpVideoChangeFramerate op;
        if (!params.contains("target_fps"))
            throw std::invalid_argument("video.change_framerate requires target_fps");
        op.target_fps = params.at("target_fps").get<double>();
        if (op.target_fps <= 0.0)
            throw std::invalid_argument("video.change_framerate target_fps must be > 0");
        return TranscodeOperation::make(op);
    }

    if (type == "video.change_codec") {
        OpVideoChangeCodec op;
        if (!params.contains("codec") || !params.contains("bitrate_kbps"))
            throw std::invalid_argument("video.change_codec requires codec & bitrate_kbps");
        op.codec = params.at("codec").get<std::string>();
        op.bitrate_kbps = params.at("bitrate_kbps").get<int>();
        if (op.bitrate_kbps <= 0)
            throw std::invalid_argument("video.change_codec bitrate_kbps must be > 0");
        return TranscodeOperation::make(op);
    }

    if (type == "audio.generate_subtitles") {
        OpAudioGenerateSubtitles op;
        if (!params.contains("language"))
            throw std::invalid_argument("audio.generate_subtitles requires language");
        op.language = params.at("language").get<std::string>();
        if (op.language.empty())
            throw std::invalid_argument("audio.generate_subtitles language must be non-empty");
        return TranscodeOperation::make(op);
    }

    if (type == "audio.change_codec") {
        OpAudioChangeCodec op;
        if (!params.contains("codec") || !params.contains("bitrate_kbps"))
            throw std::invalid_argument("audio.change_codec requires codec & bitrate_kbps");
        op.codec = params.at("codec").get<std::string>();
        op.bitrate_kbps = params.at("bitrate_kbps").get<int>();
        if (op.bitrate_kbps <= 0)
            throw std::invalid_argument("audio.change_codec bitrate_kbps must be > 0");
        return TranscodeOperation::make(op);
    }

    if (type == "subtitle.change_language") {
        OpSubtitleChangeLanguage op;
        if (!params.contains("source_language") || !params.contains("target_language"))
            throw std::invalid_argument("subtitle.change_language requires source_language & target_language");
        op.source_language = params.at("source_language").get<std::string>();
        op.target_language = params.at("target_language").get<std::string>();
        if (op.source_language.empty() || op.target_language.empty())
            throw std::invalid_argument("subtitle.change_language languages must be non-empty");
        return TranscodeOperation::make(op);
    }

    if (type == "subtitle.convert_format") {
        OpSubtitleConvertFormat op;
        if (!params.contains("target_format"))
            throw std::invalid_argument("subtitle.convert_format requires target_format");
        op.target_format = params.at("target_format").get<std::string>();
        if (op.target_format.empty())
            throw std::invalid_argument("subtitle.convert_format target_format must be non-empty");
        return TranscodeOperation::make(op);
    }

    throw std::invalid_argument("Unknown operation type: " + type);
}

enum class InferredMediaType {
    Video,
    Audio,
    Subtitle
};

inline InferredMediaType infer_media_type(OperationKind kind)
{
    switch (kind) {
        case OperationKind::VideoChangeResolution:
        case OperationKind::VideoChangeFramerate:
        case OperationKind::VideoChangeCodec:
            return InferredMediaType::Video;

        case OperationKind::AudioGenerateSubtitles:
        case OperationKind::AudioChangeCodec:
            return InferredMediaType::Audio;

        case OperationKind::SubtitleChangeLanguage:
        case OperationKind::SubtitleConvertFormat:
            return InferredMediaType::Subtitle;
    }
    // Itt elvileg sosem járunk, de a fordító miatt:
    throw std::logic_error("infer_media_type: unknown OperationKind");
}

/// Validáció: egy request-ben csak egy fajta forrásra vonatkozó operation lehet.
/// Tehát az összes OperationKind ugyanarra az InferredMediaType-ra kell essen.
inline void validate_single_media_type(const TranscodeRequest& req)
{
    if (req.operations.empty()) {
        throw std::invalid_argument("TranscodeRequest must contain at least one operation");
    }

    std::optional<InferredMediaType> mt;

    for (const auto& op : req.operations) {
        InferredMediaType cur = infer_media_type(op.kind);
        if (!mt.has_value()) {
            mt = cur;
        } else if (cur != mt.value()) {
            throw std::invalid_argument(
                "TranscodeRequest contains operations for multiple media types; "
                "only a single source media type is allowed per request"
            );
        }
    }
}



TranscodeRequest parse_transcode_request(const json& j)
{
    TranscodeRequest req;

    // --- request_id, client_id ---
    if (!j.contains("request_id") || !j.contains("client_id"))
        throw std::invalid_argument("TranscodeRequest missing request_id or client_id");

    req.request_id = j.at("request_id").get<std::string>();
    req.client_id  = j.at("client_id").get<std::string>();

    if (req.request_id.empty())
        throw std::invalid_argument("TranscodeRequest.request_id must be non-empty");
    if (req.client_id.empty())
        throw std::invalid_argument("TranscodeRequest.client_id must be non-empty");

    // --- source ---
    if (!j.contains("source"))
        throw std::invalid_argument("TranscodeRequest missing 'source'");

    const nlohmann::json& jsource = j.at("source");
    TranscodeSource src;

    if (!jsource.contains("track"))
        throw std::invalid_argument("TranscodeRequest.source missing 'track'");

    src.track = jsource.at("track").get<std::string>();
    if (src.track.empty())
        throw std::invalid_argument("TranscodeRequest.source.track must be non-empty");

    if (jsource.contains("namespace")) {
        std::string ns = jsource.at("namespace").get<std::string>();
        if (!ns.empty()) {
            src.ns = ns;
        }
    }
    req.source = std::move(src);

    // --- output (opcionális) ---
    if (j.contains("output")) {
        const nlohmann::json& jout = j.at("output");
        TranscodeOutput out;

        if (jout.contains("namespace")) {
            std::string ns = jout.at("namespace").get<std::string>();
            if (!ns.empty())
                out.ns = ns;
        }
        if (jout.contains("track_name_hint")) {
            std::string hint = jout.at("track_name_hint").get<std::string>();
            if (!hint.empty())
                out.track_name_hint = hint;
        }

        // akkor tesszük optional-ba, ha bármelyik mező ténylegesen meg van adva
        if (out.ns.has_value() || out.track_name_hint.has_value()) {
            req.output = std::move(out);
        }
    }

    // --- operations ---
    if (!j.contains("operations") || !j.at("operations").is_array())
        throw std::invalid_argument("TranscodeRequest missing 'operations' array");

    const nlohmann::json& jops = j.at("operations");
    if (jops.empty()) {
        throw std::invalid_argument("TranscodeRequest.operations must not be empty");
    }

    req.operations.clear();
    req.operations.reserve(jops.size());

    for (const auto& opj : jops) {
        req.operations.push_back(parse_single_operation(opj));
    }

    // --- constraints (opcionális) ---
    if (j.contains("constraints")) {
        const nlohmann::json& jc = j.at("constraints");
        TranscodeConstraints c;

        if (jc.contains("max_latency_ms")) {
            auto v = jc.at("max_latency_ms").get<std::uint32_t>();
            if (v == 0) {
                throw std::invalid_argument("constraints.max_latency_ms must be > 0");
            }
            c.max_latency_ms = v;
        }

        if (jc.contains("priority")) {
            std::string p = jc.at("priority").get<std::string>();
            c.priority = parse_priority(p);
        }

        // akkor állítsuk be az optional-t, ha legalább egy mező van
        if (c.max_latency_ms.has_value() || c.priority.has_value()) {
            req.constraints = std::move(c);
        }
    }

    // --- VALIDÁCIÓ: csak egy media type ---
    validate_single_media_type(req);

    return req;
}


/// Ha stringből jön:
inline TranscodeRequest parse_transcode_request(const std::string& json_str) {
    return parse_transcode_request(nlohmann::json::parse(json_str));
}


#endif // QUICR_TRANSCODE_REQUEST_H
