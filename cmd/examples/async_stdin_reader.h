
// SPDX-FileCopyrightText: Copyright (c) 2025
// SPDX-License-Identifier: BSD-2-Clause
//
// async_stdin_reader.h  (LOSSLESS VERSION)
//
// Background reader for STDIN that *never drops data*.
// If a max buffer size is set, the producer blocks until there is space.
// By default, buffering is unbounded (max_buffer_bytes == 0).
//
// Platform: POSIX (uses ::read on STDIN_FILENO)
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class AsyncStdinReader {
public:
    struct Config {
        // Size of each read() call. 64 KiB works well for pipes.
        size_t chunk_size = 64 * 1024;
        // 0 == unlimited buffering (WARNING: may grow without bound).
        // >0 == hard cap; when reached, producer blocks until consumer pops.
        size_t max_buffer_bytes = 0;
    };

    explicit AsyncStdinReader(const Config& cfg);
    ~AsyncStdinReader();

    AsyncStdinReader(const AsyncStdinReader&) = delete;
    AsyncStdinReader& operator=(const AsyncStdinReader&) = delete;

    void start();
    void stop();

    bool eof() const noexcept { return eof_.load(std::memory_order_acquire); }

    size_t queued_bytes() const;

    // Strict FIFO pop. If block==true, waits (optionally with timeout)
    // until a chunk is available or EOF with empty queue occurs.
    bool pop(std::vector<uint8_t>& out,
             bool block = true,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(50));

    // NOTE: For lossless pipelines, prefer pop(). pop_latest() is provided
    // only for latency-oriented modes and will *discard* older data explicitly.
    // Since your requirement is "never drop", do NOT call this in production.
    bool pop_latest(std::vector<uint8_t>& out);

    // Try to gather up to max_bytes without blocking. Returns bytes gathered.
    size_t try_accumulate(size_t max_bytes, std::vector<uint8_t>& out);

private:
    void reader_loop_();

    Config cfg_;
    std::atomic<bool> running_{false};
    std::atomic<bool> eof_{false};
    std::thread th_;

    mutable std::mutex m_;
    std::condition_variable cv_has_data_;
    std::condition_variable cv_has_space_;
    std::deque<std::vector<uint8_t>> q_;
    size_t queued_bytes_ = 0;
};
