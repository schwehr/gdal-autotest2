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

// Unit tests for //third_party/netcdf/ncdump/ncvalidator.c.
//
// Exercises classic/64-bit-offset/CDF-5 format validation, CLI options
// (-h, -t, -q, -x), filesystem prefix stripping, non-null header padding
// detection and in-place repair, corrupt tags/types/dimensions/sizes/offsets,
// >1 MiB multi-chunk header buffering, POSIX syscall fault injection via
// ncvalidator_shim, and file size verification inside isolated forked children.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/ncvalidator.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/types/span.h"
#include "third_party/netcdf/google/ncdump/ncvalidator_shim.h"
#include "third_party/netcdf/google/util/cli_mains.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::HasSubstr;

constexpr uint32_t kTagAbsent = 0;
constexpr uint32_t kTagDimension = 10;
constexpr uint32_t kTagVariable = 11;
constexpr uint32_t kTagAttribute = 12;

constexpr uint32_t kCdfIntMax = 2147483647U;
constexpr size_t kDefaultChunkSize = 1048576U;

CliResult RunValidator(absl::Span<const std::string_view> args,
                       std::string_view stdin_data = "") {
  return RunCliInForkedChild(ncvalidator_cli_main, args, stdin_data);
}

template <typename ArmFn>
CliResult RunValidatorArmed(ArmFn&& arm_fn,
                            std::span<const std::string_view> args) {
  auto armed_main = [&arm_fn](int argc, char** argv) -> int {
    std::forward<ArmFn>(arm_fn)();
    const int rc = ncvalidator_cli_main(argc, argv);
    ncvalidator_shim_reset();
    return rc;
  };
  return RunCliInForkedChild(armed_main, args);
}

// Big-endian binary builder for NetCDF CDF-1, CDF-2, and CDF-5 files.
class CdfFileBuilder {
 public:
  explicit CdfFileBuilder(int version) : version_(version) {
    data_.push_back('C');
    data_.push_back('D');
    data_.push_back('F');
    data_.push_back(static_cast<uint8_t>(version));
  }

  void WriteU32(uint32_t val) {
    data_.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    data_.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    data_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    data_.push_back(static_cast<uint8_t>(val & 0xFF));
  }

  void WriteU64(uint64_t val) {
    WriteU32(static_cast<uint32_t>(val >> 32));
    WriteU32(static_cast<uint32_t>(val & 0xFFFFFFFFULL));
  }

  void WriteNonNeg(uint64_t val) {
    if (version_ < 5) {
      WriteU32(static_cast<uint32_t>(val));
    } else {
      WriteU64(val);
    }
  }

  void WriteOffset(uint64_t val) {
    if (version_ == 1) {
      WriteU32(static_cast<uint32_t>(val));
    } else {
      WriteU64(val);
    }
  }

  void WriteBytes(size_t count, uint8_t byte = 0) {
    data_.insert(data_.end(), count, byte);
  }

  void WriteString(std::string_view s, uint8_t pad_byte = 0) {
    WriteNonNeg(s.size());
    for (char c : s) {
      data_.push_back(static_cast<uint8_t>(c));
    }
    const size_t rem = s.size() % 4;
    if (rem != 0) {
      WriteBytes(4 - rem, pad_byte);
    }
  }

  void WriteAbsentList() {
    WriteU32(kTagAbsent);
    WriteNonNeg(0);
  }

  void WriteSimpleAttr(std::string_view name, uint32_t xtype, uint64_t nelems,
                       size_t elem_size, uint8_t name_pad = 0,
                       uint8_t val_pad = 0) {
    WriteString(name, name_pad);
    WriteU32(xtype);
    WriteNonNeg(nelems);
    const size_t raw_bytes = static_cast<size_t>(nelems) * elem_size;
    WriteBytes(raw_bytes, 0x2A);
    const size_t rem = raw_bytes % 4;
    if (rem != 0) {
      WriteBytes(4 - rem, val_pad);
    }
  }

  void WriteSimpleVar(std::string_view name, absl::Span<const uint64_t> dimids,
                      uint32_t xtype, uint64_t vsize, uint64_t begin,
                      uint8_t name_pad = 0) {
    WriteString(name, name_pad);
    WriteNonNeg(dimids.size());
    for (uint64_t dimid : dimids) {
      WriteNonNeg(dimid);
    }
    WriteAbsentList();
    WriteU32(xtype);
    WriteNonNeg(vsize);
    WriteOffset(begin);
  }

  size_t WriteSimpleVarPlaceholder(std::string_view name,
                                   absl::Span<const uint64_t> dimids,
                                   uint32_t xtype, uint64_t vsize,
                                   uint8_t name_pad = 0) {
    WriteString(name, name_pad);
    WriteNonNeg(dimids.size());
    for (uint64_t dimid : dimids) {
      WriteNonNeg(dimid);
    }
    WriteAbsentList();
    WriteU32(xtype);
    WriteNonNeg(vsize);
    const size_t begin_pos = data_.size();
    WriteOffset(0);
    return begin_pos;
  }

  void PatchU32(size_t pos, uint32_t val) {
    ASSERT_LE(pos + 4, data_.size());
    data_[pos] = static_cast<uint8_t>((val >> 24) & 0xFF);
    data_[pos + 1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    data_[pos + 2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data_[pos + 3] = static_cast<uint8_t>(val & 0xFF);
  }

  void PatchOffset(size_t pos, uint64_t val) {
    if (version_ == 1) {
      PatchU32(pos, static_cast<uint32_t>(val));
    } else {
      PatchU32(pos, static_cast<uint32_t>(val >> 32));
      PatchU32(pos + 4, static_cast<uint32_t>(val & 0xFFFFFFFFULL));
    }
  }

  size_t size() const { return data_.size(); }

  void PadTo(size_t total_size, uint8_t fill = 0) {
    if (data_.size() < total_size) {
      WriteBytes(total_size - data_.size(), fill);
    }
  }

  void WriteToFile(const TempFile& file) const {
    std::ofstream out(file.path(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(data_.data()),
              static_cast<std::streamsize>(data_.size()));
    ASSERT_TRUE(out.good());
  }

 private:
  int version_;
  std::vector<uint8_t> data_;
};

void WriteRawBytes(const TempFile& file, absl::Span<const uint8_t> bytes) {
  std::ofstream out(file.path(), std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good());
}

std::vector<uint8_t> ReadRawBytes(const TempFile& file) {
  std::ifstream in(file.path(), std::ios::binary | std::ios::ate);
  EXPECT_TRUE(in.good());
  const std::streamsize len = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(len));
  in.read(reinterpret_cast<char*>(buf.data()), len);
  EXPECT_TRUE(in.good());
  return buf;
}

TEST(NcvalidatorTest, CliUsageHelpAndOpenFailures) {
  {
    constexpr std::string_view kArgs[] = {"ncvalidator", "-h"};
    const CliResult res = RunValidator(kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("Usage: ncvalidator"));
  }

  {
    constexpr std::string_view kArgs[] = {"ncvalidator", "-Z"};
    const CliResult res = RunValidator(kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("Usage: ncvalidator"));
  }

  {
    constexpr std::string_view kArgs[] = {"ncvalidator", "-t", "-q"};
    const CliResult res = RunValidator(kArgs);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("Usage: ncvalidator"));
  }

  {
    TempFile missing_nc("does_not_exist.nc");
    const std::string& missing = missing_nc.string();
    const std::string_view args[] = {"ncvalidator", missing};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("Error on open file"));
  }

  {
    // Opening a directory with O_RDONLY succeeds; read() fails with EISDIR.
    TempDir dir("ncvalidator_dir_input");
    const std::string dir_path = dir.string();
    const std::string_view args[] = {"ncvalidator", dir_path};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("read"));
  }

  {
    // Stdin in RunCliInForkedChild is a pipe; lseek() fails with ESPIPE.
    constexpr std::string_view kArgs[] = {"ncvalidator", "/dev/stdin"};
    const CliResult res = RunValidator(kArgs, "CDF\x01");
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, HasSubstr("lseek"));
  }
}

TEST(NcvalidatorTest, ValidatesRealNetcdfClassicOffsetAndCdf5Files) {
  struct FormatCase {
    const char* name;
    int cmode;
    int version;
    const char* trace_tag;
  };
  constexpr FormatCase kFormats[] = {
      {"cdf1", NC_CLOBBER, 1, "CDF-1"},
      {"cdf2", NC_CLOBBER | NC_64BIT_OFFSET, 2, "CDF-2"},
      {"cdf5", NC_CLOBBER | NC_CDF5, 5, "CDF-5"},
  };

  for (const FormatCase& tc : kFormats) {
    SCOPED_TRACE(tc.name);
    // NetCDF's ncx_put_NC() zero-pads minimal headers to 4096 bytes on
    // nc_create()/nc_enddef(), whereas ncvalidator requires empty CDF files
    // with zero variables to have exact header size (32 or 48 bytes).
    TempFile empty_nc(std::string("empty_") + tc.name + ".nc");
    {
      CdfFileBuilder empty_builder(tc.version);
      empty_builder.WriteNonNeg(0);
      empty_builder.WriteAbsentList();
      empty_builder.WriteAbsentList();
      empty_builder.WriteAbsentList();
      empty_builder.WriteToFile(empty_nc);
    }
    const std::string empty_path = empty_nc.string();
    const std::string_view trace_empty[] = {"ncvalidator", "-t", empty_path};
    const CliResult res_empty = RunValidator(trace_empty);
    ASSERT_TRUE(res_empty.ok()) << res_empty.harness_error;
    EXPECT_EQ(0, res_empty.exit_code) << res_empty.stdout_output;
    EXPECT_THAT(res_empty.stdout_output, HasSubstr(tc.trace_tag));
    EXPECT_THAT(res_empty.stdout_output, HasSubstr("tag = ABSENT"));
    EXPECT_THAT(res_empty.stdout_output,
                HasSubstr("is a valid NetCDF classic file"));

    // Create a populated dataset exercising dimensions, global/var attributes
    // (including 0-length and odd-length padded attributes), scalar vars,
    // fixed vars, and record vars.
    TempFile full_nc(std::string("full_") + tc.name + ".nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(full_nc.c_str(), tc.cmode, nc.ptr()));

      int dim_time = -1;
      int dim_x = -1;
      int dim_y = -1;
      ASSERT_EQ(NC_NOERR,
                nc_def_dim(nc.get(), "time", NC_UNLIMITED, &dim_time));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 3, &dim_x));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "yy", 2, &dim_y));

      // Global attributes across supported types and paddings.
      const signed char b_vals[3] = {1, -2, 3};
      const short s_vals[1] = {42};
      const int i_vals[2] = {10, 20};
      const float f_vals[1] = {1.5f};
      const double d_vals[1] = {2.5};
      ASSERT_EQ(NC_NOERR,
                nc_put_att_text(nc.get(), NC_GLOBAL, "title", 2, "hi"));
      ASSERT_EQ(NC_NOERR,
                nc_put_att_text(nc.get(), NC_GLOBAL, "empty_txt", 0, ""));
      ASSERT_EQ(NC_NOERR, nc_put_att_schar(nc.get(), NC_GLOBAL, "gb", NC_BYTE,
                                           3, b_vals));
      ASSERT_EQ(NC_NOERR, nc_put_att_short(nc.get(), NC_GLOBAL, "gs", NC_SHORT,
                                           1, s_vals));
      ASSERT_EQ(NC_NOERR,
                nc_put_att_int(nc.get(), NC_GLOBAL, "gi", NC_INT, 2, i_vals));
      ASSERT_EQ(NC_NOERR, nc_put_att_float(nc.get(), NC_GLOBAL, "gf", NC_FLOAT,
                                           1, f_vals));
      ASSERT_EQ(NC_NOERR, nc_put_att_double(nc.get(), NC_GLOBAL, "gd",
                                            NC_DOUBLE, 1, d_vals));

      if (tc.cmode & NC_CDF5) {
        const unsigned char ub_vals[2] = {200, 201};
        const unsigned short us_vals[3] = {1, 2, 3};
        const unsigned int ui_vals[1] = {3000000000U};
        const long long i64_vals[1] = {-1234567890123LL};
        const unsigned long long u64_vals[1] = {12345678901234ULL};
        ASSERT_EQ(NC_NOERR, nc_put_att_uchar(nc.get(), NC_GLOBAL, "gub",
                                             NC_UBYTE, 2, ub_vals));
        ASSERT_EQ(NC_NOERR, nc_put_att_ushort(nc.get(), NC_GLOBAL, "gus",
                                              NC_USHORT, 3, us_vals));
        ASSERT_EQ(NC_NOERR, nc_put_att_uint(nc.get(), NC_GLOBAL, "gui", NC_UINT,
                                            1, ui_vals));
        ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc.get(), NC_GLOBAL, "gi64",
                                                NC_INT64, 1, i64_vals));
        ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc.get(), NC_GLOBAL, "gu64",
                                                 NC_UINT64, 1, u64_vals));
      }

      int var_scalar = -1;
      int var_fixed = -1;
      int var_rec1d = -1;
      int var_rec2d = -1;
      const int fixed_dims[2] = {dim_x, dim_y};
      const int rec_dims[2] = {dim_time, dim_x};
      ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "scalar", NC_SHORT, 0, nullptr,
                                     &var_scalar));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "fixed_mat", NC_FLOAT, 2,
                                     fixed_dims, &var_fixed));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "rec_vec", NC_DOUBLE, 1,
                                     &dim_time, &var_rec1d));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "rec_mat", NC_BYTE, 2, rec_dims,
                                     &var_rec2d));
      ASSERT_EQ(NC_NOERR,
                nc_put_att_text(nc.get(), var_fixed, "units", 1, "K"));
      ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), var_rec1d, "scale", NC_INT,
                                         1, i_vals));

      if (tc.cmode & NC_CDF5) {
        int v_ub = -1;
        int v_us = -1;
        int v_ui = -1;
        int v_i64 = -1;
        int v_u64 = -1;
        int v_ch = -1;
        ASSERT_EQ(NC_NOERR,
                  nc_def_var(nc.get(), "v_ch", NC_CHAR, 1, &dim_x, &v_ch));
        ASSERT_EQ(NC_NOERR,
                  nc_def_var(nc.get(), "v_ub", NC_UBYTE, 0, nullptr, &v_ub));
        ASSERT_EQ(NC_NOERR,
                  nc_def_var(nc.get(), "v_us", NC_USHORT, 0, nullptr, &v_us));
        ASSERT_EQ(NC_NOERR,
                  nc_def_var(nc.get(), "v_ui", NC_UINT, 0, nullptr, &v_ui));
        ASSERT_EQ(NC_NOERR,
                  nc_def_var(nc.get(), "v_i64", NC_INT64, 0, nullptr, &v_i64));
        ASSERT_EQ(NC_NOERR,
                  nc_def_var(nc.get(), "v_u64", NC_UINT64, 0, nullptr, &v_u64));
      }

      ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

      const short s0 = 7;
      ASSERT_EQ(NC_NOERR, nc_put_var_short(nc.get(), var_scalar, &s0));
      const float mat[6] = {1, 2, 3, 4, 5, 6};
      ASSERT_EQ(NC_NOERR, nc_put_var_float(nc.get(), var_fixed, mat));
      const size_t idx0 = 0;
      const double t0 = 1.0;
      ASSERT_EQ(NC_NOERR, nc_put_var1_double(nc.get(), var_rec1d, &idx0, &t0));
      const size_t idx1 = 1;
      const double t1 = 2.0;
      ASSERT_EQ(NC_NOERR, nc_put_var1_double(nc.get(), var_rec1d, &idx1, &t1));
      ASSERT_EQ(NC_NOERR, nc.close());
    }

    const std::string full_path = full_nc.string();
    const std::string_view trace_args[] = {"ncvalidator", "-t", full_path};
    const CliResult trace_res = RunValidator(trace_args);
    ASSERT_TRUE(trace_res.ok()) << trace_res.harness_error;
    EXPECT_EQ(0, trace_res.exit_code) << trace_res.stdout_output;
    EXPECT_THAT(trace_res.stdout_output, HasSubstr(tc.trace_tag));
    EXPECT_THAT(trace_res.stdout_output, HasSubstr("UNLIMITED"));
    EXPECT_THAT(trace_res.stdout_output, HasSubstr("vatt_list"));
    if (tc.cmode & NC_CDF5) {
      EXPECT_THAT(trace_res.stdout_output, HasSubstr("NC_UBYTE"));
      EXPECT_THAT(trace_res.stdout_output, HasSubstr("NC_USHORT"));
      EXPECT_THAT(trace_res.stdout_output, HasSubstr("NC_UINT"));
      EXPECT_THAT(trace_res.stdout_output, HasSubstr("NC_INT64"));
      EXPECT_THAT(trace_res.stdout_output, HasSubstr("NC_UINT64"));
    }

    // Quiet mode suppresses stdout and exits 0 (stderr may still contain
    // runtime/coverage diagnostics emitted before child _exit()).
    const std::string_view quiet_args[] = {"ncvalidator", "-q", full_path};
    const CliResult quiet_res = RunValidator(quiet_args);
    ASSERT_TRUE(quiet_res.ok()) << quiet_res.harness_error;
    EXPECT_EQ(0, quiet_res.exit_code) << quiet_res.stderr_output;
    EXPECT_TRUE(quiet_res.stdout_output.empty());

    // Prefix stripping ("lustre:/path/to/file.nc").
    const std::string prefixed = "lustre:" + full_path;
    const std::string_view prefix_args[] = {"ncvalidator", prefixed};
    const CliResult prefix_res = RunValidator(prefix_args);
    ASSERT_TRUE(prefix_res.ok()) << prefix_res.harness_error;
    EXPECT_EQ(0, prefix_res.exit_code) << prefix_res.stdout_output;
  }
}

TEST(NcvalidatorTest, RejectsHdf5AndUnknownSignaturesAndWarnsOnTruncatedFile) {
  for (const int cmode :
       {NC_CLOBBER | NC_NETCDF4, NC_CLOBBER | NC_NETCDF4 | NC_CLASSIC_MODEL}) {
    TempFile hdf5_nc("hdf5_input.nc");
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(hdf5_nc.c_str(), cmode, nc.ptr()));
      ASSERT_EQ(NC_NOERR, nc.close());
    }
    const std::string path = hdf5_nc.string();
    const std::string_view args[] = {"ncvalidator", path};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("HDF format"));
  }

  {
    TempFile bad_magic("bad_magic.nc");
    std::array<uint8_t, 32> bytes{};
    bytes[0] = 'X';
    bytes[1] = 'Y';
    bytes[2] = 'Z';
    bytes[3] = 0x01;
    WriteRawBytes(bad_magic, bytes);
    const std::string path = bad_magic.string();
    const std::string_view args[] = {"ncvalidator", path};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("Unknown file signature"));
  }

  {
    TempFile bad_ver("bad_ver.nc");
    std::array<uint8_t, 32> bytes{};
    bytes[0] = 'C';
    bytes[1] = 'D';
    bytes[2] = 'F';
    bytes[3] = 0x03;
    WriteRawBytes(bad_ver, bytes);
    const std::string path = bad_ver.string();
    const std::string_view args[] = {"ncvalidator", path};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("Unknown file signature"));
  }

  {
    TempFile empty_file("empty_zero_bytes.nc");
    WriteRawBytes(empty_file, {});
    const std::string path = empty_file.string();
    const std::string_view args[] = {"ncvalidator", path};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("Unknown file signature"));
  }

  {
    // Truncated 8-byte classic header triggers file-size-less-than-expected
    // warning while remaining valid (exit 0).
    TempFile trunc_file("trunc_header.nc");
    const std::array<uint8_t, 8> bytes = {'C', 'D', 'F', 0x01, 0, 0, 0, 0};
    WriteRawBytes(trunc_file, bytes);
    const std::string path = trunc_file.string();
    const std::string_view args[] = {"ncvalidator", path};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("Warning:"));
    EXPECT_THAT(res.stdout_output, HasSubstr("less than expected"));
  }
}

TEST(NcvalidatorTest, ChecksDataSizesForRecordAndNonRecordFiles) {
  // 1. Empty header (numrecs == 0, vars == 0) with extra trailing bytes.
  {
    TempFile f("extra_bytes_empty.nc");
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteBytes(12, 0);
    b.WriteToFile(f);
    const std::string path = f.string();
    const std::string_view args[] = {"ncvalidator", path};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("larger than expected"));

    const std::string_view qargs[] = {"ncvalidator", "-q", path};
    const CliResult qres = RunValidator(qargs);
    ASSERT_TRUE(qres.ok()) << qres.harness_error;
    EXPECT_EQ(1, qres.exit_code);
    EXPECT_TRUE(qres.stdout_output.empty());
  }

  // 2. Fixed variable only (numrecs == 0, vars > 0): both larger and smaller.
  {
    TempFile f_big("extra_bytes_fixed.nc");
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("d");
    b.WriteU32(2);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    const uint64_t dims[1] = {0};
    const size_t begin_pos = b.WriteSimpleVarPlaceholder("v", dims, NC_INT, 8);
    const uint64_t header_end = b.size();
    b.PatchOffset(begin_pos, header_end);
    b.WriteBytes(8, 0);
    b.WriteBytes(4, 0);  // Extra bytes -> error.
    b.WriteToFile(f_big);

    const std::string p_big = f_big.string();
    const std::string_view args_big[] = {"ncvalidator", p_big};
    const CliResult res_big = RunValidator(args_big);
    ASSERT_TRUE(res_big.ok()) << res_big.harness_error;
    EXPECT_EQ(1, res_big.exit_code);
    EXPECT_THAT(res_big.stdout_output, HasSubstr("larger than expected"));

    // Shorter than expected -> warning, exit 0.
    std::vector<uint8_t> short_bytes = ReadRawBytes(f_big);
    short_bytes.resize(header_end + 4);
    TempFile f_short("short_bytes_fixed.nc");
    WriteRawBytes(f_short, short_bytes);
    const std::string p_short = f_short.string();
    const std::string_view args_short[] = {"ncvalidator", p_short};
    const CliResult res_short = RunValidator(args_short);
    ASSERT_TRUE(res_short.ok()) << res_short.harness_error;
    EXPECT_EQ(0, res_short.exit_code);
    EXPECT_THAT(res_short.stdout_output, HasSubstr("Warning:"));
    EXPECT_THAT(res_short.stdout_output, HasSubstr("less than expected"));
  }

  // 3. Record variables (numrecs > 0): both larger and smaller than expected,
  // plus single-record-var packed recsize vs multiple-record-var recsize.
  {
    TempFile f_rec("rec_size_mismatch.nc");
    CdfFileBuilder b(1);
    b.WriteU32(2);  // numrecs = 2
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("t");
    b.WriteU32(0);  // NC_UNLIMITED
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    const uint64_t dims[1] = {0};
    const size_t begin_pos = b.WriteSimpleVarPlaceholder("r", dims, NC_BYTE, 4);
    const uint64_t header_end = b.size();
    b.PatchOffset(begin_pos, header_end);
    // Single record var: recsize is packed (1 byte) * 2 = 2 bytes.
    b.WriteBytes(8, 0);  // 8 > 2 -> larger than expected.
    b.WriteToFile(f_rec);

    const std::string p_rec = f_rec.string();
    const std::string_view args_big[] = {"ncvalidator", p_rec};
    const CliResult res_big = RunValidator(args_big);
    ASSERT_TRUE(res_big.ok()) << res_big.harness_error;
    EXPECT_EQ(1, res_big.exit_code);
    EXPECT_THAT(res_big.stdout_output, HasSubstr("larger than expected"));

    // 1 byte < 2 expected -> warning, exit 0.
    std::vector<uint8_t> short_bytes = ReadRawBytes(f_rec);
    short_bytes.resize(header_end + 1);
    TempFile f_rec_short("rec_size_short.nc");
    WriteRawBytes(f_rec_short, short_bytes);
    const std::string p_short = f_rec_short.string();
    const std::string_view args_short[] = {"ncvalidator", p_short};
    const CliResult res_short = RunValidator(args_short);
    ASSERT_TRUE(res_short.ok()) << res_short.harness_error;
    EXPECT_EQ(0, res_short.exit_code);
    EXPECT_THAT(res_short.stdout_output, HasSubstr("Warning:"));
    EXPECT_THAT(res_short.stdout_output, HasSubstr("less than expected"));
  }
}

TEST(NcvalidatorTest, DetectsAndRepairsNonNullHeaderPadding) {
  auto build_bad_padding_file = [](const TempFile& file) {
    CdfFileBuilder b(1);
    b.WriteU32(0);  // numrecs
    // Three dims with 3-, 2-, and 1-byte non-null name padding (also exercises
    // second status == NC_ENULLPAD check across multiple dims).
    b.WriteU32(kTagDimension);
    b.WriteU32(3);
    b.WriteString("a", 0x5A);
    b.WriteU32(3);
    b.WriteString("bb", 0x5A);
    b.WriteU32(2);
    b.WriteString("ccc", 0x5A);
    b.WriteU32(4);

    // Two global attrs: first has bad name padding + bad char value padding;
    // second has bad name padding + bad short value padding.
    b.WriteU32(kTagAttribute);
    b.WriteU32(2);
    b.WriteSimpleAttr("c", NC_CHAR, 1, 1, /*name_pad=*/0x5A, /*val_pad=*/0x5A);
    b.WriteSimpleAttr("dd", NC_SHORT, 1, 2, /*name_pad=*/0x5A,
                      /*val_pad=*/0x5A);

    // Two vars: first has bad name padding and var attr with bad padding;
    // second has bad name padding.
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    // Var 0:
    b.WriteString("v", 0x5A);
    b.WriteU32(1);
    b.WriteU32(0);  // dim 0
    b.WriteU32(kTagAttribute);
    b.WriteU32(1);
    b.WriteSimpleAttr("p", NC_BYTE, 3, 1, /*name_pad=*/0x5A, /*val_pad=*/0x5A);
    b.WriteU32(NC_INT);
    b.WriteU32(12);
    const size_t var0_begin_pos = b.size();
    b.WriteU32(0);  // placeholder
    // Var 1:
    const uint64_t dims1[1] = {1};
    const size_t var1_begin_pos =
        b.WriteSimpleVarPlaceholder("w", dims1, NC_INT, 8, /*name_pad=*/0x5A);

    const uint64_t hdr_end = b.size();
    b.PatchOffset(var0_begin_pos, hdr_end);
    b.PatchOffset(var1_begin_pos, hdr_end + 12);
    b.WriteBytes(20, 0);
    b.WriteToFile(file);
  };

  TempFile f_rep("nullpad_repair.nc");
  build_bad_padding_file(f_rep);
  const std::string p_rep = f_rep.string();

  // Check error detection (default verbose).
  {
    const std::string_view args[] = {"ncvalidator", p_rep};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("name padding is non-null byte"));
    EXPECT_THAT(res.stdout_output, HasSubstr("value padding is non-null byte"));
  }

  // Quiet check still exits 1 with no stdout.
  {
    const std::string_view args[] = {"ncvalidator", "-q", p_rep};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_TRUE(res.stdout_output.empty());
  }

  // Repair in-place (-x) prints repair messages and exits 1.
  {
    const std::string_view args[] = {"ncvalidator", "-x", p_rep};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("**repaired**"));
    EXPECT_THAT(res.stdout_output, HasSubstr("repaired in place"));
  }

  // Re-validating the repaired file succeeds with exit 0.
  {
    const std::string_view args[] = {"ncvalidator", p_rep};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stdout_output;
    EXPECT_THAT(res.stdout_output, HasSubstr("is a valid NetCDF classic file"));
  }

  // Quiet repair (-x -q) also repairs in-place silently.
  TempFile f_qrep("nullpad_quiet_repair.nc");
  build_bad_padding_file(f_qrep);
  const std::string p_qrep = f_qrep.string();
  {
    const std::string_view args[] = {"ncvalidator", "-x", "-q", p_qrep};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_TRUE(res.stdout_output.empty());
  }
  {
    const std::string_view args[] = {"ncvalidator", p_qrep};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code);
  }

  // Exercise NC_ENULLPAD starting in gatt_list (clean dims) and in var_list
  // (clean dims + clean gatt_list) as well as clean-name + bad-attr-val-pad.
  {
    TempFile f_gatt("nullpad_gatt_only.nc");
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU32(1);
    b.WriteSimpleAttr("oknm", NC_BYTE, 1, 1, /*name_pad=*/0, /*val_pad=*/0x33);
    b.WriteAbsentList();
    b.WriteToFile(f_gatt);
    const std::string p = f_gatt.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("value padding is non-null byte"));
  }

  {
    TempFile f_var("nullpad_var_only.nc");
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    // Clean var name, var attr has NC_ENULLPAD.
    b.WriteString("okvr");
    b.WriteU32(0);
    b.WriteU32(kTagAttribute);
    b.WriteU32(1);
    b.WriteSimpleAttr("at", NC_CHAR, 1, 1, /*name_pad=*/0x11, /*val_pad=*/0);
    b.WriteU32(NC_INT);
    b.WriteU32(4);
    b.WriteU32(static_cast<uint32_t>(b.size() + 4));
    b.WriteBytes(4, 0);
    b.WriteToFile(f_var);
    const std::string p = f_var.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("name padding is non-null byte"));
  }

  // Clean dim 0 followed by dirty dim 1 (exercises NC_NOERR -> NC_ENULLPAD
  // transition in dim loop).
  {
    TempFile f_dim1("nullpad_dim1_only.nc");
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("d0", 0x00);
    b.WriteU32(3);
    b.WriteString("d1", 0x44);
    b.WriteU32(4);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteToFile(f_dim1);
    const std::string p = f_dim1.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("name padding is non-null byte"));
  }
}

TEST(NcvalidatorTest, RejectsInvalidTagsCountsTypesDimensionsAndVarErrors) {
  auto expect_fail = [](const CdfFileBuilder& b, const char* name,
                        std::string_view substr, bool trace_mode = false) {
    SCOPED_TRACE(name);
    TempFile f(std::string("err_") + name + ".nc");
    b.WriteToFile(f);
    const std::string p = f.string();
    std::vector<std::string_view> args = {"ncvalidator"};
    if (trace_mode) {
      args.push_back("-t");
    }
    args.push_back(p);
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code) << res.stdout_output;
    if (!substr.empty()) {
      EXPECT_THAT(res.stdout_output, HasSubstr(substr));
    }
  };

  // Invalid component tag (99) in dim_list.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(99);
    b.WriteU32(0);
    expect_fail(b, "bad_tag", "Invalid NC component tag (99)");
  }

  // Wrong tag (NC_ATTRIBUTE=12) when dim_list nelems == 1.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagAttribute);
    b.WriteU32(1);
    expect_fail(b, "wrong_dim_tag", "expecting NC_DIMENSION");
  }

  // dim_list nelems > NC_MAX_DIMS.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(kCdfIntMax + 1U);
    expect_fail(b, "max_dims", "exceeds NC_MAX_DIMS");
  }

  // Two NC_UNLIMITED dimensions (second dim fails and frees dim 0).
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("t1");
    b.WriteU32(0);
    b.WriteString("t2");
    b.WriteU32(0);
    expect_fail(b, "two_unlim_dims", "NC_UNLIMITED dimension already found");
  }

  // Invalid component tag (99) in gatt_list.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(99);
    b.WriteU32(0);
    expect_fail(b, "bad_gatt_tag", "Invalid NC component tag (99)");
  }

  // Wrong tag (NC_DIMENSION=10) when gatt_list nelems == 1.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    expect_fail(b, "wrong_gatt_tag", "expecting NC_ATTRIBUTE");
  }

  // gatt_list nelems > NC_MAX_ATTRS.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU32(kCdfIntMax + 1U);
    expect_fail(b, "max_attrs", "exceeds NC_MAX_ATTRS");
  }

  // Invalid attr types: NC_NAT (0), NC_UBYTE (7) in CDF-1 and CDF-2, 12 in
  // CDF-5; plus failing on 2nd attribute so free_NC_attrarray cleans up 1st.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU32(2);
    b.WriteSimpleAttr("ok", NC_INT, 1, 4);
    b.WriteString("bad0");
    b.WriteU32(0);  // NC_NAT
    b.WriteU32(1);
    expect_fail(b, "attr_type_nat", "Unknown NC data type");
  }
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU32(1);
    b.WriteString("bad7");
    b.WriteU32(NC_UBYTE);
    b.WriteU32(1);
    expect_fail(b, "attr_type_cdf1_ubyte", "Unknown NC data type");
  }
  {
    CdfFileBuilder b(2);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU32(1);
    b.WriteString("bad7_cdf2");
    b.WriteU32(NC_UBYTE);
    b.WriteU32(1);
    expect_fail(b, "attr_type_cdf2_ubyte", "Unknown NC data type");
  }
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU64(1);
    b.WriteString("bad12");
    b.WriteU32(12);
    b.WriteU64(1);
    expect_fail(b, "attr_type_cdf5_12", "Unknown NC data type");
  }

  // Invalid component tag (99) in var_list.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(99);
    b.WriteU32(0);
    expect_fail(b, "bad_var_tag", "Invalid NC component tag (99)");
  }

  // Wrong tag (0) when var_list nelems == 1.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(kTagAbsent);
    b.WriteU32(1);
    expect_fail(b, "wrong_var_tag", "expecting NC_VARIABLE");
  }

  // var_list nelems > NC_MAX_VARS.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(kCdfIntMax + 1U);
    expect_fail(b, "max_vars", "exceeds NC_MAX_VARS");
  }

  // Variable ndims > NC_MAX_VAR_DIMS.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    b.WriteString("v");
    b.WriteU32(kCdfIntMax + 1U);
    expect_fail(b, "max_var_dims", "larger than NC_MAX_VAR_DIMS");
  }

  // Variable dimid >= f_ndims (exercised with trace mode on!).
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("d0");
    b.WriteU32(4);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    b.WriteString("v");
    b.WriteU32(1);
    b.WriteU32(1);  // >= f_ndims (1)
    expect_fail(b, "var_dimid_oob", "larger than the number of dimensions",
                /*trace_mode=*/true);
  }

  // Variable attribute error (triggers var cleanup via val_get_NC_attrarray).
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    b.WriteString("v");
    b.WriteU32(0);
    b.WriteU32(kTagAttribute);
    b.WriteU32(1);
    b.WriteString("va");
    b.WriteU32(99);  // bad type
    b.WriteU32(0);
    expect_fail(b, "var_bad_attr_type", "Unknown NC data type");
  }

  // Variable attribute array wrong tag (NC_DIMENSION=10) when nelems == 1.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    b.WriteString("v");
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    expect_fail(b, "var_wrong_vatt_tag", "expecting NC_ATTRIBUTE");
  }

  // Variable invalid xtype (also on 2nd var so 1st var is cleaned up).
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    b.WriteSimpleVar("v0", {}, NC_INT, 4, 200);
    b.WriteString("v1");
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(0);  // NC_NAT
    expect_fail(b, "var_bad_type", "Unknown NC data type");
  }

  // Negative dimid (-1 = 0xFFFFFFFF) on var 0 dim 0 -> compute_var_shape error.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("d0");
    b.WriteU32(4);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    const uint64_t dims[1] = {0xFFFFFFFFULL};
    b.WriteSimpleVar("v0", dims, NC_INT, 16, 200);
    expect_fail(b, "negative_dimid", "invalid (-1)");
  }

  // NC_UNLIMITED in dim index 1 (!= 0) -> NC_EUNLIMPOS.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("fix");
    b.WriteU32(3);
    b.WriteString("rec");
    b.WriteU32(0);  // unlimited
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    const uint64_t dims[2] = {0, 1};
    b.WriteSimpleVar("bad_unlim", dims, NC_INT, 12, 200);
    expect_fail(b, "unlim_wrong_pos", "NC_UNLIMITED in the wrong index");
  }

  // Zero-length name (nchars == 0) in a valid classic file hits hdr_len_NC_name
  // nchars == 0 branch cleanly.
  {
    TempFile f("empty_dim_name.nc");
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("");  // nchars == 0
    b.WriteU32(2);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteToFile(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stdout_output;
  }
}

TEST(NcvalidatorTest, ValidatesVariableSizeConstraintsAndOffsetOrdering) {
  auto run_builder = [](const CdfFileBuilder& b, const char* name,
                        int expected_exit, std::string_view substr) {
    SCOPED_TRACE(name);
    TempFile f(std::string("vchk_") + name + ".nc");
    b.WriteToFile(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(expected_exit, res.exit_code) << res.stdout_output;
    if (!substr.empty()) {
      EXPECT_THAT(res.stdout_output, HasSubstr(substr));
    }
  };

  constexpr uint64_t kCdf1LargeDim = 550000000ULL;   // *4 = 2.2e9 > INT_MAX-3
  constexpr uint64_t kCdf2LargeDim = 1100000000ULL;  // *4 = 4.4e9 > UINT_MAX-3

  // CDF-1: valid when single large fixed var is last and no record vars exist.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("sm");
    b.WriteU32(2);
    b.WriteString("lg");
    b.WriteU32(kCdf1LargeDim);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t d0[1] = {0};
    const uint64_t d1[1] = {1};
    b.WriteSimpleVar("v0", d0, NC_INT, 8, 256);
    b.WriteSimpleVar("v1", d1, NC_INT, kCdf1LargeDim * 4, 264);
    b.PadTo(300);
    run_builder(b, "cdf1_valid_last_large_fixed", 0, "Warning:");
  }

  // CDF-1: two large fixed vars -> NC_EVARSIZE.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("lg");
    b.WriteU32(kCdf1LargeDim);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t d0[1] = {0};
    b.WriteSimpleVar("v0", d0, NC_INT, kCdf1LargeDim * 4, 256);
    b.WriteSimpleVar("v1", d0, NC_INT, kCdf1LargeDim * 4,
                     256 + kCdf1LargeDim * 4);
    run_builder(b, "cdf1_two_large_fixed", 1, "only one large fixed-size");
  }

  // CDF-1: large fixed var not last -> NC_EVARSIZE.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("lg");
    b.WriteU32(kCdf1LargeDim);
    b.WriteString("sm");
    b.WriteU32(2);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t d0[1] = {0};
    const uint64_t d1[1] = {1};
    b.WriteSimpleVar("v0", d0, NC_INT, kCdf1LargeDim * 4, 256);
    b.WriteSimpleVar("v1", d1, NC_INT, 8, 256 + kCdf1LargeDim * 4);
    run_builder(b, "cdf1_large_fixed_not_last", 1, "only one large fixed-size");
  }

  // CDF-1: large fixed var + record var -> NC_EVARSIZE.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("lg");
    b.WriteU32(kCdf1LargeDim);
    b.WriteString("tm");
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t d0[1] = {0};
    const uint64_t d1[1] = {1};
    b.WriteSimpleVar("v0", d0, NC_INT, kCdf1LargeDim * 4, 256);
    b.WriteSimpleVar("r0", d1, NC_INT, 4, 256 + kCdf1LargeDim * 4);
    run_builder(b, "cdf1_large_fixed_with_rec", 1,
                "when there is no record variable");
  }

  // CDF-1: valid when single large record var is last.
  {
    CdfFileBuilder b(1);
    b.WriteU32(1);  // numrecs = 1
    b.WriteU32(kTagDimension);
    b.WriteU32(3);
    b.WriteString("tm");
    b.WriteU32(0);
    b.WriteString("sm");
    b.WriteU32(2);
    b.WriteString("lg");
    b.WriteU32(kCdf1LargeDim);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t r0d[2] = {0, 1};
    const uint64_t r1d[2] = {0, 2};
    b.WriteSimpleVar("r0", r0d, NC_INT, 8, 256);
    b.WriteSimpleVar("r1", r1d, NC_INT, kCdf1LargeDim * 4, 264);
    b.PadTo(300);
    run_builder(b, "cdf1_valid_last_large_rec", 0, "Warning:");
  }

  // CDF-1: two large record vars -> NC_EVARSIZE.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("tm");
    b.WriteU32(0);
    b.WriteString("lg");
    b.WriteU32(kCdf1LargeDim);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t rd[2] = {0, 1};
    b.WriteSimpleVar("r0", rd, NC_INT, kCdf1LargeDim * 4, 256);
    b.WriteSimpleVar("r1", rd, NC_INT, kCdf1LargeDim * 4,
                     256 + kCdf1LargeDim * 4);
    run_builder(b, "cdf1_two_large_rec", 1, "only one large record variable");
  }

  // CDF-1: large record var not last -> NC_EVARSIZE.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(3);
    b.WriteString("tm");
    b.WriteU32(0);
    b.WriteString("lg");
    b.WriteU32(kCdf1LargeDim);
    b.WriteString("sm");
    b.WriteU32(2);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t r0d[2] = {0, 1};
    const uint64_t r1d[2] = {0, 2};
    b.WriteSimpleVar("r0", r0d, NC_INT, kCdf1LargeDim * 4, 256);
    b.WriteSimpleVar("r1", r1d, NC_INT, 8, 256 + kCdf1LargeDim * 4);
    run_builder(b, "cdf1_large_rec_not_last", 1,
                "must be the last one defined");
  }

  // CDF-2: large fixed var (valid when last, error when two).
  {
    CdfFileBuilder b(2);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("lg");
    b.WriteU32(kCdf2LargeDim);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    const uint64_t d0[1] = {0};
    b.WriteSimpleVar("v0", d0, NC_INT, kCdf2LargeDim * 4, 256);
    b.PadTo(300);
    run_builder(b, "cdf2_valid_large_fixed", 0, "Warning:");
  }

  // CDF-5: NC_check_vlen() rejects fixed var exceeding INT64_MAX - 3 ->
  // NC_EVARSIZE. Use two wrapped dimensions of size (uint64_t)-2 with NC_BYTE
  // so product is 4 in var_shape64() (avoiding signed-overflow UB), while
  // NC_check_vlen() catches shape[1] (-2) > vlen_max / (-2).
  {
    constexpr uint64_t kWrappedNegTwo = 0xFFFFFFFFFFFFFFFEULL;
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(2);
    b.WriteString("h0");
    b.WriteU64(kWrappedNegTwo);
    b.WriteString("h1");
    b.WriteU64(kWrappedNegTwo);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU64(1);
    const uint64_t d0[2] = {0, 1};
    b.WriteSimpleVar("v0", d0, NC_BYTE, 4, 256);
    run_builder(b, "cdf5_huge_fixed", 1, "allowable by CDF-5");
  }

  // CDF-5: NC_check_vlen() rejects record var exceeding INT64_MAX - 3 ->
  // NC_EVARSIZE, using wrapped dims (uint64_t)-2 to avoid signed-overflow UB.
  {
    constexpr uint64_t kWrappedNegTwo = 0xFFFFFFFFFFFFFFFEULL;
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(3);
    b.WriteString("tm");
    b.WriteU64(0);
    b.WriteString("h0");
    b.WriteU64(kWrappedNegTwo);
    b.WriteString("h1");
    b.WriteU64(kWrappedNegTwo);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU64(1);
    const uint64_t d0[3] = {0, 1, 2};
    b.WriteSimpleVar("r0", d0, NC_BYTE, 4, 256);
    run_builder(b, "cdf5_huge_rec", 1, "allowable by CDF-5");
  }

  // compute_var_shape: begin_var <= 0 (begin == 0).
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("d0");
    b.WriteU32(2);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    const uint64_t d0[1] = {0};
    b.WriteSimpleVar("v0", d0, NC_INT, 8, 0);
    run_builder(b, "begin_var_zero", 1, "is negative");
  }

  // compute_var_shape: xsz > begin_var (0 < begin < header size).
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("d0");
    b.WriteU32(2);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    const uint64_t d0[1] = {0};
    b.WriteSimpleVar("v0", d0, NC_INT, 8, 4);
    run_builder(b, "begin_var_inside_header", 1,
                "larger than the begin of data section");
  }

  // compute_var_shape: begin_rec > first_rec->begin.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("tm");
    b.WriteU32(0);
    b.WriteString("d0");
    b.WriteU32(2);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t df[1] = {1};
    const uint64_t dr[1] = {0};
    b.WriteSimpleVar("v0", df, NC_INT, 8, 256);
    b.WriteSimpleVar("r0", dr, NC_INT, 4, 258);  // < 256 + 8
    run_builder(b, "rec_overlaps_fixed", 1,
                "greater than the begin of first record");
  }

  // compute_var_shape: begin_rec > first_rec->begin with no fixed variables.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("tm");
    b.WriteU32(0);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(1);
    const uint64_t dr[1] = {0};
    b.WriteSimpleVar("r0", dr, NC_INT, 4, 0);
    run_builder(b, "rec_only_begin_zero", 1,
                "greater than the begin of first record");
  }

  // compute_var_shape: begin_rec <= 0 (CDF-2 second fixed var has negative
  // 64-bit begin).
  {
    CdfFileBuilder b(2);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("d0");
    b.WriteU32(1);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t d0[1] = {0};
    b.WriteSimpleVar("v0", d0, NC_INT, 4, 1000);
    b.WriteSimpleVar("v1", d0, NC_INT, 4, 0xFFFFFFFFFFFFFF00ULL);
    run_builder(b, "begin_rec_negative", 1, "is zero or negative");
  }

  // compute_var_shape: begin_var > begin_rec (CDF-1 fixed var 0 > var 1 end).
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("d0");
    b.WriteU32(1);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t d0[1] = {0};
    b.WriteSimpleVar("v0", d0, NC_INT, 4, 2000);
    b.WriteSimpleVar("v1", d0, NC_INT, 4, 1000);
    run_builder(b, "begin_var_gt_begin_rec", 1, "larger than record section");
  }

  // val_NC_check_voff: overlapping non-record variables.
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("d0");
    b.WriteU32(25);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(2);
    const uint64_t d0[1] = {0};
    b.WriteSimpleVar("v0", d0, NC_INT, 100, 200);
    b.WriteSimpleVar("v1", d0, NC_INT, 100, 250);  // < 200 + 100
    run_builder(b, "voff_fixed_overlap", 1, "less than previous variable");
  }

  // val_NC_check_voff: overlapping record variables (with a fixed var too).
  {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(2);
    b.WriteString("tm");
    b.WriteU32(0);
    b.WriteString("d0");
    b.WriteU32(4);
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU32(3);
    const uint64_t df[1] = {1};
    const uint64_t dr[2] = {0, 1};
    b.WriteSimpleVar("f0", df, NC_INT, 16, 300);
    b.WriteSimpleVar("r0", dr, NC_INT, 16, 320);
    b.WriteSimpleVar("r1", dr, NC_INT, 16, 328);  // < 320 + 16
    run_builder(b, "voff_rec_overlap", 1, "less than previous variable");
  }
}

TEST(NcvalidatorTest, ParsesMultiChunkHeadersExceedingOneMiB) {
  // Construct a valid CDF-5 header spanning >3 MiB to exercise val_fetch()
  // across chunk boundaries in:
  //   1) hdr_get_name() while reading a name straddling offset 1,048,576;
  //   2) val_get_NC_attrV() while reading an attribute value crossing
  //   2,097,152; 3) val_check_buffer() with non-zero slack (slack == 4) at
  //   offset
  //      3,145,724 when reading var_list tag.
  TempFile large_hdr("multi_chunk_hdr.nc");
  CdfFileBuilder b(5);
  b.WriteU64(0);        // numrecs (offset 4..12)
  b.WriteAbsentList();  // dim_list (offset 12..24)
  b.WriteU32(kTagAttribute);
  b.WriteU64(4);  // 4 global attrs (offset 24..36)

  // Attr 0: 1,048,524 bytes -> ends at offset 1,048,560 (16 bytes before 1
  // MiB).
  b.WriteSimpleAttr("a", NC_BYTE, 1048500, 1);
  ASSERT_EQ(1048560U, b.size());

  // Attr 1: name_len(8) reaches 1,048,568; 15-byte name straddles 1,048,576;
  // value crosses 2,097,152 -> ends at 2,097,156.
  b.WriteSimpleAttr("0123456789abcde", NC_BYTE, 1048560, 1);
  ASSERT_EQ(2097156U, b.size());

  // Attr 2: ends at 3,145,700.
  b.WriteSimpleAttr("b", NC_BYTE, 1048520, 1);
  ASSERT_EQ(3145700U, b.size());

  // Attr 3: 0-element attr ending at 3,145,724 (4 bytes before 3 MiB).
  b.WriteSimpleAttr("c", NC_BYTE, 0, 1);
  ASSERT_EQ(3145724U, b.size());

  // var_list tag checks 8 bytes in CDF-5 -> triggers val_fetch with slack = 4.
  b.WriteAbsentList();
  ASSERT_EQ(3145736U, b.size());
  b.WriteToFile(large_hdr);

  const std::string path = large_hdr.string();
  const std::string_view args[] = {"ncvalidator", path};
  const CliResult res = RunValidator(args);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stdout_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("is a valid NetCDF classic file"));
}

#if defined(__has_feature)
#define NCVALIDATOR_TEST_HAS_FEATURE(x) __has_feature(x)
#else
#define NCVALIDATOR_TEST_HAS_FEATURE(x) 0
#endif

#if !defined(ADDRESS_SANITIZER) && !defined(MEMORY_SANITIZER) &&       \
    !defined(THREAD_SANITIZER) && !defined(HWADDRESS_SANITIZER) &&     \
    !defined(LEAK_SANITIZER) && !defined(DATAFLOW_SANITIZER) &&        \
    !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !NCVALIDATOR_TEST_HAS_FEATURE(address_sanitizer) &&                \
    !NCVALIDATOR_TEST_HAS_FEATURE(memory_sanitizer) &&                 \
    !NCVALIDATOR_TEST_HAS_FEATURE(thread_sanitizer) &&                 \
    !NCVALIDATOR_TEST_HAS_FEATURE(hwaddress_sanitizer) &&              \
    !NCVALIDATOR_TEST_HAS_FEATURE(leak_sanitizer) &&                   \
    !NCVALIDATOR_TEST_HAS_FEATURE(dataflow_sanitizer)
// Sanitizer allocators abort on allocations exceeding allocator limits rather
// than returning NULL; run OOM error-path checks only under unsanitized builds
// (including coverage instrumentation).
TEST(NcvalidatorTest, HandlesOutOfMemoryOnExtremeCdf5Lengths) {
  constexpr uint64_t kTooHuge = 1ULL << 60;

  // 1. Huge dimension name length -> hdr_get_name returns NC_ENOMEM.
  {
    TempFile f("oom_dim_name.nc");
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(1);
    b.WriteU64(kTooHuge);
    b.WriteToFile(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
  }

  // 2. Huge attribute name length -> val_get_NC_attr returns NC_ENOMEM.
  {
    TempFile f("oom_attr_name.nc");
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU64(1);
    b.WriteU64(kTooHuge);
    b.WriteToFile(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
  }

  // 3. Huge attribute value nelems -> new_NC_attr returns NC_ENOMEM.
  {
    TempFile f("oom_attr_val.nc");
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU64(1);
    b.WriteString("big");
    b.WriteU32(NC_BYTE);
    b.WriteU64(kTooHuge);
    b.WriteToFile(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
  }

  // 4. Huge variable name length -> val_get_NC_var returns NC_ENOMEM.
  {
    TempFile f("oom_var_name.nc");
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteU32(kTagVariable);
    b.WriteU64(1);
    b.WriteU64(kTooHuge);
    b.WriteToFile(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", p};
    const CliResult res = RunValidator(args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
  }
}
#endif  // !sanitizer

TEST(NcvalidatorTest, HandlesRepairSyscallFailures) {
  if (!ncvalidator_shim_is_active()) {
    GTEST_SKIP() << "Syscall fault-injection shim disabled under sanitizers.";
  }

  auto build_repair_candidate = [](const TempFile& file) {
    CdfFileBuilder b(1);
    b.WriteU32(0);
    b.WriteU32(kTagDimension);
    b.WriteU32(1);
    b.WriteString("a", 0x5A);  // 3-byte non-null padding at offset 21
    b.WriteU32(2);
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.WriteToFile(file);
  };

  {
    TempFile f("repair_lseek_fail.nc");
    build_repair_candidate(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", "-x", p};
    const CliResult res =
        RunValidatorArmed(ncvalidator_shim_arm_repair_lseek_fail, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("lseek"));
  }

  {
    TempFile f("repair_write_fail.nc");
    build_repair_candidate(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", "-x", p};
    const CliResult res =
        RunValidatorArmed(ncvalidator_shim_arm_repair_write_fail, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("write"));
  }

  {
    TempFile f("repair_write_short.nc");
    build_repair_candidate(f);
    const std::string p = f.string();
    const std::string_view args[] = {"ncvalidator", "-x", p};
    const CliResult res =
        RunValidatorArmed(ncvalidator_shim_arm_repair_write_short, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr("bytes but only"));
  }
}

TEST(NcvalidatorTest, HandlesChunkFetchErrorsAcrossHeaderSections) {
  if (!ncvalidator_shim_is_active()) {
    GTEST_SKIP() << "Syscall fault-injection shim disabled under sanitizers.";
  }

  auto expect_fetch_fail = [](const CdfFileBuilder& b, const char* name,
                              auto arm_fn, std::string_view expected_stdout,
                              std::string_view expected_stderr,
                              bool trace_mode = false) {
    SCOPED_TRACE(name);
    TempFile f(std::string("fetch_") + name + ".nc");
    b.WriteToFile(f);
    const std::string p = f.string();
    std::vector<std::string_view> args = {"ncvalidator"};
    if (trace_mode) {
      args.push_back("-t");
    }
    args.push_back(p);
    const CliResult res = RunValidatorArmed(arm_fn, args);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stdout_output, HasSubstr(expected_stdout));
    EXPECT_THAT(res.stderr_output, HasSubstr(expected_stderr));
  };

  auto build_before_var_list = [](size_t var_list_offset) {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(1);
    b.WriteString("d0");
    b.WriteU64(4);
    b.WriteU32(kTagAttribute);
    b.WriteU64(1);
    // Prefix: magic(4) + numrecs(8) + dim_list(32) + gatt header/meta(36) = 80.
    constexpr size_t kFixedPrefix = 80U;
    EXPECT_GE(var_list_offset, kFixedPrefix);
    b.WriteSimpleAttr("a", NC_BYTE, var_list_offset - kFixedPrefix, 1);
    EXPECT_EQ(var_list_offset, b.size());
    return b;
  };

  // 1. dim 1 name length (nchars) straddling 1 MiB.
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(2);
    b.WriteString(std::string(kDefaultChunkSize - 4U - 40U, 'd'));
    b.WriteU64(4);
    ASSERT_EQ(kDefaultChunkSize - 4U, b.size());
    b.WriteString("d1");
    b.WriteU64(4);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "dim1_nchars", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to read name string length", "read");
  }

  // 2. dim 1 name bytes crossing 1 MiB inside hdr_get_name() loop.
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(2);
    b.WriteString(std::string(kDefaultChunkSize - 16U - 40U, 'd'));
    b.WriteU64(4);
    ASSERT_EQ(kDefaultChunkSize - 16U, b.size());
    b.WriteString("0123456789abcdefghij");
    b.WriteU64(4);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "dim1_name_bytes", []() { ncvalidator_shim_arm_read_fail(1); },
        "fetching name string", "read");
  }

  // 3. dim 0 dim_length straddling 1 MiB.
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(1);
    b.WriteString(std::string(kDefaultChunkSize - 4U - 32U, 'd'));
    ASSERT_EQ(kDefaultChunkSize - 4U, b.size());
    b.WriteU64(4);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "dim0_length", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to read dimension size", "read");
  }

  // 4. gatt_list component tag straddling 1 MiB (via chunk lseek failure).
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(1);
    b.WriteString(std::string(kDefaultChunkSize - 4U - 40U, 'd'));
    b.WriteU64(4);
    ASSERT_EQ(kDefaultChunkSize - 4U, b.size());
    b.WriteAbsentList();
    b.WriteAbsentList();
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "gatt_tag_lseek", []() { ncvalidator_shim_arm_lseek_chunk_fail(0); },
        "Fail to read NC component tag", "lseek");
  }

  // 5. gatt_list nelems straddling 1 MiB.
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteU32(kTagDimension);
    b.WriteU64(1);
    b.WriteString(std::string(kDefaultChunkSize - 8U - 40U, 'd'));
    b.WriteU64(4);
    ASSERT_EQ(kDefaultChunkSize - 8U, b.size());
    b.WriteAbsentList();
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "gatt_nelems", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to read tag NC_ATTRIBUTE", "read");
  }

  // 6. Global attribute xtype starting right at 1 MiB boundary.
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU64(1);
    b.WriteString(std::string(kDefaultChunkSize - 44U, 'a'));
    ASSERT_EQ(kDefaultChunkSize, b.size());
    b.WriteU32(NC_BYTE);
    b.WriteU64(0);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "attr_xtype", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to read NC data type", "read");
  }

  // 7. Global attribute nelems straddling 1 MiB.
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU64(1);
    b.WriteString(std::string(kDefaultChunkSize - 4U - 48U, 'a'));
    b.WriteU32(NC_BYTE);
    ASSERT_EQ(kDefaultChunkSize - 4U, b.size());
    b.WriteU64(0);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "attr_nelems", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to read attribute length", "read");
  }

  // 8. Global attribute values crossing 1 MiB inside val_get_NC_attrV().
  {
    CdfFileBuilder b(5);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteU32(kTagAttribute);
    b.WriteU64(1);
    b.WriteString("a");
    b.WriteU32(NC_BYTE);
    b.WriteU64(kDefaultChunkSize + 4U - 60U);
    ASSERT_EQ(60U, b.size());
    b.WriteBytes(kDefaultChunkSize + 4U - 60U, 0x2A);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "attr_values", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to fetch next chunk into a buffer", "read");
  }

  // 9. var_list nelems straddling 1 MiB.
  {
    CdfFileBuilder b = build_before_var_list(kDefaultChunkSize - 8U);
    b.WriteAbsentList();
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "var_list_nelems", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to read tag NC_VARIABLE", "read");
  }

  // 10. Variable name bytes crossing 1 MiB.
  {
    CdfFileBuilder b = build_before_var_list(kDefaultChunkSize - 16U - 12U);
    b.WriteU32(kTagVariable);
    b.WriteU64(1);
    ASSERT_EQ(kDefaultChunkSize - 16U, b.size());
    b.WriteString("0123456789abcdefghij");
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "var_name_bytes", []() { ncvalidator_shim_arm_read_fail(1); },
        "fetching name string", "read");
  }

  // 11. Variable ndims straddling 1 MiB.
  {
    CdfFileBuilder b = build_before_var_list(kDefaultChunkSize - 4U - 24U);
    b.WriteU32(kTagVariable);
    b.WriteU64(1);
    b.WriteString("v");
    ASSERT_EQ(kDefaultChunkSize - 4U, b.size());
    b.WriteU64(1);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "var_ndims", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to read number of dimensions", "read");
  }

  // 12. Variable dimid[0] straddling 1 MiB (with trace mode enabled).
  {
    CdfFileBuilder b = build_before_var_list(kDefaultChunkSize - 4U - 32U);
    b.WriteU32(kTagVariable);
    b.WriteU64(1);
    b.WriteString("v");
    b.WriteU64(1);
    ASSERT_EQ(kDefaultChunkSize - 4U, b.size());
    b.WriteU64(0);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "var_dimid_trace", []() { ncvalidator_shim_arm_read_fail(1); },
        "Fail to read dimid[0]", "read", /*trace_mode=*/true);
  }

  // 13. Variable vsize straddling 1 MiB.
  {
    CdfFileBuilder b = build_before_var_list(kDefaultChunkSize - 4U - 56U);
    b.WriteU32(kTagVariable);
    b.WriteU64(1);
    b.WriteString("v");
    b.WriteU64(1);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteU32(NC_INT);
    ASSERT_EQ(kDefaultChunkSize - 4U, b.size());
    b.WriteU64(16);
    b.PadTo(kDefaultChunkSize + 64U);
    expect_fetch_fail(
        b, "var_vsize", []() { ncvalidator_shim_arm_read_fail(1); },
        "Failed to read vsize", "read");
  }

  // 14. Variable begin straddling 1 MiB.
  {
    CdfFileBuilder b = build_before_var_list(kDefaultChunkSize - 4U - 64U);
    b.WriteU32(kTagVariable);
    b.WriteU64(1);
    b.WriteString("v");
    b.WriteU64(1);
    b.WriteU64(0);
    b.WriteAbsentList();
    b.WriteU32(NC_INT);
    b.WriteU64(16);
    ASSERT_EQ(kDefaultChunkSize - 4U, b.size());
    b.WriteOffset(kDefaultChunkSize + 64U);
    b.PadTo(kDefaultChunkSize + 128U);
    expect_fetch_fail(
        b, "var_begin", []() { ncvalidator_shim_arm_read_fail(1); },
        "Fail to read begin", "read");
  }
}

}  // namespace
}  // namespace netcdf
