#pragma once
#include <stdexcept>
#include <string>
#include "pager.hpp"
#include "buffer_pool.hpp"
#include "slotted_page.hpp"
#include "bplus_tree.hpp"   // also pulls in table.hpp for RecordId

// ===========================================================================
// Stage 4 capstone: a small storage engine that ties the whole project
// together. It is the first layer where every piece is used at once.
//
//   Pager        raw page read/write on the file
//   BufferPool   LRU page cache + dirty tracking in front of the Pager; ALL
//                page access below goes through it, so every read is a cache
//                hit or a disk miss we can measure
//   SlottedPage  variable-length records inside a 4 KB page, with delete +
//                compaction so freed space is genuinely reusable
//   BPlusTree    ordered int-key index -> RecordId, giving O(log n) point
//                lookups AND sorted range scans (which a hash index cannot do)
//
// Records are stored as "key,value". Keys are unique. The index lives in memory
// and is rebuilt from the heap (through the cache) when the engine is opened.
// ===========================================================================
class DBEngine {
public:
    explicit DBEngine(const std::string& path, size_t cache_pages = 16)
        : pager_(path), pool_(pager_, cache_pages) {
        if (pool_.num_pages() == 0) new_page();   // brand-new file: start page 0
        rebuild_index();
    }

    // Insert a new key. Returns false if the key already exists.
    bool insert(int key, const std::string& value) {
        RecordId existing;
        if (index_.find(key, existing)) return false;
        RecordId rid = heap_insert(encode(key, value));
        index_.insert(key, rid);
        return true;
    }

    // Point lookup: one index descent, then (at most) one page fetch.
    bool find(int key, std::string& value_out) {
        RecordId rid;
        if (!index_.find(key, rid)) return false;
        value_out = decode_value(heap_get(rid));
        return true;
    }

    // Delete a key. Returns false if it was not present. Frees the record's
    // bytes on its page (reclaimable by a later insert) and drops it from the
    // index.
    bool erase(int key) {
        RecordId rid;
        if (!index_.find(key, rid)) return false;
        heap_erase(rid);
        index_.remove(key);
        return true;
    }

    // Change an existing key's value. Returns false if the key is absent.
    // Implemented as delete-then-insert so the new value can land wherever it
    // fits (including the space just freed).
    bool update(int key, const std::string& value) {
        if (!erase(key)) return false;
        insert(key, value);
        return true;
    }

    // Ordered range scan over [lo, hi]: calls fn(key, value) in sorted key
    // order. This is the headline feature of the B+ tree over a hash index.
    template <typename Fn>
    void range(int lo, int hi, Fn fn) {
        index_.range(lo, hi, [&](int k, RecordId rid) {
            fn(k, decode_value(heap_get(rid)));
        });
    }

    // Flush dirty pages to disk without closing the engine.
    void flush() { pool_.flush_all(); }

    // ---- stats ----
    size_t   size()       const { return index_.size(); }
    int      height()     const { return index_.height(); }
    uint32_t num_pages()  const { return pager_.num_pages(); }
    uint64_t cache_hits()   const { return pool_.hits(); }
    uint64_t cache_misses() const { return pool_.misses(); }
    double   hit_rate()     const { return pool_.hit_rate(); }
    uint64_t disk_reads()   const { return pool_.disk_reads(); }
    uint64_t disk_writes()  const { return pool_.disk_writes(); }

private:
    static std::string encode(int key, const std::string& value) {
        return std::to_string(key) + "," + value;
    }
    static int parse_key(const std::string& rec) {
        return std::stoi(rec.substr(0, rec.find(',')));
    }
    static std::string decode_value(const std::string& rec) {
        return rec.substr(rec.find(',') + 1);
    }

    uint32_t new_page() {
        uint32_t id = pool_.new_page();        // grow the file by one zero page
        char* buf = pool_.fetch(id);
        SlottedPage(buf).init();               // lay down the slotted-page header
        pool_.unpin(id, true);                 // dirty: header must be persisted
        return id;
    }

    // First-fit heap insert: try the last page (append locality), then scan the
    // rest for any page with room (this is what reuses space freed by deletes),
    // and only allocate a new page if nothing fits.
    RecordId heap_insert(const std::string& rec) {
        if (rec.size() > MAX_RECORD) throw std::length_error("record too large for one page");

        int slot = -1;
        uint32_t np = pool_.num_pages();
        if (np > 0 && try_insert(np - 1, rec, slot))
            return {np - 1, static_cast<uint16_t>(slot)};
        for (uint32_t p = 0; p + 1 < np; ++p)
            if (try_insert(p, rec, slot))
                return {p, static_cast<uint16_t>(slot)};

        uint32_t id = new_page();
        try_insert(id, rec, slot);             // always fits in a fresh page
        return {id, static_cast<uint16_t>(slot)};
    }

    bool try_insert(uint32_t pid, const std::string& rec, int& slot_out) {
        char* buf = pool_.fetch(pid);
        SlottedPage page(buf);
        int slot = page.insert(rec.data(), static_cast<uint16_t>(rec.size()));
        pool_.unpin(pid, slot >= 0);           // dirty only if we actually wrote
        if (slot < 0) return false;
        slot_out = slot;
        return true;
    }

    std::string heap_get(RecordId rid) {
        char* buf = pool_.fetch(rid.page_id);
        SlottedPage page(buf);
        const char* data;
        uint16_t len;
        bool ok = page.get(rid.slot_id, data, len);
        std::string out = ok ? std::string(data, len) : std::string();
        pool_.unpin(rid.page_id, false);
        if (!ok) throw std::out_of_range("record missing (already deleted?)");
        return out;
    }

    void heap_erase(RecordId rid) {
        char* buf = pool_.fetch(rid.page_id);
        SlottedPage page(buf);
        bool ok = page.erase(rid.slot_id);
        pool_.unpin(rid.page_id, ok);
    }

    void rebuild_index() {
        uint32_t np = pool_.num_pages();
        for (uint32_t p = 0; p < np; ++p) {
            char* buf = pool_.fetch(p);
            SlottedPage page(buf);
            for (uint16_t s = 0; s < page.num_slots(); ++s) {
                const char* data;
                uint16_t len;
                if (page.get(s, data, len))    // live record only
                    index_.insert(parse_key(std::string(data, len)), RecordId{p, s});
            }
            pool_.unpin(p, false);
        }
    }

    // Declaration order matters: pool_ is destroyed before pager_, and the
    // BufferPool destructor flushes dirty pages, so the pager is still alive
    // when that final flush happens.
    Pager pager_;
    BufferPool pool_;
    BPlusTree index_{64};
};
