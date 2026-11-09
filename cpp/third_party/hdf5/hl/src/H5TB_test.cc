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
// Tests HDF5 High-Level Table APIs (third_party/hdf5/hl/src/H5TB.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5TB.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_table.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5_test_util.h"
#include "third_party/hdf5/hl/src/H5TBpublic.h"
#include "third_party/hdf5/hl/src/hdf5_hl.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::StrEq;

struct Particle {
  char name[16];
  int32_t lati;
  int32_t longi;
  float pressure;
  double temperature;
};

constexpr hsize_t kParticleNFields = 5;
constexpr std::array<const char*, kParticleNFields> kParticleFieldNames = {
    "Name", "Latitude", "Longitude", "Pressure", "Temperature"};
constexpr std::array<size_t, kParticleNFields> kParticleOffsets = {
    offsetof(Particle, name), offsetof(Particle, lati),
    offsetof(Particle, longi), offsetof(Particle, pressure),
    offsetof(Particle, temperature)};
constexpr std::array<size_t, kParticleNFields> kParticleSizes = {
    sizeof(Particle::name), sizeof(Particle::lati), sizeof(Particle::longi),
    sizeof(Particle::pressure), sizeof(Particle::temperature)};

ScopedDatatype MakeFixedStringType(size_t size) {
  ScopedDatatype tid(H5Tcopy(H5T_C_S1));
  if (!tid.valid() || H5Tset_size(tid, size) < 0) {
    return ScopedDatatype();
  }
  return tid;
}

std::array<hid_t, kParticleNFields> GetParticleFieldTypes(hid_t str_tid) {
  return {str_tid, H5T_NATIVE_INT32, H5T_NATIVE_INT32, H5T_NATIVE_FLOAT,
          H5T_NATIVE_DOUBLE};
}

Particle MakeParticle(const char* name, int32_t lati, int32_t longi,
                      float pressure, double temperature) {
  Particle p = {};
  std::strncpy(p.name, name, sizeof(p.name) - 1);
  p.lati = lati;
  p.longi = longi;
  p.pressure = pressure;
  p.temperature = temperature;
  return p;
}

void ExpectParticleEq(const Particle& actual, const Particle& expected) {
  EXPECT_THAT(actual.name, StrEq(expected.name));
  EXPECT_EQ(actual.lati, expected.lati);
  EXPECT_EQ(actual.longi, expected.longi);
  EXPECT_FLOAT_EQ(actual.pressure, expected.pressure);
  EXPECT_DOUBLE_EQ(actual.temperature, expected.temperature);
}

struct CompactRecord {
  int32_t a;
  int32_t b;
  int32_t c;
};

constexpr hsize_t kCompactNFields = 3;
constexpr std::array<const char*, kCompactNFields> kCompactFieldNames = {
    "a", "b", "c"};
constexpr std::array<size_t, kCompactNFields> kCompactOffsets = {
    offsetof(CompactRecord, a), offsetof(CompactRecord, b),
    offsetof(CompactRecord, c)};
constexpr std::array<size_t, kCompactNFields> kCompactSizes = {
    sizeof(CompactRecord::a), sizeof(CompactRecord::b),
    sizeof(CompactRecord::c)};
std::array<hid_t, kCompactNFields> GetCompactFieldTypes() {
  return {H5T_NATIVE_INT32, H5T_NATIVE_INT32, H5T_NATIVE_INT32};
}

TEST(H5tbTest, MakeAppendWriteReadAndMetadataQueries) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);
  ScopedDatatype str_tid = MakeFixedStringType(16);
  ASSERT_TRUE(str_tid.valid());
  const auto field_types = GetParticleFieldTypes(str_tid);
  auto field_names = kParticleFieldNames;

  // 1. Create uncompressed table with fill data and initial records.
  Particle fill = MakeParticle("none", -1, -2, 0.5f, 1.25);
  const std::vector<Particle> initial = {
      MakeParticle("alpha", 10, 20, 1.5f, 100.0),
      MakeParticle("beta", 30, 40, 2.5f, 200.0),
      MakeParticle("gamma", 50, 60, 3.5f, 300.0),
  };

  ASSERT_GE(
      H5TBmake_table("Particles", file, "particles", kParticleNFields,
                     initial.size(), sizeof(Particle), field_names.data(),
                     kParticleOffsets.data(), field_types.data(),
                     /*chunk_size=*/2, &fill, /*compress=*/0, initial.data()),
      0);

  // 2. Also test compressed table created initially empty (buf=nullptr, no
  // fill).
  ASSERT_GE(H5TBmake_table("Compressed", file, "compressed", kParticleNFields,
                           /*nrecords=*/0, sizeof(Particle), field_names.data(),
                           kParticleOffsets.data(), field_types.data(),
                           /*chunk_size=*/2, /*fill_data=*/nullptr,
                           /*compress=*/1, /*buf=*/nullptr),
            0);

  // 3. Append and overwrite records on both tables.
  const std::vector<Particle> extra = {
      MakeParticle("delta", 70, 80, 4.5f, 400.0),
      MakeParticle("epsilon", 90, 100, 5.5f, 500.0),
  };
  ASSERT_GE(H5TBappend_records(file, "particles", extra.size(),
                               sizeof(Particle), kParticleOffsets.data(),
                               kParticleSizes.data(), extra.data()),
            0);
  ASSERT_GE(H5TBappend_records(file, "compressed", extra.size(),
                               sizeof(Particle), kParticleOffsets.data(),
                               kParticleSizes.data(), extra.data()),
            0);

  const Particle replacement = MakeParticle("beta2", 31, 41, 2.75f, 201.5);
  ASSERT_GE(H5TBwrite_records(file, "particles", /*start=*/1, /*nrecords=*/1,
                              sizeof(Particle), kParticleOffsets.data(),
                              kParticleSizes.data(), &replacement),
            0);

  // 4. Query table info with all combinations of optional outputs.
  hsize_t nfields = 0;
  hsize_t nrecords = 0;
  ASSERT_GE(H5TBget_table_info(file, "particles", &nfields, &nrecords), 0);
  EXPECT_EQ(nfields, kParticleNFields);
  EXPECT_EQ(nrecords, 5u);
  nfields = 0;
  nrecords = 0;
  ASSERT_GE(H5TBget_table_info(file, "particles", &nfields, nullptr), 0);
  EXPECT_EQ(nfields, kParticleNFields);
  ASSERT_GE(H5TBget_table_info(file, "particles", nullptr, &nrecords), 0);
  EXPECT_EQ(nrecords, 5u);

  // 5. Query field info with all outputs non-NULL and then all nullptr.
  std::array<std::array<char, 64>, kParticleNFields> name_bufs = {};
  std::array<char*, kParticleNFields> name_ptrs = {};
  for (size_t i = 0; i < kParticleNFields; ++i) {
    name_ptrs[i] = name_bufs[i].data();
  }
  std::array<size_t, kParticleNFields> q_sizes = {};
  std::array<size_t, kParticleNFields> q_offsets = {};
  size_t q_type_size = 0;
  ASSERT_GE(H5TBget_field_info(file, "particles", name_ptrs.data(),
                               q_sizes.data(), q_offsets.data(), &q_type_size),
            0);
  EXPECT_EQ(q_type_size, sizeof(Particle));
  for (size_t i = 0; i < kParticleNFields; ++i) {
    EXPECT_THAT(name_ptrs[i], StrEq(field_names[i]));
    EXPECT_EQ(q_sizes[i], kParticleSizes[i]);
    EXPECT_EQ(q_offsets[i], kParticleOffsets[i]);
  }
  ASSERT_GE(
      H5TBget_field_info(file, "particles", nullptr, nullptr, nullptr, nullptr),
      0);

  // 6. Query TITLE and FIELD_i_FILL attributes directly.
  {
    ScopedDataset dset(H5Dopen2(file, "particles", H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);
    std::array<char, 64> title = {};
    ASSERT_GE(H5TBAget_title(dset, title.data()), 0);
    EXPECT_THAT(title.data(), StrEq("Particles"));

    Particle fill_out = {};
    EXPECT_EQ(1, H5TBAget_fill(file, "particles", dset,
                               reinterpret_cast<unsigned char*>(&fill_out)));
    ExpectParticleEq(fill_out, fill);

    ScopedDataset no_fill_dset(H5Dopen2(file, "compressed", H5P_DEFAULT));
    ASSERT_GE(no_fill_dset.get(), 0);
    EXPECT_EQ(0, H5TBAget_fill(file, "compressed", no_fill_dset,
                               reinterpret_cast<unsigned char*>(&fill_out)));
  }

  // 7. Read back whole table and slices.
  std::vector<Particle> full(5);
  ASSERT_GE(H5TBread_table(file, "particles", sizeof(Particle),
                           kParticleOffsets.data(), kParticleSizes.data(),
                           full.data()),
            0);
  ExpectParticleEq(full[0], initial[0]);
  ExpectParticleEq(full[1], replacement);
  ExpectParticleEq(full[2], initial[2]);
  ExpectParticleEq(full[3], extra[0]);
  ExpectParticleEq(full[4], extra[1]);

  std::vector<Particle> slice(2);
  ASSERT_GE(H5TBread_records(file, "particles", /*start=*/2, /*nrecords=*/2,
                             sizeof(Particle), kParticleOffsets.data(),
                             kParticleSizes.data(), slice.data()),
            0);
  ExpectParticleEq(slice[0], initial[2]);
  ExpectParticleEq(slice[1], extra[0]);

  std::vector<Particle> comp_out(2);
  ASSERT_GE(H5TBread_table(file, "compressed", sizeof(Particle),
                           kParticleOffsets.data(), kParticleSizes.data(),
                           comp_out.data()),
            0);
  ExpectParticleEq(comp_out[0], extra[0]);
  ExpectParticleEq(comp_out[1], extra[1]);
}

TEST(H5tbTest, ReadAndWriteFieldsByNameAndIndex) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);
  ScopedDatatype str_tid = MakeFixedStringType(16);
  ASSERT_TRUE(str_tid.valid());
  const auto field_types = GetParticleFieldTypes(str_tid);
  auto field_names = kParticleFieldNames;

  // 1. Create source table with particle records.
  const std::vector<Particle> initial = {
      MakeParticle("r0", 1, 2, 1.0f, 10.0),
      MakeParticle("r1", 3, 4, 2.0f, 20.0),
      MakeParticle("r2", 5, 6, 3.0f, 30.0),
  };
  ASSERT_GE(H5TBmake_table("Fields", file, "tbl", kParticleNFields,
                           initial.size(), sizeof(Particle), field_names.data(),
                           kParticleOffsets.data(), field_types.data(), 2,
                           nullptr, 0, initial.data()),
            0);

  // 2. Single-field write/read by name with field_offset == nullptr.
  constexpr std::array<int32_t, 2> kNewLat = {101, 202};
  constexpr size_t kI32Size = sizeof(int32_t);
  ASSERT_GE(H5TBwrite_fields_name(file, "tbl", "Latitude", /*start=*/1,
                                  /*nrecords=*/2, kI32Size, nullptr, &kI32Size,
                                  kNewLat.data()),
            0);
  std::array<int32_t, 2> out_lat = {};
  ASSERT_GE(H5TBread_fields_name(file, "tbl", "Latitude", /*start=*/1,
                                 /*nrecords=*/2, kI32Size, nullptr, &kI32Size,
                                 out_lat.data()),
            0);
  EXPECT_EQ(out_lat[0], 101);
  EXPECT_EQ(out_lat[1], 202);

  // 3. Fixed-length string field with different in-memory size (triggers
  // H5Tset_size branch).
  struct ShortName {
    char s[8];
  };
  constexpr std::array<ShortName, 2> kShortNames = {{{"n0"}, {"n1"}}};
  constexpr size_t kShortSz = sizeof(ShortName::s);
  ASSERT_GE(H5TBwrite_fields_name(file, "tbl", "Name", 0, 2, sizeof(ShortName),
                                  nullptr, &kShortSz, kShortNames.data()),
            0);
  std::array<ShortName, 2> read_short_names = {};
  ASSERT_GE(H5TBread_fields_name(file, "tbl", "Name", 0, 2, sizeof(ShortName),
                                 nullptr, &kShortSz, read_short_names.data()),
            0);
  EXPECT_THAT(read_short_names[0].s, StrEq("n0"));
  EXPECT_THAT(read_short_names[1].s, StrEq("n1"));

  // 4. Multi-field write/read by comma-separated names.
  struct PairView {
    int32_t lon;
    double temp;
  };
  constexpr std::array<size_t, 2> kPairOffsets = {offsetof(PairView, lon),
                                                  offsetof(PairView, temp)};
  constexpr std::array<size_t, 2> kPairSizes = {sizeof(PairView::lon),
                                                sizeof(PairView::temp)};
  constexpr std::array<PairView, 2> kNewPairs = {{{77, 77.5}, {88, 88.5}}};
  ASSERT_GE(H5TBwrite_fields_name(file, "tbl", "Longitude,Temperature", 0, 2,
                                  sizeof(PairView), kPairOffsets.data(),
                                  kPairSizes.data(), kNewPairs.data()),
            0);
  std::array<PairView, 2> out_pairs = {};
  ASSERT_GE(H5TBread_fields_name(file, "tbl", "Longitude,Temperature", 0, 2,
                                 sizeof(PairView), kPairOffsets.data(),
                                 kPairSizes.data(), out_pairs.data()),
            0);
  EXPECT_EQ(out_pairs[0].lon, 77);
  EXPECT_DOUBLE_EQ(out_pairs[0].temp, 77.5);
  EXPECT_EQ(out_pairs[1].lon, 88);
  EXPECT_DOUBLE_EQ(out_pairs[1].temp, 88.5);

  // 5. Single-field write/read by index with field_offset == nullptr and
  // differing string buffer size.
  constexpr std::array<ShortName, 1> kIdxName = {{{"z2"}}};
  constexpr int kNameIdx = 0;
  ASSERT_GE(
      H5TBwrite_fields_index(file, "tbl", 1, &kNameIdx, 2, 1, sizeof(ShortName),
                             nullptr, &kShortSz, kIdxName.data()),
      0);
  std::array<ShortName, 1> out_idx_name = {};
  ASSERT_GE(
      H5TBread_fields_index(file, "tbl", 1, &kNameIdx, 2, 1, sizeof(ShortName),
                            nullptr, &kShortSz, out_idx_name.data()),
      0);
  EXPECT_THAT(out_idx_name[0].s, StrEq("z2"));

  // 6. Multi-field write/read by index in non-sequential order {2, 4}.
  constexpr std::array<int, 2> kIdxs = {2, 4};
  constexpr std::array<PairView, 2> kIdxPairs = {{{99, 99.25}, {111, 111.75}}};
  ASSERT_GE(H5TBwrite_fields_index(file, "tbl", 2, kIdxs.data(), 1, 2,
                                   sizeof(PairView), kPairOffsets.data(),
                                   kPairSizes.data(), kIdxPairs.data()),
            0);
  std::array<PairView, 2> out_idx_pairs = {};
  ASSERT_GE(H5TBread_fields_index(file, "tbl", 2, kIdxs.data(), 1, 2,
                                  sizeof(PairView), kPairOffsets.data(),
                                  kPairSizes.data(), out_idx_pairs.data()),
            0);
  EXPECT_EQ(out_idx_pairs[0].lon, 99);
  EXPECT_DOUBLE_EQ(out_idx_pairs[0].temp, 99.25);
  EXPECT_EQ(out_idx_pairs[1].lon, 111);
  EXPECT_DOUBLE_EQ(out_idx_pairs[1].temp, 111.75);
}

TEST(H5tbTest, InsertDeleteRecordsAddFromAndCombineTables) {
  ScopedFile file1 = CreateCoreFile();
  ScopedFile file2 = CreateCoreFile();
  ASSERT_GE(file1.get(), 0);
  ASSERT_GE(file2.get(), 0);

  auto compact_names = kCompactFieldNames;
  const auto compact_types = GetCompactFieldTypes();

  // 1. Create source tables in two files (with and without fill values).
  CompactRecord fill = {0, 0, 0};
  const std::vector<CompactRecord> r1 = {
      {1, 10, 100}, {2, 20, 200}, {3, 30, 300}, {4, 40, 400}};
  const std::vector<CompactRecord> r2 = {{5, 50, 500}, {6, 60, 600}};
  ASSERT_GE(H5TBmake_table("T1", file1, "t1", kCompactNFields, r1.size(),
                           sizeof(CompactRecord), compact_names.data(),
                           kCompactOffsets.data(), compact_types.data(), 2,
                           &fill, 0, r1.data()),
            0);
  ASSERT_GE(H5TBmake_table("T2", file2, "t2", kCompactNFields, r2.size(),
                           sizeof(CompactRecord), compact_names.data(),
                           kCompactOffsets.data(), compact_types.data(), 2,
                           nullptr, 0, r2.data()),
            0);
  ASSERT_GE(H5TBmake_table("T1NoFill", file1, "t1_nofill", kCompactNFields,
                           r2.size(), sizeof(CompactRecord),
                           compact_names.data(), kCompactOffsets.data(),
                           compact_types.data(), 2, nullptr, 0, r2.data()),
            0);

  // 2. Insert in middle of t1.
  CompactRecord ins = {99, 990, 9900};
  ASSERT_GE(H5TBinsert_record(file1, "t1", /*start=*/1, /*nrecords=*/1,
                              sizeof(CompactRecord), kCompactOffsets.data(),
                              kCompactSizes.data(), &ins),
            0);
  hsize_t nrecords = 0;
  ASSERT_GE(H5TBget_table_info(file1, "t1", nullptr, &nrecords), 0);
  EXPECT_EQ(nrecords, 5u);

  // 3. Delete middle record, end record (read_nrecords == 0), then verify.
  ASSERT_GE(H5TBdelete_record(file1, "t1", /*start=*/1, /*nrecords=*/1), 0);
  ASSERT_GE(H5TBdelete_record(file1, "t1", /*start=*/3, /*nrecords=*/1), 0);
  ASSERT_GE(H5TBget_table_info(file1, "t1", nullptr, &nrecords), 0);
  EXPECT_EQ(nrecords, 3u);

  // 4. Add records from t1_nofill to t1 at start2=1.
  ASSERT_GE(H5TBadd_records_from(file1, "t1_nofill", /*start1=*/0,
                                 /*nrecords=*/2, "t1", /*start2=*/1),
            0);
  ASSERT_GE(H5TBget_table_info(file1, "t1", nullptr, &nrecords), 0);
  EXPECT_EQ(nrecords, 5u);
  std::vector<CompactRecord> t1_out(nrecords);
  ASSERT_GE(
      H5TBread_table(file1, "t1", sizeof(CompactRecord), kCompactOffsets.data(),
                     kCompactSizes.data(), t1_out.data()),
      0);
  EXPECT_EQ(t1_out[0].a, 1);
  EXPECT_EQ(t1_out[1].a, 5);
  EXPECT_EQ(t1_out[2].a, 6);
  EXPECT_EQ(t1_out[3].a, 2);
  EXPECT_EQ(t1_out[4].a, 3);

  // 5. Combine tables across two separate in-memory files (with fill values).
  ASSERT_GE(H5TBcombine_tables(file1, "t1", file2, "t2", "combined_cross"), 0);
  ASSERT_GE(H5TBget_table_info(file1, "combined_cross", nullptr, &nrecords), 0);
  EXPECT_EQ(nrecords, 7u);
  {
    ScopedDataset dset(H5Dopen2(file1, "combined_cross", H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);
    CompactRecord fill_out = {-1, -1, -1};
    EXPECT_EQ(1, H5TBAget_fill(file1, "combined_cross", dset,
                               reinterpret_cast<unsigned char*>(&fill_out)));
    EXPECT_EQ(0, fill_out.a);
    EXPECT_EQ(0, fill_out.b);
    EXPECT_EQ(0, fill_out.c);
  }

  // 6. Combine tables in the same file without fill attributes.
  ASSERT_GE(
      H5TBcombine_tables(file1, "t1_nofill", file1, "t1_nofill", "comb_same"),
      0);
  ASSERT_GE(H5TBget_table_info(file1, "comb_same", nullptr, &nrecords), 0);
  EXPECT_EQ(nrecords, 4u);

  // 7. Delete all records from comb_same.
  ASSERT_GE(H5TBdelete_record(file1, "comb_same", 0, 4), 0);
  ASSERT_GE(H5TBget_table_info(file1, "comb_same", nullptr, &nrecords), 0);
  EXPECT_EQ(nrecords, 0u);
}

TEST(H5tbTest, InsertAndDeleteFieldsWithAndWithoutFill) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  auto compact_names = kCompactFieldNames;
  const auto compact_types = GetCompactFieldTypes();

  CompactRecord fill = {7, 8, 9};
  const std::vector<CompactRecord> rows = {{1, 2, 3}, {4, 5, 6}};
  ASSERT_GE(H5TBmake_table("WithFill", file, "wf", kCompactNFields, rows.size(),
                           sizeof(CompactRecord), compact_names.data(),
                           kCompactOffsets.data(), compact_types.data(), 2,
                           &fill, 0, rows.data()),
            0);
  ASSERT_GE(H5TBmake_table("NoFill", file, "nf", kCompactNFields, rows.size(),
                           sizeof(CompactRecord), compact_names.data(),
                           kCompactOffsets.data(), compact_types.data(), 2,
                           nullptr, 0, rows.data()),
            0);

  // Insert field at position 0 with fill and data, delete middle field while
  // every field has FIELD_*_FILL so H5TBdelete_field re-attaches fill attrs,
  // then insert a trailing field with buf=nullptr and fill=nullptr.
  constexpr std::array<int32_t, 2> kDVals = {100, 200};
  constexpr int32_t kDFill = 42;
  ASSERT_GE(H5TBinsert_field(file, "wf", "d", H5T_NATIVE_INT32, /*position=*/0,
                             &kDFill, kDVals.data()),
            0);
  hsize_t nfields = 0;
  ASSERT_GE(H5TBget_table_info(file, "wf", &nfields, nullptr), 0);
  EXPECT_EQ(nfields, 4u);

  ASSERT_GE(H5TBdelete_field(file, "wf", "b"), 0);
  ASSERT_GE(H5TBget_table_info(file, "wf", &nfields, nullptr), 0);
  EXPECT_EQ(nfields, 3u);
  {
    ScopedDataset wf_dset(H5Dopen2(file, "wf", H5P_DEFAULT));
    ASSERT_GE(wf_dset.get(), 0);
    std::array<int32_t, 3> wf_fill_out = {};
    EXPECT_EQ(
        1, H5TBAget_fill(file, "wf", wf_dset,
                         reinterpret_cast<unsigned char*>(wf_fill_out.data())));
  }

  ASSERT_GE(H5TBinsert_field(file, "wf", "e", H5T_NATIVE_INT32, /*position=*/3,
                             nullptr, nullptr),
            0);
  ASSERT_GE(H5TBget_table_info(file, "wf", &nfields, nullptr), 0);
  EXPECT_EQ(nfields, 4u);

  // Delete field on table without fill values.
  ASSERT_GE(H5TBdelete_field(file, "nf", "b"), 0);
  ASSERT_GE(H5TBget_table_info(file, "nf", &nfields, nullptr), 0);
  EXPECT_EQ(nfields, 2u);
  struct TwoInts {
    int32_t a;
    int32_t c;
  };
  constexpr std::array<size_t, 2> kTwoOffsets = {offsetof(TwoInts, a),
                                                 offsetof(TwoInts, c)};
  constexpr std::array<size_t, 2> kTwoSizes = {sizeof(TwoInts::a),
                                               sizeof(TwoInts::c)};
  std::array<TwoInts, 2> nf_out = {};
  ASSERT_GE(H5TBread_table(file, "nf", sizeof(TwoInts), kTwoOffsets.data(),
                           kTwoSizes.data(), nf_out.data()),
            0);
  EXPECT_EQ(nf_out[0].a, 1);
  EXPECT_EQ(nf_out[0].c, 3);
  EXPECT_EQ(nf_out[1].a, 4);
  EXPECT_EQ(nf_out[1].c, 6);
}

TEST(H5tbTest, ParameterValidationAndFailureBranches) {
  // Cleanup order matters: sweep leaked H5TBwrite_fields_name datatypes only
  // after local ScopedDatatype/ScopedFile handles in this test have closed.
  const absl::Cleanup sweep_leaked_types = [] {
    SweepOpenH5Ids(std::array{H5I_DATATYPE});
  };
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);
  ScopedDatatype str_tid = MakeFixedStringType(8);
  ASSERT_TRUE(str_tid.valid());

  auto compact_names = kCompactFieldNames;
  const auto compact_types = GetCompactFieldTypes();

  const std::vector<CompactRecord> rows = {{1, 2, 3}, {4, 5, 6}};
  ASSERT_GE(H5TBmake_table("Valid", file, "valid", kCompactNFields, rows.size(),
                           sizeof(CompactRecord), compact_names.data(),
                           kCompactOffsets.data(), compact_types.data(), 2,
                           nullptr, 0, rows.data()),
            0);

  // Create a contiguous non-compound scalar dataset to trigger type/chunk
  // failures cleanly.
  constexpr hsize_t kDDim = 2;
  ScopedDataspace d_space(H5Screate_simple(1, &kDDim, nullptr));
  ASSERT_GE(d_space.get(), 0);
  ScopedDataset non_comp(H5Dcreate2(file, "non_comp", H5T_NATIVE_INT32, d_space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_GE(non_comp.get(), 0);

  // Create a chunked compound table with 0-sized / unchunked neighbor to hit
  // delete/insert chunk check.
  ScopedDatatype small_comp(H5Tcreate(H5T_COMPOUND, sizeof(CompactRecord)));
  ASSERT_GE(small_comp.get(), 0);
  ASSERT_GE(H5Tinsert(small_comp, "a", 0, H5T_NATIVE_INT32), 0);
  ScopedDataset unchunked_comp(H5Dcreate2(file, "unchunked", small_comp,
                                          d_space, H5P_DEFAULT, H5P_DEFAULT,
                                          H5P_DEFAULT));
  ASSERT_GE(unchunked_comp.get(), 0);
  ASSERT_GE(H5LTset_attribute_string(file, "unchunked", "TITLE", "Unchunked"),
            0);

  // Chunked compound dataset lacking TITLE attribute hits H5TBAget_title
  // failures in H5TBinsert_field and H5TBdelete_field.
  ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(chunk_dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(chunk_dcpl, 1, &kDDim), 0);
  ScopedDataset no_title(H5Dcreate2(file, "no_title", small_comp, d_space,
                                    H5P_DEFAULT, chunk_dcpl, H5P_DEFAULT));
  ASSERT_GE(no_title.get(), 0);

  ScopedErrorSilence silence;

  std::array<CompactRecord, 2> out = {};
  hsize_t nf = 0;
  hsize_t nr = 0;
  std::array<char, 32> title = {};
  unsigned char fill_buf[sizeof(CompactRecord)] = {};
  constexpr int kBadIdx = -1;
  constexpr int kOobIdx = 99;
  constexpr size_t kBadIntSize = sizeof(int64_t);
  constexpr std::array<size_t, kCompactNFields> kBadSizes = {sizeof(int64_t), 4,
                                                             4};

  // Null argument checks.
  EXPECT_LT(H5TBmake_table(nullptr, file, "x", 1, 0, sizeof(int32_t),
                           compact_names.data(), kCompactOffsets.data(),
                           compact_types.data(), 1, nullptr, 0, nullptr),
            0);
  EXPECT_LT(H5TBmake_table("T", file, nullptr, 1, 0, sizeof(int32_t),
                           compact_names.data(), kCompactOffsets.data(),
                           compact_types.data(), 1, nullptr, 0, nullptr),
            0);
  EXPECT_LT(H5TBmake_table("T", file, "x", 1, 0, sizeof(int32_t), nullptr,
                           kCompactOffsets.data(), compact_types.data(), 1,
                           nullptr, 0, nullptr),
            0);
  EXPECT_LT(H5TBmake_table("T", file, "bad_chunk", 1, 1, sizeof(int32_t),
                           compact_names.data(), kCompactOffsets.data(),
                           compact_types.data(), /*chunk_size=*/0, nullptr, 0,
                           nullptr),
            0);
  EXPECT_LT(H5TBmake_table("T", file, "bad_ins", 1, 0, /*type_size=*/1,
                           compact_names.data(), kCompactOffsets.data(),
                           compact_types.data(), 1, nullptr, 0, nullptr),
            0);

  EXPECT_LT(H5TBappend_records(file, nullptr, 1, sizeof(CompactRecord),
                               kCompactOffsets.data(), kCompactSizes.data(),
                               rows.data()),
            0);
  EXPECT_LT(H5TBwrite_records(file, nullptr, 0, 1, sizeof(CompactRecord),
                              kCompactOffsets.data(), kCompactSizes.data(),
                              rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_name(file, nullptr, "a", 0, 1, 4, nullptr,
                                  kCompactSizes.data(), rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_name(file, "valid", nullptr, 0, 1, 4, nullptr,
                                  kCompactSizes.data(), rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_index(file, nullptr, 1, &kBadIdx, 0, 1, 4, nullptr,
                                   kCompactSizes.data(), rows.data()),
            0);
  EXPECT_LT(
      H5TBread_table(file, nullptr, sizeof(CompactRecord),
                     kCompactOffsets.data(), kCompactSizes.data(), out.data()),
      0);
  EXPECT_LT(H5TBread_records(file, nullptr, 0, 1, sizeof(CompactRecord),
                             kCompactOffsets.data(), kCompactSizes.data(),
                             out.data()),
            0);
  EXPECT_LT(H5TBread_fields_name(file, nullptr, "a", 0, 1, 4, nullptr,
                                 kCompactSizes.data(), out.data()),
            0);
  EXPECT_LT(H5TBread_fields_name(file, "valid", nullptr, 0, 1, 4, nullptr,
                                 kCompactSizes.data(), out.data()),
            0);
  EXPECT_LT(H5TBread_fields_index(file, nullptr, 1, &kBadIdx, 0, 1, 4, nullptr,
                                  kCompactSizes.data(), out.data()),
            0);
  EXPECT_LT(H5TBget_table_info(file, nullptr, &nf, &nr), 0);
  EXPECT_LT(
      H5TBget_field_info(file, nullptr, nullptr, nullptr, nullptr, nullptr), 0);
  EXPECT_LT(H5TBdelete_record(file, nullptr, 0, 1), 0);
  EXPECT_LT(H5TBinsert_record(file, nullptr, 0, 1, sizeof(CompactRecord),
                              kCompactOffsets.data(), kCompactSizes.data(),
                              out.data()),
            0);
  EXPECT_LT(H5TBadd_records_from(file, nullptr, 0, 1, "valid", 0), 0);
  EXPECT_LT(H5TBadd_records_from(file, "valid", 0, 1, nullptr, 0), 0);
  EXPECT_LT(H5TBcombine_tables(file, nullptr, file, "valid", "c"), 0);
  EXPECT_LT(H5TBcombine_tables(file, "valid", file, nullptr, "c"), 0);
  EXPECT_LT(H5TBcombine_tables(file, "valid", file, "valid", nullptr), 0);
  EXPECT_LT(H5TBinsert_field(file, nullptr, "x", H5T_NATIVE_INT32, 0, nullptr,
                             nullptr),
            0);
  EXPECT_LT(H5TBinsert_field(file, "valid", nullptr, H5T_NATIVE_INT32, 0,
                             nullptr, nullptr),
            0);
  EXPECT_LT(H5TBdelete_field(file, nullptr, "a"), 0);
  EXPECT_LT(H5TBdelete_field(file, "valid", nullptr), 0);
  EXPECT_LT(H5TBAget_fill(file, nullptr, unchunked_comp, fill_buf), 0);

  // Missing dataset / non-compound / out-of-bounds / bad size checks.
  EXPECT_LT(H5TBget_table_info(file, "missing", &nf, &nr), 0);
  EXPECT_LT(H5TBget_table_info(file, "non_comp", &nf, &nr), 0);
  EXPECT_LT(
      H5TBget_field_info(file, "missing", nullptr, nullptr, nullptr, nullptr),
      0);
  EXPECT_LT(
      H5TBget_field_info(file, "non_comp", nullptr, nullptr, nullptr, nullptr),
      0);
  EXPECT_LT(H5TBAget_title(non_comp, title.data()), 0);
  EXPECT_LT(H5TBAget_fill(file, "missing", unchunked_comp, fill_buf), 0);
  EXPECT_LT(H5TBAget_fill(file, "valid", H5I_INVALID_HID, fill_buf), 0);

  EXPECT_LT(
      H5TBappend_records(file, "valid", 1, sizeof(CompactRecord),
                         kCompactOffsets.data(), kBadSizes.data(), rows.data()),
      0);
  EXPECT_LT(H5TBappend_records(H5I_INVALID_HID, "valid", 1,
                               sizeof(CompactRecord), kCompactOffsets.data(),
                               kCompactSizes.data(), rows.data()),
            0);
  EXPECT_LT(H5TBwrite_records(file, "valid", 10, 2, sizeof(CompactRecord),
                              kCompactOffsets.data(), kCompactSizes.data(),
                              rows.data()),
            0);
  EXPECT_LT(
      H5TBwrite_records(file, "valid", 0, 1, sizeof(CompactRecord),
                        kCompactOffsets.data(), kBadSizes.data(), rows.data()),
      0);
  // Upstream H5TB.c out: does not close member_type_id/nmtype_id on early
  // validation failures in H5TBwrite_fields_name/index; sweep_leaked_types
  // cleans them up after local handles close.
  constexpr size_t kZeroSize = 0;
  EXPECT_LT(H5TBwrite_fields_name(file, "valid", "a", 0, 1, sizeof(int32_t),
                                  nullptr, &kBadIntSize, rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_name(file, "valid", "a", 0, 1, sizeof(int32_t),
                                  nullptr, &kZeroSize, rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_name(file, "valid", "nonexistent", 10, 2, 4,
                                  nullptr, kCompactSizes.data(), rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_name(file, "non_comp", "a", 0, 1, 4, nullptr,
                                  kCompactSizes.data(), rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_index(file, "valid", 1, &kBadIdx, 0, 1, 4, nullptr,
                                   kCompactSizes.data(), rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_index(file, "valid", 1, &kOobIdx, 0, 1, 4, nullptr,
                                   kCompactSizes.data(), rows.data()),
            0);
  constexpr int kZeroIdx = 0;
  EXPECT_LT(
      H5TBwrite_fields_index(file, "valid", 1, &kZeroIdx, 0, 1, sizeof(int32_t),
                             nullptr, &kBadIntSize, rows.data()),
      0);
  EXPECT_LT(
      H5TBwrite_fields_index(file, "valid", 1, &kZeroIdx, 0, 1, sizeof(int32_t),
                             nullptr, &kZeroSize, rows.data()),
      0);

  EXPECT_LT(
      H5TBread_table(file, "missing", sizeof(CompactRecord),
                     kCompactOffsets.data(), kCompactSizes.data(), out.data()),
      0);
  EXPECT_LT(
      H5TBread_table(file, "valid", sizeof(CompactRecord),
                     kCompactOffsets.data(), kBadSizes.data(), out.data()),
      0);
  EXPECT_LT(H5TBread_records(file, "valid", 5, 2, sizeof(CompactRecord),
                             kCompactOffsets.data(), kCompactSizes.data(),
                             out.data()),
            0);
  EXPECT_LT(
      H5TBread_records(file, "valid", 0, 1, sizeof(CompactRecord),
                       kCompactOffsets.data(), kBadSizes.data(), out.data()),
      0);
  EXPECT_LT(H5TBread_fields_name(file, "valid", "unknown_field", 0, 1, 4,
                                 nullptr, kCompactSizes.data(), out.data()),
            0);
  EXPECT_LT(H5TBread_fields_name(file, "valid", "a", 0, 1, sizeof(int32_t),
                                 nullptr, &kBadIntSize, out.data()),
            0);
  EXPECT_LT(H5TBread_fields_name(file, "valid", "a", 0, 1, sizeof(int32_t),
                                 nullptr, &kZeroSize, out.data()),
            0);
  EXPECT_LT(H5TBread_fields_name(file, "non_comp", "a", 0, 1, 4, nullptr,
                                 kCompactSizes.data(), out.data()),
            0);
  EXPECT_LT(H5TBread_fields_index(file, "valid", 1, &kBadIdx, 0, 1, 4, nullptr,
                                  kCompactSizes.data(), out.data()),
            0);
  EXPECT_LT(H5TBread_fields_index(file, "valid", 1, &kOobIdx, 0, 1, 4, nullptr,
                                  kCompactSizes.data(), out.data()),
            0);
  EXPECT_LT(
      H5TBread_fields_index(file, "valid", 1, &kZeroIdx, 0, 1, sizeof(int32_t),
                            nullptr, &kBadIntSize, out.data()),
      0);
  EXPECT_LT(
      H5TBread_fields_index(file, "valid", 1, &kZeroIdx, 0, 1, sizeof(int32_t),
                            nullptr, &kZeroSize, out.data()),
      0);
  EXPECT_LT(H5TBread_fields_index(file, "valid", 1, &kZeroIdx, 10, 2, 4,
                                  nullptr, kCompactSizes.data(), out.data()),
            0);

  EXPECT_LT(H5TBdelete_record(file, "missing", 0, 1), 0);
  EXPECT_LT(H5TBdelete_record(file, "unchunked", 0, 1), 0);
  // Out-of-bounds start wraps uint64_t read_nrecords in H5TB.c; passes only
  // because allocator_may_return_null=1 turns the huge calloc(~2^64) into NULL.
  EXPECT_LT(H5TBdelete_record(file, "valid", 10, 1), 0);
  EXPECT_LT(H5TBinsert_record(file, "missing", 0, 1, sizeof(CompactRecord),
                              kCompactOffsets.data(), kCompactSizes.data(),
                              out.data()),
            0);
  EXPECT_LT(H5TBinsert_record(file, "unchunked", 0, 1, sizeof(CompactRecord),
                              kCompactOffsets.data(), kCompactSizes.data(),
                              out.data()),
            0);
  EXPECT_LT(H5TBadd_records_from(file, "missing", 0, 1, "valid", 0), 0);
  EXPECT_LT(H5TBadd_records_from(file, "valid", 10, 1, "valid", 0), 0);
  EXPECT_LT(H5TBadd_records_from(file, "valid", 0, 1, "missing", 0), 0);
  EXPECT_LT(H5TBcombine_tables(file, "missing", file, "valid", "c1"), 0);
  EXPECT_LT(H5TBcombine_tables(file, "valid", file, "missing", "c2"), 0);
  EXPECT_LT(H5TBcombine_tables(file, "valid", file, "valid", "valid"), 0);

  EXPECT_LT(H5TBinsert_field(file, "missing", "x", H5T_NATIVE_INT32, 0, nullptr,
                             nullptr),
            0);
  EXPECT_LT(H5TBinsert_field(file, "valid", "x", H5I_INVALID_HID, 0, nullptr,
                             nullptr),
            0);
  EXPECT_LT(H5TBinsert_field(file, "unchunked", "x", H5T_NATIVE_INT32, 0,
                             nullptr, nullptr),
            0);
  EXPECT_LT(H5TBinsert_field(file, "no_title", "x", H5T_NATIVE_INT32, 0,
                             nullptr, nullptr),
            0);
  EXPECT_LT(H5TBdelete_field(file, "missing", "a"), 0);
  EXPECT_LT(H5TBdelete_field(file, "valid", "missing_field"), 0);
  EXPECT_LT(H5TBdelete_field(file, "unchunked", "a"), 0);
  EXPECT_LT(H5TBdelete_field(file, "no_title", "a"), 0);

  // Zero / undersized compound types, misaligned offsets, wrap-around start,
  // duplicate field names, sole-field deletion, and unsupported time type.
  EXPECT_LT(H5TBmake_table("T", file, "zero_ts", 1, 0, /*type_size=*/0,
                           compact_names.data(), kCompactOffsets.data(),
                           compact_types.data(), 1, nullptr, 0, nullptr),
            0);
  constexpr size_t kI32SizeVal = sizeof(int32_t);
  EXPECT_LT(H5TBwrite_fields_name(file, "valid", "a", 0, 1, /*type_size=*/0,
                                  nullptr, &kI32SizeVal, rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_index(file, "valid", 1, &kZeroIdx, 0, 1,
                                   /*type_size=*/0, nullptr, &kI32SizeVal,
                                   rows.data()),
            0);
  EXPECT_LT(H5TBread_fields_name(file, "valid", "a", 0, 1, /*type_size=*/0,
                                 nullptr, &kI32SizeVal, out.data()),
            0);
  EXPECT_LT(
      H5TBread_fields_index(file, "valid", 1, &kZeroIdx, 0, 1,
                            /*type_size=*/0, nullptr, &kI32SizeVal, out.data()),
      0);
  EXPECT_LT(
      H5TBread_table(file, "valid", /*type_size=*/0, kCompactOffsets.data(),
                     kCompactSizes.data(), out.data()),
      0);

  constexpr std::array<size_t, kCompactNFields> kZeroSizes = {0, 4, 4};
  EXPECT_LT(
      H5TBread_table(file, "valid", sizeof(CompactRecord),
                     kCompactOffsets.data(), kZeroSizes.data(), out.data()),
      0);

  constexpr size_t kSmallTypeSize = 2;
  EXPECT_LT(H5TBwrite_fields_name(file, "valid", "a", 0, 1, kSmallTypeSize,
                                  nullptr, &kI32SizeVal, rows.data()),
            0);
  EXPECT_LT(
      H5TBwrite_fields_index(file, "valid", 1, &kZeroIdx, 0, 1, kSmallTypeSize,
                             nullptr, &kI32SizeVal, rows.data()),
      0);
  EXPECT_LT(H5TBread_fields_name(file, "valid", "a", 0, 1, kSmallTypeSize,
                                 nullptr, &kI32SizeVal, out.data()),
            0);
  EXPECT_LT(
      H5TBread_fields_index(file, "valid", 1, &kZeroIdx, 0, 1, kSmallTypeSize,
                            nullptr, &kI32SizeVal, out.data()),
      0);
  EXPECT_LT(
      H5TBread_table(file, "valid", kSmallTypeSize, kCompactOffsets.data(),
                     kCompactSizes.data(), out.data()),
      0);

  constexpr size_t kBadOff = 2;
  EXPECT_LT(H5TBwrite_fields_name(file, "valid", "a", 0, 1, sizeof(int32_t),
                                  &kBadOff, &kI32SizeVal, rows.data()),
            0);
  EXPECT_LT(
      H5TBwrite_fields_index(file, "valid", 1, &kZeroIdx, 0, 1, sizeof(int32_t),
                             &kBadOff, &kI32SizeVal, rows.data()),
      0);
  EXPECT_LT(H5TBread_fields_name(file, "valid", "a", 0, 1, sizeof(int32_t),
                                 &kBadOff, &kI32SizeVal, out.data()),
            0);
  EXPECT_LT(
      H5TBread_fields_index(file, "valid", 1, &kZeroIdx, 0, 1, sizeof(int32_t),
                            &kBadOff, &kI32SizeVal, out.data()),
      0);

  EXPECT_LT(
      H5TBwrite_records(file, "valid", std::numeric_limits<hsize_t>::max(), 1,
                        sizeof(CompactRecord), kCompactOffsets.data(),
                        kCompactSizes.data(), rows.data()),
      0);

  EXPECT_LT(H5TBinsert_field(file, "valid", "a", H5T_NATIVE_INT32, 1, nullptr,
                             nullptr),
            0);
  EXPECT_LT(H5TBinsert_field(file, "valid", "a", H5T_NATIVE_INT32, 0, nullptr,
                             nullptr),
            0);

  const char* only_name = "only";
  constexpr size_t kOnlyOff = 0;
  const hid_t only_type = H5T_NATIVE_INT32;
  constexpr int32_t kOnlyVal = 42;
  ASSERT_GE(H5TBmake_table("One", file, "one_field", 1, 1, sizeof(int32_t),
                           &only_name, &kOnlyOff, &only_type, 1, nullptr, 0,
                           &kOnlyVal),
            0);
  EXPECT_LT(H5TBdelete_field(file, "one_field", "only"), 0);

  const char* time_name = "t";
  const hid_t time_type = H5T_UNIX_D32LE;
  constexpr int32_t kTimeVal = 7;
  ASSERT_GE(H5TBmake_table("Time", file, "time_tbl", 1, 1, sizeof(int32_t),
                           &time_name, &kOnlyOff, &time_type, 1, nullptr, 0,
                           &kTimeVal),
            0);
  // Skip H5TBget_field_info on compound-with-H5T_TIME because upstream
  // H5T__get_native_type leaks member types when a compound member is H5T_TIME.
  EXPECT_LT(H5TBwrite_fields_name(file, "time_tbl", "t", 0, 1, sizeof(int32_t),
                                  nullptr, &kI32SizeVal, &kTimeVal),
            0);
  EXPECT_LT(
      H5TBwrite_fields_index(file, "time_tbl", 1, &kZeroIdx, 0, 1,
                             sizeof(int32_t), nullptr, &kI32SizeVal, &kTimeVal),
      0);
  int32_t time_out = 0;
  EXPECT_LT(H5TBread_fields_name(file, "time_tbl", "t", 0, 1, sizeof(int32_t),
                                 nullptr, &kI32SizeVal, &time_out),
            0);
  EXPECT_LT(
      H5TBread_fields_index(file, "time_tbl", 1, &kZeroIdx, 0, 1,
                            sizeof(int32_t), nullptr, &kI32SizeVal, &time_out),
      0);

  // Pass H5S_UNLIMITED directly so dataspace creation fails cleanly in
  // H5TBmake_table and field I/O helpers without allocating huge buffers.
  EXPECT_LT(H5TBmake_table("Unlim", file, "unlim_tbl", 1, H5S_UNLIMITED,
                           sizeof(int32_t), &only_name, &kOnlyOff, &only_type,
                           1, nullptr, 0, nullptr),
            0);
  EXPECT_LT(H5TBwrite_fields_name(file, "valid", "a", 0, H5S_UNLIMITED,
                                  sizeof(int32_t), nullptr, &kI32SizeVal,
                                  rows.data()),
            0);
  EXPECT_LT(H5TBwrite_fields_index(file, "valid", 1, &kZeroIdx, 0,
                                   H5S_UNLIMITED, sizeof(int32_t), nullptr,
                                   &kI32SizeVal, rows.data()),
            0);
  EXPECT_LT(
      H5TBread_fields_name(file, "valid", "a", 0, H5S_UNLIMITED,
                           sizeof(int32_t), nullptr, &kI32SizeVal, out.data()),
      0);
  EXPECT_LT(
      H5TBread_fields_index(file, "valid", 1, &kZeroIdx, 0, H5S_UNLIMITED,
                            sizeof(int32_t), nullptr, &kI32SizeVal, out.data()),
      0);
}

// Pass-through VOL connector that injects failures and invalid-ID returns on
// HDF5 VOL operations to exercise internal H5TB.c error and cleanup paths.
struct TbVolState {
  int in_internal_vol_op = 0;
  int dset_create_calls = 0;
  int fail_dset_create_call = 0;
  int dset_open_calls = 0;
  int fail_dset_open_call = 0;
  int dset_read_calls = 0;
  int fail_dset_read_call = 0;
  int dset_write_calls = 0;
  int fail_dset_write_call = 0;
  int dset_get_type_calls = 0;
  int fail_dset_get_type_call = 0;
  int spoof_dset_get_type_call = 0;
  hid_t spoof_dset_get_type_template = H5I_INVALID_HID;
  int dset_get_space_calls = 0;
  int fail_dset_get_space_call = 0;
  int spoof_dset_get_space_call = 0;
  int dset_get_dcpl_calls = 0;
  int fail_dset_get_dcpl_call = 0;
  int spoof_dset_get_dcpl_call = 0;
  int dset_set_extent_calls = 0;
  int fail_dset_set_extent_call = 0;
  int dset_close_calls = 0;
  int fail_dset_close_call = 0;

  int attr_create_calls = 0;
  int fail_attr_create_call = 0;
  int attr_open_calls = 0;
  int fail_attr_open_call = 0;
  int attr_read_calls = 0;
  int fail_attr_read_call = 0;
  int attr_write_calls = 0;
  int fail_attr_write_call = 0;
  int attr_exists_calls = 0;
  int fail_attr_exists_call = 0;
  int attr_close_calls = 0;
  int fail_attr_close_call = 0;
  int fail_attr_close_from = 0;
  bool fail_all_attr_close = false;

  int link_delete_calls = 0;
  int fail_link_delete_call = 0;
  int link_move_calls = 0;
  int fail_link_move_call = 0;
  int object_open_calls = 0;
  int fail_object_open_call = 0;

  void Reset() { *this = TbVolState{}; }
};

TbVolState g_tb_vol_state;
std::unordered_set<void*> g_tb_direct_datasets;

void* TbVolFileCreate(const char* name, unsigned flags, hid_t fcpl_id,
                      hid_t fapl_id, hid_t dxpl_id, void** req) {
  ScopedPlist under_fapl(H5Pcopy(fapl_id));
  if (!under_fapl.valid() || H5Pset_vol(under_fapl, H5VL_NATIVE, nullptr) < 0) {
    return nullptr;
  }
  return H5VLfile_create(name, flags, fcpl_id, under_fapl, dxpl_id, req);
}

herr_t TbVolFileGet(void* file, H5VL_file_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  return H5VLfile_get(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolFileSpecific(void* obj, H5VL_file_specific_args_t* args,
                         hid_t dxpl_id, void** req) {
  return H5VLfile_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolFileOptional(void* file, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLfile_optional(file, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolFileClose(void* file, hid_t dxpl_id, void** req) {
  return H5VLfile_close(file, H5VL_NATIVE, dxpl_id, req);
}

void* TbVolAttrCreate(void* obj, const H5VL_loc_params_t* loc_params,
                      const char* attr_name, hid_t type_id, hid_t space_id,
                      hid_t acpl_id, hid_t aapl_id, hid_t dxpl_id, void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.attr_create_calls;
    if (g_tb_vol_state.fail_attr_create_call ==
        g_tb_vol_state.attr_create_calls) {
      return nullptr;
    }
  }
  return H5VLattr_create(obj, loc_params, H5VL_NATIVE, attr_name, type_id,
                         space_id, acpl_id, aapl_id, dxpl_id, req);
}

void* TbVolAttrOpen(void* obj, const H5VL_loc_params_t* loc_params,
                    const char* attr_name, hid_t aapl_id, hid_t dxpl_id,
                    void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.attr_open_calls;
    if (g_tb_vol_state.fail_attr_open_call == g_tb_vol_state.attr_open_calls) {
      return nullptr;
    }
  }
  return H5VLattr_open(obj, loc_params, H5VL_NATIVE, attr_name, aapl_id,
                       dxpl_id, req);
}

herr_t TbVolAttrRead(void* attr, hid_t mem_type_id, void* buf, hid_t dxpl_id,
                     void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.attr_read_calls;
    if (g_tb_vol_state.fail_attr_read_call == g_tb_vol_state.attr_read_calls) {
      return -1;
    }
  }
  return H5VLattr_read(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

herr_t TbVolAttrWrite(void* attr, hid_t mem_type_id, const void* buf,
                      hid_t dxpl_id, void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.attr_write_calls;
    if (g_tb_vol_state.fail_attr_write_call ==
        g_tb_vol_state.attr_write_calls) {
      return -1;
    }
  }
  return H5VLattr_write(attr, H5VL_NATIVE, mem_type_id, buf, dxpl_id, req);
}

herr_t TbVolAttrGet(void* attr, H5VL_attr_get_args_t* args, hid_t dxpl_id,
                    void** req) {
  return H5VLattr_get(attr, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolAttrSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_attr_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  if (!g_tb_vol_state.in_internal_vol_op && args->op_type == H5VL_ATTR_EXISTS) {
    ++g_tb_vol_state.attr_exists_calls;
    if (g_tb_vol_state.fail_attr_exists_call ==
        g_tb_vol_state.attr_exists_calls) {
      return -1;
    }
  }
  ++g_tb_vol_state.in_internal_vol_op;
  const herr_t status =
      H5VLattr_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
  --g_tb_vol_state.in_internal_vol_op;
  return status;
}

herr_t TbVolAttrOptional(void* attr, H5VL_optional_args_t* args, hid_t dxpl_id,
                         void** req) {
  return H5VLattr_optional(attr, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolAttrClose(void* attr, hid_t dxpl_id, void** req) {
  // H5Aclose uses H5I_dec_app_ref; keep native attr intact on injected failure
  // so subsequent cleanup or H5Iiterate sweeps close it cleanly once.
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.attr_close_calls;
    if (g_tb_vol_state.fail_all_attr_close ||
        g_tb_vol_state.fail_attr_close_call ==
            g_tb_vol_state.attr_close_calls ||
        (g_tb_vol_state.fail_attr_close_from > 0 &&
         g_tb_vol_state.attr_close_calls >=
             g_tb_vol_state.fail_attr_close_from)) {
      return -1;
    }
  }
  return H5VLattr_close(attr, H5VL_NATIVE, dxpl_id, req);
}

void* TbVolDatasetCreate(void* obj, const H5VL_loc_params_t* loc_params,
                         const char* name, hid_t lcpl_id, hid_t type_id,
                         hid_t space_id, hid_t dcpl_id, hid_t dapl_id,
                         hid_t dxpl_id, void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.dset_create_calls;
    if (g_tb_vol_state.fail_dset_create_call ==
        g_tb_vol_state.dset_create_calls) {
      return nullptr;
    }
  }
  void* dset =
      H5VLdataset_create(obj, loc_params, H5VL_NATIVE, name, lcpl_id, type_id,
                         space_id, dcpl_id, dapl_id, dxpl_id, req);
  if (dset != nullptr) {
    g_tb_direct_datasets.insert(dset);
  }
  return dset;
}

void* TbVolDatasetOpen(void* obj, const H5VL_loc_params_t* loc_params,
                       const char* name, hid_t dapl_id, hid_t dxpl_id,
                       void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.dset_open_calls;
    if (g_tb_vol_state.fail_dset_open_call == g_tb_vol_state.dset_open_calls) {
      return nullptr;
    }
  }
  void* dset = H5VLdataset_open(obj, loc_params, H5VL_NATIVE, name, dapl_id,
                                dxpl_id, req);
  if (dset != nullptr) {
    g_tb_direct_datasets.insert(dset);
  }
  return dset;
}

herr_t TbVolDatasetRead(size_t count, void* dset[], hid_t mem_type_id[],
                        hid_t mem_space_id[], hid_t file_space_id[],
                        hid_t dxpl_id, void* buf[], void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.dset_read_calls;
    if (g_tb_vol_state.fail_dset_read_call == g_tb_vol_state.dset_read_calls) {
      return -1;
    }
  }
  return H5VLdataset_read(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                          file_space_id, dxpl_id, buf, req);
}

herr_t TbVolDatasetWrite(size_t count, void* dset[], hid_t mem_type_id[],
                         hid_t mem_space_id[], hid_t file_space_id[],
                         hid_t dxpl_id, const void* buf[], void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.dset_write_calls;
    if (g_tb_vol_state.fail_dset_write_call ==
        g_tb_vol_state.dset_write_calls) {
      return -1;
    }
  }
  return H5VLdataset_write(count, dset, H5VL_NATIVE, mem_type_id, mem_space_id,
                           file_space_id, dxpl_id, buf, req);
}

herr_t TbVolDatasetGet(void* dset, H5VL_dataset_get_args_t* args, hid_t dxpl_id,
                       void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    if (args->op_type == H5VL_DATASET_GET_TYPE) {
      ++g_tb_vol_state.dset_get_type_calls;
      if (g_tb_vol_state.fail_dset_get_type_call ==
          g_tb_vol_state.dset_get_type_calls) {
        return -1;
      }
      if (g_tb_vol_state.spoof_dset_get_type_call ==
          g_tb_vol_state.dset_get_type_calls) {
        if (g_tb_vol_state.spoof_dset_get_type_template != H5I_INVALID_HID) {
          const hid_t copy_id =
              H5Tcopy(g_tb_vol_state.spoof_dset_get_type_template);
          if (copy_id < 0) {
            return -1;
          }
          args->args.get_type.type_id = copy_id;
          return 0;
        }
        // ID 1 (> 0, H5I_UNINIT) causes downstream H5T* and out: H5Tclose
        // checks to fail without allocating any HDF5 handle.
        args->args.get_type.type_id = 1;
        return 0;
      }
    } else if (args->op_type == H5VL_DATASET_GET_SPACE) {
      ++g_tb_vol_state.dset_get_space_calls;
      if (g_tb_vol_state.fail_dset_get_space_call ==
          g_tb_vol_state.dset_get_space_calls) {
        return -1;
      }
      if (g_tb_vol_state.spoof_dset_get_space_call ==
          g_tb_vol_state.dset_get_space_calls) {
        args->args.get_space.space_id = 1;
        return 0;
      }
    } else if (args->op_type == H5VL_DATASET_GET_DCPL) {
      ++g_tb_vol_state.dset_get_dcpl_calls;
      if (g_tb_vol_state.fail_dset_get_dcpl_call ==
          g_tb_vol_state.dset_get_dcpl_calls) {
        return -1;
      }
      if (g_tb_vol_state.spoof_dset_get_dcpl_call ==
          g_tb_vol_state.dset_get_dcpl_calls) {
        args->args.get_dcpl.dcpl_id = 1;
        return 0;
      }
    }
  }
  return H5VLdataset_get(dset, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolDatasetSpecific(void* obj, H5VL_dataset_specific_args_t* args,
                            hid_t dxpl_id, void** req) {
  if (!g_tb_vol_state.in_internal_vol_op &&
      args->op_type == H5VL_DATASET_SET_EXTENT) {
    ++g_tb_vol_state.dset_set_extent_calls;
    if (g_tb_vol_state.fail_dset_set_extent_call ==
        g_tb_vol_state.dset_set_extent_calls) {
      return -1;
    }
  }
  return H5VLdataset_specific(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolDatasetOptional(void* obj, H5VL_optional_args_t* args,
                            hid_t dxpl_id, void** req) {
  return H5VLdataset_optional(obj, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolDatasetClose(void* dset, hid_t dxpl_id, void** req) {
  // Only H5Dclose (via H5I_dec_app_ref_always_close) removes the ID on close
  // failure; H5Oclose (used internally by H5LTset_attribute_string on handles
  // opened via TbVolObjectOpen) uses H5I_dec_app_ref and leaves a dangling
  // H5I_DATASET entry if close fails. Close native storage first so injected
  // direct-dataset close failures never leak the underlying H5D_t.
  const bool is_direct_dset = g_tb_direct_datasets.erase(dset) > 0;
  const herr_t status = H5VLdataset_close(dset, H5VL_NATIVE, dxpl_id, req);
  if (is_direct_dset && !g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.dset_close_calls;
    if (g_tb_vol_state.fail_dset_close_call ==
        g_tb_vol_state.dset_close_calls) {
      return -1;
    }
  }
  return status;
}

herr_t TbVolLinkMove(void* src_obj, const H5VL_loc_params_t* loc_params1,
                     void* dst_obj, const H5VL_loc_params_t* loc_params2,
                     hid_t lcpl_id, hid_t lapl_id, hid_t dxpl_id, void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.link_move_calls;
    if (g_tb_vol_state.fail_link_move_call == g_tb_vol_state.link_move_calls) {
      return -1;
    }
  }
  return H5VLlink_move(src_obj, loc_params1, dst_obj, loc_params2, H5VL_NATIVE,
                       lcpl_id, lapl_id, dxpl_id, req);
}

herr_t TbVolLinkGet(void* obj, const H5VL_loc_params_t* loc_params,
                    H5VL_link_get_args_t* args, hid_t dxpl_id, void** req) {
  return H5VLlink_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolLinkSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_link_specific_args_t* args, hid_t dxpl_id,
                         void** req) {
  if (!g_tb_vol_state.in_internal_vol_op && args->op_type == H5VL_LINK_DELETE) {
    ++g_tb_vol_state.link_delete_calls;
    if (g_tb_vol_state.fail_link_delete_call ==
        g_tb_vol_state.link_delete_calls) {
      return -1;
    }
  }
  return H5VLlink_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

void* TbVolObjectOpen(void* obj, const H5VL_loc_params_t* loc_params,
                      H5I_type_t* opened_type, hid_t dxpl_id, void** req) {
  if (!g_tb_vol_state.in_internal_vol_op) {
    ++g_tb_vol_state.object_open_calls;
    if (g_tb_vol_state.fail_object_open_call ==
        g_tb_vol_state.object_open_calls) {
      return nullptr;
    }
  }
  return H5VLobject_open(obj, loc_params, H5VL_NATIVE, opened_type, dxpl_id,
                         req);
}

herr_t TbVolObjectGet(void* obj, const H5VL_loc_params_t* loc_params,
                      H5VL_object_get_args_t* args, hid_t dxpl_id, void** req) {
  return H5VLobject_get(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolObjectSpecific(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_object_specific_args_t* args, hid_t dxpl_id,
                           void** req) {
  return H5VLobject_specific(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolObjectOptional(void* obj, const H5VL_loc_params_t* loc_params,
                           H5VL_optional_args_t* args, hid_t dxpl_id,
                           void** req) {
  return H5VLobject_optional(obj, loc_params, H5VL_NATIVE, args, dxpl_id, req);
}

herr_t TbVolBlobPut(void* obj, const void* buf, size_t size, void* blob_id,
                    void* ctx) {
  return H5VLblob_put(obj, H5VL_NATIVE, buf, size, blob_id, ctx);
}

herr_t TbVolBlobGet(void* obj, const void* blob_id, void* buf, size_t size,
                    void* ctx) {
  return H5VLblob_get(obj, H5VL_NATIVE, blob_id, buf, size, ctx);
}

herr_t TbVolBlobSpecific(void* obj, void* blob_id,
                         H5VL_blob_specific_args_t* args) {
  return H5VLblob_specific(obj, H5VL_NATIVE, blob_id, args);
}

herr_t TbVolTokenCmp(void* obj, const H5O_token_t* token1,
                     const H5O_token_t* token2, int* cmp_value) {
  return H5VLtoken_cmp(obj, H5VL_NATIVE, token1, token2, cmp_value);
}

herr_t TbVolTokenToStr(void* obj, H5I_type_t obj_type, const H5O_token_t* token,
                       char** token_str) {
  return H5VLtoken_to_str(obj, obj_type, H5VL_NATIVE, token, token_str);
}

herr_t TbVolTokenFromStr(void* obj, H5I_type_t obj_type, const char* token_str,
                         H5O_token_t* token) {
  return H5VLtoken_from_str(obj, obj_type, H5VL_NATIVE, token_str, token);
}

herr_t TbVolIntrospectGetConnCls(void* obj, H5VL_get_conn_lvl_t lvl,
                                 const H5VL_class_t** conn_cls) {
  return H5VLintrospect_get_conn_cls(obj, H5VL_NATIVE, lvl, conn_cls);
}

herr_t TbVolIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  return H5VLintrospect_get_cap_flags(nullptr, H5VL_NATIVE, cap_flags);
}

herr_t TbVolIntrospectOptQuery(void* obj, H5VL_subclass_t cls, int opt_type,
                               uint64_t* flags) {
  return H5VLintrospect_opt_query(obj, H5VL_NATIVE, cls, opt_type, flags);
}

const H5VL_class_t kTbFaultVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(51006),
    .name = "h5tb_fault_vol",
    .conn_version = 1,
    .cap_flags = 0,
    .initialize = nullptr,
    .terminate = nullptr,
    .info_cls = {},
    .wrap_cls = {},
    .attr_cls =
        {
            .create = TbVolAttrCreate,
            .open = TbVolAttrOpen,
            .read = TbVolAttrRead,
            .write = TbVolAttrWrite,
            .get = TbVolAttrGet,
            .specific = TbVolAttrSpecific,
            .optional = TbVolAttrOptional,
            .close = TbVolAttrClose,
        },
    .dataset_cls =
        {
            .create = TbVolDatasetCreate,
            .open = TbVolDatasetOpen,
            .read = TbVolDatasetRead,
            .write = TbVolDatasetWrite,
            .get = TbVolDatasetGet,
            .specific = TbVolDatasetSpecific,
            .optional = TbVolDatasetOptional,
            .close = TbVolDatasetClose,
        },
    .datatype_cls = {},
    .file_cls =
        {
            .create = TbVolFileCreate,
            .open = nullptr,
            .get = TbVolFileGet,
            .specific = TbVolFileSpecific,
            .optional = TbVolFileOptional,
            .close = TbVolFileClose,
        },
    .group_cls = {},
    .link_cls =
        {
            .create = nullptr,
            .copy = nullptr,
            .move = TbVolLinkMove,
            .get = TbVolLinkGet,
            .specific = TbVolLinkSpecific,
            .optional = nullptr,
        },
    .object_cls =
        {
            .open = TbVolObjectOpen,
            .copy = nullptr,
            .get = TbVolObjectGet,
            .specific = TbVolObjectSpecific,
            .optional = TbVolObjectOptional,
        },
    .introspect_cls =
        {
            .get_conn_cls = TbVolIntrospectGetConnCls,
            .get_cap_flags = TbVolIntrospectGetCapFlags,
            .opt_query = TbVolIntrospectOptQuery,
        },
    .request_cls = {},
    .blob_cls =
        {
            .put = TbVolBlobPut,
            .get = TbVolBlobGet,
            .specific = TbVolBlobSpecific,
            .optional = nullptr,
        },
    .token_cls =
        {
            .cmp = TbVolTokenCmp,
            .to_str = TbVolTokenToStr,
            .from_str = TbVolTokenFromStr,
        },
    .optional = nullptr,
};

void CleanupTbVolConnector(hid_t vol_id) {
  g_tb_vol_state.Reset();
  SweepOpenH5Ids();
  g_tb_direct_datasets.clear();
  H5VLunregister_connector(vol_id);
}

template <typename Fn>
void SweepFault(int* counter, int* fail_target, Fn&& fn, int max_calls = 16) {
  for (int k = 1; k <= max_calls; ++k) {
    g_tb_vol_state.Reset();
    *fail_target = k;
    const herr_t rc = fn(k);
    const int calls = *counter;
    g_tb_vol_state.Reset();
    SweepOpenH5Ids(std::array{H5I_ATTR});
    if (calls < k) {
      break;
    }
    EXPECT_LT(rc, 0) << "call=" << k;
  }
  g_tb_vol_state.Reset();
  SweepOpenH5Ids(std::array{H5I_ATTR});
}

TEST(H5tbTest, VolFaultPropagation) {
  g_tb_vol_state.Reset();
  g_tb_direct_datasets.clear();
  const hid_t vol_id = H5VLregister_connector(&kTbFaultVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  const absl::Cleanup cleanup_vol = [vol_id] { CleanupTbVolConnector(vol_id); };

  ScopedFile file = CreateCoreVolFile(vol_id, "h5tb_fault_vol.h5");
  ASSERT_GE(file.get(), 0);

  auto compact_names = kCompactFieldNames;
  const auto compact_types = GetCompactFieldTypes();
  CompactRecord fill = {1, 2, 3};
  const std::vector<CompactRecord> base_rows = {
      {10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {100, 110, 120}};

  auto reset_table = [&](const char* name, const char* title) {
    const TbVolState saved = g_tb_vol_state;
    g_tb_vol_state.Reset();
    if (H5Lexists(file, name, H5P_DEFAULT) > 0) {
      H5Ldelete(file, name, H5P_DEFAULT);
    }
    EXPECT_GE(
        H5TBmake_table(title, file, name, kCompactNFields, base_rows.size(),
                       sizeof(CompactRecord), compact_names.data(),
                       kCompactOffsets.data(), compact_types.data(), 2, &fill,
                       0, base_rows.data()),
        0);
    g_tb_vol_state = saved;
  };
  auto clean_link = [&](const char* name) {
    const TbVolState saved = g_tb_vol_state;
    g_tb_vol_state.Reset();
    if (H5Lexists(file, name, H5P_DEFAULT) > 0) {
      H5Ldelete(file, name, H5P_DEFAULT);
    }
    g_tb_vol_state = saved;
  };

  // Create reusable source tables before injecting faults.
  reset_table("src1", "Src1");
  reset_table("src2", "Src2");

  ScopedErrorSilence silence;

  // Sweep fault injection across H5TBmake_table (with fill and data).
  auto run_make = [&](int /*k*/) -> herr_t {
    clean_link("fault_mk");
    return H5TBmake_table("Make", file, "fault_mk", kCompactNFields,
                          base_rows.size(), sizeof(CompactRecord),
                          compact_names.data(), kCompactOffsets.data(),
                          compact_types.data(), 2, &fill, 0, base_rows.data());
  };
  SweepFault(&g_tb_vol_state.dset_create_calls,
             &g_tb_vol_state.fail_dset_create_call, run_make);
  SweepFault(&g_tb_vol_state.dset_open_calls,
             &g_tb_vol_state.fail_dset_open_call, run_make);
  SweepFault(&g_tb_vol_state.dset_write_calls,
             &g_tb_vol_state.fail_dset_write_call, run_make);
  SweepFault(&g_tb_vol_state.dset_close_calls,
             &g_tb_vol_state.fail_dset_close_call, run_make);
  SweepFault(&g_tb_vol_state.attr_create_calls,
             &g_tb_vol_state.fail_attr_create_call, run_make);
  SweepFault(&g_tb_vol_state.attr_write_calls,
             &g_tb_vol_state.fail_attr_write_call, run_make);
  SweepFault(&g_tb_vol_state.attr_close_calls,
             &g_tb_vol_state.fail_attr_close_call, run_make);
  SweepFault(&g_tb_vol_state.object_open_calls,
             &g_tb_vol_state.fail_object_open_call, run_make);
  {
    g_tb_vol_state.Reset();
    clean_link("fault_mk");
    g_tb_vol_state.fail_all_attr_close = true;
    EXPECT_LT(
        H5TBmake_table("Make", file, "fault_mk", kCompactNFields,
                       base_rows.size(), sizeof(CompactRecord),
                       compact_names.data(), kCompactOffsets.data(),
                       compact_types.data(), 2, &fill, 0, base_rows.data()),
        0);
    g_tb_vol_state.Reset();
    SweepOpenH5Ids(std::array{H5I_ATTR});
  }
  clean_link("fault_mk");

  // Helper to sweep standard fault knobs over a read/query/modify operation.
  auto sweep_all_knobs = [&](auto&& op, int max_calls = 28) {
    SweepFault(&g_tb_vol_state.dset_create_calls,
               &g_tb_vol_state.fail_dset_create_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_open_calls,
               &g_tb_vol_state.fail_dset_open_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_read_calls,
               &g_tb_vol_state.fail_dset_read_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_write_calls,
               &g_tb_vol_state.fail_dset_write_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_get_type_calls,
               &g_tb_vol_state.fail_dset_get_type_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_get_type_calls,
               &g_tb_vol_state.spoof_dset_get_type_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_get_space_calls,
               &g_tb_vol_state.fail_dset_get_space_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_get_space_calls,
               &g_tb_vol_state.spoof_dset_get_space_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_get_dcpl_calls,
               &g_tb_vol_state.fail_dset_get_dcpl_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_get_dcpl_calls,
               &g_tb_vol_state.spoof_dset_get_dcpl_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_set_extent_calls,
               &g_tb_vol_state.fail_dset_set_extent_call, op, max_calls);
    SweepFault(&g_tb_vol_state.dset_close_calls,
               &g_tb_vol_state.fail_dset_close_call, op, max_calls);
    SweepFault(&g_tb_vol_state.attr_create_calls,
               &g_tb_vol_state.fail_attr_create_call, op, max_calls);
    SweepFault(&g_tb_vol_state.attr_open_calls,
               &g_tb_vol_state.fail_attr_open_call, op, max_calls);
    SweepFault(&g_tb_vol_state.attr_read_calls,
               &g_tb_vol_state.fail_attr_read_call, op, max_calls);
    SweepFault(&g_tb_vol_state.attr_write_calls,
               &g_tb_vol_state.fail_attr_write_call, op, max_calls);
    SweepFault(&g_tb_vol_state.attr_exists_calls,
               &g_tb_vol_state.fail_attr_exists_call, op, max_calls);
    SweepFault(&g_tb_vol_state.attr_close_calls,
               &g_tb_vol_state.fail_attr_close_call, op, max_calls);
    SweepFault(&g_tb_vol_state.link_delete_calls,
               &g_tb_vol_state.fail_link_delete_call, op, max_calls);
    SweepFault(&g_tb_vol_state.link_move_calls,
               &g_tb_vol_state.fail_link_move_call, op, max_calls);
    SweepFault(&g_tb_vol_state.object_open_calls,
               &g_tb_vol_state.fail_object_open_call, op, max_calls);
  };

  std::vector<CompactRecord> out_rows(32);
  CompactRecord one_row = {7, 8, 9};
  constexpr int32_t kSingleI32 = 123;
  std::array<int32_t, 2> two_i32 = {};
  constexpr size_t kI32Sz = sizeof(int32_t);
  constexpr int kIdx0 = 0;

  sweep_all_knobs([&](int /*k*/) -> herr_t {
    reset_table("src1", "Src1");
    return H5TBappend_records(file, "src1", 1, sizeof(CompactRecord),
                              kCompactOffsets.data(), kCompactSizes.data(),
                              &one_row);
  });
  reset_table("src1", "Src1");
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    return H5TBwrite_records(file, "src1", 0, 1, sizeof(CompactRecord),
                             kCompactOffsets.data(), kCompactSizes.data(),
                             &one_row);
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    return H5TBwrite_fields_name(file, "src1", "a", 0, 1, sizeof(int32_t),
                                 nullptr, &kI32Sz, &kSingleI32);
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    return H5TBwrite_fields_index(file, "src1", 1, &kIdx0, 0, 1,
                                  sizeof(int32_t), nullptr, &kI32Sz,
                                  &kSingleI32);
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    return H5TBread_table(file, "src1", sizeof(CompactRecord),
                          kCompactOffsets.data(), kCompactSizes.data(),
                          out_rows.data());
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    return H5TBread_records(file, "src1", 0, 1, sizeof(CompactRecord),
                            kCompactOffsets.data(), kCompactSizes.data(),
                            out_rows.data());
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    return H5TBread_fields_name(file, "src1", "a", 0, 2, sizeof(int32_t),
                                nullptr, &kI32Sz, two_i32.data());
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    return H5TBread_fields_index(file, "src1", 1, &kIdx0, 0, 2, sizeof(int32_t),
                                 nullptr, &kI32Sz, two_i32.data());
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    // Delete middle record so read_nrecords > 0 branch is covered under faults.
    reset_table("src1", "Src1");
    return H5TBdelete_record(file, "src1", 1, 1);
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    reset_table("src1", "Src1");
    return H5TBinsert_record(file, "src1", 1, 1, sizeof(CompactRecord),
                             kCompactOffsets.data(), kCompactSizes.data(),
                             &one_row);
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    reset_table("src2", "Src2");
    return H5TBadd_records_from(file, "src2", 0, 1, "src1", 1);
  });

  // H5TBcombine_tables with fill values under fault sweep.
  reset_table("src1", "Src1");
  reset_table("src2", "Src2");
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    clean_link("comb_fault");
    return H5TBcombine_tables(file, "src1", file, "src2", "comb_fault");
  });
  clean_link("comb_fault");

  // Recreate a clean table for field-insertion / field-deletion fault sweeps.
  const std::vector<int32_t> ins_vals(16, 55);
  auto reset_field_tbl = [&]() {
    clean_link("new");
    reset_table("ftbl", "FTbl");
  };
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    reset_field_tbl();
    return H5TBinsert_field(file, "ftbl", "extra", H5T_NATIVE_INT32, 0, &fill.a,
                            ins_vals.data());
  });
  sweep_all_knobs([&](int /*k*/) -> herr_t {
    reset_field_tbl();
    return H5TBdelete_field(file, "ftbl", "b");
  });

  // Also test fail_all_attr_close during H5TBinsert_field / H5TBdelete_field to
  // cover out: H5Aclose error checks cleanly.
  reset_field_tbl();
  g_tb_vol_state.fail_all_attr_close = true;
  EXPECT_LT(H5TBinsert_field(file, "ftbl", "extra", H5T_NATIVE_INT32, 0,
                             &fill.a, ins_vals.data()),
            0);
  g_tb_vol_state.Reset();
  SweepOpenH5Ids(std::array{H5I_ATTR});
  reset_field_tbl();
  g_tb_vol_state.fail_all_attr_close = true;
  EXPECT_LT(H5TBdelete_field(file, "ftbl", "b"), 0);
  g_tb_vol_state.Reset();
  SweepOpenH5Ids(std::array{H5I_ATTR});
  clean_link("new");

  // Query functions (title, fill, table_info, field_info with all outputs).
  std::array<char, 64> qtitle = {};
  std::array<std::array<char, 64>, kCompactNFields> fnbufs = {};
  std::array<char*, kCompactNFields> fnptrs = {};
  for (size_t i = 0; i < kCompactNFields; ++i) {
    fnptrs[i] = fnbufs[i].data();
  }
  std::array<size_t, kCompactNFields> qsz = {};
  std::array<size_t, kCompactNFields> qoff = {};
  size_t qts = 0;
  hsize_t nf = 0;
  hsize_t nr = 0;
  CompactRecord fill_out = {};
  {
    ScopedDataset dset(H5Dopen2(file, "src2", H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);
    sweep_all_knobs(
        [&](int /*k*/) -> herr_t {
          return H5TBAget_title(dset, qtitle.data());
        },
        6);
    sweep_all_knobs(
        [&](int /*k*/) -> herr_t {
          return H5TBAget_fill(file, "src2", dset,
                               reinterpret_cast<unsigned char*>(&fill_out));
        },
        8);
  }
  sweep_all_knobs(
      [&](int /*k*/) -> herr_t {
        return H5TBget_table_info(file, "src2", &nf, &nr);
      },
      6);
  sweep_all_knobs(
      [&](int /*k*/) -> herr_t {
        return H5TBget_field_info(file, "src2", fnptrs.data(), qsz.data(),
                                  qoff.data(), &qts);
      },
      6);

  // Sweep fail_attr_close_from so out: H5Aclose(attr_id) fails after an
  // initial H5Aclose or H5Awrite failure leaves attr_id > 0. At least one call
  // threshold must fail so the sweep cannot pass vacuously.
  auto sweep_attr_close_from = [&](auto&& op, int max_calls) {
    bool any_failed = false;
    for (int k = 1; k <= max_calls; ++k) {
      g_tb_vol_state.Reset();
      g_tb_vol_state.fail_attr_close_from = k;
      if (op(k) < 0) {
        any_failed = true;
      }
      const int calls = g_tb_vol_state.attr_close_calls;
      g_tb_vol_state.Reset();
      SweepOpenH5Ids(std::array{H5I_ATTR});
      if (calls < k) {
        break;
      }
    }
    EXPECT_TRUE(any_failed);
  };
  sweep_attr_close_from(run_make, 16);
  clean_link("fault_mk");
  reset_table("src1", "Src1");
  reset_table("src2", "Src2");
  sweep_attr_close_from(
      [&](int /*k*/) -> herr_t {
        clean_link("comb_fault");
        return H5TBcombine_tables(file, "src1", file, "src2", "comb_fault");
      },
      20);
  clean_link("comb_fault");
  sweep_attr_close_from(
      [&](int /*k*/) -> herr_t {
        reset_field_tbl();
        return H5TBinsert_field(file, "ftbl", "extra", H5T_NATIVE_INT32, 0,
                                &fill.a, ins_vals.data());
      },
      20);
  sweep_attr_close_from(
      [&](int /*k*/) -> herr_t {
        reset_field_tbl();
        return H5TBdelete_field(file, "ftbl", "b");
      },
      20);
  clean_link("new");

  // Spoof H5Dget_type with a compound containing H5T_UNIX_D32LE (time type),
  // a 3-member H5T_ENUM, and an empty H5T_ENUM to cover H5Tget_native_type,
  // H5Tget_member_type, and H5Tget_member_name failure branches cleanly.
  ScopedDatatype time_comp_tid(H5Tcreate(H5T_COMPOUND, sizeof(CompactRecord)));
  ASSERT_TRUE(time_comp_tid.valid());
  ASSERT_GE(H5Tinsert(time_comp_tid, "a", 0, H5T_UNIX_D32LE), 0);
  ASSERT_GE(H5Tinsert(time_comp_tid, "b", 4, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(time_comp_tid, "c", 8, H5T_NATIVE_INT32), 0);

  // Use 4-byte H5T_NATIVE_INT32 enums so H5Tget_native_type succeeds safely
  // and 1-field int32_t tables have exact 4-byte record/fill size parity.
  ScopedDatatype enum_tid(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(enum_tid.valid());
  int32_t ev = 0;
  ASSERT_GE(H5Tenum_insert(enum_tid, "a", &ev), 0);
  ev = 1;
  ASSERT_GE(H5Tenum_insert(enum_tid, "b", &ev), 0);
  ev = 2;
  ASSERT_GE(H5Tenum_insert(enum_tid, "c", &ev), 0);

  ScopedDatatype empty_enum_tid(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(empty_enum_tid.valid());

  std::array<const char*, 1> f1_names = {"a"};
  constexpr std::array<size_t, 1> kF1Offsets = {0};
  const std::array<hid_t, 1> f1_types = {H5T_NATIVE_INT32};
  constexpr std::array<int32_t, 4> kF1Rows = {10, 20, 30, 40};
  auto reset_f1_table = [&](const char* name, const char* title,
                            int32_t* fill_ptr) {
    clean_link(name);
    const TbVolState saved = g_tb_vol_state;
    g_tb_vol_state.Reset();
    EXPECT_GE(
        H5TBmake_table(title, file, name, 1, kF1Rows.size(), sizeof(int32_t),
                       f1_names.data(), kF1Offsets.data(), f1_types.data(), 2,
                       fill_ptr, 0, kF1Rows.data()),
        0);
    g_tb_vol_state = saved;
  };
  auto reset_f1_insert_tbl = [&](const char* name) {
    clean_link("new");
    reset_f1_table(name, "F1NoFill", nullptr);
  };

  // Not every H5Dget_type call index returns an incompatible type under spoof,
  // but at least one call index must cause an error.
  auto sweep_spoof_type = [&](hid_t tmpl, auto&& op, int max_calls = 14) {
    bool any_failed = false;
    for (int k = 1; k <= max_calls; ++k) {
      g_tb_vol_state.Reset();
      g_tb_vol_state.spoof_dset_get_type_call = k;
      g_tb_vol_state.spoof_dset_get_type_template = tmpl;
      if (op(k) < 0) {
        any_failed = true;
      }
      if (g_tb_vol_state.dset_get_type_calls < k) {
        break;
      }
    }
    EXPECT_TRUE(any_failed);
    g_tb_vol_state.Reset();
  };
  auto spoof_type_at = [&](hid_t tmpl, int call_idx, auto&& op) {
    g_tb_vol_state.Reset();
    g_tb_vol_state.spoof_dset_get_type_call = call_idx;
    g_tb_vol_state.spoof_dset_get_type_template = tmpl;
    EXPECT_LT(op(), 0) << "call=" << call_idx;
    g_tb_vol_state.Reset();
  };

  reset_table("src1", "Src1");
  reset_table("src2", "Src2");
  // In H5TBread_table, H5Dget_type call #1 (ftype_id for H5TB_create_type)
  // extracts per-member types safely, while call #3 (inside
  // H5TBget_field_info) passes the compound itself to H5Tget_native_type and
  // leaks member types upstream if a field is H5T_TIME; spoof only call #1
  // with time_comp_tid, and test H5TBget_field_info with scalar
  // H5T_UNIX_D32LE directly.
  spoof_type_at(time_comp_tid, 1, [&]() -> herr_t {
    return H5TBread_table(file, "src1", sizeof(CompactRecord),
                          kCompactOffsets.data(), kCompactSizes.data(),
                          out_rows.data());
  });
  spoof_type_at(H5T_UNIX_D32LE, 1, [&]() -> herr_t {
    return H5TBget_field_info(file, "src2", fnptrs.data(), qsz.data(),
                              qoff.data(), &qts);
  });
  sweep_spoof_type(
      time_comp_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBwrite_fields_name(file, "src1", "a", 0, 1, sizeof(int32_t),
                                     nullptr, &kI32Sz, &kSingleI32);
      },
      4);
  sweep_spoof_type(
      time_comp_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBwrite_fields_index(file, "src1", 1, &kIdx0, 0, 1,
                                      sizeof(int32_t), nullptr, &kI32Sz,
                                      &kSingleI32);
      },
      4);
  sweep_spoof_type(
      time_comp_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBread_fields_name(file, "src1", "a", 0, 1, sizeof(int32_t),
                                    nullptr, &kI32Sz, two_i32.data());
      },
      4);
  sweep_spoof_type(
      time_comp_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBread_fields_index(file, "src1", 1, &kIdx0, 0, 1,
                                     sizeof(int32_t), nullptr, &kI32Sz,
                                     two_i32.data());
      },
      4);

  sweep_spoof_type(
      enum_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBwrite_fields_name(file, "src1", "a", 0, 1, sizeof(int32_t),
                                     nullptr, &kI32Sz, &kSingleI32);
      },
      4);
  sweep_spoof_type(
      enum_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBwrite_fields_index(file, "src1", 1, &kIdx0, 0, 1,
                                      sizeof(int32_t), nullptr, &kI32Sz,
                                      &kSingleI32);
      },
      4);
  sweep_spoof_type(
      enum_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBread_fields_name(file, "src1", "a", 0, 1, sizeof(int32_t),
                                    nullptr, &kI32Sz, two_i32.data());
      },
      4);
  sweep_spoof_type(
      enum_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBread_fields_index(file, "src1", 1, &kIdx0, 0, 1,
                                     sizeof(int32_t), nullptr, &kI32Sz,
                                     two_i32.data());
      },
      4);
  sweep_spoof_type(
      enum_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBget_field_info(file, "src2", fnptrs.data(), qsz.data(),
                                  qoff.data(), &qts);
      },
      4);
  sweep_spoof_type(
      enum_tid,
      [&](int /*k*/) -> herr_t {
        return H5TBread_table(file, "src1", sizeof(CompactRecord),
                              kCompactOffsets.data(), kCompactSizes.data(),
                              out_rows.data());
      },
      4);

  // Target exact H5Dget_type call indices on 1-field int32_t tables (4-byte
  // exact parity with enum_tid) for combine/insert, and on 3-field ftbl for
  // delete so H5TBget_table_info always observes the true field count and
  // tmp_fill_buf is never under-allocated.
  reset_f1_table("f1_src1", "F1Src1", &fill.a);
  reset_f1_table("f1_src2", "F1Src2", &fill.a);
  spoof_type_at(enum_tid, 3, [&]() -> herr_t {
    clean_link("comb_fault");
    return H5TBcombine_tables(file, "f1_src1", file, "f1_src2", "comb_fault");
  });
  clean_link("comb_fault");
  spoof_type_at(enum_tid, 2, [&]() -> herr_t {
    reset_f1_insert_tbl("f1_nofill");
    return H5TBinsert_field(file, "f1_nofill", "extra", H5T_NATIVE_INT32, 0,
                            nullptr, ins_vals.data());
  });
  spoof_type_at(empty_enum_tid, 2, [&]() -> herr_t {
    reset_f1_insert_tbl("f1_nofill");
    return H5TBinsert_field(file, "f1_nofill", "extra", H5T_NATIVE_INT32, 1,
                            nullptr, ins_vals.data());
  });
  spoof_type_at(enum_tid, 7, [&]() -> herr_t {
    reset_f1_insert_tbl("f1_nofill");
    return H5TBinsert_field(file, "f1_nofill", "extra", H5T_NATIVE_INT32, 0,
                            nullptr, ins_vals.data());
  });
  clean_link("f1_nofill");
  spoof_type_at(enum_tid, 2, [&]() -> herr_t {
    reset_field_tbl();
    return H5TBdelete_field(file, "ftbl", "b");
  });
  spoof_type_at(empty_enum_tid, 2, [&]() -> herr_t {
    reset_field_tbl();
    return H5TBdelete_field(file, "ftbl", "a");
  });
  spoof_type_at(enum_tid, 4, [&]() -> herr_t {
    reset_field_tbl();
    return H5TBdelete_field(file, "ftbl", "b");
  });

  // Cover H5TBdelete_field second-loop H5Tget_member_name failure (2-member
  // compound spoofed on a 3-field table).
  ScopedDatatype two_comp_tid(H5Tcreate(H5T_COMPOUND, 2 * sizeof(int32_t)));
  ASSERT_TRUE(two_comp_tid.valid());
  ASSERT_GE(H5Tinsert(two_comp_tid, "a", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(two_comp_tid, "b", sizeof(int32_t), H5T_NATIVE_INT32), 0);
  spoof_type_at(two_comp_tid, 2, [&]() -> herr_t {
    reset_field_tbl();
    return H5TBdelete_field(file, "ftbl", "b");
  });
  clean_link("new");

  g_tb_vol_state.Reset();
}

// TODO(schwehr): Remaining uncovered lines in H5TB.c are mainly malloc/calloc
// OOM guards and post-failure internal cleanup H5T/H5S/H5D/H5P close checks not
// reachable through public VOL fault injection.

}  // namespace
}  // namespace hdf5
