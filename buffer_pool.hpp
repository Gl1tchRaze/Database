#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "pager.hpp"

// ===========================================================================
//  BufferPool - an LRU page cache that sits between the Table / DBEngine and
//  the Pager.
//
//    fetch(id)      : returns a pinned pointer to a page. Serves it from memory
//                     on a HIT, otherwise reads it from disk (a MISS).
//    unpin(id, dirty): releases a pin; `dirty` records that the caller changed
//                     the page so it must be written back later.
//    pin            : a pinned page cannot be evicted (fetch pins, unpin releases)
//    dirty          : a modified page reaches disk only on eviction or flush
//    LRU            : when full, the least recently used UNPINNED page is evicted
//
//  Portability note: this file deliberately avoids C++17-only *syntax* such as
//  structured bindings (`auto& [k, v]`) and init-statements in `if`. The
//  language features it needs from C++17 (scope-exit `try`/`catch` in the
//  destructor, and class template argument deduction in one `emplace`) are both
//  satisfied by any reasonably modern compiler, so the file compiles even when
//  `-std=c++17` is *not* passed explicitly. (Building the project still wants
//  `-std=c++17`; this just removes one needless failure mode.)
//
//  If your compiler is genuinely old, the one line that may need adjusting is
//  marked "C++17 CTAD" below.
// ===========================================================================
class BufferPool {
public:
    BufferPool(Pager& pager, std::size_t capacity)
        : pager_(pager), capacity_(capacity), clock_(0) {
        if (capacity_ < 1) throw std::invalid_argument("capacity must be >= 1");
    }

    ~BufferPool() {
        // Never let an exception escape a destructor.
        try { flush_all(); } catch (...) {}
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // --- core cache operations -------------------------------------------

    // Returns a pinned pointer to the page. The caller MUST call
    // unpin(id, dirty) afterwards. Throws std::out_of_range if `id` is not a
    // page that already exists on disk.
    char* fetch(std::uint32_t id) {
        auto cached = cache_.find(id);
        if (cached != cache_.end()) {                          // HIT
            ++hits_;
            // Move this entry to the front of the LRU list (most recent).
            lru_.splice(lru_.begin(), lru_, cached->second.lru_pos);
            ++cached->second.pins;
            return cached->second.data.data();
        }

        ++misses_;                                             // MISS
        if (cache_.size() >= capacity_) evict_one();

        // Build the new entry first, then insert it. Inserting the map entry
        // before reading from disk keeps any thrown exception from leaving a
        // half-initialised page in the cache.
        Entry entry;
        entry.data.resize(PAGE_SIZE);
        pager_.read_page(id, entry.data.data());
        ++disk_reads_;

        lru_.push_front(id);                                   // most recent
        entry.lru_pos  = lru_.begin();
        entry.pins     = 1;
        entry.dirty    = false;
        // C++17 CTAD: deduced as std::pair<const std::uint32_t, Entry>.
        // If your compiler predates class template argument deduction, replace
        // the insert below with the explicit form in the comment.
        auto inserted = cache_.emplace(id, std::move(entry));
        // Explicit alternative (drop the line above and use this instead):
        //   std::pair<const std::uint32_t, Entry> kv(id, std::move(entry));
        //   auto inserted = cache_.insert(std::move(kv));
        return inserted.first->second.data.data();
    }

    void unpin(std::uint32_t id, bool dirty) {
        auto cached = cache_.find(id);
        if (cached == cache_.end() || cached->second.pins == 0)
            throw std::logic_error("unpin of a page that is not pinned");
        --cached->second.pins;
        if (dirty) cached->second.dirty = true;
    }

    // Allocates a fresh zero page at the end of the file and returns its id.
    // (The page is not placed in the cache here; a subsequent fetch(id) will
    // read it in normally.)
    std::uint32_t new_page() { return pager_.allocate_page(); }

    std::uint32_t num_pages() const { return pager_.num_pages(); }

    // Write every dirty page back to disk. Safe to call repeatedly.
    void flush_all() {
        for (auto iter = lru_.begin(); iter != lru_.end(); ++iter) {
            std::uint32_t id = *iter;
            auto cached = cache_.find(id);
            if (cached == cache_.end()) continue;
            if (cached->second.dirty) {
                pager_.write_page(id, cached->second.data.data());
                ++disk_writes_;
                cached->second.dirty = false;
            }
        }
    }

    // --- statistics ------------------------------------------------------

    std::uint64_t hits()        const { return hits_; }
    std::uint64_t misses()      const { return misses_; }
    std::uint64_t disk_reads()  const { return disk_reads_; }
    std::uint64_t disk_writes() const { return disk_writes_; }

    double hit_rate() const {
        std::uint64_t total = hits_ + misses_;
        if (total == 0) return 0.0;
        return 100.0 * static_cast<double>(hits_) / static_cast<double>(total);
    }

private:
    struct Entry {
        std::vector<char>          data;      // the PAGE_SIZE page bytes
        bool                       dirty = false;
        int                        pins  = 0;
        std::list<std::uint32_t>::iterator lru_pos;  // position in lru_
    };

    // Evict the least-recently-used unpinned page, writing it back if dirty.
    void evict_one() {
        while (!lru_.empty()) {
            std::uint32_t id = lru_.back();          // LRU end
            auto cached = cache_.find(id);
            if (cached == cache_.end()) {            // stale entry; drop it
                lru_.pop_back();
                continue;
            }
            if (cached->second.pins > 0) break;      // everything left is pinned

            if (cached->second.dirty) {              // write back before dropping
                pager_.write_page(id, cached->second.data.data());
                ++disk_writes_;
            }
            lru_.pop_back();
            cache_.erase(cached);
            return;
        }
        throw std::runtime_error("buffer pool full: every page is pinned");
    }

    Pager&        pager_;
    std::size_t   capacity_;
    std::unordered_map<std::uint32_t, Entry> cache_;
    // front = most recently used, back = least recently used
    std::list<std::uint32_t> lru_;
    std::uint64_t clock_;  // reserved for future use / debugging
    std::uint64_t hits_        = 0;
    std::uint64_t misses_      = 0;
    std::uint64_t disk_reads_  = 0;
    std::uint64_t disk_writes_ = 0;
};
