#include <cstdio>
#include <cstring>
#include <iostream>
#include "pager.hpp"

int main() {
    std::remove("test.db");  // start fresh for a repeatable demo

    {
        Pager pager("test.db");
        uint32_t id = pager.allocate_page();   // page 0

        char buf[PAGE_SIZE] = {0};
        std::strcpy(buf, "Hello, mini database!");
        pager.write_page(id, buf);

        std::cout << "Wrote page " << id << ", total pages = " << pager.num_pages() << "\n";
    }   // Pager destroyed here, so the file is closed

    {
        Pager pager("test.db");                // reopen: data must survive
        char buf[PAGE_SIZE];
        pager.read_page(0, buf);
        std::cout << "Read back: " << buf << "\n";
        std::cout << "File has " << pager.num_pages() << " page(s)\n";
    }
    return 0;
}