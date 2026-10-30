#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace ota {

// Thread-safe bounded blocking queue (multi-producer / multi-consumer).
//
// * push() blocks while the queue is full -> natural back-pressure on producers.
// * pop()/pop_batch() block while empty and return "nothing" only once the
//   queue is closed AND drained, so shutdown never loses queued items.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) throw std::invalid_argument("BoundedQueue capacity must be > 0");
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // Returns false if the queue was closed before the item could be enqueued.
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_) return false;
        items_.push_back(std::move(item));
        high_watermark_ = std::max(high_watermark_, items_.size());
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Non-blocking push; returns false if full or closed.
    bool try_push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || items_.size() >= capacity_) return false;
        items_.push_back(std::move(item));
        high_watermark_ = std::max(high_watermark_, items_.size());
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Blocks until an item is available; std::nullopt once closed and drained.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    // Drains up to max_items under a single lock acquisition (amortises
    // locking cost for high-rate consumers). Returns 0 once closed and drained.
    std::size_t pop_batch(std::vector<T>& out, std::size_t max_items) {
        out.clear();
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
        const std::size_t n = std::min(max_items, items_.size());
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(std::move(items_.front()));
            items_.pop_front();
        }
        lock.unlock();
        if (n > 0) not_full_.notify_all();
        return n;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    std::size_t high_watermark() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_watermark_;
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t capacity() const { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> items_;
    std::size_t high_watermark_ = 0;
    bool closed_ = false;
};

}  // namespace ota
