// ==============================================================================
// kvstore/status.h - Error Handling (C++23 Version)
// ==============================================================================
//
// This file defines our error handling strategy. In database systems, many
// operations can fail (disk full, file not found, corruption detected, etc.),
// so we need a consistent way to report errors.
//
// Design Choices:
// ---------------
//
// Why not use exceptions?
//   Exceptions are controversial in systems programming because:
//   1. They have runtime overhead (even when not thrown)
//   2. They can be hard to reason about (any function might throw)
//   3. Many codebases (Google, LLVM, game engines) avoid them
//   4. Error handling is explicit with Status - you can't ignore it
//
// Why std::expected (C++23)?
//   C++23 introduced std::expected<T, E>, which is exactly what we need!
//   It's a standard way to return either a value OR an error. Think of it
//   like a box that contains either your result or an error message.
//   
//   We no longer need our custom Result<T> template - the standard library
//   now provides this functionality, which means:
//   - Better compiler support and optimizations
//   - Familiar API for other C++ developers  
//   - Interoperability with other libraries
//
// The Pattern:
//   Status DoSomething() {
//       if (error_condition) {
//           return Status(StatusCode::kIoError, "disk full");
//       }
//       return Status::Ok();
//   }
//
//   // Caller:
//   Status s = DoSomething();
//   if (!s.ok()) {
//       std::cerr << "Error: " << s.message() << std::endl;
//       return s;  // Propagate error
//   }
//
// For functions returning values:
//   std::expected<int, Status> ParseNumber(std::string_view s) {
//       if (s.empty()) {
//           return std::unexpected(Status::InvalidArgument("empty string"));
//       }
//       return std::stoi(std::string(s));
//   }
//
// ==============================================================================

#pragma once

#include <expected>      // C++23: std::expected, std::unexpected
#include <string>
#include <string_view>
#include <utility>       // For std::move

namespace kvstore {

// ==============================================================================
// Status Codes
// ==============================================================================
//
// These are the categories of errors that can occur. Keep this list small
// and general - specific details go in the message string.

enum class StatusCode {
    // Success - operation completed normally
    kOk,

    // The requested key was not found in the database
    kNotFound,

    // A disk I/O operation failed (read, write, sync, etc.)
    kIoError,

    // Data on disk doesn't match expected format or checksum
    kCorruption,

    // The caller provided invalid arguments
    kInvalidArgument,

    // Resource is temporarily unavailable (e.g., locked by another operation)
    kBusy,

    // An internal logic error - indicates a bug in our code
    kInternal,

    // Operation would exceed a limit (e.g., page full, no free pages)
    kNoSpace,
};

// ==============================================================================
// Status Class
// ==============================================================================
//
// Status combines a code with an optional message. The message provides
// human-readable details for debugging.
//
// Usage:
//   Status s = Status::IoError("write failed: disk full");
//   if (!s.ok()) {
//       LOG << s.message();  // "write failed: disk full"
//   }

class Status {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Default constructor creates an "Ok" status
    // This is intentional - success should be easy, errors require explanation
    Status() = default;

    // Construct with code and message
    Status(StatusCode code, std::string msg)
        : code_(code), msg_(std::move(msg)) {}

    // -------------------------------------------------------------------------
    // Static Factory Methods
    // -------------------------------------------------------------------------
    // These provide a cleaner API than raw constructors

    // Success status
    static Status Ok() { 
        return Status(); 
    }

    // Common error types with message
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

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    // Returns true if this status represents success
    [[nodiscard]] bool ok() const { 
        return code_ == StatusCode::kOk; 
    }

    // Returns the error code
    [[nodiscard]] StatusCode code() const { 
        return code_; 
    }

    // Returns the error message (empty for Ok status)
    [[nodiscard]] std::string_view message() const { 
        return msg_; 
    }

    // -------------------------------------------------------------------------
    // String Conversion
    // -------------------------------------------------------------------------

    // Returns a human-readable representation
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

// ==============================================================================
// Result<T> Type Alias - Using std::expected (C++23)
// ==============================================================================
//
// In C++23, we can use std::expected<T, E> from the standard library instead
// of writing our own Result<T> template. This is a huge improvement!
//
// std::expected<T, Status> works like this:
//   - It holds either a value of type T (the "expected" case)
//   - OR an error of type Status (the "unexpected" case)
//
// Key differences from our old custom Result<T>:
//   - Use std::unexpected(status) to create an error result
//   - Use .has_value() or implicit bool conversion to check success
//   - Use .value() to get the value (throws if error!)
//   - Use .error() to get the Status when there's an error
//   - Use .value_or(default) to get value or a default on error
//
// Example - Returning success:
//   Result<int> GetAge() {
//       return 25;  // Just return the value directly
//   }
//
// Example - Returning an error:
//   Result<int> GetAge() {
//       return std::unexpected(Status::NotFound("age not set"));
//   }
//
// Example - Using a Result:
//   auto result = GetAge();
//   if (result) {  // or: if (result.has_value())
//       std::cout << "Age: " << *result << std::endl;  // or: result.value()
//   } else {
//       std::cout << "Error: " << result.error().message() << std::endl;
//   }

template <typename T>
using Result = std::expected<T, Status>;

// ==============================================================================
// Helper Function for Creating Error Results
// ==============================================================================
//
// Since std::unexpected requires wrapping the error, we provide helper 
// functions to make error creation more readable.
//
// Instead of:  return std::unexpected(Status::NotFound("key missing"));
// You can use: return Err(Status::NotFound("key missing"));
//
// Or even shorter factory functions below.

template <typename T>
[[nodiscard]] std::unexpected<Status> Err(Status s) {
    return std::unexpected(std::move(s));
}

// Convenience: Create unexpected results directly from error types
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

// ==============================================================================
// Macros for Error Propagation
// ==============================================================================
//
// These macros reduce boilerplate when checking and propagating errors.
// They're similar to Rust's ? operator.
//
// Usage:
//   Status DoWork() {
//       KVSTORE_RETURN_NOT_OK(Step1());  // Returns early if Step1 fails
//       KVSTORE_RETURN_NOT_OK(Step2());
//       return Status::Ok();
//   }

// Return early if status is not ok
#define KVSTORE_RETURN_NOT_OK(s) \
    do { \
        ::kvstore::Status _status = (s); \
        if (!_status.ok()) { \
            return _status; \
        } \
    } while (false)

// Return early if std::expected result is not ok
// Updated for std::expected: use .has_value() and .error()
#define KVSTORE_RETURN_NOT_OK_RESULT(r) \
    do { \
        if (!(r).has_value()) { \
            return (r).error(); \
        } \
    } while (false)

// Assign value from result or return early on error
// Updated for std::expected
// Usage: KVSTORE_ASSIGN_OR_RETURN(auto value, SomeFunction());
#define KVSTORE_ASSIGN_OR_RETURN(lhs, rhs) \
    auto _result_##__LINE__ = (rhs); \
    if (!_result_##__LINE__.has_value()) { \
        return std::unexpected(_result_##__LINE__.error()); \
    } \
    lhs = std::move(_result_##__LINE__.value())

// New macro: Assign or return Status (not unexpected)
// Use this when returning Status instead of Result<T>
#define KVSTORE_ASSIGN_OR_RETURN_STATUS(lhs, rhs) \
    auto _result_##__LINE__ = (rhs); \
    if (!_result_##__LINE__.has_value()) { \
        return _result_##__LINE__.error(); \
    } \
    lhs = std::move(_result_##__LINE__.value())

}  // namespace kvstore
