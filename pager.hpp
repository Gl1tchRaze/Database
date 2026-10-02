#pragma once
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

constexpr uint32_t PAGE_SIZE = 4096;  // 4 KB, same as a typical OS page

class Pager {
public:
    explicit Pager(const std::string& path) : path_(path) {
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open()) {
            // File does not exist yet, so create it first
            std::ofstream create(path_, std::ios::binary);
            create.close();
            file_.clear();
            file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        }
        if (!file_.is_open()) throw std::runtime_error("cannot open " + path_);

        file_.seekg(0, std::ios::end);
        num_pages_ = static_cast<uint32_t>(file_.tellg() / PAGE_SIZE);
    }

    ~Pager() { file_.close(); }

    uint32_t num_pages() const { return num_pages_; }

    // Read page `id` from disk into buf (buf must be PAGE_SIZE bytes)
    void read_page(uint32_t id, char* buf) {
        if (id >= num_pages_) throw std::out_of_range("page does not exist");
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(id) * PAGE_SIZE);
        file_.read(buf, PAGE_SIZE);
        if (!file_) throw std::runtime_error("read failed");
    }

    // Write buf (PAGE_SIZE bytes) to page `id`
    void write_page(uint32_t id, const char* buf) {
        if (id > num_pages_) throw std::out_of_range("cannot skip pages");
        file_.clear();
        file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE);
        file_.write(buf, PAGE_SIZE);
        file_.flush();
        if (!file_) throw std::runtime_error("write failed");
        if (id == num_pages_) ++num_pages_;  // file grew by one page
    }

    // Add a new zero-filled page at the end and return its id
    uint32_t allocate_page() {
        std::vector<char> zeros(PAGE_SIZE, 0);
        uint32_t id = num_pages_;
        write_page(id, zeros.data());
        return id;
    }

private:
    std::string path_;
    std::fstream file_;
    uint32_t num_pages_ = 0;
};