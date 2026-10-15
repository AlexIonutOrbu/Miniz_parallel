#pragma once
#include <string>
#include <thread>
// operations
enum WorkMode : int { COMP = 0,
    DECOMP = 1 };

// Global configuration arguments
struct Config {
    int recursive = 1;                                               // -r 0|1 (default: recur)
    bool remove_input = false;                                       // -C 0|1 or -D 0|1, 1 = remove, 0 = preserve (default 0)
    int level = 6;                                                   // -l 0..9 compression level
    WorkMode mode = COMP;                                            // default: compress
    bool verbose = false;                                            // -v verbose mode
    std::string out_ext = ".defl";                                   // extension for compressed files (default: .defl)
    unsigned int num_threads = std::thread::hardware_concurrency();  // -t number of threads (default: hardware concurrency)
    std::size_t big_file_size = 1024 * 1024 * 4;                     // -s size of a big file (default: 4MB)
    std::size_t small_file_block_size = 1024 * 1024 * 4;             // -b size of a block of small files (default: 1MB)
};

// Unique instance of the global cfg variable (C++17 inline variable)
inline Config cfg;
