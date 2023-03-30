#include "duckdb/web/io/file_stats.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>

#include "arrow/buffer.h"
#include "arrow/result.h"
#include "duckdb/web/utils/parallel.h"

namespace duckdb {
namespace web {
namespace io {

/// Encode the nibble
static uint16_t as_nibble(uint64_t hits) {
    size_t hitsGEQ = 0;
    for (size_t value = 0; value < 16; ++value) {
        if (hits >= ((1 << value) - 1)) {
            hitsGEQ = value;
        } else {
            break;
        }
    }
    return hitsGEQ;
};

/// Resize the file
void FileStatisticsCollector::Resize(uint64_t n) {
    std::unique_lock<LightMutex> collector_guard{collector_mutex_};
    auto block_count = std::max<size_t>(n >> MIN_RANGE_SHIFT, 1);
    auto block_shift = MIN_RANGE_SHIFT;
    for (; block_count > MAX_RANGE_COUNT; block_count >>= 1, ++block_shift)
        ;
    block_count = ((block_count << block_shift) < n) ? (block_count + 1) : block_count;
    assert((block_count << block_shift) >= n);
    if (block_count == block_count_ && block_shift == block_shift_) return;
    block_stats_ = unique_ptr<BlockStatistics[]>(new BlockStatistics[block_count]);
    block_shift_ = block_shift;
    block_count_ = block_count;
    std::memset(block_stats_.get(), 0, block_count_ * sizeof(BlockStatistics));
}

/// Encode the block statistics
arrow::Result<std::shared_ptr<arrow::Buffer>> FileStatisticsCollector::ExportStatistics() const {
    auto buffer = arrow::AllocateBuffer(sizeof(FileStatisticsCollector::ExportFileStatistics) +
                                        block_count_ * sizeof(ExportedBlockStats));
    auto writer = buffer.ValueOrDie()->mutable_data();
    auto* out = reinterpret_cast<ExportFileStatistics*>(writer);
    out->bytes_file_cold = bytes_file_read_cold_;
    out->bytes_file_ahead = bytes_file_read_ahead_;
    out->bytes_file_cached = bytes_file_read_cached_;
    out->bytes_file_write = bytes_file_write_;
    out->bytes_page_access = bytes_page_access_;
    out->bytes_page_load = bytes_page_load_;
    out->block_size = 1 << block_shift_;
    for (size_t i = 0; i < block_count_; ++i) {
        auto& b = block_stats_[i];
        auto& o = out->block_stats[i];
        o[0] = as_nibble(b.file_write) | (as_nibble(b.file_read_cold) << 4);
        o[1] = as_nibble(b.file_read_ahead) | (as_nibble(b.file_read_cached) << 4);
        o[2] = as_nibble(b.page_access) | (as_nibble(b.page_load) << 4);
    }
    return buffer;
}

/// Tracks file statistics?
bool FileStatisticsRegistry::TracksFile(std::string_view file_name) {
    std::unique_lock<LightMutex> reg_guard{registry_mutex_};
    auto iter = collectors_.find(std::string{file_name});
    return iter != collectors_.end();
}

/// Find a collector
std::shared_ptr<FileStatisticsCollector> FileStatisticsRegistry::FindCollector(std::string_view file_name) {
    std::unique_lock<LightMutex> reg_guard{registry_mutex_};
    auto iter = collectors_.find(std::string{file_name});
    return (iter != collectors_.end()) ? iter->second : nullptr;
}

/// Enable a collector (if exists)
std::shared_ptr<FileStatisticsCollector> FileStatisticsRegistry::EnableCollector(std::string_view file_name,
                                                                                 bool enable) {
    std::unique_lock<LightMutex> reg_guard{registry_mutex_};
    auto iter = collectors_.find(std::string{file_name});
    if (iter != collectors_.end()) {
        iter->second->Activate(enable);
        return iter->second;
    } else if (enable) {
        auto collector = std::make_shared<FileStatisticsCollector>();
        collectors_.insert({std::string{file_name}, collector});
        return collector;
    }
    return nullptr;
}

/// Export block statistics
arrow::Result<std::shared_ptr<arrow::Buffer>> FileStatisticsRegistry::ExportStatistics(std::string_view path) {
    std::unique_lock<LightMutex> reg_guard{registry_mutex_};
    auto iter = collectors_.find(std::string{path});
    if (iter != collectors_.end()) {
        return iter->second->ExportStatistics();
    }
    return nullptr;
}

}  // namespace io
}  // namespace web
}  // namespace duckdb
