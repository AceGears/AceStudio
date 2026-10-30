// Helper process for test_subprocess. Behaviour is chosen by argv[1].
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "echo";
    if (mode == "echo") {   // echo stdin lines until EOF or "quit"
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "quit") return 0;
            std::cout << "echo:" << line << std::endl;
            std::cerr << "err:" << line << std::endl;
        }
        std::cout << "stdin-closed" << std::endl;
        return 0;
    }
    if (mode == "exit") return argc > 2 ? std::atoi(argv[2]) : 0;
    if (mode == "args") {
        for (int i = 2; i < argc; ++i) std::cout << "[" << argv[i] << "]" << std::endl;
        return 0;
    }
    if (mode == "env") {
        const char* v = std::getenv(argc > 2 ? argv[2] : "ACE_TEST");
        std::cout << (v ? v : "<unset>") << std::endl;
        return 0;
    }
    if (mode == "cwd") {
        std::cout << std::filesystem::current_path().string() << std::endl;
        return 0;
    }
    if (mode == "spam") {   // many lines, one without a newline at the end
        const int n = argc > 2 ? std::atoi(argv[2]) : 1000;
        for (int i = 0; i < n; ++i) std::printf("line %d\n", i);
        std::printf("tail-without-newline");
        std::fflush(stdout);
        return 3;
    }
    if (mode == "sleep") {
        std::cout << "sleeping" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(argc > 2 ? std::atoi(argv[2]) : 60));
        return 0;
    }
    return 99;
}
