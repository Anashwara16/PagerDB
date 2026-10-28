// ==============================================================================
// kvstore/storage/lru_replacer.h - LRU Eviction Policy (Phase 3, C++23)
// ==============================================================================
//
// Tracks buffer pool frames in least-recently-used order and picks eviction
// victims among the frames marked evictable (pin_count == 0). Map + list:
// the map gives O(1) lookup of a frame's position, the list keeps recency
// order (front = least recently used).
//
// The replacer knows nothing about pages or pins - the BufferPool tells it
// which frames are evictable.
//
// ==============================================================================

#pragma once

#include <list>
#include <optional>
#include <unordered_map>

#include "kvstore/types.h"

namespace kvstore::storage {

class LruReplacer {
public:
    // Mark `frame` as most recently used, inserting it if unknown.
    // New frames start out non-evictable (the caller just pinned them).
    void RecordAccess(FrameId frame) {
        if (auto it = entries_.find(frame); it != entries_.end()) {
            order_.erase(it->second.position);
            it->second.position = order_.insert(order_.end(), frame);
        } else {
            entries_.emplace(frame,
                             Entry{order_.insert(order_.end(), frame), false});
        }
    }

    // Frames are evictable exactly while their pin count is zero.
    void SetEvictable(FrameId frame, bool evictable) {
        if (auto it = entries_.find(frame); it != entries_.end()) {
            it->second.evictable = evictable;
        }
    }

    // Remove and return the least recently used evictable frame, or nullopt
    // if every tracked frame is pinned.
    [[nodiscard("Evict returns the victim frame")]]
    std::optional<FrameId> Evict() {
        for (FrameId frame : order_) {
            if (entries_.at(frame).evictable) {
                Remove(frame);
                return frame;
            }
        }
        return std::nullopt;
    }

    // Forget a frame entirely (e.g. its page left the pool).
    void Remove(FrameId frame) {
        if (auto it = entries_.find(frame); it != entries_.end()) {
            order_.erase(it->second.position);
            entries_.erase(it);
        }
    }

private:
    struct Entry {
        std::list<FrameId>::iterator position;
        bool evictable;
    };

    std::list<FrameId> order_;  // front = least recently used
    std::unordered_map<FrameId, Entry> entries_;
};

}  // namespace kvstore::storage
