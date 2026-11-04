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
// Tests the GDAL multidimensional data model API (gcore/gdalmultidim.cpp).
//
// Build caveat in google3: //third_party/gdal/BUILD compiles with libcurl
// disabled (HAVE_CURL undefined) and without linked netCDF multidimensional
// driver, so /vsicurl/ cache paths execute offline fall-through branches and
// a minimal in-memory fake netCDF driver is registered where needed.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalmultidim.cpp

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_pam_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/gcore/gdalmultidim_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;

using AttributeHPtr = std::unique_ptr<std::remove_pointer_t<GDALAttributeH>,
                                      decltype(&GDALAttributeRelease)>;
using DatasetHPtr =
    std::unique_ptr<std::remove_pointer_t<GDALDatasetH>, decltype(&GDALClose)>;
using DimensionHPtr = std::unique_ptr<std::remove_pointer_t<GDALDimensionH>,
                                      decltype(&GDALDimensionRelease)>;
using EDTComponentHPtr =
    std::unique_ptr<std::remove_pointer_t<GDALEDTComponentH>,
                    decltype(&GDALEDTComponentRelease)>;
using ExtendedDataTypeHPtr =
    std::unique_ptr<std::remove_pointer_t<GDALExtendedDataTypeH>,
                    decltype(&GDALExtendedDataTypeRelease)>;
using GroupHPtr = std::unique_ptr<std::remove_pointer_t<GDALGroupH>,
                                  decltype(&GDALGroupRelease)>;
using MDArrayHPtr = std::unique_ptr<std::remove_pointer_t<GDALMDArrayH>,
                                    decltype(&GDALMDArrayRelease)>;
using SpatialRefHPtr =
    std::unique_ptr<std::remove_pointer_t<OGRSpatialReferenceH>,
                    decltype(&OSRDestroySpatialReference)>;

size_t MultiplyExtents(std::span<const size_t> extents) {
  size_t total = 1;
  for (size_t v : extents) {
    total *= v;
  }
  return total;
}

// Minimal custom GDALGroup to exercise base-class virtuals in GDALGroup and
// GDALIHasAttribute.
class DummyBaseGroup : public GDALGroup {
 public:
  DummyBaseGroup(const std::string& parent_name, const std::string& name,
                 const std::string& context = std::string())
      : GDALGroup(parent_name, name, context) {}

  static std::shared_ptr<DummyBaseGroup> Create(
      const std::string& parent_name, const std::string& name,
      const std::string& context = std::string()) {
    auto grp = std::make_shared<DummyBaseGroup>(parent_name, name, context);
    grp->SetSelf(grp);
    return grp;
  }

  void AddDataType(const std::shared_ptr<GDALExtendedDataType>& dt) {
    m_apoTypes.push_back(dt);
  }

  void AddDummyAttribute(const std::shared_ptr<GDALAttribute>& attr) {
    attrs_.push_back(attr);
  }

  std::vector<std::shared_ptr<GDALAttribute>> GetAttributes(
      CSLConstList papszOptions = nullptr) const override {
    if (!attrs_.empty()) return attrs_;
    return GDALGroup::GetAttributes(papszOptions);
  }

  bool CallCheckValid() const { return CheckValidAndErrorOutIfNot(); }
  void CallBaseRename(const std::string& new_name) { BaseRename(new_name); }
  void CallNotifyChildren() {
    NotifyChildrenOfRenaming();
    NotifyChildrenOfDeletion();
  }

 private:
  std::vector<std::shared_ptr<GDALAttribute>> attrs_;
};

// Minimal custom GDALDimension to exercise base-class virtuals in
// GDALDimension.
class DummyBaseDimension : public GDALDimension {
 public:
  DummyBaseDimension(const std::string& parent_name, const std::string& name,
                     const std::string& type, const std::string& direction,
                     GUInt64 size)
      : GDALDimension(parent_name, name, type, direction, size) {}

  void CallBaseRename(const std::string& new_name) { BaseRename(new_name); }
};

// Custom GDALMDArray to exercise GDALAbstractMDArray and GDALMDArray base
// methods and protected helpers.
class CustomTestMDArray : public GDALMDArray {
 public:
  CustomTestMDArray(const std::string& parent_name, const std::string& name,
                    std::vector<std::shared_ptr<GDALDimension>> dims,
                    const GDALExtendedDataType& dt,
                    const std::string& filename = "",
                    const std::string& context = "")
      : GDALAbstractMDArray(parent_name, name),
        GDALMDArray(parent_name, name, context),
        dims_(std::move(dims)),
        dt_(dt),
        filename_(filename) {
    size_t total = 1;
    for (const auto& d : dims_) {
      total *= static_cast<size_t>(d->GetSize());
    }
    data_.resize(total * dt_.GetSize(), 0);
  }

  static std::shared_ptr<CustomTestMDArray> Create(
      const std::string& parent_name, const std::string& name,
      std::vector<std::shared_ptr<GDALDimension>> dims,
      const GDALExtendedDataType& dt, const std::string& filename = "",
      const std::string& context = "") {
    auto ar = std::make_shared<CustomTestMDArray>(
        parent_name, name, std::move(dims), dt, filename, context);
    ar->SetSelf(ar);
    return ar;
  }

  bool IsWritable() const override { return writable_; }
  void SetWritable(bool w) { writable_ = w; }

  bool mock_always_succeed_iread_ = false;

  const std::string& GetFilename() const override { return filename_; }

  const std::vector<std::shared_ptr<GDALDimension>>& GetDimensions()
      const override {
    return dims_;
  }

  const GDALExtendedDataType& GetDataType() const override { return dt_; }

  std::vector<GUInt64> GetBlockSize() const override {
    if (!block_size_.empty()) return block_size_;
    return GDALMDArray::GetBlockSize();
  }

  void SetCustomBlockSize(std::vector<GUInt64> bs) {
    block_size_ = std::move(bs);
  }

  void SetRootGroup(const std::shared_ptr<GDALGroup>& rg) { root_group_ = rg; }
  std::shared_ptr<GDALGroup> GetRootGroup() const override {
    if (root_group_) return root_group_;
    return GDALMDArray::GetRootGroup();
  }

  bool CallBaseIWrite(const GUInt64* start, const size_t* count,
                      const GInt64* step, const GPtrDiff_t* stride,
                      const GDALExtendedDataType& dt, const void* buf) {
    return GDALAbstractMDArray::IWrite(start, count, step, stride, dt, buf);
  }

  bool CallBaseSetStatistics(bool approx, double min, double max, double mean,
                             double stddev, GUInt64 valid_count,
                             CSLConstList options) {
    return GDALMDArray::SetStatistics(approx, min, max, mean, stddev,
                                      valid_count, options);
  }

  bool CallIsCacheable() const { return IsCacheable(); }
  static std::string CallMassageName(const std::string& s) {
    return MassageName(s);
  }

  bool CallIsTransposedRequest(const size_t* count,
                               const GPtrDiff_t* stride) const {
    return IsTransposedRequest(count, stride);
  }

  bool CallReadForTransposedRequest(const GUInt64* start, const size_t* count,
                                    const GInt64* step,
                                    const GPtrDiff_t* stride,
                                    const GDALExtendedDataType& dt,
                                    void* dst) const {
    return ReadForTransposedRequest(start, count, step, stride, dt, dst);
  }

  bool CallIsStepOneContiguous(const size_t* count, const GInt64* step,
                               const GPtrDiff_t* stride,
                               const GDALExtendedDataType& dt) const {
    return IsStepOneContiguousRowMajorOrderedSameDataType(count, step, stride,
                                                          dt);
  }

  bool CallReadUsingContiguousIRead(const GUInt64* start, const size_t* count,
                                    const GInt64* step,
                                    const GPtrDiff_t* stride,
                                    const GDALExtendedDataType& dt,
                                    void* dst) const {
    return ReadUsingContiguousIRead(start, count, step, stride, dt, dst);
  }

  void CallBaseRename(const std::string& new_name) { BaseRename(new_name); }
  bool CallCheckValid() const { return CheckValidAndErrorOutIfNot(); }
  void CallNotifyChildren() {
    NotifyChildrenOfRenaming();
    NotifyChildrenOfDeletion();
  }

  std::vector<GByte>& RawData() { return data_; }

 protected:
  bool IRead(const GUInt64* arrayStartIdx, const size_t* count,
             const GInt64* arrayStep, const GPtrDiff_t* bufferStride,
             const GDALExtendedDataType& bufferDataType,
             void* pDstBuffer) const override {
    if (mock_always_succeed_iread_) return true;
    if (!pDstBuffer) return false;
    if (dims_.empty()) {
      GDALExtendedDataType::CopyValue(data_.data(), dt_, pDstBuffer,
                                      bufferDataType);
      return true;
    }
    const size_t nDims = dims_.size();
    std::vector<GInt64> default_step;
    if (!arrayStep) {
      default_step.assign(nDims, 1);
      arrayStep = default_step.data();
    }
    std::vector<GPtrDiff_t> default_stride;
    if (!bufferStride && count) {
      default_stride.resize(nDims);
      size_t s = 1;
      for (size_t i = nDims; i > 0; --i) {
        default_stride[i - 1] = s;
        s *= count[i - 1];
      }
      bufferStride = default_stride.data();
    }
    std::vector<GUInt64> default_start;
    if (!arrayStartIdx) {
      default_start.assign(nDims, 0);
      arrayStartIdx = default_start.data();
    }
    if (!IsStepOneContiguousRowMajorOrderedSameDataType(
            count, arrayStep, bufferStride, bufferDataType)) {
      if (use_transposed_helper_ && IsTransposedRequest(count, bufferStride)) {
        return ReadForTransposedRequest(arrayStartIdx, count, arrayStep,
                                        bufferStride, bufferDataType,
                                        pDstBuffer);
      }
      return ReadUsingContiguousIRead(arrayStartIdx, count, arrayStep,
                                      bufferStride, bufferDataType, pDstBuffer);
    }
    // Contiguous read from data_ for 1D/2D/3D.
    const size_t total_elts =
        MultiplyExtents(std::span<const size_t>(count, nDims));
    const size_t dt_size = dt_.GetSize();
    std::vector<size_t> idx(nDims, 0);
    auto* dst_ptr = static_cast<GByte*>(pDstBuffer);
    for (size_t iter = 0; iter < total_elts; ++iter) {
      size_t flat_idx = 0;
      size_t mul = 1;
      for (size_t d = nDims; d > 0; --d) {
        const size_t pos =
            static_cast<size_t>(arrayStartIdx[d - 1]) + idx[d - 1];
        flat_idx += pos * mul;
        mul *= static_cast<size_t>(dims_[d - 1]->GetSize());
      }
      GDALExtendedDataType::CopyValue(data_.data() + flat_idx * dt_size, dt_,
                                      dst_ptr + iter * dt_size, bufferDataType);
      for (size_t d = nDims; d > 0; --d) {
        idx[d - 1]++;
        if (idx[d - 1] < count[d - 1]) break;
        idx[d - 1] = 0;
      }
    }
    return true;
  }

 private:
  std::vector<std::shared_ptr<GDALDimension>> dims_;
  GDALExtendedDataType dt_;
  std::string filename_;
  bool writable_ = false;
  bool use_transposed_helper_ = false;
  std::vector<GUInt64> block_size_;
  std::vector<GByte> data_;
  std::shared_ptr<GDALGroup> root_group_;
};

// Custom GDALPamMDArray subclass to exercise GDALPamMDArray methods.
class CustomPamMDArray : public GDALPamMDArray {
 public:
  CustomPamMDArray(const std::string& parent_name, const std::string& name,
                   std::vector<std::shared_ptr<GDALDimension>> dims,
                   const GDALExtendedDataType& dt,
                   std::shared_ptr<GDALPamMultiDim> pam)
      : GDALAbstractMDArray(parent_name, name),
        GDALPamMDArray(parent_name, name, std::move(pam)),
        dims_(std::move(dims)),
        dt_(dt),
        vals_{1.0, 2.0, 3.0, 4.0} {}

  static std::shared_ptr<CustomPamMDArray> Create(
      const std::string& parent_name, const std::string& name,
      std::vector<std::shared_ptr<GDALDimension>> dims,
      const GDALExtendedDataType& dt, std::shared_ptr<GDALPamMultiDim> pam) {
    auto ar = std::make_shared<CustomPamMDArray>(
        parent_name, name, std::move(dims), dt, std::move(pam));
    ar->SetSelf(ar);
    return ar;
  }

  bool IsWritable() const override { return true; }
  const std::string& GetFilename() const override { return filename_; }
  const std::vector<std::shared_ptr<GDALDimension>>& GetDimensions()
      const override {
    return dims_;
  }
  const GDALExtendedDataType& GetDataType() const override { return dt_; }

 protected:
  bool IRead(const GUInt64* arrayStartIdx, const size_t* count,
             const GInt64* arrayStep, const GPtrDiff_t* bufferStride,
             const GDALExtendedDataType& bufferDataType,
             void* pDstBuffer) const override {
    for (size_t i = 0; i < count[0]; ++i) {
      const size_t idx =
          static_cast<size_t>(arrayStartIdx[0] + i * arrayStep[0]);
      const double v = vals_[idx % vals_.size()];
      GDALExtendedDataType::CopyValue(
          &v, dt_,
          static_cast<GByte*>(pDstBuffer) +
              i * bufferStride[0] * bufferDataType.GetSize(),
          bufferDataType);
    }
    return true;
  }

 private:
  std::vector<std::shared_ptr<GDALDimension>> dims_;
  GDALExtendedDataType dt_;
  std::string filename_;
  std::vector<double> vals_;
};

// ============================================================================
// 1. GDALExtendedDataType & GDALEDTComponent Tests (C++ and C API)
// ============================================================================

TEST(GdalmultidimTest, ExtendedDataTypeAndComponents) {
  // Numeric types.
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  EXPECT_EQ(GEDTC_NUMERIC, dt_f64.GetClass());
  EXPECT_EQ(GDT_Float64, dt_f64.GetNumericDataType());
  EXPECT_EQ(sizeof(double), dt_f64.GetSize());
  EXPECT_EQ(GEDTST_NONE, dt_f64.GetSubType());
  EXPECT_EQ(nullptr, dt_f64.GetRAT());
  EXPECT_FALSE(dt_f64.NeedsFreeDynamicMemory());

  // Copy and move constructors & assignment operators.
  GDALExtendedDataType dt_copy(dt_f64);
  EXPECT_EQ(dt_f64, dt_copy);
  GDALExtendedDataType dt_assigned = GDALExtendedDataType::Create(GDT_Int32);
  EXPECT_NE(dt_f64, dt_assigned);
  dt_assigned = dt_copy;
  EXPECT_EQ(dt_f64, dt_assigned);
  GDALExtendedDataType dt_moved(std::move(dt_copy));
  EXPECT_EQ(dt_f64, dt_moved);
  GDALExtendedDataType dt_move_assigned =
      GDALExtendedDataType::Create(GDT_Byte);
  dt_move_assigned = std::move(dt_moved);
  EXPECT_EQ(dt_f64, dt_move_assigned);

  // ExtendedDataType with RAT.
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  rat->CreateColumn("value", GFT_Integer, GFU_MinMax);
  rat->CreateColumn("name", GFT_String, GFU_Name);
  rat->SetValue(0, 0, 1);
  rat->SetValue(0, 1, "one");
  const auto dt_rat =
      GDALExtendedDataType::Create("enum_type", GDT_Int32, std::move(rat));
  EXPECT_EQ(GEDTC_NUMERIC, dt_rat.GetClass());
  EXPECT_EQ("enum_type", dt_rat.GetName());
  EXPECT_NE(nullptr, dt_rat.GetRAT());
  // Copy constructor and assignment with RAT.
  GDALExtendedDataType dt_rat_copy(dt_rat);
  EXPECT_NE(nullptr, dt_rat_copy.GetRAT());
  EXPECT_EQ(dt_rat, dt_rat_copy);
  GDALExtendedDataType dt_rat_assign = GDALExtendedDataType::Create(GDT_Byte);
  dt_rat_assign = dt_rat;
  EXPECT_NE(nullptr, dt_rat_assign.GetRAT());

  // String types.
  const auto dt_str = GDALExtendedDataType::CreateString(0, GEDTST_JSON);
  EXPECT_EQ(GEDTC_STRING, dt_str.GetClass());
  EXPECT_EQ(GEDTST_JSON, dt_str.GetSubType());
  EXPECT_EQ(0u, dt_str.GetMaxStringLength());
  EXPECT_EQ(sizeof(char*), dt_str.GetSize());
  EXPECT_TRUE(dt_str.NeedsFreeDynamicMemory());

  const auto dt_str10 = GDALExtendedDataType::CreateString(10, GEDTST_NONE);
  EXPECT_NE(dt_str, dt_str10);
  EXPECT_EQ(10u, dt_str10.GetMaxStringLength());

  // Compound types.
  auto comp1 = std::make_unique<GDALEDTComponent>("x", 0, dt_f64);
  auto comp2 = std::make_unique<GDALEDTComponent>(
      "s", sizeof(double), GDALExtendedDataType::CreateString());
  const GDALEDTComponent comp1_copy(*comp1);
  EXPECT_EQ(*comp1, comp1_copy);
  EXPECT_EQ("x", comp1->GetName());
  EXPECT_EQ(0u, comp1->GetOffset());
  EXPECT_EQ(dt_f64, comp1->GetType());

  std::vector<std::unique_ptr<GDALEDTComponent>> comps;
  comps.push_back(std::move(comp1));
  comps.push_back(std::move(comp2));
  const auto dt_comp = GDALExtendedDataType::Create(
      "my_comp", sizeof(double) + sizeof(char*), std::move(comps));
  EXPECT_EQ(GEDTC_COMPOUND, dt_comp.GetClass());
  EXPECT_EQ("my_comp", dt_comp.GetName());
  EXPECT_EQ(2u, dt_comp.GetComponents().size());
  EXPECT_TRUE(dt_comp.NeedsFreeDynamicMemory());

  const GDALExtendedDataType dt_comp_copy(dt_comp);
  EXPECT_EQ(dt_comp, dt_comp_copy);
  GDALExtendedDataType dt_comp_assign = GDALExtendedDataType::Create(GDT_Byte);
  dt_comp_assign = dt_comp;
  EXPECT_EQ(dt_comp, dt_comp_assign);

  // Invalid compound creations: empty comps, overlapping offsets, too small
  // total size, huge offset/size.
  struct BadCompoundCase {
    std::string_view name;
    bool empty_components;
    size_t first_offset;
    bool has_overlap;
    size_t second_offset;
    size_t total_size;
  };
  constexpr std::array<BadCompoundCase, 4> kBadCompoundCases = {{
      {.name = "bad1",
       .empty_components = true,
       .first_offset = 0,
       .has_overlap = false,
       .second_offset = 0,
       .total_size = 8},
      {.name = "bad2",
       .empty_components = false,
       .first_offset = 4,
       .has_overlap = true,
       .second_offset = 2,
       .total_size = 16},
      {.name = "bad3",
       .empty_components = false,
       .first_offset = 0,
       .has_overlap = false,
       .second_offset = 0,
       .total_size = 4},
      {.name = "bad4",
       .empty_components = false,
       .first_offset = static_cast<size_t>(std::numeric_limits<int>::max()),
       .has_overlap = false,
       .second_offset = 0,
       .total_size = static_cast<size_t>(std::numeric_limits<int>::max()) + 16},
  }};
  {
    WithQuietHandler quiet;
    for (const BadCompoundCase& spec : kBadCompoundCases) {
      CPLErrorReset();
      std::vector<std::unique_ptr<GDALEDTComponent>> bad_comps;
      if (!spec.empty_components) {
        bad_comps.push_back(
            std::make_unique<GDALEDTComponent>("a", spec.first_offset, dt_f64));
        if (spec.has_overlap) {
          bad_comps.push_back(std::make_unique<GDALEDTComponent>(
              "b", spec.second_offset, dt_f64));
        }
      }
      const auto bad_comp = GDALExtendedDataType::Create(
          std::string(spec.name), spec.total_size, std::move(bad_comps));
      EXPECT_EQ(GDT_Unknown, bad_comp.GetNumericDataType());
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    CPLErrorReset();
  }

  // CanConvertTo tests.
  const auto dt_unknown = GDALExtendedDataType::Create(GDT_Unknown);
  EXPECT_FALSE(dt_unknown.CanConvertTo(dt_f64));
  EXPECT_FALSE(dt_f64.CanConvertTo(dt_unknown));
  EXPECT_TRUE(dt_f64.CanConvertTo(dt_str));
  EXPECT_FALSE(dt_str.CanConvertTo(dt_f64));
  EXPECT_TRUE(dt_str.CanConvertTo(dt_str10));
  EXPECT_FALSE(dt_comp.CanConvertTo(dt_f64));
  EXPECT_TRUE(dt_comp.CanConvertTo(dt_comp_copy));

  // Compound with a single component X,Y vs compound with X only.
  std::vector<std::unique_ptr<GDALEDTComponent>> sub_comps;
  sub_comps.push_back(std::make_unique<GDALEDTComponent>("x", 0, dt_f64));
  const auto dt_sub_comp = GDALExtendedDataType::Create(
      "sub_comp", sizeof(double), std::move(sub_comps));
  EXPECT_TRUE(dt_comp.CanConvertTo(dt_sub_comp));
  EXPECT_FALSE(dt_sub_comp.CanConvertTo(dt_comp));

  // CopyValue & FreeDynamicMemory on compound + string + numeric.
  struct MyCompStruct {
    double x;
    char* s;
  };
  MyCompStruct src_val{.x = 42.5, .s = CPLStrdup("hello_comp")};
  MyCompStruct dst_val{.x = 0.0, .s = nullptr};
  absl::Cleanup free_comp_vals = [&] {
    dt_comp.FreeDynamicMemory(&src_val);
    dt_comp.FreeDynamicMemory(&dst_val);
  };
  EXPECT_TRUE(
      GDALExtendedDataType::CopyValue(&src_val, dt_comp, &dst_val, dt_comp));
  EXPECT_DOUBLE_EQ(42.5, dst_val.x);
  EXPECT_STREQ("hello_comp", dst_val.s);

  double sub_dst = 0.0;
  EXPECT_TRUE(GDALExtendedDataType::CopyValue(&src_val, dt_comp, &sub_dst,
                                              dt_sub_comp));
  EXPECT_DOUBLE_EQ(42.5, sub_dst);
  EXPECT_FALSE(GDALExtendedDataType::CopyValue(&sub_dst, dt_sub_comp, &dst_val,
                                               dt_comp));
  dt_f64.FreeDynamicMemory(&sub_dst);
}

TEST(GdalmultidimTest, ExtendedDataTypeCopyValuesAllTypes) {
  const auto dt_str = GDALExtendedDataType::CreateString();
  constexpr auto kAllNumericTypes = std::to_array<GDALDataType>({
      GDT_Byte,
      GDT_Int8,
      GDT_UInt16,
      GDT_Int16,
      GDT_UInt32,
      GDT_Int32,
      GDT_UInt64,
      GDT_Int64,
      GDT_Float16,
      GDT_Float32,
      GDT_Float64,
      GDT_CInt16,
      GDT_CInt32,
      GDT_CFloat16,
      GDT_CFloat32,
      GDT_CFloat64,
  });

  for (GDALDataType eType : kAllNumericTypes) {
    const auto dt_num = GDALExtendedDataType::Create(eType);
    const std::array<double, 2> src_d = {12.0, 3.0};
    std::vector<GByte> num_buf(dt_num.GetSize(), 0);
    GDALCopyWords(src_d.data(), GDT_Float64, sizeof(double), num_buf.data(),
                  eType, static_cast<int>(dt_num.GetSize()), 1);

    // Numeric -> String.
    char* str_out = nullptr;
    absl::Cleanup free_str_out = [&] { dt_str.FreeDynamicMemory(&str_out); };
    EXPECT_TRUE(GDALExtendedDataType::CopyValue(num_buf.data(), dt_num,
                                                &str_out, dt_str));
    ASSERT_NE(nullptr, str_out);

    // String -> Numeric (for real numeric types).
    if (!GDALDataTypeIsComplex(eType)) {
      const char* s_in = "25";
      std::vector<GByte> out_buf(dt_num.GetSize(), 0);
      EXPECT_TRUE(GDALExtendedDataType::CopyValue(&s_in, dt_str, out_buf.data(),
                                                  dt_num));
      const char* s_null = nullptr;
      EXPECT_TRUE(GDALExtendedDataType::CopyValue(&s_null, dt_str,
                                                  out_buf.data(), dt_num));
    }
  }

  // String -> String (non-null and null) and CopyValues loop.
  const std::array<const char*, 2> src_strs = {"first", nullptr};
  std::array<char*, 2> dst_strs = {nullptr, nullptr};
  absl::Cleanup free_dst_strs = [&] {
    dt_str.FreeDynamicMemory(&dst_strs[0]);
    dt_str.FreeDynamicMemory(&dst_strs[1]);
  };
  EXPECT_TRUE(GDALExtendedDataType::CopyValues(src_strs.data(), dt_str, 1,
                                               dst_strs.data(), dt_str, 1, 2));
  EXPECT_STREQ("first", dst_strs[0]);
  EXPECT_EQ(nullptr, dst_strs[1]);

  // Numeric -> Numeric CopyValues.
  const auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const std::array<int32_t, 3> src_i = {10, 20, 30};
  std::array<double, 3> dst_d = {0, 0, 0};
  EXPECT_TRUE(GDALExtendedDataType::CopyValues(src_i.data(), dt_i32, 1,
                                               dst_d.data(), dt_f64, 1, 3));
  EXPECT_THAT(dst_d, ElementsAre(10.0, 20.0, 30.0));

  // Incompatible CopyValue & CopyValues.
  const auto dt_unk = GDALExtendedDataType::Create(GDT_Unknown);
  char* s_out = nullptr;
  EXPECT_TRUE(
      GDALExtendedDataType::CopyValue(src_i.data(), dt_unk, &s_out, dt_str));
  EXPECT_EQ(nullptr, s_out);
  EXPECT_TRUE(GDALExtendedDataType::CopyValues(src_i.data(), dt_unk, 1, &s_out,
                                               dt_str, 1, 1));
  std::unique_ptr<char, CplFreeDeleter> owned_s_out(s_out);
}

TEST(GdalmultidimTest, ExtendedDataTypeCApi) {
  ExtendedDataTypeHPtr h_f64(GDALExtendedDataTypeCreate(GDT_Float64),
                             GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_f64);
  EXPECT_EQ(GEDTC_NUMERIC, GDALExtendedDataTypeGetClass(h_f64.get()));
  EXPECT_EQ(GDT_Float64, GDALExtendedDataTypeGetNumericDataType(h_f64.get()));
  EXPECT_EQ(sizeof(double), GDALExtendedDataTypeGetSize(h_f64.get()));
  EXPECT_EQ(GEDTST_NONE, GDALExtendedDataTypeGetSubType(h_f64.get()));
  EXPECT_EQ(nullptr, GDALExtendedDataTypeGetRAT(h_f64.get()));
  EXPECT_STREQ("", GDALExtendedDataTypeGetName(h_f64.get()));

  ExtendedDataTypeHPtr h_str(GDALExtendedDataTypeCreateString(15),
                             GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_str);
  EXPECT_EQ(GEDTC_STRING, GDALExtendedDataTypeGetClass(h_str.get()));
  EXPECT_EQ(15u, GDALExtendedDataTypeGetMaxStringLength(h_str.get()));

  ExtendedDataTypeHPtr h_str_ex(
      GDALExtendedDataTypeCreateStringEx(0, GEDTST_JSON),
      GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_str_ex);
  EXPECT_EQ(GEDTST_JSON, GDALExtendedDataTypeGetSubType(h_str_ex.get()));

  EDTComponentHPtr h_comp(GDALEDTComponentCreate("field1", 0, h_f64.get()),
                          GDALEDTComponentRelease);
  ASSERT_NE(nullptr, h_comp);
  EXPECT_STREQ("field1", GDALEDTComponentGetName(h_comp.get()));
  EXPECT_EQ(0u, GDALEDTComponentGetOffset(h_comp.get()));
  ExtendedDataTypeHPtr h_comp_type(GDALEDTComponentGetType(h_comp.get()),
                                   GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_comp_type);
  EXPECT_TRUE(GDALExtendedDataTypeEquals(h_f64.get(), h_comp_type.get()));
  h_comp_type.reset();

  GDALEDTComponentH raw_comp = h_comp.get();
  ExtendedDataTypeHPtr h_compound(GDALExtendedDataTypeCreateCompound(
                                      "comp_c", sizeof(double), 1, &raw_comp),
                                  GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_compound);
  EXPECT_EQ(GEDTC_COMPOUND, GDALExtendedDataTypeGetClass(h_compound.get()));
  EXPECT_STREQ("comp_c", GDALExtendedDataTypeGetName(h_compound.get()));

  size_t n_comps = 0;
  GDALEDTComponentH* comps =
      GDALExtendedDataTypeGetComponents(h_compound.get(), &n_comps);
  ASSERT_NE(nullptr, comps);
  absl::Cleanup free_comps = [&] {
    GDALExtendedDataTypeFreeComponents(comps, n_comps);
  };
  EXPECT_EQ(1u, n_comps);
  std::move(free_comps).Invoke();
  GDALExtendedDataTypeFreeComponents(nullptr, 0);

  EXPECT_TRUE(GDALExtendedDataTypeCanConvertTo(h_f64.get(), h_str.get()));
  EXPECT_FALSE(GDALExtendedDataTypeEquals(h_f64.get(), h_str.get()));

  h_comp.reset();
  GDALEDTComponentRelease(nullptr);
  h_compound.reset();
  h_str_ex.reset();
  h_str.reset();
  h_f64.reset();
  GDALExtendedDataTypeRelease(nullptr);
}

// ============================================================================
// 2. GDALIHasAttribute, GDALGroup, GDALDimension Base & CopyFrom Tests
// ============================================================================

TEST(GdalmultidimTest, BaseGroupDimensionAndHasAttribute) {
  auto grp = DummyBaseGroup::Create("", "root", "my_ctx");
  EXPECT_EQ("/", grp->GetName());
  EXPECT_EQ("/", grp->GetFullName());
  EXPECT_EQ("my_ctx", grp->GetContext());

  auto sub = DummyBaseGroup::Create("/", "sub");
  EXPECT_EQ("sub", sub->GetName());
  EXPECT_EQ("/sub", sub->GetFullName());

  auto sub2 = DummyBaseGroup::Create("/sub", "leaf");
  EXPECT_EQ("/sub/leaf", sub2->GetFullName());

  // Base virtuals returning empty/nullptr/false.
  EXPECT_TRUE(grp->GetMDArrayNames().empty());
  EXPECT_EQ(nullptr, grp->OpenMDArray("x"));
  EXPECT_TRUE(grp->GetGroupNames().empty());
  EXPECT_EQ(nullptr, grp->OpenGroup("g"));
  EXPECT_TRUE(grp->GetVectorLayerNames().empty());
  EXPECT_EQ(nullptr, grp->OpenVectorLayer("l"));
  EXPECT_TRUE(grp->GetDimensions().empty());
  EXPECT_EQ(nullptr, grp->GetStructuralInfo());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, grp->CreateGroup("g"));
    EXPECT_FALSE(grp->DeleteGroup("g"));
    EXPECT_EQ(nullptr, grp->CreateDimension("d", "", "", 1));
    EXPECT_EQ(nullptr, grp->CreateMDArray(
                           "a", {}, GDALExtendedDataType::Create(GDT_Byte)));
    EXPECT_FALSE(grp->DeleteMDArray("a"));
    EXPECT_FALSE(grp->Rename("new_name"));
    EXPECT_EQ(nullptr, grp->CreateAttribute(
                           "attr", {}, GDALExtendedDataType::Create(GDT_Byte)));
    EXPECT_FALSE(grp->DeleteAttribute("attr"));
    CPLErrorReset();
  }
  EXPECT_TRUE(grp->GetAttributes().empty());
  EXPECT_EQ(nullptr, grp->GetAttribute("attr"));

  // Add a dummy attribute to test GetAttributeFromAttributes.
  auto attr_str =
      std::make_shared<GDALAttributeString>("/", "my_attr", "val_str");
  grp->AddDummyAttribute(attr_str);
  EXPECT_NE(nullptr, grp->GetAttribute("my_attr"));
  EXPECT_EQ(nullptr, grp->GetAttribute("other_attr"));

  // DataTypes on group (C++ and C API).
  grp->AddDataType(std::make_shared<GDALExtendedDataType>(
      GDALExtendedDataType::Create(GDT_Int16)));
  EXPECT_EQ(1u, grp->GetDataTypes().size());
  GroupHPtr h_grp(new GDALGroupHS(grp), GDALGroupRelease);
  EXPECT_EQ(1u, GDALGroupGetDataTypeCount(h_grp.get()));
  ExtendedDataTypeHPtr h_dt0(GDALGroupGetDataType(h_grp.get(), 0),
                             GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_dt0);
  h_dt0.reset();
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALGroupGetDataType(h_grp.get(), 99));
    EXPECT_EQ(nullptr, GDALGroupGetVectorLayerNames(h_grp.get(), nullptr));
    EXPECT_EQ(nullptr, GDALGroupOpenVectorLayer(h_grp.get(), "layer", nullptr));
    CPLErrorReset();
  }
  h_grp.reset();

  // Rename / Deleted lifecycle on group.
  EXPECT_TRUE(sub2->CallCheckValid());
  sub2->CallBaseRename("leaf_renamed");
  EXPECT_EQ("leaf_renamed", sub2->GetName());
  EXPECT_EQ("/sub/leaf_renamed", sub2->GetFullName());
  sub2->ParentRenamed("/sub_renamed");
  EXPECT_EQ("/sub_renamed/leaf_renamed", sub2->GetFullName());
  sub2->ParentRenamed("");
  EXPECT_EQ("/leaf_renamed", sub2->GetFullName());
  sub2->CallNotifyChildren();
  sub2->ParentDeleted();
  sub->Deleted();
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(sub2->CallCheckValid());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_FALSE(sub->CallCheckValid());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Base GDALDimension & GDALDimensionWeakIndexingVar.
  DummyBaseDimension base_dim("/sub", "dim1", "HORIZONTAL_X", "EAST", 10);
  EXPECT_EQ("dim1", base_dim.GetName());
  EXPECT_EQ("/sub/dim1", base_dim.GetFullName());
  EXPECT_EQ("HORIZONTAL_X", base_dim.GetType());
  EXPECT_EQ("EAST", base_dim.GetDirection());
  EXPECT_EQ(10u, base_dim.GetSize());
  EXPECT_EQ(nullptr, base_dim.GetIndexingVariable());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(base_dim.SetIndexingVariable(nullptr));
    EXPECT_FALSE(base_dim.Rename("dim2"));
    CPLErrorReset();
  }
  base_dim.CallBaseRename("dim2");
  EXPECT_EQ("dim2", base_dim.GetName());
  EXPECT_EQ("/sub/dim2", base_dim.GetFullName());
  base_dim.ParentRenamed("/sub_new");
  EXPECT_EQ("/sub_new/dim2", base_dim.GetFullName());
  base_dim.ParentRenamed("");
  EXPECT_EQ("/dim2", base_dim.GetFullName());
  base_dim.ParentDeleted();

  auto weak_dim = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "wdim", "VERTICAL", "UP", 5);
  EXPECT_EQ(5u, weak_dim->GetSize());
  weak_dim->SetSize(8);
  EXPECT_EQ(8u, weak_dim->GetSize());
}

TEST(GdalmultidimTest, GroupNavigationAndCopyFrom) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_x = src_root->CreateDimension("x", "HORIZONTAL_X", "EAST", 4);
  auto dim_y = src_root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 3);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  auto ar_x = src_root->CreateMDArray("x", {dim_x}, dt_f64);
  const std::array<double, 4> x_vals = {10.0, 20.0, 30.0, 40.0};
  const std::array<GUInt64, 1> start1 = {0};
  const std::array<size_t, 1> count_x = {4};
  ASSERT_TRUE(ar_x->Write(start1.data(), count_x.data(), nullptr, nullptr,
                          dt_f64, x_vals.data()));
  EXPECT_TRUE(dim_x->SetIndexingVariable(ar_x));

  auto root_attr = src_root->CreateAttribute(
      "title", {}, GDALExtendedDataType::CreateString());
  EXPECT_TRUE(root_attr->Write("test_dataset"));

  auto ar_2d = src_root->CreateMDArray("temp", {dim_y, dim_x}, dt_f64);
  const std::array<double, 12> temp_vals = {1, 2, 3, 4,  5,  6,
                                            7, 8, 9, 10, 11, 12};
  const std::array<GUInt64, 2> start2 = {0, 0};
  const std::array<size_t, 2> count2 = {3, 4};
  ASSERT_TRUE(ar_2d->Write(start2.data(), count2.data(), nullptr, nullptr,
                           dt_f64, temp_vals.data()));
  ar_2d->SetUnit("K");
  auto arr_attr = ar_2d->CreateAttribute("long_name", {},
                                         GDALExtendedDataType::CreateString());
  EXPECT_TRUE(arr_attr->Write("temperature"));

  // Subgroup with its own dimension and array.
  auto sub1 = src_root->CreateGroup("sub1");
  ASSERT_NE(nullptr, sub1);
  auto sub_attr = sub1->CreateAttribute("sub_info", {}, dt_f64);
  EXPECT_TRUE(sub_attr->Write(99.5));
  // Create a dimension with same name "x" but different size in subgroup, plus
  // an anonymous/unregistered dimension to exercise disambiguation in CopyFrom.
  auto sub_dim = sub1->CreateDimension("x", "", "", 2);
  auto custom_dim1 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/other", "x", "", "", 2);
  auto custom_dim2 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/other2", "x", "", "", 2);
  auto sub_ar = sub1->CreateMDArray(
      "sub_arr", {sub_dim, custom_dim1, custom_dim2}, dt_f64);
  ASSERT_NE(nullptr, sub_ar);
  auto sub2 = sub1->CreateGroup("sub2");
  ASSERT_NE(nullptr, sub2);
  auto sub2_ar = sub2->CreateMDArray("leaf_arr", {dim_x}, dt_f64);
  ASSERT_NE(nullptr, sub2_ar);

  // Test recursive array listing and fullname navigation.
  const auto full_names = src_root->GetMDArrayFullNamesRecursive();
  EXPECT_EQ(4u, full_names.size());

  EXPECT_NE(nullptr, src_root->OpenMDArrayFromFullname("/temp"));
  EXPECT_NE(nullptr, src_root->OpenMDArrayFromFullname("/sub1/sub_arr"));
  EXPECT_NE(nullptr, src_root->OpenMDArrayFromFullname("/sub1/sub2/leaf_arr"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, src_root->OpenMDArrayFromFullname("invalid_no_slash"));
    EXPECT_EQ(nullptr, src_root->OpenMDArrayFromFullname("/"));
    EXPECT_EQ(nullptr, src_root->OpenMDArrayFromFullname("/no_such_grp/arr"));
    CPLErrorReset();
  }

  // OpenAttributeFromFullname.
  EXPECT_NE(nullptr, src_root->OpenAttributeFromFullname("/title"));
  EXPECT_NE(nullptr, src_root->OpenAttributeFromFullname("/temp/long_name"));
  EXPECT_NE(nullptr, src_root->OpenAttributeFromFullname("/sub1/sub_info"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, src_root->OpenAttributeFromFullname("no_slash"));
    EXPECT_EQ(nullptr,
              src_root->OpenAttributeFromFullname("/nonexistent/attr"));
    CPLErrorReset();
  }

  // OpenGroupFromFullname & OpenDimensionFromFullname.
  EXPECT_EQ(nullptr, src_root->OpenGroupFromFullname("/"));
  EXPECT_NE(nullptr, src_root->OpenGroupFromFullname("/sub1"));
  EXPECT_NE(nullptr, src_root->OpenGroupFromFullname("/sub1/sub2"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, src_root->OpenGroupFromFullname("no_slash"));
    EXPECT_EQ(nullptr, src_root->OpenGroupFromFullname("/no_grp/sub"));
    EXPECT_EQ(nullptr, src_root->OpenDimensionFromFullname("no_slash"));
    EXPECT_EQ(nullptr, src_root->OpenDimensionFromFullname("/"));
    EXPECT_EQ(nullptr, src_root->OpenDimensionFromFullname("/no_grp/d"));
    EXPECT_EQ(nullptr, src_root->OpenDimensionFromFullname("/sub1/no_dim"));
    CPLErrorReset();
  }
  EXPECT_NE(nullptr, src_root->OpenDimensionFromFullname("/x"));
  EXPECT_NE(nullptr, src_root->OpenDimensionFromFullname("/sub1/x"));

  // ResolveMDArray.
  EXPECT_NE(nullptr, src_root->ResolveMDArray("/temp", ""));
  EXPECT_NE(nullptr, src_root->ResolveMDArray("temp", "/"));
  EXPECT_NE(nullptr, src_root->ResolveMDArray("temp", ""));
  EXPECT_NE(nullptr, src_root->ResolveMDArray("sub_arr", "/sub1"));
  EXPECT_NE(nullptr, src_root->ResolveMDArray("temp", "/sub1/sub2"));
  EXPECT_NE(nullptr, src_root->ResolveMDArray("leaf_arr", "/"));
  EXPECT_EQ(nullptr, src_root->ResolveMDArray("does_not_exist", "/sub1"));

  // ClearStatistics and GetTotalCopyCost.
  src_root->ClearStatistics();
  const GUInt64 total_cost = src_root->GetTotalCopyCost();
  EXPECT_GT(total_cost, 0u);

  // CopyFrom with AUTOSCALE and conditional ARRAY: options.
  constexpr auto kAutoscaleTypes = std::to_array<std::string_view>({
      "Byte",
      "Int8",
      "UInt16",
      "Int16",
      "UInt32",
      "Int32",
      "UInt64",
      "Int64",
  });
  for (std::string_view dt_name : kAutoscaleTypes) {
    GDALDatasetUniquePtr dst_ds(
        mem_drv->CreateMultiDimensional("dst", nullptr, nullptr));
    ASSERT_NE(nullptr, dst_ds);
    auto dst_root = dst_ds->GetRootGroup();
    ASSERT_NE(nullptr, dst_root);
    CPLStringList opts;
    opts.AddString("ARRAY:IF(DIM=2):AUTOSCALE=YES");
    const std::string autoscale_opt =
        "ARRAY:IF(NAME=temp):AUTOSCALE_DATA_TYPE=" + std::string(dt_name);
    opts.AddString(autoscale_opt.c_str());
    opts.AddString("ARRAY:IF(DIM=99):IGNORED=1");
    opts.AddString("ARRAY:IF(NAME=/nonexistent):IGNORED=1");
    opts.AddString("ARRAY:SOME_OPTION=VAL");
    GUInt64 cur_cost = 0;
    EXPECT_TRUE(dst_root->CopyFrom(dst_root, src_ds.get(), src_root,
                                   /*bStrict=*/false, cur_cost, total_cost,
                                   GDALDummyProgress, nullptr, opts.List()));
    auto dst_temp = dst_root->OpenMDArray("temp");
    ASSERT_NE(nullptr, dst_temp);
    bool has_scale = false;
    dst_temp->GetScale(&has_scale);
    EXPECT_TRUE(has_scale);
  }

  // Test invalid AUTOSCALE_DATA_TYPE (e.g. Float32) and progress cancellation.
  {
    WithQuietHandler quiet;
    GDALDatasetUniquePtr dst_ds(
        mem_drv->CreateMultiDimensional("dst_bad", nullptr, nullptr));
    ASSERT_NE(nullptr, dst_ds);
    auto dst_root = dst_ds->GetRootGroup();
    ASSERT_NE(nullptr, dst_root);
    CPLStringList bad_opts;
    bad_opts.AddString("ARRAY:AUTOSCALE=YES");
    bad_opts.AddString("ARRAY:AUTOSCALE_DATA_TYPE=Float32");
    GUInt64 cur_cost = 0;
    CPLErrorReset();
    EXPECT_FALSE(dst_root->CopyFrom(dst_root, src_ds.get(), src_root,
                                    /*bStrict=*/true, cur_cost, total_cost,
                                    nullptr, nullptr, bad_opts.List()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    GDALDatasetUniquePtr dst_cancel(
        mem_drv->CreateMultiDimensional("dst_cancel", nullptr, nullptr));
    ASSERT_NE(nullptr, dst_cancel);
    auto cancel_root = dst_cancel->GetRootGroup();
    ASSERT_NE(nullptr, cancel_root);
    cur_cost = 0;
    auto cancel_progress = [](double, const char*, void*) -> int {
      return FALSE;
    };
    CPLErrorReset();
    EXPECT_FALSE(cancel_root->CopyFrom(cancel_root, src_ds.get(), src_root,
                                       /*bStrict=*/true, cur_cost, total_cost,
                                       cancel_progress, nullptr, nullptr));
    CPLErrorReset();
  }
}

// ============================================================================
// 3. GDALAttribute, GDALRawResult, Helper Attribute Classes & C API Tests
// ============================================================================

TEST(GdalmultidimTest, AttributeReadWriteAndHelpers) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("attr_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  const auto dt_i64 = GDALExtendedDataType::Create(GDT_Int64);
  const auto dt_str = GDALExtendedDataType::CreateString();

  // Scalar string attribute.
  auto attr_s = root->CreateAttribute("attr_s", {}, dt_str);
  ASSERT_NE(nullptr, attr_s);
  EXPECT_TRUE(attr_s->GetDimensionsSize().empty());
  EXPECT_TRUE(attr_s->Write("hello_world"));
  EXPECT_STREQ("hello_world", attr_s->ReadAsString());

  // RawResult move constructor, move assignment, operator[], StealData.
  GDALRawResult raw_s = attr_s->ReadAsRaw();
  EXPECT_GT(raw_s.size(), 0u);
  EXPECT_NE(nullptr, raw_s.data());
  GDALRawResult raw_s_moved(std::move(raw_s));
  GDALRawResult raw_s_assigned = attr_s->ReadAsRaw();
  raw_s_assigned = std::move(raw_s_moved);
  EXPECT_EQ(raw_s_assigned.data()[0], raw_s_assigned[0]);

  // Scalar numeric attributes.
  auto attr_i = root->CreateAttribute("attr_i", {}, dt_i32);
  ASSERT_NE(nullptr, attr_i);
  EXPECT_TRUE(attr_i->WriteInt(12345));
  EXPECT_EQ(12345, attr_i->ReadAsInt());
  EXPECT_EQ(12345, attr_i->ReadAsInt64());
  EXPECT_DOUBLE_EQ(12345.0, attr_i->ReadAsDouble());
  EXPECT_STREQ("12345", attr_i->ReadAsString());

  GDALRawResult raw_i = attr_i->ReadAsRaw();
  std::unique_ptr<GByte, CplFreeDeleter> stolen(raw_i.StealData());
  ASSERT_NE(nullptr, stolen);
  stolen.reset();

  auto attr_i64 = root->CreateAttribute("attr_i64", {}, dt_i64);
  ASSERT_NE(nullptr, attr_i64);
  EXPECT_TRUE(attr_i64->WriteInt64(1234567890123LL));
  EXPECT_EQ(1234567890123LL, attr_i64->ReadAsInt64());

  auto attr_d = root->CreateAttribute("attr_d", {}, dt_f64);
  ASSERT_NE(nullptr, attr_d);
  EXPECT_TRUE(attr_d->Write(3.25));
  EXPECT_DOUBLE_EQ(3.25, attr_d->ReadAsDouble());
  double raw_val = 6.5;
  EXPECT_TRUE(attr_d->Write(&raw_val, 1));
  EXPECT_DOUBLE_EQ(6.5, attr_d->ReadAsDouble());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(attr_d->Write(&raw_val, 2));
    CPLErrorReset();
  }

  // 1D array attributes (string, int, int64, double).
  auto attr_sa = root->CreateAttribute("attr_sa", {2}, dt_str);
  ASSERT_NE(nullptr, attr_sa);
  EXPECT_EQ(1u, attr_sa->GetDimensionsSize().size());
  CPLStringList sl;
  sl.AddString("str_one");
  sl.AddString("str_two");
  EXPECT_TRUE(attr_sa->Write(sl.List()));
  const CPLStringList sl_read = attr_sa->ReadAsStringArray();
  EXPECT_THAT(CslToVector(sl_read.List()), ElementsAre("str_one", "str_two"));
  EXPECT_STREQ("str_one", attr_sa->ReadAsString());
  {
    WithQuietHandler quiet;
    CPLStringList sl_bad;
    sl_bad.AddString("only_one");
    CPLErrorReset();
    EXPECT_FALSE(attr_sa->Write(sl_bad.List()));
    CPLErrorReset();
  }

  auto attr_ia = root->CreateAttribute("attr_ia", {3}, dt_i32);
  ASSERT_NE(nullptr, attr_ia);
  const std::array<int, 3> i_vals = {10, 20, 30};
  EXPECT_TRUE(attr_ia->Write(i_vals.data(), 3));
  EXPECT_THAT(attr_ia->ReadAsIntArray(), ElementsAre(10, 20, 30));
  EXPECT_THAT(attr_ia->ReadAsInt64Array(), ElementsAre(10, 20, 30));
  EXPECT_THAT(attr_ia->ReadAsDoubleArray(), ElementsAre(10.0, 20.0, 30.0));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(attr_ia->Write(i_vals.data(), 2));
    CPLErrorReset();
  }

  auto attr_i64a = root->CreateAttribute("attr_i64a", {2}, dt_i64);
  ASSERT_NE(nullptr, attr_i64a);
  const std::array<int64_t, 2> i64_vals = {100LL, 200LL};
  EXPECT_TRUE(attr_i64a->Write(i64_vals.data(), 2));
  EXPECT_THAT(attr_i64a->ReadAsInt64Array(), ElementsAre(100, 200));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(attr_i64a->Write(i64_vals.data(), 1));
    CPLErrorReset();
  }

  auto attr_da = root->CreateAttribute("attr_da", {2}, dt_f64);
  ASSERT_NE(nullptr, attr_da);
  const std::array<double, 2> d_vals = {1.5, 2.5};
  EXPECT_TRUE(attr_da->Write(d_vals.data(), 2));
  EXPECT_THAT(attr_da->ReadAsDoubleArray(), ElementsAre(1.5, 2.5));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(attr_da->Write(d_vals.data(), 1));
    CPLErrorReset();
  }

  // GDALAttributeString, GDALAttributeNumeric, GDALMDIAsAttribute.
  const GDALAttributeString helper_s("/", "hs", "42", GEDTST_JSON);
  EXPECT_TRUE(helper_s.GetDimensions().empty());
  EXPECT_EQ(GEDTC_STRING, helper_s.GetDataType().GetClass());
  EXPECT_STREQ("42", helper_s.ReadAsString());

  const GDALAttributeNumeric helper_d("/", "hd", 12.5);
  EXPECT_TRUE(helper_d.GetDimensions().empty());
  EXPECT_EQ(GDT_Float64, helper_d.GetDataType().GetNumericDataType());
  EXPECT_DOUBLE_EQ(12.5, helper_d.ReadAsDouble());

  const GDALAttributeNumeric helper_i("/", "hi", 77);
  EXPECT_EQ(GDT_Int32, helper_i.GetDataType().GetNumericDataType());
  EXPECT_EQ(77, helper_i.ReadAsInt());

  const GDALAttributeNumeric helper_u32("/", "hu32",
                                        std::vector<GUInt32>{5, 6, 7});
  EXPECT_EQ(1u, helper_u32.GetDimensions().size());
  EXPECT_EQ(GDT_UInt32, helper_u32.GetDataType().GetNumericDataType());
  EXPECT_THAT(helper_u32.ReadAsIntArray(), ElementsAre(5, 6, 7));

  const GDALMDIAsAttribute helper_mdi("mdi_name", "mdi_val");
  EXPECT_TRUE(helper_mdi.GetDimensions().empty());
  EXPECT_EQ(GEDTC_STRING, helper_mdi.GetDataType().GetClass());
  EXPECT_STREQ("mdi_val", helper_mdi.ReadAsString());
}

// ============================================================================
// 4. GDALAbstractMDArray CheckReadWriteParams, Chunking & Protected Helpers
// ============================================================================

TEST(GdalmultidimTest, CheckReadWriteParamsAndChunking) {
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  std::vector<std::unique_ptr<GDALEDTComponent>> dt_comp_comps;
  dt_comp_comps.push_back(std::make_unique<GDALEDTComponent>("a", 0, dt_f64));
  const auto dt_comp = GDALExtendedDataType::Create("c", sizeof(double),
                                                    std::move(dt_comp_comps));

  // 0D array CheckReadWriteParams & ProcessPerChunk.
  auto ar_0d = CustomTestMDArray::Create("/", "ar0", {}, dt_f64);
  EXPECT_EQ(0u, ar_0d->GetDimensionCount());
  EXPECT_EQ(1u, ar_0d->GetTotalElementsCount());
  double v0 = 0.0;
  EXPECT_TRUE(ar_0d->Read(nullptr, nullptr, nullptr, nullptr, dt_f64, &v0, &v0,
                          sizeof(double)));
  {
    WithQuietHandler quiet;
    // Buffer out of bounds of alloc_start/alloc_size for 0D.
    std::array<double, 2> other_buf = {0.0, 0.0};
    CPLErrorReset();
    EXPECT_FALSE(ar_0d->Read(nullptr, nullptr, nullptr, nullptr, dt_f64,
                             &other_buf[1], &other_buf[0], sizeof(double)));
    EXPECT_FALSE(ar_0d->Read(nullptr, nullptr, nullptr, nullptr, dt_f64,
                             &other_buf[0], &other_buf[1], sizeof(double)));
    // Non-convertible type.
    EXPECT_FALSE(ar_0d->Read(nullptr, nullptr, nullptr, nullptr, dt_comp, &v0));
    // Not writable Write() and base IWrite().
    EXPECT_FALSE(ar_0d->Write(nullptr, nullptr, nullptr, nullptr, dt_f64, &v0));
    ar_0d->SetWritable(true);
    EXPECT_FALSE(
        ar_0d->CallBaseIWrite(nullptr, nullptr, nullptr, nullptr, dt_f64, &v0));
    CPLErrorReset();
  }

  int chunk_calls_0d = 0;
  auto chunk_cb = [](GDALAbstractMDArray*, const GUInt64*, const size_t*,
                     GUInt64, GUInt64, void* user_data) -> bool {
    (*static_cast<int*>(user_data))++;
    return true;
  };
  EXPECT_TRUE(ar_0d->ProcessPerChunk(nullptr, nullptr, nullptr, chunk_cb,
                                     &chunk_calls_0d));
  EXPECT_EQ(1, chunk_calls_0d);

  // 2D array (4 x 5) CheckReadWriteParams edge cases.
  auto dim0 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d0", "", "", 4);
  auto dim1 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d1", "", "", 5);
  auto ar_2d = CustomTestMDArray::Create("/", "ar2", {dim0, dim1}, dt_f64);
  ar_2d->SetWritable(true);

  std::vector<double> buf(20, 0.0);
  constexpr std::array<GUInt64, 2> start_ok = {0, 0};
  constexpr std::array<size_t, 2> count_ok = {4, 5};
  // Positive strides with buffer_alloc_start.
  EXPECT_TRUE(ar_2d->Read(start_ok.data(), count_ok.data(), nullptr, nullptr,
                          dt_f64, buf.data(), buf.data(),
                          buf.size() * sizeof(double)));

  // Negative arrayStep and negative bufferStride with buffer_alloc_start.
  // In gdalmultidim.cpp:2156, `paby += bufferStride[i] * (count[i] - 1) *
  // elementSize` promotes signed `bufferStride[i]` (GPtrDiff_t) to unsigned
  // `size_t`, which triggers UBSan pointer-overflow under --config=asan-chlor
  // whenever bufferStride[i] < 0 and count[i] > 1. Using count = 1 on the
  // negative-stride axis (stride_neg = {-2, 1}, count_neg_stride = {1, 2})
  // still sets bufferStride_all_positive = false and exercises the hypercube
  // corner bounds-check loop without unsigned pointer wrap-around.
  constexpr std::array<GUInt64, 2> start_neg = {3, 4};
  constexpr std::array<size_t, 2> count_neg = {2, 2};
  constexpr std::array<size_t, 2> count_neg_stride = {1, 2};
  constexpr std::array<GInt64, 2> step_neg = {-1, -1};
  constexpr std::array<GPtrDiff_t, 2> stride_neg = {-2, 1};
  ar_2d->mock_always_succeed_iread_ = true;
  EXPECT_TRUE(ar_2d->Read(start_neg.data(), count_neg.data(), step_neg.data(),
                          nullptr, dt_f64, buf.data(), buf.data(),
                          4 * sizeof(double)));
  EXPECT_TRUE(ar_2d->Read(start_neg.data(), count_neg_stride.data(),
                          step_neg.data(), stride_neg.data(), dt_f64, &buf[0],
                          buf.data(), 2 * sizeof(double)));
  ar_2d->mock_always_succeed_iread_ = false;

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    // Null buffer check.
    EXPECT_FALSE(ar_2d->Read(start_ok.data(), count_ok.data(), nullptr, nullptr,
                             dt_f64, nullptr));
    // count[i] == 0.
    constexpr std::array<size_t, 2> count_zero = {4, 0};
    EXPECT_FALSE(ar_2d->Read(start_ok.data(), count_zero.data(), nullptr,
                             nullptr, dt_f64, buf.data()));
    // arrayStartIdx out of bounds.
    constexpr std::array<GUInt64, 2> start_oob = {4, 0};
    EXPECT_FALSE(ar_2d->Read(start_oob.data(), count_ok.data(), nullptr,
                             nullptr, dt_f64, buf.data()));
    // Positive step out of bounds.
    constexpr std::array<GInt64, 2> step_oob = {2, 1};
    EXPECT_FALSE(ar_2d->Read(start_ok.data(), count_ok.data(), step_oob.data(),
                             nullptr, dt_f64, buf.data()));
    // Negative step out of bounds.
    constexpr std::array<GUInt64, 2> start_small = {0, 0};
    EXPECT_FALSE(ar_2d->Read(start_small.data(), count_neg.data(),
                             step_neg.data(), nullptr, dt_f64, buf.data()));
    // Negative step INT64_MIN overflow.
    constexpr std::array<GInt64, 2> step_min = {
        std::numeric_limits<GInt64>::min(), 1};
    EXPECT_FALSE(ar_2d->Read(start_neg.data(), count_neg.data(),
                             step_min.data(), nullptr, dt_f64, buf.data()));
    // buffer_alloc_size too small for positive stride.
    EXPECT_FALSE(ar_2d->Read(start_ok.data(), count_ok.data(), nullptr, nullptr,
                             dt_f64, buf.data(), buf.data(),
                             2 * sizeof(double)));
    // buffer_alloc_size too small / out of bounds for negative stride.
    EXPECT_FALSE(ar_2d->Read(start_neg.data(), count_neg_stride.data(),
                             step_neg.data(), stride_neg.data(), dt_f64,
                             &buf[1], buf.data(), 2 * sizeof(double)));
    EXPECT_FALSE(ar_2d->Read(start_neg.data(), count_neg_stride.data(),
                             step_neg.data(), stride_neg.data(), dt_f64,
                             &buf[0], &buf[1], 2 * sizeof(double)));
    CPLErrorReset();
  }

  // GetProcessingChunkSize & ProcessPerChunk on 2D array.
  EXPECT_THAT(ar_2d->GetBlockSize(), ElementsAre(0, 0));
  const auto pcs1 = ar_2d->GetProcessingChunkSize(1024);
  EXPECT_EQ(2u, pcs1.size());
  ar_2d->SetCustomBlockSize({2, 2});
  const auto pcs2 = ar_2d->GetProcessingChunkSize(1024);
  EXPECT_THAT(pcs2, ElementsAre(4, 5));
  const auto pcs3 = ar_2d->GetProcessingChunkSize(16);
  EXPECT_THAT(pcs3, ElementsAre(2, 2));

  const std::array<GUInt64, 2> p_start = {0, 0};
  const std::array<GUInt64, 2> p_count = {4, 5};
  const std::array<size_t, 2> p_chunk = {2, 2};
  int chunk_calls_2d = 0;
  EXPECT_TRUE(ar_2d->ProcessPerChunk(p_start.data(), p_count.data(),
                                     p_chunk.data(), chunk_cb,
                                     &chunk_calls_2d));
  EXPECT_EQ(6, chunk_calls_2d);

  // ProcessPerChunk error cases & callback returning false.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::array<size_t, 2> p_chunk_zero = {2, 0};
    EXPECT_FALSE(ar_2d->ProcessPerChunk(p_start.data(), p_count.data(),
                                        p_chunk_zero.data(), chunk_cb,
                                        &chunk_calls_2d));
    const std::array<GUInt64, 2> p_count_oob = {10, 5};
    EXPECT_FALSE(ar_2d->ProcessPerChunk(p_start.data(), p_count_oob.data(),
                                        p_chunk.data(), chunk_cb,
                                        &chunk_calls_2d));
    auto fail_cb = [](GDALAbstractMDArray*, const GUInt64*, const size_t*,
                      GUInt64, GUInt64, void*) -> bool { return false; };
    EXPECT_FALSE(ar_2d->ProcessPerChunk(p_start.data(), p_count.data(),
                                        p_chunk.data(), fail_cb, nullptr));
    CPLErrorReset();
  }
}

TEST(GdalmultidimTest, MDArrayProtectedTransposedAndContiguousHelpers) {
  auto dim0 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d0", "", "", 2);
  auto dim1 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d1", "", "", 3);
  auto dim2 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d2", "", "", 2);

  // Test ReadForTransposedRequest for 2D (last 2 dims transposed) and 3D
  // (general N-D transpose) across element sizes 1, 2, 4, 8, and strings.
  constexpr auto kSizesDataTypes = std::to_array<GDALDataType>({
      GDT_Byte,
      GDT_UInt16,
      GDT_UInt32,
      GDT_Float64,
  });
  for (GDALDataType eDT : kSizesDataTypes) {
    const auto dt = GDALExtendedDataType::Create(eDT);
    // 2D (2x3) transposed stride: [1, 2].
    auto ar2 = CustomTestMDArray::Create("/", "a2", {dim0, dim1}, dt);
    const std::array<GUInt64, 2> start2 = {0, 0};
    const std::array<size_t, 2> count2 = {2, 3};
    const std::array<GInt64, 2> step2 = {1, 1};
    const std::array<GPtrDiff_t, 2> stride2_trans = {1, 2};
    EXPECT_TRUE(
        ar2->CallIsTransposedRequest(count2.data(), stride2_trans.data()));
    std::vector<GByte> dst2(6 * dt.GetSize(), 0);
    EXPECT_TRUE(ar2->CallReadForTransposedRequest(
        start2.data(), count2.data(), step2.data(), stride2_trans.data(), dt,
        dst2.data()));

    // 3D (2x3x2) transposed [2, 0, 1] -> strides [1, 2, 6].
    auto ar3 = CustomTestMDArray::Create("/", "a3", {dim0, dim1, dim2}, dt);
    const std::array<GUInt64, 3> start3 = {0, 0, 0};
    const std::array<size_t, 3> count3 = {2, 3, 2};
    const std::array<GInt64, 3> step3 = {1, 1, 1};
    const std::array<GPtrDiff_t, 3> stride3_trans = {1, 2, 6};
    EXPECT_TRUE(
        ar3->CallIsTransposedRequest(count3.data(), stride3_trans.data()));
    std::vector<GByte> dst3(12 * dt.GetSize(), 0);
    EXPECT_TRUE(ar3->CallReadForTransposedRequest(
        start3.data(), count3.data(), step3.data(), stride3_trans.data(), dt,
        dst3.data()));
  }

  // 3D with string type (NeedsFreeDynamicMemory() == true) in
  // ReadForTransposedRequest and ReadUsingContiguousIRead.
  const auto dt_str = GDALExtendedDataType::CreateString();
  auto ar_str =
      CustomTestMDArray::Create("/", "as", {dim0, dim1, dim2}, dt_str);
  const std::array<GUInt64, 3> start3 = {0, 0, 0};
  const std::array<size_t, 3> count3 = {2, 3, 2};
  const std::array<GInt64, 3> step3 = {1, 1, 1};
  const std::array<GPtrDiff_t, 3> stride3_trans = {1, 2, 6};
  std::vector<char*> dst_strs(12, nullptr);
  absl::Cleanup free_dst_strs = [&] {
    for (char*& s : dst_strs) {
      CPLFree(s);
      s = nullptr;
    }
  };
  EXPECT_TRUE(ar_str->CallReadForTransposedRequest(
      start3.data(), count3.data(), step3.data(), stride3_trans.data(), dt_str,
      dst_strs.data()));

  // ReadUsingContiguousIRead with negative steps and different bufferDataType.
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  auto ar_num = CustomTestMDArray::Create("/", "an", {dim0, dim1}, dt_f64);
  const std::array<GUInt64, 2> start_neg = {1, 2};
  const std::array<size_t, 2> count_neg = {2, 2};
  const std::array<GInt64, 2> step_neg = {-1, -1};
  const std::array<GPtrDiff_t, 2> stride_contig = {2, 1};
  EXPECT_FALSE(ar_num->CallIsStepOneContiguous(
      count_neg.data(), step_neg.data(), stride_contig.data(), dt_i32));
  std::array<int32_t, 4> out_i32 = {0, 0, 0, 0};
  EXPECT_TRUE(ar_num->CallReadUsingContiguousIRead(
      start_neg.data(), count_neg.data(), step_neg.data(), stride_contig.data(),
      dt_i32, out_i32.data()));

  // Base methods on CustomTestMDArray.
  EXPECT_EQ("", ar_num->GetUnit());
  EXPECT_EQ(nullptr, ar_num->GetSpatialRef());
  EXPECT_EQ(nullptr, ar_num->GetRawNoDataValue());
  bool has_nd = true;
  EXPECT_DOUBLE_EQ(0.0, ar_num->GetNoDataValueAsDouble(&has_nd));
  EXPECT_FALSE(has_nd);
  EXPECT_EQ(std::numeric_limits<int64_t>::min(),
            ar_num->GetNoDataValueAsInt64(&has_nd));
  EXPECT_FALSE(has_nd);
  EXPECT_EQ(std::numeric_limits<uint64_t>::max(),
            ar_num->GetNoDataValueAsUInt64(&has_nd));
  EXPECT_FALSE(has_nd);
  bool has_val = true;
  GDALDataType st = GDT_Byte;
  EXPECT_DOUBLE_EQ(0.0, ar_num->GetOffset(&has_val, &st));
  EXPECT_FALSE(has_val);
  EXPECT_DOUBLE_EQ(1.0, ar_num->GetScale(&has_val, &st));
  EXPECT_FALSE(has_val);
  EXPECT_EQ(nullptr, ar_num->GetStructuralInfo());
  EXPECT_EQ(nullptr, ar_num->GetRootGroup());
  EXPECT_TRUE(ar_num->GetCoordinateVariables().empty());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(ar_num->SetUnit("m"));
    EXPECT_FALSE(ar_num->SetSpatialRef(nullptr));
    EXPECT_FALSE(ar_num->SetRawNoDataValue(nullptr));
    EXPECT_FALSE(ar_num->SetNoDataValue(1.0));
    EXPECT_FALSE(ar_num->SetNoDataValue(static_cast<int64_t>(1)));
    EXPECT_FALSE(ar_num->SetNoDataValue(static_cast<uint64_t>(1)));
    EXPECT_FALSE(ar_num->Resize({4, 4}, nullptr));
    EXPECT_FALSE(ar_num->SetOffset(1.0));
    EXPECT_FALSE(ar_num->SetScale(2.0));
    EXPECT_FALSE(
        ar_num->CallBaseSetStatistics(false, 0, 1, 0.5, 0.1, 4, nullptr));
    EXPECT_FALSE(ar_num->Rename("new_ar"));
    CPLErrorReset();
  }
  ar_num->ClearStatistics();
  EXPECT_TRUE(ar_num->CallIsCacheable());
  EXPECT_EQ("a_b_c_1", CustomTestMDArray::CallMassageName("a/b:c_1"));
  ar_num->CallBaseRename("new_ar");
  EXPECT_EQ("new_ar", ar_num->GetName());
  ar_num->ParentRenamed("/sub");
  EXPECT_EQ("/sub/new_ar", ar_num->GetFullName());
  ar_num->ParentRenamed("");
  EXPECT_EQ("/new_ar", ar_num->GetFullName());
  ar_num->CallNotifyChildren();
  ar_num->ParentDeleted();
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(ar_num->CallCheckValid());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  ar_num->Deleted();
}

TEST(GdalmultidimTest, AdviseReadRegularlySpacedAndGuessGeoTransform) {
  auto dim_large = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "x", "HORIZONTAL_X", "EAST", 10);
  auto dim_y = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "y", "HORIZONTAL_Y", "NORTH", 4);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  // GDALMDArrayRegularlySpaced test.
  auto reg_x =
      GDALMDArrayRegularlySpaced::Create("/", "x", dim_large, 100.0, 10.0, 0.5);
  ASSERT_NE(nullptr, reg_x);
  EXPECT_FALSE(reg_x->IsWritable());
  EXPECT_EQ("", reg_x->GetFilename());
  EXPECT_EQ(1u, reg_x->GetDimensions().size());
  EXPECT_EQ(dt_f64, reg_x->GetDataType());
  auto attr = std::make_shared<GDALAttributeString>("/", "units", "m");
  reg_x->AddAttribute(attr);
  EXPECT_EQ(1u, reg_x->GetAttributes(nullptr).size());

  double start = 0.0;
  double incr = 0.0;
  EXPECT_TRUE(reg_x->IsRegularlySpaced(start, incr));
  EXPECT_DOUBLE_EQ(105.0, start);
  EXPECT_DOUBLE_EQ(10.0, incr);

  // Read with negative step on GDALMDArrayRegularlySpaced.
  const std::array<GUInt64, 1> r_start = {3};
  const std::array<size_t, 1> r_count = {3};
  const std::array<GInt64, 1> r_step = {-1};
  std::array<double, 3> r_vals = {0, 0, 0};
  EXPECT_TRUE(reg_x->Read(r_start.data(), r_count.data(), r_step.data(),
                          nullptr, dt_f64, r_vals.data()));
  EXPECT_THAT(r_vals, ElementsAre(135.0, 125.0, 115.0));

  // Test IsRegularlySpaced two-stage check when nCount >= 5 and nBlockSize <=
  // nCount / 2.
  auto custom_x = CustomTestMDArray::Create("/", "cx", {dim_large}, dt_f64);
  custom_x->SetCustomBlockSize({2});
  auto* cx_ptr = reinterpret_cast<double*>(custom_x->RawData().data());
  for (int i = 0; i < 10; ++i) cx_ptr[i] = 50.0 + i * 5.0;
  EXPECT_TRUE(custom_x->IsRegularlySpaced(start, incr));
  EXPECT_DOUBLE_EQ(50.0, start);
  EXPECT_DOUBLE_EQ(5.0, incr);
  // Make stage 2 irregular so it fails.
  cx_ptr[4] = 999.0;
  EXPECT_FALSE(custom_x->IsRegularlySpaced(start, incr));
  // Make first two equal (dfIncrement == 0).
  cx_ptr[1] = 50.0;
  EXPECT_FALSE(custom_x->IsRegularlySpaced(start, incr));

  // GuessGeoTransform on 2D array with regularly spaced indexing variables.
  auto reg_y =
      GDALMDArrayRegularlySpaced::Create("/", "y", dim_y, 200.0, -20.0, 0.0);
  ASSERT_NE(nullptr, reg_y);
  dim_large->SetIndexingVariable(reg_x);
  dim_y->SetIndexingVariable(reg_y);
  auto ar_grid =
      CustomTestMDArray::Create("/", "grid", {dim_y, dim_large}, dt_f64);

  std::array<double, 6> gt = {0};
  EXPECT_TRUE(
      ar_grid->GuessGeoTransform(1, 0, /*bPixelIsPoint=*/false, gt.data()));
  EXPECT_DOUBLE_EQ(100.0, gt[0]);
  EXPECT_DOUBLE_EQ(10.0, gt[1]);
  EXPECT_DOUBLE_EQ(210.0, gt[3]);
  EXPECT_DOUBLE_EQ(-20.0, gt[5]);

  GDALGeoTransform gdal_gt;
  EXPECT_TRUE(
      ar_grid->GuessGeoTransform(1, 0, /*bPixelIsPoint=*/true, gdal_gt));
  EXPECT_TRUE(ar_grid->GuessGeoTransform(0, 0, false, gdal_gt));
  auto dim_no_iv =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "no_iv", "", "", 2);
  auto ar_no_gt =
      CustomTestMDArray::Create("/", "no_gt", {dim_no_iv, dim_no_iv}, dt_f64);
  EXPECT_FALSE(ar_no_gt->GuessGeoTransform(0, 0, false, gdal_gt));

  // AdviseRead (valid and invalid).
  EXPECT_TRUE(ar_grid->AdviseRead(nullptr, nullptr));
  const std::array<GUInt64, 2> adv_start = {0, 0};
  const std::array<size_t, 2> adv_count = {2, 2};
  EXPECT_TRUE(ar_grid->AdviseRead(adv_start.data(), adv_count.data()));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::array<size_t, 2> adv_bad_count = {10, 2};
    EXPECT_FALSE(ar_grid->AdviseRead(adv_start.data(), adv_bad_count.data()));
    CPLErrorReset();
  }
}

// ============================================================================
// 5. Views: GetView (Sliced & ExtractField), Transpose, GetUnscaled, GetMask
// ============================================================================

TEST(GdalmultidimTest, SlicedAndExtractFieldViews) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("view_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_z = root->CreateDimension("z", "VERTICAL", "UP", 3);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 4);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 5);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  auto ar_x = root->CreateMDArray("x", {dim_x}, dt_f64);
  dim_x->SetIndexingVariable(ar_x);

  auto ar_3d = root->CreateMDArray("cube", {dim_z, dim_y, dim_x}, dt_f64);
  ASSERT_NE(nullptr, ar_3d);
  ar_3d->SetUnit("m/s");
  ar_3d->SetOffset(1.0);
  ar_3d->SetScale(2.0);
  double nd = -999.0;
  ar_3d->SetRawNoDataValue(&nd);
  auto attr = ar_3d->CreateAttribute("info", {}, dt_f64);
  attr->Write(42.0);

  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetDataAxisToSRSAxisMapping({2, 3});
  ar_3d->SetSpatialRef(&srs);

  // Write sequential values to ar_3d.
  std::vector<double> cube_vals(60);
  for (size_t i = 0; i < 60; ++i) cube_vals[i] = static_cast<double>(i);
  const std::array<GUInt64, 3> start3 = {0, 0, 0};
  const std::array<size_t, 3> count3 = {3, 4, 5};
  ASSERT_TRUE(ar_3d->Write(start3.data(), count3.data(), nullptr, nullptr,
                           dt_f64, cube_vals.data()));

  // Various slice expressions.
  auto v_at = ar_3d->at(1, 2, 3);
  ASSERT_NE(nullptr, v_at);
  EXPECT_EQ(0u, v_at->GetDimensionCount());
  double val_0d = 0.0;
  EXPECT_TRUE(v_at->Read(nullptr, nullptr, nullptr, nullptr, dt_f64, &val_0d));
  EXPECT_DOUBLE_EQ(static_cast<double>(1 * 20 + 2 * 5 + 3), val_0d);
  val_0d = 777.0;
  EXPECT_TRUE(v_at->Write(nullptr, nullptr, nullptr, nullptr, dt_f64, &val_0d));
  EXPECT_TRUE(v_at->AdviseRead(nullptr, nullptr));

  auto v_slice1 = ar_3d->GetView("[1,::-1,1:5:2]");
  ASSERT_NE(nullptr, v_slice1);
  EXPECT_TRUE(v_slice1->IsWritable());
  EXPECT_EQ(ar_3d->GetFilename(), v_slice1->GetFilename());
  EXPECT_EQ(2u, v_slice1->GetDimensionCount());
  EXPECT_EQ("m/s", v_slice1->GetUnit());
  EXPECT_NE(nullptr, v_slice1->GetSpatialRef());
  EXPECT_NE(nullptr, v_slice1->GetRawNoDataValue());
  EXPECT_DOUBLE_EQ(1.0, v_slice1->GetOffset());
  EXPECT_DOUBLE_EQ(2.0, v_slice1->GetScale());
  EXPECT_THAT(v_slice1->GetBlockSize(), ElementsAre(0, 0));
  EXPECT_NE(nullptr, v_slice1->GetAttribute("info"));
  EXPECT_EQ(1u, v_slice1->GetAttributes().size());

  const std::array<GUInt64, 2> start2 = {0, 0};
  const std::array<size_t, 2> count2 = {4, 2};
  std::array<double, 8> sub_vals = {0};
  EXPECT_TRUE(v_slice1->Read(start2.data(), count2.data(), nullptr, nullptr,
                             dt_f64, sub_vals.data()));
  EXPECT_TRUE(v_slice1->Write(start2.data(), count2.data(), nullptr, nullptr,
                              dt_f64, sub_vals.data()));
  EXPECT_FALSE(v_slice1->AdviseRead(start2.data(), count2.data()));

  // Chained brackets, ellipsis, newaxis, negative indices.
  EXPECT_NE(nullptr, ar_3d->GetView("[1][2][3]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[...,1]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[1,...]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[newaxis,...,np.newaxis]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[-1,-3:-1:1,3:0:-1]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[:,:2,1:]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[0,0]"));

  // Error cases for GetView.
  constexpr auto kInvalidSliceViews = std::to_array<std::string_view>({
      "",
      "no_brackets",
      "[]",
      "[0",
      "[...,...]",
      "[0,0,0,0]",
      "[10,0,0]",
      "[-10,0,0]",
      "[1:2:3:4,0,0]",
      "[1:2:0,0,0]",
      "[3:1:1,0,0]",
      "[1:3:-1,0,0]",
      "[abc,0,0]",
      "[1:abc,0,0]",
      "[1:2:abc,0,0]",
  });
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, v_at->GetView("[0]"));
    for (std::string_view view_expr : kInvalidSliceViews) {
      EXPECT_EQ(nullptr, ar_3d->GetView(std::string(view_expr)));
    }
    CPLErrorReset();
  }

  // Field extraction on compound array: ['x'], ["a'b\\c"].
  std::vector<std::unique_ptr<GDALEDTComponent>> comps;
  comps.push_back(std::make_unique<GDALEDTComponent>("x", 0, dt_f64));
  comps.push_back(
      std::make_unique<GDALEDTComponent>("a'b\\c", sizeof(double), dt_f64));
  const auto dt_comp = GDALExtendedDataType::Create("comp", 2 * sizeof(double),
                                                    std::move(comps));
  auto ar_comp = root->CreateMDArray("comp_arr", {dim_x}, dt_comp);
  ASSERT_NE(nullptr, ar_comp);
  ar_comp->SetUnit("u");
  ar_comp->SetSpatialRef(&srs);
  const std::array<double, 2> comp_nd = {-1.0, -2.0};
  ar_comp->SetRawNoDataValue(comp_nd.data());
  const std::array<double, 10> comp_vals = {1, 10, 2, 20, 3, 30, 4, 40, 5, 50};
  const std::array<GUInt64, 1> start1 = {0};
  const std::array<size_t, 1> count1 = {5};
  ASSERT_TRUE(ar_comp->Write(start1.data(), count1.data(), nullptr, nullptr,
                             dt_comp, comp_vals.data()));

  auto field_x = (*ar_comp)["x"];
  ASSERT_NE(nullptr, field_x);
  EXPECT_TRUE(field_x->IsWritable());
  EXPECT_EQ(ar_comp->GetFilename(), field_x->GetFilename());
  EXPECT_EQ(1u, field_x->GetDimensionCount());
  EXPECT_EQ(dt_f64, field_x->GetDataType());
  EXPECT_EQ("u", field_x->GetUnit());
  EXPECT_NE(nullptr, field_x->GetSpatialRef());
  EXPECT_NE(nullptr, field_x->GetRawNoDataValue());
  EXPECT_DOUBLE_EQ(-1.0, field_x->GetNoDataValueAsDouble());
  EXPECT_DOUBLE_EQ(0.0, field_x->GetOffset());
  EXPECT_DOUBLE_EQ(1.0, field_x->GetScale());
  EXPECT_EQ(1u, field_x->GetBlockSize().size());
  EXPECT_TRUE(field_x->AdviseRead(start1.data(), count1.data()));

  std::array<double, 5> fx_out = {0};
  EXPECT_TRUE(field_x->Read(start1.data(), count1.data(), nullptr, nullptr,
                            dt_f64, fx_out.data()));
  EXPECT_THAT(fx_out, ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0));

  auto field_esc = ar_comp->GetView("['a\\'b\\\\c']");
  ASSERT_NE(nullptr, field_esc);
  EXPECT_DOUBLE_EQ(-2.0, field_esc->GetNoDataValueAsDouble());
  EXPECT_TRUE(field_esc->Read(start1.data(), count1.data(), nullptr, nullptr,
                              dt_f64, fx_out.data()));
  EXPECT_THAT(fx_out, ElementsAre(10.0, 20.0, 30.0, 40.0, 50.0));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, (*ar_3d)["x"]);
    EXPECT_EQ(nullptr, (*ar_comp)["nonexistent_field"]);
    CPLErrorReset();
  }
}

TEST(GdalmultidimTest, TransposeAndUnscaledViews) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("tu_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 2);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 3);
  const auto dt_i16 = GDALExtendedDataType::Create(GDT_Int16);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  auto ar = root->CreateMDArray("ar", {dim_y, dim_x}, dt_i16);
  ASSERT_NE(nullptr, ar);
  ar->SetUnit("degC");
  ar->SetScale(0.5, GDT_Float32);
  ar->SetOffset(10.0, GDT_Float32);
  const int16_t nd = -99;
  ar->SetRawNoDataValue(&nd);
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetDataAxisToSRSAxisMapping({1, 2});
  ar->SetSpatialRef(&srs);
  auto attr = ar->CreateAttribute("a", {}, dt_f64);
  attr->Write(1.0);

  const std::array<int16_t, 6> vals = {10, 20, -99, 40, 50, 60};
  const std::array<GUInt64, 2> start2 = {0, 0};
  const std::array<size_t, 2> count2 = {2, 3};
  ASSERT_TRUE(ar->Write(start2.data(), count2.data(), nullptr, nullptr, dt_i16,
                        vals.data()));

  // Transpose with newaxis (-1): [1, -1, 0] -> shape (3, 1, 2).
  auto trans = ar->Transpose({1, -1, 0});
  ASSERT_NE(nullptr, trans);
  EXPECT_TRUE(trans->IsWritable());
  EXPECT_EQ(ar->GetFilename(), trans->GetFilename());
  EXPECT_EQ(3u, trans->GetDimensionCount());
  EXPECT_EQ(dt_i16, trans->GetDataType());
  EXPECT_EQ("degC", trans->GetUnit());
  EXPECT_NE(nullptr, trans->GetSpatialRef());
  EXPECT_NE(nullptr, trans->GetRawNoDataValue());
  EXPECT_DOUBLE_EQ(10.0, trans->GetOffset());
  EXPECT_DOUBLE_EQ(0.5, trans->GetScale());
  EXPECT_THAT(trans->GetBlockSize(), ElementsAre(0, 0, 0));
  EXPECT_NE(nullptr, trans->GetAttribute("a"));
  EXPECT_EQ(1u, trans->GetAttributes().size());

  const std::array<GUInt64, 3> start3 = {0, 0, 0};
  const std::array<size_t, 3> count3 = {3, 1, 2};
  std::array<int16_t, 6> trans_vals = {0};
  EXPECT_TRUE(trans->Read(start3.data(), count3.data(), nullptr, nullptr,
                          dt_i16, trans_vals.data()));
  EXPECT_THAT(trans_vals, ElementsAre(10, 40, 20, 50, -99, 60));
  EXPECT_TRUE(trans->Write(start3.data(), count3.data(), nullptr, nullptr,
                           dt_i16, trans_vals.data()));
  EXPECT_TRUE(trans->AdviseRead(start3.data(), count3.data()));

  // Transpose error cases.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ar->Transpose({0}));
    EXPECT_EQ(nullptr, ar->Transpose({0, 5}));
    EXPECT_EQ(nullptr, ar->Transpose({0, 0}));
    CPLErrorReset();
  }

  // GetUnscaled tests.
  // 1) Scale=1, Offset=0 returns self.
  const auto unscaled_self = ar->GetUnscaled(1.0, 0.0);
  EXPECT_EQ(ar.get(), unscaled_self.get());

  // 2) Standard unscaled on Int16 with scale=0.5, offset=10.0.
  auto unscaled = ar->GetUnscaled();
  ASSERT_NE(nullptr, unscaled);
  EXPECT_TRUE(unscaled->IsWritable());
  EXPECT_EQ(ar->GetFilename(), unscaled->GetFilename());
  EXPECT_EQ(2u, unscaled->GetDimensionCount());
  EXPECT_EQ(GDT_Float64, unscaled->GetDataType().GetNumericDataType());
  EXPECT_EQ("degC", unscaled->GetUnit());
  EXPECT_TRUE(unscaled->SetUnit("K"));
  EXPECT_NE(nullptr, unscaled->GetSpatialRef());
  EXPECT_TRUE(unscaled->SetSpatialRef(&srs));
  EXPECT_NE(nullptr, unscaled->GetRawNoDataValue());
  EXPECT_EQ(2u, unscaled->GetBlockSize().size());
  EXPECT_NE(nullptr, unscaled->GetAttribute("a"));
  EXPECT_EQ(1u, unscaled->GetAttributes().size());
  EXPECT_NE(nullptr, unscaled->CreateAttribute("b", {}, dt_f64));
  EXPECT_TRUE(unscaled->AdviseRead(start2.data(), count2.data()));

  // Read as Float64 and as Int32.
  std::array<double, 6> u_f64 = {0};
  EXPECT_TRUE(unscaled->Read(start2.data(), count2.data(), nullptr, nullptr,
                             dt_f64, u_f64.data()));
  EXPECT_DOUBLE_EQ(15.0, u_f64[0]);
  EXPECT_DOUBLE_EQ(20.0, u_f64[1]);
  EXPECT_TRUE(std::isnan(u_f64[2]));

  std::array<int32_t, 6> u_i32 = {0};
  const auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  EXPECT_TRUE(unscaled->Read(start2.data(), count2.data(), nullptr, nullptr,
                             dt_i32, u_i32.data()));

  // Write to unscaled from Float64 and from Int32, including nodata.
  EXPECT_TRUE(unscaled->Write(start2.data(), count2.data(), nullptr, nullptr,
                              dt_f64, u_f64.data()));
  EXPECT_TRUE(unscaled->Write(start2.data(), count2.data(), nullptr, nullptr,
                              dt_i32, u_i32.data()));
  const double new_nd = -500.0;
  EXPECT_TRUE(unscaled->SetRawNoDataValue(&new_nd));

  // 3) 0D unscaled array & complex unscaled array (GDT_CInt16 -> GDT_CFloat64).
  auto ar_0d = ar->at(0, 0)->GetUnscaled(2.0, 5.0, -999.0);
  ASSERT_NE(nullptr, ar_0d);
  double v0 = 0.0;
  EXPECT_TRUE(ar_0d->Read(nullptr, nullptr, nullptr, nullptr, dt_f64, &v0));
  EXPECT_TRUE(ar_0d->Write(nullptr, nullptr, nullptr, nullptr, dt_f64, &v0));

  const auto dt_ci16 = GDALExtendedDataType::Create(GDT_CInt16);
  const auto dt_cf64 = GDALExtendedDataType::Create(GDT_CFloat64);
  auto ar_comp = root->CreateMDArray("c_ar", {dim_x}, dt_ci16);
  auto unscaled_comp = ar_comp->GetUnscaled(2.0, 1.0);
  ASSERT_NE(nullptr, unscaled_comp);
  EXPECT_EQ(GDT_CFloat64, unscaled_comp->GetDataType().GetNumericDataType());
  const std::array<GUInt64, 1> start1 = {0};
  const std::array<size_t, 1> count1 = {3};
  const std::array<double, 6> c_vals = {3.0, 5.0, 7.0, 9.0, 11.0, 13.0};
  EXPECT_TRUE(unscaled_comp->Write(start1.data(), count1.data(), nullptr,
                                   nullptr, dt_cf64, c_vals.data()));
  std::array<double, 6> c_out = {0};
  EXPECT_TRUE(unscaled_comp->Read(start1.data(), count1.data(), nullptr,
                                  nullptr, dt_cf64, c_out.data()));
  EXPECT_DOUBLE_EQ(3.0, c_out[0]);
  EXPECT_DOUBLE_EQ(5.0, c_out[1]);

  // Float32 with scale=-1, offset=0 preserves Float32.
  const auto dt_f32 = GDALExtendedDataType::Create(GDT_Float32);
  auto ar_f32 = root->CreateMDArray("f32_ar", {dim_x}, dt_f32);
  auto unscaled_f32 = ar_f32->GetUnscaled(-1.0, 0.0);
  ASSERT_NE(nullptr, unscaled_f32);
  EXPECT_EQ(GDT_Float32, unscaled_f32->GetDataType().GetNumericDataType());
}

TEST(GdalmultidimTest, MaskViewAllTypesAndFlags) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("mask_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto dim = root->CreateDimension("x", "", "", 5);
  auto dim_y = root->CreateDimension("y", "", "", 2);
  const auto dt_byte = GDALExtendedDataType::Create(GDT_Byte);
  const auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  // 1) Fast path: integer array with no mask attributes/nodata.
  auto ar_fast = root->CreateMDArray("fast", {dim_y, dim}, dt_i32);
  auto mask_fast = ar_fast->GetMask(nullptr);
  ASSERT_NE(nullptr, mask_fast);
  EXPECT_FALSE(mask_fast->IsWritable());
  EXPECT_EQ(ar_fast->GetFilename(), mask_fast->GetFilename());
  EXPECT_EQ(2u, mask_fast->GetDimensionCount());
  EXPECT_EQ(dt_byte, mask_fast->GetDataType());
  EXPECT_EQ(nullptr, mask_fast->GetSpatialRef());
  EXPECT_EQ(2u, mask_fast->GetBlockSize().size());

  const std::array<GUInt64, 2> start2 = {0, 0};
  const std::array<size_t, 2> count2 = {2, 5};
  EXPECT_TRUE(mask_fast->AdviseRead(start2.data(), count2.data()));
  std::array<GByte, 10> m_buf = {0};
  // Contiguous Byte read.
  EXPECT_TRUE(mask_fast->Read(start2.data(), count2.data(), nullptr, nullptr,
                              dt_byte, m_buf.data()));
  EXPECT_EQ(1, m_buf[0]);
  // Strided Byte read.
  const std::array<GPtrDiff_t, 2> stride2 = {10, 2};
  std::vector<GByte> m_strided(20, 0);
  EXPECT_TRUE(mask_fast->Read(start2.data(), count2.data(), nullptr,
                              stride2.data(), dt_byte, m_strided.data()));
  EXPECT_EQ(1, m_strided[0]);
  // Non-Byte buffer read.
  std::array<int32_t, 10> m_i32 = {0};
  EXPECT_TRUE(mask_fast->Read(start2.data(), count2.data(), nullptr, nullptr,
                              dt_i32, m_i32.data()));
  EXPECT_EQ(1, m_i32[0]);

  // 2) General path across all numeric data types with nodata, valid_min,
  // valid_max, missing_value, _FillValue.
  constexpr auto kMaskTypes = std::to_array<GDALDataType>({
      GDT_Byte,
      GDT_Int8,
      GDT_UInt16,
      GDT_Int16,
      GDT_UInt32,
      GDT_Int32,
      GDT_UInt64,
      GDT_Int64,
      GDT_Float16,
      GDT_Float32,
      GDT_Float64,
      GDT_CFloat64,
  });
  const std::array<GUInt64, 1> start1 = {0};
  const std::array<size_t, 1> count1 = {5};
  for (size_t idx = 0; idx < kMaskTypes.size(); ++idx) {
    const GDALDataType eDT = kMaskTypes[idx];
    const auto dt = GDALExtendedDataType::Create(eDT);
    auto ar = root->CreateMDArray(CPLSPrintf("ar_%zu", idx), {dim}, dt);
    ASSERT_NE(nullptr, ar);
    ar->SetNoDataValue(10.0);
    if (!GDALDataTypeIsComplex(eDT)) {
      auto attr_mv = ar->CreateAttribute("missing_value", {1}, dt_f64);
      const double mv = 20.0;
      attr_mv->Write(&mv, 1);
      auto attr_fv = ar->CreateAttribute("_FillValue", {}, dt_f64);
      attr_fv->Write(30.0);
      auto attr_vmin = ar->CreateAttribute("valid_min", {}, dt_f64);
      attr_vmin->Write(2.0);
      auto attr_vmax = ar->CreateAttribute("valid_max", {}, dt_f64);
      attr_vmax->Write(50.0);
    }
    const std::array<double, 10> vals = {1.0, 5.0, 10.0, 20.0, 60.0,
                                         0.0, 0.0, 0.0,  0.0,  0.0};
    EXPECT_TRUE(ar->Write(start1.data(), count1.data(), nullptr, nullptr,
                          dt_f64, vals.data()));

    auto mask = ar->GetMask(nullptr);
    ASSERT_NE(nullptr, mask);
    std::array<GByte, 5> out_b = {9, 9, 9, 9, 9};
    EXPECT_TRUE(mask->Read(start1.data(), count1.data(), nullptr, nullptr,
                           dt_byte, out_b.data()));
    if (!GDALDataTypeIsComplex(eDT)) {
      EXPECT_THAT(out_b, ElementsAre(0, 1, 0, 0, 0));
    }
    // Also test non-Byte buffer and strided buffer on general path.
    std::array<int32_t, 5> out_i = {0};
    EXPECT_TRUE(mask->Read(start1.data(), count1.data(), nullptr, nullptr,
                           dt_i32, out_i.data()));
    const std::array<GPtrDiff_t, 1> stride_2 = {2};
    std::array<GByte, 10> out_strided = {0};
    EXPECT_TRUE(mask->Read(start1.data(), count1.data(), nullptr,
                           stride_2.data(), dt_byte, out_strided.data()));
  }

  // 3) valid_range attribute & UNMASK_FLAGS with flag_meanings, flag_values,
  // flag_masks.
  auto ar_flags = root->CreateMDArray("flags_ar", {dim}, dt_byte);
  auto vr = ar_flags->CreateAttribute("valid_range", {2}, dt_byte);
  const std::array<int, 2> vr_vals = {0, 10};
  vr->Write(vr_vals.data(), 2);
  auto fm = ar_flags->CreateAttribute("flag_meanings", {},
                                      GDALExtendedDataType::CreateString());
  fm->Write("low_quality cloud_cover valid_snow");
  auto fv = ar_flags->CreateAttribute("flag_values", {3}, dt_byte);
  const std::array<int, 3> fv_vals = {1, 2, 6};
  fv->Write(fv_vals.data(), 3);
  auto fmask = ar_flags->CreateAttribute("flag_masks", {3}, dt_byte);
  const std::array<int, 3> fmask_vals = {1, 2, 6};
  fmask->Write(fmask_vals.data(), 3);

  const std::array<GByte, 5> flag_data = {0, 1, 2, 6, 20};
  ar_flags->Write(start1.data(), count1.data(), nullptr, nullptr, dt_byte,
                  flag_data.data());

  CPLStringList unmask_opts;
  unmask_opts.SetNameValue("UNMASK_FLAGS", "valid_snow,low_quality");
  auto mask_flags = ar_flags->GetMask(unmask_opts.List());
  ASSERT_NE(nullptr, mask_flags);
  std::array<GByte, 5> out_f = {0};
  EXPECT_TRUE(mask_flags->Read(start1.data(), count1.data(), nullptr, nullptr,
                               dt_byte, out_f.data()));
  EXPECT_THAT(out_f, ElementsAre(0, 1, 0, 1, 0));

  // UNMASK_FLAGS error cases.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList bad_flag_opts;
    bad_flag_opts.SetNameValue("UNMASK_FLAGS", "unknown_flag");
    EXPECT_EQ(nullptr, ar_flags->GetMask(bad_flag_opts.List()));
    EXPECT_EQ(nullptr, ar_fast->GetMask(unmask_opts.List()));
    auto ar_str = root->CreateMDArray("str_ar", {dim},
                                      GDALExtendedDataType::CreateString());
    EXPECT_EQ(nullptr, ar_str->GetMask(nullptr));
    CPLErrorReset();
  }
}

// ============================================================================
// 6. GetResampled & AsClassicDataset & GDALMDRasterIOFromBand Tests
// ============================================================================

class CoordVarsTestMDArray : public CustomTestMDArray {
 public:
  CoordVarsTestMDArray(const std::string& parent_name, const std::string& name,
                       std::vector<std::shared_ptr<GDALDimension>> dims,
                       const GDALExtendedDataType& dt)
      : GDALAbstractMDArray(parent_name, name),
        CustomTestMDArray(parent_name, name, std::move(dims), dt) {}

  static std::shared_ptr<CoordVarsTestMDArray> Create(
      const std::string& parent_name, const std::string& name,
      std::vector<std::shared_ptr<GDALDimension>> dims,
      const GDALExtendedDataType& dt) {
    auto ar = std::make_shared<CoordVarsTestMDArray>(parent_name, name,
                                                     std::move(dims), dt);
    ar->SetSelf(ar);
    return ar;
  }

  void SetCoordVars(std::vector<std::shared_ptr<GDALMDArray>> cvs) {
    cvs_ = std::move(cvs);
  }
  std::vector<std::shared_ptr<GDALMDArray>> GetCoordinateVariables()
      const override {
    return cvs_;
  }

 private:
  std::vector<std::shared_ptr<GDALMDArray>> cvs_;
};

TEST(GdalmultidimTest, GetResampled2DAnd3DAndGeolocAndGLT) {
  GDALRegister_MEM();
  GDALRegister_VRT();
  GDALRegister_GTiff();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("VRT"));
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("GTiff"));
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("res_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_z = root->CreateDimension("z", "VERTICAL", "UP", 2);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 4);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 4);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  auto reg_x =
      GDALMDArrayRegularlySpaced::Create("/", "x", dim_x, 0.5, 1.0, 0.0);
  auto reg_y =
      GDALMDArrayRegularlySpaced::Create("/", "y", dim_y, 3.5, -1.0, 0.0);
  dim_x->SetIndexingVariable(reg_x);
  dim_y->SetIndexingVariable(reg_y);

  auto ar_3d = root->CreateMDArray("ar3", {dim_z, dim_y, dim_x}, dt_f64);
  ASSERT_NE(nullptr, ar_3d);
  ar_3d->SetUnit("m");
  ar_3d->SetOffset(1.0);
  ar_3d->SetScale(2.0);
  const double nd = -999.0;
  ar_3d->SetRawNoDataValue(&nd);
  auto attr = ar_3d->CreateAttribute("k", {}, dt_f64);
  attr->Write(5.0);
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  ar_3d->SetSpatialRef(&srs);

  const std::vector<double> data3(32, 10.0);
  const std::array<GUInt64, 3> start3 = {0, 0, 0};
  const std::array<size_t, 3> count3 = {2, 4, 4};
  ASSERT_TRUE(ar_3d->Write(start3.data(), count3.data(), nullptr, nullptr,
                           dt_f64, data3.data()));

  // All supported resample algorithms.
  constexpr auto kResampleAlgs = std::to_array<GDALRIOResampleAlg>({
      GRIORA_NearestNeighbour,
      GRIORA_Bilinear,
      GRIORA_Cubic,
      GRIORA_CubicSpline,
      GRIORA_Lanczos,
      GRIORA_Average,
      GRIORA_Mode,
      GRIORA_RMS,
  });
  for (GDALRIOResampleAlg alg : kResampleAlgs) {
    auto res =
        ar_3d->GetResampled({dim_z, nullptr, nullptr}, alg, nullptr, nullptr);
    ASSERT_NE(nullptr, res);
    EXPECT_FALSE(res->IsWritable());
    EXPECT_EQ(ar_3d->GetFilename(), res->GetFilename());
    EXPECT_EQ(3u, res->GetDimensionCount());
    EXPECT_EQ(dt_f64, res->GetDataType());
    EXPECT_NE(nullptr, res->GetSpatialRef());
    EXPECT_EQ(3u, res->GetBlockSize().size());
    EXPECT_NE(nullptr, res->GetAttribute("k"));
    EXPECT_EQ(1u, res->GetAttributes().size());
    EXPECT_EQ("m", res->GetUnit());
    EXPECT_NE(nullptr, res->GetRawNoDataValue());
    EXPECT_DOUBLE_EQ(1.0, res->GetOffset());
    EXPECT_DOUBLE_EQ(2.0, res->GetScale());
  }

  // Read from resampled 3D array with explicit new X and Y dimensions.
  auto new_dim_y =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "ny", "", "", 2);
  auto new_dim_x =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "nx", "", "", 2);
  auto new_reg_x =
      GDALMDArrayRegularlySpaced::Create("/", "nx", new_dim_x, 1.0, 2.0, 0.0);
  auto new_reg_y =
      GDALMDArrayRegularlySpaced::Create("/", "ny", new_dim_y, 3.0, -2.0, 0.0);
  new_dim_x->SetIndexingVariable(new_reg_x);
  new_dim_y->SetIndexingVariable(new_reg_y);

  auto res_3d = ar_3d->GetResampled({dim_z, new_dim_y, new_dim_x},
                                    GRIORA_Bilinear, &srs, nullptr);
  ASSERT_NE(nullptr, res_3d);
  const std::array<size_t, 3> res_count3 = {2, 2, 2};
  std::array<double, 8> res_out = {0};
  EXPECT_TRUE(res_3d->Read(start3.data(), res_count3.data(), nullptr, nullptr,
                           dt_f64, res_out.data()));
  constexpr double kResampleTolerance = 1e-5;
  EXPECT_NEAR(10.0, res_out[0], kResampleTolerance);

  // Also test GetResampled with only new_dim_x or only new_dim_y, and without
  // indexing variables on new dims.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              ar_3d->GetResampled({dim_z, nullptr, new_dim_x},
                                  GRIORA_NearestNeighbour, nullptr, nullptr));
    EXPECT_EQ(nullptr,
              ar_3d->GetResampled({dim_z, new_dim_y, nullptr},
                                  GRIORA_NearestNeighbour, nullptr, nullptr));
    CPLErrorReset();
  }
  auto bare_dim_y =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "by", "", "", 2);
  auto bare_dim_x =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "bx", "", "", 2);
  EXPECT_NE(nullptr,
            ar_3d->GetResampled({dim_z, bare_dim_y, bare_dim_x},
                                GRIORA_NearestNeighbour, nullptr, nullptr));

  // Geolocation (GetCoordinateVariables) path in GetResampled: 2D lon/lat.
  auto dim_y4 = root->CreateDimension("y4", "", "", 4);
  auto dim_x4 = root->CreateDimension("x4", "", "", 4);
  auto ar_lon = root->CreateMDArray("lon", {dim_y4, dim_x4}, dt_f64);
  auto ar_lat = root->CreateMDArray("lat", {dim_y4, dim_x4}, dt_f64);
  std::array<double, 16> lon_vals = {};
  std::array<double, 16> lat_vals = {};
  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      lon_vals[j * 4 + i] = i * 1.0;
      lat_vals[j * 4 + i] = 3.0 - j * 1.0;
    }
  }
  const std::array<GUInt64, 2> start2 = {0, 0};
  const std::array<size_t, 2> count2 = {4, 4};
  ASSERT_TRUE(ar_lon->Write(start2.data(), count2.data(), nullptr, nullptr,
                            dt_f64, lon_vals.data()));
  ASSERT_TRUE(ar_lat->Write(start2.data(), count2.data(), nullptr, nullptr,
                            dt_f64, lat_vals.data()));

  auto cv_ar =
      CoordVarsTestMDArray::Create("/", "cv_ar", {dim_y4, dim_x4}, dt_f64);
  cv_ar->SetCoordVars({ar_lon, ar_lat});
  auto res_cv = cv_ar->GetResampled({nullptr, nullptr}, GRIORA_NearestNeighbour,
                                    nullptr, nullptr);
  ASSERT_NE(nullptr, res_cv);
  double sample = 0.0;
  const std::array<size_t, 2> c1 = {1, 1};
  EXPECT_TRUE(res_cv->Read(start2.data(), c1.data(), nullptr, nullptr, dt_f64,
                           &sample));
  GDALDatasetUniquePtr ds_res(res_cv->AsClassicDataset(1, 0, nullptr, nullptr));
  ASSERT_NE(nullptr, ds_res);
  ASSERT_GT(ds_res->GetRasterCount(), 0);
  int nBXSize = 0;
  int nBYSize = 0;
  ds_res->GetRasterBand(1)->GetBlockSize(&nBXSize, &nBYSize);
  std::vector<double> block(static_cast<size_t>(nBXSize) * nBYSize, 0.0);
  EXPECT_EQ(CE_None, ds_res->GetRasterBand(1)->ReadBlock(0, 0, block.data()));

  // NASA EMIT GLT orthorectification path in GetResampled.
  auto dim_dt = root->CreateDimension("downtrack", "", "", 4);
  auto dim_ct = root->CreateDimension("crosstrack", "", "", 4);
  auto dim_bands = root->CreateDimension("bands", "", "", 2);
  auto gt_attr = root->CreateAttribute("geotransform", {6}, dt_f64);
  const std::array<double, 6> emit_gt = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  gt_attr->Write(emit_gt.data(), 6);
  auto loc_grp = root->CreateGroup("location");
  auto dim_oy = loc_grp->CreateDimension("ortho_y", "", "", 3);
  auto dim_ox = loc_grp->CreateDimension("ortho_x", "", "", 3);
  const auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  auto glt_x = loc_grp->CreateMDArray("glt_x", {dim_oy, dim_ox}, dt_i32);
  auto glt_y = loc_grp->CreateMDArray("glt_y", {dim_oy, dim_ox}, dt_i32);
  const std::array<int32_t, 9> glt_vals = {1, 2, 3, 1, 2, 3, 1, 2, 3};
  const std::array<size_t, 2> count_glt = {3, 3};
  ASSERT_TRUE(glt_x->Write(start2.data(), count_glt.data(), nullptr, nullptr,
                           dt_i32, glt_vals.data()));
  ASSERT_TRUE(glt_y->Write(start2.data(), count_glt.data(), nullptr, nullptr,
                           dt_i32, glt_vals.data()));

  auto emit_ar = CustomTestMDArray::Create("/", "reflectance",
                                           {dim_dt, dim_ct, dim_bands}, dt_f64);
  emit_ar->SetRootGroup(root);
  auto emit_res = emit_ar->GetResampled(
      {nullptr, nullptr, dim_bands}, GRIORA_NearestNeighbour, nullptr, nullptr);
  ASSERT_NE(nullptr, emit_res);
  const std::array<size_t, 3> emit_count = {3, 3, 2};
  std::vector<double> emit_out(18, 0.0);
  EXPECT_TRUE(emit_res->Read(start3.data(), emit_count.data(), nullptr, nullptr,
                             dt_f64, emit_out.data()));

  // GetResampled error cases.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, reg_x->GetResampled({dim_x}, GRIORA_NearestNeighbour,
                                           nullptr, nullptr));
    EXPECT_EQ(nullptr, ar_3d->GetResampled({dim_z, nullptr, nullptr},
                                           GRIORA_Gauss, nullptr, nullptr));
    EXPECT_EQ(nullptr, ar_3d->GetResampled({dim_z}, GRIORA_NearestNeighbour,
                                           nullptr, nullptr));
    EXPECT_EQ(nullptr,
              ar_3d->GetResampled({dim_x, nullptr, nullptr},
                                  GRIORA_NearestNeighbour, nullptr, nullptr));
    auto mixed_dim_x =
        std::make_shared<GDALDimensionWeakIndexingVar>("/", "mx", "", "", 2);
    EXPECT_EQ(nullptr,
              ar_3d->GetResampled({dim_z, new_dim_y, mixed_dim_x},
                                  GRIORA_NearestNeighbour, nullptr, nullptr));
    CPLErrorReset();
  }
}

TEST(GdalmultidimTest, AsClassicDatasetAndBandMetadata) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("classic_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_b = root->CreateDimension("band", "PARAMETRIC", "", 2);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 3);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 4);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const auto dt_str = GDALExtendedDataType::CreateString();

  auto ar_b = root->CreateMDArray("band_vals", {dim_b}, dt_f64);
  ar_b->SetUnit("um");
  const std::array<double, 2> b_vals = {0.45, 0.55};
  const std::array<GUInt64, 1> start1 = {0};
  const std::array<size_t, 1> count_b = {2};
  ASSERT_TRUE(ar_b->Write(start1.data(), count_b.data(), nullptr, nullptr,
                          dt_f64, b_vals.data()));
  auto b_attr = ar_b->CreateAttribute("units", {}, dt_str);
  b_attr->Write("nm");
  dim_b->SetIndexingVariable(ar_b);

  auto ar_fwhm = root->CreateMDArray("fwhm_vals", {dim_b}, dt_f64);
  const std::array<double, 2> f_vals = {10.0, 20.0};
  ASSERT_TRUE(ar_fwhm->Write(start1.data(), count_b.data(), nullptr, nullptr,
                             dt_f64, f_vals.data()));

  auto root_wl_attr = root->CreateAttribute("wl_attr", {2}, dt_f64);
  root_wl_attr->Write(b_vals.data(), 2);
  auto root_unit_attr = root->CreateAttribute("global_unit", {}, dt_str);
  root_unit_attr->Write("um");

  auto reg_x =
      GDALMDArrayRegularlySpaced::Create("/", "x", dim_x, 100.0, 10.0, 0.0);
  auto reg_y =
      GDALMDArrayRegularlySpaced::Create("/", "y", dim_y, 200.0, -10.0, 0.0);
  dim_x->SetIndexingVariable(reg_x);
  dim_y->SetIndexingVariable(reg_y);

  auto ar_3d = root->CreateMDArray("img3d", {dim_b, dim_y, dim_x}, dt_f64);
  ASSERT_NE(nullptr, ar_3d);
  ar_3d->SetUnit("W");
  ar_3d->SetOffset(2.0);
  ar_3d->SetScale(3.0);
  ar_3d->SetNoDataValue(-999.0);
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetDataAxisToSRSAxisMapping({2, 3});
  ar_3d->SetSpatialRef(&srs);

  auto ci_attr = ar_3d->CreateAttribute("COLOR_INTERPRETATION", {2}, dt_str);
  CPLStringList ci_list;
  ci_list.AddString("Red");
  ci_list.AddString("Green");
  ci_attr->Write(ci_list.List());
  auto gen_attr = ar_3d->CreateAttribute("sensor", {}, dt_str);
  gen_attr->Write("landsat");

  // Options with BAND_METADATA and BAND_IMAGERY_METADATA.
  CPLStringList opts;
  opts.SetNameValue("BAND_METADATA",
                    "[{\"array\":\"/band_vals\",\"item_name\":\"WL\","
                    "\"item_value\":\"%% %.2f ${units} ${/global_unit}\"},"
                    "{\"attribute\":\"/wl_attr\",\"item_name\":\"ATTR_WL\","
                    "\"item_value\":\"%s\"}]");
  opts.SetNameValue(
      "BAND_IMAGERY_METADATA",
      "{\"CENTRAL_WAVELENGTH_UM\":{\"array\":\"/"
      "band_vals\",\"unit\":\"${units}\"},"
      "\"FWHM_UM\":{\"attribute\":\"/wl_attr\",\"unit\":\"${/global_unit}\"}}");

  GDALDatasetUniquePtr classic_ds(
      ar_3d->AsClassicDataset(2, 1, root, opts.List()));
  ASSERT_NE(nullptr, classic_ds);
  EXPECT_EQ(4, classic_ds->GetRasterXSize());
  EXPECT_EQ(3, classic_ds->GetRasterYSize());
  EXPECT_EQ(2, classic_ds->GetRasterCount());
  EXPECT_NE(nullptr, classic_ds->GetSpatialRef());
  GDALGeoTransform gt;
  EXPECT_EQ(CE_None, classic_ds->GetGeoTransform(gt));
  EXPECT_STREQ("landsat", classic_ds->GetMetadataItem("sensor"));
  EXPECT_EQ(nullptr, classic_ds->GetMetadata("OVERVIEWS"));
  EXPECT_EQ(nullptr, classic_ds->GetMetadataItem("OVERVIEW_FILE", "OVERVIEWS"));

  GDALRasterBand* b1 = classic_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, b1);
  EXPECT_EQ(GCI_RedBand, b1->GetColorInterpretation());
  EXPECT_STREQ("W", b1->GetUnitType());
  int has_val = 0;
  EXPECT_DOUBLE_EQ(-999.0, b1->GetNoDataValue(&has_val));
  EXPECT_NE(0, has_val);
  EXPECT_DOUBLE_EQ(2.0, b1->GetOffset(&has_val));
  EXPECT_NE(0, has_val);
  EXPECT_DOUBLE_EQ(3.0, b1->GetScale(&has_val));
  EXPECT_NE(0, has_val);
  EXPECT_STREQ("0", b1->GetMetadataItem("DIM_band_INDEX"));
  EXPECT_NE(nullptr, b1->GetMetadataItem("WL"));
  EXPECT_NE(nullptr, b1->GetMetadataItem("ATTR_WL"));
  EXPECT_NE(nullptr, b1->GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY"));

  // Write and read blocks and IRasterIO (including resampled IRasterIO) on
  // GDALRasterBandFromArray.
  const std::vector<double> block_data(12, 42.0);
  EXPECT_EQ(CE_None,
            b1->WriteBlock(0, 0, const_cast<double*>(block_data.data())));
  std::vector<double> read_data(12, 0.0);
  EXPECT_EQ(CE_None, b1->ReadBlock(0, 0, read_data.data()));
  EXPECT_DOUBLE_EQ(42.0, read_data[0]);

  std::array<double, 4> resampled_buf = {0};
  EXPECT_EQ(CE_None, b1->RasterIO(GF_Read, 0, 0, 4, 3, resampled_buf.data(), 2,
                                  2, GDT_Float64, 0, 0, nullptr));
  EXPECT_DOUBLE_EQ(42.0, resampled_buf[0]);

  // Also test Int64 and UInt64 nodata on GDALRasterBandFromArray, and 1D
  // AsClassicDataset, and GDALMDRasterIOFromBand.
  const auto dt_i64 = GDALExtendedDataType::Create(GDT_Int64);
  const auto dt_u64 = GDALExtendedDataType::Create(GDT_UInt64);
  auto ar_i64 = root->CreateMDArray("ar_i64", {dim_x}, dt_i64);
  ar_i64->SetNoDataValue(static_cast<int64_t>(-12345));
  auto ar_u64 = root->CreateMDArray("ar_u64", {dim_x}, dt_u64);
  ar_u64->SetNoDataValue(static_cast<uint64_t>(54321));

  GDALDatasetUniquePtr ds_1d_i64(ar_i64->AsClassicDataset(0, 0));
  ASSERT_NE(nullptr, ds_1d_i64);
  EXPECT_EQ(-12345,
            ds_1d_i64->GetRasterBand(1)->GetNoDataValueAsInt64(&has_val));
  EXPECT_NE(0, has_val);

  GDALDatasetUniquePtr ds_1d_u64(ar_u64->AsClassicDataset(0, 0));
  ASSERT_NE(nullptr, ds_1d_u64);
  EXPECT_EQ(54321u,
            ds_1d_u64->GetRasterBand(1)->GetNoDataValueAsUInt64(&has_val));
  EXPECT_NE(0, has_val);

  constexpr std::string_view kClassicDir =
      "/vsimem/gdalmultidim_test/as_classic";
  absl::Cleanup cleanup_classic_dir = [kClassicDir] {
    VSIRmdirRecursive(std::string(kClassicDir).c_str());
  };
  auto ar_with_fn = CustomTestMDArray::Create(
      "/", "with_fn", {dim_y, dim_x}, dt_f64,
      "/vsimem/gdalmultidim_test/as_classic/dummy.dat", "my_ctx");
  GDALDatasetUniquePtr ds_fn(
      ar_with_fn->AsClassicDataset(1, 0, nullptr, nullptr));
  ASSERT_NE(nullptr, ds_fn);

  // Test GDALMDRasterIOFromBand directly (read & write, strided & negative
  // step).
  const std::array<GUInt64, 2> md_start = {0, 0};
  const std::array<size_t, 2> md_count = {3, 4};
  const std::array<GInt64, 2> md_step = {1, 1};
  const std::array<GPtrDiff_t, 2> md_stride = {4, 1};
  EXPECT_TRUE(GDALMDRasterIOFromBand(
      b1, GF_Write, 1, 0, md_start.data(), md_count.data(), md_step.data(),
      md_stride.data(), dt_f64, const_cast<double*>(block_data.data())));
  EXPECT_TRUE(GDALMDRasterIOFromBand(
      b1, GF_Read, 1, 0, md_start.data(), md_count.data(), md_step.data(),
      md_stride.data(), dt_f64, read_data.data()));
  const std::array<GUInt64, 2> md_start_neg = {2, 3};
  const std::array<size_t, 2> md_count_neg = {2, 2};
  const std::array<GInt64, 2> md_step_neg = {-1, -1};
  const std::array<GPtrDiff_t, 2> md_stride_neg = {2, 1};
  EXPECT_TRUE(GDALMDRasterIOFromBand(
      b1, GF_Read, 1, 0, md_start_neg.data(), md_count_neg.data(),
      md_step_neg.data(), md_stride_neg.data(), dt_f64, read_data.data()));

  // Error cases in AsClassicDataset BAND_METADATA / BAND_IMAGERY_METADATA.
  {
    WithQuietHandler quiet;
    CPLStringList bad_band_meta_opts;
    bad_band_meta_opts.SetNameValue("BAND_METADATA", "not_json");
    CPLErrorReset();
    GDALDatasetUniquePtr ds_bad_meta(
        ar_3d->AsClassicDataset(2, 1, root, bad_band_meta_opts.List()));
    EXPECT_EQ(nullptr, ds_bad_meta);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLStringList bad_imagery_meta_opts;
    bad_imagery_meta_opts.SetNameValue("BAND_IMAGERY_METADATA", "not_json");
    CPLErrorReset();
    GDALDatasetUniquePtr ds_bad_imagery(
        ar_3d->AsClassicDataset(2, 1, root, bad_imagery_meta_opts.List()));
    EXPECT_EQ(nullptr, ds_bad_imagery);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

// ============================================================================
// 7. Statistics, Cache, GDALPamMultiDim / GDALPamMDArray & Full C API Tests
// ============================================================================

TEST(GdalmultidimTest, StatisticsAndPamMultiDim) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("stats_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_y = root->CreateDimension("y", "", "", 2);
  auto dim_x = root->CreateDimension("x", "", "", 3);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);

  // 1) ComputeStatistics on Float64 and Int32 arrays with nodata.
  auto ar_f64 = root->CreateMDArray("ar_f64", {dim_y, dim_x}, dt_f64);
  ASSERT_NE(nullptr, ar_f64);
  ar_f64->SetNoDataValue(-999.0);
  const std::array<double, 6> vals_f64 = {1.0, 2.0, -999.0, 3.0, 4.0, 5.0};
  const std::array<GUInt64, 2> start2 = {0, 0};
  const std::array<size_t, 2> count2 = {2, 3};
  ASSERT_TRUE(ar_f64->Write(start2.data(), count2.data(), nullptr, nullptr,
                            dt_f64, vals_f64.data()));

  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
  GUInt64 valid_count = 0;
  // bForce = false returns CE_Warning when stats not cached.
  EXPECT_EQ(CE_Warning,
            ar_f64->GetStatistics(false, /*bForce=*/false, &min, &max, &mean,
                                  &stddev, &valid_count, nullptr, nullptr));
  // bForce = true computes stats.
  EXPECT_EQ(CE_None, ar_f64->GetStatistics(false, /*bForce=*/true, &min, &max,
                                           &mean, &stddev, &valid_count,
                                           GDALDummyProgress, nullptr));
  EXPECT_DOUBLE_EQ(1.0, min);
  EXPECT_DOUBLE_EQ(5.0, max);
  EXPECT_DOUBLE_EQ(3.0, mean);
  EXPECT_EQ(5u, valid_count);

  // Int32 array (non-Float64 promotion path in ComputeStatistics).
  auto ar_i32 = root->CreateMDArray("ar_i32", {dim_y, dim_x}, dt_i32);
  ASSERT_NE(nullptr, ar_i32);
  const std::array<int32_t, 6> vals_i32 = {10, 20, 30, 40, 50, 60};
  ASSERT_TRUE(ar_i32->Write(start2.data(), count2.data(), nullptr, nullptr,
                            dt_i32, vals_i32.data()));
  EXPECT_TRUE(ar_i32->ComputeStatistics(false, &min, &max, &mean, &stddev,
                                        &valid_count, nullptr, nullptr,
                                        nullptr));
  EXPECT_DOUBLE_EQ(10.0, min);
  EXPECT_DOUBLE_EQ(60.0, max);
  EXPECT_EQ(6u, valid_count);

  // Error cases for ComputeStatistics: progress cancellation and complex type.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto cancel_cb = [](double, const char*, void*) -> int { return FALSE; };
    EXPECT_FALSE(ar_f64->ComputeStatistics(false, &min, &max, &mean, &stddev,
                                           &valid_count, cancel_cb, nullptr,
                                           nullptr));
    auto ar_cf64 = root->CreateMDArray(
        "ar_cf64", {dim_x}, GDALExtendedDataType::Create(GDT_CFloat64));
    EXPECT_FALSE(ar_cf64->ComputeStatistics(false, &min, &max, &mean, &stddev,
                                            &valid_count, nullptr, nullptr,
                                            nullptr));
    CPLErrorReset();
  }

  // 2) GDALPamMultiDim and CustomPamMDArray with .aux.xml in /vsimem/.
  constexpr std::string_view kPamDir =
      "/vsimem/gdalmultidim_test/statistics_and_pam";
  constexpr std::string_view kPamFile =
      "/vsimem/gdalmultidim_test/statistics_and_pam/test_multidim_pam.dat";
  constexpr std::string_view kAuxFile =
      "/vsimem/gdalmultidim_test/statistics_and_pam/"
      "test_multidim_pam.dat.aux.xml";
  // Pre-seed aux.xml with an existing node so Save() merges into it.
  constexpr std::string_view kInitialXml =
      "<PAMDataset><OtherNode>hello</OtherNode>"
      "<Array name=\"/preexisting\"><SRS dataAxisToSRSAxisMapping=\"2,1\" "
      "coordinateEpoch=\"2025.5\">EPSG:4326</SRS>"
      "<Statistics ApproxStats=\"1\" Minimum=\"1\" Maximum=\"10\" Mean=\"5\" "
      "StdDev=\"2\" ValidSampleCount=\"100\"/></Array></PAMDataset>";

  absl::Cleanup cleanup_pam_dir = [kPamDir] {
    VSIRmdirRecursive(std::string(kPamDir).c_str());
  };
  VsiMemTempWrapper aux_wrapper{std::string(kAuxFile),
                                std::string(kInitialXml)};

  auto pam = std::make_shared<GDALPamMultiDim>(std::string(kPamFile));
  auto pre_srs = pam->GetSpatialRef("/preexisting", "");
  ASSERT_NE(nullptr, pre_srs);
  EXPECT_DOUBLE_EQ(2025.5, pre_srs->GetCoordinateEpoch());
  double init_min = 0.0;
  double init_max = 0.0;
  double init_mean = 0.0;
  double init_std = 0.0;
  GUInt64 init_vc = 0;
  EXPECT_EQ(CE_None,
            pam->GetStatistics("/preexisting", "", true, &init_min, &init_max,
                               &init_mean, &init_std, &init_vc));
  EXPECT_DOUBLE_EQ(1.0, init_min);
  EXPECT_DOUBLE_EQ(10.0, init_max);

  auto pam_ar = CustomPamMDArray::Create("/", "pam_arr", {dim_x}, dt_f64, pam);
  OGRSpatialReference srs;
  srs.importFromEPSG(32631);
  srs.SetDataAxisToSRSAxisMapping({1, 2});
  srs.SetCoordinateEpoch(2020.0);
  EXPECT_TRUE(pam_ar->SetSpatialRef(&srs));
  ASSERT_NE(nullptr, pam_ar->GetSpatialRef());
  EXPECT_DOUBLE_EQ(2020.0, pam_ar->GetSpatialRef()->GetCoordinateEpoch());

  EXPECT_EQ(CE_None,
            pam_ar->GetStatistics(false, true, &min, &max, &mean, &stddev,
                                  &valid_count, nullptr, nullptr));
  // Now statistics are saved in PAM; retrieve them from PAM without forcing.
  double p_min = 0.0;
  double p_max = 0.0;
  double p_mean = 0.0;
  double p_std = 0.0;
  GUInt64 p_vc = 0;
  EXPECT_EQ(CE_None, pam_ar->GetStatistics(true, false, &p_min, &p_max, &p_mean,
                                           &p_std, &p_vc, nullptr, nullptr));
  EXPECT_DOUBLE_EQ(min, p_min);
  EXPECT_DOUBLE_EQ(max, p_max);

  // Save statistics into PAM XML by resetting pam references.
  pam->SetStatistics("/preexisting", "", true, 1.0, 10.0, 5.0, 2.0, 100);
  pam_ar.reset();
  pam.reset();

  // Re-open PAM to verify saved XML and test clearing statistics.
  pam = std::make_shared<GDALPamMultiDim>(std::string(kPamFile));
  pam_ar = CustomPamMDArray::Create("/", "pam_arr", {dim_x}, dt_f64, pam);
  pam_ar->ClearStatistics();
  EXPECT_EQ(CE_Warning,
            pam_ar->GetStatistics(true, false, &p_min, &p_max, &p_mean, &p_std,
                                  &p_vc, nullptr, nullptr));
  EXPECT_TRUE(pam_ar->SetSpatialRef(nullptr));
  EXPECT_EQ(nullptr, pam_ar->GetSpatialRef());
  pam->ClearStatistics();
  pam_ar.reset();
  pam.reset();
}

class FakeNetCDFMultiDimDataset final : public GDALDataset {
 public:
  explicit FakeNetCDFMultiDimDataset(std::shared_ptr<GDALGroup> root)
      : root_(std::move(root)) {}
  std::shared_ptr<GDALGroup> GetRootGroup() const override { return root_; }

 private:
  std::shared_ptr<GDALGroup> root_;
};

std::map<std::string, std::shared_ptr<GDALGroup>>* GetFakeNetCDFGroupMap() {
  static auto* m = new std::map<std::string, std::shared_ptr<GDALGroup>>();
  return m;
}

TEST(GdalmultidimTest, ArrayCacheAndGetCacheRootGroup) {
  GDALRegister_MEM();
  ASSERT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("MEM"));
  auto dim0 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "y", "", "", 2);
  auto dim1 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "x", "", "", 3);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  // 1) Empty filename fails to cache.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ar_empty = CustomTestMDArray::Create("/", "empty_fn", {dim0, dim1},
                                              dt_f64, /*filename=*/"");
    EXPECT_FALSE(ar_empty->Cache(nullptr));
    CPLErrorReset();
  }

  // 2) Register temporary fake "netCDF" driver for caching tests.
  constexpr std::string_view kCacheDir =
      "/vsimem/gdalmultidim_test/array_cache";
  constexpr std::string_view kCacheBase =
      "/vsimem/gdalmultidim_test/array_cache/cache_test_ar.dat";
  constexpr std::string_view kPamProxyDir =
      "/vsimem/gdalmultidim_test/array_cache/pam_proxy";

  static bool s_nc_creation_in_progress = false;
  s_nc_creation_in_progress = false;
  GDALDriver* orig_nc = GetGDALDriverManager()->GetDriverByName("netCDF");
  if (orig_nc != nullptr) {
    GetGDALDriverManager()->DeregisterDriver(orig_nc);
  }
  auto* fake_nc = new GDALDriver();
  absl::Cleanup restore_drivers_and_vsimem = [&] {
    s_nc_creation_in_progress = false;
    PamCleanProxyDB();
    VSIRmdirRecursive(std::string(kCacheDir).c_str());
    GetFakeNetCDFGroupMap()->clear();
    if (fake_nc != nullptr) {
      GetGDALDriverManager()->DeregisterDriver(fake_nc);
      delete fake_nc;
    }
    if (orig_nc != nullptr) {
      GetGDALDriverManager()->RegisterDriver(orig_nc);
    }
  };
  fake_nc->SetDescription("netCDF");
  fake_nc->SetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER, "YES");
  fake_nc->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
  fake_nc->pfnCreateMultiDimensional = [](const char* pszFilename, CSLConstList,
                                          CSLConstList) -> GDALDataset* {
    if (pszFilename == nullptr ||
        std::string_view(pszFilename).starts_with("/vsicurl/")) {
      return nullptr;
    }
    s_nc_creation_in_progress = true;
    VSILFILE* fp = VSIFOpenL(pszFilename, "wb");
    if (fp == nullptr) return nullptr;
    VSIFWriteL("NC", 1, 2, fp);
    VSIFCloseL(fp);
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    if (mem_drv == nullptr) return nullptr;
    GDALDatasetUniquePtr mem_ds(
        mem_drv->CreateMultiDimensional(pszFilename, nullptr, nullptr));
    if (mem_ds == nullptr) return nullptr;
    auto root = mem_ds->GetRootGroup();
    if (root == nullptr) return nullptr;
    (*GetFakeNetCDFGroupMap())[pszFilename] = root;
    return new FakeNetCDFMultiDimDataset(root);
  };
  fake_nc->pfnOpen = [](GDALOpenInfo* poOpenInfo) -> GDALDataset* {
    if (s_nc_creation_in_progress || poOpenInfo == nullptr ||
        poOpenInfo->pszFilename == nullptr) {
      return nullptr;
    }
    auto* m = GetFakeNetCDFGroupMap();
    auto it = m->find(poOpenInfo->pszFilename);
    if (it == m->end()) return nullptr;
    return new FakeNetCDFMultiDimDataset(it->second);
  };
  GetGDALDriverManager()->RegisterDriver(fake_nc);

  // Test caching a normal /vsimem/ file with BLOCKSIZE option, re-caching
  // (which fails because array already exists in cache), and reading from
  // cache.
  auto ar_cached = CustomTestMDArray::Create(
      "/", "my_var", {dim0, dim1}, dt_f64, std::string(kCacheBase), "ctx1");
  auto* raw_ptr = reinterpret_cast<double*>(ar_cached->RawData().data());
  for (int i = 0; i < 6; ++i) raw_ptr[i] = (i + 1) * 10.0;

  CPLStringList cache_opts;
  cache_opts.SetNameValue("BLOCKSIZE", "1,2");
  EXPECT_TRUE(ar_cached->Cache(cache_opts.List()));
  s_nc_creation_in_progress = false;

  // Re-caching fails because array already exists in cache.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(ar_cached->Cache(nullptr));
    CPLErrorReset();
  }

  // Fresh array reading through the cache.
  auto ar_cached_read = CustomTestMDArray::Create(
      "/", "my_var", {dim0, dim1}, dt_f64, std::string(kCacheBase), "ctx1");
  const std::array<GUInt64, 2> start2 = {0, 0};
  const std::array<size_t, 2> count2 = {2, 3};
  std::array<double, 6> read_out = {0};
  EXPECT_TRUE(ar_cached_read->Read(start2.data(), count2.data(), nullptr,
                                   nullptr, dt_f64, read_out.data()));
  EXPECT_THAT(read_out, ElementsAre(10.0, 20.0, 30.0, 40.0, 50.0, 60.0));

  // Test /vsicurl/http...?query with GDAL_PAM_PROXY_DIR fallback.
  // Pins the curl-disabled fall-through in google3 (HAVE_CURL is compiled
  // out, so VSIStatL never touches the network); the fake netCDF driver's
  // starts_with("/vsicurl/") refusal forces the GDAL_PAM_PROXY_DIR branch.
  {
    PamCleanProxyDB();
    VSIMkdir(std::string(kPamProxyDir).c_str(), 0755);
    const std::string pam_proxy_dir_str(kPamProxyDir);
    CPLConfigOptionSetter pam_proxy_setter("GDAL_PAM_PROXY_DIR",
                                           pam_proxy_dir_str.c_str(),
                                           /*bSetOnlyIfUndefined=*/false);
    absl::Cleanup clean_pam_proxy = [] { PamCleanProxyDB(); };
    auto ar_curl = CustomTestMDArray::Create(
        "/", "curl_var", {dim0, dim1}, dt_f64,
        "/vsicurl/http://example.com/data.nc?token=123");
    EXPECT_TRUE(ar_curl->Cache(nullptr));
    s_nc_creation_in_progress = false;
  }
}

TEST(GdalmultidimTest, FullCApiCoverage) {
  GDALRegister_MEM();
  GDALRegister_VRT();
  GDALDriverH h_drv = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, h_drv);
  ASSERT_NE(nullptr, GDALGetDriverByName("VRT"));

  DatasetHPtr h_ds(
      GDALCreateMultiDimensional(h_drv, "c_api_ds", nullptr, nullptr),
      GDALClose);
  ASSERT_NE(nullptr, h_ds);
  GroupHPtr h_root(GDALDatasetGetRootGroup(h_ds.get()), GDALGroupRelease);
  ASSERT_NE(nullptr, h_root);

  EXPECT_STREQ("/", GDALGroupGetName(h_root.get()));
  EXPECT_STREQ("/", GDALGroupGetFullName(h_root.get()));
  EXPECT_EQ(nullptr, GDALGroupGetStructuralInfo(h_root.get()));

  // Dimensions C API.
  DimensionHPtr h_dim_y(
      GDALGroupCreateDimension(h_root.get(), "y", "HORIZONTAL_Y", "NORTH", 3,
                               nullptr),
      GDALDimensionRelease);
  DimensionHPtr h_dim_x(
      GDALGroupCreateDimension(h_root.get(), "x", "HORIZONTAL_X", "EAST", 4,
                               nullptr),
      GDALDimensionRelease);
  ASSERT_NE(nullptr, h_dim_y);
  ASSERT_NE(nullptr, h_dim_x);
  EXPECT_STREQ("x", GDALDimensionGetName(h_dim_x.get()));
  EXPECT_STREQ("/x", GDALDimensionGetFullName(h_dim_x.get()));
  EXPECT_STREQ("HORIZONTAL_X", GDALDimensionGetType(h_dim_x.get()));
  EXPECT_STREQ("EAST", GDALDimensionGetDirection(h_dim_x.get()));
  EXPECT_EQ(4u, GDALDimensionGetSize(h_dim_x.get()));

  size_t n_dims = 0;
  GDALDimensionH* pah_dims =
      GDALGroupGetDimensions(h_root.get(), &n_dims, nullptr);
  ASSERT_NE(nullptr, pah_dims);
  absl::Cleanup free_dims = [&] { GDALReleaseDimensions(pah_dims, n_dims); };
  EXPECT_EQ(2u, n_dims);
  std::move(free_dims).Invoke();
  GDALReleaseDimensions(nullptr, 0);

  // Subgroups C API.
  GroupHPtr h_sub(GDALGroupCreateGroup(h_root.get(), "sub", nullptr),
                  GDALGroupRelease);
  ASSERT_NE(nullptr, h_sub);
  StringListPtr grp_names(GDALGroupGetGroupNames(h_root.get(), nullptr),
                          CSLDestroy);
  EXPECT_THAT(CslToVector(grp_names.get()), ElementsAre("sub"));
  grp_names.reset();
  GroupHPtr h_sub_open(GDALGroupOpenGroup(h_root.get(), "sub", nullptr),
                       GDALGroupRelease);
  ASSERT_NE(nullptr, h_sub_open);
  h_sub_open.reset();
  GroupHPtr h_sub_full(
      GDALGroupOpenGroupFromFullname(h_root.get(), "/sub", nullptr),
      GDALGroupRelease);
  ASSERT_NE(nullptr, h_sub_full);
  h_sub_full.reset();
  EXPECT_TRUE(GDALGroupRename(h_sub.get(), "sub_ren"));
  h_sub.reset();
  EXPECT_TRUE(GDALGroupDeleteGroup(h_root.get(), "sub_ren", nullptr));

  // Attributes on Group C API.
  ExtendedDataTypeHPtr h_dt_f64(GDALExtendedDataTypeCreate(GDT_Float64),
                                GDALExtendedDataTypeRelease);
  ExtendedDataTypeHPtr h_dt_str(GDALExtendedDataTypeCreateString(0),
                                GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_dt_f64);
  ASSERT_NE(nullptr, h_dt_str);
  AttributeHPtr h_gattr(
      GDALGroupCreateAttribute(h_root.get(), "g_attr", 0, nullptr,
                               h_dt_str.get(), nullptr),
      GDALAttributeRelease);
  ASSERT_NE(nullptr, h_gattr);
  EXPECT_STREQ("g_attr", GDALAttributeGetName(h_gattr.get()));
  EXPECT_STREQ("/_GLOBAL_/g_attr", GDALAttributeGetFullName(h_gattr.get()));
  EXPECT_EQ(1u, GDALAttributeGetTotalElementsCount(h_gattr.get()));
  EXPECT_EQ(0u, GDALAttributeGetDimensionCount(h_gattr.get()));
  size_t attr_dim_cnt = 99;
  EXPECT_EQ(nullptr,
            GDALAttributeGetDimensionsSize(h_gattr.get(), &attr_dim_cnt));
  EXPECT_EQ(0u, attr_dim_cnt);
  ExtendedDataTypeHPtr h_attr_dt(GDALAttributeGetDataType(h_gattr.get()),
                                 GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_attr_dt);
  h_attr_dt.reset();

  EXPECT_TRUE(GDALAttributeWriteString(h_gattr.get(), "hello_c"));
  EXPECT_STREQ("hello_c", GDALAttributeReadAsString(h_gattr.get()));
  size_t raw_sz = 0;
  GByte* raw_bytes = GDALAttributeReadAsRaw(h_gattr.get(), &raw_sz);
  ASSERT_NE(nullptr, raw_bytes);
  absl::Cleanup free_raw = [&] {
    GDALAttributeFreeRawResult(h_gattr.get(), raw_bytes, raw_sz);
  };
  std::move(free_raw).Invoke();
  GDALAttributeFreeRawResult(h_gattr.get(), nullptr, 0);
  EXPECT_TRUE(GDALAttributeRename(h_gattr.get(), "g_attr_ren"));
  h_gattr.reset();

  h_gattr.reset(GDALGroupGetAttribute(h_root.get(), "g_attr_ren"));
  ASSERT_NE(nullptr, h_gattr);
  h_gattr.reset();
  size_t n_attrs = 0;
  GDALAttributeH* pah_attrs =
      GDALGroupGetAttributes(h_root.get(), &n_attrs, nullptr);
  ASSERT_NE(nullptr, pah_attrs);
  absl::Cleanup free_gattrs = [&] {
    GDALReleaseAttributes(pah_attrs, n_attrs);
  };
  EXPECT_EQ(1u, n_attrs);
  std::move(free_gattrs).Invoke();
  GDALReleaseAttributes(nullptr, 0);
  EXPECT_TRUE(GDALGroupDeleteAttribute(h_root.get(), "g_attr_ren", nullptr));

  // Numeric & 1D array attributes C API.
  const std::array<GUInt64, 1> dim_len2 = {2};
  AttributeHPtr h_nattr(
      GDALGroupCreateAttribute(h_root.get(), "n_attr", 1, dim_len2.data(),
                               h_dt_f64.get(), nullptr),
      GDALAttributeRelease);
  ASSERT_NE(nullptr, h_nattr);
  std::unique_ptr<GUInt64, CplFreeDeleter> pan_attr_dims(
      GDALAttributeGetDimensionsSize(h_nattr.get(), &attr_dim_cnt));
  ASSERT_NE(nullptr, pan_attr_dims);
  EXPECT_EQ(1u, attr_dim_cnt);
  pan_attr_dims.reset();

  const std::array<int, 2> i_arr = {11, 22};
  EXPECT_TRUE(GDALAttributeWriteIntArray(h_nattr.get(), i_arr.data(), 2));
  size_t out_cnt = 0;
  std::unique_ptr<int, CplFreeDeleter> out_i_arr(
      GDALAttributeReadAsIntArray(h_nattr.get(), &out_cnt));
  ASSERT_NE(nullptr, out_i_arr);
  EXPECT_EQ(2u, out_cnt);
  out_i_arr.reset();

  const std::array<int64_t, 2> i64_arr = {111, 222};
  EXPECT_TRUE(GDALAttributeWriteInt64Array(h_nattr.get(), i64_arr.data(), 2));
  std::unique_ptr<int64_t, CplFreeDeleter> out_i64_arr(
      GDALAttributeReadAsInt64Array(h_nattr.get(), &out_cnt));
  ASSERT_NE(nullptr, out_i64_arr);
  EXPECT_EQ(2u, out_cnt);
  out_i64_arr.reset();

  const std::array<double, 2> d_arr = {1.5, 2.5};
  EXPECT_TRUE(GDALAttributeWriteDoubleArray(h_nattr.get(), d_arr.data(), 2));
  std::unique_ptr<double, CplFreeDeleter> out_d_arr(
      GDALAttributeReadAsDoubleArray(h_nattr.get(), &out_cnt));
  ASSERT_NE(nullptr, out_d_arr);
  EXPECT_EQ(2u, out_cnt);
  out_d_arr.reset();

  EXPECT_TRUE(GDALAttributeWriteRaw(h_nattr.get(), d_arr.data(),
                                    d_arr.size() * sizeof(double)));
  EXPECT_EQ(2, GDALAttributeReadAsInt(h_nattr.get()));
  EXPECT_EQ(2, GDALAttributeReadAsInt64(h_nattr.get()));
  EXPECT_DOUBLE_EQ(1.5, GDALAttributeReadAsDouble(h_nattr.get()));
  h_nattr.reset();

  AttributeHPtr h_sattr_1d(
      GDALGroupCreateAttribute(h_root.get(), "s_attr1d", 1, dim_len2.data(),
                               h_dt_str.get(), nullptr),
      GDALAttributeRelease);
  ASSERT_NE(nullptr, h_sattr_1d);
  const std::array<const char*, 3> psz_vals = {"s1", "s2", nullptr};
  EXPECT_TRUE(GDALAttributeWriteStringArray(h_sattr_1d.get(), psz_vals.data()));
  StringListPtr out_s_arr(GDALAttributeReadAsStringArray(h_sattr_1d.get()),
                          CSLDestroy);
  EXPECT_THAT(CslToVector(out_s_arr.get()), ElementsAre("s1", "s2"));
  out_s_arr.reset();
  h_sattr_1d.reset();

  AttributeHPtr h_scalar_num(
      GDALGroupCreateAttribute(h_root.get(), "sc_num", 0, nullptr,
                               h_dt_f64.get(), nullptr),
      GDALAttributeRelease);
  ASSERT_NE(nullptr, h_scalar_num);
  EXPECT_TRUE(GDALAttributeWriteInt(h_scalar_num.get(), 5));
  EXPECT_TRUE(GDALAttributeWriteInt64(h_scalar_num.get(), 6));
  EXPECT_TRUE(GDALAttributeWriteDouble(h_scalar_num.get(), 7.5));
  h_scalar_num.reset();

  // MDArray C API.
  std::array<GDALDimensionH, 1> dims_1d = {h_dim_x.get()};
  MDArrayHPtr h_ar_x(
      GDALGroupCreateMDArray(h_root.get(), "x", 1, dims_1d.data(),
                             h_dt_f64.get(), nullptr),
      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_ar_x);
  const std::array<double, 4> x_coords = {10.0, 20.0, 30.0, 40.0};
  const std::array<GUInt64, 1> start1 = {0};
  const std::array<size_t, 1> count1 = {4};
  EXPECT_TRUE(GDALMDArrayWrite(h_ar_x.get(), start1.data(), count1.data(),
                               nullptr, nullptr, h_dt_f64.get(),
                               x_coords.data(), nullptr, 0));
  EXPECT_TRUE(GDALDimensionSetIndexingVariable(h_dim_x.get(), h_ar_x.get()));
  MDArrayHPtr h_idx_var(GDALDimensionGetIndexingVariable(h_dim_x.get()),
                        GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_idx_var);
  h_idx_var.reset();

  std::array<GDALDimensionH, 1> dims_1d_y = {h_dim_y.get()};
  MDArrayHPtr h_ar_y(
      GDALGroupCreateMDArray(h_root.get(), "y", 1, dims_1d_y.data(),
                             h_dt_f64.get(), nullptr),
      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_ar_y);
  const std::array<double, 3> y_coords = {1.0, 2.0, 3.0};
  const std::array<size_t, 1> count1_y = {3};
  EXPECT_TRUE(GDALMDArrayWrite(h_ar_y.get(), start1.data(), count1_y.data(),
                               nullptr, nullptr, h_dt_f64.get(),
                               y_coords.data(), nullptr, 0));
  EXPECT_TRUE(GDALDimensionSetIndexingVariable(h_dim_y.get(), h_ar_y.get()));

  std::array<GDALDimensionH, 2> dims_2d = {h_dim_y.get(), h_dim_x.get()};
  MDArrayHPtr h_ar(
      GDALGroupCreateMDArray(h_root.get(), "data", 2, dims_2d.data(),
                             h_dt_f64.get(), nullptr),
      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_ar);
  EXPECT_STREQ("data", GDALMDArrayGetName(h_ar.get()));
  EXPECT_STREQ("/data", GDALMDArrayGetFullName(h_ar.get()));
  EXPECT_EQ(12u, GDALMDArrayGetTotalElementsCount(h_ar.get()));
  EXPECT_EQ(2u, GDALMDArrayGetDimensionCount(h_ar.get()));
  size_t ar_dim_cnt = 0;
  GDALDimensionH* pah_ar_dims =
      GDALMDArrayGetDimensions(h_ar.get(), &ar_dim_cnt);
  ASSERT_NE(nullptr, pah_ar_dims);
  absl::Cleanup free_ar_dims = [&] {
    GDALReleaseDimensions(pah_ar_dims, ar_dim_cnt);
  };
  EXPECT_EQ(2u, ar_dim_cnt);
  std::move(free_ar_dims).Invoke();
  ExtendedDataTypeHPtr h_ar_dt(GDALMDArrayGetDataType(h_ar.get()),
                               GDALExtendedDataTypeRelease);
  ASSERT_NE(nullptr, h_ar_dt);
  h_ar_dt.reset();
  EXPECT_EQ(nullptr, GDALMDArrayGetStructuralInfo(h_ar.get()));

  const std::array<GUInt64, 2> start2 = {0, 0};
  const std::array<size_t, 2> count2 = {3, 4};
  const std::array<double, 12> vals_2d = {1, 2, 3, 4,  5,  6,
                                          7, 8, 9, 10, 11, 12};
  EXPECT_TRUE(GDALMDArrayWrite(h_ar.get(), start2.data(), count2.data(),
                               nullptr, nullptr, h_dt_f64.get(), vals_2d.data(),
                               nullptr, 0));
  std::array<double, 12> read_2d = {0};
  EXPECT_TRUE(GDALMDArrayRead(h_ar.get(), start2.data(), count2.data(), nullptr,
                              nullptr, h_dt_f64.get(), read_2d.data(), nullptr,
                              0));
  EXPECT_TRUE(GDALMDArrayAdviseRead(h_ar.get(), start2.data(), count2.data()));
  EXPECT_TRUE(GDALMDArrayAdviseReadEx(h_ar.get(), start2.data(), count2.data(),
                                      nullptr));

  // Attributes on MDArray C API.
  AttributeHPtr h_aattr(
      GDALMDArrayCreateAttribute(h_ar.get(), "a_attr", 0, nullptr,
                                 h_dt_f64.get(), nullptr),
      GDALAttributeRelease);
  ASSERT_NE(nullptr, h_aattr);
  h_aattr.reset();
  h_aattr.reset(GDALMDArrayGetAttribute(h_ar.get(), "a_attr"));
  ASSERT_NE(nullptr, h_aattr);
  h_aattr.reset();
  pah_attrs = GDALMDArrayGetAttributes(h_ar.get(), &n_attrs, nullptr);
  ASSERT_NE(nullptr, pah_attrs);
  absl::Cleanup free_aattrs = [&] {
    GDALReleaseAttributes(pah_attrs, n_attrs);
  };
  EXPECT_EQ(1u, n_attrs);
  std::move(free_aattrs).Invoke();
  EXPECT_TRUE(GDALMDArrayDeleteAttribute(h_ar.get(), "a_attr", nullptr));

  // NoData, Scale, Offset, Unit, SpatialRef, BlockSize, ProcessingChunkSize.
  int has_val = 0;
  EXPECT_TRUE(GDALMDArraySetNoDataValueAsDouble(h_ar.get(), -999.0));
  EXPECT_DOUBLE_EQ(-999.0,
                   GDALMDArrayGetNoDataValueAsDouble(h_ar.get(), &has_val));
  EXPECT_NE(0, has_val);
  EXPECT_NE(nullptr, GDALMDArrayGetRawNoDataValue(h_ar.get()));
  EXPECT_TRUE(GDALMDArraySetRawNoDataValue(h_ar.get(), nullptr));
  EXPECT_TRUE(GDALMDArraySetNoDataValueAsInt64(h_ar.get(), -10));
  EXPECT_EQ(-10, GDALMDArrayGetNoDataValueAsInt64(h_ar.get(), &has_val));
  EXPECT_TRUE(GDALMDArraySetNoDataValueAsUInt64(h_ar.get(), 10));
  EXPECT_EQ(10u, GDALMDArrayGetNoDataValueAsUInt64(h_ar.get(), &has_val));

  GDALDataType st_type = GDT_Unknown;
  EXPECT_TRUE(GDALMDArraySetScale(h_ar.get(), 2.0));
  EXPECT_DOUBLE_EQ(2.0, GDALMDArrayGetScale(h_ar.get(), &has_val));
  EXPECT_TRUE(GDALMDArraySetScaleEx(h_ar.get(), 3.0, GDT_Float32));
  EXPECT_DOUBLE_EQ(3.0, GDALMDArrayGetScaleEx(h_ar.get(), &has_val, &st_type));
  EXPECT_EQ(GDT_Float32, st_type);

  EXPECT_TRUE(GDALMDArraySetOffset(h_ar.get(), 1.0));
  EXPECT_DOUBLE_EQ(1.0, GDALMDArrayGetOffset(h_ar.get(), &has_val));
  EXPECT_TRUE(GDALMDArraySetOffsetEx(h_ar.get(), 4.0, GDT_Float32));
  EXPECT_DOUBLE_EQ(4.0, GDALMDArrayGetOffsetEx(h_ar.get(), &has_val, &st_type));

  EXPECT_TRUE(GDALMDArraySetUnit(h_ar.get(), "m"));
  EXPECT_STREQ("m", GDALMDArrayGetUnit(h_ar.get()));

  SpatialRefHPtr h_srs(OSRNewSpatialReference(nullptr),
                       OSRDestroySpatialReference);
  ASSERT_NE(nullptr, h_srs);
  OSRImportFromEPSG(h_srs.get(), 4326);
  EXPECT_TRUE(GDALMDArraySetSpatialRef(h_ar.get(), h_srs.get()));
  SpatialRefHPtr h_srs_out(GDALMDArrayGetSpatialRef(h_ar.get()),
                           OSRDestroySpatialReference);
  ASSERT_NE(nullptr, h_srs_out);
  h_srs_out.reset();
  EXPECT_TRUE(GDALMDArraySetSpatialRef(h_ar.get(), nullptr));
  h_srs.reset();

  size_t bs_cnt = 0;
  std::unique_ptr<GUInt64, CplFreeDeleter> pan_bs(
      GDALMDArrayGetBlockSize(h_ar.get(), &bs_cnt));
  ASSERT_NE(nullptr, pan_bs);
  EXPECT_EQ(2u, bs_cnt);
  pan_bs.reset();

  size_t pcs_cnt = 0;
  std::unique_ptr<size_t, CplFreeDeleter> pan_pcs(
      GDALMDArrayGetProcessingChunkSize(h_ar.get(), &pcs_cnt, 1024));
  ASSERT_NE(nullptr, pan_pcs);
  EXPECT_EQ(2u, pcs_cnt);
  pan_pcs.reset();

  // Views, Transpose, Unscaled, Mask, Resampled, Gridded, MeshGrid,
  // CoordinateVariables, AsClassicDataset C API.
  MDArrayHPtr h_view(GDALMDArrayGetView(h_ar.get(), "[0,:]"),
                     GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_view);
  h_view.reset();

  const std::array<int, 2> trans_map = {1, 0};
  MDArrayHPtr h_trans(GDALMDArrayTranspose(h_ar.get(), 2, trans_map.data()),
                      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_trans);
  h_trans.reset();

  MDArrayHPtr h_unscaled(GDALMDArrayGetUnscaled(h_ar.get()),
                         GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_unscaled);
  h_unscaled.reset();

  MDArrayHPtr h_mask(GDALMDArrayGetMask(h_ar.get(), nullptr),
                     GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_mask);
  h_mask.reset();

  const std::array<GDALDimensionH, 2> res_dims = {nullptr, nullptr};
  MDArrayHPtr h_res(
      GDALMDArrayGetResampled(h_ar.get(), 2, res_dims.data(),
                              GRIORA_NearestNeighbour, nullptr, nullptr),
      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_res);
  h_res.reset();

  // GDALMDArrayGetGridded on 1D scattered data.
  MDArrayHPtr h_gridded(
      GDALMDArrayGetGridded(h_ar_y.get(), "invdist", h_ar_y.get(), h_ar_y.get(),
                            nullptr),
      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_gridded);
  h_gridded.reset();

  // GDALMDArrayGetMeshGrid.
  const std::array<GDALMDArrayH, 2> mg_in = {h_ar_x.get(), h_ar_y.get()};
  size_t mg_out_cnt = 0;
  GDALMDArrayH* pah_mg =
      GDALMDArrayGetMeshGrid(mg_in.data(), 2, &mg_out_cnt, nullptr);
  ASSERT_NE(nullptr, pah_mg);
  absl::Cleanup free_mg = [&] { GDALReleaseArrays(pah_mg, mg_out_cnt); };
  EXPECT_EQ(2u, mg_out_cnt);
  std::move(free_mg).Invoke();
  GDALReleaseArrays(nullptr, 0);

  // GDALMDArrayGetCoordinateVariables.
  size_t cv_cnt = 0;
  GDALMDArrayH* pah_cv = GDALMDArrayGetCoordinateVariables(h_ar.get(), &cv_cnt);
  GDALReleaseArrays(pah_cv, cv_cnt);

  // Statistics C API.
  double s_min = 0.0;
  double s_max = 0.0;
  double s_mean = 0.0;
  double s_std = 0.0;
  GUInt64 s_vc = 0;
  EXPECT_TRUE(GDALMDArrayComputeStatistics(h_ar.get(), h_ds.get(), FALSE,
                                           &s_min, &s_max, &s_mean, &s_std,
                                           &s_vc, nullptr, nullptr));
  EXPECT_TRUE(GDALMDArrayComputeStatisticsEx(h_ar.get(), h_ds.get(), FALSE,
                                             &s_min, &s_max, &s_mean, &s_std,
                                             &s_vc, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_None, GDALMDArrayGetStatistics(h_ar.get(), h_ds.get(), FALSE,
                                              TRUE, &s_min, &s_max, &s_mean,
                                              &s_std, &s_vc, nullptr, nullptr));
  GDALDatasetClearStatistics(h_ds.get());

  // Classic dataset conversions C API.
  DatasetHPtr h_classic1(GDALMDArrayAsClassicDataset(h_ar.get(), 1, 0),
                         GDALClose);
  ASSERT_NE(nullptr, h_classic1);
  MDArrayHPtr h_from_ds(GDALDatasetAsMDArray(h_classic1.get(), nullptr),
                        GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_from_ds);
  h_from_ds.reset();
  MDArrayHPtr h_from_band(
      GDALRasterBandAsMDArray(GDALGetRasterBand(h_classic1.get(), 1)),
      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_from_band);
  h_from_band.reset();
  h_classic1.reset();

  DatasetHPtr h_classic2(
      GDALMDArrayAsClassicDatasetEx(h_ar.get(), 1, 0, h_root.get(), nullptr),
      GDALClose);
  ASSERT_NE(nullptr, h_classic2);
  h_classic2.reset();

  // Group array open / resolve / subset / resize / rename / delete C API.
  StringListPtr arr_names(GDALGroupGetMDArrayNames(h_root.get(), nullptr),
                          CSLDestroy);
  EXPECT_GE(CSLCount(arr_names.get()), 3);
  arr_names.reset();
  StringListPtr full_arr_names(
      GDALGroupGetMDArrayFullNamesRecursive(h_root.get(), nullptr, nullptr),
      CSLDestroy);
  EXPECT_GE(CSLCount(full_arr_names.get()), 3);
  full_arr_names.reset();

  MDArrayHPtr h_open_ar(GDALGroupOpenMDArray(h_root.get(), "data", nullptr),
                        GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_open_ar);
  h_open_ar.reset();
  MDArrayHPtr h_open_full(
      GDALGroupOpenMDArrayFromFullname(h_root.get(), "/data", nullptr),
      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_open_full);
  h_open_full.reset();
  MDArrayHPtr h_res_ar(
      GDALGroupResolveMDArray(h_root.get(), "data", "/", nullptr),
      GDALMDArrayRelease);
  ASSERT_NE(nullptr, h_res_ar);
  h_res_ar.reset();

  GroupHPtr h_subset_grp(
      GDALGroupSubsetDimensionFromSelection(h_root.get(), "/x=20", nullptr),
      GDALGroupRelease);
  ASSERT_NE(nullptr, h_subset_grp);
  h_subset_grp.reset();

  const std::array<GUInt64, 2> new_sizes = {4, 5};
  EXPECT_TRUE(GDALMDArrayResize(h_ar.get(), new_sizes.data(), nullptr));
  EXPECT_TRUE(GDALMDArrayRename(h_ar.get(), "data_ren"));
  EXPECT_TRUE(GDALDimensionRename(h_dim_y.get(), "y_ren"));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(GDALMDArrayCache(h_ar.get(), nullptr));
    CPLErrorReset();
  }

  h_ar.reset();
  h_ar_y.reset();
  h_ar_x.reset();
  GDALMDArrayRelease(nullptr);
  EXPECT_TRUE(GDALGroupDeleteMDArray(h_root.get(), "data_ren", nullptr));
  EXPECT_TRUE(GDALDimensionSetIndexingVariable(h_dim_x.get(), nullptr));
  EXPECT_TRUE(GDALDimensionSetIndexingVariable(h_dim_y.get(), nullptr));
  EXPECT_TRUE(GDALGroupDeleteMDArray(h_root.get(), "x", nullptr));
  EXPECT_TRUE(GDALGroupDeleteMDArray(h_root.get(), "y", nullptr));
  EXPECT_TRUE(GDALGroupDeleteAttribute(h_root.get(), "n_attr", nullptr));
  EXPECT_TRUE(GDALGroupDeleteAttribute(h_root.get(), "s_attr1d", nullptr));
  EXPECT_TRUE(GDALGroupDeleteAttribute(h_root.get(), "sc_num", nullptr));

  h_dim_x.reset();
  h_dim_y.reset();
  GDALDimensionRelease(nullptr);
  h_dt_str.reset();
  h_dt_f64.reset();
  h_root.reset();
  GDALGroupRelease(nullptr);
  h_ds.reset();
}

TEST(GdalmultidimTest, DuplicateDimNamesCopyFrom) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);
  auto dim_a1 = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "shared_dim", "", "", 3);
  auto dim_a2 = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "shared_dim", "", "", 5);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto ar1 = src_root->CreateMDArray("ar1", {dim_a1}, dt_f64);
  auto ar2 = src_root->CreateMDArray("ar2", {dim_a2}, dt_f64);
  ASSERT_NE(nullptr, ar1);
  ASSERT_NE(nullptr, ar2);

  GDALDatasetUniquePtr dst_ds(
      mem_drv->CreateMultiDimensional("dst", nullptr, nullptr));
  ASSERT_NE(nullptr, dst_ds);
  auto dst_root = dst_ds->GetRootGroup();
  ASSERT_NE(nullptr, dst_root);
  GUInt64 cost = 0;
  EXPECT_TRUE(dst_root->CopyFrom(dst_root, src_ds.get(), src_root, true, cost,
                                 src_root->GetTotalCopyCost(), nullptr,
                                 nullptr));
  EXPECT_NE(nullptr, dst_root->OpenMDArray("ar1"));
  EXPECT_NE(nullptr, dst_root->OpenMDArray("ar2"));
}

TEST(GdalmultidimTest, CopyToFinalBuffer3DTypeConversion) {
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const auto dt_f32 = GDALExtendedDataType::Create(GDT_Float32);
  auto d0 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d0", "", "", 2);
  auto d1 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d1", "", "", 2);
  auto d2 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d2", "", "", 2);
  auto ar3d = CustomTestMDArray::Create("/", "ar3d", {d0, d1, d2}, dt_f64);
  auto* p = reinterpret_cast<double*>(ar3d->RawData().data());
  for (int i = 0; i < 8; ++i) p[i] = i + 1;
  const std::array<GUInt64, 3> start = {0, 0, 0};
  const std::array<size_t, 3> count = {2, 2, 2};
  const std::array<GInt64, 3> step = {1, 1, 1};
  // Non-contiguous buffer stride to exercise CopyToFinalBuffer stack recursion:
  const std::array<GPtrDiff_t, 3> stride = {8, 4, 1};
  std::vector<float> buf(20, 0.0f);
  EXPECT_TRUE(ar3d->Read(start.data(), count.data(), step.data(), stride.data(),
                         dt_f32, buf.data()));
  EXPECT_FLOAT_EQ(1.0f, buf[0]);
  EXPECT_FLOAT_EQ(2.0f, buf[1]);
}

TEST(GdalmultidimTest, CopyFrom0DAndStringArray) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("mem", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const auto dt_str = GDALExtendedDataType::CreateString();
  auto s0 = root->CreateMDArray("s0", {}, dt_f64);
  ASSERT_NE(nullptr, s0);
  const double val = 42.0;
  ASSERT_TRUE(s0->Write(nullptr, nullptr, nullptr, nullptr, dt_f64, &val));
  auto d0 = root->CreateMDArray("d0", {}, dt_f64);
  ASSERT_NE(nullptr, d0);
  GUInt64 cost = 0;
  EXPECT_TRUE(d0->CopyFrom(nullptr, s0.get(), true, cost,
                           s0->GetTotalCopyCost(), nullptr, nullptr));
  double read_val = 0.0;
  ASSERT_TRUE(d0->Read(nullptr, nullptr, nullptr, nullptr, dt_f64, &read_val));
  EXPECT_DOUBLE_EQ(42.0, read_val);

  auto dim2 = root->CreateDimension("dim2", "", "", 2);
  auto str_src = root->CreateMDArray("str_src", {dim2}, dt_str);
  ASSERT_NE(nullptr, str_src);
  const std::array<const char*, 2> str_vals = {"hello", "world"};
  const std::array<GUInt64, 1> start = {0};
  const std::array<size_t, 1> count = {2};
  ASSERT_TRUE(str_src->Write(start.data(), count.data(), nullptr, nullptr,
                             dt_str, str_vals.data()));
  auto str_dst = root->CreateMDArray("str_dst", {dim2}, dt_str);
  ASSERT_NE(nullptr, str_dst);
  EXPECT_TRUE(str_dst->CopyFrom(nullptr, str_src.get(), true, cost,
                                str_src->GetTotalCopyCost(), nullptr, nullptr));
}

TEST(GdalmultidimTest, AsClassicDatasetSubsetAndUnit) {
  GDALRegister_MEM();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("mem", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto dim_sub = root->CreateDimension("subset_band_0_1_2", "", "", 2);
  auto dim_y = root->CreateDimension("y", "", "", 2);
  auto dim_x = root->CreateDimension("x", "", "", 2);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto iv = root->CreateMDArray("iv", {dim_sub}, dt_f64);
  ASSERT_NE(nullptr, iv);
  iv->SetUnit("m");
  const std::array<double, 2> iv_vals = {10.0, 11.0};
  const std::array<GUInt64, 1> s0 = {0};
  const std::array<size_t, 1> c2 = {2};
  ASSERT_TRUE(iv->Write(s0.data(), c2.data(), nullptr, nullptr, dt_f64,
                        iv_vals.data()));
  dim_sub->SetIndexingVariable(iv);

  auto ar = root->CreateMDArray("sub_img", {dim_sub, dim_y, dim_x}, dt_f64);
  ASSERT_NE(nullptr, ar);
  GDALDatasetUniquePtr cds(ar->AsClassicDataset(2, 1, root, nullptr));
  ASSERT_NE(nullptr, cds);
  auto* b1 = cds->GetRasterBand(1);
  ASSERT_NE(nullptr, b1);
  EXPECT_STREQ("0", b1->GetMetadataItem("DIM_band_INDEX"));
  EXPECT_STREQ("10", b1->GetMetadataItem("DIM_band_VALUE"));
  EXPECT_STREQ("m", b1->GetMetadataItem("DIM_band_UNIT"));
}

}  // namespace
}  // namespace autotest2
