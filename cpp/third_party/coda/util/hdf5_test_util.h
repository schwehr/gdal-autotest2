// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// RAII wrappers for HDF5 C handles so tests can synthesize .h5 files cleanly
// using versioned HDF5 1.8+ APIs (since config.h defines H5_USE_16_API).

#ifndef THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_HDF5_TEST_UTIL_H_
#define THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_HDF5_TEST_UTIL_H_

#include <utility>

// clang-format off
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"  // IWYU pragma: export
#include "hdf5.h"  // IWYU pragma: export
// clang-format on

namespace stcorp_coda {

template <herr_t (*CloseFn)(hid_t)>
class ScopedH5Handle {
 public:
  ScopedH5Handle() = default;
  explicit ScopedH5Handle(hid_t id) : id_(id) {}

  ~ScopedH5Handle() { Reset(); }

  ScopedH5Handle(const ScopedH5Handle&) = delete;
  ScopedH5Handle& operator=(const ScopedH5Handle&) = delete;

  ScopedH5Handle(ScopedH5Handle&& other) noexcept
      : id_(std::exchange(other.id_, H5I_INVALID_HID)) {}

  ScopedH5Handle& operator=(ScopedH5Handle&& other) noexcept {
    if (this != &other) {
      Reset();
      id_ = std::exchange(other.id_, H5I_INVALID_HID);
    }
    return *this;
  }

  void Reset(hid_t id = H5I_INVALID_HID) {
    if (id_ >= 0) {
      CloseFn(id_);
    }
    id_ = id;
  }

  hid_t get() const { return id_; }
  bool valid() const { return id_ >= 0; }
  explicit operator bool() const { return valid(); }

 private:
  hid_t id_ = H5I_INVALID_HID;
};

using ScopedH5File = ScopedH5Handle<H5Fclose>;
using ScopedH5Group = ScopedH5Handle<H5Gclose>;
using ScopedH5Dataset = ScopedH5Handle<H5Dclose>;
using ScopedH5Space = ScopedH5Handle<H5Sclose>;
using ScopedH5Type = ScopedH5Handle<H5Tclose>;
using ScopedH5Attr = ScopedH5Handle<H5Aclose>;
using ScopedH5Plist = ScopedH5Handle<H5Pclose>;

}  // namespace stcorp_coda

#endif  // THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_HDF5_TEST_UTIL_H_
