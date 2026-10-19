#include <chrono>
#include <cstdio>
#include <iostream>

#include "cmdline.hpp"
#include "config.hpp"
#include "threadPool.hpp"
#include "utility.hpp"

using clock_type = std::chrono::steady_clock;

int main(int argc, char* argv[]) {
    long start = parseCommandLine(argc, argv);
    if (start < 0) return -1;
    bool success = true;
    auto t0 = clock_type::now();
    threadPool p = threadPool(cfg.num_threads);
    while (argv[start]) {
        if (isDirectory(argv[start])) {
            success &= walkDirParallel(argv[start], p);
        } else {
            success &= doWork(argv[start]);
        }
        start++;
    }
    if (!success) {
        printf("Exiting with (some) Error(s)\n");
        return -1;
    }
    printf("Exiting with Success\n");

    auto t1 = clock_type::now();
    double tsecSeq = std::chrono::duration<double>(t1 - t0).count();
    std::cout.setf(std::ios::fixed);
    std::cout.precision(6);
    std::cout << "Parallel time = " << tsecSeq << " (s)\n";
    return 0;
}