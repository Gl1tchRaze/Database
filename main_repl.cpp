// ===========================================================================
//  main_repl.cpp - an interactive prompt for the database engine.
//
//  Build (build.bat / build.sh already include this file, or by hand):
//      g++ -std=c++17 -O2 main_repl.cpp -o main_repl
//
//  Run:  ./main_repl        (on Windows: main_repl.exe)
//
//  Type "help" at the prompt to see the commands. Type "exit" to quit.
//  Data is stored in repl.db and survives quitting and restarting.
// ===========================================================================
#include <iostream>
#include <sstream>
#include <string>
#include "db_engine.hpp"

static void print_help() {
    std::cout <<
        "\nCommands:\n"
        "  insert <key> <value>   store a record (key is a whole number)\n"
        "  find <key>             look up a key\n"
        "  update <key> <value>   change an existing key's value\n"
        "  erase <key>            delete a key\n"
        "  range <lo> <hi>        list all keys from lo to hi, in order\n"
        "  count                  how many records are stored\n"
        "  stats                  buffer-pool cache statistics\n"
        "  help                   show this list\n"
        "  exit                   save and quit\n\n";
}

int main(int argc, char** argv) {
    // Optional: pass a filename, e.g.  ./main_repl mydata.db
    std::string path = (argc > 1) ? argv[1] : "repl.db";

    DBEngine db(path, 64);  // 64-page cache
    std::cout << "mini-db ready.  file = " << path
              << "  (" << db.size() << " records loaded)\n";
    print_help();

    std::string line;
    while (true) {
        std::cout << "db> ";
        if (!std::getline(std::cin, line)) break;     // EOF (Ctrl-D / Ctrl-Z)
        if (line.empty()) continue;

        std::istringstream in(line);
        std::string cmd;
        in >> cmd;

        if (cmd == "exit" || cmd == "quit") {
            break;
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "insert" || cmd == "update") {
            int key; std::string value;
            if (!(in >> key >> value)) { std::cout << "usage: " << cmd << " <key> <value>\n"; continue; }
            bool ok = (cmd == "insert") ? db.insert(key, value) : db.update(key, value);
            if (ok) std::cout << "ok\n";
            else    std::cout << (cmd == "insert" ? "key already exists\n" : "no such key\n");
        } else if (cmd == "find") {
            int key;
            if (!(in >> key)) { std::cout << "usage: find <key>\n"; continue; }
            std::string value;
            if (db.find(key, value)) std::cout << key << " = " << value << "\n";
            else                     std::cout << key << " = (not found)\n";
        } else if (cmd == "erase") {
            int key;
            if (!(in >> key)) { std::cout << "usage: erase <key>\n"; continue; }
            std::cout << (db.erase(key) ? "ok\n" : "no such key\n");
        } else if (cmd == "range") {
            int lo, hi;
            if (!(in >> lo >> hi)) { std::cout << "usage: range <lo> <hi>\n"; continue; }
            int n = 0;
            db.range(lo, hi, [&](int k, const std::string& v) {
                std::cout << "  " << k << " = " << v << "\n";
                ++n;
            });
            std::cout << "(" << n << " record" << (n == 1 ? "" : "s") << ")\n";
        } else if (cmd == "count") {
            std::cout << db.size() << " records\n";
        } else if (cmd == "stats") {
            std::cout << "pages: " << db.num_pages()
                      << "  cache hit rate: " << db.hit_rate() << "%"
                      << "  disk reads: " << db.disk_reads()
                      << "  disk writes: " << db.disk_writes() << "\n";
        } else {
            std::cout << "unknown command: " << cmd << "  (type \"help\")\n";
        }
    }

    db.flush();
    std::cout << "saved " << db.size() << " records to " << path << ". bye.\n";
    return 0;
}
