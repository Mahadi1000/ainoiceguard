/**
 * Lock-free Single-Producer Single-Consumer (SPSC) ring buffer for real-time audio.
 *
 * RULES FOR REAL-TIME AUDIO:
 * - No allocations in the audio callback or processing thread after construction.
 * - No locks, no syscalls, no blocking. Use atomics only.
 * - Capacity must be power-of-2 for O(1) indexing via bitwise mask.
 * - Producer = capture callback; Consumer = processing thread (or vice versa for output).
 */

#ifndef NOISEGUARD_RINGBUFFER_H
#define NOISEGUARD_RINGBUFFER_H

#include <atomic>
#include <cstddef>
#include <cstring>

namespace noiseguard {

/** Round up to next power of 2 (for capacity). */
inline size_t nextPowerOf2(size_t n) {
  if (n == 0) return 1;
  n--;
  for (size_t i = 1; i < sizeof(size_t) * 8; i *= 2) n |= n >> i;
  return n + 1;
}

class RingBuffer {
 public:
  /** capacity will be rounded up to next power of 2. No allocations after this. */
  explicit RingBuffer(size_t capacity)
      : capacity_(nextPowerOf2(capacity)), mask_(capacity_ - 1) {
    buffer_ = new float[capacity_];
  }

  ~RingBuffer() { delete[] buffer_; }

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;

  /** Number of samples available to read. */
  size_t available_read() const {
    size_t w = write_idx_.load(std::memory_order_acquire);
    size_t r = read_idx_.load(std::memory_order_acquire);
    if (w >= r) return w - r;
    return capacity_ - (r - w);
  }

  /** Number of sample slots available to write. */
  size_t available_write() const { return capacity_ - available_read() - 1; }

  /** Write up to count samples. Returns number actually written. */
  size_t write(const float* src, size_t count) {
    size_t w = write_idx_.load(std::memory_order_relaxed);
    size_t r = read_idx_.load(std::memory_order_acquire);
    size_t used = (w >= r) ? (w - r) : (capacity_ - (r - w));
    size_t free = capacity_ - used - 1;
    if (count > free) count = free;
    if (count == 0) return 0;
    for (size_t i = 0; i < count; i++) {
      buffer_[(w + i) & mask_] = src[i];
    }
    write_idx_.store(w + count, std::memory_order_release);
    return count;
  }

  /** Read up to count samples. Returns number actually read. */
  size_t read(float* dst, size_t count) {
    size_t r = read_idx_.load(std::memory_order_relaxed);
    size_t w = write_idx_.load(std::memory_order_acquire);
    size_t used = (w >= r) ? (w - r) : (capacity_ - (r - w));
    if (count > used) count = used;
    if (count == 0) return 0;
    for (size_t i = 0; i < count; i++) {
      dst[i] = buffer_[(r + i) & mask_];
    }
    read_idx_.store(r + count, std::memory_order_release);
    return count;
  }

  size_t capacity() const { return capacity_; }

 private:
  const size_t capacity_;
  const size_t mask_;
  float* buffer_;
  std::atomic<size_t> read_idx_{0};
  std::atomic<size_t> write_idx_{0};
};

}  // namespace noiseguard

#endif  // NOISEGUARD_RINGBUFFER_H
