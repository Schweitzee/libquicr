
// async_stdin_reader.cpp  (LOSSLESS VERSION)
#include "async_stdin_reader.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#  error "This async stdin reader is implemented for POSIX only."
#endif

#include <unistd.h>   // read, STDIN_FILENO

AsyncStdinReader::AsyncStdinReader(const Config& cfg) : cfg_(cfg) {}

AsyncStdinReader::~AsyncStdinReader() {
    stop();
}

void AsyncStdinReader::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return; // already running
    }
    eof_.store(false, std::memory_order_release);
    th_ = std::thread(&AsyncStdinReader::reader_loop_, this);
}

void AsyncStdinReader::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    // Wake the producer/consumer and join.
    cv_has_data_.notify_all();
    cv_has_space_.notify_all();
    if (th_.joinable()) th_.join();
}

size_t AsyncStdinReader::queued_bytes() const {
    std::scoped_lock lk(m_);
    return queued_bytes_;
}

bool AsyncStdinReader::pop(std::vector<uint8_t>& out, bool block, std::chrono::milliseconds timeout) {
    std::unique_lock lk(m_);
    auto have_data = [&]{ return !q_.empty() || eof_.load(std::memory_order_acquire) || !running_.load(); };

    if (block) {
        if (!have_data()) {
            if (timeout.count() > 0) {
                cv_has_data_.wait_for(lk, timeout, have_data);
            } else {
                cv_has_data_.wait(lk, have_data);
            }
        }
    }

    if (q_.empty()) {
        return false; // nothing now (maybe EOF and empty)
    }

    out = std::move(q_.front());
    queued_bytes_ -= out.size();
    q_.pop_front();
    lk.unlock();
    cv_has_space_.notify_all(); // free space for producer if bounded
    return true;
}

// WARNING: Not lossless! This explicitly discards backlog to return the newest.
bool AsyncStdinReader::pop_latest(std::vector<uint8_t>& out) {
    std::unique_lock lk(m_);
    if (q_.empty()) return false;
    // Keep only newest chunk
    size_t removed = 0;
    for (size_t i = 0; i + 1 < q_.size(); ++i) removed += q_[i].size();
    out = std::move(q_.back());
    q_.clear();
    queued_bytes_ = 0;
    lk.unlock();
    cv_has_space_.notify_all();
    return true;
}

size_t AsyncStdinReader::try_accumulate(size_t max_bytes, std::vector<uint8_t>& out) {
    std::unique_lock lk(m_);
    if (q_.empty()) return 0;

    const size_t want = std::max<size_t>(1, max_bytes);
    size_t acc = 0;
    out.clear();
    out.reserve(std::min(want, queued_bytes_));

    while (!q_.empty() && acc < want) {
        auto& front = q_.front();
        const size_t take = std::min(want - acc, front.size());
        out.insert(out.end(), front.begin(), front.begin() + take);
        acc += take;

        if (take == front.size()) {
            queued_bytes_ -= front.size();
            q_.pop_front();
        } else {
            front.erase(front.begin(), front.begin() + take);
            queued_bytes_ -= take;
        }
    }
    lk.unlock();
    cv_has_space_.notify_all();
    return acc;
}

void AsyncStdinReader::reader_loop_() {
    std::vector<uint8_t> buf(cfg_.chunk_size);

    while (running_.load(std::memory_order_acquire)) {
        ssize_t n = ::read(STDIN_FILENO, buf.data(), buf.size());
        if (n > 0) {
            std::vector<uint8_t> chunk(buf.begin(), buf.begin() + n);

            std::unique_lock lk(m_);
            // If bounded, wait for space to stay lossless.
            if (cfg_.max_buffer_bytes > 0) {
                auto has_space = [&]{
                    return !running_.load(std::memory_order_acquire) ||
                           (queued_bytes_ + static_cast<size_t>(n) <= cfg_.max_buffer_bytes);
                };
                cv_has_space_.wait(lk, has_space);
                if (!running_.load(std::memory_order_acquire)) break;
            }

            q_.push_back(std::move(chunk));
            queued_bytes_ += static_cast<size_t>(n);
            lk.unlock();
            cv_has_data_.notify_all();
            continue;
        }

        if (n == 0) {
            // EOF
            eof_.store(true, std::memory_order_release);
            break;
        }

        // n < 0
        if (errno == EINTR) {
            continue; // retry
        } else {
            // Treat other errors as EOF to unblock consumer gracefully.
            eof_.store(true, std::memory_order_release);
            break;
        }
    }

    // Wake consumers/producers so they can exit
    cv_has_data_.notify_all();
    cv_has_space_.notify_all();
}
