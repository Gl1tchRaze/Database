#include <cstdio>
#include <iostream>
#include "table.hpp"

int main() {
    std::remove("students.db");

    RecordId first{}, last{};
    {
        Table table("students.db");
        for (int i = 1; i <= 1000; ++i) {
            RecordId rid = table.insert(std::to_string(i) + ",Student_" + std::to_string(i) + ",CSE-21");
            if (i == 1) first = rid;
            if (i == 1000) last = rid;
        }
        std::cout << "Inserted 1000 records into " << table.num_pages() << " pages\n";
        std::cout << "First record at (page " << first.page_id << ", slot " << first.slot_id << ")\n";
        std::cout << "Last  record at (page " << last.page_id << ", slot " << last.slot_id << ")\n";
        std::cout << "get(last) = " << table.get(last) << "\n";
    }

    {
        Table table("students.db");   // reopen: records must still be there
        int count = 0;
        table.scan([&](RecordId, const std::string&) { ++count; });
        std::cout << "After reopen, scan found " << count << " records\n";
    }
    return 0;
}