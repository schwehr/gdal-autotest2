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
// Shared in-memory test helpers for the OGR Parquet driver C++ unit tests.

#ifndef THIRD_PARTY_GDAL_AUTOTEST2_CPP_OGR_OGRSF_FRMTS_PARQUET_PARQUET_TEST_UTIL_H_
#define THIRD_PARTY_GDAL_AUTOTEST2_CPP_OGR_OGRSF_FRMTS_PARQUET_PARQUET_TEST_UTIL_H_

#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arrow/io/memory.h"
#include "arrow/table.h"
#include "parquet/arrow/writer.h"
#include "parquet/properties.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {

// Writes an Arrow table into a /vsimem/ Parquet file using Arrow/Parquet
// writers directly, bypassing GDAL's writer to construct arbitrary inputs.
inline ::testing::AssertionResult WriteArrowTableToVsiMem(
    const char* path, const std::shared_ptr<arrow::Table>& table,
    int64_t chunk_size = 1024,
    const std::shared_ptr<parquet::WriterProperties>& props = nullptr) {
  auto out_res = arrow::io::BufferOutputStream::Create();
  if (!out_res.ok()) {
    return ::testing::AssertionFailure() << out_res.status().ToString();
  }
  auto out = *out_res;

  auto writer_props =
      props ? props : parquet::WriterProperties::Builder().build();
  auto arrow_props =
      parquet::ArrowWriterProperties::Builder().store_schema()->build();
  auto st =
      parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out,
                                 chunk_size, writer_props, arrow_props);
  if (!st.ok()) {
    return ::testing::AssertionFailure() << st.ToString();
  }
  auto buf_res = out->Finish();
  if (!buf_res.ok()) {
    return ::testing::AssertionFailure() << buf_res.status().ToString();
  }
  auto buf = *buf_res;

  const std::string dir = CPLGetPathSafe(path);
  if (!dir.empty()) {
    VSIMkdirRecursive(dir.c_str(), 0755);
  }
  VSILFILE* fp = VSIFOpenL(path, "wb");
  if (fp == nullptr) {
    return ::testing::AssertionFailure() << "VSIFOpenL failed for " << path;
  }
  const size_t written = VSIFWriteL(buf->data(), 1, buf->size(), fp);
  const int close_ret = VSIFCloseL(fp);
  if (written != static_cast<size_t>(buf->size()) || close_ret != 0) {
    return ::testing::AssertionFailure() << "Failed writing " << path;
  }
  return ::testing::AssertionSuccess();
}

// Writes arbitrary raw bytes into /vsimem/, creating parent directories.
inline ::testing::AssertionResult WriteBytesToVsiMem(const char* path,
                                                     std::string_view data) {
  const std::string dir = CPLGetPathSafe(path);
  if (!dir.empty()) {
    VSIMkdirRecursive(dir.c_str(), 0755);
  }
  VSILFILE* fp = VSIFOpenL(path, "wb");
  if (fp == nullptr) {
    return ::testing::AssertionFailure() << "VSIFOpenL failed for " << path;
  }
  const size_t written =
      data.empty() ? 0 : VSIFWriteL(data.data(), 1, data.size(), fp);
  const int close_ret = VSIFCloseL(fp);
  if (written != data.size() || close_ret != 0) {
    return ::testing::AssertionFailure() << "Failed writing bytes to " << path;
  }
  return ::testing::AssertionSuccess();
}

// Converts a 2D point into canonical little-endian WKB bytes.
inline std::string MakePointWkb(double x, double y) {
  static_assert(std::endian::native == std::endian::little);
  std::string out(21, '\0');
  out[0] = 1;                   // NDR (little-endian)
  const uint32_t wkb_type = 1;  // wkbPoint
  std::memcpy(&out[1], &wkb_type, sizeof(wkb_type));
  std::memcpy(&out[5], &x, sizeof(x));
  std::memcpy(&out[13], &y, sizeof(y));
  return out;
}

// Converts a 3D PointZ into canonical little-endian WKB bytes.
inline std::string MakePointZWkb(double x, double y, double z) {
  std::string out(29, '\0');
  out[0] = 1;
  const uint32_t wkb_type = 1001;  // ISO wkbPointZ
  std::memcpy(&out[1], &wkb_type, sizeof(wkb_type));
  std::memcpy(&out[5], &x, sizeof(x));
  std::memcpy(&out[13], &y, sizeof(y));
  std::memcpy(&out[21], &z, sizeof(z));
  return out;
}

// Opens a /vsimem/ Parquet dataset through GDALOpenEx.
inline GDALDatasetUniquePtr OpenParquetDataset(
    const char* path, unsigned int flags = GDAL_OF_VECTOR | GDAL_OF_READONLY,
    CSLConstList open_options = nullptr) {
  const char* const drivers[] = {"Parquet", nullptr};
  return GDALDatasetUniquePtr(GDALDataset::FromHandle(
      GDALOpenEx(path, flags, drivers, open_options, nullptr)));
}

// Creates a simple multi-row-group Parquet dataset via GDAL's Parquet writer.
inline ::testing::AssertionResult CreateSampleParquetViaGdal(
    const char* path, int row_count, int row_group_size,
    bool with_fid_column = false, bool with_geom = true, int epsg = 4326) {
  const std::string dir = CPLGetPathSafe(path);
  if (!dir.empty()) {
    VSIMkdirRecursive(dir.c_str(), 0755);
  }
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("Parquet");
  if (drv == nullptr) {
    return ::testing::AssertionFailure() << "Parquet driver not registered";
  }
  GDALDatasetUniquePtr ds(drv->Create(path, 0, 0, 0, GDT_Unknown, nullptr));
  if (ds == nullptr) {
    return ::testing::AssertionFailure()
           << "Failed to create " << path << ": " << CPLGetLastErrorMsg();
  }

  CPLStringList options;
  options.SetNameValue("ROW_GROUP_SIZE",
                       std::to_string(row_group_size).c_str());
  if (with_fid_column) {
    options.SetNameValue("FID", "fid");
  }

  OGRSpatialReference srs;
  if (with_geom && epsg > 0) {
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    srs.importFromEPSG(epsg);
  }
  OGRLayer* lyr =
      ds->CreateLayer("sample", with_geom && epsg > 0 ? &srs : nullptr,
                      with_geom ? wkbPoint : wkbNone, options.List());
  if (lyr == nullptr) {
    return ::testing::AssertionFailure()
           << "CreateLayer failed: " << CPLGetLastErrorMsg();
  }

  OGRFieldDefn id_field("id", OFTInteger);
  OGRFieldDefn name_field("name", OFTString);
  OGRFieldDefn val_field("val", OFTReal);
  if (lyr->CreateField(&id_field) != OGRERR_NONE ||
      lyr->CreateField(&name_field) != OGRERR_NONE ||
      lyr->CreateField(&val_field) != OGRERR_NONE) {
    return ::testing::AssertionFailure() << "CreateField failed";
  }

  for (int i = 0; i < row_count; ++i) {
    OGRFeature feat(lyr->GetLayerDefn());
    if (with_fid_column) {
      feat.SetFID(i * 10 + 1);
    }
    feat.SetField(0, i);
    if (i % 3 != 2) {
      feat.SetField(1, ("item_" + std::to_string(i)).c_str());
    }
    feat.SetField(2, static_cast<double>(i) * 1.5);
    if (with_geom && i % 4 != 3) {
      auto pt = std::make_unique<OGRPoint>(static_cast<double>(i),
                                           static_cast<double>(i) * 2.0);
      feat.SetGeometryDirectly(pt.release());
    }
    if (lyr->CreateFeature(&feat) != OGRERR_NONE) {
      return ::testing::AssertionFailure() << "CreateFeature failed";
    }
  }

  if (ds->Close() != CE_None) {
    return ::testing::AssertionFailure()
           << "Close failed: " << CPLGetLastErrorMsg();
  }
  return ::testing::AssertionSuccess();
}

// OutputStream that allows up to `fail_after_writes` Write() calls before
// returning IOError, and fails Close() to exercise write/close error paths.
class ThresholdFailingOutputStream final : public arrow::io::OutputStream {
 public:
  explicit ThresholdFailingOutputStream(int fail_after_writes = 1)
      : fail_after_writes_(fail_after_writes) {}

  arrow::Status Close() override {
    closed_ = true;
    return arrow::Status::IOError("simulated close failure");
  }
  bool closed() const override { return closed_; }
  arrow::Result<int64_t> Tell() const override { return pos_; }
  arrow::Status Write(const void*, int64_t nbytes) override {
    if (writes_seen_ < fail_after_writes_) {
      ++writes_seen_;
      pos_ += nbytes;
      return arrow::Status::OK();
    }
    return arrow::Status::IOError("simulated write failure");
  }

 private:
  int fail_after_writes_;
  int writes_seen_ = 0;
  int64_t pos_ = 0;
  bool closed_ = false;
};

}  // namespace autotest2

#endif  // THIRD_PARTY_GDAL_AUTOTEST2_CPP_OGR_OGRSF_FRMTS_PARQUET_PARQUET_TEST_UTIL_H_
