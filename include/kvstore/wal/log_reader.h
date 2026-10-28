// ==============================================================================
// kvstore/wal/log_reader.h - WAL Scan for Recovery (Phase 4)
// ==============================================================================
//
// Reads every complete, CRC-valid record from a wal.log. A torn or corrupt
// record marks the end of the log: everything before it is returned,
// everything from it on is ignored (it can only be an append that never
// committed). A missing file is an empty log.
//
// ==============================================================================

#pragma once

#include <string>
#include <vector>

#include "kvstore/status.h"
#include "kvstore/wal/log_record.h"

namespace kvstore::wal {

class LogReader {
public:
    // Read all complete records in order. LSN continuity is verified from
    // the first record's stored LSN (records must form a contiguous byte
    // chain); a break is treated as end-of-log.
    [[nodiscard("ReadAll returns the recovered records")]]
    static Result<std::vector<LogRecord>> ReadAll(const std::string& path);
};

}  // namespace kvstore::wal
