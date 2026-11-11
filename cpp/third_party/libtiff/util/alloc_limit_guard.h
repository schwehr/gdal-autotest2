// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Scoped allocation-limit guards over internal TIFF handle fields (tiffiop.h)
// for deterministic OOM injection tests.

#ifndef THIRD_PARTY_TIFF_GOOGLE_UTIL_ALLOC_LIMIT_GUARD_H_
#define THIRD_PARTY_TIFF_GOOGLE_UTIL_ALLOC_LIMIT_GUARD_H_

#include <limits>

#include "testing/base/public/gunit.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {

// Temporarily overrides tif->tif_max_single_mem_alloc and restores it on exit.
class ScopedSingleMemAllocLimit {
 public:
  ScopedSingleMemAllocLimit(TIFF* tif, tmsize_t limit)
      : tif_(tif),
        prev_limit_(tif != nullptr ? tif->tif_max_single_mem_alloc : 0) {
    if (tif_ != nullptr) {
      tif_->tif_max_single_mem_alloc = limit;
    }
  }

  ~ScopedSingleMemAllocLimit() {
    if (tif_ != nullptr) {
      tif_->tif_max_single_mem_alloc = prev_limit_;
    }
  }

  ScopedSingleMemAllocLimit(const ScopedSingleMemAllocLimit&) = delete;
  ScopedSingleMemAllocLimit& operator=(const ScopedSingleMemAllocLimit&) =
      delete;

 private:
  TIFF* tif_;
  tmsize_t prev_limit_;
};

// Temporarily narrows tif->tif_max_cumulated_mem_alloc to
// (tif_cur_cumulated_mem_alloc + remaining_budget) and restores it on exit.
//
// IMPORTANT: libtiff prepends a size header inside _TIFFmallocExt only when
// tif_max_cumulated_mem_alloc > 0. Never transition max_cumulated_mem_alloc
// between 0 and >0 while allocations are live; open tif with a large limit
// (e.g., TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts, 1 << 24)) first.
class ScopedCumulatedMemAllocRemaining {
 public:
  ScopedCumulatedMemAllocRemaining(TIFF* tif, tmsize_t remaining_budget)
      : tif_(tif) {
    if (tif_ == nullptr || tif_->tif_max_cumulated_mem_alloc <= 0) {
      ADD_FAILURE() << "TIFF handle must be opened with non-zero "
                       "max_cumulated_mem_alloc before tightening budget";
      tif_ = nullptr;
      return;
    }
    prev_limit_ = tif_->tif_max_cumulated_mem_alloc;
    const tmsize_t cur = tif_->tif_cur_cumulated_mem_alloc;
    tmsize_t next_limit = prev_limit_;
    if (remaining_budget <= 0) {
      next_limit = cur > 0 ? cur : 1;
    } else if (cur <= std::numeric_limits<tmsize_t>::max() - remaining_budget) {
      next_limit = cur + remaining_budget;
    } else {
      next_limit = std::numeric_limits<tmsize_t>::max();
    }
    if (next_limit <= 0) {
      next_limit = 1;
    }
    tif_->tif_max_cumulated_mem_alloc = next_limit;
  }

  ~ScopedCumulatedMemAllocRemaining() {
    if (tif_ != nullptr) {
      tif_->tif_max_cumulated_mem_alloc = prev_limit_;
    }
  }

  ScopedCumulatedMemAllocRemaining(const ScopedCumulatedMemAllocRemaining&) =
      delete;
  ScopedCumulatedMemAllocRemaining& operator=(
      const ScopedCumulatedMemAllocRemaining&) = delete;

 private:
  TIFF* tif_ = nullptr;
  tmsize_t prev_limit_ = 0;
};

}  // namespace libtiff

#endif  // THIRD_PARTY_TIFF_GOOGLE_UTIL_ALLOC_LIMIT_GUARD_H_
