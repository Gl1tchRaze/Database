#pragma once
#include <string>
#include <unordered_map>
#include "table.hpp"

// A Table plus a hash index: key (int) -> RecordId.
// Each record is stored as "key,value".
class IndexedTable {
public:
    explicit IndexedTable(const std::string& path) : table_(path) {
        // The index lives only in memory, so rebuild it from the file on open
        table_.scan([&](RecordId rid, const std::string& rec) {
            index_[parse_key(rec)] = rid;
        });
    }

    // Returns false if the key already exists
    bool insert(int key, const std::string& value) {
        if (index_.count(key)) return false;
        RecordId rid = table_.insert(std::to_string(key) + "," + value);
        index_[key] = rid;
        return true;
    }

    // Fast lookup: hash index, then ONE page read
    bool find(int key, std::string& out) {
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        out = table_.get(it->second);
        return true;
    }

    // Slow lookup: full table scan (kept only for comparison)
    bool find_by_scan(int key, std::string& out) {
        bool found = false;
        table_.scan([&](RecordId, const std::string& rec) {
            if (!found && parse_key(rec) == key) {
                out = rec;
                found = true;
            }
        });
        return found;
    }

    size_t size() const { return index_.size(); }
    uint32_t num_pages() const { return table_.num_pages(); }

private:
    static int parse_key(const std::string& rec) {
        return std::stoi(rec.substr(0, rec.find(',')));
    }

    Table table_;
    std::unordered_map<int, RecordId> index_;
};