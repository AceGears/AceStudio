#pragma once

// Internal: in-memory ZIP archives (miniz) with size limits, shared by the
// project format and other AceStudio container files.

#include "acestudio/core/Result.h"

#include <miniz.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace acestudio::detail {

inline constexpr std::uint64_t kMaxEntryBytes = 512ull << 20;     // per ZIP entry, uncompressed
inline constexpr std::uint64_t kMaxTotalBytes = 4ull << 30;       // whole archive, uncompressed

class ZipWriter {
public:
    ZipWriter() { m_ok = mz_zip_writer_init_heap(&m_zip, 0, 0) == MZ_TRUE; }
    ~ZipWriter() { mz_zip_writer_end(&m_zip); }
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    Status add(const std::string& name, std::span<const std::uint8_t> data, bool compress) {
        if (!m_ok) return Error{ErrorCode::Internal, "ZIP writer not initialised"};
        const mz_uint level = compress ? static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION) : static_cast<mz_uint>(MZ_NO_COMPRESSION);
        if (!mz_zip_writer_add_mem(&m_zip, name.c_str(), data.data(), data.size(), level))
            return Error{ErrorCode::Internal, "Failed to add " + name + " to project archive"};
        return {};
    }

    Result<std::vector<std::uint8_t>> finish() {
        void* buf = nullptr;
        std::size_t size = 0;
        if (!m_ok || !mz_zip_writer_finalize_heap_archive(&m_zip, &buf, &size))
            return Error{ErrorCode::Internal, "Failed to finalise project archive"};
        // Ownership of the finalised heap buffer passes to the caller.
        std::unique_ptr<void, decltype(&mz_free)> guard(buf, &mz_free);
        const auto* p = static_cast<const std::uint8_t*>(buf);
        return std::vector<std::uint8_t>(p, p + size);
    }

private:
    mz_zip_archive m_zip{};
    bool m_ok = false;
};

class ZipReader {
public:
    explicit ZipReader(std::span<const std::uint8_t> bytes) {
        m_ok = mz_zip_reader_init_mem(&m_zip, bytes.data(), bytes.size(), 0) == MZ_TRUE;
    }
    ~ZipReader() { mz_zip_reader_end(&m_zip); }
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool ok() const { return m_ok; }
    bool contains(const std::string& name) { return mz_zip_reader_locate_file(&m_zip, name.c_str(), nullptr, 0) >= 0; }

    // Extracts and CRC-verifies one entry, enforcing size limits.
    Result<std::vector<std::uint8_t>> read(const std::string& name) {
        const int idx = mz_zip_reader_locate_file(&m_zip, name.c_str(), nullptr, 0);
        if (idx < 0) return Error{ErrorCode::CorruptData, "Project is missing " + name};
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&m_zip, static_cast<mz_uint>(idx), &st))
            return Error{ErrorCode::CorruptData, "Cannot read entry header for " + name};
        if (st.m_uncomp_size > kMaxEntryBytes) return Error{ErrorCode::CorruptData, name + " is too large"};
        m_total += st.m_uncomp_size;
        if (m_total > kMaxTotalBytes) return Error{ErrorCode::CorruptData, "Project exceeds size limit"};

        std::size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&m_zip, static_cast<mz_uint>(idx), &size, 0);
        if (!p) {
            return Error{ErrorCode::CorruptData, "Entry " + name + " is damaged (" +
                                                     mz_zip_get_error_string(mz_zip_get_last_error(&m_zip)) + ")"};
        }
        std::unique_ptr<void, decltype(&mz_free)> guard(p, &mz_free);
        const auto* b = static_cast<const std::uint8_t*>(p);
        return std::vector<std::uint8_t>(b, b + size);
    }

private:
    mz_zip_archive m_zip{};
    bool m_ok = false;
    std::uint64_t m_total = 0;
};

} // namespace acestudio::detail
