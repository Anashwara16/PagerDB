// ==============================================================================
// kvstore/status.h
// ==============================================================================
//
// Defines the kvstore error/result model.
//
// APIs that can fail return Status for operations without a value, or Result<T>
// for operations that produce a value. StatusCode identifies the error category;
// Status::message() carries operation-specific diagnostic text for logs and
// debugging.
//
// The storage engine reports errors explicitly instead of throwing exceptions.
// Callers are expected to check returned Status and Result<T> values and
// propagate failures with the original Status when possible.
//
// ==============================================================================

#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace kvstore {

// Broad error categories. Operation-specific details belong in Status::message().
enum class StatusCode {
    // Operation completed successfully.
    kOk,

    // Requested key, page, file, or record does not exist.
    kNotFound,

    // Filesystem or device operation failed.
    kIoError,

    // Persisted data failed validation or decoding.
    kCorruption,

    // Caller supplied invalid input.
    kInvalidArgument,

    // Resource is temporarily unavailable.
    kBusy,

    // Internal invariant violation.
    kInternal,

    // Operation cannot proceed due to capacity limits.
    kNoSpace,
};

// Lightweight value type used to report operation success or failure.
//
// A default-constructed Status is successful. Error statuses carry a non-kOk
// StatusCode and may include diagnostic text suitable for logs.
class Status {
public:
    Status() = default;

    Status(StatusCode code, std::string msg)
        : code_(code), msg_(std::move(msg)) {}

    static Status Ok() {
        return Status();
    }

    static Status NotFound(std::string_view msg) {
        return Status(StatusCode::kNotFound, std::string(msg));
    }

    static Status IoError(std::string_view msg) {
        return Status(StatusCode::kIoError, std::string(msg));
    }

    static Status Corruption(std::string_view msg) {
        return Status(StatusCode::kCorruption, std::string(msg));
    }

    static Status InvalidArgument(std::string_view msg) {
        return Status(StatusCode::kInvalidArgument, std::string(msg));
    }

    static Status Busy(std::string_view msg) {
        return Status(StatusCode::kBusy, std::string(msg));
    }

    static Status Internal(std::string_view msg) {
        return Status(StatusCode::kInternal, std::string(msg));
    }

    static Status NoSpace(std::string_view msg) {
        return Status(StatusCode::kNoSpace, std::string(msg));
    }

    [[nodiscard]] bool ok() const {
        return code_ == StatusCode::kOk;
    }

    [[nodiscard]] StatusCode code() const {
        return code_;
    }

    [[nodiscard]] std::string_view message() const {
        return msg_;
    }

    [[nodiscard]] std::string ToString() const {
        if (ok()) {
            return "Ok";
        }

        std::string result;
        switch (code_) {
            case StatusCode::kOk:              result = "Ok"; break;
            case StatusCode::kNotFound:        result = "NotFound"; break;
            case StatusCode::kIoError:         result = "IoError"; break;
            case StatusCode::kCorruption:      result = "Corruption"; break;
            case StatusCode::kInvalidArgument: result = "InvalidArgument"; break;
            case StatusCode::kBusy:            result = "Busy"; break;
            case StatusCode::kInternal:        result = "Internal"; break;
            case StatusCode::kNoSpace:         result = "NoSpace"; break;
        }

        if (!msg_.empty()) {
            result += ": ";
            result += msg_;
        }

        return result;
    }

private:
    StatusCode code_ = StatusCode::kOk;
    std::string msg_;
};

// Result type for operations that either produce T or fail with Status.
template <typename T>
using Result = std::expected<T, Status>;

// Wraps a Status in std::unexpected for returning Result<T> failures.
template <typename T>
[[nodiscard]] std::unexpected<Status> Err(Status s) {
    return std::unexpected(std::move(s));
}

[[nodiscard]] inline std::unexpected<Status> ErrNotFound(std::string_view msg) {
    return std::unexpected(Status::NotFound(msg));
}

[[nodiscard]] inline std::unexpected<Status> ErrIoError(std::string_view msg) {
    return std::unexpected(Status::IoError(msg));
}

[[nodiscard]] inline std::unexpected<Status> ErrCorruption(std::string_view msg) {
    return std::unexpected(Status::Corruption(msg));
}

[[nodiscard]] inline std::unexpected<Status> ErrInvalidArgument(std::string_view msg) {
    return std::unexpected(Status::InvalidArgument(msg));
}

[[nodiscard]] inline std::unexpected<Status> ErrBusy(std::string_view msg) {
    return std::unexpected(Status::Busy(msg));
}

[[nodiscard]] inline std::unexpected<Status> ErrInternal(std::string_view msg) {
    return std::unexpected(Status::Internal(msg));
}

[[nodiscard]] inline std::unexpected<Status> ErrNoSpace(std::string_view msg) {
    return std::unexpected(Status::NoSpace(msg));
}

}  // namespace kvstore
