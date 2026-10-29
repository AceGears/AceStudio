#pragma once

#include <filesystem>
#include <random>
#include <string>

// Creates a unique temporary directory and removes it on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        m_path = std::filesystem::temp_directory_path() /
                 ("acestudio-test-" + std::to_string(rd()) + "-" + std::to_string(rd()));
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return m_path; }
    std::filesystem::path operator/(const std::string& name) const { return m_path / name; }

private:
    std::filesystem::path m_path;
};
