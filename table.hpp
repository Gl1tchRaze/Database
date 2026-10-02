#pragma once
#include <string>
#include <stdexcept>
#include "pager.hpp"
#include "slotted_page.hpp"

// RecordId = the "address" of a record: which page, which slot
struct RecordId {
    uint32_t page_id;
    uint16_t slot_id;
};

class Table {
public:
    explicit Table(const std::string& path) : pager_(path) {
        if (pager_.num_pages() == 0) new_page();  // brand-new file
    }

    uint32_t num_pages() const { return pager_.num_pages(); }

    RecordId insert(const std::string& rec) {
        if (rec.size() > MAX_RECORD) throw std::length_error("record too large for one page");

        uint32_t page_id = pager_.num_pages() - 1;   // try the last page first
        char buf[PAGE_SIZE];
        pager_.read_page(page_id, buf);
        SlottedPage page(buf);

        int slot = page.insert(rec.data(), static_cast<uint16_t>(rec.size()));
        if (slot < 0) {                              // last page is full
            page_id = new_page();
            pager_.read_page(page_id, buf);
            slot = page.insert(rec.data(), static_cast<uint16_t>(rec.size()));
        }
        pager_.write_page(page_id, buf);
        return {page_id, static_cast<uint16_t>(slot)};
    }

    std::string get(RecordId rid) {
        char buf[PAGE_SIZE];
        pager_.read_page(rid.page_id, buf);
        SlottedPage page(buf);
        const char* data;
        uint16_t len;
        if (!page.get(rid.slot_id, data, len)) throw std::out_of_range("bad slot id");
        return std::string(data, len);
    }

    // Deletes a record. Returns false if it was already gone (bad slot or a
    // tombstone). The slot is tombstoned in place, so all other RecordIds remain
    // valid; the freed space is reclaimed by a later insert on that page.
    bool erase(RecordId rid) {
        char buf[PAGE_SIZE];
        pager_.read_page(rid.page_id, buf);
        SlottedPage page(buf);
        if (!page.erase(rid.slot_id)) return false;
        pager_.write_page(rid.page_id, buf);
        return true;
    }

    // Full table scan: calls fn(RecordId, record) for every LIVE record
    // (tombstoned slots are skipped).
    template <typename Fn>
    void scan(Fn fn) {
        char buf[PAGE_SIZE];
        for (uint32_t p = 0; p < pager_.num_pages(); ++p) {
            pager_.read_page(p, buf);
            SlottedPage page(buf);
            for (uint16_t s = 0; s < page.num_slots(); ++s) {
                const char* data;
                uint16_t len;
                if (page.get(s, data, len))          // skip tombstones
                    fn(RecordId{p, s}, std::string(data, len));
            }
        }
    }

private:
    uint32_t new_page() {
        uint32_t id = pager_.allocate_page();
        char buf[PAGE_SIZE] = {0};   // clean page, then set up the header
        SlottedPage(buf).init();
        pager_.write_page(id, buf);
        return id;
    }

    Pager pager_;
};