#pragma once
#include <cstdint>
#include <cstring>
#include "pager.hpp"

// Page layout (4096 bytes):
// [ header | slot 0 | slot 1 | ... | free space | ... record 1 | record 0 ]
//   header = num_slots (2) + free_end (2) + live_bytes (2) = 6 bytes
//   slot   = offset (2 bytes) + length (2 bytes)
// Slots grow forward from the header; records grow backward from the end of the
// page. `free_end` is where the record area currently starts.
//
// Deletion keeps the page simple and every other RecordId valid:
//   - a deleted slot keeps its number but its length is set to 0 (a "tombstone")
//   - record bytes are NOT shifted, so surviving (page,slot) addresses still work
//   - the slot is reused by the next insert, so slot numbers are never reused
//     for a *different* live record and never need renumbering
//   - `free_space()` counts both the gap between the slot array and the record
//     area, plus the holes left behind by tombstones (see dead_space())

constexpr uint16_t HEADER_SIZE = 6;
constexpr uint16_t SLOT_SIZE = 4;
constexpr uint16_t MAX_RECORD = PAGE_SIZE - HEADER_SIZE - SLOT_SIZE;

class SlottedPage {
public:
    explicit SlottedPage(char* buf) : buf_(buf) {}

    void init() {
        set16(0, 0);          // num_slots = 0
        set16(2, PAGE_SIZE);  // free_end = end of page
        set16(4, 0);          // live_bytes = 0
    }

    uint16_t num_slots() const { return get16(0); }

    // Live records currently stored (tombstones are not counted)
    uint16_t live_count() const {
        uint16_t n = 0;
        for (uint16_t s = 0; s < num_slots(); ++s)
            if (!is_deleted(s)) ++n;
        return n;
    }

    // Space available for a new record: the gap between the slot array and the
    // record area, plus the bytes reclaimable from tombstoned records.
    uint16_t free_space() const {
        return static_cast<uint16_t>(contiguous_gap() + dead_space());
    }

    // True if `slot` exists and currently holds no live record
    bool is_deleted(uint16_t slot) const {
        if (slot >= num_slots()) return false;
        return get16(slot_pos(slot) + 2) == 0;
    }

    // Returns slot id, or -1 if the page genuinely has no room.
    // Reuses the lowest tombstoned slot number if one exists, else appends a new
    // slot. If tombstone holes are needed to make the record fit, the page is
    // compacted first so the freed bytes become usable. Live records may be
    // moved by compaction, but their slot numbers (and thus RecordIds) do not
    // change, so external (page,slot) addresses stay valid.
    int insert(const char* data, uint16_t len) {
        int reuse = find_dead_slot();
        uint16_t slot_cost = (reuse < 0) ? SLOT_SIZE : 0;  // a brand-new slot costs 4 bytes up front

        if (free_space() < len + slot_cost) return -1;     // truly no room, even after compaction

        // The record bytes are written at the back and the (possible) new slot
        // grows at the front; both must fit in the single contiguous gap. If the
        // gap alone is too small but tombstone holes would cover it, compact.
        if (contiguous_gap() < len + slot_cost) compact();

        uint16_t slot;
        if (reuse >= 0) {
            slot = static_cast<uint16_t>(reuse);           // recycle the slot number
        } else {
            slot = num_slots();
            set16(0, slot + 1);                            // one more slot
        }

        uint16_t new_end = static_cast<uint16_t>(free_end() - len);
        std::memcpy(buf_ + new_end, data, len);            // record goes at the back

        uint16_t sp = slot_pos(slot);
        set16(sp, new_end);                                // slot.offset
        set16(sp + 2, len);                                // slot.length
        set16(2, new_end);                                 // free_end moves left
        set16(4, static_cast<uint16_t>(get16(4) + len));   // live_bytes grows
        return slot;
    }

    bool get(uint16_t slot, const char*& data, uint16_t& len) const {
        if (slot >= num_slots()) return false;
        uint16_t sp = slot_pos(slot);
        len = get16(sp + 2);
        if (len == 0) return false;                   // tombstone: no live record
        data = buf_ + get16(sp);
        return true;
    }

    // Marks `slot` deleted. Returns false if missing or already deleted.
    // The reclamation bookkeeping is a single subtraction (via live_bytes), so
    // all other RecordIds on the page stay valid and no bytes move.
    bool erase(uint16_t slot) {
        if (slot >= num_slots()) return false;
        uint16_t sp = slot_pos(slot);
        uint16_t len = get16(sp + 2);
        if (len == 0) return false;                   // already a tombstone
        set16(sp + 2, 0);                             // length 0 = tombstone
        set16(4, static_cast<uint16_t>(get16(4) - len));
        return true;
    }

private:
    // Byte offset of slot `s`'s 4-byte entry within the page.
    static uint16_t slot_pos(uint16_t s) {
        return static_cast<uint16_t>(HEADER_SIZE + s * SLOT_SIZE);
    }

    // The single contiguous free run between the slot array (front) and the
    // record area (back). This is where a new slot entry and freshly written
    // record bytes must physically go.
    uint16_t contiguous_gap() const {
        return static_cast<uint16_t>(free_end() - slot_pos(num_slots()));
    }

    // Rewrites all live records packed tightly against the end of the page,
    // turning tombstone holes back into contiguous free space. Slot numbers are
    // preserved (tombstone slots keep length 0), so RecordIds remain valid.
    void compact() {
        char tmp[PAGE_SIZE];
        uint16_t end = PAGE_SIZE;
        uint16_t n = num_slots();
        for (uint16_t s = 0; s < n; ++s) {
            uint16_t sp = slot_pos(s);
            uint16_t len = get16(sp + 2);
            if (len == 0) continue;                        // tombstone: leave as-is
            uint16_t off = get16(sp);
            end = static_cast<uint16_t>(end - len);
            std::memcpy(tmp + end, buf_ + off, len);       // relocate into temp
            set16(sp, end);                                // point slot at new home
        }
        std::memcpy(buf_ + end, tmp + end, static_cast<size_t>(PAGE_SIZE - end));
        set16(2, end);                                     // free_end = packed start
        // live_bytes is unchanged by compaction
    }

    // The record region runs from free_end to the end of the page and holds
    // only record bytes: live records plus the holes left by tombstones. (The
    // slot array lives at the FRONT, before free_end, so it is not in here.)
    // Dead/reclaimable bytes are therefore just region size minus live bytes.
    uint16_t dead_space() const {
        uint16_t used = static_cast<uint16_t>(PAGE_SIZE - free_end());
        return static_cast<uint16_t>(used - get16(4));
    }

    int find_dead_slot() const {
        uint16_t n = num_slots();
        for (uint16_t s = 0; s < n; ++s)
            if (get16(slot_pos(s) + 2) == 0) return s;
        return -1;
    }

    uint16_t free_end() const { return get16(2); }

    // `pos` is a byte offset within a single 4096-byte page, so it always fits
    // comfortably in a uint16_t. Taking uint16_t here (instead of size_t) keeps
    // every caller's offset arithmetic in the same unsigned type and avoids the
    // -Wsign-conversion warnings that an int->size_t conversion would raise.
    uint16_t get16(uint16_t pos) const {
        uint16_t v;
        std::memcpy(&v, buf_ + pos, sizeof(v));
        return v;
    }
    void set16(uint16_t pos, uint16_t v) { std::memcpy(buf_ + pos, &v, sizeof(v)); }

    char* buf_;
};
