#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>

class SpscByteRingBuffer {
public:
    explicit SpscByteRingBuffer(size_t capacityBytes)
        : buffer_(capacityBytes ? capacityBytes : 1), capacity_(buffer_.size()) {}

    size_t Capacity() const { return capacity_; }

    size_t AvailableToWrite() const {
        const uint64_t writePos = writePos_.load(std::memory_order_relaxed);
        const uint64_t readPos = readPos_.load(std::memory_order_acquire);
        return capacity_ - static_cast<size_t>(writePos - readPos);
    }

    size_t AvailableToRead() const {
        const uint64_t writePos = writePos_.load(std::memory_order_acquire);
        const uint64_t readPos = readPos_.load(std::memory_order_relaxed);
        return static_cast<size_t>(writePos - readPos);
    }

    size_t Write(const BYTE* data, size_t bytes) {
        if (bytes == 0) {
            return 0;
        }
        size_t writable = AvailableToWrite();
        if (writable == 0) {
            return 0;
        }
        bytes = std::min(bytes, writable);
        uint64_t writePos = writePos_.load(std::memory_order_relaxed);
        size_t offset = static_cast<size_t>(writePos % capacity_);
        size_t firstPart = std::min(bytes, capacity_ - offset);
        std::memcpy(buffer_.data() + offset, data, firstPart);
        const size_t secondPart = bytes - firstPart;
        if (secondPart > 0) {
            std::memcpy(buffer_.data(), data + firstPart, secondPart);
        }
        writePos_.store(writePos + bytes, std::memory_order_release);
        return bytes;
    }

    size_t Read(BYTE* dest, size_t maxBytes) {
        if (maxBytes == 0) {
            return 0;
        }
        size_t readable = AvailableToRead();
        if (readable == 0) {
            return 0;
        }
        const size_t bytes = std::min(maxBytes, readable);
        uint64_t readPos = readPos_.load(std::memory_order_relaxed);
        size_t offset = static_cast<size_t>(readPos % capacity_);
        size_t firstPart = std::min(bytes, capacity_ - offset);
        std::memcpy(dest, buffer_.data() + offset, firstPart);
        const size_t secondPart = bytes - firstPart;
        if (secondPart > 0) {
            std::memcpy(dest + firstPart, buffer_.data(), secondPart);
        }
        readPos_.store(readPos + bytes, std::memory_order_release);
        return bytes;
    }

private:
    std::vector<BYTE> buffer_;
    const size_t capacity_;
    std::atomic<uint64_t> writePos_{0};
    std::atomic<uint64_t> readPos_{0};
};
