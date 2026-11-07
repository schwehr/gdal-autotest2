// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests NASA CDF file reading and type unmapping (mfhdf/src/nssdc.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/nssdc.c

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"

// clang-format off
#include "third_party/hdf4/mfhdf/src/nc_priv.h"
#include "third_party/hdf4/mfhdf/src/hdf_xdr_priv.h"
// clang-format on

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

void CleanupCdfHandleResources(NC* handle) {
  if (handle == nullptr) return;
  if (handle->vars != nullptr && handle->vars->values != nullptr) {
    NC_var** vpp = reinterpret_cast<NC_var**>(handle->vars->values);
    for (unsigned i = 0; i < handle->vars->count; ++i) {
      if (vpp[i] != nullptr) {
        vix_t* cur = vpp[i]->vixHead;
        while (cur != nullptr) {
          vix_t* next = cur->next;
          std::free(cur);
          cur = next;
        }
        vpp[i]->vixHead = nullptr;
      }
    }
  }
  if (handle->cdf_fp != nullptr) {
    HI_CLOSE(handle->cdf_fp);
    handle->cdf_fp = nullptr;
  }
}

class NssdcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    ncopts = 0;
    ncerr = NC_NOERR;
  }

  void TearDown() override {
    SDPfreebuf();
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
    HEclear();
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }

  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

class CdfBufferBuilder {
 public:
  size_t Tell() const { return buf_.size(); }

  void WriteI32(int32_t val) {
    buf_.push_back(static_cast<uint8_t>((val >> 24) & 0xff));
    buf_.push_back(static_cast<uint8_t>((val >> 16) & 0xff));
    buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xff));
    buf_.push_back(static_cast<uint8_t>(val & 0xff));
  }

  void WriteBytes(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + len);
  }

  void PatchI32(size_t offset, int32_t val) {
    buf_[offset] = static_cast<uint8_t>((val >> 24) & 0xff);
    buf_[offset + 1] = static_cast<uint8_t>((val >> 16) & 0xff);
    buf_[offset + 2] = static_cast<uint8_t>((val >> 8) & 0xff);
    buf_[offset + 3] = static_cast<uint8_t>(val & 0xff);
  }

  void WritePad64(std::string_view name, char pad_char = '\0') {
    std::vector<char> p(64, pad_char);
    size_t copy_len = std::min(name.size(), static_cast<size_t>(64));
    std::memcpy(p.data(), name.data(), copy_len);
    WriteBytes(p.data(), 64);
  }

  void WriteToFile(const std::string& path) const {
    const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd != -1) {
      (void)write(fd, buf_.data(), buf_.size());
      close(fd);
    }
  }

  const std::vector<uint8_t>& Buffer() const { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

struct TypeMappingTestCase {
  int cdf_type;
  nc_type expected_nc_type;
};

constexpr TypeMappingTestCase kTypeCases[] = {
    {.cdf_type = 1, .expected_nc_type = NC_BYTE},           // CDF_INT1
    {.cdf_type = 2, .expected_nc_type = NC_SHORT},          // CDF_INT2
    {.cdf_type = 4, .expected_nc_type = NC_LONG},           // CDF_INT4
    {.cdf_type = 11, .expected_nc_type = NC_BYTE},          // CDF_UINT1
    {.cdf_type = 12, .expected_nc_type = NC_SHORT},         // CDF_UINT2
    {.cdf_type = 14, .expected_nc_type = NC_LONG},          // CDF_UINT4
    {.cdf_type = 21, .expected_nc_type = NC_FLOAT},         // CDF_REAL4
    {.cdf_type = 22, .expected_nc_type = NC_DOUBLE},        // CDF_REAL8
    {.cdf_type = 31, .expected_nc_type = NC_DOUBLE},        // CDF_EPOCH
    {.cdf_type = 41, .expected_nc_type = NC_BYTE},          // CDF_BYTE
    {.cdf_type = 44, .expected_nc_type = NC_FLOAT},         // CDF_FLOAT
    {.cdf_type = 45, .expected_nc_type = NC_DOUBLE},        // CDF_DOUBLE
    {.cdf_type = 51, .expected_nc_type = NC_CHAR},          // CDF_CHAR
    {.cdf_type = 52, .expected_nc_type = NC_CHAR},          // CDF_UCHAR
    {.cdf_type = 0x100 | 51, .expected_nc_type = NC_CHAR},  // Masking 0xff
    {.cdf_type = 99, .expected_nc_type = static_cast<nc_type>(FAIL)},
};

TEST_F(NssdcTest, CdfUnmapTypeAllMappings) {
  for (const auto& tc : kTypeCases) {
    EXPECT_THAT(cdf_unmap_type(tc.cdf_type), Eq(tc.expected_nc_type));
  }
}

TEST_F(NssdcTest, NssdcWriteAndXdrDispatch) {
  EXPECT_THAT(nssdc_write_cdf(nullptr, nullptr), Eq(FALSE));

  XDR xdrs{};
  NC* dummy = nullptr;

  xdrs.x_op = XDR_ENCODE;
  EXPECT_THAT(nssdc_xdr_cdf(&xdrs, &dummy), Eq(FALSE));

  constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
  xdrs.x_op = kBogusXdrOp;
  EXPECT_THAT(nssdc_xdr_cdf(&xdrs, &dummy), Eq(TRUE));

  // XDR_FREE frees handle via NC_free_cdf
  NC* free_target = static_cast<NC*>(std::calloc(1, sizeof(NC)));
  ASSERT_THAT(free_target, NotNull());
  free_target->xdrs = static_cast<XDR*>(std::calloc(1, sizeof(XDR)));
  ASSERT_THAT(free_target->xdrs, NotNull());
  free_target->file_type = CDF_FILE;
  xdrs.x_op = XDR_FREE;
  EXPECT_THAT(nssdc_xdr_cdf(&xdrs, &free_target), Eq(TRUE));
}

TEST_F(NssdcTest, ReadRichSynthesizedCdfFileViaNcOpen) {
  const std::string file_path = GetTempFilePath("rich_test.cdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  CdfBufferBuilder b;
  // 1. Magic (8 bytes)
  b.WriteI32(0x0000FFFF);
  b.WriteI32(0x0000FFFF);

  // 2. CDR (48 bytes, offset 8..55)
  const size_t gdr_offset = 56;
  b.WriteI32(48);  // record size
  b.WriteI32(1);   // record type (CDR)
  b.WriteI32(static_cast<int32_t>(gdr_offset));
  b.WriteI32(2);  // vers
  b.WriteI32(5);  // release
  b.WriteI32(1);  // encoding (NETWORK_ENCODING)
  b.WriteI32(2);  // flags (1 << CDF_FORMAT_BIT)
  for (int i = 0; i < 5; ++i) b.WriteI32(0);

  // 3. GDR (68 bytes, offset 56..123)
  ASSERT_THAT(b.Tell(), Eq(gdr_offset));
  b.WriteI32(68);  // size (60 + 2*4)
  b.WriteI32(2);   // GDR
  const size_t gdr_varnext_pos = b.Tell();
  b.WriteI32(0);
  const size_t gdr_zvarnext_pos = b.Tell();
  b.WriteI32(0);
  const size_t gdr_adrnext_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(0);  // eof
  b.WriteI32(4);  // numRVars
  b.WriteI32(4);  // numAttrs
  b.WriteI32(0);  // maxRec
  b.WriteI32(2);  // numDims
  b.WriteI32(4);  // numZVars
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteI32(3);  // dim 0 size
  b.WriteI32(4);  // dim 1 size

  // 4. VXRs for rvar0: vxr0_1 then vxr0_0
  const size_t vxr0_1_offset = b.Tell();
  b.WriteI32(32);    // 20 + 12*1
  b.WriteI32(6);     // VXR
  b.WriteI32(0);     // vxrNext
  b.WriteI32(1);     // nEntries
  b.WriteI32(1);     // nUsed
  b.WriteI32(2);     // firstRec
  b.WriteI32(3);     // lastRec
  b.WriteI32(2048);  // offset

  const size_t vxr0_0_offset = b.Tell();
  b.WriteI32(44);  // 20 + 12*2
  b.WriteI32(6);   // VXR
  b.WriteI32(static_cast<int32_t>(vxr0_1_offset));
  b.WriteI32(2);  // nEntries
  b.WriteI32(2);  // nUsed
  b.WriteI32(0);
  b.WriteI32(1);  // firstRec
  b.WriteI32(0);
  b.WriteI32(1);  // lastRec
  b.WriteI32(1024);
  b.WriteI32(1028);  // offset

  // 5. rVars (4 vars)
  const size_t rvar0_offset = b.Tell();
  b.PatchI32(gdr_varnext_pos, static_cast<int32_t>(rvar0_offset));
  b.WriteI32(264);
  b.WriteI32(3);  // VDR
  const size_t rvar0_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(11);  // nt = CDF_UINT1 (DFNT_UINT8)
  b.WriteI32(4);   // vMaxRec
  b.WriteI32(static_cast<int32_t>(vxr0_0_offset));
  b.WriteI32(0);  // VXRtail
  b.WriteI32(3);  // vFlags = RECVARY | PADVALUE
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  const std::vector<char> rfu128(128, 0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(2);  // numElem = 2
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WritePad64("rvar0", ' ');  // space-padded
  b.WriteI32(1);
  b.WriteI32(0);  // variances

  const size_t rvar1_offset = b.Tell();
  b.PatchI32(rvar0_next_pos, static_cast<int32_t>(rvar1_offset));
  b.WriteI32(264);
  b.WriteI32(3);
  const size_t rvar1_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(12);  // nt = CDF_UINT2 (DFNT_UINT16)
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(1);  // vFlags = RECVARY (numElem=1, maxRec=0 -> NC_UNLIMITED)
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(1);
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WritePad64("rvar1");
  b.WriteI32(1);
  b.WriteI32(1);  // variances

  const size_t rvar2_offset = b.Tell();
  b.PatchI32(rvar1_next_pos, static_cast<int32_t>(rvar2_offset));
  b.WriteI32(264);
  b.WriteI32(3);
  const size_t rvar2_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(14);  // nt = CDF_UINT4 (DFNT_UINT32)
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(1);
  b.WriteI32(2);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WritePad64("rvar2");
  b.WriteI32(0);
  b.WriteI32(1);

  const size_t rvar3_offset = b.Tell();
  b.PatchI32(rvar2_next_pos, static_cast<int32_t>(rvar3_offset));
  b.WriteI32(264);
  b.WriteI32(3);
  b.WriteI32(0);   // varNext = 0 (last rVar)
  b.WriteI32(21);  // nt = CDF_REAL4
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(1);
  b.WriteI32(3);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WritePad64("rvar3");
  b.WriteI32(0);
  b.WriteI32(0);

  // 6. VXR for zvar0
  const size_t vxr_z0_offset = b.Tell();
  b.WriteI32(32);
  b.WriteI32(6);
  b.WriteI32(0);
  b.WriteI32(1);
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(4096);

  // 7. zVars (4 vars)
  const size_t zvar0_offset = b.Tell();
  b.PatchI32(gdr_zvarnext_pos, static_cast<int32_t>(zvar0_offset));
  // zvar0: 256 + 4 + 8*2 = 276 bytes
  b.WriteI32(276);
  b.WriteI32(3);
  const size_t zvar0_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(11);  // nt = CDF_UINT1
  b.WriteI32(0);
  b.WriteI32(static_cast<int32_t>(vxr_z0_offset));
  b.WriteI32(0);
  b.WriteI32(0);
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(1);
  b.WriteI32(4);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WritePad64("zvar0", ' ');
  b.WriteI32(2);  // zNumDims = 2
  b.WriteI32(5);
  b.WriteI32(6);  // dim sizes
  b.WriteI32(1);
  b.WriteI32(0);  // variances

  const size_t zvar1_offset = b.Tell();
  b.PatchI32(zvar0_next_pos, static_cast<int32_t>(zvar1_offset));
  // zvar1: 256 + 4 + 8*1 = 268 bytes
  b.WriteI32(268);
  b.WriteI32(3);
  const size_t zvar1_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(12);  // nt = CDF_UINT2
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(1);
  b.WriteI32(5);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WritePad64("zvar1");
  b.WriteI32(1);  // zNumDims = 1
  b.WriteI32(4);  // dim size 4
  b.WriteI32(1);  // variance 1

  const size_t zvar2_offset = b.Tell();
  b.PatchI32(zvar1_next_pos, static_cast<int32_t>(zvar2_offset));
  b.WriteI32(268);
  b.WriteI32(3);
  const size_t zvar2_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(14);  // nt = CDF_UINT4
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(1);
  b.WriteI32(6);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WritePad64("zvar2");
  b.WriteI32(1);  // zNumDims = 1
  b.WriteI32(2);  // dim size 2
  b.WriteI32(0);  // variance 0

  const size_t zvar3_offset = b.Tell();
  b.PatchI32(zvar2_next_pos, static_cast<int32_t>(zvar3_offset));
  // zvar3: 256 + 4 = 260 bytes (0 dims)
  b.WriteI32(260);
  b.WriteI32(3);
  b.WriteI32(0);   // zVarNext = 0
  b.WriteI32(44);  // nt = CDF_FLOAT
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(1);
  b.WriteI32(7);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WritePad64("zvar3");
  b.WriteI32(0);  // zNumDims = 0

  // 8. Attributes & Entries
  // AEDR for adr0 (global scope 1): aedr0_0 -> aedr0_1
  // nt = 4 (CDF_INT4), count = 2 -> bsize = 8 -> record size = 48 + 8 = 56
  const size_t aedr0_1_offset = b.Tell();
  b.WriteI32(52);
  b.WriteI32(5);
  b.WriteI32(0);  // size=52 (48+4), type=5, next=0
  b.WriteI32(1);
  b.WriteI32(4);
  b.WriteI32(0);
  b.WriteI32(1);  // attr=1, nt=4, var=0, count=1
  for (int i = 0; i < 5; ++i) b.WriteI32(0);
  b.WriteI32(200);  // 1 int value

  const size_t aedr0_0_offset = b.Tell();
  b.WriteI32(56);
  b.WriteI32(5);
  b.WriteI32(static_cast<int32_t>(aedr0_1_offset));
  b.WriteI32(0);
  b.WriteI32(4);
  b.WriteI32(0);
  b.WriteI32(2);  // attr=0, nt=4, var=0, count=2
  for (int i = 0; i < 5; ++i) b.WriteI32(0);
  b.WriteI32(100);
  b.WriteI32(101);  // 2 int values

  // AEDZ for adr1 (global scope 1): aedz1_0 -> aedz1_1
  // nt = 22 (CDF_REAL8), count = 1 -> bsize = 8 -> record size = 56
  const size_t aedz1_1_offset = b.Tell();
  b.WriteI32(56);
  b.WriteI32(5);
  b.WriteI32(0);
  b.WriteI32(1);
  b.WriteI32(22);
  b.WriteI32(0);
  b.WriteI32(1);
  for (int i = 0; i < 5; ++i) b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);  // double

  const size_t aedz1_0_offset = b.Tell();
  b.WriteI32(56);
  b.WriteI32(5);
  b.WriteI32(static_cast<int32_t>(aedz1_1_offset));
  b.WriteI32(0);
  b.WriteI32(22);
  b.WriteI32(0);
  b.WriteI32(1);
  for (int i = 0; i < 5; ++i) b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);  // double

  // AEDR for adr2 (local scope 2, targeting rvar0 = var 0): aedr2_0 -> aedr2_1
  // nt = 51 (CDF_CHAR), count = 3 -> bsize = 3 -> record size = 48 + 3 = 51
  const size_t aedr2_1_offset = b.Tell();
  b.WriteI32(50);
  b.WriteI32(5);
  b.WriteI32(0);
  b.WriteI32(1);
  b.WriteI32(51);
  b.WriteI32(0);
  b.WriteI32(2);  // var 0, count=2
  for (int i = 0; i < 5; ++i) b.WriteI32(0);
  b.WriteBytes("ok", 2);

  const size_t aedr2_0_offset = b.Tell();
  b.WriteI32(51);
  b.WriteI32(5);
  b.WriteI32(static_cast<int32_t>(aedr2_1_offset));
  b.WriteI32(0);
  b.WriteI32(51);
  b.WriteI32(0);
  b.WriteI32(3);  // var 0, count=3
  for (int i = 0; i < 5; ++i) b.WriteI32(0);
  b.WriteBytes("xyz", 3);

  // AEDZ for adr3 (local scope 2, targeting zvar0 = var 4): aedz3_0 -> aedz3_1
  // nt = 2 (CDF_INT2), count = 2 -> bsize = 4 -> record size = 52
  const size_t aedz3_1_offset = b.Tell();
  b.WriteI32(50);
  b.WriteI32(5);
  b.WriteI32(0);
  b.WriteI32(1);
  b.WriteI32(2);
  b.WriteI32(4);
  b.WriteI32(1);  // var 4, count=1
  for (int i = 0; i < 5; ++i) b.WriteI32(0);
  b.WriteBytes("\x00\x05", 2);

  const size_t aedz3_0_offset = b.Tell();
  b.WriteI32(52);
  b.WriteI32(5);
  b.WriteI32(static_cast<int32_t>(aedz3_1_offset));
  b.WriteI32(0);
  b.WriteI32(2);
  b.WriteI32(4);
  b.WriteI32(2);  // var 4, count=2
  for (int i = 0; i < 5; ++i) b.WriteI32(0);
  b.WriteBytes("\x00\x01\x00\x02", 4);

  // Now 4 ADRs: adr0, adr1, adr2, adr3
  const size_t adr0_offset = b.Tell();
  b.PatchI32(gdr_adrnext_pos, static_cast<int32_t>(adr0_offset));
  b.WriteI32(116);
  b.WriteI32(4);
  const size_t adr0_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(static_cast<int32_t>(aedr0_0_offset));  // aedrNext
  b.WriteI32(1);                                     // scope = 1 (global)
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteI32(0);  // aedzNext = 0
  for (int i = 0; i < 3; ++i) b.WriteI32(0);
  b.WritePad64("g_aedr");

  const size_t adr1_offset = b.Tell();
  b.PatchI32(adr0_next_pos, static_cast<int32_t>(adr1_offset));
  b.WriteI32(116);
  b.WriteI32(4);
  const size_t adr1_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(0);  // aedrNext = 0
  b.WriteI32(1);  // scope = 1 (global)
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteI32(static_cast<int32_t>(aedz1_0_offset));  // aedzNext
  for (int i = 0; i < 3; ++i) b.WriteI32(0);
  b.WritePad64("g_aedz");

  const size_t adr2_offset = b.Tell();
  b.PatchI32(adr1_next_pos, static_cast<int32_t>(adr2_offset));
  b.WriteI32(116);
  b.WriteI32(4);
  const size_t adr2_next_pos = b.Tell();
  b.WriteI32(0);
  b.WriteI32(static_cast<int32_t>(aedr2_0_offset));  // aedrNext
  b.WriteI32(2);                                     // scope = 2 (local)
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteI32(0);  // aedzNext = 0
  for (int i = 0; i < 3; ++i) b.WriteI32(0);
  b.WritePad64("v_aedr");

  const size_t adr3_offset = b.Tell();
  b.PatchI32(adr2_next_pos, static_cast<int32_t>(adr3_offset));
  b.WriteI32(116);
  b.WriteI32(4);
  b.WriteI32(0);  // adrNext = 0 (last ADR)
  b.WriteI32(0);  // aedrNext = 0
  b.WriteI32(2);  // scope = 2 (local)
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteI32(static_cast<int32_t>(aedz3_0_offset));  // aedzNext
  for (int i = 0; i < 3; ++i) b.WriteI32(0);
  b.WritePad64("v_aedz");

  b.WriteToFile(file_path);

  const int cdfid = ncopen(file_path.c_str(), NC_NOWRITE);
  ASSERT_THAT(cdfid, Ge(0));
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());

  // Verify file type and structures
  EXPECT_THAT(handle->file_type, Eq(CDF_FILE));
  ASSERT_THAT(handle->dims, NotNull());
  EXPECT_THAT(handle->dims->count, Eq(6u));
  ASSERT_THAT(handle->vars, NotNull());
  EXPECT_THAT(handle->vars->count, Eq(8u));

  NC_var** vars = reinterpret_cast<NC_var**>(handle->vars->values);
  EXPECT_THAT(vars[0]->HDFtype, Eq(DFNT_UINT8));
  EXPECT_THAT(vars[1]->HDFtype, Eq(DFNT_UINT16));
  EXPECT_THAT(vars[2]->HDFtype, Eq(DFNT_UINT32));
  EXPECT_THAT(vars[4]->HDFtype, Eq(DFNT_UINT8));
  EXPECT_THAT(vars[5]->HDFtype, Eq(DFNT_UINT16));
  EXPECT_THAT(vars[6]->HDFtype, Eq(DFNT_UINT32));

  // Verify VXR chains
  ASSERT_THAT(vars[0]->vixHead, NotNull());
  EXPECT_THAT(vars[0]->vixHead->nEntries, Eq(2));
  ASSERT_THAT(vars[0]->vixHead->next, NotNull());
  EXPECT_THAT(vars[0]->vixHead->next->nEntries, Eq(1));
  ASSERT_THAT(vars[4]->vixHead, NotNull());
  EXPECT_THAT(vars[4]->vixHead->nEntries, Eq(1));

  // Verify attributes
  ASSERT_THAT(handle->attrs, NotNull());
  EXPECT_THAT(handle->attrs->count, Eq(4u));
  ASSERT_THAT(vars[0]->attrs, NotNull());
  EXPECT_THAT(vars[0]->attrs->count, Eq(2u));
  ASSERT_THAT(vars[4]->attrs, NotNull());
  EXPECT_THAT(vars[4]->attrs->count, Eq(2u));

  CleanupCdfHandleResources(handle);
  EXPECT_THAT(ncclose(cdfid), Eq(0));
}

TEST_F(NssdcTest, CompactCdfWithPositiveMaxRecAndEmptyCdf) {
  // 1. maxRec > 0 and SUN_ENCODING, plus global ADR with AEDZ only
  {
    const std::string file1 = GetTempFilePath("maxrec_pos.cdf");
    absl::Cleanup rem1 = [&file1] { std::filesystem::remove(file1); };

    CdfBufferBuilder b;
    b.WriteI32(0x0000FFFF);
    b.WriteI32(0x0000FFFF);  // Magic
    // CDR: encoding = 2 (SUN_ENCODING)
    b.WriteI32(48);
    b.WriteI32(1);
    b.WriteI32(56);
    b.WriteI32(2);
    b.WriteI32(5);
    b.WriteI32(2);
    b.WriteI32(2);
    for (int i = 0; i < 5; ++i) b.WriteI32(0);

    // GDR at 56
    b.WriteI32(64);
    b.WriteI32(2);
    const size_t vnext_pos = b.Tell();
    b.WriteI32(0);
    b.WriteI32(0);  // zVarNext = 0
    const size_t adr_pos = b.Tell();
    b.WriteI32(0);
    b.WriteI32(0);  // eof
    b.WriteI32(1);  // numRVars = 1
    b.WriteI32(1);  // numAttrs = 1
    b.WriteI32(5);  // maxRec = 5 (triggers nssdc.c else if (maxRec > 0) branch)
    b.WriteI32(1);  // numDims = 1
    b.WriteI32(0);  // numZVars = 0
    for (int i = 0; i < 4; ++i) b.WriteI32(0);
    b.WriteI32(4);  // dim 0 size

    // rVar 0
    const size_t rvar_off = b.Tell();
    b.PatchI32(vnext_pos, static_cast<int32_t>(rvar_off));
    b.WriteI32(260);
    b.WriteI32(3);
    b.WriteI32(0);
    b.WriteI32(1);  // nt = CDF_INT1
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(1);  // vFlags = 1 (RECVARY)
    for (int i = 0; i < 4; ++i) b.WriteI32(0);
    const std::vector<char> rfu128(128, 0);
    b.WriteBytes(rfu128.data(), 128);
    b.WriteI32(1);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WritePad64("r0");
    b.WriteI32(1);  // variance

    // AEDZ for adr0 (global scope 1, *ap == NULL in nssdc.c AEDZ loop)
    const size_t aedz_off = b.Tell();
    b.WriteI32(52);
    b.WriteI32(5);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(4);
    b.WriteI32(0);
    b.WriteI32(1);  // nt=4, count=1
    for (int i = 0; i < 5; ++i) b.WriteI32(0);
    b.WriteI32(999);

    // ADR 0
    const size_t adr_off = b.Tell();
    b.PatchI32(adr_pos, static_cast<int32_t>(adr_off));
    b.WriteI32(116);
    b.WriteI32(4);
    b.WriteI32(0);
    b.WriteI32(0);  // aedrNext = 0
    b.WriteI32(1);  // scope = 1 (global)
    for (int i = 0; i < 4; ++i) b.WriteI32(0);
    b.WriteI32(static_cast<int32_t>(aedz_off));  // aedzNext
    for (int i = 0; i < 3; ++i) b.WriteI32(0);
    b.WritePad64("g_aedz_only");

    b.WriteToFile(file1);

    const int id1 = ncopen(file1.c_str(), NC_NOWRITE);
    ASSERT_THAT(id1, Ge(0));
    NC* h1 = NC_check_id(id1);
    ASSERT_THAT(h1, NotNull());
    ASSERT_THAT(h1->dims, NotNull());
    EXPECT_THAT(h1->dims->count, Eq(2u));
    NC_dim** dims = reinterpret_cast<NC_dim**>(h1->dims->values);
    EXPECT_THAT(dims[1]->size, Eq(5));  // maxRec = 5
    ASSERT_THAT(h1->attrs, NotNull());
    EXPECT_THAT(h1->attrs->count, Eq(1u));

    CleanupCdfHandleResources(h1);
    EXPECT_THAT(ncclose(id1), Eq(0));
  }

  // 2. Empty CDF (numDims=0, varNext=0, zVarNext=0, numAttrs=0 covers
  // dims==NULL, vars==NULL)
  {
    const std::string file2 = GetTempFilePath("empty.cdf");
    absl::Cleanup rem2 = [&file2] { std::filesystem::remove(file2); };

    CdfBufferBuilder b;
    b.WriteI32(0x0000FFFF);
    b.WriteI32(0x0000FFFF);
    b.WriteI32(48);
    b.WriteI32(1);
    b.WriteI32(56);
    b.WriteI32(2);
    b.WriteI32(5);
    b.WriteI32(1);
    b.WriteI32(2);
    for (int i = 0; i < 5; ++i) b.WriteI32(0);

    // GDR at 56 (60 bytes, 0 dims)
    b.WriteI32(60);
    b.WriteI32(2);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    for (int i = 0; i < 4; ++i) b.WriteI32(0);

    b.WriteToFile(file2);

    const int id2 = ncopen(file2.c_str(), NC_NOWRITE);
    ASSERT_THAT(id2, Ge(0));
    NC* h2 = NC_check_id(id2);
    ASSERT_THAT(h2, NotNull());
    EXPECT_THAT(h2->dims, ::testing::IsNull());
    EXPECT_THAT(h2->vars, ::testing::IsNull());

    CleanupCdfHandleResources(h2);
    EXPECT_THAT(ncclose(id2), Eq(0));
  }
}

TEST_F(NssdcTest, CorruptAndUnsupportedCdfInputsFail) {
  // 1. Truncated CDR (< 56 bytes)
  {
    const std::string p = GetTempFilePath("trunc_cdr.cdf");
    absl::Cleanup rem = [&p] { std::filesystem::remove(p); };
    CdfBufferBuilder b;
    b.WriteI32(0x0000FFFF);
    b.WriteI32(0x0000FFFF);
    b.WriteI32(20);  // Short file
    b.WriteToFile(p);

    NC nc{};
    nc.cdf_fp = reinterpret_cast<hdf_file_t>(HI_OPEN(p.c_str(), DFACC_READ));
    ASSERT_THAT(nc.cdf_fp, NotNull());
    absl::Cleanup close_fp = [&nc] {
      if (nc.cdf_fp) HI_CLOSE(nc.cdf_fp);
    };

    XDR xdrs{};
    NC* pnc = &nc;
    EXPECT_THAT(nssdc_read_cdf(&xdrs, &pnc), Eq(FALSE));
  }

  // 2. Multi-file CDF (flags = 0, bit 1 not set)
  {
    const std::string p = GetTempFilePath("multifile.cdf");
    absl::Cleanup rem = [&p] { std::filesystem::remove(p); };
    CdfBufferBuilder b;
    b.WriteI32(0x0000FFFF);
    b.WriteI32(0x0000FFFF);
    b.WriteI32(48);
    b.WriteI32(1);
    b.WriteI32(56);
    b.WriteI32(2);
    b.WriteI32(5);
    b.WriteI32(1);
    b.WriteI32(0);  // flags = 0 -> !bitset(flags, CDF_FORMAT_BIT)
    for (int i = 0; i < 5; ++i) b.WriteI32(0);
    b.WriteToFile(p);

    NC nc{};
    nc.cdf_fp = reinterpret_cast<hdf_file_t>(HI_OPEN(p.c_str(), DFACC_READ));
    ASSERT_THAT(nc.cdf_fp, NotNull());
    absl::Cleanup close_fp = [&nc] {
      if (nc.cdf_fp) HI_CLOSE(nc.cdf_fp);
    };

    XDR xdrs{};
    NC* pnc = &nc;
    EXPECT_THAT(nssdc_read_cdf(&xdrs, &pnc), Eq(FALSE));
  }

  // 3. Unsupported encoding (IBMPC_ENCODING = 6)
  {
    const std::string p = GetTempFilePath("ibmpc.cdf");
    absl::Cleanup rem = [&p] { std::filesystem::remove(p); };
    CdfBufferBuilder b;
    b.WriteI32(0x0000FFFF);
    b.WriteI32(0x0000FFFF);
    b.WriteI32(48);
    b.WriteI32(1);
    b.WriteI32(56);
    b.WriteI32(2);
    b.WriteI32(5);
    b.WriteI32(6);  // IBMPC_ENCODING
    b.WriteI32(2);  // flags = 2
    for (int i = 0; i < 5; ++i) b.WriteI32(0);
    b.WriteToFile(p);

    NC nc{};
    nc.cdf_fp = reinterpret_cast<hdf_file_t>(HI_OPEN(p.c_str(), DFACC_READ));
    ASSERT_THAT(nc.cdf_fp, NotNull());
    absl::Cleanup close_fp = [&nc] {
      if (nc.cdf_fp) HI_CLOSE(nc.cdf_fp);
    };

    XDR xdrs{};
    NC* pnc = &nc;
    EXPECT_THAT(nssdc_read_cdf(&xdrs, &pnc), Eq(FALSE));
  }

  // 4. Truncated GDR (< 15 int32s)
  {
    const std::string p = GetTempFilePath("trunc_gdr.cdf");
    absl::Cleanup rem = [&p] { std::filesystem::remove(p); };
    CdfBufferBuilder b;
    b.WriteI32(0x0000FFFF);
    b.WriteI32(0x0000FFFF);
    b.WriteI32(48);
    b.WriteI32(1);
    b.WriteI32(56);
    b.WriteI32(2);
    b.WriteI32(5);
    b.WriteI32(1);
    b.WriteI32(2);
    for (int i = 0; i < 5; ++i) b.WriteI32(0);
    b.WriteI32(20);  // GDR too short
    b.WriteToFile(p);

    NC nc{};
    nc.cdf_fp = reinterpret_cast<hdf_file_t>(HI_OPEN(p.c_str(), DFACC_READ));
    ASSERT_THAT(nc.cdf_fp, NotNull());
    absl::Cleanup close_fp = [&nc] {
      if (nc.cdf_fp) HI_CLOSE(nc.cdf_fp);
    };

    XDR xdrs{};
    NC* pnc = &nc;
    EXPECT_THAT(nssdc_read_cdf(&xdrs, &pnc), Eq(FALSE));
  }

  // 5. numDims > H4_MAX_VAR_DIMS (33)
  {
    const std::string p = GetTempFilePath("toomany_dims.cdf");
    absl::Cleanup rem = [&p] { std::filesystem::remove(p); };
    CdfBufferBuilder b;
    b.WriteI32(0x0000FFFF);
    b.WriteI32(0x0000FFFF);
    b.WriteI32(48);
    b.WriteI32(1);
    b.WriteI32(56);
    b.WriteI32(2);
    b.WriteI32(5);
    b.WriteI32(1);
    b.WriteI32(2);
    for (int i = 0; i < 5; ++i) b.WriteI32(0);
    // GDR with numDims = 33
    b.WriteI32(60);
    b.WriteI32(2);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(0);
    b.WriteI32(33);  // numDims = 33 > H4_MAX_VAR_DIMS(32)
    b.WriteI32(0);
    for (int i = 0; i < 4; ++i) b.WriteI32(0);
    b.WriteToFile(p);

    NC nc{};
    nc.cdf_fp = reinterpret_cast<hdf_file_t>(HI_OPEN(p.c_str(), DFACC_READ));
    ASSERT_THAT(nc.cdf_fp, NotNull());
    absl::Cleanup close_fp = [&nc] {
      if (nc.cdf_fp) HI_CLOSE(nc.cdf_fp);
    };

    XDR xdrs{};
    NC* pnc = &nc;
    EXPECT_THAT(nssdc_read_cdf(&xdrs, &pnc), Eq(FALSE));
  }
}

// TODO(schwehr): Uncovered lines in nssdc.c (~91.7% covered) are defensive OOM
// paths, XDR decode/seek errors on malformed records, and CDF internal errors.

}  // namespace
}  // namespace hdf4
