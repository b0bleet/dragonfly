// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/types/span.h>

#include <atomic>
#include <boost/fiber/mutex.hpp>
#include <string_view>
#include <vector>

#include "facade/facade_types.h"
#include "facade/op_status.h"

namespace dfly {

enum class ListDir : uint8_t { LEFT, RIGHT };

// Dependent on ExpirePeriod representation of the value.
constexpr int64_t kMaxExpireDeadlineSec = (1u << 27) - 1;

using DbIndex = uint16_t;
using ShardId = uint16_t;
using LSN = uint64_t;
using TxId = uint64_t;
using TxClock = uint64_t;

using facade::ArgS;
using facade::CmdArgList;
using facade::CmdArgVec;
using facade::MutableSlice;
using facade::OpResult;

using ArgSlice = absl::Span<const std::string_view>;
using StringVec = std::vector<std::string>;

// keys are RDB_TYPE_xxx constants.
using RdbTypeFreqMap = absl::flat_hash_map<unsigned, size_t>;

constexpr DbIndex kInvalidDbId = DbIndex(-1);
constexpr ShardId kInvalidSid = ShardId(-1);
constexpr DbIndex kMaxDbId = 1024;  // Reasonable starting point.

class CommandId;
class Transaction;
class EngineShard;

struct KeyLockArgs {
  DbIndex db_index;
  ArgSlice args;
  unsigned key_step;
};

// Describes key indices.
struct KeyIndex {
  // if index is non-zero then adds another key index (usually 1).
  // relevant for for commands like ZUNIONSTORE/ZINTERSTORE for destination key.
  unsigned bonus = 0;
  unsigned start;
  unsigned end;   // does not include this index (open limit).
  unsigned step;  // 1 for commands like mget. 2 for commands like mset.

  bool HasSingleKey() const {
    return bonus == 0 && (start + step >= end);
  }

  unsigned num_args() const {
    return end - start + (bonus > 0);
  }
};

struct DbContext {
  DbIndex db_index = 0;
  uint64_t time_now_ms = 0;
};

struct OpArgs {
  EngineShard* shard;
  TxId txid;
  DbContext db_cntx;

  OpArgs() : shard(nullptr), txid(0) {
  }

  OpArgs(EngineShard* s, TxId i, const DbContext& cntx) : shard(s), txid(i), db_cntx(cntx) {
  }
};

struct TieredStats {
  size_t external_reads = 0;
  size_t external_writes = 0;

  size_t storage_capacity = 0;

  // how much was reserved by actively stored items.
  size_t storage_reserved = 0;

  TieredStats& operator+=(const TieredStats&);
};

enum class GlobalState : uint8_t {
  ACTIVE,
  LOADING,
  SAVING,
  SHUTTING_DOWN,
};

enum class TimeUnit : uint8_t { SEC, MSEC };

inline void ToUpper(const MutableSlice* val) {
  for (auto& c : *val) {
    c = absl::ascii_toupper(c);
  }
}

inline void ToLower(const MutableSlice* val) {
  for (auto& c : *val) {
    c = absl::ascii_tolower(c);
  }
}

bool ParseHumanReadableBytes(std::string_view str, int64_t* num_bytes);
bool ParseDouble(std::string_view src, double* value);
const char* ObjTypeName(int type);

const char* RdbTypeName(unsigned type);

// Cached values, updated frequently to represent the correct state of the system.
extern std::atomic_uint64_t used_mem_peak;
extern std::atomic_uint64_t used_mem_current;
extern size_t max_memory_limit;

// malloc memory stats.
int64_t GetMallocCurrentCommitted();

// version 5.11 maps to 511 etc.
// set upon server start.
extern unsigned kernel_version;

const char* GlobalStateName(GlobalState gs);

template <typename RandGen> std::string GetRandomHex(RandGen& gen, size_t len) {
  static_assert(std::is_same<uint64_t, decltype(gen())>::value);
  std::string res(len, '\0');
  size_t indx = 0;

  for (size_t i = 0; i < len / 16; ++i) {  // 2 chars per byte
    absl::AlphaNum an(absl::Hex(gen(), absl::kZeroPad16));

    for (unsigned j = 0; j < 16; ++j) {
      res[indx++] = an.Piece()[j];
    }
  }

  if (indx < res.size()) {
    absl::AlphaNum an(absl::Hex(gen(), absl::kZeroPad16));

    for (unsigned j = 0; indx < res.size(); indx++, j++) {
      res[indx] = an.Piece()[j];
    }
  }

  return res;
}

// AggregateValue is a thread safe utility to store the first
// non-default value.
template <typename T> struct AggregateValue {
  bool operator=(T val) {
    std::lock_guard l{mu_};
    if (current_ == T{} && val != T{}) {
      current_ = val;
    }
    return val != T{};
  }

  T operator*() {
    std::lock_guard l{mu_};
    return current_;
  }

  operator bool() {
    return **this != T{};
  }

 private:
  ::boost::fibers::mutex mu_{};
  T current_{};
};

using AggregateError = AggregateValue<std::error_code>;

using AggregateStatus = AggregateValue<facade::OpStatus>;
static_assert(facade::OpStatus::OK == facade::OpStatus{},
              "Default intitialization should be OK value");

// Re-usable component for signaling cancellation.
// Simple wrapper around atomic flag.
struct Cancellation {
  void Cancel() {
    flag_.store(true, std::memory_order_relaxed);
  }

  bool IsCancelled() const {
    return flag_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic_bool flag_;
};

// Error wrapper, that stores error_code and optional string message.
class GenericError {
 public:
  GenericError() = default;
  GenericError(std::error_code ec) : ec_{ec}, details_{} {
  }
  GenericError(std::error_code ec, std::string details) : ec_{ec}, details_{std::move(details)} {
  }

  std::error_code GetError() const {
    return ec_;
  }

  const std::string& GetDetails() const {
    return details_;
  }

  operator bool() const {
    return bool(ec_);
  }

  // Get string representation of error.
  std::string Format() const;

 private:
  std::error_code ec_;
  std::string details_;
};

using AggregateGenericError = AggregateValue<GenericError>;

// Contest combines Cancellation and AggregateGenericError in one class.
// Allows setting an error_handler to run on errors.
class Context : public Cancellation {
 public:
  // The error handler should return false if this error is ignored.
  using ErrHandler = std::function<bool(const GenericError&)>;

  Context() = default;
  Context(ErrHandler err_handler) : Cancellation{}, err_handler_{std::move(err_handler)} {
  }

  template <typename... T> void Error(T... ts) {
    std::lock_guard lk{mu_};
    if (err_)
      return;

    GenericError new_err{std::forward<T>(ts)...};
    if (!err_handler_ || err_handler_(new_err)) {
      err_ = std::move(new_err);
      Cancel();
    }
  }

 private:
  GenericError err_;
  ErrHandler err_handler_;
  ::boost::fibers::mutex mu_;
};

struct ScanOpts {
  std::string_view pattern;
  size_t limit = 10;
  std::string_view type_filter;
  unsigned bucket_id = UINT_MAX;

  bool Matches(std::string_view val_name) const;
  static OpResult<ScanOpts> TryFrom(CmdArgList args);
};

}  // namespace dfly
