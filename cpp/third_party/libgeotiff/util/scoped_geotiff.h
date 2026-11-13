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
// Shared RAII deleters, ST_TIFF fixtures, FILE* output capture, and GTIF error
// capture helpers for //third_party/libgeotiff/google tests.

#ifndef THIRD_PARTY_LIBGEOTIFF_GOOGLE_UTIL_SCOPED_GEOTIFF_H_
#define THIRD_PARTY_LIBGEOTIFF_GOOGLE_UTIL_SCOPED_GEOTIFF_H_

#include <stdio.h>  // For POSIX open_memstream.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/absl/types/span.h"
#include "third_party/libgeotiff/geo_normalize.h"
#include "third_party/libgeotiff/geo_simpletags.h"
#include "third_party/libgeotiff/geo_tiffp.h"
#include "third_party/libgeotiff/geotiff.h"
#include "third_party/libgeotiff/libxtiff/xtiffio.h"  // IWYU pragma: keep

namespace libgeotiff {

struct SimpleTagsDeleter {
  void operator()(ST_TIFF* st) const { ST_Destroy(st); }
};

using ScopedSimpleTags = std::unique_ptr<ST_TIFF, SimpleTagsDeleter>;

struct GtifDeleter {
  void operator()(GTIF* gtif) const { GTIFFree(gtif); }
};

using ScopedGtif = std::unique_ptr<GTIF, GtifDeleter>;

struct DefnDeleter {
  void operator()(GTIFDefn* defn) const { GTIFFreeDefn(defn); }
};

using ScopedDefn = std::unique_ptr<GTIFDefn, DefnDeleter>;

struct GtifMemDeleter {
  void operator()(char* mem) const { GTIFFreeMemory(mem); }
};

using ScopedGtifMem = std::unique_ptr<char, GtifMemDeleter>;

struct RawGtifMemDeleter {
  void operator()(void* ptr) const { _GTIFFree(ptr); }
};

using ScopedRawGtifMem = std::unique_ptr<void, RawGtifMemDeleter>;

struct FileCloser {
  void operator()(FILE* fp) const { std::fclose(fp); }
};

using ScopedFile = std::unique_ptr<FILE, FileCloser>;

struct SimpleTagsGtif {
  ScopedSimpleTags st;
  // Declared after `st` so GTIFFree always runs before ST_Destroy.
  ScopedGtif gtif;
};

inline SimpleTagsGtif MakeSimpleTagsGtif() {
  ScopedSimpleTags st(ST_Create());
  ScopedGtif gtif(st != nullptr ? GTIFNewSimpleTags(st.get()) : nullptr);
  return SimpleTagsGtif{.st = std::move(st), .gtif = std::move(gtif)};
}

template <typename Range>
int SetSimpleTag(ST_TIFF* st, int tag, int st_type, const Range& values) {
  const auto span_vals = absl::MakeConstSpan(values);
  using Value = typename decltype(span_vals)::value_type;
  // ST_SetKey takes non-const void* even though it only reads via memcpy.
  return ST_SetKey(st, tag, static_cast<int>(span_vals.size()), st_type,
                   const_cast<Value*>(span_vals.data()));
}

inline int SetKeyDirectory(ST_TIFF* st, absl::Span<const uint16_t> dir) {
  return SetSimpleTag(st, TIFFTAG_GEOKEYDIRECTORY, STT_SHORT, dir);
}

struct ErrorRecord {
  int level = 0;
  std::string message;
};

struct ErrorCapture {
  GTIF* last_gtif = nullptr;
  std::vector<ErrorRecord> records;
};

inline void CaptureGtifError(GTIF* gtif, int level, const char* msg, ...) {
  auto* capture = static_cast<ErrorCapture*>(GTIFGetUserData(gtif));
  if (capture == nullptr) {
    return;
  }
  capture->last_gtif = gtif;
  std::string formatted;
  if (msg != nullptr) {
    va_list ap;
    va_start(ap, msg);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    const int needed = std::vsnprintf(nullptr, 0, msg, ap_copy);
    va_end(ap_copy);
    if (needed > 0) {
      formatted.resize(static_cast<size_t>(needed) + 1);
      std::vsnprintf(formatted.data(), formatted.size(), msg, ap);
      formatted.resize(static_cast<size_t>(needed));
    }
    va_end(ap);
  }
  capture->records.push_back(ErrorRecord{
      .level = level,
      .message = std::move(formatted),
  });
}

inline ScopedGtif MakeCapturingSimpleTagsGtif(ST_TIFF* st,
                                              ErrorCapture* capture) {
  TIFFMethod methods{};
  GTIFSetSimpleTagsMethods(&methods);
  return ScopedGtif(GTIFNewWithMethodsEx(
      st, &methods, capture != nullptr ? CaptureGtifError : nullptr, capture));
}

namespace internal {

struct CFreeDeleter {
  void operator()(void* ptr) const { std::free(ptr); }
};

}  // namespace internal

template <typename Fn>
std::string CaptureFileOutput(Fn&& fn) {
  char* raw_buf = nullptr;
  size_t size = 0;
  ScopedFile fp(open_memstream(&raw_buf, &size));
  std::unique_ptr<char, internal::CFreeDeleter> buf_guard;
  if (fp == nullptr) {
    return {};
  }
  fn(fp.get());
  // Closing `fp` flushes and populates `raw_buf` and `size`.
  fp.reset();
  buf_guard.reset(raw_buf);
  if (buf_guard == nullptr) {
    return {};
  }
  return std::string(buf_guard.get(), size);
}

}  // namespace libgeotiff

#endif  // THIRD_PARTY_LIBGEOTIFF_GOOGLE_UTIL_SCOPED_GEOTIFF_H_
