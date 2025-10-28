
#ifndef QUICR_EXAMPLES_BASE64_TOOL_H_
#define QUICR_EXAMPLES_BASE64_TOOL_H_

#pragma once


#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
//
// Created by schweitzer on 2025. 10. 18..
//
namespace base64 {
    // From https://gist.github.com/williamdes/308b95ac9ef1ee89ae0143529c361d37;

    inline constexpr std::string_view kValues = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";//=
    static std::string Encode(const std::string& in)
    {
        std::string out;

        int val = 0;
        int valb = -6;

        for (std::uint8_t c : in) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out += kValues[(val >> valb) & 0x3F];
                valb -= 6;
            }
        }

        if (valb > -6) {
            out += kValues[((val << 8) >> (valb + 8)) & 0x3F];
        }

        while (out.size() % 4) {
            out += '=';
        }

        return out;
    }

    [[maybe_unused]] static std::string Decode(const std::string& in)
    {
        std::string out;

        std::vector<int> values(256, -1);
        for (int i = 0; i < 64; i++) {
            values[kValues[i]] = i;
        }

        int val = 0;
        int valb = -8;

        for (std::uint8_t c : in) {
            if (values[c] == -1) {
                break;
            }

            val = (val << 6) + values[c];
            valb += 6;

            if (valb >= 0) {
                out += char((val >> valb) & 0xFF);
                valb -= 8;
            }
        }

        return out;
    }

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

    // Dekód táblázat (standard, + és / jelekhez)
    inline const std::array<int8_t, 256>& decode_table() {
        static std::array<int8_t, 256> D{};
        static bool init = false;
        if (!init) {
            D.fill(-1);
            for (int i = 0; i < 64; ++i) {
                D[static_cast<unsigned char>(kValues[i])] = static_cast<int8_t>(i);
            }
            init = true;
        }
        return D;
    }

    // Robusztus dekóder: kezel URL-safe-et, whitespace-et, data URI prefixet, paddinget
    inline std::vector<uint8_t> decode_to_uint8_vec(std::string_view in) {
        // 1) Ha data: URI, vágjuk le az előtagot a vesszőig
        if (in.rfind("data:", 0) == 0) {
            const size_t comma = in.find(',');
            if (comma == std::string_view::npos) {
                throw std::invalid_argument("base64: invalid data URI (missing comma)");
            }
            in = in.substr(comma + 1);
        }

        // 2) Normalizálás: minden whitespace eldobása; URL-safe (-/_) -> +/
        std::string norm;
        norm.reserve(in.size());
        bool saw_urlsafe = false;

        for (size_t i = 0; i < in.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(in[i]);

            if (std::isspace(c)) continue;           // minden ASCII whitespace törlése

            if (c == '-') { norm.push_back('+'); saw_urlsafe = true; continue; }
            if (c == '_') { norm.push_back('/'); saw_urlsafe = true; continue; }

            // Megengedett karakterek: A-Z a-z 0-9 + / =
            if ((c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '+' || c == '/' || c == '=') {
                norm.push_back(static_cast<char>(c));
            } else {
                // Adjunk értelmes hibát pozícióval és kóddal
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "base64: invalid character at pos %zu: 0x%02X ('%c')",
                              i, c, (c >= 32 && c < 127 ? c : '.'));
                throw std::invalid_argument(msg);
            }
        }

        // 3) Pótoljuk a paddinget 4-es többszörösre
        if (norm.size() % 4 != 0) {
            size_t pad = (4 - (norm.size() % 4)) % 4;
            norm.append(pad, '=');
        }

        // 4) Tényleges dekódolás
        const auto& D = decode_table();
        std::vector<uint8_t> out;
        out.reserve((norm.size() / 4) * 3);

        int val = 0;
        int valb = -8;

        for (size_t pos = 0; pos < norm.size(); ++pos) {
            unsigned char c = static_cast<unsigned char>(norm[pos]);
            if (c == '=') {
                // padding -> a hátralevő részt ignoráljuk (csak whitespace/’=’ lehet)
                break;
            }
            int8_t d = D[c];
            if (d < 0) {
                // Ez elvileg nem fordulhat elő a fenti szűrés után
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "base64: unexpected character after normalization at pos %zu: 0x%02X",
                              pos, c);
                throw std::invalid_argument(msg);
            }
            val = (val << 6) | d;
            valb += 6;
            if (valb >= 0) {
                out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }

        return out;
    }
}

#endif // QUICR_EXAMPLES_BASE64_TOOL_H_