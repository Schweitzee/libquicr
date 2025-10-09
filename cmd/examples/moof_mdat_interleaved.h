
#ifndef MOOF_MDAT_INTERLEAVED_H
#define MOOF_MDAT_INTERLEAVED_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>
#include <optional>
#include <functional>
#include <cstring>
#include <cstdlib>

class MoofMdatInterleaved {
public:
    explicit MoofMdatInterleaved(size_t max_unpaired_bytes = DefaultMaxUnpairedBytes())
        : max_unpaired_bytes_(max_unpaired_bytes) {}

    void set_publish(std::function<void(const uint8_t*, size_t, const uint8_t*, size_t)> cb) {
        publish_ = std::move(cb);
    }

    void push(const uint8_t* data, size_t len) { buf_.insert(buf_.end(), data, data + len); }

    void parse() {
        bool progressed = true;
        while (progressed) {
            progressed = false;
            if (head_ + 8 > buf_.size()) break;
            BoxHeader h;
            if (!peek_box(head_, h)) break;
            if (head_ + h.size > buf_.size()) break;

            if (is_type(h.type, "moof")) {
                auto expect = ExpectedMdatSizeFromMoof(&buf_[head_], (size_t)h.size);
                pending_moofs_.push_back({ head_, (size_t)h.size, expect });
                head_ += (size_t)h.size;
                progressed = true;
                progressed |= try_match_and_publish();
                enforce_unpaired_budget();
                compact_if_possible();
                continue;
            }
            if (is_type(h.type, "mdat")) {
                if (h.size == 0) break;
                pending_mdats_.push_back({ head_, (size_t)h.size });
                head_ += (size_t)h.size;
                progressed = true;
                progressed |= try_match_and_publish();
                enforce_unpaired_budget();
                compact_if_possible();
                continue;
            }
            head_ += (size_t)h.size;
            progressed = true;
            compact_if_possible();
        }
    }

    void flush() { compact_if_possible(true); }

private:
    struct BoxHeader {
        uint64_t size = 0;
        char     type[5] = {0,0,0,0,0};
        size_t   header_bytes = 0;
    };
    struct PendingMoof { size_t off; size_t size; std::optional<uint64_t> expected_mdat; };
    struct PendingMdat { size_t off; size_t size; };

    static bool is_type(const char* t, const char* fourcc) { return std::memcmp(t, fourcc, 4) == 0; }
    static uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]); }
    static uint64_t rd64(const uint8_t* p) { uint64_t hi=rd32(p), lo=rd32(p+4); return (hi<<32)|lo; }

    static size_t DefaultMaxUnpairedBytes() {
        const char* env = std::getenv("MOOF_MDAT_BUFFER_BYTES");
        if (!env || !*env) return 8 * 1024 * 1024;
        char* end = nullptr;
        unsigned long long v = std::strtoull(env, &end, 10);
        if (end == env) return 8 * 1024 * 1024;
        return (size_t)v;
    }

    bool peek_box(size_t off, BoxHeader& out) const {
        if (off + 8 > buf_.size()) return false;
        uint32_t size32 = rd32(&buf_[off]);
        out.size = size32;
        std::memcpy(out.type, &buf_[off+4], 4);
        out.type[4] = 0;
        out.header_bytes = 8;
        if (size32 == 1) {
            if (off + 16 > buf_.size()) return false;
            out.size = rd64(&buf_[off+8]);
            out.header_bytes = 16;
        } else if (size32 == 0) {
            return false;
        }
        if (out.size < out.header_bytes) return false;
        return true;
    }

    static std::optional<uint64_t> ExpectedMdatSizeFromMoof(const uint8_t* moof, size_t moof_size) {
        if (moof_size < 8) return std::nullopt;
        size_t off = 8;
        uint64_t total = 0;
        bool have_any = false;
        auto rd32l = rd32;
        while (off + 8 <= moof_size) {
            uint32_t sz = rd32l(moof + off);
            if (sz == 0 || off + sz > moof_size) break;
            const char* tp = (const char*)(moof + off + 4);
            if (std::memcmp(tp, "traf", 4) == 0) {
                uint32_t default_sample_size = 0;
                size_t sub = off + 8;
                while (sub + 8 <= off + sz) {
                    uint32_t ssz = rd32l(moof + sub);
                    if (ssz == 0 || sub + ssz > off + sz) break;
                    const char* st = (const char*)(moof + sub + 4);
                    if (std::memcmp(st, "tfhd", 4) == 0 && ssz >= 16) {
                        uint32_t flags = (uint32_t(moof[sub+9])<<16)|(uint32_t(moof[sub+10])<<8)|uint32_t(moof[sub+11]);
                        size_t pos = sub + 12;
                        pos += 4;
                        if (flags & 0x000001) pos += 8;
                        if (flags & 0x000002) pos += 4;
                        if (flags & 0x000008) { if (pos + 4 <= sub + ssz) default_sample_size = rd32l(moof + pos); pos += 4; }
                    }
                    if (std::memcmp(st, "trun", 4) == 0 && ssz >= 16) {
                        uint32_t flags = (uint32_t(moof[sub+9])<<16)|(uint32_t(moof[sub+10])<<8)|uint32_t(moof[sub+11]);
                        uint32_t sample_count = rd32l(moof + sub + 12);
                        size_t pos = sub + 16;
                        if (flags & 0x000001) pos += 4;
                        if (flags & 0x000004) pos += 4;
                        bool has_sample_duration = flags & 0x000100;
                        bool has_sample_size     = flags & 0x000200;
                        bool has_sample_flags    = flags & 0x000400;
                        bool has_sample_cto      = flags & 0x000800;
                        uint64_t run_bytes = 0;
                        if (has_sample_size) {
                            for (uint32_t i=0;i<sample_count;i++) {
                                if (pos + 4 > sub + ssz) { run_bytes = 0; break; }
                                run_bytes += rd32l(moof + pos); pos += 4;
                                if (has_sample_duration) pos += 4;
                                if (has_sample_flags)    pos += 4;
                                if (has_sample_cto)      pos += 4;
                            }
                            if (run_bytes == 0) return std::nullopt;
                            total += run_bytes; have_any = true;
                        } else if (default_sample_size != 0) {
                            total += uint64_t(default_sample_size) * sample_count; have_any = true;
                            for (uint32_t i=0;i<sample_count;i++) {
                                if (has_sample_duration) pos += 4;
                                if (has_sample_flags)    pos += 4;
                                if (has_sample_cto)      pos += 4;
                            }
                        } else {
                            return std::nullopt;
                        }
                    }
                    sub += ssz;
                }
            }
            off += sz;
        }
        if (!have_any) return std::nullopt;
        return total;
    }

    bool try_match_and_publish() {
        if (!publish_) return false;
        for (auto m_it = pending_moofs_.begin(); m_it != pending_moofs_.end(); ++m_it) {
            if (!m_it->expected_mdat.has_value()) continue;
            for (auto d_it = pending_mdats_.begin(); d_it != pending_mdats_.end(); ++d_it) {
                if (d_it->size == m_it->expected_mdat.value()) {
                    publish_(&buf_[m_it->off], m_it->size, &buf_[d_it->off], d_it->size);
                    pending_moofs_.erase(m_it);
                    pending_mdats_.erase(d_it);
                    return true;
                }
            }
        }
        if (!pending_moofs_.empty() && !pending_mdats_.empty()) {
            auto m = pending_moofs_.front(); pending_moofs_.pop_front();
            auto d = pending_mdats_.front(); pending_mdats_.pop_front();
            publish_(&buf_[m.off], m.size, &buf_[d.off], d.size);
            return true;
        }
        return false;
    }

    size_t unpaired_bytes() const {
        size_t b = 0;
        for (const auto& m : pending_moofs_) b += m.size;
        for (const auto& d : pending_mdats_) b += d.size;
        return b;
    }
    void enforce_unpaired_budget() {
        while (unpaired_bytes() > max_unpaired_bytes_) {
            if (!pending_mdats_.empty()) pending_mdats_.pop_front();
            else if (!pending_moofs_.empty()) pending_moofs_.pop_front();
            else break;
        }
    }

    void compact_if_possible(bool force=false) {
        size_t protect = head_;
        auto upd = [&](size_t off){ if (off < protect) protect = off; };
        for (const auto& m : pending_moofs_) upd(m.off);
        for (const auto& d : pending_mdats_) upd(d.off);
        const size_t THRESH = 2*1024*1024;
        if (force || protect > THRESH) {
            if (protect == 0) return;
            buf_.erase(buf_.begin(), buf_.begin()+protect);
            head_ -= protect;
            auto shift = [&](size_t& off){ off -= protect; };
            for (auto& m : pending_moofs_) shift(m.off);
            for (auto& d : pending_mdats_) shift(d.off);
        }
    }

private:
    std::vector<uint8_t> buf_;
    size_t head_ = 0;
    std::deque<PendingMoof> pending_moofs_;
    std::deque<PendingMdat> pending_mdats_;
    size_t max_unpaired_bytes_;
    std::function<void(const uint8_t*, size_t, const uint8_t*, size_t)> publish_;
};

#endif // MOOF_MDAT_INTERLEAVED_H
