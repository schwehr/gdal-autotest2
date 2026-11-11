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
// Thread-safe in-memory PROJ_FILE_API virtual filesystem and pure-C++ synthetic
// binary grid builders (GTX, CTable2, NTv2, Classic GeoTIFF) so unit tests can
// exercise PROJ file and grid readers without checked-in binary testdata or
// real disk I/O.

#ifndef THIRD_PARTY_PROJ4_GOOGLE_UTIL_IN_MEMORY_FILE_API_H_
#define THIRD_PARTY_PROJ4_GOOGLE_UTIL_IN_MEMORY_FILE_API_H_

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>  // NOLINT(build/c++20)
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "third_party/proj4/proj/src/proj.h"

namespace proj {

namespace internal {

inline void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

inline void AppendLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

inline void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline void AppendBe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

inline void AppendLeFloat32(std::vector<uint8_t>& out, float v) {
  AppendLe32(out, std::bit_cast<uint32_t>(v));
}

inline void AppendBeFloat32(std::vector<uint8_t>& out, float v) {
  AppendBe32(out, std::bit_cast<uint32_t>(v));
}

inline void AppendLeFloat64(std::vector<uint8_t>& out, double v) {
  AppendLe64(out, std::bit_cast<uint64_t>(v));
}

inline void AppendBeFloat64(std::vector<uint8_t>& out, double v) {
  AppendBe64(out, std::bit_cast<uint64_t>(v));
}

inline void AppendFixedString(std::vector<uint8_t>& out, std::string_view str,
                              size_t width, char pad) {
  const size_t n = std::min(width, str.size());
  for (size_t i = 0; i < n; ++i) {
    out.push_back(static_cast<uint8_t>(str[i]));
  }
  for (size_t i = n; i < width; ++i) {
    out.push_back(static_cast<uint8_t>(pad));
  }
}

}  // namespace internal

// Thread-safe in-memory filesystem implementing PROJ's PROJ_FILE_API.
// Note: prefer virtual paths starting with '/' (e.g., "/mem/grid.gtx") so
// PROJ's FileManager::open_resource_file() bypasses PROJ_DATA search paths
// and google3's file finder and routes directly to open_cbk.
class InMemoryFileSystem {
 public:
  InMemoryFileSystem() = default;
  InMemoryFileSystem(const InMemoryFileSystem&) = delete;
  InMemoryFileSystem& operator=(const InMemoryFileSystem&) = delete;

  void AddFile(std::string path, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(mu_);  // NOLINT(build/c++11)
    files_[std::move(path)] = std::move(data);
  }

  void AddTextFile(std::string path, std::string_view text) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    AddFile(std::move(path), std::move(bytes));
  }

  std::optional<std::vector<uint8_t>> GetFile(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mu_);  // NOLINT(build/c++11)
    const auto it = files_.find(path);
    if (it == files_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  bool Contains(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mu_);  // NOLINT(build/c++11)
    return files_.find(path) != files_.end();
  }

  bool ContainsDir(std::string_view path) const {
    std::lock_guard<std::mutex> lock(mu_);  // NOLINT(build/c++11)
    return dirs_.find(path) != dirs_.end();
  }

  bool Install(PJ_CONTEXT* ctx) {
    return proj_context_set_fileapi(ctx, &kApi, this) != 0;
  }

 private:
  struct OpenHandle {
    InMemoryFileSystem* fs;
    std::string path;
    PROJ_OPEN_ACCESS access;
    size_t pos = 0;
  };

  static PROJ_FILE_HANDLE* OpenCbk(PJ_CONTEXT*, const char* filename,
                                   PROJ_OPEN_ACCESS access, void* user_data) {
    if (filename == nullptr || user_data == nullptr) {
      return nullptr;
    }
    auto* fs = static_cast<InMemoryFileSystem*>(user_data);
    std::lock_guard<std::mutex> lock(fs->mu_);  // NOLINT(build/c++11)
    auto it = fs->files_.find(filename);
    if (access == PROJ_OPEN_ACCESS_READ_ONLY) {
      if (it == fs->files_.end()) {
        return nullptr;
      }
    } else if (access == PROJ_OPEN_ACCESS_READ_UPDATE) {
      if (it == fs->files_.end()) {
        fs->files_[filename] = {};
      }
    } else if (access == PROJ_OPEN_ACCESS_CREATE) {
      fs->files_[filename].clear();
    } else {
      return nullptr;
    }
    return reinterpret_cast<PROJ_FILE_HANDLE*>(
        new OpenHandle{.fs = fs, .path = filename, .access = access, .pos = 0});
  }

  static size_t ReadCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file, void* buffer,
                        size_t size_bytes, void*) {
    if (file == nullptr || buffer == nullptr || size_bytes == 0) {
      return 0;
    }
    auto* handle = reinterpret_cast<OpenHandle*>(file);
    std::lock_guard<std::mutex> lock(handle->fs->mu_);  // NOLINT(build/c++11)
    const auto it = handle->fs->files_.find(handle->path);
    if (it == handle->fs->files_.end() || handle->pos >= it->second.size()) {
      return 0;
    }
    const size_t n = std::min(size_bytes, it->second.size() - handle->pos);
    std::memcpy(buffer, it->second.data() + handle->pos, n);
    handle->pos += n;
    return n;
  }

  static size_t WriteCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file,
                         const void* buffer, size_t size_bytes, void*) {
    if (file == nullptr || (buffer == nullptr && size_bytes > 0)) {
      return 0;
    }
    auto* handle = reinterpret_cast<OpenHandle*>(file);
    if (handle->access == PROJ_OPEN_ACCESS_READ_ONLY) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(handle->fs->mu_);  // NOLINT(build/c++11)
    const auto it = handle->fs->files_.find(handle->path);
    if (it == handle->fs->files_.end()) {
      return 0;
    }
    if (size_bytes == 0) {
      return 0;
    }
    const size_t end_pos = handle->pos + size_bytes;
    if (end_pos > it->second.size()) {
      it->second.resize(end_pos, 0);
    }
    std::memcpy(it->second.data() + handle->pos, buffer, size_bytes);
    handle->pos = end_pos;
    return size_bytes;
  }

  // NOLINTNEXTLINE(runtime/int)
  static int SeekCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file, long long offset,
                     int whence, void*) {
    if (file == nullptr) {
      return 0;
    }
    auto* handle = reinterpret_cast<OpenHandle*>(file);
    std::lock_guard<std::mutex> lock(handle->fs->mu_);  // NOLINT(build/c++11)
    const auto it = handle->fs->files_.find(handle->path);
    if (it == handle->fs->files_.end()) {
      return 0;
    }
    long long base = 0;  // NOLINT(runtime/int)
    if (whence == SEEK_SET) {
      base = 0;
    } else if (whence == SEEK_CUR) {
      base = static_cast<long long>(handle->pos);
    } else if (whence == SEEK_END) {
      base = static_cast<long long>(it->second.size());
    } else {
      return 0;
    }
    const long long next_pos = base + offset;  // NOLINT(runtime/int)
    if (next_pos < 0) {
      return 0;
    }
    handle->pos = static_cast<size_t>(next_pos);
    return 1;
  }

  // NOLINTNEXTLINE(runtime/int)
  static unsigned long long TellCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file,
                                    void*) {
    if (file == nullptr) {
      return 0;
    }
    return reinterpret_cast<OpenHandle*>(file)->pos;
  }

  static void CloseCbk(PJ_CONTEXT*, PROJ_FILE_HANDLE* file, void*) {
    delete reinterpret_cast<OpenHandle*>(file);
  }

  static int ExistsCbk(PJ_CONTEXT*, const char* filename, void* user_data) {
    if (filename == nullptr || user_data == nullptr) {
      return 0;
    }
    auto* fs = static_cast<InMemoryFileSystem*>(user_data);
    std::lock_guard<std::mutex> lock(fs->mu_);  // NOLINT(build/c++11)
    return (fs->files_.find(filename) != fs->files_.end() ||
            fs->dirs_.find(filename) != fs->dirs_.end())
               ? 1
               : 0;
  }

  static int MkdirCbk(PJ_CONTEXT*, const char* filename, void* user_data) {
    if (filename == nullptr || user_data == nullptr) {
      return 0;
    }
    auto* fs = static_cast<InMemoryFileSystem*>(user_data);
    std::lock_guard<std::mutex> lock(fs->mu_);  // NOLINT(build/c++11)
    fs->dirs_.insert(filename);
    return 1;
  }

  static int UnlinkCbk(PJ_CONTEXT*, const char* filename, void* user_data) {
    if (filename == nullptr || user_data == nullptr) {
      return 0;
    }
    auto* fs = static_cast<InMemoryFileSystem*>(user_data);
    std::lock_guard<std::mutex> lock(fs->mu_);  // NOLINT(build/c++11)
    return fs->files_.erase(filename) > 0 ? 1 : 0;
  }

  static int RenameCbk(PJ_CONTEXT*, const char* old_path, const char* new_path,
                       void* user_data) {
    if (old_path == nullptr || new_path == nullptr || user_data == nullptr) {
      return 0;
    }
    auto* fs = static_cast<InMemoryFileSystem*>(user_data);
    std::lock_guard<std::mutex> lock(fs->mu_);  // NOLINT(build/c++11)
    const auto it = fs->files_.find(old_path);
    if (it == fs->files_.end()) {
      return 0;
    }
    if (std::strcmp(old_path, new_path) == 0) {
      return 1;
    }
    fs->files_[new_path] = std::move(it->second);
    fs->files_.erase(it);
    return 1;
  }

  static constexpr PROJ_FILE_API kApi = {
      /*version=*/1, &OpenCbk,   &ReadCbk,   &WriteCbk,
      &SeekCbk,      &TellCbk,   &CloseCbk,  &ExistsCbk,
      &MkdirCbk,     &UnlinkCbk, &RenameCbk,
  };

  mutable std::mutex mu_;  // NOLINT(build/c++11)
  std::map<std::string, std::vector<uint8_t>, std::less<>> files_;
  std::set<std::string, std::less<>> dirs_;
};

// Synthesizes a big-endian GTX vertical shift grid (filename must end in .gtx
// or .GTX for PROJ's VerticalShiftGridSet::open() to detect it).
// `shifts_m` has size rows * cols, ordered south-to-north, west-to-east.
inline std::vector<uint8_t> BuildGtxGrid(double west_deg, double south_deg,
                                         double res_x_deg, double res_y_deg,
                                         int rows, int cols,
                                         std::span<const float> shifts_m) {
  std::vector<uint8_t> out;
  if (rows <= 0 || cols <= 0 ||
      shifts_m.size() != static_cast<size_t>(rows) * cols) {
    return out;
  }
  out.reserve(40 + shifts_m.size() * sizeof(float));
  internal::AppendBeFloat64(out, south_deg);
  internal::AppendBeFloat64(out, west_deg);
  internal::AppendBeFloat64(out, res_y_deg);
  internal::AppendBeFloat64(out, res_x_deg);
  internal::AppendBe32(out, static_cast<uint32_t>(rows));
  internal::AppendBe32(out, static_cast<uint32_t>(cols));
  for (const float v : shifts_m) {
    internal::AppendBeFloat32(out, v);
  }
  return out;
}

// Synthesizes a little-endian CTable2 horizontal shift grid.
// `lon_lat_shifts_rad` contains 2 * cols * rows floats ordered south-to-north,
// west-to-east, with each node stored as (lon_shift_west_positive_rad,
// lat_shift_rad).
inline std::vector<uint8_t> BuildCTable2Grid(
    double west_rad, double south_rad, double res_x_rad, double res_y_rad,
    int cols, int rows, std::span<const float> lon_lat_shifts_rad) {
  std::vector<uint8_t> out;
  if (cols <= 0 || rows <= 0 ||
      lon_lat_shifts_rad.size() != static_cast<size_t>(cols) * rows * 2) {
    return out;
  }
  out.reserve(160 + lon_lat_shifts_rad.size() * sizeof(float));
  internal::AppendFixedString(out, "CTABLE V2.0", 96, '\0');
  internal::AppendLeFloat64(out, west_rad);
  internal::AppendLeFloat64(out, south_rad);
  internal::AppendLeFloat64(out, res_x_rad);
  internal::AppendLeFloat64(out, res_y_rad);
  internal::AppendLe32(out, static_cast<uint32_t>(cols));
  internal::AppendLe32(out, static_cast<uint32_t>(rows));
  out.resize(160, 0);
  for (const float v : lon_lat_shifts_rad) {
    internal::AppendLeFloat32(out, v);
  }
  return out;
}

// Synthesizes a little-endian NTv2 single-subfile grid (GS_TYPE=SECONDS).
// Coordinates use standard east-positive degrees; NTv2's internal west-positive
// arcsecond fields (W_LONG / E_LONG) are derived automatically.
// `records` contains 4 * cols * rows floats in NTv2 file order (south-to-north
// rows, east-to-west within each row): (lat_shift_arcsec,
// lon_shift_west_pos_arcsec, lat_accuracy, lon_accuracy).
inline std::vector<uint8_t> BuildNtv2Grid(
    double west_deg, double south_deg, double res_x_deg, double res_y_deg,
    int cols, int rows, std::span<const float> records,
    std::string_view sub_name = "TESTGRID",
    std::string_view parent_name = "NONE    ") {
  std::vector<uint8_t> out;
  if (cols <= 0 || rows <= 0 ||
      records.size() != static_cast<size_t>(cols) * rows * 4) {
    return out;
  }
  out.reserve(352 + records.size() * sizeof(float));

  // Overview header (11 * 16 = 176 bytes).
  internal::AppendFixedString(out, "NUM_OREC", 8, ' ');
  internal::AppendLe32(out, 11);
  internal::AppendLe32(out, 0);
  internal::AppendFixedString(out, "NUM_SREC", 8, ' ');
  internal::AppendLe32(out, 11);
  internal::AppendLe32(out, 0);
  internal::AppendFixedString(out, "NUM_FILE", 8, ' ');
  internal::AppendLe32(out, 1);
  internal::AppendLe32(out, 0);
  internal::AppendFixedString(out, "GS_TYPE ", 8, ' ');
  internal::AppendFixedString(out, "SECONDS ", 8, ' ');
  internal::AppendFixedString(out, "VERSION ", 8, ' ');
  internal::AppendFixedString(out, "NTv2.0  ", 8, ' ');
  internal::AppendFixedString(out, "SYSTEM_F", 8, ' ');
  internal::AppendFixedString(out, "NAD27   ", 8, ' ');
  internal::AppendFixedString(out, "SYSTEM_T", 8, ' ');
  internal::AppendFixedString(out, "NAD83   ", 8, ' ');
  internal::AppendFixedString(out, "MAJOR_F ", 8, ' ');
  internal::AppendLeFloat64(out, 6378206.4);
  internal::AppendFixedString(out, "MINOR_F ", 8, ' ');
  internal::AppendLeFloat64(out, 6356583.8);
  internal::AppendFixedString(out, "MAJOR_T ", 8, ' ');
  internal::AppendLeFloat64(out, 6378137.0);
  internal::AppendFixedString(out, "MINOR_T ", 8, ' ');
  internal::AppendLeFloat64(out, 6356752.31414);

  // Subfile header (11 * 16 = 176 bytes).
  const double north_deg = south_deg + (rows - 1) * res_y_deg;
  const double east_deg = west_deg + (cols - 1) * res_x_deg;
  internal::AppendFixedString(out, "SUB_NAME", 8, ' ');
  internal::AppendFixedString(out, sub_name, 8, ' ');
  internal::AppendFixedString(out, "PARENT  ", 8, ' ');
  internal::AppendFixedString(out, parent_name, 8, ' ');
  internal::AppendFixedString(out, "CREATED ", 8, ' ');
  internal::AppendFixedString(out, "20260101", 8, ' ');
  internal::AppendFixedString(out, "UPDATED ", 8, ' ');
  internal::AppendFixedString(out, "20260101", 8, ' ');
  internal::AppendFixedString(out, "S_LAT   ", 8, ' ');
  internal::AppendLeFloat64(out, south_deg * 3600.0);
  internal::AppendFixedString(out, "N_LAT   ", 8, ' ');
  internal::AppendLeFloat64(out, north_deg * 3600.0);
  internal::AppendFixedString(out, "E_LONG  ", 8, ' ');
  internal::AppendLeFloat64(out, -east_deg * 3600.0);
  internal::AppendFixedString(out, "W_LONG  ", 8, ' ');
  internal::AppendLeFloat64(out, -west_deg * 3600.0);
  internal::AppendFixedString(out, "LAT_INC ", 8, ' ');
  internal::AppendLeFloat64(out, res_y_deg * 3600.0);
  internal::AppendFixedString(out, "LONG_INC", 8, ' ');
  internal::AppendLeFloat64(out, res_x_deg * 3600.0);
  internal::AppendFixedString(out, "GS_COUNT", 8, ' ');
  internal::AppendLe32(out, static_cast<uint32_t>(cols * rows));
  internal::AppendLe32(out, 0);

  for (const float v : records) {
    internal::AppendLeFloat32(out, v);
  }
  return out;
}

// Synthesizes a Classic little-endian uncompressed Float32 GeoTIFF with
// PixelIsPoint georeferencing (north-to-south top-down rows, interleaved
// samples) without requiring a dependency on //third_party/tiff.
inline std::vector<uint8_t> BuildMinimalGeoTiff(
    int width, int height, int samples_per_pixel, double west, double north,
    double res_x, double res_y, std::span<const float> pixels,
    bool is_geographic = true, std::string_view gdal_metadata_xml = {},
    std::optional<double> nodata = std::nullopt) {
  std::vector<uint8_t> out;
  if (width <= 0 || height <= 0 || samples_per_pixel <= 0 ||
      pixels.size() !=
          static_cast<size_t>(width) * height * samples_per_pixel) {
    return out;
  }

  struct TiffTag {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    std::vector<uint8_t> payload;
  };
  std::vector<TiffTag> tags;

  auto add_long1 = [&](uint16_t tag, uint32_t value) {
    std::vector<uint8_t> p;
    internal::AppendLe32(p, value);
    tags.push_back({tag, /*TIFF_LONG=*/4, 1, std::move(p)});
  };
  auto add_short_vec = [&](uint16_t tag, std::span<const uint16_t> values) {
    std::vector<uint8_t> p;
    for (const uint16_t v : values) {
      internal::AppendLe16(p, v);
    }
    tags.push_back({tag, /*TIFF_SHORT=*/3, static_cast<uint32_t>(values.size()),
                    std::move(p)});
  };
  auto add_double_vec = [&](uint16_t tag, std::span<const double> values) {
    std::vector<uint8_t> p;
    for (const double v : values) {
      internal::AppendLeFloat64(p, v);
    }
    tags.push_back({tag, /*TIFF_DOUBLE=*/12,
                    static_cast<uint32_t>(values.size()), std::move(p)});
  };
  auto add_ascii = [&](uint16_t tag, std::string_view value) {
    std::vector<uint8_t> p(value.begin(), value.end());
    p.push_back('\0');
    tags.push_back(
        {tag, /*TIFF_ASCII=*/2, static_cast<uint32_t>(p.size()), std::move(p)});
  };

  const uint16_t spp = static_cast<uint16_t>(samples_per_pixel);
  const std::vector<uint16_t> bits_per_sample(spp, 32);
  const std::vector<uint16_t> sample_format(spp, /*SAMPLEFORMAT_IEEEFP=*/3);
  const uint16_t one_u16 = 1;
  const uint32_t pixel_bytes =
      static_cast<uint32_t>(pixels.size() * sizeof(float));

  // Tag order must be strictly ascending for libtiff.
  add_long1(256, static_cast<uint32_t>(width));   // ImageWidth
  add_long1(257, static_cast<uint32_t>(height));  // ImageLength
  add_short_vec(258, bits_per_sample);            // BitsPerSample
  add_short_vec(259, {&one_u16, 1});              // Compression = NONE
  add_short_vec(262, {&one_u16, 1});              // Photometric = MINISBLACK
  add_long1(273, 0);              // StripOffsets (patched below)
  add_short_vec(277, {&spp, 1});  // SamplesPerPixel
  add_long1(278, static_cast<uint32_t>(height));  // RowsPerStrip
  add_long1(279, pixel_bytes);                    // StripByteCounts
  add_short_vec(284, {&one_u16, 1});  // PlanarConfiguration = CONTIG
  add_short_vec(339, sample_format);  // SampleFormat = IEEEFP

  const double pixel_scale[3] = {res_x, res_y, 0.0};
  const double tie_points[6] = {0.0, 0.0, 0.0, west, north, 0.0};
  const uint16_t model_type = is_geographic ? 2 : 1;
  const uint16_t geo_keys[12] = {
      1,    1, 0, 2,           // GeoTIFF v1.1 rev 0, 2 keys
      1024, 0, 1, model_type,  // GTModelTypeGeoKey
      1025, 0, 1, 2,           // GTRasterTypeGeoKey = RasterPixelIsPoint
  };
  add_double_vec(33550, pixel_scale);  // GeoPixelScale
  add_double_vec(33922, tie_points);   // GeoTiePoints
  add_short_vec(34735, geo_keys);      // GeoKeyDirectory

  if (!gdal_metadata_xml.empty()) {
    add_ascii(42112, gdal_metadata_xml);
  }
  if (nodata.has_value()) {
    std::ostringstream oss;
    oss << *nodata;
    add_ascii(42113, oss.str());
  }

  // Layout:
  //   [0..8): Classic TIFF header ("II", 42, ifd_offset=8)
  //   [8..8 + 2 + 12*N + 4): IFD
  //   out-of-line tag payloads
  //   strip pixel data (patched into StripOffsets)
  const uint32_t ifd_offset = 8;
  const uint32_t ifd_size = 2 + static_cast<uint32_t>(tags.size()) * 12 + 4;
  uint32_t next_data_offset = ifd_offset + ifd_size;

  std::vector<uint32_t> tag_offsets(tags.size(), 0);
  size_t strip_offsets_idx = 0;
  for (size_t i = 0; i < tags.size(); ++i) {
    if (tags[i].tag == 273) {
      strip_offsets_idx = i;
    }
    if (tags[i].payload.size() > 4) {
      tag_offsets[i] = next_data_offset;
      next_data_offset += static_cast<uint32_t>(tags[i].payload.size());
      if ((next_data_offset & 1U) != 0) {
        ++next_data_offset;  // TIFF value offsets must be word-aligned.
      }
    }
  }
  const uint32_t strip_data_offset = next_data_offset;
  tags[strip_offsets_idx].payload.clear();
  internal::AppendLe32(tags[strip_offsets_idx].payload, strip_data_offset);

  out.reserve(strip_data_offset + pixel_bytes);
  out.push_back('I');
  out.push_back('I');
  internal::AppendLe16(out, 42);
  internal::AppendLe32(out, ifd_offset);

  internal::AppendLe16(out, static_cast<uint16_t>(tags.size()));
  for (size_t i = 0; i < tags.size(); ++i) {
    internal::AppendLe16(out, tags[i].tag);
    internal::AppendLe16(out, tags[i].type);
    internal::AppendLe32(out, tags[i].count);
    if (tags[i].payload.size() <= 4) {
      for (uint8_t b : tags[i].payload) {
        out.push_back(b);
      }
      for (size_t k = tags[i].payload.size(); k < 4; ++k) {
        out.push_back(0);
      }
    } else {
      internal::AppendLe32(out, tag_offsets[i]);
    }
  }
  internal::AppendLe32(out, 0);  // next IFD offset = 0

  for (size_t i = 0; i < tags.size(); ++i) {
    if (tags[i].payload.size() > 4) {
      out.insert(out.end(), tags[i].payload.begin(), tags[i].payload.end());
      if ((out.size() & 1U) != 0) {
        out.push_back(0);
      }
    }
  }
  for (const float px : pixels) {
    internal::AppendLeFloat32(out, px);
  }
  return out;
}

}  // namespace proj

#endif  // THIRD_PARTY_PROJ4_GOOGLE_UTIL_IN_MEMORY_FILE_API_H_
