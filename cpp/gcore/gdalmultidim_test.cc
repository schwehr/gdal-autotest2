// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/gdalmultidim.cpp.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/frmts/mem/memmultidim.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_pam_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/gcore/gdalmultidim_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

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
    // Contiguous read from data_ for 1D/2D/3D
    size_t total_elts = 1;
    for (size_t i = 0; i < nDims; ++i) total_elts *= count[i];
    const size_t dt_size = dt_.GetSize();
    std::vector<size_t> idx(nDims, 0);
    GByte* dst_ptr = static_cast<GByte*>(pDstBuffer);
    for (size_t iter = 0; iter < total_elts; ++iter) {
      size_t flat_idx = 0;
      size_t mul = 1;
      for (size_t d = nDims; d > 0; --d) {
        size_t pos = static_cast<size_t>(arrayStartIdx[d - 1]) + idx[d - 1];
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
        vals_(4, 0.0) {
    vals_[0] = 1.0;
    vals_[1] = 2.0;
    vals_[2] = 3.0;
    vals_[3] = 4.0;
  }

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
  const std::string& GetFilename() const override {
    static const std::string empty;
    return empty;
  }
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
      size_t idx = static_cast<size_t>(arrayStartIdx[0] + i * arrayStep[0]);
      double v = vals_[idx % vals_.size()];
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
  std::vector<double> vals_;
};

// ============================================================================
// 1. GDALExtendedDataType & GDALEDTComponent Tests (C++ and C API)
// ============================================================================

TEST(GdalmultidimTest, ExtendedDataTypeAndComponents) {
  // Numeric types.
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
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
  auto dt_rat =
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
  auto dt_str = GDALExtendedDataType::CreateString(0, GEDTST_JSON);
  EXPECT_EQ(GEDTC_STRING, dt_str.GetClass());
  EXPECT_EQ(GEDTST_JSON, dt_str.GetSubType());
  EXPECT_EQ(0u, dt_str.GetMaxStringLength());
  EXPECT_EQ(sizeof(char*), dt_str.GetSize());
  EXPECT_TRUE(dt_str.NeedsFreeDynamicMemory());

  auto dt_str10 = GDALExtendedDataType::CreateString(10, GEDTST_NONE);
  EXPECT_NE(dt_str, dt_str10);
  EXPECT_EQ(10u, dt_str10.GetMaxStringLength());

  // Compound types.
  auto comp1 = std::make_unique<GDALEDTComponent>("x", 0, dt_f64);
  auto comp2 = std::make_unique<GDALEDTComponent>(
      "s", sizeof(double), GDALExtendedDataType::CreateString());
  GDALEDTComponent comp1_copy(*comp1);
  EXPECT_EQ(*comp1, comp1_copy);
  EXPECT_EQ("x", comp1->GetName());
  EXPECT_EQ(0u, comp1->GetOffset());
  EXPECT_EQ(dt_f64, comp1->GetType());

  std::vector<std::unique_ptr<GDALEDTComponent>> comps;
  comps.push_back(std::move(comp1));
  comps.push_back(std::move(comp2));
  auto dt_comp = GDALExtendedDataType::Create(
      "my_comp", sizeof(double) + sizeof(char*), std::move(comps));
  EXPECT_EQ(GEDTC_COMPOUND, dt_comp.GetClass());
  EXPECT_EQ("my_comp", dt_comp.GetName());
  EXPECT_EQ(2u, dt_comp.GetComponents().size());
  EXPECT_TRUE(dt_comp.NeedsFreeDynamicMemory());

  GDALExtendedDataType dt_comp_copy(dt_comp);
  EXPECT_EQ(dt_comp, dt_comp_copy);
  GDALExtendedDataType dt_comp_assign = GDALExtendedDataType::Create(GDT_Byte);
  dt_comp_assign = dt_comp;
  EXPECT_EQ(dt_comp, dt_comp_assign);

  // Invalid compound creations: empty comps, overlapping offsets, too small
  // total size, huge offset/size.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  std::vector<std::unique_ptr<GDALEDTComponent>> empty_comps;
  auto bad_comp1 =
      GDALExtendedDataType::Create("bad1", 8, std::move(empty_comps));
  EXPECT_EQ(GDT_Unknown, bad_comp1.GetNumericDataType());

  std::vector<std::unique_ptr<GDALEDTComponent>> overlap_comps;
  overlap_comps.push_back(std::make_unique<GDALEDTComponent>("a", 4, dt_f64));
  overlap_comps.push_back(std::make_unique<GDALEDTComponent>("b", 2, dt_f64));
  auto bad_comp2 =
      GDALExtendedDataType::Create("bad2", 16, std::move(overlap_comps));
  EXPECT_EQ(GDT_Unknown, bad_comp2.GetNumericDataType());

  std::vector<std::unique_ptr<GDALEDTComponent>> small_comps;
  small_comps.push_back(std::make_unique<GDALEDTComponent>("a", 0, dt_f64));
  auto bad_comp3 =
      GDALExtendedDataType::Create("bad3", 4, std::move(small_comps));
  EXPECT_EQ(GDT_Unknown, bad_comp3.GetNumericDataType());

  std::vector<std::unique_ptr<GDALEDTComponent>> huge_comps;
  huge_comps.push_back(std::make_unique<GDALEDTComponent>(
      "a", static_cast<size_t>(std::numeric_limits<int>::max()), dt_f64));
  auto bad_comp4 = GDALExtendedDataType::Create(
      "bad4", static_cast<size_t>(std::numeric_limits<int>::max()) + 16,
      std::move(huge_comps));
  EXPECT_EQ(GDT_Unknown, bad_comp4.GetNumericDataType());
  CPLPopErrorHandler();

  // CanConvertTo tests.
  auto dt_unknown = GDALExtendedDataType::Create(GDT_Unknown);
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
  auto dt_sub_comp = GDALExtendedDataType::Create("sub_comp", sizeof(double),
                                                  std::move(sub_comps));
  EXPECT_TRUE(dt_comp.CanConvertTo(dt_sub_comp));
  EXPECT_FALSE(dt_sub_comp.CanConvertTo(dt_comp));

  // CopyValue & FreeDynamicMemory on compound + string + numeric.
  struct MyCompStruct {
    double x;
    char* s;
  };
  MyCompStruct src_val{42.5, CPLStrdup("hello_comp")};
  MyCompStruct dst_val{0.0, nullptr};
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

  dt_comp.FreeDynamicMemory(&src_val);
  dt_comp.FreeDynamicMemory(&dst_val);
  dt_f64.FreeDynamicMemory(&sub_dst);
}

TEST(GdalmultidimTest, ExtendedDataTypeCopyValuesAllTypes) {
  auto dt_str = GDALExtendedDataType::CreateString();
  const GDALDataType all_numeric_types[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_UInt32,  GDT_Int32,    GDT_UInt64,   GDT_Int64,
      GDT_Float16, GDT_Float32,  GDT_Float64,  GDT_CInt16,
      GDT_CInt32,  GDT_CFloat16, GDT_CFloat32, GDT_CFloat64};

  for (GDALDataType eType : all_numeric_types) {
    auto dt_num = GDALExtendedDataType::Create(eType);
    double src_d[2] = {12.0, 3.0};
    std::vector<GByte> num_buf(dt_num.GetSize(), 0);
    GDALCopyWords(src_d, GDT_Float64, sizeof(double), num_buf.data(), eType,
                  static_cast<int>(dt_num.GetSize()), 1);

    // Numeric -> String.
    char* str_out = nullptr;
    EXPECT_TRUE(GDALExtendedDataType::CopyValue(num_buf.data(), dt_num,
                                                &str_out, dt_str));
    ASSERT_NE(nullptr, str_out);
    dt_str.FreeDynamicMemory(&str_out);

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
  const char* src_strs[2] = {"first", nullptr};
  char* dst_strs[2] = {nullptr, nullptr};
  EXPECT_TRUE(GDALExtendedDataType::CopyValues(src_strs, dt_str, 1, dst_strs,
                                               dt_str, 1, 2));
  EXPECT_STREQ("first", dst_strs[0]);
  EXPECT_EQ(nullptr, dst_strs[1]);
  dt_str.FreeDynamicMemory(&dst_strs[0]);
  dt_str.FreeDynamicMemory(&dst_strs[1]);

  // Numeric -> Numeric CopyValues.
  auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  int32_t src_i[3] = {10, 20, 30};
  double dst_d[3] = {0, 0, 0};
  EXPECT_TRUE(
      GDALExtendedDataType::CopyValues(src_i, dt_i32, 1, dst_d, dt_f64, 1, 3));
  EXPECT_DOUBLE_EQ(10.0, dst_d[0]);
  EXPECT_DOUBLE_EQ(20.0, dst_d[1]);
  EXPECT_DOUBLE_EQ(30.0, dst_d[2]);

  // Incompatible CopyValue & CopyValues.
  auto dt_unk = GDALExtendedDataType::Create(GDT_Unknown);
  char* s_out = nullptr;
  EXPECT_TRUE(GDALExtendedDataType::CopyValue(src_i, dt_unk, &s_out, dt_str));
  EXPECT_EQ(nullptr, s_out);
  EXPECT_TRUE(
      GDALExtendedDataType::CopyValues(src_i, dt_unk, 1, &s_out, dt_str, 1, 1));
  CPLFree(s_out);
}

TEST(GdalmultidimTest, ExtendedDataTypeCApi) {
  GDALExtendedDataTypeH h_f64 = GDALExtendedDataTypeCreate(GDT_Float64);
  ASSERT_NE(nullptr, h_f64);
  EXPECT_EQ(GEDTC_NUMERIC, GDALExtendedDataTypeGetClass(h_f64));
  EXPECT_EQ(GDT_Float64, GDALExtendedDataTypeGetNumericDataType(h_f64));
  EXPECT_EQ(sizeof(double), GDALExtendedDataTypeGetSize(h_f64));
  EXPECT_EQ(GEDTST_NONE, GDALExtendedDataTypeGetSubType(h_f64));
  EXPECT_EQ(nullptr, GDALExtendedDataTypeGetRAT(h_f64));
  EXPECT_STREQ("", GDALExtendedDataTypeGetName(h_f64));

  GDALExtendedDataTypeH h_str = GDALExtendedDataTypeCreateString(15);
  ASSERT_NE(nullptr, h_str);
  EXPECT_EQ(GEDTC_STRING, GDALExtendedDataTypeGetClass(h_str));
  EXPECT_EQ(15u, GDALExtendedDataTypeGetMaxStringLength(h_str));

  GDALExtendedDataTypeH h_str_ex =
      GDALExtendedDataTypeCreateStringEx(0, GEDTST_JSON);
  ASSERT_NE(nullptr, h_str_ex);
  EXPECT_EQ(GEDTST_JSON, GDALExtendedDataTypeGetSubType(h_str_ex));

  GDALEDTComponentH h_comp = GDALEDTComponentCreate("field1", 0, h_f64);
  ASSERT_NE(nullptr, h_comp);
  EXPECT_STREQ("field1", GDALEDTComponentGetName(h_comp));
  EXPECT_EQ(0u, GDALEDTComponentGetOffset(h_comp));
  GDALExtendedDataTypeH h_comp_type = GDALEDTComponentGetType(h_comp);
  EXPECT_TRUE(GDALExtendedDataTypeEquals(h_f64, h_comp_type));
  GDALExtendedDataTypeRelease(h_comp_type);

  GDALExtendedDataTypeH h_compound =
      GDALExtendedDataTypeCreateCompound("comp_c", sizeof(double), 1, &h_comp);
  ASSERT_NE(nullptr, h_compound);
  EXPECT_EQ(GEDTC_COMPOUND, GDALExtendedDataTypeGetClass(h_compound));
  EXPECT_STREQ("comp_c", GDALExtendedDataTypeGetName(h_compound));

  size_t n_comps = 0;
  GDALEDTComponentH* comps =
      GDALExtendedDataTypeGetComponents(h_compound, &n_comps);
  ASSERT_NE(nullptr, comps);
  EXPECT_EQ(1u, n_comps);
  GDALExtendedDataTypeFreeComponents(comps, n_comps);
  GDALExtendedDataTypeFreeComponents(nullptr, 0);

  EXPECT_TRUE(GDALExtendedDataTypeCanConvertTo(h_f64, h_str));
  EXPECT_FALSE(GDALExtendedDataTypeEquals(h_f64, h_str));

  GDALEDTComponentRelease(h_comp);
  GDALEDTComponentRelease(nullptr);
  GDALExtendedDataTypeRelease(h_compound);
  GDALExtendedDataTypeRelease(h_str_ex);
  GDALExtendedDataTypeRelease(h_str);
  GDALExtendedDataTypeRelease(h_f64);
  GDALExtendedDataTypeRelease(nullptr);
}

// ============================================================================
// 2. GDALIHasAttribute, GDALGroup, GDALDimension Base & CopyFrom Tests
// ============================================================================

TEST(GdalmultidimTest, BaseGroupDimensionAndHasAttribute) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
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
  EXPECT_EQ(nullptr, grp->CreateGroup("g"));
  EXPECT_FALSE(grp->DeleteGroup("g"));
  EXPECT_EQ(nullptr, grp->CreateDimension("d", "", "", 1));
  EXPECT_EQ(nullptr, grp->CreateMDArray(
                         "a", {}, GDALExtendedDataType::Create(GDT_Byte)));
  EXPECT_FALSE(grp->DeleteMDArray("a"));
  EXPECT_FALSE(grp->Rename("new_name"));
  EXPECT_TRUE(grp->GetAttributes().empty());
  EXPECT_EQ(nullptr, grp->GetAttribute("attr"));
  EXPECT_EQ(nullptr, grp->CreateAttribute(
                         "attr", {}, GDALExtendedDataType::Create(GDT_Byte)));
  EXPECT_FALSE(grp->DeleteAttribute("attr"));

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
  auto h_grp =
      reinterpret_cast<GDALGroupH>(new std::shared_ptr<GDALGroup>(grp));
  EXPECT_EQ(1u, GDALGroupGetDataTypeCount(h_grp));
  GDALExtendedDataTypeH h_dt0 = GDALGroupGetDataType(h_grp, 0);
  ASSERT_NE(nullptr, h_dt0);
  GDALExtendedDataTypeRelease(h_dt0);
  EXPECT_EQ(nullptr, GDALGroupGetDataType(h_grp, 99));
  char** layer_names = GDALGroupGetVectorLayerNames(h_grp, nullptr);
  EXPECT_EQ(nullptr, layer_names);
  EXPECT_EQ(nullptr, GDALGroupOpenVectorLayer(h_grp, "layer", nullptr));
  GDALGroupRelease(h_grp);

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
  EXPECT_FALSE(sub2->CallCheckValid());
  sub->Deleted();
  EXPECT_FALSE(sub->CallCheckValid());

  // Base GDALDimension & GDALDimensionWeakIndexingVar.
  DummyBaseDimension base_dim("/sub", "dim1", "HORIZONTAL_X", "EAST", 10);
  EXPECT_EQ("dim1", base_dim.GetName());
  EXPECT_EQ("/sub/dim1", base_dim.GetFullName());
  EXPECT_EQ("HORIZONTAL_X", base_dim.GetType());
  EXPECT_EQ("EAST", base_dim.GetDirection());
  EXPECT_EQ(10u, base_dim.GetSize());
  EXPECT_EQ(nullptr, base_dim.GetIndexingVariable());
  EXPECT_FALSE(base_dim.SetIndexingVariable(nullptr));
  EXPECT_FALSE(base_dim.Rename("dim2"));
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
  CPLPopErrorHandler();
}

TEST(GdalmultidimTest, GroupNavigationAndCopyFrom) {
  GDALAllRegister();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  std::unique_ptr<GDALDataset> src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_x = src_root->CreateDimension("x", "HORIZONTAL_X", "EAST", 4);
  auto dim_y = src_root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 3);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  auto ar_x = src_root->CreateMDArray("x", {dim_x}, dt_f64);
  double x_vals[4] = {10.0, 20.0, 30.0, 40.0};
  const GUInt64 start1[1] = {0};
  const size_t count_x[1] = {4};
  ASSERT_TRUE(ar_x->Write(start1, count_x, nullptr, nullptr, dt_f64, x_vals));
  EXPECT_TRUE(dim_x->SetIndexingVariable(ar_x));

  auto root_attr = src_root->CreateAttribute(
      "title", {}, GDALExtendedDataType::CreateString());
  EXPECT_TRUE(root_attr->Write("test_dataset"));

  auto ar_2d = src_root->CreateMDArray("temp", {dim_y, dim_x}, dt_f64);
  double temp_vals[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {3, 4};
  ASSERT_TRUE(
      ar_2d->Write(start2, count2, nullptr, nullptr, dt_f64, temp_vals));
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
  auto sub2 = sub1->CreateGroup("sub2");
  auto sub2_ar = sub2->CreateMDArray("leaf_arr", {dim_x}, dt_f64);

  // Test recursive array listing and fullname navigation.
  auto full_names = src_root->GetMDArrayFullNamesRecursive();
  EXPECT_EQ(4u, full_names.size());

  EXPECT_NE(nullptr, src_root->OpenMDArrayFromFullname("/temp"));
  EXPECT_NE(nullptr, src_root->OpenMDArrayFromFullname("/sub1/sub_arr"));
  EXPECT_NE(nullptr, src_root->OpenMDArrayFromFullname("/sub1/sub2/leaf_arr"));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, src_root->OpenMDArrayFromFullname("invalid_no_slash"));
  EXPECT_EQ(nullptr, src_root->OpenMDArrayFromFullname("/"));
  EXPECT_EQ(nullptr, src_root->OpenMDArrayFromFullname("/no_such_grp/arr"));
  CPLPopErrorHandler();

  // OpenAttributeFromFullname.
  EXPECT_NE(nullptr, src_root->OpenAttributeFromFullname("/title"));
  EXPECT_NE(nullptr, src_root->OpenAttributeFromFullname("/temp/long_name"));
  EXPECT_NE(nullptr, src_root->OpenAttributeFromFullname("/sub1/sub_info"));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, src_root->OpenAttributeFromFullname("no_slash"));
  EXPECT_EQ(nullptr, src_root->OpenAttributeFromFullname("/nonexistent/attr"));
  CPLPopErrorHandler();

  // OpenGroupFromFullname & OpenDimensionFromFullname.
  EXPECT_EQ(nullptr, src_root->OpenGroupFromFullname("/"));
  EXPECT_NE(nullptr, src_root->OpenGroupFromFullname("/sub1"));
  EXPECT_NE(nullptr, src_root->OpenGroupFromFullname("/sub1/sub2"));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, src_root->OpenGroupFromFullname("no_slash"));
  EXPECT_EQ(nullptr, src_root->OpenGroupFromFullname("/no_grp/sub"));
  EXPECT_EQ(nullptr, src_root->OpenDimensionFromFullname("no_slash"));
  EXPECT_EQ(nullptr, src_root->OpenDimensionFromFullname("/"));
  EXPECT_EQ(nullptr, src_root->OpenDimensionFromFullname("/no_grp/d"));
  EXPECT_EQ(nullptr, src_root->OpenDimensionFromFullname("/sub1/no_dim"));
  CPLPopErrorHandler();
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
  GUInt64 total_cost = src_root->GetTotalCopyCost();
  EXPECT_GT(total_cost, 0u);

  // CopyFrom with AUTOSCALE and conditional ARRAY: options.
  const char* autoscale_types[] = {"Byte",   "Int8",  "UInt16", "Int16",
                                   "UInt32", "Int32", "UInt64", "Int64"};
  for (const char* dt_name : autoscale_types) {
    std::unique_ptr<GDALDataset> dst_ds(
        mem_drv->CreateMultiDimensional("dst", nullptr, nullptr));
    auto dst_root = dst_ds->GetRootGroup();
    CPLStringList opts;
    opts.AddString("ARRAY:IF(DIM=2):AUTOSCALE=YES");
    opts.AddString(
        (std::string("ARRAY:IF(NAME=temp):AUTOSCALE_DATA_TYPE=") + dt_name)
            .c_str());
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
  CPLPushErrorHandler(CPLQuietErrorHandler);
  {
    std::unique_ptr<GDALDataset> dst_ds(
        mem_drv->CreateMultiDimensional("dst_bad", nullptr, nullptr));
    auto dst_root = dst_ds->GetRootGroup();
    CPLStringList bad_opts;
    bad_opts.AddString("ARRAY:AUTOSCALE=YES");
    bad_opts.AddString("ARRAY:AUTOSCALE_DATA_TYPE=Float32");
    GUInt64 cur_cost = 0;
    EXPECT_FALSE(dst_root->CopyFrom(dst_root, src_ds.get(), src_root,
                                    /*bStrict=*/true, cur_cost, total_cost,
                                    nullptr, nullptr, bad_opts.List()));
  }
  {
    std::unique_ptr<GDALDataset> dst_ds(
        mem_drv->CreateMultiDimensional("dst_cancel", nullptr, nullptr));
    auto dst_root = dst_ds->GetRootGroup();
    GUInt64 cur_cost = 0;
    auto cancel_progress = [](double, const char*, void*) -> int {
      return FALSE;
    };
    EXPECT_FALSE(dst_root->CopyFrom(dst_root, src_ds.get(), src_root,
                                    /*bStrict=*/true, cur_cost, total_cost,
                                    cancel_progress, nullptr, nullptr));
  }
  CPLPopErrorHandler();
}

// ============================================================================
// 3. GDALAttribute, GDALRawResult, Helper Attribute Classes & C API Tests
// ============================================================================

TEST(GdalmultidimTest, AttributeReadWriteAndHelpers) {
  GDALAllRegister();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> ds(
      mem_drv->CreateMultiDimensional("attr_ds", nullptr, nullptr));
  auto root = ds->GetRootGroup();

  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  auto dt_i64 = GDALExtendedDataType::Create(GDT_Int64);
  auto dt_str = GDALExtendedDataType::CreateString();

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
  EXPECT_TRUE(attr_i->WriteInt(12345));
  EXPECT_EQ(12345, attr_i->ReadAsInt());
  EXPECT_EQ(12345, attr_i->ReadAsInt64());
  EXPECT_DOUBLE_EQ(12345.0, attr_i->ReadAsDouble());
  EXPECT_STREQ("12345", attr_i->ReadAsString());

  GDALRawResult raw_i = attr_i->ReadAsRaw();
  GByte* stolen = raw_i.StealData();
  ASSERT_NE(nullptr, stolen);
  CPLFree(stolen);

  auto attr_i64 = root->CreateAttribute("attr_i64", {}, dt_i64);
  EXPECT_TRUE(attr_i64->WriteInt64(1234567890123LL));
  EXPECT_EQ(1234567890123LL, attr_i64->ReadAsInt64());

  auto attr_d = root->CreateAttribute("attr_d", {}, dt_f64);
  EXPECT_TRUE(attr_d->Write(3.25));
  EXPECT_DOUBLE_EQ(3.25, attr_d->ReadAsDouble());
  double raw_val = 6.5;
  EXPECT_TRUE(attr_d->Write(&raw_val, 1));
  EXPECT_DOUBLE_EQ(6.5, attr_d->ReadAsDouble());
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(attr_d->Write(&raw_val, 2));
  CPLPopErrorHandler();

  // 1D array attributes (string, int, int64, double).
  auto attr_sa = root->CreateAttribute("attr_sa", {2}, dt_str);
  EXPECT_EQ(1u, attr_sa->GetDimensionsSize().size());
  CPLStringList sl;
  sl.AddString("str_one");
  sl.AddString("str_two");
  EXPECT_TRUE(attr_sa->Write(sl.List()));
  CPLStringList sl_read = attr_sa->ReadAsStringArray();
  EXPECT_EQ(2, sl_read.size());
  EXPECT_STREQ("str_one", sl_read[0]);
  EXPECT_STREQ("str_two", sl_read[1]);
  EXPECT_STREQ("str_one", attr_sa->ReadAsString());
  CPLPushErrorHandler(CPLQuietErrorHandler);
  CPLStringList sl_bad;
  sl_bad.AddString("only_one");
  EXPECT_FALSE(attr_sa->Write(sl_bad.List()));
  CPLPopErrorHandler();

  auto attr_ia = root->CreateAttribute("attr_ia", {3}, dt_i32);
  int i_vals[3] = {10, 20, 30};
  EXPECT_TRUE(attr_ia->Write(i_vals, 3));
  EXPECT_THAT(attr_ia->ReadAsIntArray(), testing::ElementsAre(10, 20, 30));
  EXPECT_THAT(attr_ia->ReadAsInt64Array(), testing::ElementsAre(10, 20, 30));
  EXPECT_THAT(attr_ia->ReadAsDoubleArray(),
              testing::ElementsAre(10.0, 20.0, 30.0));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(attr_ia->Write(i_vals, 2));
  CPLPopErrorHandler();

  auto attr_i64a = root->CreateAttribute("attr_i64a", {2}, dt_i64);
  int64_t i64_vals[2] = {100LL, 200LL};
  EXPECT_TRUE(attr_i64a->Write(i64_vals, 2));
  EXPECT_THAT(attr_i64a->ReadAsInt64Array(), testing::ElementsAre(100, 200));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(attr_i64a->Write(i64_vals, 1));
  CPLPopErrorHandler();

  auto attr_da = root->CreateAttribute("attr_da", {2}, dt_f64);
  double d_vals[2] = {1.5, 2.5};
  EXPECT_TRUE(attr_da->Write(d_vals, 2));
  EXPECT_THAT(attr_da->ReadAsDoubleArray(), testing::ElementsAre(1.5, 2.5));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(attr_da->Write(d_vals, 1));
  CPLPopErrorHandler();

  // GDALAttributeString, GDALAttributeNumeric, GDALMDIAsAttribute.
  GDALAttributeString helper_s("/", "hs", "42", GEDTST_JSON);
  EXPECT_TRUE(helper_s.GetDimensions().empty());
  EXPECT_EQ(GEDTC_STRING, helper_s.GetDataType().GetClass());
  EXPECT_STREQ("42", helper_s.ReadAsString());

  GDALAttributeNumeric helper_d("/", "hd", 12.5);
  EXPECT_TRUE(helper_d.GetDimensions().empty());
  EXPECT_EQ(GDT_Float64, helper_d.GetDataType().GetNumericDataType());
  EXPECT_DOUBLE_EQ(12.5, helper_d.ReadAsDouble());

  GDALAttributeNumeric helper_i("/", "hi", 77);
  EXPECT_EQ(GDT_Int32, helper_i.GetDataType().GetNumericDataType());
  EXPECT_EQ(77, helper_i.ReadAsInt());

  GDALAttributeNumeric helper_u32("/", "hu32", std::vector<GUInt32>{5, 6, 7});
  EXPECT_EQ(1u, helper_u32.GetDimensions().size());
  EXPECT_EQ(GDT_UInt32, helper_u32.GetDataType().GetNumericDataType());
  EXPECT_THAT(helper_u32.ReadAsIntArray(), testing::ElementsAre(5, 6, 7));

  GDALMDIAsAttribute helper_mdi("mdi_name", "mdi_val");
  EXPECT_TRUE(helper_mdi.GetDimensions().empty());
  EXPECT_EQ(GEDTC_STRING, helper_mdi.GetDataType().GetClass());
  EXPECT_STREQ("mdi_val", helper_mdi.ReadAsString());
}

// ============================================================================
// 4. GDALAbstractMDArray CheckReadWriteParams, Chunking & Protected Helpers
// ============================================================================

TEST(GdalmultidimTest, CheckReadWriteParamsAndChunking) {
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_comp_comps = std::vector<std::unique_ptr<GDALEDTComponent>>{};
  dt_comp_comps.push_back(std::make_unique<GDALEDTComponent>("a", 0, dt_f64));
  auto dt_comp = GDALExtendedDataType::Create("c", sizeof(double),
                                              std::move(dt_comp_comps));

  // 0D array CheckReadWriteParams & ProcessPerChunk.
  auto ar_0d = CustomTestMDArray::Create("/", "ar0", {}, dt_f64);
  EXPECT_EQ(0u, ar_0d->GetDimensionCount());
  EXPECT_EQ(1u, ar_0d->GetTotalElementsCount());
  double v0 = 0.0;
  EXPECT_TRUE(ar_0d->Read(nullptr, nullptr, nullptr, nullptr, dt_f64, &v0, &v0,
                          sizeof(double)));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  // Buffer out of bounds of alloc_start/alloc_size for 0D.
  double other_buf[2] = {0.0, 0.0};
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
  CPLPopErrorHandler();

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
  const GUInt64 start_ok[2] = {0, 0};
  const size_t count_ok[2] = {4, 5};
  // Positive strides with buffer_alloc_start.
  EXPECT_TRUE(ar_2d->Read(start_ok, count_ok, nullptr, nullptr, dt_f64,
                          buf.data(), buf.data(), buf.size() * sizeof(double)));

  // Negative arrayStep and negative bufferStride with buffer_alloc_start!
  const GUInt64 start_neg[2] = {3, 4};
  const size_t count_neg[2] = {2, 2};
  const GInt64 step_neg[2] = {-1, -1};
  const GPtrDiff_t stride_neg[2] = {-2, -1};
  ar_2d->mock_always_succeed_iread_ = true;
  EXPECT_TRUE(ar_2d->Read(start_neg, count_neg, step_neg, stride_neg, dt_f64,
                          &buf[3], buf.data(), 4 * sizeof(double)));
  ar_2d->mock_always_succeed_iread_ = false;

  CPLPushErrorHandler(CPLQuietErrorHandler);
  // Null buffer check.
  EXPECT_FALSE(
      ar_2d->Read(start_ok, count_ok, nullptr, nullptr, dt_f64, nullptr));
  // count[i] == 0.
  const size_t count_zero[2] = {4, 0};
  EXPECT_FALSE(
      ar_2d->Read(start_ok, count_zero, nullptr, nullptr, dt_f64, buf.data()));
  // arrayStartIdx out of bounds.
  const GUInt64 start_oob[2] = {4, 0};
  EXPECT_FALSE(
      ar_2d->Read(start_oob, count_ok, nullptr, nullptr, dt_f64, buf.data()));
  // Positive step out of bounds.
  const GInt64 step_oob[2] = {2, 1};
  EXPECT_FALSE(
      ar_2d->Read(start_ok, count_ok, step_oob, nullptr, dt_f64, buf.data()));
  // Negative step out of bounds.
  const GUInt64 start_small[2] = {0, 0};
  EXPECT_FALSE(ar_2d->Read(start_small, count_neg, step_neg, nullptr, dt_f64,
                           buf.data()));
  // Negative step INT64_MIN overflow.
  const GInt64 step_min[2] = {std::numeric_limits<GInt64>::min(), 1};
  EXPECT_FALSE(
      ar_2d->Read(start_neg, count_neg, step_min, nullptr, dt_f64, buf.data()));
  // buffer_alloc_size too small for positive stride.
  EXPECT_FALSE(ar_2d->Read(start_ok, count_ok, nullptr, nullptr, dt_f64,
                           buf.data(), buf.data(), 2 * sizeof(double)));
  // buffer_alloc_size too small for negative stride.
  EXPECT_FALSE(ar_2d->Read(start_neg, count_neg, step_neg, stride_neg, dt_f64,
                           &buf[1], buf.data(), 4 * sizeof(double)));
  CPLPopErrorHandler();

  // GetProcessingChunkSize & ProcessPerChunk on 2D array.
  EXPECT_THAT(ar_2d->GetBlockSize(), testing::ElementsAre(0, 0));
  auto pcs1 = ar_2d->GetProcessingChunkSize(1024);
  EXPECT_EQ(2u, pcs1.size());
  ar_2d->SetCustomBlockSize({2, 2});
  auto pcs2 = ar_2d->GetProcessingChunkSize(1024);
  EXPECT_THAT(pcs2, testing::ElementsAre(4, 5));
  auto pcs3 = ar_2d->GetProcessingChunkSize(16);
  EXPECT_THAT(pcs3, testing::ElementsAre(2, 2));

  const GUInt64 p_start[2] = {0, 0};
  const GUInt64 p_count[2] = {4, 5};
  const size_t p_chunk[2] = {2, 2};
  int chunk_calls_2d = 0;
  EXPECT_TRUE(ar_2d->ProcessPerChunk(p_start, p_count, p_chunk, chunk_cb,
                                     &chunk_calls_2d));
  EXPECT_EQ(6, chunk_calls_2d);

  // ProcessPerChunk error cases & callback returning false.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const size_t p_chunk_zero[2] = {2, 0};
  EXPECT_FALSE(ar_2d->ProcessPerChunk(p_start, p_count, p_chunk_zero, chunk_cb,
                                      &chunk_calls_2d));
  const GUInt64 p_count_oob[2] = {10, 5};
  EXPECT_FALSE(ar_2d->ProcessPerChunk(p_start, p_count_oob, p_chunk, chunk_cb,
                                      &chunk_calls_2d));
  auto fail_cb = [](GDALAbstractMDArray*, const GUInt64*, const size_t*,
                    GUInt64, GUInt64, void*) -> bool { return false; };
  EXPECT_FALSE(
      ar_2d->ProcessPerChunk(p_start, p_count, p_chunk, fail_cb, nullptr));
  CPLPopErrorHandler();
}

TEST(GdalmultidimTest, MDArrayProtectedTransposedAndContiguousHelpers) {
  auto dim0 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d0", "", "", 2);
  auto dim1 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d1", "", "", 3);
  auto dim2 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d2", "", "", 2);

  // Test ReadForTransposedRequest for 2D (last 2 dims transposed) and 3D
  // (general N-D transpose) across element sizes 1, 2, 4, 8, and strings!
  const GDALDataType sizes_dts[] = {GDT_Byte, GDT_UInt16, GDT_UInt32,
                                    GDT_Float64};
  for (GDALDataType eDT : sizes_dts) {
    auto dt = GDALExtendedDataType::Create(eDT);
    // 2D (2x3) transposed stride: [1, 2].
    auto ar2 = CustomTestMDArray::Create("/", "a2", {dim0, dim1}, dt);
    const GUInt64 start2[2] = {0, 0};
    const size_t count2[2] = {2, 3};
    const GInt64 step2[2] = {1, 1};
    const GPtrDiff_t stride2_trans[2] = {1, 2};
    EXPECT_TRUE(ar2->CallIsTransposedRequest(count2, stride2_trans));
    std::vector<GByte> dst2(6 * dt.GetSize(), 0);
    EXPECT_TRUE(ar2->CallReadForTransposedRequest(
        start2, count2, step2, stride2_trans, dt, dst2.data()));

    // 3D (2x3x2) transposed [2, 0, 1] -> strides [1, 2, 6].
    auto ar3 = CustomTestMDArray::Create("/", "a3", {dim0, dim1, dim2}, dt);
    const GUInt64 start3[3] = {0, 0, 0};
    const size_t count3[3] = {2, 3, 2};
    const GInt64 step3[3] = {1, 1, 1};
    const GPtrDiff_t stride3_trans[3] = {1, 2, 6};
    EXPECT_TRUE(ar3->CallIsTransposedRequest(count3, stride3_trans));
    std::vector<GByte> dst3(12 * dt.GetSize(), 0);
    EXPECT_TRUE(ar3->CallReadForTransposedRequest(
        start3, count3, step3, stride3_trans, dt, dst3.data()));
  }

  // 3D with string type (NeedsFreeDynamicMemory() == true) in
  // ReadForTransposedRequest and ReadUsingContiguousIRead.
  auto dt_str = GDALExtendedDataType::CreateString();
  auto ar_str =
      CustomTestMDArray::Create("/", "as", {dim0, dim1, dim2}, dt_str);
  const GUInt64 start3[3] = {0, 0, 0};
  const size_t count3[3] = {2, 3, 2};
  const GInt64 step3[3] = {1, 1, 1};
  const GPtrDiff_t stride3_trans[3] = {1, 2, 6};
  std::vector<char*> dst_strs(12, nullptr);
  EXPECT_TRUE(ar_str->CallReadForTransposedRequest(
      start3, count3, step3, stride3_trans, dt_str, dst_strs.data()));
  for (char*& s : dst_strs) {
    CPLFree(s);
    s = nullptr;
  }

  // ReadUsingContiguousIRead with negative steps and different bufferDataType.
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  auto ar_num = CustomTestMDArray::Create("/", "an", {dim0, dim1}, dt_f64);
  const GUInt64 start_neg[2] = {1, 2};
  const size_t count_neg[2] = {2, 2};
  const GInt64 step_neg[2] = {-1, -1};
  const GPtrDiff_t stride_contig[2] = {2, 1};
  EXPECT_FALSE(ar_num->CallIsStepOneContiguous(count_neg, step_neg,
                                               stride_contig, dt_i32));
  int32_t out_i32[4] = {0, 0, 0, 0};
  EXPECT_TRUE(ar_num->CallReadUsingContiguousIRead(
      start_neg, count_neg, step_neg, stride_contig, dt_i32, out_i32));

  // Base methods on CustomTestMDArray.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ("", ar_num->GetUnit());
  EXPECT_FALSE(ar_num->SetUnit("m"));
  EXPECT_EQ(nullptr, ar_num->GetSpatialRef());
  EXPECT_FALSE(ar_num->SetSpatialRef(nullptr));
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
  EXPECT_FALSE(ar_num->SetRawNoDataValue(nullptr));
  EXPECT_FALSE(ar_num->SetNoDataValue(1.0));
  EXPECT_FALSE(ar_num->SetNoDataValue(static_cast<int64_t>(1)));
  EXPECT_FALSE(ar_num->SetNoDataValue(static_cast<uint64_t>(1)));
  EXPECT_FALSE(ar_num->Resize({4, 4}, nullptr));
  bool has_val = true;
  GDALDataType st = GDT_Byte;
  EXPECT_DOUBLE_EQ(0.0, ar_num->GetOffset(&has_val, &st));
  EXPECT_FALSE(has_val);
  EXPECT_DOUBLE_EQ(1.0, ar_num->GetScale(&has_val, &st));
  EXPECT_FALSE(has_val);
  EXPECT_FALSE(ar_num->SetOffset(1.0));
  EXPECT_FALSE(ar_num->SetScale(2.0));
  EXPECT_EQ(nullptr, ar_num->GetStructuralInfo());
  EXPECT_EQ(nullptr, ar_num->GetRootGroup());
  EXPECT_TRUE(ar_num->GetCoordinateVariables().empty());
  EXPECT_FALSE(
      ar_num->CallBaseSetStatistics(false, 0, 1, 0.5, 0.1, 4, nullptr));
  ar_num->ClearStatistics();
  EXPECT_TRUE(ar_num->CallIsCacheable());
  EXPECT_EQ("a_b_c_1", CustomTestMDArray::CallMassageName("a/b:c_1"));
  EXPECT_FALSE(ar_num->Rename("new_ar"));
  ar_num->CallBaseRename("new_ar");
  EXPECT_EQ("new_ar", ar_num->GetName());
  ar_num->ParentRenamed("/sub");
  EXPECT_EQ("/sub/new_ar", ar_num->GetFullName());
  ar_num->ParentRenamed("");
  EXPECT_EQ("/new_ar", ar_num->GetFullName());
  ar_num->CallNotifyChildren();
  ar_num->ParentDeleted();
  EXPECT_FALSE(ar_num->CallCheckValid());
  ar_num->Deleted();
  CPLPopErrorHandler();
}

TEST(GdalmultidimTest, AdviseReadRegularlySpacedAndGuessGeoTransform) {
  auto dim_large = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "x", "HORIZONTAL_X", "EAST", 10);
  auto dim_y = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "y", "HORIZONTAL_Y", "NORTH", 4);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

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

  double start = 0.0, incr = 0.0;
  EXPECT_TRUE(reg_x->IsRegularlySpaced(start, incr));
  EXPECT_DOUBLE_EQ(105.0, start);
  EXPECT_DOUBLE_EQ(10.0, incr);

  // Read with negative step on GDALMDArrayRegularlySpaced.
  const GUInt64 r_start[1] = {3};
  const size_t r_count[1] = {3};
  const GInt64 r_step[1] = {-1};
  double r_vals[3] = {0, 0, 0};
  EXPECT_TRUE(reg_x->Read(r_start, r_count, r_step, nullptr, dt_f64, r_vals));
  EXPECT_DOUBLE_EQ(135.0, r_vals[0]);
  EXPECT_DOUBLE_EQ(125.0, r_vals[1]);
  EXPECT_DOUBLE_EQ(115.0, r_vals[2]);

  // Test IsRegularlySpaced two-stage check when nCount >= 5 and nBlockSize <=
  // nCount / 2.
  auto custom_x = CustomTestMDArray::Create("/", "cx", {dim_large}, dt_f64);
  custom_x->SetCustomBlockSize({2});
  double* cx_ptr = reinterpret_cast<double*>(custom_x->RawData().data());
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
  dim_large->SetIndexingVariable(reg_x);
  dim_y->SetIndexingVariable(reg_y);
  auto ar_grid =
      CustomTestMDArray::Create("/", "grid", {dim_y, dim_large}, dt_f64);

  double gt[6] = {0};
  EXPECT_TRUE(ar_grid->GuessGeoTransform(1, 0, /*bPixelIsPoint=*/false, gt));
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
  const GUInt64 adv_start[2] = {0, 0};
  const size_t adv_count[2] = {2, 2};
  EXPECT_TRUE(ar_grid->AdviseRead(adv_start, adv_count));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  const size_t adv_bad_count[2] = {10, 2};
  EXPECT_FALSE(ar_grid->AdviseRead(adv_start, adv_bad_count));
  CPLPopErrorHandler();
}

// ============================================================================
// 5. Views: GetView (Sliced & ExtractField), Transpose, GetUnscaled, GetMask
// ============================================================================

TEST(GdalmultidimTest, SlicedAndExtractFieldViews) {
  GDALAllRegister();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  std::unique_ptr<GDALDataset> ds(
      mem_drv->CreateMultiDimensional("view_ds", nullptr, nullptr));
  auto root = ds->GetRootGroup();

  auto dim_z = root->CreateDimension("z", "VERTICAL", "UP", 3);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 4);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 5);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  auto ar_x = root->CreateMDArray("x", {dim_x}, dt_f64);
  dim_x->SetIndexingVariable(ar_x);

  auto ar_3d = root->CreateMDArray("cube", {dim_z, dim_y, dim_x}, dt_f64);
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
  const GUInt64 start3[3] = {0, 0, 0};
  const size_t count3[3] = {3, 4, 5};
  ASSERT_TRUE(
      ar_3d->Write(start3, count3, nullptr, nullptr, dt_f64, cube_vals.data()));

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
  EXPECT_THAT(v_slice1->GetBlockSize(), testing::ElementsAre(0, 0));
  EXPECT_NE(nullptr, v_slice1->GetAttribute("info"));
  EXPECT_EQ(1u, v_slice1->GetAttributes().size());

  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {4, 2};
  double sub_vals[8] = {0};
  EXPECT_TRUE(
      v_slice1->Read(start2, count2, nullptr, nullptr, dt_f64, sub_vals));
  EXPECT_TRUE(
      v_slice1->Write(start2, count2, nullptr, nullptr, dt_f64, sub_vals));
  EXPECT_FALSE(v_slice1->AdviseRead(start2, count2));

  // Chained brackets, ellipsis, newaxis, negative indices.
  EXPECT_NE(nullptr, ar_3d->GetView("[1][2][3]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[...,1]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[1,...]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[newaxis,...,np.newaxis]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[-1,-3:-1:1,3:0:-1]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[:,:2,1:]"));
  EXPECT_NE(nullptr, ar_3d->GetView("[0,0]"));

  // Error cases for GetView.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, v_at->GetView("[0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView(""));
  EXPECT_EQ(nullptr, ar_3d->GetView("no_brackets"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[0"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[...,...]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[0,0,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[10,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[-10,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[1:2:3:4,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[1:2:0,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[3:1:1,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[1:3:-1,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[abc,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[1:abc,0,0]"));
  EXPECT_EQ(nullptr, ar_3d->GetView("[1:2:abc,0,0]"));
  CPLPopErrorHandler();

  // Field extraction on compound array: ['x'], ["a'b\\c"].
  std::vector<std::unique_ptr<GDALEDTComponent>> comps;
  comps.push_back(std::make_unique<GDALEDTComponent>("x", 0, dt_f64));
  comps.push_back(
      std::make_unique<GDALEDTComponent>("a'b\\c", sizeof(double), dt_f64));
  auto dt_comp = GDALExtendedDataType::Create("comp", 2 * sizeof(double),
                                              std::move(comps));
  auto ar_comp = root->CreateMDArray("comp_arr", {dim_x}, dt_comp);
  ar_comp->SetUnit("u");
  ar_comp->SetSpatialRef(&srs);
  double comp_nd[2] = {-1.0, -2.0};
  ar_comp->SetRawNoDataValue(comp_nd);
  double comp_vals[10] = {1, 10, 2, 20, 3, 30, 4, 40, 5, 50};
  const GUInt64 start1[1] = {0};
  const size_t count1[1] = {5};
  ASSERT_TRUE(
      ar_comp->Write(start1, count1, nullptr, nullptr, dt_comp, comp_vals));

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
  EXPECT_TRUE(field_x->AdviseRead(start1, count1));

  double fx_out[5] = {0};
  EXPECT_TRUE(field_x->Read(start1, count1, nullptr, nullptr, dt_f64, fx_out));
  EXPECT_THAT(fx_out, testing::ElementsAre(1.0, 2.0, 3.0, 4.0, 5.0));

  auto field_esc = ar_comp->GetView("['a\\'b\\\\c']");
  ASSERT_NE(nullptr, field_esc);
  EXPECT_DOUBLE_EQ(-2.0, field_esc->GetNoDataValueAsDouble());
  EXPECT_TRUE(
      field_esc->Read(start1, count1, nullptr, nullptr, dt_f64, fx_out));
  EXPECT_THAT(fx_out, testing::ElementsAre(10.0, 20.0, 30.0, 40.0, 50.0));

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, (*ar_3d)["x"]);
  EXPECT_EQ(nullptr, (*ar_comp)["nonexistent_field"]);
  CPLPopErrorHandler();
}

TEST(GdalmultidimTest, TransposeAndUnscaledViews) {
  GDALAllRegister();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> ds(
      mem_drv->CreateMultiDimensional("tu_ds", nullptr, nullptr));
  auto root = ds->GetRootGroup();

  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 2);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 3);
  auto dt_i16 = GDALExtendedDataType::Create(GDT_Int16);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  auto ar = root->CreateMDArray("ar", {dim_y, dim_x}, dt_i16);
  ar->SetUnit("degC");
  ar->SetScale(0.5, GDT_Float32);
  ar->SetOffset(10.0, GDT_Float32);
  int16_t nd = -99;
  ar->SetRawNoDataValue(&nd);
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetDataAxisToSRSAxisMapping({1, 2});
  ar->SetSpatialRef(&srs);
  auto attr = ar->CreateAttribute("a", {}, dt_f64);
  attr->Write(1.0);

  int16_t vals[6] = {10, 20, -99, 40, 50, 60};
  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {2, 3};
  ASSERT_TRUE(ar->Write(start2, count2, nullptr, nullptr, dt_i16, vals));

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
  EXPECT_THAT(trans->GetBlockSize(), testing::ElementsAre(0, 0, 0));
  EXPECT_NE(nullptr, trans->GetAttribute("a"));
  EXPECT_EQ(1u, trans->GetAttributes().size());

  const GUInt64 start3[3] = {0, 0, 0};
  const size_t count3[3] = {3, 1, 2};
  int16_t trans_vals[6] = {0};
  EXPECT_TRUE(
      trans->Read(start3, count3, nullptr, nullptr, dt_i16, trans_vals));
  EXPECT_THAT(trans_vals, testing::ElementsAre(10, 40, 20, 50, -99, 60));
  EXPECT_TRUE(
      trans->Write(start3, count3, nullptr, nullptr, dt_i16, trans_vals));
  EXPECT_TRUE(trans->AdviseRead(start3, count3));

  // Transpose error cases.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, ar->Transpose({0}));
  EXPECT_EQ(nullptr, ar->Transpose({0, 5}));
  EXPECT_EQ(nullptr, ar->Transpose({0, 0}));
  CPLPopErrorHandler();

  // GetUnscaled tests.
  // 1) Scale=1, Offset=0 returns self.
  auto unscaled_self = ar->GetUnscaled(1.0, 0.0);
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
  EXPECT_TRUE(unscaled->AdviseRead(start2, count2));

  // Read as Float64 and as Int32.
  double u_f64[6] = {0};
  EXPECT_TRUE(unscaled->Read(start2, count2, nullptr, nullptr, dt_f64, u_f64));
  EXPECT_DOUBLE_EQ(15.0, u_f64[0]);
  EXPECT_DOUBLE_EQ(20.0, u_f64[1]);
  EXPECT_TRUE(std::isnan(u_f64[2]));

  int32_t u_i32[6] = {0};
  auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  EXPECT_TRUE(unscaled->Read(start2, count2, nullptr, nullptr, dt_i32, u_i32));

  // Write to unscaled from Float64 and from Int32, including nodata.
  EXPECT_TRUE(unscaled->Write(start2, count2, nullptr, nullptr, dt_f64, u_f64));
  EXPECT_TRUE(unscaled->Write(start2, count2, nullptr, nullptr, dt_i32, u_i32));
  double new_nd = -500.0;
  EXPECT_TRUE(unscaled->SetRawNoDataValue(&new_nd));

  // 3) 0D unscaled array & complex unscaled array (GDT_CInt16 -> GDT_CFloat64).
  auto ar_0d = ar->at(0, 0)->GetUnscaled(2.0, 5.0, -999.0);
  ASSERT_NE(nullptr, ar_0d);
  double v0 = 0.0;
  EXPECT_TRUE(ar_0d->Read(nullptr, nullptr, nullptr, nullptr, dt_f64, &v0));
  EXPECT_TRUE(ar_0d->Write(nullptr, nullptr, nullptr, nullptr, dt_f64, &v0));

  auto dt_ci16 = GDALExtendedDataType::Create(GDT_CInt16);
  auto dt_cf64 = GDALExtendedDataType::Create(GDT_CFloat64);
  auto ar_comp = root->CreateMDArray("c_ar", {dim_x}, dt_ci16);
  auto unscaled_comp = ar_comp->GetUnscaled(2.0, 1.0);
  ASSERT_NE(nullptr, unscaled_comp);
  EXPECT_EQ(GDT_CFloat64, unscaled_comp->GetDataType().GetNumericDataType());
  const GUInt64 start1[1] = {0};
  const size_t count1[1] = {3};
  double c_vals[6] = {3.0, 5.0, 7.0, 9.0, 11.0, 13.0};
  EXPECT_TRUE(
      unscaled_comp->Write(start1, count1, nullptr, nullptr, dt_cf64, c_vals));
  double c_out[6] = {0};
  EXPECT_TRUE(
      unscaled_comp->Read(start1, count1, nullptr, nullptr, dt_cf64, c_out));
  EXPECT_DOUBLE_EQ(3.0, c_out[0]);
  EXPECT_DOUBLE_EQ(5.0, c_out[1]);

  // Float32 with scale=-1, offset=0 preserves Float32.
  auto dt_f32 = GDALExtendedDataType::Create(GDT_Float32);
  auto ar_f32 = root->CreateMDArray("f32_ar", {dim_x}, dt_f32);
  auto unscaled_f32 = ar_f32->GetUnscaled(-1.0, 0.0);
  EXPECT_EQ(GDT_Float32, unscaled_f32->GetDataType().GetNumericDataType());
}

TEST(GdalmultidimTest, MaskViewAllTypesAndFlags) {
  GDALAllRegister();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> ds(
      mem_drv->CreateMultiDimensional("mask_ds", nullptr, nullptr));
  auto root = ds->GetRootGroup();
  auto dim = root->CreateDimension("x", "", "", 5);
  auto dim_y = root->CreateDimension("y", "", "", 2);
  auto dt_byte = GDALExtendedDataType::Create(GDT_Byte);
  auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

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

  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {2, 5};
  EXPECT_TRUE(mask_fast->AdviseRead(start2, count2));
  GByte m_buf[10] = {0};
  // Contiguous Byte read.
  EXPECT_TRUE(
      mask_fast->Read(start2, count2, nullptr, nullptr, dt_byte, m_buf));
  EXPECT_EQ(1, m_buf[0]);
  // Strided Byte read.
  const GPtrDiff_t stride2[2] = {10, 2};
  std::vector<GByte> m_strided(20, 0);
  EXPECT_TRUE(mask_fast->Read(start2, count2, nullptr, stride2, dt_byte,
                              m_strided.data()));
  EXPECT_EQ(1, m_strided[0]);
  // Non-Byte buffer read.
  int32_t m_i32[10] = {0};
  EXPECT_TRUE(mask_fast->Read(start2, count2, nullptr, nullptr, dt_i32, m_i32));
  EXPECT_EQ(1, m_i32[0]);

  // 2) General path across all numeric data types with nodata, valid_min,
  // valid_max, missing_value, _FillValue.
  const GDALDataType mask_types[] = {GDT_Byte,    GDT_Int8,    GDT_UInt16,
                                     GDT_Int16,   GDT_UInt32,  GDT_Int32,
                                     GDT_UInt64,  GDT_Int64,   GDT_Float16,
                                     GDT_Float32, GDT_Float64, GDT_CFloat64};
  const GUInt64 start1[1] = {0};
  const size_t count1[1] = {5};
  for (int idx = 0;
       idx < static_cast<int>(sizeof(mask_types) / sizeof(mask_types[0]));
       ++idx) {
    GDALDataType eDT = mask_types[idx];
    auto dt = GDALExtendedDataType::Create(eDT);
    auto ar = root->CreateMDArray(CPLSPrintf("ar_%d", idx), {dim}, dt);
    ar->SetNoDataValue(10.0);
    if (!GDALDataTypeIsComplex(eDT)) {
      auto attr_mv = ar->CreateAttribute("missing_value", {1}, dt_f64);
      double mv = 20.0;
      attr_mv->Write(&mv, 1);
      auto attr_fv = ar->CreateAttribute("_FillValue", {}, dt_f64);
      attr_fv->Write(30.0);
      auto attr_vmin = ar->CreateAttribute("valid_min", {}, dt_f64);
      attr_vmin->Write(2.0);
      auto attr_vmax = ar->CreateAttribute("valid_max", {}, dt_f64);
      attr_vmax->Write(50.0);
    }
    double vals[10] = {1.0, 5.0, 10.0, 20.0, 60.0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(ar->Write(start1, count1, nullptr, nullptr, dt_f64, vals));

    auto mask = ar->GetMask(nullptr);
    ASSERT_NE(nullptr, mask);
    GByte out_b[5] = {9, 9, 9, 9, 9};
    EXPECT_TRUE(mask->Read(start1, count1, nullptr, nullptr, dt_byte, out_b));
    if (!GDALDataTypeIsComplex(eDT)) {
      EXPECT_THAT(out_b, testing::ElementsAre(0, 1, 0, 0, 0));
    }
    // Also test non-Byte buffer and strided buffer on general path.
    int32_t out_i[5] = {0};
    EXPECT_TRUE(mask->Read(start1, count1, nullptr, nullptr, dt_i32, out_i));
    const GPtrDiff_t stride_2[1] = {2};
    GByte out_strided[10] = {0};
    EXPECT_TRUE(
        mask->Read(start1, count1, nullptr, stride_2, dt_byte, out_strided));
  }

  // 3) valid_range attribute & UNMASK_FLAGS with flag_meanings, flag_values,
  // flag_masks.
  auto ar_flags = root->CreateMDArray("flags_ar", {dim}, dt_byte);
  auto vr = ar_flags->CreateAttribute("valid_range", {2}, dt_byte);
  int vr_vals[2] = {0, 10};
  vr->Write(vr_vals, 2);
  auto fm = ar_flags->CreateAttribute("flag_meanings", {},
                                      GDALExtendedDataType::CreateString());
  fm->Write("low_quality cloud_cover valid_snow");
  auto fv = ar_flags->CreateAttribute("flag_values", {3}, dt_byte);
  int fv_vals[3] = {1, 2, 6};
  fv->Write(fv_vals, 3);
  auto fmask = ar_flags->CreateAttribute("flag_masks", {3}, dt_byte);
  int fmask_vals[3] = {1, 2, 6};
  fmask->Write(fmask_vals, 3);

  GByte flag_data[5] = {0, 1, 2, 6, 20};
  ar_flags->Write(start1, count1, nullptr, nullptr, dt_byte, flag_data);

  CPLStringList unmask_opts;
  unmask_opts.SetNameValue("UNMASK_FLAGS", "valid_snow,low_quality");
  auto mask_flags = ar_flags->GetMask(unmask_opts.List());
  ASSERT_NE(nullptr, mask_flags);
  GByte out_f[5] = {0};
  EXPECT_TRUE(
      mask_flags->Read(start1, count1, nullptr, nullptr, dt_byte, out_f));
  EXPECT_THAT(out_f, testing::ElementsAre(0, 1, 0, 1, 0));

  // UNMASK_FLAGS error cases.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  CPLStringList bad_flag_opts;
  bad_flag_opts.SetNameValue("UNMASK_FLAGS", "unknown_flag");
  EXPECT_EQ(nullptr, ar_flags->GetMask(bad_flag_opts.List()));
  EXPECT_EQ(nullptr, ar_fast->GetMask(unmask_opts.List()));
  auto ar_str = root->CreateMDArray("str_ar", {dim},
                                    GDALExtendedDataType::CreateString());
  EXPECT_EQ(nullptr, ar_str->GetMask(nullptr));
  CPLPopErrorHandler();
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
  GDALAllRegister();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> ds(
      mem_drv->CreateMultiDimensional("res_ds", nullptr, nullptr));
  auto root = ds->GetRootGroup();

  auto dim_z = root->CreateDimension("z", "VERTICAL", "UP", 2);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 4);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 4);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  auto reg_x =
      GDALMDArrayRegularlySpaced::Create("/", "x", dim_x, 0.5, 1.0, 0.0);
  auto reg_y =
      GDALMDArrayRegularlySpaced::Create("/", "y", dim_y, 3.5, -1.0, 0.0);
  dim_x->SetIndexingVariable(reg_x);
  dim_y->SetIndexingVariable(reg_y);

  auto ar_3d = root->CreateMDArray("ar3", {dim_z, dim_y, dim_x}, dt_f64);
  ar_3d->SetUnit("m");
  ar_3d->SetOffset(1.0);
  ar_3d->SetScale(2.0);
  double nd = -999.0;
  ar_3d->SetRawNoDataValue(&nd);
  auto attr = ar_3d->CreateAttribute("k", {}, dt_f64);
  attr->Write(5.0);
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  ar_3d->SetSpatialRef(&srs);

  std::vector<double> data3(32, 10.0);
  const GUInt64 start3[3] = {0, 0, 0};
  const size_t count3[3] = {2, 4, 4};
  ar_3d->Write(start3, count3, nullptr, nullptr, dt_f64, data3.data());

  // All supported resample algorithms.
  const GDALRIOResampleAlg algs[] = {GRIORA_NearestNeighbour,
                                     GRIORA_Bilinear,
                                     GRIORA_Cubic,
                                     GRIORA_CubicSpline,
                                     GRIORA_Lanczos,
                                     GRIORA_Average,
                                     GRIORA_Mode,
                                     GRIORA_RMS};
  for (GDALRIOResampleAlg alg : algs) {
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
  const size_t res_count3[3] = {2, 2, 2};
  double res_out[8] = {0};
  EXPECT_TRUE(
      res_3d->Read(start3, res_count3, nullptr, nullptr, dt_f64, res_out));
  EXPECT_NEAR(10.0, res_out[0], 1e-5);

  // Also test GetResampled with only new_dim_x or only new_dim_y, and without
  // indexing variables on new dims.
  EXPECT_EQ(nullptr,
            ar_3d->GetResampled({dim_z, nullptr, new_dim_x},
                                GRIORA_NearestNeighbour, nullptr, nullptr));
  EXPECT_EQ(nullptr,
            ar_3d->GetResampled({dim_z, new_dim_y, nullptr},
                                GRIORA_NearestNeighbour, nullptr, nullptr));
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
  double lon_vals[16], lat_vals[16];
  for (int j = 0; j < 4; ++j) {
    for (int i = 0; i < 4; ++i) {
      lon_vals[j * 4 + i] = i * 1.0;
      lat_vals[j * 4 + i] = 3.0 - j * 1.0;
    }
  }
  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {4, 4};
  ar_lon->Write(start2, count2, nullptr, nullptr, dt_f64, lon_vals);
  ar_lat->Write(start2, count2, nullptr, nullptr, dt_f64, lat_vals);

  auto cv_ar =
      CoordVarsTestMDArray::Create("/", "cv_ar", {dim_y4, dim_x4}, dt_f64);
  cv_ar->SetCoordVars({ar_lon, ar_lat});
  auto res_cv = cv_ar->GetResampled({nullptr, nullptr}, GRIORA_NearestNeighbour,
                                    nullptr, nullptr);
  EXPECT_NE(nullptr, res_cv);
  if (res_cv) {
    double sample = 0;
    const size_t c1[2] = {1, 1};
    res_cv->Read(start2, c1, nullptr, nullptr, dt_f64, &sample);
    std::unique_ptr<GDALDataset> ds_res(
        res_cv->AsClassicDataset(1, 0, nullptr, nullptr));
    if (ds_res && ds_res->GetRasterCount() > 0) {
      int nBXSize = 0, nBYSize = 0;
      ds_res->GetRasterBand(1)->GetBlockSize(&nBXSize, &nBYSize);
      std::vector<double> block(static_cast<size_t>(nBXSize) * nBYSize, 0.0);
      EXPECT_EQ(CE_None,
                ds_res->GetRasterBand(1)->ReadBlock(0, 0, block.data()));
    }
  }

  // NASA EMIT GLT orthorectification path in GetResampled!
  auto dim_dt = root->CreateDimension("downtrack", "", "", 4);
  auto dim_ct = root->CreateDimension("crosstrack", "", "", 4);
  auto dim_bands = root->CreateDimension("bands", "", "", 2);
  auto gt_attr = root->CreateAttribute("geotransform", {6}, dt_f64);
  double emit_gt[6] = {0.0, 1.0, 0.0, 4.0, 0.0, -1.0};
  gt_attr->Write(emit_gt, 6);
  auto loc_grp = root->CreateGroup("location");
  auto dim_oy = loc_grp->CreateDimension("ortho_y", "", "", 3);
  auto dim_ox = loc_grp->CreateDimension("ortho_x", "", "", 3);
  auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  auto glt_x = loc_grp->CreateMDArray("glt_x", {dim_oy, dim_ox}, dt_i32);
  auto glt_y = loc_grp->CreateMDArray("glt_y", {dim_oy, dim_ox}, dt_i32);
  int32_t glt_vals[9] = {1, 2, 3, 1, 2, 3, 1, 2, 3};
  const size_t count_glt[2] = {3, 3};
  glt_x->Write(start2, count_glt, nullptr, nullptr, dt_i32, glt_vals);
  glt_y->Write(start2, count_glt, nullptr, nullptr, dt_i32, glt_vals);

  auto emit_ar = CustomTestMDArray::Create("/", "reflectance",
                                           {dim_dt, dim_ct, dim_bands}, dt_f64);
  emit_ar->SetRootGroup(root);
  auto emit_res = emit_ar->GetResampled(
      {nullptr, nullptr, dim_bands}, GRIORA_NearestNeighbour, nullptr, nullptr);
  ASSERT_NE(nullptr, emit_res);
  const size_t emit_count[3] = {3, 3, 2};
  std::vector<double> emit_out(18, 0.0);
  EXPECT_TRUE(emit_res->Read(start3, emit_count, nullptr, nullptr, dt_f64,
                             emit_out.data()));

  // GetResampled error cases.
  CPLPushErrorHandler(CPLQuietErrorHandler);
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
  CPLPopErrorHandler();
}

TEST(GdalmultidimTest, AsClassicDatasetAndBandMetadata) {
  GDALAllRegister();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> ds(
      mem_drv->CreateMultiDimensional("classic_ds", nullptr, nullptr));
  auto root = ds->GetRootGroup();

  auto dim_b = root->CreateDimension("band", "PARAMETRIC", "", 2);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 3);
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 4);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_str = GDALExtendedDataType::CreateString();

  auto ar_b = root->CreateMDArray("band_vals", {dim_b}, dt_f64);
  ar_b->SetUnit("um");
  double b_vals[2] = {0.45, 0.55};
  const GUInt64 start1[1] = {0};
  const size_t count_b[1] = {2};
  ar_b->Write(start1, count_b, nullptr, nullptr, dt_f64, b_vals);
  auto b_attr = ar_b->CreateAttribute("units", {}, dt_str);
  b_attr->Write("nm");
  dim_b->SetIndexingVariable(ar_b);

  auto ar_fwhm = root->CreateMDArray("fwhm_vals", {dim_b}, dt_f64);
  double f_vals[2] = {10.0, 20.0};
  ar_fwhm->Write(start1, count_b, nullptr, nullptr, dt_f64, f_vals);

  auto root_wl_attr = root->CreateAttribute("wl_attr", {2}, dt_f64);
  root_wl_attr->Write(b_vals, 2);
  auto root_unit_attr = root->CreateAttribute("global_unit", {}, dt_str);
  root_unit_attr->Write("um");

  auto reg_x =
      GDALMDArrayRegularlySpaced::Create("/", "x", dim_x, 100.0, 10.0, 0.0);
  auto reg_y =
      GDALMDArrayRegularlySpaced::Create("/", "y", dim_y, 200.0, -10.0, 0.0);
  dim_x->SetIndexingVariable(reg_x);
  dim_y->SetIndexingVariable(reg_y);

  auto ar_3d = root->CreateMDArray("img3d", {dim_b, dim_y, dim_x}, dt_f64);
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

  std::unique_ptr<GDALDataset> classic_ds(
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
  EXPECT_TRUE(has_val);
  EXPECT_DOUBLE_EQ(2.0, b1->GetOffset(&has_val));
  EXPECT_TRUE(has_val);
  EXPECT_DOUBLE_EQ(3.0, b1->GetScale(&has_val));
  EXPECT_TRUE(has_val);
  EXPECT_STREQ("0", b1->GetMetadataItem("DIM_band_INDEX"));
  EXPECT_NE(nullptr, b1->GetMetadataItem("WL"));
  EXPECT_NE(nullptr, b1->GetMetadataItem("ATTR_WL"));
  EXPECT_NE(nullptr, b1->GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY"));

  // Write and read blocks and IRasterIO (including resampled IRasterIO) on
  // GDALRasterBandFromArray.
  std::vector<double> block_data(12, 42.0);
  EXPECT_EQ(CE_None, b1->WriteBlock(0, 0, block_data.data()));
  std::vector<double> read_data(12, 0.0);
  EXPECT_EQ(CE_None, b1->ReadBlock(0, 0, read_data.data()));
  EXPECT_DOUBLE_EQ(42.0, read_data[0]);

  double resampled_buf[4] = {0};
  EXPECT_EQ(CE_None, b1->RasterIO(GF_Read, 0, 0, 4, 3, resampled_buf, 2, 2,
                                  GDT_Float64, 0, 0, nullptr));
  EXPECT_DOUBLE_EQ(42.0, resampled_buf[0]);

  // Also test Int64 and UInt64 nodata on GDALRasterBandFromArray, and 1D
  // AsClassicDataset, and GDALMDRasterIOFromBand.
  auto dt_i64 = GDALExtendedDataType::Create(GDT_Int64);
  auto dt_u64 = GDALExtendedDataType::Create(GDT_UInt64);
  auto ar_i64 = root->CreateMDArray("ar_i64", {dim_x}, dt_i64);
  ar_i64->SetNoDataValue(static_cast<int64_t>(-12345));
  auto ar_u64 = root->CreateMDArray("ar_u64", {dim_x}, dt_u64);
  ar_u64->SetNoDataValue(static_cast<uint64_t>(54321));

  std::unique_ptr<GDALDataset> ds_1d_i64(ar_i64->AsClassicDataset(0, 0));
  ASSERT_NE(nullptr, ds_1d_i64);
  EXPECT_EQ(-12345,
            ds_1d_i64->GetRasterBand(1)->GetNoDataValueAsInt64(&has_val));
  EXPECT_TRUE(has_val);

  std::unique_ptr<GDALDataset> ds_1d_u64(ar_u64->AsClassicDataset(0, 0));
  ASSERT_NE(nullptr, ds_1d_u64);
  EXPECT_EQ(54321u,
            ds_1d_u64->GetRasterBand(1)->GetNoDataValueAsUInt64(&has_val));
  EXPECT_TRUE(has_val);

  auto ar_with_fn = CustomTestMDArray::Create(
      "/", "with_fn", {dim_y, dim_x}, dt_f64, "/vsimem/dummy.dat", "my_ctx");
  std::unique_ptr<GDALDataset> ds_fn(
      ar_with_fn->AsClassicDataset(1, 0, nullptr, nullptr));
  ASSERT_NE(nullptr, ds_fn);

  // Test GDALMDRasterIOFromBand directly (read & write, strided & negative
  // step).
  const GUInt64 md_start[2] = {0, 0};
  const size_t md_count[2] = {3, 4};
  const GInt64 md_step[2] = {1, 1};
  const GPtrDiff_t md_stride[2] = {4, 1};
  EXPECT_TRUE(GDALMDRasterIOFromBand(b1, GF_Write, 1, 0, md_start, md_count,
                                     md_step, md_stride, dt_f64,
                                     block_data.data()));
  EXPECT_TRUE(GDALMDRasterIOFromBand(b1, GF_Read, 1, 0, md_start, md_count,
                                     md_step, md_stride, dt_f64,
                                     read_data.data()));
  const GUInt64 md_start_neg[2] = {2, 3};
  const size_t md_count_neg[2] = {2, 2};
  const GInt64 md_step_neg[2] = {-1, -1};
  const GPtrDiff_t md_stride_neg[2] = {2, 1};
  EXPECT_TRUE(GDALMDRasterIOFromBand(b1, GF_Read, 1, 0, md_start_neg,
                                     md_count_neg, md_step_neg, md_stride_neg,
                                     dt_f64, read_data.data()));

  // Error cases in AsClassicDataset BAND_METADATA / BAND_IMAGERY_METADATA.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  CPLStringList bad_opts;
  bad_opts.SetNameValue("BAND_METADATA", "not_json");
  std::unique_ptr<GDALDataset>(
      ar_3d->AsClassicDataset(2, 1, root, bad_opts.List()));
  bad_opts.SetNameValue("BAND_IMAGERY_METADATA", "not_json");
  std::unique_ptr<GDALDataset>(
      ar_3d->AsClassicDataset(2, 1, root, bad_opts.List()));
  CPLPopErrorHandler();
}

// ============================================================================
// 7. Statistics, Cache, GDALPamMultiDim / GDALPamMDArray & Full C API Tests
// ============================================================================

TEST(GdalmultidimTest, StatisticsAndPamMultiDim) {
  GDALAllRegister();
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> ds(
      mem_drv->CreateMultiDimensional("stats_ds", nullptr, nullptr));
  auto root = ds->GetRootGroup();

  auto dim_y = root->CreateDimension("y", "", "", 2);
  auto dim_x = root->CreateDimension("x", "", "", 3);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);

  // 1) ComputeStatistics on Float64 and Int32 arrays with nodata.
  auto ar_f64 = root->CreateMDArray("ar_f64", {dim_y, dim_x}, dt_f64);
  ar_f64->SetNoDataValue(-999.0);
  double vals_f64[6] = {1.0, 2.0, -999.0, 3.0, 4.0, 5.0};
  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {2, 3};
  ar_f64->Write(start2, count2, nullptr, nullptr, dt_f64, vals_f64);

  double min = 0, max = 0, mean = 0, stddev = 0;
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
  int32_t vals_i32[6] = {10, 20, 30, 40, 50, 60};
  ar_i32->Write(start2, count2, nullptr, nullptr, dt_i32, vals_i32);
  EXPECT_TRUE(ar_i32->ComputeStatistics(false, &min, &max, &mean, &stddev,
                                        &valid_count, nullptr, nullptr,
                                        nullptr));
  EXPECT_DOUBLE_EQ(10.0, min);
  EXPECT_DOUBLE_EQ(60.0, max);
  EXPECT_EQ(6u, valid_count);

  // Error cases for ComputeStatistics: progress cancellation and complex type.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  auto cancel_cb = [](double, const char*, void*) -> int { return FALSE; };
  EXPECT_FALSE(ar_f64->ComputeStatistics(false, &min, &max, &mean, &stddev,
                                         &valid_count, cancel_cb, nullptr,
                                         nullptr));
  auto ar_cf64 = root->CreateMDArray(
      "ar_cf64", {dim_x}, GDALExtendedDataType::Create(GDT_CFloat64));
  EXPECT_FALSE(ar_cf64->ComputeStatistics(false, &min, &max, &mean, &stddev,
                                          &valid_count, nullptr, nullptr,
                                          nullptr));
  CPLPopErrorHandler();

  // 2) GDALPamMultiDim and CustomPamMDArray with .aux.xml in /vsimem/.
  const char* pam_file = "/vsimem/test_multidim_pam.dat";
  const char* aux_file = "/vsimem/test_multidim_pam.dat.aux.xml";
  // Pre-seed aux.xml with an existing node so Save() merges into it.
  const char* initial_xml =
      "<PAMDataset><OtherNode>hello</OtherNode>"
      "<Array name=\"/preexisting\"><SRS dataAxisToSRSAxisMapping=\"2,1\" "
      "coordinateEpoch=\"2025.5\">EPSG:4326</SRS>"
      "<Statistics ApproxStats=\"1\" Minimum=\"1\" Maximum=\"10\" Mean=\"5\" "
      "StdDev=\"2\" ValidSampleCount=\"100\"/></Array></PAMDataset>";
  VSILFILE* fp = VSIFOpenL(aux_file, "wb");
  ASSERT_NE(nullptr, fp);
  VSIFWriteL(initial_xml, 1, strlen(initial_xml), fp);
  VSIFCloseL(fp);

  auto pam = std::make_shared<GDALPamMultiDim>(pam_file);
  auto pre_srs = pam->GetSpatialRef("/preexisting", "");
  ASSERT_NE(nullptr, pre_srs);
  EXPECT_DOUBLE_EQ(2025.5, pre_srs->GetCoordinateEpoch());
  double init_min = 0, init_max = 0, init_mean = 0, init_std = 0;
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
  double p_min = 0, p_max = 0, p_mean = 0, p_std = 0;
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
  pam = std::make_shared<GDALPamMultiDim>(pam_file);
  pam_ar = CustomPamMDArray::Create("/", "pam_arr", {dim_x}, dt_f64, pam);
  pam_ar->ClearStatistics();
  EXPECT_EQ(CE_Warning,
            pam_ar->GetStatistics(true, false, &p_min, &p_max, &p_mean, &p_std,
                                  &p_vc, nullptr, nullptr));
  EXPECT_TRUE(pam_ar->SetSpatialRef(nullptr));
  EXPECT_EQ(nullptr, pam_ar->GetSpatialRef());
  pam->ClearStatistics();

  VSIUnlink(pam_file);
  VSIUnlink(aux_file);
}

class FakeNetCDFMultiDimDataset final : public GDALDataset {
 public:
  explicit FakeNetCDFMultiDimDataset(std::shared_ptr<GDALGroup> root)
      : root_(std::move(root)) {}
  std::shared_ptr<GDALGroup> GetRootGroup() const override { return root_; }

 private:
  std::shared_ptr<GDALGroup> root_;
};

static std::map<std::string, std::shared_ptr<GDALGroup>>*
GetFakeNetCDFGroupMap() {
  static auto* m = new std::map<std::string, std::shared_ptr<GDALGroup>>();
  return m;
}

TEST(GdalmultidimTest, ArrayCacheAndGetCacheRootGroup) {
  GDALAllRegister();
  auto dim0 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "y", "", "", 2);
  auto dim1 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "x", "", "", 3);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);

  // 1) Empty filename fails to cache.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  auto ar_empty = CustomTestMDArray::Create("/", "empty_fn", {dim0, dim1},
                                            dt_f64, /*filename=*/"");
  EXPECT_FALSE(ar_empty->Cache(nullptr));
  CPLPopErrorHandler();

  // 2) Register temporary fake "netCDF" driver for caching tests.
  GDALDriver* orig_nc = GetGDALDriverManager()->GetDriverByName("netCDF");
  if (orig_nc != nullptr) {
    GetGDALDriverManager()->DeregisterDriver(orig_nc);
  }
  auto* fake_nc = new GDALDriver();
  fake_nc->SetDescription("netCDF");
  fake_nc->SetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER, "YES");
  fake_nc->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
  static bool s_nc_creation_in_progress = false;
  fake_nc->pfnCreateMultiDimensional = [](const char* pszFilename, CSLConstList,
                                          CSLConstList) -> GDALDataset* {
    if (STARTS_WITH(pszFilename, "/vsicurl/")) {
      return nullptr;
    }
    s_nc_creation_in_progress = true;
    VSILFILE* fp = VSIFOpenL(pszFilename, "wb");
    if (!fp) return nullptr;
    VSIFWriteL("NC", 1, 2, fp);
    VSIFCloseL(fp);
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    std::unique_ptr<GDALDataset> mem_ds(
        mem_drv->CreateMultiDimensional(pszFilename, nullptr, nullptr));
    auto root = mem_ds->GetRootGroup();
    (*GetFakeNetCDFGroupMap())[pszFilename] = root;
    return new FakeNetCDFMultiDimDataset(root);
  };
  fake_nc->pfnOpen = [](GDALOpenInfo* poOpenInfo) -> GDALDataset* {
    if (s_nc_creation_in_progress) return nullptr;
    auto* m = GetFakeNetCDFGroupMap();
    auto it = m->find(poOpenInfo->pszFilename);
    if (it == m->end()) return nullptr;
    return new FakeNetCDFMultiDimDataset(it->second);
  };
  GetGDALDriverManager()->RegisterDriver(fake_nc);

  // Test caching a normal /vsimem/ file with BLOCKSIZE option, re-caching
  // (which fails because array already exists in cache), and reading from
  // cache!
  const char* cache_base = "/vsimem/cache_test_ar.dat";
  const char* cache_gmac = "/vsimem/cache_test_ar.dat.gmac";
  auto ar_cached = CustomTestMDArray::Create("/", "my_var", {dim0, dim1},
                                             dt_f64, cache_base, "ctx1");
  double* raw_ptr = reinterpret_cast<double*>(ar_cached->RawData().data());
  for (int i = 0; i < 6; ++i) raw_ptr[i] = (i + 1) * 10.0;

  CPLStringList cache_opts;
  cache_opts.SetNameValue("BLOCKSIZE", "1,2");
  EXPECT_TRUE(ar_cached->Cache(cache_opts.List()));
  s_nc_creation_in_progress = false;

  // Re-caching fails because array already exists in cache.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(ar_cached->Cache(nullptr));
  CPLPopErrorHandler();

  // Fresh array reading through the cache.
  auto ar_cached_read = CustomTestMDArray::Create("/", "my_var", {dim0, dim1},
                                                  dt_f64, cache_base, "ctx1");
  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {2, 3};
  double read_out[6] = {0};
  EXPECT_TRUE(
      ar_cached_read->Read(start2, count2, nullptr, nullptr, dt_f64, read_out));
  EXPECT_THAT(read_out,
              testing::ElementsAre(10.0, 20.0, 30.0, 40.0, 50.0, 60.0));

  // Test /vsicurl/http...?query with GDAL_PAM_PROXY_DIR fallback!
  PamCleanProxyDB();
  VSIMkdir("/vsimem/pam_proxy", 0755);
  CPLSetConfigOption("GDAL_PAM_PROXY_DIR", "/vsimem/pam_proxy");
  auto ar_curl = CustomTestMDArray::Create(
      "/", "curl_var", {dim0, dim1}, dt_f64,
      "/vsicurl/http://example.com/data.nc?token=123");
  EXPECT_TRUE(ar_curl->Cache(nullptr));
  s_nc_creation_in_progress = false;
  PamCleanProxyDB();
  CPLSetConfigOption("GDAL_PAM_PROXY_DIR", nullptr);

  VSIUnlink(cache_gmac);
  GetFakeNetCDFGroupMap()->clear();
  GetGDALDriverManager()->DeregisterDriver(fake_nc);
  delete fake_nc;
  if (orig_nc != nullptr) {
    GetGDALDriverManager()->RegisterDriver(orig_nc);
  }
}

TEST(GdalmultidimTest, FullCApiCoverage) {
  GDALAllRegister();
  GDALDriverH h_drv = GDALGetDriverByName("MEM");
  ASSERT_NE(nullptr, h_drv);

  GDALDatasetH h_ds =
      GDALCreateMultiDimensional(h_drv, "c_api_ds", nullptr, nullptr);
  ASSERT_NE(nullptr, h_ds);
  GDALGroupH h_root = GDALDatasetGetRootGroup(h_ds);
  ASSERT_NE(nullptr, h_root);

  EXPECT_STREQ("/", GDALGroupGetName(h_root));
  EXPECT_STREQ("/", GDALGroupGetFullName(h_root));
  EXPECT_EQ(nullptr, GDALGroupGetStructuralInfo(h_root));

  // Dimensions C API.
  GDALDimensionH h_dim_y = GDALGroupCreateDimension(h_root, "y", "HORIZONTAL_Y",
                                                    "NORTH", 3, nullptr);
  GDALDimensionH h_dim_x =
      GDALGroupCreateDimension(h_root, "x", "HORIZONTAL_X", "EAST", 4, nullptr);
  ASSERT_NE(nullptr, h_dim_y);
  ASSERT_NE(nullptr, h_dim_x);
  EXPECT_STREQ("x", GDALDimensionGetName(h_dim_x));
  EXPECT_STREQ("/x", GDALDimensionGetFullName(h_dim_x));
  EXPECT_STREQ("HORIZONTAL_X", GDALDimensionGetType(h_dim_x));
  EXPECT_STREQ("EAST", GDALDimensionGetDirection(h_dim_x));
  EXPECT_EQ(4u, GDALDimensionGetSize(h_dim_x));

  size_t n_dims = 0;
  GDALDimensionH* pah_dims = GDALGroupGetDimensions(h_root, &n_dims, nullptr);
  EXPECT_EQ(2u, n_dims);
  GDALReleaseDimensions(pah_dims, n_dims);
  GDALReleaseDimensions(nullptr, 0);

  // Subgroups C API.
  GDALGroupH h_sub = GDALGroupCreateGroup(h_root, "sub", nullptr);
  ASSERT_NE(nullptr, h_sub);
  char** grp_names = GDALGroupGetGroupNames(h_root, nullptr);
  EXPECT_EQ(1, CSLCount(grp_names));
  CSLDestroy(grp_names);
  GDALGroupH h_sub_open = GDALGroupOpenGroup(h_root, "sub", nullptr);
  ASSERT_NE(nullptr, h_sub_open);
  GDALGroupRelease(h_sub_open);
  GDALGroupH h_sub_full =
      GDALGroupOpenGroupFromFullname(h_root, "/sub", nullptr);
  ASSERT_NE(nullptr, h_sub_full);
  GDALGroupRelease(h_sub_full);
  EXPECT_TRUE(GDALGroupRename(h_sub, "sub_ren"));
  GDALGroupRelease(h_sub);
  EXPECT_TRUE(GDALGroupDeleteGroup(h_root, "sub_ren", nullptr));

  // Attributes on Group C API.
  GDALExtendedDataTypeH h_dt_f64 = GDALExtendedDataTypeCreate(GDT_Float64);
  GDALExtendedDataTypeH h_dt_str = GDALExtendedDataTypeCreateString(0);
  GDALAttributeH h_gattr =
      GDALGroupCreateAttribute(h_root, "g_attr", 0, nullptr, h_dt_str, nullptr);
  ASSERT_NE(nullptr, h_gattr);
  EXPECT_STREQ("g_attr", GDALAttributeGetName(h_gattr));
  EXPECT_STREQ("/_GLOBAL_/g_attr", GDALAttributeGetFullName(h_gattr));
  EXPECT_EQ(1u, GDALAttributeGetTotalElementsCount(h_gattr));
  EXPECT_EQ(0u, GDALAttributeGetDimensionCount(h_gattr));
  size_t attr_dim_cnt = 99;
  EXPECT_EQ(nullptr, GDALAttributeGetDimensionsSize(h_gattr, &attr_dim_cnt));
  EXPECT_EQ(0u, attr_dim_cnt);
  GDALExtendedDataTypeH h_attr_dt = GDALAttributeGetDataType(h_gattr);
  GDALExtendedDataTypeRelease(h_attr_dt);

  EXPECT_TRUE(GDALAttributeWriteString(h_gattr, "hello_c"));
  EXPECT_STREQ("hello_c", GDALAttributeReadAsString(h_gattr));
  size_t raw_sz = 0;
  GByte* raw_bytes = GDALAttributeReadAsRaw(h_gattr, &raw_sz);
  ASSERT_NE(nullptr, raw_bytes);
  GDALAttributeFreeRawResult(h_gattr, raw_bytes, raw_sz);
  GDALAttributeFreeRawResult(h_gattr, nullptr, 0);
  EXPECT_TRUE(GDALAttributeRename(h_gattr, "g_attr_ren"));
  GDALAttributeRelease(h_gattr);

  h_gattr = GDALGroupGetAttribute(h_root, "g_attr_ren");
  ASSERT_NE(nullptr, h_gattr);
  GDALAttributeRelease(h_gattr);
  size_t n_attrs = 0;
  GDALAttributeH* pah_attrs = GDALGroupGetAttributes(h_root, &n_attrs, nullptr);
  EXPECT_EQ(1u, n_attrs);
  GDALReleaseAttributes(pah_attrs, n_attrs);
  GDALReleaseAttributes(nullptr, 0);
  EXPECT_TRUE(GDALGroupDeleteAttribute(h_root, "g_attr_ren", nullptr));

  // Numeric & 1D array attributes C API.
  const GUInt64 dim_len2[1] = {2};
  GDALAttributeH h_nattr = GDALGroupCreateAttribute(
      h_root, "n_attr", 1, dim_len2, h_dt_f64, nullptr);
  ASSERT_NE(nullptr, h_nattr);
  GUInt64* pan_attr_dims =
      GDALAttributeGetDimensionsSize(h_nattr, &attr_dim_cnt);
  EXPECT_EQ(1u, attr_dim_cnt);
  CPLFree(pan_attr_dims);

  const int i_arr[2] = {11, 22};
  EXPECT_TRUE(GDALAttributeWriteIntArray(h_nattr, i_arr, 2));
  size_t out_cnt = 0;
  int* out_i_arr = GDALAttributeReadAsIntArray(h_nattr, &out_cnt);
  EXPECT_EQ(2u, out_cnt);
  CPLFree(out_i_arr);

  const int64_t i64_arr[2] = {111, 222};
  EXPECT_TRUE(GDALAttributeWriteInt64Array(h_nattr, i64_arr, 2));
  int64_t* out_i64_arr = GDALAttributeReadAsInt64Array(h_nattr, &out_cnt);
  EXPECT_EQ(2u, out_cnt);
  CPLFree(out_i64_arr);

  const double d_arr[2] = {1.5, 2.5};
  EXPECT_TRUE(GDALAttributeWriteDoubleArray(h_nattr, d_arr, 2));
  double* out_d_arr = GDALAttributeReadAsDoubleArray(h_nattr, &out_cnt);
  EXPECT_EQ(2u, out_cnt);
  CPLFree(out_d_arr);

  EXPECT_TRUE(GDALAttributeWriteRaw(h_nattr, d_arr, sizeof(d_arr)));
  EXPECT_EQ(2, GDALAttributeReadAsInt(h_nattr));
  EXPECT_EQ(2, GDALAttributeReadAsInt64(h_nattr));
  EXPECT_DOUBLE_EQ(1.5, GDALAttributeReadAsDouble(h_nattr));
  GDALAttributeRelease(h_nattr);

  GDALAttributeH h_sattr_1d = GDALGroupCreateAttribute(
      h_root, "s_attr1d", 1, dim_len2, h_dt_str, nullptr);
  const char* const psz_vals[3] = {"s1", "s2", nullptr};
  EXPECT_TRUE(GDALAttributeWriteStringArray(h_sattr_1d, psz_vals));
  char** out_s_arr = GDALAttributeReadAsStringArray(h_sattr_1d);
  EXPECT_EQ(2, CSLCount(out_s_arr));
  CSLDestroy(out_s_arr);
  GDALAttributeRelease(h_sattr_1d);

  GDALAttributeH h_scalar_num =
      GDALGroupCreateAttribute(h_root, "sc_num", 0, nullptr, h_dt_f64, nullptr);
  EXPECT_TRUE(GDALAttributeWriteInt(h_scalar_num, 5));
  EXPECT_TRUE(GDALAttributeWriteInt64(h_scalar_num, 6));
  EXPECT_TRUE(GDALAttributeWriteDouble(h_scalar_num, 7.5));
  GDALAttributeRelease(h_scalar_num);

  // MDArray C API.
  GDALDimensionH dims_1d[1] = {h_dim_x};
  GDALMDArrayH h_ar_x =
      GDALGroupCreateMDArray(h_root, "x", 1, dims_1d, h_dt_f64, nullptr);
  ASSERT_NE(nullptr, h_ar_x);
  const double x_coords[4] = {10.0, 20.0, 30.0, 40.0};
  const GUInt64 start1[1] = {0};
  const size_t count1[1] = {4};
  EXPECT_TRUE(GDALMDArrayWrite(h_ar_x, start1, count1, nullptr, nullptr,
                               h_dt_f64, x_coords, nullptr, 0));
  EXPECT_TRUE(GDALDimensionSetIndexingVariable(h_dim_x, h_ar_x));
  GDALMDArrayH h_idx_var = GDALDimensionGetIndexingVariable(h_dim_x);
  ASSERT_NE(nullptr, h_idx_var);
  GDALMDArrayRelease(h_idx_var);

  GDALDimensionH dims_1d_y[1] = {h_dim_y};
  GDALMDArrayH h_ar_y =
      GDALGroupCreateMDArray(h_root, "y", 1, dims_1d_y, h_dt_f64, nullptr);
  const double y_coords[3] = {1.0, 2.0, 3.0};
  const size_t count1_y[1] = {3};
  EXPECT_TRUE(GDALMDArrayWrite(h_ar_y, start1, count1_y, nullptr, nullptr,
                               h_dt_f64, y_coords, nullptr, 0));
  EXPECT_TRUE(GDALDimensionSetIndexingVariable(h_dim_y, h_ar_y));

  GDALDimensionH dims_2d[2] = {h_dim_y, h_dim_x};
  GDALMDArrayH h_ar =
      GDALGroupCreateMDArray(h_root, "data", 2, dims_2d, h_dt_f64, nullptr);
  ASSERT_NE(nullptr, h_ar);
  EXPECT_STREQ("data", GDALMDArrayGetName(h_ar));
  EXPECT_STREQ("/data", GDALMDArrayGetFullName(h_ar));
  EXPECT_EQ(12u, GDALMDArrayGetTotalElementsCount(h_ar));
  EXPECT_EQ(2u, GDALMDArrayGetDimensionCount(h_ar));
  size_t ar_dim_cnt = 0;
  GDALDimensionH* pah_ar_dims = GDALMDArrayGetDimensions(h_ar, &ar_dim_cnt);
  EXPECT_EQ(2u, ar_dim_cnt);
  GDALReleaseDimensions(pah_ar_dims, ar_dim_cnt);
  GDALExtendedDataTypeH h_ar_dt = GDALMDArrayGetDataType(h_ar);
  GDALExtendedDataTypeRelease(h_ar_dt);
  EXPECT_EQ(nullptr, GDALMDArrayGetStructuralInfo(h_ar));

  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {3, 4};
  double vals_2d[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  EXPECT_TRUE(GDALMDArrayWrite(h_ar, start2, count2, nullptr, nullptr, h_dt_f64,
                               vals_2d, nullptr, 0));
  double read_2d[12] = {0};
  EXPECT_TRUE(GDALMDArrayRead(h_ar, start2, count2, nullptr, nullptr, h_dt_f64,
                              read_2d, nullptr, 0));
  EXPECT_TRUE(GDALMDArrayAdviseRead(h_ar, start2, count2));
  EXPECT_TRUE(GDALMDArrayAdviseReadEx(h_ar, start2, count2, nullptr));

  // Attributes on MDArray C API.
  GDALAttributeH h_aattr =
      GDALMDArrayCreateAttribute(h_ar, "a_attr", 0, nullptr, h_dt_f64, nullptr);
  ASSERT_NE(nullptr, h_aattr);
  GDALAttributeRelease(h_aattr);
  h_aattr = GDALMDArrayGetAttribute(h_ar, "a_attr");
  ASSERT_NE(nullptr, h_aattr);
  GDALAttributeRelease(h_aattr);
  pah_attrs = GDALMDArrayGetAttributes(h_ar, &n_attrs, nullptr);
  EXPECT_EQ(1u, n_attrs);
  GDALReleaseAttributes(pah_attrs, n_attrs);
  EXPECT_TRUE(GDALMDArrayDeleteAttribute(h_ar, "a_attr", nullptr));

  // NoData, Scale, Offset, Unit, SpatialRef, BlockSize, ProcessingChunkSize.
  int has_val = 0;
  EXPECT_TRUE(GDALMDArraySetNoDataValueAsDouble(h_ar, -999.0));
  EXPECT_DOUBLE_EQ(-999.0, GDALMDArrayGetNoDataValueAsDouble(h_ar, &has_val));
  EXPECT_TRUE(has_val);
  EXPECT_NE(nullptr, GDALMDArrayGetRawNoDataValue(h_ar));
  EXPECT_TRUE(GDALMDArraySetRawNoDataValue(h_ar, nullptr));
  EXPECT_TRUE(GDALMDArraySetNoDataValueAsInt64(h_ar, -10));
  EXPECT_EQ(-10, GDALMDArrayGetNoDataValueAsInt64(h_ar, &has_val));
  EXPECT_TRUE(GDALMDArraySetNoDataValueAsUInt64(h_ar, 10));
  EXPECT_EQ(10u, GDALMDArrayGetNoDataValueAsUInt64(h_ar, &has_val));

  GDALDataType st_type = GDT_Unknown;
  EXPECT_TRUE(GDALMDArraySetScale(h_ar, 2.0));
  EXPECT_DOUBLE_EQ(2.0, GDALMDArrayGetScale(h_ar, &has_val));
  EXPECT_TRUE(GDALMDArraySetScaleEx(h_ar, 3.0, GDT_Float32));
  EXPECT_DOUBLE_EQ(3.0, GDALMDArrayGetScaleEx(h_ar, &has_val, &st_type));
  EXPECT_EQ(GDT_Float32, st_type);

  EXPECT_TRUE(GDALMDArraySetOffset(h_ar, 1.0));
  EXPECT_DOUBLE_EQ(1.0, GDALMDArrayGetOffset(h_ar, &has_val));
  EXPECT_TRUE(GDALMDArraySetOffsetEx(h_ar, 4.0, GDT_Float32));
  EXPECT_DOUBLE_EQ(4.0, GDALMDArrayGetOffsetEx(h_ar, &has_val, &st_type));

  EXPECT_TRUE(GDALMDArraySetUnit(h_ar, "m"));
  EXPECT_STREQ("m", GDALMDArrayGetUnit(h_ar));

  OGRSpatialReferenceH h_srs = OSRNewSpatialReference(nullptr);
  OSRImportFromEPSG(h_srs, 4326);
  EXPECT_TRUE(GDALMDArraySetSpatialRef(h_ar, h_srs));
  OGRSpatialReferenceH h_srs_out = GDALMDArrayGetSpatialRef(h_ar);
  ASSERT_NE(nullptr, h_srs_out);
  OSRDestroySpatialReference(h_srs_out);
  EXPECT_TRUE(GDALMDArraySetSpatialRef(h_ar, nullptr));
  OSRDestroySpatialReference(h_srs);

  size_t bs_cnt = 0;
  GUInt64* pan_bs = GDALMDArrayGetBlockSize(h_ar, &bs_cnt);
  EXPECT_EQ(2u, bs_cnt);
  CPLFree(pan_bs);

  size_t pcs_cnt = 0;
  size_t* pan_pcs = GDALMDArrayGetProcessingChunkSize(h_ar, &pcs_cnt, 1024);
  EXPECT_EQ(2u, pcs_cnt);
  CPLFree(pan_pcs);

  // Views, Transpose, Unscaled, Mask, Resampled, Gridded, MeshGrid,
  // CoordinateVariables, AsClassicDataset C API.
  GDALMDArrayH h_view = GDALMDArrayGetView(h_ar, "[0,:]");
  ASSERT_NE(nullptr, h_view);
  GDALMDArrayRelease(h_view);

  const int trans_map[2] = {1, 0};
  GDALMDArrayH h_trans = GDALMDArrayTranspose(h_ar, 2, trans_map);
  ASSERT_NE(nullptr, h_trans);
  GDALMDArrayRelease(h_trans);

  GDALMDArrayH h_unscaled = GDALMDArrayGetUnscaled(h_ar);
  ASSERT_NE(nullptr, h_unscaled);
  GDALMDArrayRelease(h_unscaled);

  GDALMDArrayH h_mask = GDALMDArrayGetMask(h_ar, nullptr);
  ASSERT_NE(nullptr, h_mask);
  GDALMDArrayRelease(h_mask);

  const GDALDimensionH res_dims[2] = {nullptr, nullptr};
  GDALMDArrayH h_res = GDALMDArrayGetResampled(
      h_ar, 2, res_dims, GRIORA_NearestNeighbour, nullptr, nullptr);
  ASSERT_NE(nullptr, h_res);
  GDALMDArrayRelease(h_res);

  // GDALMDArrayGetGridded on 1D scattered data.
  GDALMDArrayH h_gridded =
      GDALMDArrayGetGridded(h_ar_y, "invdist", h_ar_y, h_ar_y, nullptr);
  ASSERT_NE(nullptr, h_gridded);
  GDALMDArrayRelease(h_gridded);

  // GDALMDArrayGetMeshGrid.
  const GDALMDArrayH mg_in[2] = {h_ar_x, h_ar_y};
  size_t mg_out_cnt = 0;
  GDALMDArrayH* pah_mg = GDALMDArrayGetMeshGrid(mg_in, 2, &mg_out_cnt, nullptr);
  ASSERT_NE(nullptr, pah_mg);
  EXPECT_EQ(2u, mg_out_cnt);
  GDALReleaseArrays(pah_mg, mg_out_cnt);
  GDALReleaseArrays(nullptr, 0);

  // GDALMDArrayGetCoordinateVariables.
  size_t cv_cnt = 0;
  GDALMDArrayH* pah_cv = GDALMDArrayGetCoordinateVariables(h_ar, &cv_cnt);
  GDALReleaseArrays(pah_cv, cv_cnt);

  // Statistics C API.
  double s_min = 0, s_max = 0, s_mean = 0, s_std = 0;
  GUInt64 s_vc = 0;
  EXPECT_TRUE(GDALMDArrayComputeStatistics(h_ar, h_ds, FALSE, &s_min, &s_max,
                                           &s_mean, &s_std, &s_vc, nullptr,
                                           nullptr));
  EXPECT_TRUE(GDALMDArrayComputeStatisticsEx(h_ar, h_ds, FALSE, &s_min, &s_max,
                                             &s_mean, &s_std, &s_vc, nullptr,
                                             nullptr, nullptr));
  EXPECT_EQ(CE_None,
            GDALMDArrayGetStatistics(h_ar, h_ds, FALSE, TRUE, &s_min, &s_max,
                                     &s_mean, &s_std, &s_vc, nullptr, nullptr));
  GDALDatasetClearStatistics(h_ds);

  // Classic dataset conversions C API.
  GDALDatasetH h_classic1 = GDALMDArrayAsClassicDataset(h_ar, 1, 0);
  ASSERT_NE(nullptr, h_classic1);
  GDALMDArrayH h_from_ds = GDALDatasetAsMDArray(h_classic1, nullptr);
  ASSERT_NE(nullptr, h_from_ds);
  GDALMDArrayRelease(h_from_ds);
  GDALMDArrayH h_from_band =
      GDALRasterBandAsMDArray(GDALGetRasterBand(h_classic1, 1));
  ASSERT_NE(nullptr, h_from_band);
  GDALMDArrayRelease(h_from_band);
  GDALClose(h_classic1);

  GDALDatasetH h_classic2 =
      GDALMDArrayAsClassicDatasetEx(h_ar, 1, 0, h_root, nullptr);
  ASSERT_NE(nullptr, h_classic2);
  GDALClose(h_classic2);

  // Group array open / resolve / subset / resize / rename / delete C API.
  char** arr_names = GDALGroupGetMDArrayNames(h_root, nullptr);
  EXPECT_GE(CSLCount(arr_names), 3);
  CSLDestroy(arr_names);
  char** full_arr_names =
      GDALGroupGetMDArrayFullNamesRecursive(h_root, nullptr, nullptr);
  EXPECT_GE(CSLCount(full_arr_names), 3);
  CSLDestroy(full_arr_names);

  GDALMDArrayH h_open_ar = GDALGroupOpenMDArray(h_root, "data", nullptr);
  ASSERT_NE(nullptr, h_open_ar);
  GDALMDArrayRelease(h_open_ar);
  GDALMDArrayH h_open_full =
      GDALGroupOpenMDArrayFromFullname(h_root, "/data", nullptr);
  ASSERT_NE(nullptr, h_open_full);
  GDALMDArrayRelease(h_open_full);
  GDALMDArrayH h_res_ar = GDALGroupResolveMDArray(h_root, "data", "/", nullptr);
  ASSERT_NE(nullptr, h_res_ar);
  GDALMDArrayRelease(h_res_ar);

  GDALGroupH h_subset_grp =
      GDALGroupSubsetDimensionFromSelection(h_root, "/x=20", nullptr);
  ASSERT_NE(nullptr, h_subset_grp);
  GDALGroupRelease(h_subset_grp);

  const GUInt64 new_sizes[2] = {4, 5};
  EXPECT_TRUE(GDALMDArrayResize(h_ar, new_sizes, nullptr));
  EXPECT_TRUE(GDALMDArrayRename(h_ar, "data_ren"));
  EXPECT_TRUE(GDALDimensionRename(h_dim_y, "y_ren"));

  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_FALSE(GDALMDArrayCache(h_ar, nullptr));
  CPLPopErrorHandler();

  GDALMDArrayRelease(h_ar);
  GDALMDArrayRelease(h_ar_y);
  GDALMDArrayRelease(h_ar_x);
  GDALMDArrayRelease(nullptr);
  EXPECT_TRUE(GDALGroupDeleteMDArray(h_root, "data_ren", nullptr));
  EXPECT_TRUE(GDALDimensionSetIndexingVariable(h_dim_x, nullptr));
  EXPECT_TRUE(GDALDimensionSetIndexingVariable(h_dim_y, nullptr));
  EXPECT_TRUE(GDALGroupDeleteMDArray(h_root, "x", nullptr));
  EXPECT_TRUE(GDALGroupDeleteMDArray(h_root, "y", nullptr));
  EXPECT_TRUE(GDALGroupDeleteAttribute(h_root, "n_attr", nullptr));
  EXPECT_TRUE(GDALGroupDeleteAttribute(h_root, "s_attr1d", nullptr));
  EXPECT_TRUE(GDALGroupDeleteAttribute(h_root, "sc_num", nullptr));

  GDALDimensionRelease(h_dim_x);
  GDALDimensionRelease(h_dim_y);
  GDALDimensionRelease(nullptr);
  GDALExtendedDataTypeRelease(h_dt_str);
  GDALExtendedDataTypeRelease(h_dt_f64);
  GDALGroupRelease(h_root);
  GDALGroupRelease(nullptr);
  GDALClose(h_ds);
}

TEST(GdalmultidimTest, DuplicateDimNamesCopyFrom) {
  auto drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> src_ds(
      drv->CreateMultiDimensional("src", nullptr, nullptr));
  auto src_root = src_ds->GetRootGroup();
  auto dim_a1 = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "shared_dim", "", "", 3);
  auto dim_a2 = std::make_shared<GDALDimensionWeakIndexingVar>(
      "/", "shared_dim", "", "", 5);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto ar1 = src_root->CreateMDArray("ar1", {dim_a1}, dt_f64);
  auto ar2 = src_root->CreateMDArray("ar2", {dim_a2}, dt_f64);

  std::unique_ptr<GDALDataset> dst_ds(
      drv->CreateMultiDimensional("dst", nullptr, nullptr));
  auto dst_root = dst_ds->GetRootGroup();
  GUInt64 cost = 0;
  EXPECT_TRUE(dst_root->CopyFrom(dst_root, src_ds.get(), src_root, true, cost,
                                 src_root->GetTotalCopyCost(), nullptr,
                                 nullptr));
  EXPECT_NE(nullptr, dst_root->OpenMDArray("ar1"));
  EXPECT_NE(nullptr, dst_root->OpenMDArray("ar2"));
}

TEST(GdalmultidimTest, CopyToFinalBuffer3DTypeConversion) {
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_f32 = GDALExtendedDataType::Create(GDT_Float32);
  auto d0 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d0", "", "", 2);
  auto d1 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d1", "", "", 2);
  auto d2 =
      std::make_shared<GDALDimensionWeakIndexingVar>("/", "d2", "", "", 2);
  auto ar3d = CustomTestMDArray::Create("/", "ar3d", {d0, d1, d2}, dt_f64);
  double* p = reinterpret_cast<double*>(ar3d->RawData().data());
  for (int i = 0; i < 8; ++i) p[i] = i + 1;
  const GUInt64 start[3] = {0, 0, 0};
  const size_t count[3] = {2, 2, 2};
  const GInt64 step[3] = {1, 1, 1};
  // Non-contiguous buffer stride to exercise CopyToFinalBuffer stack recursion:
  const GPtrDiff_t stride[3] = {8, 4, 1};
  std::vector<float> buf(20, 0.0f);
  EXPECT_TRUE(ar3d->Read(start, count, step, stride, dt_f32, buf.data()));
  EXPECT_FLOAT_EQ(1.0f, buf[0]);
  EXPECT_FLOAT_EQ(2.0f, buf[1]);
}

TEST(GdalmultidimTest, CopyFrom0DAndStringArray) {
  auto drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional("mem", nullptr, nullptr));
  auto root = ds->GetRootGroup();
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_str = GDALExtendedDataType::CreateString();
  auto s0 = root->CreateMDArray("s0", {}, dt_f64);
  double val = 42.0;
  s0->Write(nullptr, nullptr, nullptr, nullptr, dt_f64, &val);
  auto d0 = root->CreateMDArray("d0", {}, dt_f64);
  GUInt64 cost = 0;
  EXPECT_TRUE(d0->CopyFrom(nullptr, s0.get(), true, cost,
                           s0->GetTotalCopyCost(), nullptr, nullptr));
  double read_val = 0;
  d0->Read(nullptr, nullptr, nullptr, nullptr, dt_f64, &read_val);
  EXPECT_DOUBLE_EQ(42.0, read_val);

  auto dim2 = root->CreateDimension("dim2", "", "", 2);
  auto str_src = root->CreateMDArray("str_src", {dim2}, dt_str);
  const char* str_vals[2] = {"hello", "world"};
  const GUInt64 start[1] = {0};
  const size_t count[1] = {2};
  str_src->Write(start, count, nullptr, nullptr, dt_str, str_vals);
  auto str_dst = root->CreateMDArray("str_dst", {dim2}, dt_str);
  EXPECT_TRUE(str_dst->CopyFrom(nullptr, str_src.get(), true, cost,
                                str_src->GetTotalCopyCost(), nullptr, nullptr));
}

TEST(GdalmultidimTest, AsClassicDatasetSubsetAndUnit) {
  auto drv = GetGDALDriverManager()->GetDriverByName("MEM");
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional("mem", nullptr, nullptr));
  auto root = ds->GetRootGroup();
  auto dim_sub = root->CreateDimension("subset_band_0_1_2", "", "", 2);
  auto dim_y = root->CreateDimension("y", "", "", 2);
  auto dim_x = root->CreateDimension("x", "", "", 2);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto iv = root->CreateMDArray("iv", {dim_sub}, dt_f64);
  iv->SetUnit("m");
  double iv_vals[2] = {10.0, 11.0};
  const GUInt64 s0[1] = {0};
  const size_t c2[1] = {2};
  iv->Write(s0, c2, nullptr, nullptr, dt_f64, iv_vals);
  dim_sub->SetIndexingVariable(iv);

  auto ar = root->CreateMDArray("sub_img", {dim_sub, dim_y, dim_x}, dt_f64);
  std::unique_ptr<GDALDataset> cds(ar->AsClassicDataset(2, 1, root, nullptr));
  ASSERT_NE(nullptr, cds);
  auto b1 = cds->GetRasterBand(1);
  ASSERT_NE(nullptr, b1);
  EXPECT_STREQ("0", b1->GetMetadataItem("DIM_band_INDEX"));
  EXPECT_STREQ("10", b1->GetMetadataItem("DIM_band_VALUE"));
  EXPECT_STREQ("m", b1->GetMetadataItem("DIM_band_UNIT"));
}

}  // namespace
