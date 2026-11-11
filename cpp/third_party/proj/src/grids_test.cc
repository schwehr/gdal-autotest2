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
// Unit tests for PROJ grid management and grid shift formats
// (proj/src/grids.cpp). Note: in google3 default non-GCE builds, networking is
// disabled and non-skipped gridshift files are embedded, while ntv2_0.gsb and
// ca_nrc_ntv2_0.tif are intentionally skipped by google3_proj4_embed_fileapi.
// Synthetic binary grids in InMemoryFileSystem are used so tests remain
// self-contained without checked-in testdata or third_party/tiff headers.
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/grids.cpp

#include "third_party/proj4/proj/src/grids.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>  // NOLINT(build/c++20)
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/in_memory_file_api.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/google/util/temp_file.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kArcsecToRad = kDegToRad / 3600.0;

constexpr double DegToRad(double deg) { return deg * kDegToRad; }
constexpr double RadToDeg(double rad) { return rad / kDegToRad; }

// Endian-aware binary helpers for crafting synthetic grids in memory.
void AppendU16(std::vector<uint8_t>& out, uint16_t v, bool big_endian) {
  if (big_endian) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  } else {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  }
}

void AppendU32(std::vector<uint8_t>& out, uint32_t v, bool big_endian) {
  if (big_endian) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
  } else {
    for (int shift = 0; shift <= 24; shift += 8) {
      out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
  }
}

void AppendU64(std::vector<uint8_t>& out, uint64_t v, bool big_endian) {
  if (big_endian) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
  } else {
    for (int shift = 0; shift <= 56; shift += 8) {
      out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
  }
}

void AppendF32(std::vector<uint8_t>& out, float v, bool big_endian) {
  AppendU32(out, std::bit_cast<uint32_t>(v), big_endian);
}

void AppendF64(std::vector<uint8_t>& out, double v, bool big_endian) {
  AppendU64(out, std::bit_cast<uint64_t>(v), big_endian);
}

void PatchU32(std::vector<uint8_t>& buf, size_t offset, uint32_t v,
              bool big_endian) {
  std::vector<uint8_t> tmp;
  AppendU32(tmp, v, big_endian);
  std::memcpy(buf.data() + offset, tmp.data(), 4);
}

void PatchU64(std::vector<uint8_t>& buf, size_t offset, uint64_t v,
              bool big_endian) {
  std::vector<uint8_t> tmp;
  AppendU64(tmp, v, big_endian);
  std::memcpy(buf.data() + offset, tmp.data(), 8);
}

void AppendFixedStr(std::vector<uint8_t>& out, std::string_view s, size_t width,
                    char pad = ' ') {
  const size_t n = std::min(width, s.size());
  out.insert(out.end(), s.begin(), s.begin() + static_cast<ptrdiff_t>(n));
  out.insert(out.end(), width - n, static_cast<uint8_t>(pad));
}

enum class TiffSampleType : uint8_t {
  kInt16,
  kUInt16,
  kInt32,
  kUInt32,
  kFloat32,
  kFloat64,
  kUnsupportedUInt8,
};

struct TiffIfdOptions {
  uint32_t width = 3;
  uint32_t height = 3;
  bool include_dimensions = true;
  std::optional<uint16_t> samples_per_pixel = 1;
  bool include_bits_per_sample = true;
  bool include_planar_config = true;
  uint16_t planar_config = 1;  // 1 = CONTIG, 2 = SEPARATE
  bool include_sample_format = true;
  TiffSampleType sample_type = TiffSampleType::kFloat32;
  std::optional<uint16_t> photometric = 1;  // 1 = MINISBLACK, 2 = RGB
  std::optional<uint16_t> compression = 1;  // 1 = NONE, 7 = OJPEG
  std::optional<uint32_t> subfile_type;     // Tag 254

  uint32_t rows_per_strip =
      0;  // 0 => height; if tile_width/length > 0 => tiled
  uint32_t tile_width = 0;
  uint32_t tile_length = 0;

  bool include_geokeys = true;
  std::vector<uint16_t> custom_geokeys;
  uint16_t model_type = 2;   // 2 = Geographic, 1 = Projected
  uint16_t raster_type = 2;  // 2 = PixelIsPoint, 1 = PixelIsArea

  bool use_trans_matrix = false;
  std::array<double, 16> trans_matrix{};
  bool include_pixel_scale = true;
  std::vector<double> custom_pixel_scale;
  bool include_tie_points = true;
  std::vector<double> custom_tie_points;

  double west = 10.0;
  double north = 50.0;
  double res_x = 1.0;
  double res_y = 1.0;

  std::string gdal_metadata;
  std::optional<std::string> gdal_nodata;

  // Pixel values ordered row-major (TIFF row 0 = northmost when top-down,
  // southmost when bottom-up), interleaved as [row][col][sample].
  std::vector<double> pixels;
};

void EncodeTiffSample(double value, TiffSampleType sample_type, bool big_endian,
                      std::vector<uint8_t>& dst) {
  switch (sample_type) {
    case TiffSampleType::kInt16:
      AppendU16(dst, std::bit_cast<uint16_t>(static_cast<int16_t>(value)),
                big_endian);
      break;
    case TiffSampleType::kUInt16:
      AppendU16(dst, static_cast<uint16_t>(value), big_endian);
      break;
    case TiffSampleType::kInt32:
      AppendU32(dst, std::bit_cast<uint32_t>(static_cast<int32_t>(value)),
                big_endian);
      break;
    case TiffSampleType::kUInt32:
      AppendU32(dst, static_cast<uint32_t>(value), big_endian);
      break;
    case TiffSampleType::kFloat32:
      AppendF32(dst, static_cast<float>(value), big_endian);
      break;
    case TiffSampleType::kFloat64:
      AppendF64(dst, value, big_endian);
      break;
    case TiffSampleType::kUnsupportedUInt8:
      dst.push_back(static_cast<uint8_t>(value));
      break;
  }
}

uint16_t SampleBits(TiffSampleType sample_type) {
  switch (sample_type) {
    case TiffSampleType::kInt16:
    case TiffSampleType::kUInt16:
      return 16;
    case TiffSampleType::kInt32:
    case TiffSampleType::kUInt32:
    case TiffSampleType::kFloat32:
      return 32;
    case TiffSampleType::kFloat64:
      return 64;
    case TiffSampleType::kUnsupportedUInt8:
      return 8;
  }
  return 32;
}

uint16_t SampleFormatCode(TiffSampleType sample_type) {
  switch (sample_type) {
    case TiffSampleType::kUInt16:
    case TiffSampleType::kUInt32:
    case TiffSampleType::kUnsupportedUInt8:
      return 1;  // SAMPLEFORMAT_UINT
    case TiffSampleType::kInt16:
    case TiffSampleType::kInt32:
      return 2;  // SAMPLEFORMAT_INT
    case TiffSampleType::kFloat32:
    case TiffSampleType::kFloat64:
      return 3;  // SAMPLEFORMAT_IEEEFP
  }
  return 3;
}

std::vector<uint8_t> BuildCustomTiff(std::span<const TiffIfdOptions> ifds,
                                     bool big_endian = false,
                                     bool big_tiff = false) {
  std::vector<uint8_t> out;
  if (ifds.empty()) {
    return out;
  }

  out.push_back(big_endian ? 'M' : 'I');
  out.push_back(big_endian ? 'M' : 'I');
  if (big_tiff) {
    AppendU16(out, 43, big_endian);  // 0x002B
    AppendU16(out, 8, big_endian);   // 8-byte offsets
    AppendU16(out, 0, big_endian);
    AppendU64(out, 16, big_endian);  // First IFD starts at byte 16
  } else {
    AppendU16(out, 42, big_endian);  // 0x002A
    AppendU32(out, 8, big_endian);   // First IFD starts at byte 8
  }

  std::optional<size_t> prev_next_ifd_patch;

  for (const TiffIfdOptions& opt : ifds) {
    while (out.size() % 4 != 0) {
      out.push_back(0);
    }
    const size_t ifd_start = out.size();
    if (prev_next_ifd_patch.has_value()) {
      if (big_tiff) {
        PatchU64(out, *prev_next_ifd_patch, static_cast<uint64_t>(ifd_start),
                 big_endian);
      } else {
        PatchU32(out, *prev_next_ifd_patch, static_cast<uint32_t>(ifd_start),
                 big_endian);
      }
    }

    const uint16_t spp =
        std::max<uint16_t>(1, opt.samples_per_pixel.value_or(1));
    const bool is_tiled = opt.tile_width > 0 && opt.tile_length > 0;
    const uint32_t block_w = is_tiled ? opt.tile_width : opt.width;
    const uint32_t block_h =
        is_tiled ? opt.tile_length
                 : (opt.rows_per_strip > 0 ? opt.rows_per_strip : opt.height);

    // Prepare raw encoded blocks.
    std::vector<std::vector<uint8_t>> blocks;
    const bool has_full_pixels =
        opt.include_dimensions && opt.width > 0 && opt.height > 0 &&
        static_cast<uint64_t>(opt.width) * opt.height * spp <= 1000000ULL &&
        (opt.pixels.empty() ||
         opt.pixels.size() ==
             static_cast<size_t>(opt.width) * opt.height * spp);
    if (has_full_pixels && block_w > 0 && block_h > 0) {
      const uint32_t blocks_per_row = (opt.width + block_w - 1) / block_w;
      const uint32_t blocks_per_col = (opt.height + block_h - 1) / block_h;
      const uint32_t blocks_per_plane = blocks_per_row * blocks_per_col;
      const uint32_t total_blocks = (opt.planar_config == 2 && spp > 1)
                                        ? blocks_per_plane * spp
                                        : blocks_per_plane;
      blocks.resize(total_blocks);

      for (uint32_t b = 0; b < total_blocks; ++b) {
        const uint16_t plane = (opt.planar_config == 2 && spp > 1)
                                   ? static_cast<uint16_t>(b / blocks_per_plane)
                                   : 0;
        const uint32_t pb = b % blocks_per_plane;
        const uint32_t by = pb / blocks_per_row;
        const uint32_t bx = pb % blocks_per_row;
        const uint32_t row_count =
            is_tiled ? block_h : std::min(block_h, opt.height - by * block_h);
        const uint32_t col_count = block_w;

        for (uint32_t ly = 0; ly < row_count; ++ly) {
          const uint32_t gy = by * block_h + ly;
          for (uint32_t lx = 0; lx < col_count; ++lx) {
            const uint32_t gx = bx * block_w + lx;
            if (opt.planar_config == 2 && spp > 1) {
              double v = 0.0;
              if (!opt.pixels.empty() && gx < opt.width && gy < opt.height) {
                v = opt.pixels[(static_cast<size_t>(gy) * opt.width + gx) *
                                   spp +
                               plane];
              }
              EncodeTiffSample(v, opt.sample_type, big_endian, blocks[b]);
            } else {
              for (uint16_t s = 0; s < spp; ++s) {
                double v = 0.0;
                if (!opt.pixels.empty() && gx < opt.width && gy < opt.height) {
                  v = opt.pixels[(static_cast<size_t>(gy) * opt.width + gx) *
                                     spp +
                                 s];
                }
                EncodeTiffSample(v, opt.sample_type, big_endian, blocks[b]);
              }
            }
          }
        }
      }
    } else {
      // Dummy single block for header-validation / huge-block error tests.
      blocks.push_back({0, 0, 0, 0});
    }

    struct RawTag {
      uint16_t tag;
      uint16_t type;
      uint64_t count;
      std::vector<uint8_t> payload;
    };
    std::vector<RawTag> tags;

    auto add_u16_vec = [&](uint16_t tag, std::span<const uint16_t> vals) {
      std::vector<uint8_t> p;
      for (uint16_t v : vals) {
        AppendU16(p, v, big_endian);
      }
      tags.push_back({tag, /*TIFF_SHORT=*/3, vals.size(), std::move(p)});
    };
    auto add_u32_vec = [&](uint16_t tag, std::span<const uint32_t> vals) {
      std::vector<uint8_t> p;
      for (uint32_t v : vals) {
        AppendU32(p, v, big_endian);
      }
      tags.push_back({tag, /*TIFF_LONG=*/4, vals.size(), std::move(p)});
    };
    auto add_f64_vec = [&](uint16_t tag, std::span<const double> vals) {
      std::vector<uint8_t> p;
      for (double v : vals) {
        AppendF64(p, v, big_endian);
      }
      tags.push_back({tag, /*TIFF_DOUBLE=*/12, vals.size(), std::move(p)});
    };
    auto add_ascii = [&](uint16_t tag, std::string_view s) {
      std::vector<uint8_t> p(s.begin(), s.end());
      p.push_back('\0');
      tags.push_back({tag, /*TIFF_ASCII=*/2, p.size(), std::move(p)});
    };

    // Emit tags in strictly ascending numerical order.
    if (opt.subfile_type.has_value()) {
      const uint32_t v = *opt.subfile_type;
      add_u32_vec(254, std::span<const uint32_t>(&v, 1));
    }
    if (opt.include_dimensions) {
      add_u32_vec(256, std::span<const uint32_t>(&opt.width, 1));
      add_u32_vec(257, std::span<const uint32_t>(&opt.height, 1));
    }
    if (opt.include_bits_per_sample) {
      const std::vector<uint16_t> bps(spp, SampleBits(opt.sample_type));
      add_u16_vec(258, bps);
    }
    if (opt.compression.has_value()) {
      const uint16_t c = *opt.compression;
      add_u16_vec(259, std::span<const uint16_t>(&c, 1));
    }
    if (opt.photometric.has_value()) {
      const uint16_t pm = *opt.photometric;
      add_u16_vec(262, std::span<const uint16_t>(&pm, 1));
    }

    size_t offsets_tag_idx = 0;
    std::vector<uint32_t> block_byte_counts;
    block_byte_counts.reserve(blocks.size());
    for (const auto& blk : blocks) {
      block_byte_counts.push_back(static_cast<uint32_t>(blk.size()));
    }
    const std::vector<uint32_t> placeholder_offsets(blocks.size(), 0);

    if (!is_tiled) {
      offsets_tag_idx = tags.size();
      add_u32_vec(273, placeholder_offsets);  // StripOffsets
    }
    if (opt.samples_per_pixel.has_value()) {
      const uint16_t s = *opt.samples_per_pixel;
      add_u16_vec(277, std::span<const uint16_t>(&s, 1));
    }
    if (!is_tiled) {
      const uint32_t rps =
          opt.rows_per_strip > 0 ? opt.rows_per_strip : opt.height;
      add_u32_vec(278, std::span<const uint32_t>(&rps, 1));
      add_u32_vec(279, block_byte_counts);  // StripByteCounts
    }
    if (opt.include_planar_config) {
      add_u16_vec(284, std::span<const uint16_t>(&opt.planar_config, 1));
    }
    if (is_tiled) {
      add_u32_vec(322, std::span<const uint32_t>(&opt.tile_width, 1));
      add_u32_vec(323, std::span<const uint32_t>(&opt.tile_length, 1));
      offsets_tag_idx = tags.size();
      add_u32_vec(324, placeholder_offsets);  // TileOffsets
      add_u32_vec(325, block_byte_counts);    // TileByteCounts
    }
    if (opt.include_sample_format) {
      const std::vector<uint16_t> sf(spp, SampleFormatCode(opt.sample_type));
      add_u16_vec(339, sf);
    }
    if (opt.use_trans_matrix) {
      add_f64_vec(34264, opt.trans_matrix);
    } else {
      if (opt.include_pixel_scale) {
        if (!opt.custom_pixel_scale.empty()) {
          add_f64_vec(33550, opt.custom_pixel_scale);
        } else {
          const double ps[3] = {opt.res_x, opt.res_y, 0.0};
          add_f64_vec(33550, ps);
        }
      }
      if (opt.include_tie_points) {
        if (!opt.custom_tie_points.empty()) {
          add_f64_vec(33922, opt.custom_tie_points);
        } else {
          const double tp[6] = {0.0, 0.0, 0.0, opt.west, opt.north, 0.0};
          add_f64_vec(33922, tp);
        }
      }
    }
    if (opt.include_geokeys) {
      if (!opt.custom_geokeys.empty()) {
        add_u16_vec(34735, opt.custom_geokeys);
      } else {
        const uint16_t gk[12] = {
            1,    1, 0, 2,                // GeoTIFF 1.1 rev 0, 2 keys
            1024, 0, 1, opt.model_type,   // GTModelTypeGeoKey
            1025, 0, 1, opt.raster_type,  // GTRasterTypeGeoKey
        };
        add_u16_vec(34735, gk);
      }
    }
    if (!opt.gdal_metadata.empty()) {
      add_ascii(42112, opt.gdal_metadata);
    }
    if (opt.gdal_nodata.has_value()) {
      add_ascii(42113, *opt.gdal_nodata);
    }

    // Re-sort by tag just in case custom matrix/scale/tie order varied.
    // Keep track of offsets_tag_idx after sorting.
    const uint16_t offset_tag_id = is_tiled ? 324 : 273;
    std::ranges::sort(tags, {}, &RawTag::tag);  // NOLINT(build/c++20)
    for (size_t i = 0; i < tags.size(); ++i) {
      if (tags[i].tag == offset_tag_id) {
        offsets_tag_idx = i;
        break;
      }
    }

    const size_t entry_size = big_tiff ? 20 : 12;
    const size_t count_size = big_tiff ? 8 : 2;
    const size_t next_size = big_tiff ? 8 : 4;
    const size_t inline_limit = big_tiff ? 8 : 4;
    const size_t ifd_table_size =
        count_size + tags.size() * entry_size + next_size;

    // Reserve IFD space to fill once block and out-of-line offsets are known.
    out.resize(ifd_start + ifd_table_size, 0);
    while (out.size() % 4 != 0) {
      out.push_back(0);
    }

    // Write out-of-line tag payloads first so image blocks always sit at the
    // end of the IFD/file and tail-truncation tests corrupt only pixel data.
    std::vector<uint64_t> tag_offsets(tags.size(), 0);
    for (size_t i = 0; i < tags.size(); ++i) {
      if (tags[i].payload.size() > inline_limit) {
        while (out.size() % 4 != 0) {
          out.push_back(0);
        }
        tag_offsets[i] = out.size();
        out.insert(out.end(), tags[i].payload.begin(), tags[i].payload.end());
      }
    }

    // Write image blocks and patch StripOffsets / TileOffsets payload.
    tags[offsets_tag_idx].payload.clear();
    for (const auto& blk : blocks) {
      while (out.size() % 4 != 0) {
        out.push_back(0);
      }
      AppendU32(tags[offsets_tag_idx].payload,
                static_cast<uint32_t>(out.size()), big_endian);
      out.insert(out.end(), blk.begin(), blk.end());
    }
    if (tags[offsets_tag_idx].payload.size() > inline_limit) {
      std::memcpy(out.data() + tag_offsets[offsets_tag_idx],
                  tags[offsets_tag_idx].payload.data(),
                  tags[offsets_tag_idx].payload.size());
    }

    // Serialize IFD table at ifd_start.
    std::vector<uint8_t> table;
    table.reserve(ifd_table_size);
    if (big_tiff) {
      AppendU64(table, tags.size(), big_endian);
    } else {
      AppendU16(table, static_cast<uint16_t>(tags.size()), big_endian);
    }
    for (size_t i = 0; i < tags.size(); ++i) {
      AppendU16(table, tags[i].tag, big_endian);
      AppendU16(table, tags[i].type, big_endian);
      if (big_tiff) {
        AppendU64(table, tags[i].count, big_endian);
      } else {
        AppendU32(table, static_cast<uint32_t>(tags[i].count), big_endian);
      }
      if (tags[i].payload.size() <= inline_limit) {
        table.insert(table.end(), tags[i].payload.begin(),
                     tags[i].payload.end());
        table.insert(table.end(), inline_limit - tags[i].payload.size(), 0);
      } else if (big_tiff) {
        AppendU64(table, tag_offsets[i], big_endian);
      } else {
        AppendU32(table, static_cast<uint32_t>(tag_offsets[i]), big_endian);
      }
    }
    prev_next_ifd_patch = ifd_start + table.size();
    if (big_tiff) {
      AppendU64(table, 0, big_endian);
    } else {
      AppendU32(table, 0, big_endian);
    }
    std::memcpy(out.data() + ifd_start, table.data(), table.size());
  }

  return out;
}

std::vector<uint8_t> BuildSingleTiff(const TiffIfdOptions& opt,
                                     bool big_endian = false,
                                     bool big_tiff = false) {
  return BuildCustomTiff(std::span<const TiffIfdOptions>(&opt, 1), big_endian,
                         big_tiff);
}

// Synthesizes a big-endian NTv1 grid (rows south-to-north, cols east-to-west,
// records as pairs of (lat_shift_arcsec, lon_shift_west_pos_arcsec)).
std::vector<uint8_t> BuildNtv1Grid(double west_deg, double south_deg,
                                   double east_deg, double north_deg,
                                   double res_x_deg, double res_y_deg,
                                   std::span<const double> records_ew,
                                   int record_count = 12) {
  std::vector<uint8_t> out(192, 0);
  auto write_str = [&](size_t off, std::string_view s) {
    std::memcpy(out.data() + off, s.data(), s.size());
  };
  auto write_be_f64 = [&](size_t off, double v) {
    std::vector<uint8_t> tmp;
    AppendF64(tmp, v, /*big_endian=*/true);
    std::memcpy(out.data() + off, tmp.data(), 8);
  };
  std::vector<uint8_t> rc_bytes;
  AppendU32(rc_bytes, static_cast<uint32_t>(record_count), /*big_endian=*/true);
  std::memcpy(out.data() + 8, rc_bytes.data(), 4);

  write_str(0, "HEADER");
  write_be_f64(24, south_deg);
  write_be_f64(40, north_deg);
  write_be_f64(56, -east_deg);  // NTv1 stores west-positive longitudes.
  write_be_f64(72, -west_deg);
  write_be_f64(88, res_y_deg);
  write_str(96, "W GRID");
  write_be_f64(104, res_x_deg);
  write_str(144, "TO      NAD83   ");

  for (double v : records_ew) {
    AppendF64(out, v, /*big_endian=*/true);
  }
  return out;
}

struct Ntv2SubfileSpec {
  std::string sub_name = "SUB0    ";
  std::string parent_name = "NONE    ";
  double west_deg = 10.0;
  double south_deg = 50.0;
  double res_x_deg = 1.0;
  double res_y_deg = 1.0;
  int cols = 2;
  int rows = 2;
  std::string sub_tag = "SUB_NAME";
  std::optional<uint32_t> gs_count_override;
  // 4 floats per cell: lat_arcsec, lon_west_arcsec, lat_acc, lon_acc
  std::vector<float> records;
};

std::vector<uint8_t> BuildCustomNtv2(std::span<const Ntv2SubfileSpec> subfiles,
                                     bool big_endian,
                                     std::string_view gs_type = "SECONDS ") {
  std::vector<uint8_t> out;
  out.reserve(176);

  // NUM_OREC: note grids.cpp tests `header[8] == 11` for LE detection.
  AppendFixedStr(out, "NUM_OREC", 8);
  AppendU32(out, 11, big_endian);
  AppendU32(out, 0, big_endian);
  AppendFixedStr(out, "NUM_SREC", 8);
  AppendU32(out, 11, big_endian);
  AppendU32(out, 0, big_endian);
  AppendFixedStr(out, "NUM_FILE", 8);
  AppendU32(out, static_cast<uint32_t>(subfiles.size()), big_endian);
  AppendU32(out, 0, big_endian);
  AppendFixedStr(out, "GS_TYPE ", 8);
  AppendFixedStr(out, gs_type, 8);
  AppendFixedStr(out, "VERSION ", 8);
  AppendFixedStr(out, "NTv2.0  ", 8);
  AppendFixedStr(out, "SYSTEM_F", 8);
  AppendFixedStr(out, "SRC     ", 8);
  AppendFixedStr(out, "SYSTEM_T", 8);
  AppendFixedStr(out, "DST     ", 8);
  AppendFixedStr(out, "MAJOR_F ", 8);
  AppendF64(out, 6378137.0, big_endian);
  AppendFixedStr(out, "MINOR_F ", 8);
  AppendF64(out, 6356752.314, big_endian);
  AppendFixedStr(out, "MAJOR_T ", 8);
  AppendF64(out, 6378137.0, big_endian);
  AppendFixedStr(out, "MINOR_T ", 8);
  AppendF64(out, 6356752.314, big_endian);

  for (const auto& sf : subfiles) {
    const double north_deg = sf.south_deg + (sf.rows - 1) * sf.res_y_deg;
    const double east_deg = sf.west_deg + (sf.cols - 1) * sf.res_x_deg;
    const uint32_t gs_count =
        sf.gs_count_override.value_or(static_cast<uint32_t>(sf.cols * sf.rows));

    AppendFixedStr(out, sf.sub_tag, 8);
    AppendFixedStr(out, sf.sub_name, 8);
    AppendFixedStr(out, "PARENT  ", 8);
    AppendFixedStr(out, sf.parent_name, 8);
    AppendFixedStr(out, "CREATED ", 8);
    AppendFixedStr(out, "20260101", 8);
    AppendFixedStr(out, "UPDATED ", 8);
    AppendFixedStr(out, "20260101", 8);
    AppendFixedStr(out, "S_LAT   ", 8);
    AppendF64(out, sf.south_deg * 3600.0, big_endian);
    AppendFixedStr(out, "N_LAT   ", 8);
    AppendF64(out, north_deg * 3600.0, big_endian);
    AppendFixedStr(out, "E_LONG  ", 8);
    AppendF64(out, -east_deg * 3600.0, big_endian);
    AppendFixedStr(out, "W_LONG  ", 8);
    AppendF64(out, -sf.west_deg * 3600.0, big_endian);
    AppendFixedStr(out, "LAT_INC ", 8);
    AppendF64(out, sf.res_y_deg * 3600.0, big_endian);
    AppendFixedStr(out, "LONG_INC", 8);
    AppendF64(out, sf.res_x_deg * 3600.0, big_endian);
    AppendFixedStr(out, "GS_COUNT", 8);
    AppendU32(out, gs_count, big_endian);
    AppendU32(out, 0, big_endian);
    for (float v : sf.records) {
      AppendF32(out, v, big_endian);
    }
  }
  return out;
}

TEST(ExtentAndResTest, ComputeInvResFullWorldContainsAndIntersects) {
  NS_PROJ::ExtentAndRes a{
      .isGeographic = true,
      .west = 0.0,
      .south = 0.0,
      .east = 10.0 * kDegToRad,
      .north = 10.0 * kDegToRad,
      .resX = 1.0 * kDegToRad,
      .resY = 2.0 * kDegToRad,
  };
  a.computeInvRes();
  EXPECT_NEAR(1.0 / (1.0 * kDegToRad), a.invResX, 1e-12);
  EXPECT_NEAR(1.0 / (2.0 * kDegToRad), a.invResY, 1e-12);
  EXPECT_FALSE(a.fullWorldLongitude());

  NS_PROJ::ExtentAndRes full_world = a;
  full_world.west = -kPi;
  full_world.east = kPi - 1.0 * kDegToRad;
  EXPECT_TRUE(full_world.fullWorldLongitude());

  NS_PROJ::ExtentAndRes projected = full_world;
  projected.isGeographic = false;
  EXPECT_FALSE(projected.fullWorldLongitude());

  const NS_PROJ::ExtentAndRes inside{
      .isGeographic = true,
      .west = 2.0 * kDegToRad,
      .south = 2.0 * kDegToRad,
      .east = 8.0 * kDegToRad,
      .north = 8.0 * kDegToRad,
  };
  EXPECT_TRUE(a.contains(inside));
  EXPECT_FALSE(inside.contains(a));

  const NS_PROJ::ExtentAndRes overlap{
      .isGeographic = true,
      .west = 5.0 * kDegToRad,
      .south = 5.0 * kDegToRad,
      .east = 15.0 * kDegToRad,
      .north = 15.0 * kDegToRad,
  };
  EXPECT_FALSE(a.contains(overlap));
  EXPECT_TRUE(a.intersects(overlap));

  const NS_PROJ::ExtentAndRes disjoint{
      .isGeographic = true,
      .west = 20.0 * kDegToRad,
      .south = 20.0 * kDegToRad,
      .east = 25.0 * kDegToRad,
      .north = 25.0 * kDegToRad,
  };
  EXPECT_FALSE(a.intersects(disjoint));
}

TEST(NullGridsTest, NullVerticalHorizontalAndGenericGridSets) {
  ScopedContext ctx = MakeContext();
  ScopedContext other_ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ASSERT_NE(nullptr, other_ctx.get());

  // 1. NullVerticalShiftGrid
  auto vset = NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "null");
  ASSERT_NE(nullptr, vset);
  EXPECT_EQ("null", vset->name());
  EXPECT_EQ("null", vset->format());
  ASSERT_EQ(1U, vset->grids().size());
  const auto* vgrid = vset->gridAt(0.5, 0.5);
  ASSERT_NE(nullptr, vgrid);
  EXPECT_TRUE(vgrid->isNullGrid());
  EXPECT_FALSE(vgrid->hasChanged());
  EXPECT_TRUE(vgrid->metadataItem("any").empty());
  EXPECT_FALSE(vgrid->isNodata(0.0f, 1.0));
  float vout = 99.0f;
  EXPECT_TRUE(vgrid->valueAt(0, 0, vout));
  EXPECT_EQ(0.0f, vout);
  vset->reassign_context(other_ctx.get());
  EXPECT_TRUE(vset->reopen(ctx.get()));

  // 2. NullHorizontalShiftGrid
  auto hset = NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "null");
  ASSERT_NE(nullptr, hset);
  EXPECT_EQ("null", hset->format());
  const auto* hgrid = hset->gridAt(0.5, 0.5);
  ASSERT_NE(nullptr, hgrid);
  EXPECT_TRUE(hgrid->isNullGrid());
  EXPECT_FALSE(hgrid->hasChanged());
  EXPECT_TRUE(hgrid->metadataItem("any").empty());
  float dlon = 99.0f;
  float dlat = 99.0f;
  EXPECT_TRUE(hgrid->valueAt(0, 0, true, dlon, dlat));
  EXPECT_EQ(0.0f, dlon);
  EXPECT_EQ(0.0f, dlat);
  hset->reassign_context(other_ctx.get());
  EXPECT_TRUE(hset->reopen(ctx.get()));

  // 3. NullGenericShiftGrid (also exercises GenericShiftGrid::valuesAt)
  auto gset = NS_PROJ::GenericShiftGridSet::open(ctx.get(), "null");
  ASSERT_NE(nullptr, gset);
  EXPECT_EQ("null", gset->format());
  EXPECT_EQ(gset->gridAt(0.5, 0.5), gset->gridAt("", 0.5, 0.5));
  const auto* ggrid = gset->gridAt(0.5, 0.5);
  ASSERT_NE(nullptr, ggrid);
  EXPECT_TRUE(ggrid->isNullGrid());
  EXPECT_TRUE(ggrid->type().empty());
  EXPECT_EQ(0, ggrid->samplesPerPixel());
  EXPECT_TRUE(ggrid->unit(0).empty());
  EXPECT_TRUE(ggrid->description(0).empty());
  EXPECT_TRUE(ggrid->metadataItem("any").empty());
  EXPECT_FALSE(ggrid->hasChanged());
  float gout = 42.0f;
  EXPECT_TRUE(ggrid->valueAt(0, 0, 0, gout));
  EXPECT_EQ(0.0f, gout);
  const int sample_idx[1] = {0};
  float vals[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  bool nodata = true;
  EXPECT_TRUE(ggrid->valuesAt(0, 0, 2, 2, 1, sample_idx, vals, nodata));
  EXPECT_FALSE(nodata);
  EXPECT_EQ(0.0f, vals[3]);
  gset->reassign_context(other_ctx.get());
  EXPECT_TRUE(gset->reopen(ctx.get()));
}

TEST(GtxGridTest, ValidUppercaseExtensionWrapAndDatelineLog) {
  ScopedContext ctx = MakeContext();
  ScopedContext other_ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogCapture log_cap(ctx.get(), PJ_LOG_DEBUG);

  // xorigin = 200.0 wraps by -360 to -160.0; second grid tests dateline log.
  const std::vector<float> shifts = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  mem.AddFile("/mem/wrap.GTX",
              BuildGtxGrid(/*west_deg=*/200.0, /*south_deg=*/10.0,
                           /*res_x_deg=*/1.0, /*res_y_deg=*/1.0,
                           /*rows=*/2, /*cols=*/3, shifts));
  mem.AddFile("/mem/dateline.gtx",
              BuildGtxGrid(/*west_deg=*/100.0, /*south_deg=*/0.0,
                           /*res_x_deg=*/30.0, /*res_y_deg=*/1.0,
                           /*rows=*/2, /*cols=*/3, shifts));

  auto dateline =
      NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "/mem/dateline.gtx");
  ASSERT_NE(nullptr, dateline);
  EXPECT_TRUE(log_cap.ContainsMessage("spans the dateline"));

  auto set = NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "/mem/wrap.GTX");
  ASSERT_NE(nullptr, set);
  EXPECT_EQ("gtx", set->format());
  const auto* grid = set->gridAt(DegToRad(-159.0), DegToRad(10.5));
  ASSERT_NE(nullptr, grid);
  EXPECT_NEAR(DegToRad(-160.0), grid->extentAndRes().west, 1e-12);
  EXPECT_NEAR(DegToRad(-158.0), grid->extentAndRes().east, 1e-12);
  EXPECT_FALSE(grid->hasChanged());
  EXPECT_TRUE(grid->metadataItem("unused").empty());

  // Miss then cache hit on row 1.
  float val = 0.0f;
  EXPECT_TRUE(grid->valueAt(0, 1, val));
  EXPECT_FLOAT_EQ(4.0f, val);
  EXPECT_TRUE(grid->valueAt(2, 1, val));
  EXPECT_FLOAT_EQ(6.0f, val);

  // isNodata branches: -88.88880f, > 1000, < -1000, and valid.
  EXPECT_TRUE(grid->isNodata(-88.88880f, 1.0));
  EXPECT_TRUE(grid->isNodata(500.0f, 3.0));
  EXPECT_TRUE(grid->isNodata(-1001.0f, 1.0));
  EXPECT_FALSE(grid->isNodata(10.0f, 1.0));

  set->reassign_context(other_ctx.get());
  set->reassign_context(ctx.get());
}

TEST(GtxGridTest, InvalidHeadersExtentsAndTruncatedData) {
  ScopedContext ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogSuppressor quiet(ctx.get());

  // 1. Header < 40 bytes.
  mem.AddFile("/mem/short.gtx", std::vector<uint8_t>(20, 0));
  EXPECT_EQ(nullptr,
            NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "/mem/short.gtx"));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);

  // 2. Invalid extent parameters.
  struct BadExtent {
    double west;
    double south;
    int32_t rows;
    int32_t cols;
  };
  constexpr BadExtent kBadExtents[] = {
      {.west = 0.0, .south = 0.0, .rows = 2, .cols = 0},
      {.west = 0.0, .south = 0.0, .rows = 0, .cols = 2},
      {.west = -361.0, .south = 0.0, .rows = 2, .cols = 2},
      {.west = 361.0, .south = 0.0, .rows = 2, .cols = 2},
      {.west = 0.0, .south = -91.0, .rows = 2, .cols = 2},
      {.west = 0.0, .south = 91.0, .rows = 2, .cols = 2},
  };
  for (size_t i = 0; i < std::size(kBadExtents); ++i) {
    std::vector<uint8_t> hdr;
    AppendF64(hdr, kBadExtents[i].south, true);
    AppendF64(hdr, kBadExtents[i].west, true);
    AppendF64(hdr, 1.0, true);
    AppendF64(hdr, 1.0, true);
    AppendU32(hdr, static_cast<uint32_t>(kBadExtents[i].rows), true);
    AppendU32(hdr, static_cast<uint32_t>(kBadExtents[i].cols), true);
    const std::string path = "/mem/bad_" + std::to_string(i) + ".gtx";
    mem.AddFile(path, std::move(hdr));
    EXPECT_EQ(nullptr, NS_PROJ::VerticalShiftGridSet::open(ctx.get(), path));
    proj_context_errno_set(ctx.get(), 0);
  }

  // 3. Valid header but truncated body fails in valueAt.
  std::vector<uint8_t> trunc =
      BuildGtxGrid(0.0, 0.0, 1.0, 1.0, 2, 2, std::vector<float>(4, 1.0f));
  trunc.resize(40);
  mem.AddFile("/mem/trunc.gtx", std::move(trunc));
  auto set = NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "/mem/trunc.gtx");
  ASSERT_NE(nullptr, set);
  float out = 0.0f;
  EXPECT_FALSE(set->grids().front()->valueAt(0, 0, out));
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
}

TEST(Ntv1GridTest, SyntheticAndEmbeddedValidAndErrorBranches) {
  ScopedContext ctx = MakeContext();
  ScopedContext other_ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogSuppressor quiet(ctx.get());

  // 1. Valid synthetic BE NTv1 (2x2: cols west->east = -60, -59; south->north =
  // 45, 46). Remember NTv1 stores rows East-to-West! So row 0 stores (x=1,
  // x=0).
  const std::vector<double> recs = {
      10.0, 20.0,  // y=0, x=1 (east)
      30.0, 40.0,  // y=0, x=0 (west)
      50.0, 60.0,  // y=1, x=1 (east)
      70.0, 80.0,  // y=1, x=0 (west)
  };
  mem.AddFile("/mem/valid.dat",
              BuildNtv1Grid(-60.0, 45.0, -59.0, 46.0, 1.0, 1.0, recs));

  auto set = NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/valid.dat");
  ASSERT_NE(nullptr, set);
  EXPECT_EQ("ntv1", set->format());
  const auto* grid = set->gridAt(DegToRad(-59.5), DegToRad(45.5));
  ASSERT_NE(nullptr, grid);
  EXPECT_FALSE(grid->hasChanged());
  EXPECT_TRUE(grid->metadataItem("any").empty());

  float dlon = 0.0f;
  float dlat = 0.0f;
  EXPECT_TRUE(grid->valueAt(0, 0, /*compensateNTConvention=*/true, dlon, dlat));
  EXPECT_NEAR(-40.0 * kArcsecToRad, dlon, 1e-7);
  EXPECT_NEAR(30.0 * kArcsecToRad, dlat, 1e-7);
  EXPECT_TRUE(
      grid->valueAt(1, 1, /*compensateNTConvention=*/false, dlon, dlat));
  EXPECT_NEAR(60.0 * kArcsecToRad, dlon, 1e-7);
  EXPECT_NEAR(50.0 * kArcsecToRad, dlat, 1e-7);
  set->reassign_context(other_ctx.get());
  set->reassign_context(ctx.get());

  // 2. Short header (160..191 bytes passes format detection, fails
  // NTv1Grid::open).
  std::vector<uint8_t> short_ntv1 =
      BuildNtv1Grid(-60.0, 45.0, -59.0, 46.0, 1.0, 1.0, recs);
  short_ntv1.resize(170);
  mem.AddFile("/mem/short_ntv1.dat", std::move(short_ntv1));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/short_ntv1.dat"));

  // 3. Wrong record count (!= 12).
  mem.AddFile("/mem/bad_rc.dat",
              BuildNtv1Grid(-60.0, 45.0, -59.0, 46.0, 1.0, 1.0, recs, 7));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(ctx.get(),
                                                           "/mem/bad_rc.dat"));

  // 4. Inconsistent georeferencing (west > east).
  mem.AddFile("/mem/bad_georef.dat",
              BuildNtv1Grid(10.0, 45.0, -10.0, 46.0, 1.0, 1.0, recs));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/bad_georef.dat"));

  // 5. Truncated data -> valueAt returns false.
  std::vector<uint8_t> trunc_ntv1 =
      BuildNtv1Grid(-60.0, 45.0, -59.0, 46.0, 1.0, 1.0, recs);
  trunc_ntv1.resize(192);
  mem.AddFile("/mem/trunc_ntv1.dat", std::move(trunc_ntv1));
  auto trunc_set =
      NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/trunc_ntv1.dat");
  ASSERT_NE(nullptr, trunc_set);
  EXPECT_FALSE(trunc_set->grids().front()->valueAt(0, 0, true, dlon, dlat));

  // 6. Embedded ntv1_can.dat using a fresh default-API context.
  ScopedContext embed_ctx = MakeContext();
  auto can_set =
      NS_PROJ::HorizontalShiftGridSet::open(embed_ctx.get(), "ntv1_can.dat");
  ASSERT_NE(nullptr, can_set);
  EXPECT_EQ("ntv1", can_set->format());
}

TEST(CTable2GridTest, ValidAndErrorBranches) {
  ScopedContext ctx = MakeContext();
  ScopedContext other_ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogSuppressor quiet(ctx.get());

  // 1. Valid synthetic CTable2.
  const std::vector<float> shifts = {
      0.01f, 0.02f,  // (0, 0)
      0.03f, 0.04f,  // (1, 0)
      0.05f, 0.06f,  // (0, 1)
      0.07f, 0.08f,  // (1, 1)
  };
  mem.AddFile("/mem/valid.ct2",
              BuildCTable2Grid(DegToRad(10.0), DegToRad(50.0), DegToRad(1.0),
                               DegToRad(1.0), 2, 2, shifts));
  auto set = NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/valid.ct2");
  ASSERT_NE(nullptr, set);
  EXPECT_EQ("ctable2", set->format());
  const auto* grid = set->gridAt(DegToRad(10.5), DegToRad(50.5));
  ASSERT_NE(nullptr, grid);
  EXPECT_FALSE(grid->hasChanged());
  EXPECT_TRUE(grid->metadataItem("any").empty());

  float dlon = 0.0f;
  float dlat = 0.0f;
  EXPECT_TRUE(grid->valueAt(1, 0, true, dlon, dlat));
  EXPECT_FLOAT_EQ(-0.03f, dlon);
  EXPECT_FLOAT_EQ(0.04f, dlat);
  EXPECT_TRUE(grid->valueAt(1, 0, false, dlon, dlat));
  EXPECT_FLOAT_EQ(0.03f, dlon);
  set->reassign_context(other_ctx.get());
  set->reassign_context(ctx.get());

  // 2. Short header (< 160 bytes).
  std::vector<uint8_t> short_ct2(100, 0);
  std::memcpy(short_ct2.data(), "CTABLE V2", 9);
  mem.AddFile("/mem/short.ct2", std::move(short_ct2));
  EXPECT_EQ(nullptr,
            NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/short.ct2"));

  // 3. Inconsistent georeferencing (resX == 0).
  std::vector<uint8_t> bad_ct2 =
      BuildCTable2Grid(0.0, 0.0, 0.1, 0.1, 2, 2, std::vector<float>(8, 0.0f));
  // Overwrite resX at offset 112 with 0.0.
  std::memset(bad_ct2.data() + 112, 0, 8);
  mem.AddFile("/mem/bad_res.ct2", std::move(bad_ct2));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(ctx.get(),
                                                           "/mem/bad_res.ct2"));

  // 4. Invalid width <= 0.
  std::vector<uint8_t> bad_dim =
      BuildCTable2Grid(0.0, 0.0, 0.1, 0.1, 2, 2, std::vector<float>(8, 0.0f));
  PatchU32(bad_dim, 128, 0, false);  // cols = 0
  mem.AddFile("/mem/bad_dim.ct2", std::move(bad_dim));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(ctx.get(),
                                                           "/mem/bad_dim.ct2"));

  // 5. Truncated data -> valueAt returns false.
  std::vector<uint8_t> trunc_ct2 =
      BuildCTable2Grid(0.0, 0.0, 0.1, 0.1, 2, 2, std::vector<float>(8, 0.0f));
  trunc_ct2.resize(160);
  mem.AddFile("/mem/trunc.ct2", std::move(trunc_ct2));
  auto trunc_set =
      NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/trunc.ct2");
  ASSERT_NE(nullptr, trunc_set);
  EXPECT_FALSE(trunc_set->grids().front()->valueAt(0, 0, true, dlon, dlat));

  // 6. Embedded "conus" grid.
  ScopedContext embed_ctx = MakeContext();
  auto conus = NS_PROJ::HorizontalShiftGridSet::open(embed_ctx.get(), "conus");
  ASSERT_NE(nullptr, conus);
  EXPECT_EQ("ctable2", conus->format());
}

TEST(Ntv2GridTest, LittleAndBigEndianHierarchyCacheAndErrors) {
  ScopedContext ctx = MakeContext();
  ScopedContext other_ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogCapture log_cap(ctx.get(), PJ_LOG_TRACE);

  // 1. Little-endian NTv2 via BuildNtv2Grid (2x2; note East-to-West row order).
  const std::vector<float> le_recs = {
      1.0f, 2.0f, 0.0f, 0.0f,  // y=0, x=1
      3.0f, 4.0f, 0.0f, 0.0f,  // y=0, x=0
      5.0f, 6.0f, 0.0f, 0.0f,  // y=1, x=1
      7.0f, 8.0f, 0.0f, 0.0f,  // y=1, x=0
  };
  mem.AddFile("/mem/le.gsb",
              BuildNtv2Grid(10.0, 50.0, 1.0, 1.0, 2, 2, le_recs));
  auto le_set = NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/le.gsb");
  ASSERT_NE(nullptr, le_set);
  EXPECT_EQ("ntv2", le_set->format());
  EXPECT_TRUE(log_cap.ContainsMessage("NTv2 TESTGRID"));

  // 2. Big-endian multi-subfile NTv2 (parent + child, width = 3 exercises swap
  // loop).
  const Ntv2SubfileSpec parent_sf{
      .sub_name = "PARENT0 ",
      .parent_name = "NONE    ",
      .west_deg = 0.0,
      .south_deg = 0.0,
      .res_x_deg = 2.0,
      .res_y_deg = 2.0,
      .cols = 3,
      .rows = 2,
      .records = std::vector<float>(3 * 2 * 4, 1.0f),
  };
  // Child inside [1, 3] x [0.5, 1.5], width = 2 (east-to-west cols).
  const Ntv2SubfileSpec child_sf{
      .sub_name = "CHILD0  ",
      .parent_name = "PARENT0 ",
      .west_deg = 1.0,
      .south_deg = 0.5,
      .res_x_deg = 1.0,
      .res_y_deg = 1.0,
      .cols = 2,
      .rows = 2,
      .records =
          {
              11.0f,
              12.0f,
              0.0f,
              0.0f,  // y=0, x=1
              21.0f,
              22.0f,
              0.0f,
              0.0f,  // y=0, x=0
              31.0f,
              32.0f,
              0.0f,
              0.0f,  // y=1, x=1
              41.0f,
              42.0f,
              0.0f,
              0.0f,  // y=1, x=0
          },
  };
  const std::vector<Ntv2SubfileSpec> be_subfiles = {parent_sf, child_sf};
  mem.AddFile("/mem/be_hier.gsb",
              BuildCustomNtv2(be_subfiles, /*big_endian=*/true));

  auto be_set =
      NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/be_hier.gsb");
  ASSERT_NE(nullptr, be_set);
  // Outside child selects parent.
  const auto* p_grid = be_set->gridAt(DegToRad(0.2), DegToRad(0.2));
  ASSERT_NE(nullptr, p_grid);
  EXPECT_THAT(p_grid->name(), HasSubstr("PARENT0"));
  // Inside child selects child.
  const auto* c_grid = be_set->gridAt(DegToRad(1.5), DegToRad(1.0));
  ASSERT_NE(nullptr, c_grid);
  EXPECT_THAT(c_grid->name(), HasSubstr("CHILD0"));
  EXPECT_FALSE(c_grid->hasChanged());
  EXPECT_TRUE(c_grid->metadataItem("any").empty());

  float dlon = 0.0f;
  float dlat = 0.0f;
  // Miss then cache hit on row 0; also check both compensateNTConvention modes.
  EXPECT_TRUE(c_grid->valueAt(0, 0, true, dlon, dlat));
  EXPECT_NEAR(-22.0 * kArcsecToRad, dlon, 1e-7);
  EXPECT_NEAR(21.0 * kArcsecToRad, dlat, 1e-7);
  EXPECT_TRUE(c_grid->valueAt(1, 0, false, dlon, dlat));
  EXPECT_NEAR(12.0 * kArcsecToRad, dlon, 1e-7);
  EXPECT_NEAR(11.0 * kArcsecToRad, dlat, 1e-7);

  be_set->reassign_context(other_ctx.get());
  be_set->reassign_context(ctx.get());

  // 3. Error branches for NTv2.
  std::vector<uint8_t> short_ov = BuildCustomNtv2(be_subfiles, false);
  short_ov.resize(165);
  mem.AddFile("/mem/short_ov.gsb", std::move(short_ov));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/short_ov.gsb"));

  mem.AddFile("/mem/bad_gstype.gsb",
              BuildCustomNtv2(std::span<const Ntv2SubfileSpec>(&parent_sf, 1),
                              false, "RADIANS "));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/bad_gstype.gsb"));

  std::vector<uint8_t> short_sf =
      BuildCustomNtv2(std::span<const Ntv2SubfileSpec>(&parent_sf, 1), false);
  short_sf.resize(200);
  mem.AddFile("/mem/short_sf.gsb", std::move(short_sf));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/short_sf.gsb"));

  Ntv2SubfileSpec bad_subname = parent_sf;
  bad_subname.sub_tag = "NOT_SUB ";
  mem.AddFile("/mem/bad_subtag.gsb",
              BuildCustomNtv2(std::span<const Ntv2SubfileSpec>(&bad_subname, 1),
                              false));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/bad_subtag.gsb"));

  Ntv2SubfileSpec bad_georef = parent_sf;
  bad_georef.res_x_deg = 0.0;
  mem.AddFile(
      "/mem/bad_ntv2_geo.gsb",
      BuildCustomNtv2(std::span<const Ntv2SubfileSpec>(&bad_georef, 1), false));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/bad_ntv2_geo.gsb"));

  Ntv2SubfileSpec bad_count = parent_sf;
  bad_count.gs_count_override = 1;
  mem.AddFile(
      "/mem/bad_gscount.gsb",
      BuildCustomNtv2(std::span<const Ntv2SubfileSpec>(&bad_count, 1), false));
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/bad_gscount.gsb"));

  // Truncated pixel payload fails in valueAt.
  std::vector<uint8_t> trunc_ntv2 =
      BuildCustomNtv2(std::span<const Ntv2SubfileSpec>(&parent_sf, 1), false);
  trunc_ntv2.resize(352);
  mem.AddFile("/mem/trunc_ntv2.gsb", std::move(trunc_ntv2));
  auto trunc_set =
      NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/trunc_ntv2.gsb");
  ASSERT_NE(nullptr, trunc_set);
  EXPECT_FALSE(trunc_set->grids().front()->valueAt(0, 0, true, dlon, dlat));

  // 4. Embedded NTv2 grids.
  ScopedContext embed_ctx = MakeContext();
  auto ntf =
      NS_PROJ::HorizontalShiftGridSet::open(embed_ctx.get(), "ntf_r93.gsb");
  ASSERT_NE(nullptr, ntf);
  EXPECT_EQ("ntv2", ntf->format());
}

TEST(GeoTiffFormatTest, ClassicBigEndianBigTiffAndAllSampleTypes) {
  ScopedContext ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));

  struct TypeCase {
    std::string name;
    TiffSampleType type;
    bool big_endian;
    bool big_tiff;
    double raw_val;
    double expected_scaled;
    double nodata_raw;
  };
  const TypeCase cases[] = {
      {.name = "/mem/i16.tif",
       .type = TiffSampleType::kInt16,
       .big_endian = false,
       .big_tiff = false,
       .raw_val = -20.0,
       .expected_scaled = -20.0 * 0.5 + 10.0,
       .nodata_raw = -32768.0},
      {.name = "/mem/u16_be.tif",
       .type = TiffSampleType::kUInt16,
       .big_endian = true,
       .big_tiff = false,
       .raw_val = 40.0,
       .expected_scaled = 40.0 * 0.5 + 10.0,
       .nodata_raw = 65535.0},
      {.name = "/mem/i32.tif",
       .type = TiffSampleType::kInt32,
       .big_endian = false,
       .big_tiff = false,
       .raw_val = -1000.0,
       .expected_scaled = -1000.0 * 0.5 + 10.0,
       .nodata_raw = -9999.0},
      {.name = "/mem/u32_big.tif",
       .type = TiffSampleType::kUInt32,
       .big_endian = false,
       .big_tiff = true,
       .raw_val = 200.0,
       .expected_scaled = 200.0 * 0.5 + 10.0,
       .nodata_raw = 99999.0},
      {.name = "/mem/f32_big_be.tif",
       .type = TiffSampleType::kFloat32,
       .big_endian = true,
       .big_tiff = true,
       .raw_val = 3.5,
       .expected_scaled = 3.5 * 0.5 + 10.0,
       .nodata_raw = -8888.0},
      {.name = "/mem/f64.tif",
       .type = TiffSampleType::kFloat64,
       .big_endian = false,
       .big_tiff = false,
       .raw_val = -4.0,
       .expected_scaled = -4.0 * 0.5 + 10.0,
       .nodata_raw = -7777.0},
  };

  for (const auto& tc : cases) {
    // Also place scale before offset to hit m_adfOffset init in scale branch.
    const std::string meta =
        "<GDALMetadata>"
        "<Item name=\"OFFSET\" sample=\"0\" role=\"scale\">0.5</Item>"
        "<Item name=\"SCALE\" sample=\"0\" role=\"offset\">10.0</Item>"
        "</GDALMetadata>";
    TiffIfdOptions ifd{
        .width = 2,
        .height = 2,
        .sample_type = tc.type,
        .gdal_metadata = meta,
        .gdal_nodata = std::to_string(tc.nodata_raw),
        .pixels = {tc.raw_val, tc.nodata_raw, tc.raw_val, tc.raw_val},
    };
    mem.AddFile(tc.name,
                BuildCustomTiff(std::span<const TiffIfdOptions>(&ifd, 1),
                                tc.big_endian, tc.big_tiff));

    auto gset = NS_PROJ::GenericShiftGridSet::open(ctx.get(), tc.name);
    ASSERT_NE(nullptr, gset) << tc.name;
    EXPECT_EQ("gtiff", gset->format());
    const auto* grid = gset->gridAt(DegToRad(10.5), DegToRad(49.5));
    ASSERT_NE(nullptr, grid);

    // yFromBottom = 1 maps to TIFF row 0 (top-down): col 0 has raw_val, col 1
    // has nodata.
    float v0 = 0.0f;
    float v1 = 0.0f;
    EXPECT_TRUE(grid->valueAt(0, 1, 0, v0));
    EXPECT_NEAR(tc.expected_scaled, v0, 1e-4) << tc.name;
    // Nodata cell skips scale/offset in readValue<T>.
    EXPECT_TRUE(grid->valueAt(1, 1, 0, v1));
    EXPECT_NEAR(tc.nodata_raw, v1, 1e-1) << tc.name;
  }
}

TEST(GeoTiffFormatTest,
     TiledSeparateMultiIfdMatrixPixelIsAreaAndMetadataEdges) {
  ScopedContext ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogCapture log_cap(ctx.get(), PJ_LOG_TRACE);

  // 1. Tiled 4x4 with 2x2 tiles, PLANARCONFIG_SEPARATE, 2 samples, PixelIsArea,
  //    GeoTransformationMatrix bottom-up, GeoTIFF minor revision trace,
  //    GDAL_METADATA.
  TiffIfdOptions ifd0{
      .width = 4,
      .height = 4,
      .samples_per_pixel = 2,
      .planar_config = 2,  // SEPARATE
      .tile_width = 2,
      .tile_length = 2,
      // No GeoKeyDirectory on IFD 0 triggers "No GeoKeys tag" trace, wait: test
      // PixelIsArea via custom_geokeys on IFD 0 and omit GeoKeys on IFD 1.
      .custom_geokeys = {1, 1, 2, 2, 1024, 0, 1, 2, 1025, 0, 1, 1},
      .use_trans_matrix = true,
      // matrix[5] > 0 => vRes < 0 => bottom-up! west=10, south=40, res=1
      .trans_matrix = {1.0, 0.0, 0.0, 10.0, 0.0, 1.0, 0.0, 40.0, 0.0, 0.0, 0.0,
                       0.0, 0.0, 0.0, 0.0, 1.0},
      .gdal_metadata =
          "<GDALMetadata>"
          "<Item name=\"grid_name\">G0</Item>"
          "<Item name=\"OFFSET\" sample=\"0\" role=\"offset\">1.0</Item>"
          "<Item name=\"SCALE\" sample=\"0\" role=\"scale\">2.0</Item>"
          "</GDALMetadata>",
  };
  ifd0.pixels.resize(4 * 4 * 2);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const size_t idx = static_cast<size_t>(y * 4 + x) * 2;
      ifd0.pixels[idx] = static_cast<double>(y * 10 + x);
      ifd0.pixels[idx + 1] = static_cast<double>(100 + y * 10 + x);
    }
  }

  // IFD 1: multi-strip (RowsPerStrip = 1), no GeoKeys tag ("No GeoKeys tag"
  // trace), same grid_name, invalid float metadata, metadata role/sample edges.
  TiffIfdOptions ifd1{
      .width = 2,
      .height = 2,
      .rows_per_strip = 1,
      .include_geokeys = false,
      .west = 10.5,
      .north = 42.5,
      .res_x = 1.0,
      .res_y = 1.0,
      .gdal_metadata =
          "<GDALMetadata>"
          "<Item name=\"grid_name\">G1</Item>"
          "<Item name=\"parent_grid_name\">G0</Item>"
          "<Item name=\"PLAIN\" role=\"metadata\">v</Item>"
          "<Item name=\"OFF_NOSAMP\" role=\"offset\">1.0</Item>"
          "<Item name=\"SCL_NOSAMP\" role=\"scale\">2.0</Item>"
          "<Item name=\"OFF_OOR\" sample=\"5\" role=\"offset\">1.0</Item>"
          "<Item name=\"SCL_OOR\" sample=\"5\" role=\"scale\">2.0</Item>"
          "<Item name=\"BAD_OFF\" sample=\"0\" role=\"offset\">nan_bad</Item>"
          "<Item name=\"BAD_SCL\" sample=\"0\" role=\"scale\">nan_bad</Item>"
          "</GDALMetadata>",
      .gdal_nodata = "not_a_float",
      .pixels = {7.0, 8.0, 9.0, 10.0},
  };

  const TiffIfdOptions both[2] = {ifd0, ifd1};
  mem.AddFile("/mem/multi.tif", BuildCustomTiff(both));

  auto gset = NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/multi.tif");
  ASSERT_NE(nullptr, gset);
  EXPECT_TRUE(log_cap.ContainsMessage("GeoTIFF 1.2"));
  EXPECT_TRUE(log_cap.ContainsMessage("No GeoKeys tag"));

  // PixelIsArea shifts tie point inward: IFD0 spans [10.5..13.5] x
  // [40.5..43.5].
  const auto* top = gset->gridAt(DegToRad(13.0), DegToRad(40.6));
  ASSERT_NE(nullptr, top);
  EXPECT_THAT(top->name(), HasSubstr("(index 1)"));
  EXPECT_THAT(top->name(), HasSubstr("G0"));

  const auto* child = gset->gridAt(DegToRad(11.0), DegToRad(42.0));
  ASSERT_NE(nullptr, child);
  EXPECT_THAT(child->name(), HasSubstr("(index 2)"));

  // Read IFD 0 (switches TIFF directory back from IFD 1), then IFD 1 strip 1,
  // strip 0, and strip 1 again to exercise BlockCache hit (m_cache.get).
  float val = 0.0f;
  // On bottom-up grid, yFromBottom = 3 is TIFF row 3. Pixel (2, 3) sample 0
  // raw=32 => 32*2+1=65.
  EXPECT_TRUE(top->valueAt(2, 3, 0, val));
  EXPECT_FLOAT_EQ(65.0f, val);
  EXPECT_TRUE(top->valueAt(2, 3, 1, val));
  EXPECT_FLOAT_EQ(132.0f, val);
  EXPECT_TRUE(child->valueAt(1, 0, 0, val));
  EXPECT_FLOAT_EQ(10.0f, val);
  EXPECT_TRUE(child->valueAt(0, 1, 0, val));
  EXPECT_FLOAT_EQ(7.0f, val);
  EXPECT_TRUE(child->valueAt(1, 0, 0, val));
  EXPECT_FLOAT_EQ(10.0f, val);

  // Malformed GDAL_METADATA break branches.
  const char* kMalformedXmls[] = {
      "<GDALMetadata><Item unclosed",
      "<GDALMetadata><Item name=\"a\">val_no_close",
      "<GDALMetadata><Item foo=\"bar\">val</Item>",
      "<GDALMetadata><Item name=\"unclosed>val</Item>",
      "<GDALMetadata><Item name=\"k\" role=\"unclosed>val</Item>",
  };
  for (size_t i = 0; i < std::size(kMalformedXmls); ++i) {
    TiffIfdOptions bad_xml{
        .width = 2,
        .height = 2,
        .gdal_metadata = kMalformedXmls[i],
        .pixels = {1.0, 2.0, 3.0, 4.0},
    };
    const std::string path = "/mem/xml_" + std::to_string(i) + ".tif";
    mem.AddFile(path, BuildSingleTiff(bad_xml));
    auto xset = NS_PROJ::GenericShiftGridSet::open(ctx.get(), path);
    ASSERT_NE(nullptr, xset) << i;
  }

  // RowsPerStrip > height clamping + truncated strip and tile read errors.
  TiffIfdOptions rps_big{
      .width = 2,
      .height = 2,
      .rows_per_strip = 10,
      .pixels = {1.0, 2.0, 3.0, 4.0},
  };
  std::vector<uint8_t> trunc_strip = BuildSingleTiff(rps_big);
  trunc_strip.resize(trunc_strip.size() - 8);
  mem.AddFile("/mem/trunc_strip.tif", std::move(trunc_strip));
  auto ts_set =
      NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/trunc_strip.tif");
  ASSERT_NE(nullptr, ts_set);
  const auto* ts_grid = ts_set->grids().front().get();
  EXPECT_FALSE(ts_grid->valueAt(0, 0, 0, val));
  const int s0[1] = {0};
  bool nodata = false;
  float ts_vals[4] = {};
  EXPECT_FALSE(ts_grid->valuesAt(0, 0, 2, 2, 1, s0, ts_vals, nodata));

  TiffIfdOptions tiled_small{
      .width = 4,
      .height = 4,
      .tile_width = 2,
      .tile_length = 2,
      .pixels = std::vector<double>(16, 1.0),
  };
  std::vector<uint8_t> trunc_tile = BuildSingleTiff(tiled_small);
  trunc_tile.resize(trunc_tile.size() - 56);
  mem.AddFile("/mem/trunc_tile.tif", std::move(trunc_tile));
  auto tt_set =
      NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/trunc_tile.tif");
  ASSERT_NE(nullptr, tt_set);
  const auto* tt_grid = tt_set->grids().front().get();
  EXPECT_FALSE(tt_grid->valueAt(0, 3, 0, val));
  EXPECT_FALSE(tt_grid->valueAt(0, 0, 0, val));
  EXPECT_FALSE(tt_grid->valuesAt(0, 0, 2, 2, 1, s0, ts_vals, nodata));
}

TEST(GeoTiffFastPathTest, Block256ValuesAtFastPathsAndFallbackNodata) {
  ScopedContext ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));

  // 1. 256x256 stripped Float32 PLANARCONFIG_CONTIG with 4 samples (top-down).
  TiffIfdOptions opt_top4{
      .width = 256,
      .height = 256,
      .samples_per_pixel = 4,
      .planar_config = 1,
      .sample_type = TiffSampleType::kFloat32,
      .rows_per_strip = 256,
      .west = 0.0,
      .north = 25.5,
      .res_x = 0.1,
      .res_y = 0.1,
  };
  opt_top4.pixels.resize(size_t{256} * 256 * 4);
  for (int y = 0; y < 256; ++y) {
    for (int x = 0; x < 256; ++x) {
      const size_t base = (static_cast<size_t>(y) * 256 + x) * 4;
      opt_top4.pixels[base + 0] = static_cast<double>(x);
      opt_top4.pixels[base + 1] = static_cast<double>(y + 1000);
      opt_top4.pixels[base + 2] = static_cast<double>(x + y);
      opt_top4.pixels[base + 3] = 99.0;
    }
  }
  mem.AddFile("/mem/fast4.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&opt_top4, 1)));
  auto set4 = NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/fast4.tif");
  ASSERT_NE(nullptr, set4);
  const auto* g4 = set4->grids().front().get();

  // Exercise cases 1, 2, 3 in valuesAt fast path (x_start=2, y_start=3 => top
  // row y=252,251).
  bool nodata = true;
  float out1[4] = {};
  const int s1[1] = {1};
  ASSERT_TRUE(g4->valuesAt(2, 3, 2, 2, 1, s1, out1, nodata));
  EXPECT_FALSE(nodata);
  // y=3 from bottom => top-down row 252; y=4 from bottom => top-down row 251.
  EXPECT_FLOAT_EQ(1252.0f, out1[0]);
  EXPECT_FLOAT_EQ(1251.0f, out1[2]);

  float out2[8] = {};
  const int s2[2] = {1, 2};
  ASSERT_TRUE(g4->valuesAt(2, 3, 2, 2, 2, s2, out2, nodata));
  EXPECT_FLOAT_EQ(1252.0f, out2[0]);
  EXPECT_FLOAT_EQ(254.0f, out2[1]);

  float out3[12] = {};
  const int s3[3] = {1, 2, 3};
  ASSERT_TRUE(g4->valuesAt(2, 3, 2, 2, 3, s3, out3, nodata));
  EXPECT_FLOAT_EQ(99.0f, out3[2]);

  // 2. 256x256 stripped spp=3 (hits sample_count == m_samplesPerPixel fast
  // path),
  //    PLUS tiled 256x256 multi-IFD with bottom-up IFD1 (hits
  //    TIFFReadEncodedTile AND TIFFSetSubDirectory inside valuesAt!).
  TiffIfdOptions opt_spp3{
      .width = 256,
      .height = 256,
      .samples_per_pixel = 3,
      .planar_config = 1,
      .sample_type = TiffSampleType::kFloat32,
      .rows_per_strip = 256,
      .west = 0.0,
      .north = 25.5,
      .res_x = 0.1,
      .res_y = 0.1,
      .pixels = std::vector<double>(size_t{256} * 256 * 3, 5.5),
  };
  // Put unique sentinel at (x=1, yFromBottom=1) => top-down row 254, col 1.
  opt_spp3.pixels[(size_t{254} * 256 + 1) * 3 + 0] = 77.0;
  opt_spp3.pixels[(size_t{254} * 256 + 1) * 3 + 1] = 88.0;
  opt_spp3.pixels[(size_t{254} * 256 + 1) * 3 + 2] = 99.0;

  TiffIfdOptions opt_tiled256_bu{
      .width = 4,
      .height = 4,
      .samples_per_pixel = 3,
      .planar_config = 1,
      .sample_type = TiffSampleType::kFloat32,
      .tile_width = 256,
      .tile_length = 256,
      .use_trans_matrix = true,
      .trans_matrix = {0.1, 0.0, 0.0, 50.0, 0.0, 0.1, 0.0, 1.0, 0.0, 0.0, 0.0,
                       0.0, 0.0, 0.0, 0.0, 1.0},
      .pixels = std::vector<double>(4 * 4 * 3, 3.25),
  };
  // Bottom-up row 1, col 1 => yFromBottom = 1, x = 1.
  opt_tiled256_bu.pixels[(1 * 4 + 1) * 3 + 0] = 11.0;
  opt_tiled256_bu.pixels[(1 * 4 + 1) * 3 + 1] = 22.0;
  opt_tiled256_bu.pixels[(1 * 4 + 1) * 3 + 2] = 33.0;

  const TiffIfdOptions both[2] = {opt_spp3, opt_tiled256_bu};
  mem.AddFile("/mem/fast_multi.tif", BuildCustomTiff(both));
  auto setm =
      NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/fast_multi.tif");
  ASSERT_NE(nullptr, setm);
  ASSERT_EQ(2U, setm->grids().size());
  const int all3[3] = {0, 1, 2};
  float all_out[12] = {};
  // IFD 0 sits at non-current offset since nextGrid moved to IFD 1 -> switches
  // dir.
  ASSERT_TRUE(setm->grids()[0]->valuesAt(1, 1, 2, 2, 3, all3, all_out, nodata));
  EXPECT_FLOAT_EQ(77.0f, all_out[0]);
  EXPECT_FLOAT_EQ(88.0f, all_out[1]);
  EXPECT_FLOAT_EQ(99.0f, all_out[2]);
  // Read bottom-up tiled 256x256 IFD 1 (switches dir & calls
  // TIFFReadEncodedTile).
  ASSERT_TRUE(setm->grids()[1]->valuesAt(1, 1, 2, 2, 3, all3, all_out, nodata));
  EXPECT_FLOAT_EQ(11.0f, all_out[0]);
  EXPECT_FLOAT_EQ(22.0f, all_out[1]);
  EXPECT_FLOAT_EQ(33.0f, all_out[2]);

  // 3. Multi-tile 512x256 (256x256 tiles): fast-path cache hit + cross-tile
  // fallback.
  TiffIfdOptions opt_2tile{
      .width = 512,
      .height = 256,
      .samples_per_pixel = 2,
      .planar_config = 1,
      .sample_type = TiffSampleType::kFloat32,
      .tile_width = 256,
      .tile_length = 256,
      .res_x = 0.1,
      .res_y = 0.1,
  };
  opt_2tile.pixels.resize(size_t{512} * 256 * 2);
  for (int y = 0; y < 256; ++y) {
    for (int x = 0; x < 512; ++x) {
      const size_t base = (static_cast<size_t>(y) * 512 + x) * 2;
      opt_2tile.pixels[base + 0] = static_cast<double>(x + 1000 * y);
      opt_2tile.pixels[base + 1] = -static_cast<double>(x);
    }
  }
  mem.AddFile("/mem/fast_2tile.tif", BuildSingleTiff(opt_2tile));
  auto set2t =
      NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/fast_2tile.tif");
  ASSERT_NE(nullptr, set2t);
  const auto* g2t = set2t->grids().front().get();
  const int s0[1] = {0};
  float t_out[4] = {};
  // Read tile 0, then tile 1, then tile 0 again (hits m_cache.get in fast
  // path).
  ASSERT_TRUE(g2t->valuesAt(0, 1, 2, 2, 1, s0, t_out, nodata));
  EXPECT_FLOAT_EQ(254000.0f, t_out[0]);
  ASSERT_TRUE(g2t->valuesAt(256, 1, 2, 2, 1, s0, t_out, nodata));
  EXPECT_FLOAT_EQ(254256.0f, t_out[0]);
  ASSERT_TRUE(g2t->valuesAt(0, 1, 2, 2, 1, s0, t_out, nodata));
  EXPECT_FLOAT_EQ(254000.0f, t_out[0]);
  // Cross tile boundary (x=255..256) to run fallback path without nodata.
  nodata = true;
  ASSERT_TRUE(g2t->valuesAt(255, 1, 2, 2, 1, s0, t_out, nodata));
  EXPECT_FALSE(nodata);
  EXPECT_FLOAT_EQ(254255.0f, t_out[0]);
  EXPECT_FLOAT_EQ(254256.0f, t_out[1]);
  EXPECT_FLOAT_EQ(253255.0f, t_out[2]);
  EXPECT_FLOAT_EQ(253256.0f, t_out[3]);

  // 4. Truncated 256x256 stripped & tiled fast-path read errors.
  TiffIfdOptions fast_trunc_strip{
      .width = 256,
      .height = 256,
      .rows_per_strip = 256,
      .res_x = 0.1,
      .res_y = 0.1,
      .pixels = std::vector<double>(size_t{256} * 256, 1.0),
  };
  std::vector<uint8_t> ft_strip_bytes = BuildSingleTiff(fast_trunc_strip);
  ft_strip_bytes.resize(ft_strip_bytes.size() - 16);
  mem.AddFile("/mem/ft_strip.tif", std::move(ft_strip_bytes));
  auto fts = NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/ft_strip.tif");
  ASSERT_NE(nullptr, fts);
  float ft_val = 0.0f;
  EXPECT_FALSE(
      fts->grids().front()->valuesAt(0, 0, 1, 1, 1, s0, &ft_val, nodata));

  TiffIfdOptions fast_trunc_tile{
      .width = 256,
      .height = 256,
      .tile_width = 256,
      .tile_length = 256,
      .res_x = 0.1,
      .res_y = 0.1,
      .pixels = std::vector<double>(size_t{256} * 256, 1.0),
  };
  std::vector<uint8_t> ft_tile_bytes = BuildSingleTiff(fast_trunc_tile);
  ft_tile_bytes.resize(ft_tile_bytes.size() - 16);
  mem.AddFile("/mem/ft_tile.tif", std::move(ft_tile_bytes));
  auto ftt = NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/ft_tile.tif");
  ASSERT_NE(nullptr, ftt);
  EXPECT_FALSE(
      ftt->grids().front()->valuesAt(0, 0, 1, 1, 1, s0, &ft_val, nodata));

  // 5. Fallback valuesAt with explicit GDAL_NODATA and NaN pixel.
  TiffIfdOptions fallback_nan{
      .width = 2,
      .height = 2,
      .gdal_nodata = "-9999",
      .pixels = {1.0, -9999.0, std::numeric_limits<double>::quiet_NaN(), 4.0},
  };
  mem.AddFile("/mem/fb_nan.tif", BuildSingleTiff(fallback_nan));
  auto fb_set =
      NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/fb_nan.tif");
  ASSERT_NE(nullptr, fb_set);
  float fb_out[4] = {};
  ASSERT_TRUE(
      fb_set->grids().front()->valuesAt(0, 0, 2, 2, 1, s0, fb_out, nodata));
  EXPECT_TRUE(nodata);
}

TEST(GeoTiffDatasetValidationTest, AllHeaderAndGeoreferencingFailures) {
  ScopedContext ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogSuppressor quiet(ctx.get());

  // 1. Corrupt TIFF header passed to GTiffVGridShiftSet::open indirectly.
  mem.AddTextFile("/mem/corrupt.tif", "II*\x00not_valid_tiff");
  EXPECT_EQ(nullptr,
            NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "/mem/corrupt.tif"));

  std::vector<TiffIfdOptions> bad_tiffs;
  // Invalid image dimensions (0, > INT_MAX, and missing dimension tags).
  bad_tiffs.push_back(TiffIfdOptions{.width = 0, .height = 2});
  bad_tiffs.push_back(TiffIfdOptions{
      .width = 0x80000001u,
      .height = 1,
      .rows_per_strip = 1,
  });
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .include_dimensions = false});
  // Missing and invalid (0) SamplesPerPixel.
  bad_tiffs.push_back(TiffIfdOptions{
      .width = 2, .height = 2, .samples_per_pixel = std::nullopt});
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .samples_per_pixel = 0});
  // Missing BitsPerSample.
  bad_tiffs.push_back(TiffIfdOptions{
      .width = 2, .height = 2, .include_bits_per_sample = false});
  // Missing SampleFormat.
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .include_sample_format = false});
  // Unsupported SampleFormat + BitsPerSample (UInt8).
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2,
                     .height = 2,
                     .sample_type = TiffSampleType::kUnsupportedUInt8});
  // Unsupported Photometric (RGB = 2).
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .photometric = 2});
  // Missing codec (compression = 9999) and OJPEG (compression = 6).
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .compression = 9999});
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .compression = 6});
  // BlockSize > 64*1024*2014 bytes without needing pixel data allocation.
  bad_tiffs.push_back(TiffIfdOptions{
      .width = 20000000,
      .height = 1,
      .sample_type = TiffSampleType::kFloat64,
      .compression = 8,
      .rows_per_strip = 1,
  });
  // Bad GeoKeys count (< 4 and non-multiple of 4), bad major version, bad model
  // type.
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .custom_geokeys = {1, 1}});
  bad_tiffs.push_back(TiffIfdOptions{
      .width = 2, .height = 2, .custom_geokeys = {1, 1, 0, 1, 1024}});
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .custom_geokeys = {2, 1, 0, 0}});
  bad_tiffs.push_back(TiffIfdOptions{.width = 2, .height = 2, .model_type = 3});
  // Rotational GeoTransformationMatrix.
  bad_tiffs.push_back(TiffIfdOptions{
      .width = 2,
      .height = 2,
      .use_trans_matrix = true,
      .trans_matrix = {1.0, 0.2, 0.0, 10.0, 0.0, -1.0, 0.0, 50.0, 0.0, 0.0, 0.0,
                       0.0, 0.0, 0.0, 0.0, 1.0},
  });
  // Missing / wrong count GeoPixelScale and GeoTiePoints.
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .include_pixel_scale = false});
  bad_tiffs.push_back(TiffIfdOptions{
      .width = 2, .height = 2, .custom_pixel_scale = {1.0, 1.0}});
  bad_tiffs.push_back(
      TiffIfdOptions{.width = 2, .height = 2, .include_tie_points = false});
  bad_tiffs.push_back(TiffIfdOptions{
      .width = 2, .height = 2, .custom_tie_points = {0.0, 0.0, 0.0}});
  // Inconsistent georeferencing (negative resX => west > east; huge lat > 180).
  bad_tiffs.push_back(TiffIfdOptions{.width = 2, .height = 2, .res_x = -1.0});
  bad_tiffs.push_back(TiffIfdOptions{.width = 2, .height = 2, .north = 200.0});

  for (size_t i = 0; i < bad_tiffs.size(); ++i) {
    const std::string path = "/mem/bad_gtiff_" + std::to_string(i) + ".tif";
    mem.AddFile(path, BuildCustomTiff(
                          std::span<const TiffIfdOptions>(&bad_tiffs[i], 1)));
    EXPECT_EQ(nullptr, NS_PROJ::GenericShiftGridSet::open(ctx.get(), path))
        << "Case " << i;
  }

  // Omitted Photometric and Compression tags default cleanly to MINISBLACK /
  // NONE.
  TiffIfdOptions omitted_defaults{
      .width = 2,
      .height = 2,
      .photometric = std::nullopt,
      .compression = std::nullopt,
      .pixels = {1.0, 2.0, 3.0, 4.0},
  };
  mem.AddFile("/mem/defaults_gtiff.tif", BuildSingleTiff(omitted_defaults));
  ASSERT_NE(nullptr, NS_PROJ::GenericShiftGridSet::open(
                         ctx.get(), "/mem/defaults_gtiff.tif"));
}

TEST(GeoTiffGridSetsTest, ChannelDetectionHierarchiesReopenAndReassign) {
  ScopedContext ctx = MakeContext();
  ScopedContext other_ctx = MakeContext();
  InMemoryFileSystem mem;
  InMemoryFileSystem empty_mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogCapture log_cap(ctx.get(), PJ_LOG_DEBUG);

  // 1. Vertical descriptions + subfileType filtering + named hierarchy.
  const std::string_view vdescs[] = {"geoid_undulation", "vertical_offset",
                                     "hydroid_height",
                                     "ellipsoidal_height_offset"};
  for (size_t i = 0; i < std::size(vdescs); ++i) {
    TiffIfdOptions opt{
        .width = 2,
        .height = 2,
        .samples_per_pixel = 2,
        .gdal_metadata =
            "<GDALMetadata><Item name=\"DESCRIPTION\" sample=\"1\">" +
            std::string(vdescs[i]) + "</Item></GDALMetadata>",
        .pixels = {0.0, 1.5, 0.0, 2.5, 0.0, 3.5, 0.0, 4.5},
    };
    const std::string path = "/mem/vd_" + std::to_string(i) + ".tif";
    mem.AddFile(path,
                BuildCustomTiff(std::span<const TiffIfdOptions>(&opt, 1)));
    auto vset = NS_PROJ::VerticalShiftGridSet::open(ctx.get(), path);
    ASSERT_NE(nullptr, vset);
    float v = 0.0f;
    EXPECT_TRUE(vset->grids().front()->valueAt(0, 1, v));
    EXPECT_FLOAT_EQ(1.5f, v);
  }

  // Unsupported subfileType / description on IFD 0 fails; on IFD 1 skips.
  TiffIfdOptions v_bad_subfile{
      .width = 2, .height = 2, .subfile_type = 1, .pixels = {1, 2, 3, 4}};
  mem.AddFile(
      "/mem/v_bad_sf.tif",
      BuildCustomTiff(std::span<const TiffIfdOptions>(&v_bad_subfile, 1)));
  EXPECT_EQ(nullptr, NS_PROJ::VerticalShiftGridSet::open(ctx.get(),
                                                         "/mem/v_bad_sf.tif"));

  TiffIfdOptions v_bad_desc{
      .width = 2,
      .height = 2,
      .gdal_metadata =
          "<GDALMetadata><Item name=\"DESCRIPTION\" "
          "sample=\"0\">accuracy</Item></GDALMetadata>",
      .pixels = {1, 2, 3, 4},
  };
  mem.AddFile("/mem/v_bad_desc.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&v_bad_desc, 1)));
  EXPECT_EQ(nullptr, NS_PROJ::VerticalShiftGridSet::open(
                         ctx.get(), "/mem/v_bad_desc.tif"));

  // Multi-IFD named hierarchy (covering duplicate name, missing parent,
  // uncontained parent, TYPE mismatch, partial intersection) + IFD 1 bad
  // subfileType/desc skipped.
  auto make_v_ifd = [](double west, double north, int w, int h,
                       std::string meta,
                       std::optional<uint32_t> sf = std::nullopt) {
    return TiffIfdOptions{
        .width = static_cast<uint32_t>(w),
        .height = static_cast<uint32_t>(h),
        .subfile_type = sf,
        .west = west,
        .north = north,
        .res_x = 1.0,
        .res_y = 1.0,
        .gdal_metadata = std::move(meta),
        .pixels = std::vector<double>(static_cast<size_t>(w) * h, 1.0),
    };
  };
  const std::vector<TiffIfdOptions> v_named = {
      make_v_ifd(0, 16, 17, 17,
                 "<GDALMetadata><Item name=\"grid_name\">R</Item>"
                 "<Item name=\"TYPE\">T1</Item></GDALMetadata>"),
      make_v_ifd(2, 10, 9, 9,
                 "<GDALMetadata><Item name=\"grid_name\">C</Item>"
                 "<Item name=\"parent_grid_name\">R</Item></GDALMetadata>"),
      make_v_ifd(1, 5, 5, 5,
                 "<GDALMetadata><Item name=\"grid_name\">C</Item>"
                 "<Item name=\"parent_grid_name\">NOPE</Item>"
                 "<Item name=\"TYPE\">T2</Item></GDALMetadata>"),
      make_v_ifd(12, 15, 4, 4,
                 "<GDALMetadata><Item name=\"grid_name\">BC</Item>"
                 "<Item name=\"parent_grid_name\">C</Item>"
                 "<Item name=\"TYPE\">T2</Item></GDALMetadata>"),
      make_v_ifd(8, 20, 13, 13, ""),
      make_v_ifd(0, 1, 2, 2, "", /*sf=*/1),
      make_v_ifd(0, 1, 2, 2,
                 "<GDALMetadata><Item name=\"DESCRIPTION\" "
                 "sample=\"0\">other</Item></GDALMetadata>"),
  };
  mem.AddFile("/mem/v_named.tif", BuildCustomTiff(v_named));
  auto v_named_set =
      NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "/mem/v_named.tif");
  ASSERT_NE(nullptr, v_named_set);
  EXPECT_TRUE(log_cap.ContainsMessage("Several grids called C"));
  EXPECT_TRUE(log_cap.ContainsMessage("non-existing parent NOPE"));
  EXPECT_TRUE(log_cap.ContainsMessage("not included in it"));
  EXPECT_TRUE(log_cap.ContainsMessage("Partially intersecting grids found"));
  EXPECT_TRUE(log_cap.ContainsMessage("Ignoring IFD 5"));
  EXPECT_TRUE(log_cap.ContainsMessage("Ignoring IFD 6"));

  // 2. Bbox 3-level hierarchy (Root -> Child1 -> Grandchild + partially
  //    intersecting Child2) for Vertical, Horizontal, AND Generic!
  auto make_bbox_ifd = [](double west, double north, int w, int h, int spp,
                          std::string meta = {}) {
    return TiffIfdOptions{
        .width = static_cast<uint32_t>(w),
        .height = static_cast<uint32_t>(h),
        .samples_per_pixel = static_cast<uint16_t>(spp),
        .west = west,
        .north = north,
        .res_x = 1.0,
        .res_y = 1.0,
        .gdal_metadata = std::move(meta),
        .pixels = std::vector<double>(static_cast<size_t>(w) * h * spp, 2.0),
    };
  };
  const std::vector<TiffIfdOptions> bbox_v = {
      make_bbox_ifd(0, 16, 17, 17, 1),
      make_bbox_ifd(2, 10, 9, 9, 1),
      make_bbox_ifd(4, 6, 3, 3, 1),
      make_bbox_ifd(6, 14, 9, 9, 1),
  };
  const std::vector<TiffIfdOptions> bbox_hg = {
      make_bbox_ifd(
          0, 16, 17, 17, 2,
          "<GDALMetadata><Item name=\"TYPE\">VEL</Item></GDALMetadata>"),
      make_bbox_ifd(2, 10, 9, 9, 2),
      make_bbox_ifd(4, 6, 3, 3, 2),
      make_bbox_ifd(6, 14, 9, 9, 2),
  };
  mem.AddFile("/mem/bbox_v.tif", BuildCustomTiff(bbox_v));
  mem.AddFile("/mem/bbox_hg.tif", BuildCustomTiff(bbox_hg));

  auto bv = NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "/mem/bbox_v.tif");
  auto bh =
      NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/bbox_hg.tif");
  auto bg = NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/bbox_hg.tif");
  ASSERT_NE(nullptr, bv);
  ASSERT_NE(nullptr, bh);
  ASSERT_NE(nullptr, bg);

  const auto* bv_child = bv->gridAt(DegToRad(5.0), DegToRad(5.0));
  ASSERT_NE(nullptr, bv_child);
  EXPECT_THAT(bv_child->name(), HasSubstr("(index 3)"));
  EXPECT_FALSE(bv_child->hasChanged());
  EXPECT_TRUE(bv_child->metadataItem("missing").empty());
  const auto* bh_child = bh->gridAt(DegToRad(5.0), DegToRad(5.0));
  ASSERT_NE(nullptr, bh_child);
  EXPECT_THAT(bh_child->name(), HasSubstr("(index 3)"));
  EXPECT_FALSE(bh_child->hasChanged());
  EXPECT_TRUE(bh_child->metadataItem("missing").empty());
  const auto* bg_child = bg->gridAt(DegToRad(5.0), DegToRad(5.0));
  ASSERT_NE(nullptr, bg_child);
  EXPECT_THAT(bg_child->name(), HasSubstr("(index 3)"));
  // Inherited metadata from firstGrid when IFD 2 lacked TYPE!
  EXPECT_EQ("VEL", bg_child->type());
  EXPECT_EQ(bg_child, bg->gridAt("VEL", DegToRad(5.0), DegToRad(5.0)));
  EXPECT_EQ(nullptr, bg->gridAt("OTHER", DegToRad(5.0), DegToRad(5.0)));
  EXPECT_EQ(2, bg_child->samplesPerPixel());
  EXPECT_TRUE(bg_child->unit(0).empty());
  EXPECT_TRUE(bg_child->description(0).empty());
  EXPECT_FALSE(bg_child->hasChanged());
  float dummy = 0.0f;
  EXPECT_FALSE(bg_child->valueAt(0, 0, -1, dummy));
  EXPECT_FALSE(bg_child->valueAt(0, 0, 2, dummy));
  EXPECT_TRUE(bg_child->valueAt(0, 0, 0, dummy));

  // Test reassign_context and reopen (success + corrupt + missing file) on all
  // 3!
  bv->reassign_context(other_ctx.get());
  bh->reassign_context(other_ctx.get());
  bg->reassign_context(other_ctx.get());
  bv->reassign_context(ctx.get());
  bh->reassign_context(ctx.get());
  bg->reassign_context(ctx.get());

  EXPECT_TRUE(bv->reopen(ctx.get()));
  EXPECT_TRUE(bh->reopen(ctx.get()));
  EXPECT_TRUE(bg->reopen(ctx.get()));

  mem.AddTextFile("/mem/bbox_v.tif", "II*\x00broken");
  mem.AddTextFile("/mem/bbox_hg.tif", "II*\x00broken");
  EXPECT_FALSE(bv->reopen(ctx.get()));
  EXPECT_FALSE(bh->reopen(ctx.get()));
  EXPECT_FALSE(bg->reopen(ctx.get()));

  // Non-TIFF VerticalShiftGridSet::reopen and HorizontalShiftGridSet::reopen.
  mem.AddFile("/mem/reopen.gtx",
              BuildGtxGrid(0, 0, 1, 1, 2, 2, std::vector<float>(4, 1.0f)));
  auto v_gtx =
      NS_PROJ::VerticalShiftGridSet::open(ctx.get(), "/mem/reopen.gtx");
  ASSERT_NE(nullptr, v_gtx);
  EXPECT_TRUE(v_gtx->reopen(ctx.get()));

  mem.AddFile("/mem/reopen.ct2", BuildCTable2Grid(0, 0, 0.1, 0.1, 2, 2,
                                                  std::vector<float>(8, 0.0f)));
  auto h_ct2 =
      NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/reopen.ct2");
  ASSERT_NE(nullptr, h_ct2);
  EXPECT_TRUE(h_ct2->reopen(ctx.get()));

  // Switch to empty filesystem and verify reopen missing-file failure paths.
  ASSERT_TRUE(empty_mem.Install(ctx.get()));
  EXPECT_FALSE(bv->reopen(ctx.get()));
  EXPECT_FALSE(bh->reopen(ctx.get()));
  EXPECT_FALSE(bg->reopen(ctx.get()));
  EXPECT_FALSE(v_gtx->reopen(ctx.get()));
  EXPECT_FALSE(h_ct2->reopen(ctx.get()));
  bv.reset();
  bh.reset();
  bg.reset();
  v_gtx.reset();
  h_ct2.reset();

  // 3. Horizontal-specific GTiff metadata, units, positive_value, and errors.
  ASSERT_TRUE(mem.Install(ctx.get()));
  struct HCase {
    std::string name;
    std::string meta;
    double expected_dlon_rad;
    double expected_dlat_rad;
  };
  const HCase hcases[] = {
      {.name = "/mem/h_west.tif",
       .meta =
           "<GDALMetadata>"
           "<Item name=\"DESCRIPTION\" sample=\"0\">longitude_offset</Item>"
           "<Item name=\"DESCRIPTION\" sample=\"1\">latitude_offset</Item>"
           "<Item name=\"positive_value\" sample=\"0\">west</Item>"
           "<Item name=\"UNITTYPE\" sample=\"0\">arc-seconds per year</Item>"
           "<Item name=\"UNITTYPE\" sample=\"1\">arc-seconds per year</Item>"
           "</GDALMetadata>",
       .expected_dlon_rad = -1.0 * kArcsecToRad,
       .expected_dlat_rad = 2.0 * kArcsecToRad},
      {.name = "/mem/h_arcsec.tif",
       .meta = "<GDALMetadata>"
               "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
               "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
               "<Item name=\"UNITTYPE\" sample=\"0\">arc-second</Item>"
               "<Item name=\"UNITTYPE\" sample=\"1\">arc-second</Item>"
               "</GDALMetadata>",
       .expected_dlon_rad = 2.0 * kArcsecToRad,
       .expected_dlat_rad = 1.0 * kArcsecToRad},
      {.name = "/mem/h_rad.tif",
       .meta = "<GDALMetadata>"
               "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
               "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
               "<Item name=\"positive_value\" sample=\"1\">east</Item>"
               "<Item name=\"UNITTYPE\" sample=\"0\">radian</Item>"
               "<Item name=\"UNITTYPE\" sample=\"1\">radian</Item>"
               "</GDALMetadata>",
       .expected_dlon_rad = 2.0,
       .expected_dlat_rad = 1.0},
      {.name = "/mem/h_deg.tif",
       .meta = "<GDALMetadata>"
               "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
               "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
               "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
               "<Item name=\"UNITTYPE\" sample=\"1\">degree</Item>"
               "</GDALMetadata>",
       .expected_dlon_rad = DegToRad(2.0),
       .expected_dlat_rad = DegToRad(1.0)},
  };
  for (const auto& hc : hcases) {
    TiffIfdOptions opt{
        .width = 2,
        .height = 2,
        .samples_per_pixel = 2,
        .gdal_metadata = hc.meta,
        .pixels = {1.0, 2.0, 1.0, 2.0, 1.0, 2.0, 1.0, 2.0},
    };
    mem.AddFile(hc.name,
                BuildCustomTiff(std::span<const TiffIfdOptions>(&opt, 1)));
    auto hset = NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), hc.name);
    ASSERT_NE(nullptr, hset) << hc.name;
    float lon_s = 0.0f;
    float lat_s = 0.0f;
    EXPECT_TRUE(hset->grids().front()->valueAt(0, 0, true, lon_s, lat_s));
    EXPECT_NEAR(hc.expected_dlon_rad, lon_s, 1e-6) << hc.name;
    EXPECT_NEAR(hc.expected_dlat_rad, lat_s, 1e-6) << hc.name;
  }

  // Horizontal & Generic multi-IFD skip branches + IFD 0 errors.
  TiffIfdOptions h_ifd1_skip_1band{
      .width = 2, .height = 2, .samples_per_pixel = 1};
  TiffIfdOptions h_ifd1_skip_sf{
      .width = 2, .height = 2, .samples_per_pixel = 2, .subfile_type = 1};
  TiffIfdOptions h_ifd1_skip_desc{
      .width = 2,
      .height = 2,
      .samples_per_pixel = 2,
      .gdal_metadata =
          "<GDALMetadata><Item name=\"DESCRIPTION\" "
          "sample=\"0\">acc</Item></GDALMetadata>",
  };
  TiffIfdOptions h_ok{
      .width = 2,
      .height = 2,
      .samples_per_pixel = 2,
      .pixels = std::vector<double>(8, 0.0),
  };
  const TiffIfdOptions h_multi_skips[4] = {h_ok, h_ifd1_skip_sf,
                                           h_ifd1_skip_1band, h_ifd1_skip_desc};
  mem.AddFile("/mem/h_skips.tif", BuildCustomTiff(h_multi_skips));
  ASSERT_NE(nullptr, NS_PROJ::HorizontalShiftGridSet::open(ctx.get(),
                                                           "/mem/h_skips.tif"));

  const TiffIfdOptions g_multi_skips[2] = {h_ok, h_ifd1_skip_sf};
  mem.AddFile("/mem/g_skips.tif", BuildCustomTiff(g_multi_skips));
  ASSERT_NE(nullptr,
            NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/g_skips.tif"));

  // Horizontal & Generic IFD 0 errors.
  const std::vector<TiffIfdOptions> h_err_cases = {
      TiffIfdOptions{
          .width = 2, .height = 2, .samples_per_pixel = 2, .subfile_type = 1},
      TiffIfdOptions{.width = 2, .height = 2, .samples_per_pixel = 1},
      TiffIfdOptions{
          .width = 2,
          .height = 2,
          .samples_per_pixel = 2,
          .gdal_metadata = "<GDALMetadata><Item name=\"DESCRIPTION\" "
                           "sample=\"0\">other</Item></GDALMetadata>",
      },
      TiffIfdOptions{
          .width = 2,
          .height = 2,
          .samples_per_pixel = 2,
          .gdal_metadata = "<GDALMetadata><Item name=\"DESCRIPTION\" "
                           "sample=\"0\">latitude_offset</Item></GDALMetadata>",
      },
      TiffIfdOptions{
          .width = 2,
          .height = 2,
          .samples_per_pixel = 2,
          .gdal_metadata =
              "<GDALMetadata><Item name=\"DESCRIPTION\" "
              "sample=\"0\">longitude_offset</Item></GDALMetadata>",
      },
      TiffIfdOptions{
          .width = 2,
          .height = 2,
          .samples_per_pixel = 2,
          .gdal_metadata =
              "<GDALMetadata>"
              "<Item name=\"DESCRIPTION\" sample=\"0\">latitude_offset</Item>"
              "<Item name=\"DESCRIPTION\" sample=\"1\">longitude_offset</Item>"
              "<Item name=\"positive_value\" sample=\"1\">north</Item>"
              "</GDALMetadata>",
      },
      TiffIfdOptions{
          .width = 2,
          .height = 2,
          .samples_per_pixel = 2,
          .gdal_metadata = "<GDALMetadata>"
                           "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
                           "<Item name=\"UNITTYPE\" sample=\"1\">radian</Item>"
                           "</GDALMetadata>",
      },
      TiffIfdOptions{
          .width = 2,
          .height = 2,
          .samples_per_pixel = 2,
          .gdal_metadata = "<GDALMetadata>"
                           "<Item name=\"UNITTYPE\" sample=\"0\">furlong</Item>"
                           "<Item name=\"UNITTYPE\" sample=\"1\">furlong</Item>"
                           "</GDALMetadata>",
      },
  };
  for (size_t i = 0; i < h_err_cases.size(); ++i) {
    const std::string p = "/mem/herr_" + std::to_string(i) + ".tif";
    mem.AddFile(p, BuildCustomTiff(
                       std::span<const TiffIfdOptions>(&h_err_cases[i], 1)));
    EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), p))
        << i;
  }

  std::vector<uint8_t> trunc_h_bytes = BuildSingleTiff(h_ok);
  trunc_h_bytes.resize(trunc_h_bytes.size() - 8);
  mem.AddFile("/mem/trunc_h.tif", std::move(trunc_h_bytes));
  auto trunc_h =
      NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), "/mem/trunc_h.tif");
  ASSERT_NE(nullptr, trunc_h);
  float trunc_dlon = 0.0f;
  float trunc_dlat = 0.0f;
  EXPECT_FALSE(
      trunc_h->grids().front()->valueAt(0, 0, true, trunc_dlon, trunc_dlat));

  mem.AddFile("/mem/g_bad_sf.tif", BuildSingleTiff(v_bad_subfile));
  EXPECT_EQ(nullptr,
            NS_PROJ::GenericShiftGridSet::open(ctx.get(), "/mem/g_bad_sf.tif"));
}

TEST(OpenErrorsTest, MissingShortAndUnrecognizedGridFormats) {
  ScopedContext ctx = MakeContext();
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogSuppressor quiet(ctx.get());

  mem.AddTextFile("/mem/tiny", "AB");
  mem.AddTextFile("/mem/short_unknown", "NOT_A_KNOWN_HEADER");
  mem.AddTextFile("/mem/long_unknown", std::string(200, 'Z'));

  for (const char* path : {"/mem/missing", "/mem/tiny", "/mem/long_unknown"}) {
    EXPECT_EQ(nullptr, NS_PROJ::VerticalShiftGridSet::open(ctx.get(), path));
    EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(ctx.get(), path));
    EXPECT_EQ(nullptr, NS_PROJ::GenericShiftGridSet::open(ctx.get(), path));
  }
  EXPECT_EQ(nullptr, NS_PROJ::HorizontalShiftGridSet::open(
                         ctx.get(), "/mem/short_unknown"));
}

TEST(GridInitAndInterpolationTest, InitHGridVGridAndGenericGridFunctions) {
  ScopedContext ctx = MakeContext();
  absl::Cleanup cleanup_caches = []() { proj_cleanup(); };
  InMemoryFileSystem mem;
  ASSERT_TRUE(mem.Install(ctx.get()));
  ScopedLogCapture log_cap(ctx.get(), PJ_LOG_TRACE);

  // Build test grids:
  // 1) H-grid in degrees (positive east) for [10, 12] x [50, 52], shift (+0.25,
  // +0.1) deg.
  TiffIfdOptions h_std{
      .width = 3,
      .height = 3,
      .samples_per_pixel = 2,
      .west = 10.0,
      .north = 52.0,
      .res_x = 1.0,
      .res_y = 1.0,
      .gdal_metadata =
          "<GDALMetadata>"
          "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
          "<Item name=\"UNITTYPE\" sample=\"1\">degree</Item>"
          "</GDALMetadata>",
  };
  for (int i = 0; i < 9; ++i) {
    h_std.pixels.push_back(0.1);   // lat offset deg
    h_std.pixels.push_back(0.25);  // lon offset deg
  }
  mem.AddFile("/mem/h_std.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&h_std, 1)));

  // 2) Two grids to trigger inverse grid switching: gridA [10..12] -> gridB
  // [9..10].
  auto make_switch_grid = [](double west, double north, int w, int h,
                             double lon_shift_deg) {
    TiffIfdOptions opt{
        .width = static_cast<uint32_t>(w),
        .height = static_cast<uint32_t>(h),
        .samples_per_pixel = 2,
        .west = west,
        .north = north,
        .res_x = 1.0,
        .res_y = 1.0,
        .gdal_metadata =
            "<GDALMetadata>"
            "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
            "<Item name=\"UNITTYPE\" sample=\"1\">degree</Item>"
            "</GDALMetadata>",
    };
    for (int i = 0; i < w * h; ++i) {
      opt.pixels.push_back(0.0);
      opt.pixels.push_back(lon_shift_deg);
    }
    return opt;
  };
  const TiffIfdOptions sw_a = make_switch_grid(10.0, 12.0, 3, 3, 0.5);
  const TiffIfdOptions sw_b = make_switch_grid(9.0, 12.0, 2, 3, 0.5);
  mem.AddFile("/mem/sw_a.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&sw_a, 1)));
  mem.AddFile("/mem/sw_b.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&sw_b, 1)));

  // 3) Oscillatory H-grid forcing non-convergence (MAX_ITERATIONS exhausted).
  TiffIfdOptions osc_grid{
      .width = 5,
      .height = 3,
      .samples_per_pixel = 2,
      .west = 0.0,
      .north = 2.0,
      .res_x = 1.0,
      .res_y = 1.0,
      .gdal_metadata =
          "<GDALMetadata>"
          "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
          "<Item name=\"UNITTYPE\" sample=\"1\">degree</Item>"
          "</GDALMetadata>",
  };
  const double osc_lon[5] = {0.0, 0.0, 1.5, 0.0, 0.0};
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 5; ++x) {
      osc_grid.pixels.push_back(0.0);
      osc_grid.pixels.push_back(osc_lon[x]);
    }
  }
  mem.AddFile("/mem/osc.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&osc_grid, 1)));

  // 4) Edge-break H-grid: shift +1.0 deg on [10, 12] so inverse at 10.5 steps
  // to 9.5.
  const TiffIfdOptions edge_brk = make_switch_grid(10.0, 12.0, 3, 3, 1.0);
  mem.AddFile("/mem/edge_brk.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&edge_brk, 1)));

  // 5) Projected (non-geographic) grid for CRS error paths in
  // vgrid/hgrid/generic.
  TiffIfdOptions proj_grid{
      .width = 2,
      .height = 2,
      .samples_per_pixel = 3,
      .model_type = 1,  // ModelTypeProjected
      .west = 1000.0,
      .north = 2000.0,
      .res_x = 1000.0,
      .res_y = 1000.0,
      .pixels = std::vector<double>(12, 1.0),
  };
  mem.AddFile("/mem/proj.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&proj_grid, 1)));

  // 6) Vertical GTX grids: normal + nodata combinations + full world longitude.
  // 4 cols x 3 rows covering [0..3] x [0..2]:
  //   (x=0,y=0)=1, (x=1,y=0)=ND, (x=2,y=0)=ND, (x=3,y=0)=2
  //   (x=0,y=1)=ND,(x=1,y=1)=3,  (x=2,y=1)=4,  (x=3,y=1)=ND
  //   (x=0,y=2)=ND,(x=1,y=2)=ND, (x=2,y=2)=ND, (x=3,y=2)=ND
  constexpr float kND = -88.88880f;
  const std::vector<float> v_nodata_cells = {
      1.0f, kND,  kND,  2.0f,  // y=0
      kND,  3.0f, 4.0f, kND,   // y=1
      kND,  kND,  kND,  kND,   // y=2
  };
  mem.AddFile("/mem/v_nd.gtx",
              BuildGtxGrid(0.0, 0.0, 1.0, 1.0, 3, 4, v_nodata_cells));

  const std::vector<float> v_world_cells = {
      10.0f, 20.0f, 30.0f, 40.0f,  // y=0 (-90)
      10.0f, 20.0f, 30.0f, 40.0f,  // y=1 (0)
      10.0f, 20.0f, 30.0f, 40.0f,  // y=2 (+90)
  };
  mem.AddFile("/mem/v_world.gtx",
              BuildGtxGrid(-180.0, -90.0, 90.0, 90.0, 3, 4, v_world_cells));

  // Test pj_*_init with missing key, @optional, and required missing.
  ScopedPj p_nokey(pj_create_internal(ctx.get(), "+proj=latlong +ellps=WGS84"));
  ASSERT_NE(nullptr, p_nokey.get());
  EXPECT_TRUE(NS_PROJ::pj_vgrid_init(p_nokey.get(), "grids").empty());
  EXPECT_TRUE(NS_PROJ::pj_hgrid_init(p_nokey.get(), "grids").empty());
  EXPECT_TRUE(NS_PROJ::pj_generic_grid_init(p_nokey.get(), "grids").empty());

  ScopedPj p_req_miss(pj_create_internal(
      ctx.get(), "+proj=latlong +ellps=WGS84 +grids=/mem/nope"));
  ASSERT_NE(nullptr, p_req_miss.get());
  EXPECT_TRUE(NS_PROJ::pj_vgrid_init(p_req_miss.get(), "grids").empty());
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_TRUE(NS_PROJ::pj_hgrid_init(p_req_miss.get(), "grids").empty());
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_TRUE(NS_PROJ::pj_generic_grid_init(p_req_miss.get(), "grids").empty());
  proj_context_errno_set(ctx.get(), 0);

  ScopedPj p_opt(pj_create_internal(
      ctx.get(),
      "+proj=latlong +ellps=WGS84 +grids=@/mem/missing,/mem/h_std.tif"));
  ASSERT_NE(nullptr, p_opt.get());
  auto hgrids = NS_PROJ::pj_hgrid_init(p_opt.get(), "grids");
  ASSERT_EQ(1U, hgrids.size());
  EXPECT_EQ(0, proj_context_errno(ctx.get()));

  // Forward, inverse, HUGE_VAL, outside grid, 2pi wraparound, edge tolerances.
  const PJ_LP in_lp = {.lam = DegToRad(11.0), .phi = DegToRad(51.0)};
  const PJ_LP fwd_lp =
      NS_PROJ::pj_hgrid_apply(ctx.get(), hgrids, in_lp, PJ_FWD);
  EXPECT_NEAR(DegToRad(11.25), fwd_lp.lam, 1e-6);
  EXPECT_NEAR(DegToRad(51.1), fwd_lp.phi, 1e-6);
  const PJ_LP inv_lp =
      NS_PROJ::pj_hgrid_apply(ctx.get(), hgrids, fwd_lp, PJ_INV);
  EXPECT_NEAR(in_lp.lam, inv_lp.lam, 1e-9);
  EXPECT_NEAR(in_lp.phi, inv_lp.phi, 1e-9);

  // 2pi wraparound and edge clamping in pj_hgrid_apply / pj_hgrid_value.
  const PJ_LP wrap_neg = {.lam = DegToRad(11.0 - 360.0), .phi = DegToRad(51.0)};
  const PJ_LP wrap_pos = {.lam = DegToRad(11.0 + 360.0), .phi = DegToRad(51.0)};
  EXPECT_NEAR(DegToRad(11.25 - 360.0),
              NS_PROJ::pj_hgrid_apply(ctx.get(), hgrids, wrap_neg, PJ_FWD).lam,
              1e-6);
  EXPECT_NEAR(DegToRad(11.25 + 360.0),
              NS_PROJ::pj_hgrid_apply(ctx.get(), hgrids, wrap_pos, PJ_FWD).lam,
              1e-6);
  EXPECT_NEAR(DegToRad(0.25),
              NS_PROJ::pj_hgrid_value(p_opt.get(), hgrids, wrap_neg).lam, 1e-6);
  EXPECT_NEAR(DegToRad(0.25),
              NS_PROJ::pj_hgrid_value(p_opt.get(), hgrids, wrap_pos).lam, 1e-6);

  // REL_TOLERANCE_HGRIDSHIFT on west, east, south, north edges.
  for (const PJ_LP& edge : {
           PJ_LP{.lam = DegToRad(10.0 - 1e-5), .phi = DegToRad(51.0)},
           PJ_LP{.lam = DegToRad(12.0 + 1e-5), .phi = DegToRad(51.0)},
           PJ_LP{.lam = DegToRad(11.0), .phi = DegToRad(50.0 - 1e-5)},
           PJ_LP{.lam = DegToRad(11.0), .phi = DegToRad(52.0 + 1e-5)},
       }) {
    const PJ_LP shift = NS_PROJ::pj_hgrid_value(p_opt.get(), hgrids, edge);
    EXPECT_NEAR(DegToRad(0.25), shift.lam, 1e-6);
  }

  EXPECT_EQ(HUGE_VAL,
            NS_PROJ::pj_hgrid_apply(ctx.get(), hgrids,
                                    PJ_LP{.lam = HUGE_VAL, .phi = 0.0}, PJ_FWD)
                .lam);
  EXPECT_EQ(HUGE_VAL,
            NS_PROJ::pj_hgrid_apply(ctx.get(), hgrids,
                                    PJ_LP{.lam = 0.0, .phi = 0.0}, PJ_FWD)
                .lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_EQ(HUGE_VAL, NS_PROJ::pj_hgrid_value(p_opt.get(), hgrids,
                                              PJ_LP{.lam = 0.0, .phi = 0.0})
                          .lam);
  proj_context_errno_set(ctx.get(), 0);

  // Null grid in pj_hgrid_apply.
  ScopedPj p_hnull(pj_create_internal(ctx.get(), "+proj=latlong +grids=null"));
  auto hnull = NS_PROJ::pj_hgrid_init(p_hnull.get(), "grids");
  EXPECT_NEAR(in_lp.lam,
              NS_PROJ::pj_hgrid_apply(ctx.get(), hnull, in_lp, PJ_FWD).lam,
              1e-12);

  // Inverse grid switching (both unwrapped and -2*pi wrapped inputs).
  ScopedPj p_sw(pj_create_internal(
      ctx.get(), "+proj=latlong +grids=/mem/sw_a.tif,/mem/sw_b.tif"));
  auto hsw = NS_PROJ::pj_hgrid_init(p_sw.get(), "grids");
  const PJ_LP sw_inv = NS_PROJ::pj_hgrid_apply(
      ctx.get(), hsw, PJ_LP{.lam = DegToRad(10.2), .phi = DegToRad(11.0)},
      PJ_INV);
  EXPECT_NEAR(DegToRad(9.7), sw_inv.lam, 1e-7);
  const PJ_LP sw_inv_wrap = NS_PROJ::pj_hgrid_apply(
      ctx.get(), hsw,
      PJ_LP{.lam = DegToRad(10.2 - 360.0), .phi = DegToRad(11.0)}, PJ_INV);
  EXPECT_NEAR(DegToRad(9.7), sw_inv_wrap.lam, 1e-7);
  EXPECT_TRUE(log_cap.ContainsMessage("Switching from grid"));

  // NaN grid value inside horizontal shift grid triggers std::isnan handling.
  TiffIfdOptions h_nan_opt{
      .width = 3,
      .height = 3,
      .samples_per_pixel = 2,
      .west = 10.0,
      .north = 12.0,
      .res_x = 1.0,
      .res_y = 1.0,
      .gdal_metadata =
          "<GDALMetadata>"
          "<Item name=\"UNITTYPE\" sample=\"0\">degree</Item>"
          "<Item name=\"UNITTYPE\" sample=\"1\">degree</Item>"
          "</GDALMetadata>",
      .pixels =
          std::vector<double>(18, std::numeric_limits<double>::quiet_NaN()),
  };
  mem.AddFile("/mem/h_nan.tif", BuildSingleTiff(h_nan_opt));
  ScopedPj p_hnan(
      pj_create_internal(ctx.get(), "+proj=latlong +grids=/mem/h_nan.tif"));
  auto hnan = NS_PROJ::pj_hgrid_init(p_hnan.get(), "grids");
  const PJ_LP hnan_out = NS_PROJ::pj_hgrid_apply(
      ctx.get(), hnan, PJ_LP{.lam = DegToRad(11.0), .phi = DegToRad(11.0)},
      PJ_INV);
  EXPECT_TRUE(std::isnan(hnan_out.lam));
  EXPECT_TRUE(std::isnan(hnan_out.phi));
  proj_context_errno_set(ctx.get(), 0);

  // Inverse non-convergence.
  ScopedPj p_osc(
      pj_create_internal(ctx.get(), "+proj=latlong +grids=/mem/osc.tif"));
  auto hosc = NS_PROJ::pj_hgrid_init(p_osc.get(), "grids");
  EXPECT_EQ(HUGE_VAL,
            NS_PROJ::pj_hgrid_apply(
                ctx.get(), hosc,
                PJ_LP{.lam = DegToRad(2.0), .phi = DegToRad(1.0)}, PJ_INV)
                .lam);
  EXPECT_TRUE(log_cap.ContainsMessage("failed to converge"));
  proj_context_errno_set(ctx.get(), 0);

  // Inverse edge break (del == HUGE_VAL while i > 0).
  ScopedPj p_ebrk(
      pj_create_internal(ctx.get(), "+proj=latlong +grids=/mem/edge_brk.tif"));
  auto hebrk = NS_PROJ::pj_hgrid_init(p_ebrk.get(), "grids");
  const PJ_LP brk_out = NS_PROJ::pj_hgrid_apply(
      ctx.get(), hebrk, PJ_LP{.lam = DegToRad(10.5), .phi = DegToRad(11.0)},
      PJ_INV);
  EXPECT_NEAR(DegToRad(9.5), brk_out.lam, 1e-7);
  EXPECT_TRUE(log_cap.ContainsMessage("presumably at grid edge"));

  // Vertical grids: nodata combinations, fullWorldLongitude wrap, 2pi wrap,
  // NaN, null.
  ScopedPj p_vnd(pj_create_internal(
      ctx.get(),
      "+proj=latlong +grids=@/mem/missing.gtx,/mem/v_nd.gtx,/mem/v_world.gtx"));
  auto vgrids = NS_PROJ::pj_vgrid_init(p_vnd.get(), "grids");
  ASSERT_EQ(2U, vgrids.size());
  // Cell [0..1] x [0..1]: a=(0,0)=1.0 (w=0.5625) and d=(1,1)=3.0 (w=0.0625)
  // are valid => value = (1*0.5625 + 3*0.0625) / 0.625 = 1.2 => * 2.0 = 2.4.
  EXPECT_NEAR(2.4,
              NS_PROJ::pj_vgrid_value(
                  p_vnd.get(), vgrids,
                  PJ_LP{.lam = DegToRad(0.25), .phi = DegToRad(0.25)}, 2.0),
              1e-6);
  // Cell [1..2] x [0..1]: (1,1)=3.0, (2,1)=4.0 valid; (1,0)=ND, (2,0)=ND.
  EXPECT_NEAR(3.5,
              NS_PROJ::pj_vgrid_value(
                  p_vnd.get(), vgrids,
                  PJ_LP{.lam = DegToRad(1.5), .phi = DegToRad(0.5)}, 1.0),
              1e-6);
  // Cell [0..1] x [1..2]: only b=(1,1)=3.0 is valid.
  EXPECT_NEAR(3.0,
              NS_PROJ::pj_vgrid_value(
                  p_vnd.get(), vgrids,
                  PJ_LP{.lam = DegToRad(0.25), .phi = DegToRad(1.25)}, 1.0),
              1e-6);
  // Cell [2..3] x [0..1]: b=(3,0)=2.0 and c=(2,1)=4.0 valid -> tests b_valid +
  // c_valid.
  EXPECT_NEAR(3.0,
              NS_PROJ::pj_vgrid_value(
                  p_vnd.get(), vgrids,
                  PJ_LP{.lam = DegToRad(2.5), .phi = DegToRad(0.5)}, 1.0),
              1e-6);
  // Exact east boundary (lon=3.0, lat=0.0) clamps grid_ix2 = width - 1.
  EXPECT_NEAR(2.0,
              NS_PROJ::pj_vgrid_value(
                  p_vnd.get(), vgrids,
                  PJ_LP{.lam = DegToRad(3.0), .phi = DegToRad(0.0)}, 1.0),
              1e-6);
  // Cell [0..1] x [2..2]: all 4 nodata -> HUGE_VAL + GRID_AT_NODATA.
  EXPECT_EQ(HUGE_VAL,
            NS_PROJ::pj_vgrid_value(
                p_vnd.get(), vgrids,
                PJ_LP{.lam = DegToRad(0.5), .phi = DegToRad(2.0)}, 1.0));
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_GRID_AT_NODATA,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
  // +/-2pi wrap on v_nd.
  EXPECT_NEAR(
      1.2,
      NS_PROJ::pj_vgrid_value(
          p_vnd.get(), vgrids,
          PJ_LP{.lam = DegToRad(0.25 - 360.0), .phi = DegToRad(0.25)}, 1.0),
      1e-6);
  EXPECT_NEAR(
      1.2,
      NS_PROJ::pj_vgrid_value(
          p_vnd.get(), vgrids,
          PJ_LP{.lam = DegToRad(0.25 + 360.0), .phi = DegToRad(0.25)}, 1.0),
      1e-6);
  // Full world longitude grid wrap (< west and > east, grid_ix2 -> 0) and NaN.
  EXPECT_NEAR(25.0,
              NS_PROJ::pj_vgrid_value(
                  p_vnd.get(), vgrids,
                  PJ_LP{.lam = DegToRad(135.0), .phi = DegToRad(0.0)}, 1.0),
              1e-5);
  EXPECT_NEAR(25.0,
              NS_PROJ::pj_vgrid_value(
                  p_vnd.get(), vgrids,
                  PJ_LP{.lam = DegToRad(-225.0), .phi = DegToRad(0.0)}, 1.0),
              1e-5);
  EXPECT_EQ(HUGE_VAL, NS_PROJ::pj_vgrid_value(
                          p_vnd.get(), vgrids,
                          PJ_LP{.lam = std::numeric_limits<double>::quiet_NaN(),
                                .phi = 0.0},
                          1.0));

  ScopedPj p_vnull(pj_create_internal(ctx.get(), "+proj=latlong +grids=null"));
  auto vnull = NS_PROJ::pj_vgrid_init(p_vnull.get(), "grids");
  EXPECT_EQ(0.0, NS_PROJ::pj_vgrid_value(p_vnull.get(), vnull, in_lp, 1.0));

  // Projected grid errors in pj_hgrid_value, read_vgrid_value,
  // pj_bilinear_interpolation_three_samples.
  ScopedPj p_proj(
      pj_create_internal(ctx.get(), "+proj=latlong +grids=/mem/proj.tif"));
  auto ph = NS_PROJ::pj_hgrid_init(p_proj.get(), "grids");
  auto pv = NS_PROJ::pj_vgrid_init(p_proj.get(), "grids");
  auto pg = NS_PROJ::pj_generic_grid_init(p_proj.get(), "grids");
  const PJ_LP proj_pt = {.lam = 1500.0, .phi = 1500.0};
  EXPECT_EQ(HUGE_VAL, NS_PROJ::pj_hgrid_value(p_proj.get(), ph, proj_pt).lam);
  EXPECT_EQ(PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
            proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_EQ(HUGE_VAL, NS_PROJ::pj_vgrid_value(p_proj.get(), pv, proj_pt, 1.0));
  proj_context_errno_set(ctx.get(), 0);
  NS_PROJ::GenericShiftGridSet* gset_out = nullptr;
  const auto* proj_g = NS_PROJ::pj_find_generic_grid(pg, proj_pt, gset_out);
  ASSERT_NE(nullptr, proj_g);
  double b1 = 0.0, b2 = 0.0, b3 = 0.0;
  bool retry = false;
  EXPECT_FALSE(NS_PROJ::pj_bilinear_interpolation_three_samples(
      ctx.get(), proj_g, proj_pt, 0, 1, 2, b1, b2, b3, retry));
  proj_context_errno_set(ctx.get(), 0);

  // Geographic 3-sample generic grid + wraparound + truncated generic read + C
  // API +proj=xyzgridshift / +proj=hgridshift / +proj=vgridshift /
  // +proj=gridshift.
  TiffIfdOptions xyz_grid{
      .width = 256,
      .height = 256,
      .samples_per_pixel = 3,
      .planar_config = 1,
      .sample_type = TiffSampleType::kFloat32,
      .rows_per_strip = 256,
      .west = 10.0,
      .north = 60.0,
      .res_x = 0.1,
      .res_y = 0.1,
      .gdal_metadata =
          "<GDALMetadata>"
          "<Item name=\"DESCRIPTION\" sample=\"0\">x_translation</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"1\">y_translation</Item>"
          "<Item name=\"DESCRIPTION\" sample=\"2\">z_translation</Item>"
          "<Item name=\"UNITTYPE\" sample=\"0\">metre</Item>"
          "<Item name=\"UNITTYPE\" sample=\"1\">metre</Item>"
          "<Item name=\"UNITTYPE\" sample=\"2\">metre</Item>"
          "</GDALMetadata>",
  };
  for (size_t i = 0; i < size_t{256} * 256; ++i) {
    xyz_grid.pixels.push_back(10.0);
    xyz_grid.pixels.push_back(20.0);
    xyz_grid.pixels.push_back(30.0);
  }
  mem.AddFile("/mem/xyz256.tif",
              BuildCustomTiff(std::span<const TiffIfdOptions>(&xyz_grid, 1)));

  ScopedPj p_xyz_g(
      pj_create_internal(ctx.get(), "+proj=latlong +grids=/mem/xyz256.tif"));
  auto xyz_grids = NS_PROJ::pj_generic_grid_init(p_xyz_g.get(), "grids");
  ASSERT_EQ(1U, xyz_grids.size());
  const auto* xyz_ptr = NS_PROJ::pj_find_generic_grid(
      xyz_grids, PJ_LP{.lam = DegToRad(15.0), .phi = DegToRad(50.0)}, gset_out);
  ASSERT_NE(nullptr, xyz_ptr);
  EXPECT_EQ(nullptr, NS_PROJ::pj_find_generic_grid(
                         xyz_grids, PJ_LP{.lam = 0.0, .phi = 0.0}, gset_out));
  EXPECT_TRUE(NS_PROJ::pj_bilinear_interpolation_three_samples(
      ctx.get(), xyz_ptr,
      PJ_LP{.lam = DegToRad(15.0 - 360.0), .phi = DegToRad(50.0)}, 0, 1, 2, b1,
      b2, b3, retry));
  EXPECT_NEAR(10.0, b1, 1e-6);
  EXPECT_TRUE(NS_PROJ::pj_bilinear_interpolation_three_samples(
      ctx.get(), xyz_ptr,
      PJ_LP{.lam = DegToRad(15.0 + 360.0), .phi = DegToRad(50.0)}, 0, 1, 2, b1,
      b2, b3, retry));
  EXPECT_NEAR(30.0, b3, 1e-6);

  // Null generic grid and truncated vgrid / hgrid / generic error paths.
  ScopedPj p_gnull(pj_create_internal(ctx.get(), "+proj=latlong +grids=null"));
  auto gnull = NS_PROJ::pj_generic_grid_init(p_gnull.get(), "grids");
  const auto* gnull_ptr = NS_PROJ::pj_find_generic_grid(
      gnull, PJ_LP{.lam = 0.0, .phi = 0.0}, gset_out);
  ASSERT_NE(nullptr, gnull_ptr);
  b1 = 1.0;
  b2 = 2.0;
  b3 = 3.0;
  EXPECT_TRUE(NS_PROJ::pj_bilinear_interpolation_three_samples(
      ctx.get(), gnull_ptr, PJ_LP{.lam = 0.0, .phi = 0.0}, 0, 1, 2, b1, b2, b3,
      retry));
  EXPECT_EQ(0.0, b1);
  EXPECT_EQ(0.0, b2);
  EXPECT_EQ(0.0, b3);

  const TiffIfdOptions v_gtiff_opt{
      .width = 2,
      .height = 2,
      .west = 0.0,
      .north = 1.0,
      .res_x = 1.0,
      .res_y = 1.0,
      .pixels = {5.0, 5.0, 5.0, 5.0},
  };
  mem.AddFile("/mem/v_gtiff.tif", BuildSingleTiff(v_gtiff_opt));
  ScopedPj p_vgt(
      pj_create_internal(ctx.get(), "+proj=latlong +grids=/mem/v_gtiff.tif"));
  auto vgt = NS_PROJ::pj_vgrid_init(p_vgt.get(), "grids");
  ASSERT_EQ(1U, vgt.size());
  EXPECT_NEAR(5.0,
              NS_PROJ::pj_vgrid_value(
                  p_vgt.get(), vgt,
                  PJ_LP{.lam = DegToRad(0.5), .phi = DegToRad(0.5)}, 1.0),
              1e-6);
  EXPECT_TRUE(log_cap.ContainsMessage("proj_vgrid_value:"));

  std::vector<uint8_t> v_trunc_bytes =
      BuildGtxGrid(0.0, 0.0, 1.0, 1.0, 2, 2, std::vector<float>(4, 1.0f));
  v_trunc_bytes.resize(40);
  mem.AddFile("/mem/v_trunc.gtx", std::move(v_trunc_bytes));
  ScopedPj p_vtr(
      pj_create_internal(ctx.get(), "+proj=latlong +grids=/mem/v_trunc.gtx"));
  auto vtr = NS_PROJ::pj_vgrid_init(p_vtr.get(), "grids");
  ASSERT_EQ(1U, vtr.size());
  EXPECT_EQ(HUGE_VAL,
            NS_PROJ::pj_vgrid_value(
                p_vtr.get(), vtr,
                PJ_LP{.lam = DegToRad(0.5), .phi = DegToRad(0.5)}, 1.0));
  proj_context_errno_set(ctx.get(), 0);

  const TiffIfdOptions trunc_hg_opt{
      .width = 2,
      .height = 2,
      .samples_per_pixel = 3,
      .west = 0.0,
      .north = 1.0,
      .res_x = 1.0,
      .res_y = 1.0,
      .pixels = std::vector<double>(12, 1.0),
  };
  std::vector<uint8_t> hg_trunc_bytes = BuildSingleTiff(trunc_hg_opt);
  hg_trunc_bytes.resize(hg_trunc_bytes.size() - 16);
  mem.AddFile("/mem/hg_trunc.tif", std::move(hg_trunc_bytes));
  ScopedPj p_hgtr(
      pj_create_internal(ctx.get(), "+proj=latlong +grids=/mem/hg_trunc.tif"));
  auto htr = NS_PROJ::pj_hgrid_init(p_hgtr.get(), "grids");
  auto gtr = NS_PROJ::pj_generic_grid_init(p_hgtr.get(), "grids");
  ASSERT_EQ(1U, htr.size());
  ASSERT_EQ(1U, gtr.size());
  const PJ_LP tr_pt = {.lam = DegToRad(0.5), .phi = DegToRad(0.5)};
  EXPECT_EQ(HUGE_VAL,
            NS_PROJ::pj_hgrid_apply(ctx.get(), htr, tr_pt, PJ_FWD).lam);
  EXPECT_EQ(PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID, proj_context_errno(ctx.get()));
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_EQ(HUGE_VAL, NS_PROJ::pj_hgrid_value(p_hgtr.get(), htr, tr_pt).lam);
  proj_context_errno_set(ctx.get(), 0);
  const auto* gtr_ptr = NS_PROJ::pj_find_generic_grid(gtr, tr_pt, gset_out);
  ASSERT_NE(nullptr, gtr_ptr);
  EXPECT_FALSE(NS_PROJ::pj_bilinear_interpolation_three_samples(
      ctx.get(), gtr_ptr, tr_pt, 0, 1, 2, b1, b2, b3, retry));

  ScopedPj pipe_xyz(proj_create(
      ctx.get(),
      "+proj=pipeline +step +proj=cart +ellps=WGS84 "
      "+step +proj=xyzgridshift +ellps=WGS84 +grids=/mem/xyz256.tif"));
  ASSERT_NE(nullptr, pipe_xyz.get());
  const PJ_COORD xyz_out =
      proj_trans(pipe_xyz.get(), PJ_FWD,
                 proj_coord(DegToRad(15.0), DegToRad(50.0), 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(pipe_xyz.get()));
  EXPECT_TRUE(std::isfinite(xyz_out.xyz.x));
  const PJ_COORD xyz_roundtrip = proj_trans(pipe_xyz.get(), PJ_INV, xyz_out);
  EXPECT_EQ(0, proj_errno(pipe_xyz.get()));
  EXPECT_NEAR(DegToRad(15.0), xyz_roundtrip.lp.lam, 1e-9);
  EXPECT_NEAR(DegToRad(50.0), xyz_roundtrip.lp.phi, 1e-9);

  ScopedPj p_hg(proj_create(
      ctx.get(), "+proj=hgridshift +ellps=WGS84 +grids=/mem/h_std.tif"));
  ASSERT_NE(nullptr, p_hg.get());
  const PJ_COORD hg_out = proj_trans(
      p_hg.get(), PJ_FWD, proj_coord(in_lp.lam, in_lp.phi, 0.0, 0.0));
  EXPECT_EQ(0, proj_errno(p_hg.get()));
  EXPECT_NEAR(DegToRad(11.25), hg_out.lp.lam, 1e-6);

  ScopedPj p_vg(proj_create(
      ctx.get(), "+proj=vgridshift +ellps=WGS84 +grids=/mem/v_nd.gtx"));
  ASSERT_NE(nullptr, p_vg.get());
  const PJ_COORD vg_out =
      proj_trans(p_vg.get(), PJ_FWD,
                 proj_coord(DegToRad(0.25), DegToRad(0.25), 10.0, 0.0));
  EXPECT_EQ(0, proj_errno(p_vg.get()));
  EXPECT_NEAR(8.8, vg_out.lpz.z, 1e-6);
}

TEST(GridAndInitInfoTest, ProjGridInfoAndProjInitInfo) {
  TempDir tmp_dir("proj_grids_info_test");
  ASSERT_TRUE(WriteBinaryFile(
      tmp_dir.child("tvgrid.gtx"),
      BuildGtxGrid(10.0, 50.0, 1.0, 1.0, 2, 2, std::vector<float>(4, 2.0f))));
  const TiffIfdOptions th{
      .width = 2,
      .height = 2,
      .samples_per_pixel = 2,
      .west = -5.0,
      .north = 45.0,
      .res_x = 0.5,
      .res_y = 0.5,
      .pixels = std::vector<double>(8, 1.0),
  };
  ASSERT_TRUE(
      WriteBinaryFile(tmp_dir.child("thgrid.tif"), BuildSingleTiff(th)));
  ASSERT_TRUE(WriteTextFile(tmp_dir.child("tmeta"),
                            "<metadata> +version=1.2.3 +origin=GOOGTEST "
                            "+lastupdate=2026-02-03 <>\n"));
  ASSERT_TRUE(
      WriteTextFile(tmp_dir.child("tpartial"), "<metadata> +version=2.0 <>\n"));
  ASSERT_TRUE(WriteTextFile(tmp_dir.child("tnometa"),
                            "<1000> +proj=latlong +ellps=WGS84 <>\n"));

  PJ_CONTEXT* def_ctx = pj_get_default_ctx();
  const std::vector<std::string> orig_paths = def_ctx->search_paths;
  const PJ_LOG_LEVEL orig_level = proj_log_level(def_ctx, PJ_LOG_NONE);
  absl::Cleanup restore_default_ctx = [&]() {
    std::vector<const char*> c_paths;
    c_paths.reserve(orig_paths.size());
    for (const auto& p : orig_paths) {
      c_paths.push_back(p.c_str());
    }
    proj_context_set_search_paths(nullptr, static_cast<int>(c_paths.size()),
                                  c_paths.empty() ? nullptr : c_paths.data());
    proj_log_level(nullptr, orig_level);
    proj_context_errno_set(def_ctx, 0);
    def_ctx->lastFullErrorMessage.clear();
    proj_cleanup();
  };

  const char* search_path = tmp_dir.c_str();
  proj_context_set_search_paths(nullptr, 1, &search_path);

  // 1. proj_grid_info
  const PJ_GRID_INFO null_info = proj_grid_info("null");
  EXPECT_STREQ("null", null_info.format);
  EXPECT_STREQ("null", null_info.gridname);

  const PJ_GRID_INFO gtx_info = proj_grid_info("tvgrid.gtx");
  EXPECT_STREQ("gtx", gtx_info.format);
  EXPECT_EQ(2, gtx_info.n_lon);
  EXPECT_EQ(2, gtx_info.n_lat);
  EXPECT_THAT(gtx_info.filename, HasSubstr("tvgrid.gtx"));

  const PJ_GRID_INFO tif_info = proj_grid_info("thgrid.tif");
  EXPECT_STREQ("gtiff", tif_info.format);
  EXPECT_EQ(2, tif_info.n_lon);
  EXPECT_NEAR(DegToRad(-5.0), tif_info.lowerleft.lam, 1e-12);

  const PJ_GRID_INFO conus_info = proj_grid_info("conus");
  EXPECT_STREQ("ctable2", conus_info.format);

  const PJ_GRID_INFO ntv1_info = proj_grid_info("ntv1_can.dat");
  EXPECT_STREQ("ntv1", ntv1_info.format);

  const PJ_GRID_INFO ntv2_info = proj_grid_info("ntf_r93.gsb");
  EXPECT_STREQ("ntv2", ntv2_info.format);

  const PJ_GRID_INFO miss_info = proj_grid_info("missing_nonexistent_grid");
  EXPECT_STREQ("missing", miss_info.format);
  proj_context_errno_set(def_ctx, 0);

  // 2. proj_init_info
  const PJ_INIT_INFO epsg_info = proj_init_info("epsg");
  EXPECT_STREQ("epsg", epsg_info.name);
  EXPECT_STREQ("EPSG", epsg_info.origin);

  const PJ_INIT_INFO epsg_upper = proj_init_info("EPSG");
  EXPECT_STREQ("EPSG", epsg_upper.name);
  EXPECT_STREQ("EPSG", epsg_upper.origin);

  const PJ_INIT_INFO ignf_info = proj_init_info("IGNF");
  EXPECT_STREQ("IGNF", ignf_info.name);
  EXPECT_STREQ("IGNF", ignf_info.origin);

  const PJ_INIT_INFO missing_init = proj_init_info("no_such_init_file");
  EXPECT_THAT(std::string(missing_init.name), IsEmpty());

  const std::string long_name(70, 'x');
  const PJ_INIT_INFO long_init = proj_init_info(long_name.c_str());
  EXPECT_THAT(std::string(long_init.name), IsEmpty());

  const PJ_INIT_INFO meta_info = proj_init_info("tmeta");
  EXPECT_STREQ("tmeta", meta_info.name);
  EXPECT_STREQ("1.2.3", meta_info.version);
  EXPECT_STREQ("GOOGTEST", meta_info.origin);
  EXPECT_STREQ("2026-02-03", meta_info.lastupdate);
  EXPECT_THAT(meta_info.filename, HasSubstr("tmeta"));

  const PJ_INIT_INFO partial_info = proj_init_info("tpartial");
  EXPECT_STREQ("tpartial", partial_info.name);
  EXPECT_STREQ("2.0", partial_info.version);
  EXPECT_STREQ("Unknown", partial_info.origin);
  EXPECT_STREQ("Unknown", partial_info.lastupdate);

  const PJ_INIT_INFO nometa_info = proj_init_info("tnometa");
  EXPECT_STREQ("tnometa", nometa_info.name);
  EXPECT_STREQ("Unknown", nometa_info.origin);
  EXPECT_STREQ("Unknown", nometa_info.version);
  EXPECT_STREQ("Unknown", nometa_info.lastupdate);
  EXPECT_THAT(std::string(nometa_info.filename), Not(IsEmpty()));
  proj_context_errno_set(def_ctx, 0);
  EXPECT_EQ(0, proj_context_errno(def_ctx));
}

// TODO(schwehr): Unreached defensive/build-time-guarded branches in grids.cpp:
//   - `!TIFF_ENABLED` compile-time fallback branches (TIFF is enabled in
//   google3).
//   - `File::hasChanged()` retry paths in `pj_hgrid_apply`, `pj_hgrid_value`,
//     and `read_vgrid_value` (only `NetworkFile` returns true; networking is
//     disabled/stubbed in non-GCE google3 builds).
//   - `Missing PlanarConfig tag` and `Unsupported compression method (OJPEG)`
//     under libtiff (libtiff presets PlanarConfig and OJPEG codec is disabled).
//   - `std::bad_alloc` OOM catch blocks in buffer/cache insertion.

}  // namespace
}  // namespace proj
