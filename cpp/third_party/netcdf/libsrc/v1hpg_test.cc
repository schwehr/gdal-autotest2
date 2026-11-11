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

// Unit tests for //third_party/netcdf/libsrc/v1hpg.c (CDF-1/2/5 headers).
//
// Exercises ncx_len_NC(), ncx_put_NC(), and nc_get_NC() through both public
// netCDF APIs (in-memory, diskless, and POSIX temp files across chunk hints and
// NC_SHARE) and direct internal entry points with synthetic NC3_INFO structs,
// corrupt binary images, and fault-injecting mock ncio implementations.
//
// Note for google3 build config (//third_party/netcdf/config.h):
// USE_STRICT_NULL_BYTE_HEADER_PADDING and ERANGE_FILL are undefined, while
// USE_CDF5/NETCDF_ENABLE_CDF5 and 64-bit off_t/size_t are enabled.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc/v1hpg.c

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/types/span.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/nc3internal.h"
#include "third_party/netcdf/libsrc/ncio.h"
#include "third_party/netcdf/libsrc/ncx.h"
}  // extern "C"

namespace netcdf {
namespace {

using ::testing::ElementsAre;

// Helpers for building big-endian CDF-1/CDF-2/CDF-5 headers for negative and
// boundary parser tests.
class HeaderBuilder {
 public:
  explicit HeaderBuilder(int version) : version_(version) {}

  void PutRaw(absl::Span<const uint8_t> bytes) {
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  }

  void PutMagic(uint8_t ver_byte) {
    PutRaw(std::array<uint8_t, 4>{'C', 'D', 'F', ver_byte});
  }

  void PutStandardPreamble(uint64_t numrecs = 0) {
    PutMagic(static_cast<uint8_t>(version_));
    PutSize(numrecs);
  }

  void PutU32(uint32_t value) {
    buf_.push_back(static_cast<uint8_t>((value >> 24) & 0xffU));
    buf_.push_back(static_cast<uint8_t>((value >> 16) & 0xffU));
    buf_.push_back(static_cast<uint8_t>((value >> 8) & 0xffU));
    buf_.push_back(static_cast<uint8_t>(value & 0xffU));
  }

  void PutU64(uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      buf_.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
    }
  }

  void PutSize(uint64_t value) {
    if (version_ == 5) {
      PutU64(value);
    } else {
      PutU32(static_cast<uint32_t>(value));
    }
  }

  void PutOff(uint64_t value) {
    if (version_ == 1) {
      PutU32(static_cast<uint32_t>(value));
    } else {
      PutU64(value);
    }
  }

  void PutPaddedString(std::string_view text) {
    PutSize(text.size());
    for (char ch : text) {
      buf_.push_back(static_cast<uint8_t>(ch));
    }
    const size_t pad = (4 - (text.size() % 4)) % 4;
    buf_.insert(buf_.end(), pad, 0);
  }

  void PutArrayHeader(uint32_t nc_tag, uint64_t nelems) {
    PutU32(nc_tag);
    PutSize(nelems);
  }

  void PutEmptyArray() { PutArrayHeader(NC_UNSPECIFIED, 0); }

  void PutDim(std::string_view name, uint64_t size) {
    PutPaddedString(name);
    PutSize(size);
  }

  void PutInt32Attr(std::string_view name, absl::Span<const int32_t> values) {
    PutPaddedString(name);
    PutU32(NC_INT);
    PutSize(values.size());
    for (int32_t v : values) {
      PutU32(static_cast<uint32_t>(v));
    }
  }

  void PutCharAttr(std::string_view name, std::string_view value) {
    PutPaddedString(name);
    PutU32(NC_CHAR);
    PutSize(value.size());
    for (char ch : value) {
      buf_.push_back(static_cast<uint8_t>(ch));
    }
    const size_t pad = (4 - (value.size() % 4)) % 4;
    buf_.insert(buf_.end(), pad, 0);
  }

  void PutVarHeader(std::string_view name, absl::Span<const int64_t> dimids,
                    bool has_var_attr, uint32_t type, uint64_t vsize,
                    uint64_t begin) {
    PutPaddedString(name);
    PutSize(dimids.size());
    for (int64_t d : dimids) {
      if (version_ == 5) {
        PutU64(static_cast<uint64_t>(d));
      } else {
        PutU32(static_cast<uint32_t>(d));
      }
    }
    if (has_var_attr) {
      PutArrayHeader(NC_ATTRIBUTE, 1);
      PutCharAttr("units", "K");
    } else {
      PutEmptyArray();
    }
    PutU32(type);
    PutSize(vsize);
    PutOff(begin);
  }

  void PadToMinSize(size_t min_bytes) {
    if (buf_.size() < min_bytes) {
      buf_.resize(min_bytes, 0);
    }
  }

  const std::vector<uint8_t>& bytes() const { return buf_; }
  std::vector<uint8_t>& mutable_bytes() { return buf_; }

 private:
  int version_;
  std::vector<uint8_t> buf_;
};

// RAII owner for a directly constructed/populated NC3_INFO.
struct ScopedNc3Info {
  NC3_INFO info{};

  ScopedNc3Info() = default;
  ~ScopedNc3Info() { reset(); }

  ScopedNc3Info(const ScopedNc3Info&) = delete;
  ScopedNc3Info& operator=(const ScopedNc3Info&) = delete;

  void reset() {
    free_NC_dimarrayV(&info.dims);
    free_NC_attrarrayV(&info.attrs);
    free_NC_vararrayV(&info.vars);
    info.dims = NC_dimarray{};
    info.attrs = NC_attrarray{};
    info.vars = NC_vararray{};
    info.nciop = nullptr;
  }
};

// Mock ncio implementation with deterministic region sizing and step-level I/O
// fault injection.
struct MockNcioState {
  std::vector<uint8_t> storage;
  off_t reported_filesize = -1;  // < 0 means report storage.size().
  int filesize_status = NC_NOERR;
  int sync_status = NC_NOERR;
  int get_calls = 0;
  int rel_calls = 0;
  int fail_on_get_call = -1;  // 0-based index, or -1 to disable.
  int fail_get_status = NC_EIO;
  int fail_on_rel_call = -1;  // 0-based index, or -1 to disable.
  int fail_rel_status = NC_EIO;
  bool writable = true;
  bool fail_read_past_eof = true;

  struct ActiveRegion {
    off_t offset = 0;
    size_t extent = 0;
    bool active = false;
    std::vector<uint8_t> buf;
  } region;
};

extern "C" {

static int MockNcioRel(ncio* nciop, off_t /*offset*/, int rflags) {
  auto* st = static_cast<MockNcioState*>(nciop->pvt);
  const int call_idx = st->rel_calls++;
  if (st->region.active) {
    if ((rflags & RGN_MODIFIED) != 0) {
      if (!st->writable) {
        st->region.active = false;
        return EPERM;
      }
      const size_t end =
          static_cast<size_t>(st->region.offset) + st->region.extent;
      if (st->storage.size() < end) {
        st->storage.resize(end, 0);
      }
      std::copy(
          st->region.buf.begin(), st->region.buf.end(),
          st->storage.begin() + static_cast<std::ptrdiff_t>(st->region.offset));
    }
    st->region.active = false;
  }
  if (st->fail_on_rel_call >= 0 && call_idx == st->fail_on_rel_call) {
    return st->fail_rel_status;
  }
  return NC_NOERR;
}

static int MockNcioGet(ncio* nciop, off_t offset, size_t extent, int rflags,
                       void** vpp) {
  auto* st = static_cast<MockNcioState*>(nciop->pvt);
  const int call_idx = st->get_calls++;
  if (st->fail_on_get_call >= 0 && call_idx == st->fail_on_get_call) {
    return st->fail_get_status;
  }
  if ((rflags & RGN_WRITE) != 0 && !st->writable) {
    return EPERM;
  }
  if (offset < 0 || extent == 0) {
    return NC_EINVAL;
  }
  const size_t end = static_cast<size_t>(offset) + extent;
  if (!st->writable && st->fail_read_past_eof && end > st->storage.size()) {
    return EPERM;
  }
  if (st->writable && end > st->storage.size()) {
    st->storage.resize(end, 0);
  }
  st->region.offset = offset;
  st->region.extent = extent;
  st->region.buf.assign(extent, 0);
  const size_t avail =
      st->storage.size() > static_cast<size_t>(offset)
          ? std::min(extent, st->storage.size() - static_cast<size_t>(offset))
          : 0;
  if (avail > 0) {
    std::copy_n(st->storage.begin() + static_cast<std::ptrdiff_t>(offset),
                avail, st->region.buf.begin());
  }
  st->region.active = true;
  *vpp = st->region.buf.data();
  return NC_NOERR;
}

static int MockNcioMove(ncio* /*nciop*/, off_t /*to*/, off_t /*from*/,
                        size_t /*nbytes*/, int /*rflags*/) {
  return NC_NOERR;
}

static int MockNcioSync(ncio* nciop) {
  auto* st = static_cast<MockNcioState*>(nciop->pvt);
  return st->sync_status;
}

static int MockNcioPadLength(ncio* nciop, off_t length) {
  auto* st = static_cast<MockNcioState*>(nciop->pvt);
  if (!st->writable) {
    return EPERM;
  }
  if (length > 0 && st->storage.size() < static_cast<size_t>(length)) {
    st->storage.resize(static_cast<size_t>(length), 0);
  }
  return NC_NOERR;
}

static int MockNcioFilesize(ncio* nciop, off_t* filesizep) {
  auto* st = static_cast<MockNcioState*>(nciop->pvt);
  if (st->filesize_status != NC_NOERR) {
    return st->filesize_status;
  }
  *filesizep = st->reported_filesize >= 0
                   ? st->reported_filesize
                   : static_cast<off_t>(st->storage.size());
  return NC_NOERR;
}

static int MockNcioClose(ncio* /*nciop*/, int /*doUnlink*/) { return NC_NOERR; }

}  // extern "C"

ncio MakeMockNcio(MockNcioState* state) {
  ncio io{};
  io.ioflags = state->writable ? NC_WRITE : NC_NOWRITE;
  io.fd = -1;
  io.rel = &MockNcioRel;
  io.get = &MockNcioGet;
  io.move = &MockNcioMove;
  io.sync = &MockNcioSync;
  io.pad_length = &MockNcioPadLength;
  io.filesize = &MockNcioFilesize;
  io.close = &MockNcioClose;
  io.path = "mock_v1hpg.nc";
  io.pvt = state;
  return io;
}

bool AppendDim(NC_dimarray* arr, const char* name, size_t size) {
  NC_string* strp = new_NC_string(std::strlen(name), name);
  if (strp == nullptr) {
    return false;
  }
  NC_dim* dimp = new_x_NC_dim(strp);
  if (dimp == nullptr) {
    free_NC_string(strp);
    return false;
  }
  dimp->size = size;
  auto** next = static_cast<NC_dim**>(
      std::realloc(arr->value, (arr->nelems + 1) * sizeof(NC_dim*)));
  if (next == nullptr) {
    free_NC_dim(dimp);
    return false;
  }
  arr->value = next;
  arr->value[arr->nelems++] = dimp;
  arr->nalloc = arr->nelems;
  return true;
}

bool AppendAttr(NC_attrarray* arr, const char* name, nc_type type,
                size_t nelems, const void* xdata) {
  NC_string* strp = new_NC_string(std::strlen(name), name);
  if (strp == nullptr) {
    return false;
  }
  NC_attr* attrp = new_x_NC_attr(strp, type, nelems);
  if (attrp == nullptr) {
    free_NC_string(strp);
    return false;
  }
  if (attrp->xvalue != nullptr) {
    // Zero-initialize full padded external storage before writing payload.
    std::memset(attrp->xvalue, 0, attrp->xsz);
    if (xdata != nullptr && attrp->xsz > 0) {
      std::memcpy(attrp->xvalue, xdata, attrp->xsz);
    }
  }
  auto** next = static_cast<NC_attr**>(
      std::realloc(arr->value, (arr->nelems + 1) * sizeof(NC_attr*)));
  if (next == nullptr) {
    free_NC_attr(attrp);
    return false;
  }
  arr->value = next;
  arr->value[arr->nelems++] = attrp;
  arr->nalloc = arr->nelems;
  return true;
}

bool AppendVar(NC_vararray* arr, const char* name, nc_type type,
               absl::Span<const int> dimids, long long len, off_t begin,
               bool with_attr = false) {
  NC_string* strp = new_NC_string(std::strlen(name), name);
  if (strp == nullptr) {
    return false;
  }
  NC_var* varp = new_x_NC_var(strp, dimids.size());
  if (varp == nullptr) {
    free_NC_string(strp);
    return false;
  }
  varp->type = type;
  varp->len = len;
  varp->begin = begin;
  if (!dimids.empty() && varp->dimids != nullptr) {
    std::copy(dimids.begin(), dimids.end(), varp->dimids);
    std::fill_n(varp->shape, dimids.size(), size_t{1});
    std::fill_n(varp->dsizes, dimids.size(), off_t{1});
  }
  if (with_attr) {
    const uint8_t raw_i32[4] = {0, 0, 0, 7};
    if (!AppendAttr(&varp->attrs, "va", NC_INT, 1, raw_i32)) {
      free_NC_var(varp);
      return false;
    }
  }
  auto** next = static_cast<NC_var**>(
      std::realloc(arr->value, (arr->nelems + 1) * sizeof(NC_var*)));
  if (next == nullptr) {
    free_NC_var(varp);
    return false;
  }
  arr->value = next;
  arr->value[arr->nelems++] = varp;
  arr->nalloc = arr->nelems;
  return true;
}

uint32_t ReadBeU32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t ReadBeU64(const uint8_t* p) {
  uint64_t out = 0;
  for (int i = 0; i < 8; ++i) {
    out = (out << 8) | static_cast<uint64_t>(p[i]);
  }
  return out;
}

TEST(V1hpgTest, RoundTripsEmptyDatasetsAcrossFormatsAndIoBackends) {
  struct FormatSpec {
    const char* name;
    int create_mode;
    int expected_format;
    uint8_t magic_ver;
    size_t min_xsz;
  };
  constexpr FormatSpec kFormats[] = {
      {"classic", NC_CLOBBER, NC_FORMAT_CLASSIC, 0x01U, 32U},
      {"offset64", NC_CLOBBER | NC_64BIT_OFFSET, NC_FORMAT_64BIT_OFFSET, 0x02U,
       32U},
      {"cdf5", NC_CLOBBER | NC_CDF5, NC_FORMAT_CDF5, 0x05U, 48U},
  };

  for (const FormatSpec& spec : kFormats) {
    SCOPED_TRACE(spec.name);

    // 1) In-memory create + close_memio + open_mem round-trip.
    ScopedMemio memio;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("empty.nc", spec.create_mode, 64, nc.ptr()));
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
    }
    ASSERT_GE(memio.size(), spec.min_xsz);
    const uint8_t* wire = memio.bytes();
    EXPECT_EQ('C', wire[0]);
    EXPECT_EQ('D', wire[1]);
    EXPECT_EQ('F', wire[2]);
    EXPECT_EQ(spec.magic_ver, wire[3]);
    if (spec.magic_ver == 0x05U) {
      // CDF-5 stores 64-bit numrecs + 3 empty NC_UNSPECIFIED arrays.
      EXPECT_EQ(0ULL, ReadBeU64(wire + 4));
      EXPECT_EQ(uint32_t{NC_UNSPECIFIED}, ReadBeU32(wire + 12));
      EXPECT_EQ(0ULL, ReadBeU64(wire + 16));
      EXPECT_EQ(uint32_t{NC_UNSPECIFIED}, ReadBeU32(wire + 24));
      EXPECT_EQ(0ULL, ReadBeU64(wire + 28));
      EXPECT_EQ(uint32_t{NC_UNSPECIFIED}, ReadBeU32(wire + 36));
      EXPECT_EQ(0ULL, ReadBeU64(wire + 40));
    } else {
      EXPECT_EQ(0U, ReadBeU32(wire + 4));
      EXPECT_EQ(uint32_t{NC_UNSPECIFIED}, ReadBeU32(wire + 8));
      EXPECT_EQ(0U, ReadBeU32(wire + 12));
      EXPECT_EQ(uint32_t{NC_UNSPECIFIED}, ReadBeU32(wire + 16));
      EXPECT_EQ(0U, ReadBeU32(wire + 20));
      EXPECT_EQ(uint32_t{NC_UNSPECIFIED}, ReadBeU32(wire + 24));
      EXPECT_EQ(0U, ReadBeU32(wire + 28));
    }

    {
      ScopedNcFile opened;
      NC_memio ro_params{
          .size = memio.size(),
          .memory = const_cast<uint8_t*>(memio.bytes()),
          .flags = NC_MEMIO_LOCKED,
      };
      ASSERT_EQ(NC_NOERR, nc_open_memio("empty.nc", NC_NOWRITE, &ro_params,
                                        opened.ptr()));
      int fmt = -1;
      int ndims = -1;
      int nvars = -1;
      int natts = -1;
      int unlim = -2;
      EXPECT_EQ(NC_NOERR, nc_inq_format(opened, &fmt));
      EXPECT_EQ(spec.expected_format, fmt);
      EXPECT_EQ(NC_NOERR, nc_inq(opened, &ndims, &nvars, &natts, &unlim));
      EXPECT_EQ(0, ndims);
      EXPECT_EQ(0, nvars);
      EXPECT_EQ(0, natts);
      EXPECT_EQ(-1, unlim);
    }

    // 2) POSIX TempFile with chunksizehint > 4096 exercises the 4096-byte clamp
    // in both ncx_put_NC() and nc_get_NC().
    TempFile file(absl::StrCat("v1hpg_empty_", spec.name, ".nc"));
    size_t large_chunk = 8192;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc__create(file.c_str(), spec.create_mode, 0,
                                     &large_chunk, nc.ptr()));
      ASSERT_EQ(NC_NOERR, nc.close());
    }
    large_chunk = 8192;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc__open(file.c_str(), NC_NOWRITE, &large_chunk, nc.ptr()));
      EXPECT_EQ(NC_NOERR, nc_sync(nc));
      int fmt = -1;
      EXPECT_EQ(NC_NOERR, nc_inq_format(nc, &fmt));
      EXPECT_EQ(spec.expected_format, fmt);
    }
  }

  // 3) NC_DISKLESS create + persist to TempFile and reopen NC_DISKLESS.
  TempFile diskless_file("v1hpg_diskless.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(
        NC_NOERR,
        nc_create(diskless_file.c_str(),
                  NC_CLOBBER | NC_CDF5 | NC_DISKLESS | NC_PERSIST, nc.ptr()));
    int did = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 3, &did));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(diskless_file.c_str(), NC_NOWRITE | NC_DISKLESS,
                                nc.ptr()));
    int fmt = -1;
    size_t len = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_format(nc, &fmt));
    EXPECT_EQ(NC_FORMAT_CDF5, fmt);
    EXPECT_EQ(NC_NOERR, nc_inq_dimlen(nc, 0, &len));
    EXPECT_EQ(size_t{3}, len);
  }
}

TEST(V1hpgTest, RoundTripsAllAtomicAttrTypesZeroLenPaddingAndMultiChunk) {
  ScopedMemio memio;
  const std::string large_text(5003, 'Z');

  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem("all_types.nc", NC_CLOBBER | NC_CDF5, 256,
                                      nc.ptr()));

    // Zero-length attributes exercise value == NULL in put/get NC_attrV.
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(nc, NC_GLOBAL, "a_empty_c", 0, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, NC_GLOBAL, "a_empty_i", NC_INT, 0, nullptr));

    // Padded short/1-byte attributes exercise ncmpix_len_nctype and NUL pad.
    const signed char schar_vals[3] = {-5, 0, 127};
    ASSERT_EQ(NC_NOERR, nc_put_att_schar(nc, NC_GLOBAL, "a_byte", NC_BYTE, 3,
                                         schar_vals));
    const unsigned char uchar_vals[1] = {251U};
    ASSERT_EQ(NC_NOERR, nc_put_att_uchar(nc, NC_GLOBAL, "a_ubyte", NC_UBYTE, 1,
                                         uchar_vals));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc, NC_GLOBAL, "a_char1", 1, "X"));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc, NC_GLOBAL, "a_char4", 4, "ABCD"));

    const int16_t s_vals[3] = {-32000, 7, 32000};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_short(nc, NC_GLOBAL, "a_short", NC_SHORT, 3, s_vals));
    const uint16_t us_vals[1] = {65001U};
    ASSERT_EQ(NC_NOERR, nc_put_att_ushort(nc, NC_GLOBAL, "a_ushort", NC_USHORT,
                                          1, us_vals));

    // Remaining atomic CDF-5 numeric types.
    const int32_t i_vals[2] = {-123456, 654321};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(nc, NC_GLOBAL, "a_int", NC_INT, 2, i_vals));
    const uint32_t ui_vals[2] = {0U, 4000000001U};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_uint(nc, NC_GLOBAL, "a_uint", NC_UINT, 2, ui_vals));
    const float f_vals[2] = {1.5F, -3.25F};
    ASSERT_EQ(NC_NOERR,
              nc_put_att_float(nc, NC_GLOBAL, "a_float", NC_FLOAT, 2, f_vals));
    const double d_vals[2] = {3.125, -99.5};
    ASSERT_EQ(NC_NOERR, nc_put_att_double(nc, NC_GLOBAL, "a_double", NC_DOUBLE,
                                          2, d_vals));
    const long long i64_vals[2] = {-5000000000LL, 9000000000LL};
    ASSERT_EQ(NC_NOERR, nc_put_att_longlong(nc, NC_GLOBAL, "a_int64", NC_INT64,
                                            2, i64_vals));
    const unsigned long long ui64_vals[2] = {1ULL, 18000000000000000001ULL};
    ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(nc, NC_GLOBAL, "a_uint64",
                                             NC_UINT64, 2, ui64_vals));

    // Large attribute > 4096 bytes forces multi-chunk loop in put/get attrV.
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc, NC_GLOBAL, "a_large",
                                        large_text.size(), large_text.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  // Reopen read-only via nc_open_mem and verify every attribute.
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("all_types_ro.nc", NC_NOWRITE, memio.size(),
                        const_cast<uint8_t*>(memio.bytes()), nc.ptr()));

  size_t len = 999;
  EXPECT_EQ(NC_NOERR, nc_inq_attlen(nc, NC_GLOBAL, "a_empty_c", &len));
  EXPECT_EQ(size_t{0}, len);
  EXPECT_EQ(NC_NOERR, nc_inq_attlen(nc, NC_GLOBAL, "a_empty_i", &len));
  EXPECT_EQ(size_t{0}, len);

  std::vector<signed char> sb(3, 0);
  ASSERT_EQ(NC_NOERR, nc_get_att_schar(nc, NC_GLOBAL, "a_byte", sb.data()));
  EXPECT_THAT(sb, ElementsAre(-5, 0, 127));

  unsigned char ub = 0;
  ASSERT_EQ(NC_NOERR, nc_get_att_uchar(nc, NC_GLOBAL, "a_ubyte", &ub));
  EXPECT_EQ(251U, ub);

  char c1[2]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(nc, NC_GLOBAL, "a_char1", c1));
  EXPECT_EQ('X', c1[0]);

  char c4[5]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(nc, NC_GLOBAL, "a_char4", c4));
  EXPECT_EQ("ABCD", std::string_view(c4, 4));

  std::vector<int16_t> sv(3, 0);
  ASSERT_EQ(NC_NOERR, nc_get_att_short(nc, NC_GLOBAL, "a_short", sv.data()));
  EXPECT_THAT(sv, ElementsAre(-32000, 7, 32000));

  uint16_t usv = 0;
  ASSERT_EQ(NC_NOERR, nc_get_att_ushort(nc, NC_GLOBAL, "a_ushort", &usv));
  EXPECT_EQ(65001U, usv);

  std::vector<int32_t> iv(2, 0);
  ASSERT_EQ(NC_NOERR, nc_get_att_int(nc, NC_GLOBAL, "a_int", iv.data()));
  EXPECT_THAT(iv, ElementsAre(-123456, 654321));

  std::vector<uint32_t> uiv(2, 0);
  ASSERT_EQ(NC_NOERR, nc_get_att_uint(nc, NC_GLOBAL, "a_uint", uiv.data()));
  EXPECT_THAT(uiv, ElementsAre(0U, 4000000001U));

  std::vector<float> fv(2, 0.0F);
  ASSERT_EQ(NC_NOERR, nc_get_att_float(nc, NC_GLOBAL, "a_float", fv.data()));
  EXPECT_THAT(fv, ElementsAre(1.5F, -3.25F));

  std::vector<double> dv(2, 0.0);
  ASSERT_EQ(NC_NOERR, nc_get_att_double(nc, NC_GLOBAL, "a_double", dv.data()));
  EXPECT_THAT(dv, ElementsAre(3.125, -99.5));

  std::vector<long long> i64v(2, 0);
  ASSERT_EQ(NC_NOERR,
            nc_get_att_longlong(nc, NC_GLOBAL, "a_int64", i64v.data()));
  EXPECT_THAT(i64v, ElementsAre(-5000000000LL, 9000000000LL));

  std::vector<unsigned long long> ui64v(2, 0);
  ASSERT_EQ(NC_NOERR,
            nc_get_att_ulonglong(nc, NC_GLOBAL, "a_uint64", ui64v.data()));
  EXPECT_THAT(ui64v, ElementsAre(1ULL, 18000000000000000001ULL));

  std::string got_large(large_text.size(), '\0');
  ASSERT_EQ(NC_NOERR,
            nc_get_att_text(nc, NC_GLOBAL, "a_large", got_large.data()));
  EXPECT_EQ(large_text, got_large);
}

TEST(V1hpgTest, RoundTripsVarShapesRecPackingSmallChunksAndShareMode) {
  // 1) Packed single record variable (recsize == first_rec->dsizes * xsz) vs
  // multiple record variables + scalar fixed variable.
  {
    ScopedMemio memio;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("single_rec.nc", NC_CLOBBER, 256, nc.ptr()));
      int rec_dim = -1;
      int rvar = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "t", NC_UNLIMITED, &rec_dim));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "rbyte", NC_BYTE, 1, &rec_dim, &rvar));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      const size_t idx0 = 0;
      const size_t idx1 = 1;
      const signed char v0 = 11;
      const signed char v1 = 22;
      ASSERT_EQ(NC_NOERR, nc_put_var1_schar(nc, rvar, &idx0, &v0));
      ASSERT_EQ(NC_NOERR, nc_put_var1_schar(nc, rvar, &idx1, &v1));
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
    }
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("single_rec_ro.nc", NC_NOWRITE, memio.size(),
                          const_cast<uint8_t*>(memio.bytes()), nc.ptr()));
    size_t nrecs = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_unlimdim(nc, nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_dimlen(nc, 0, &nrecs));
    EXPECT_EQ(size_t{2}, nrecs);
  }

  // 2) Mixed fixed + multiple record vars in 64-bit offset.
  {
    ScopedMemio memio;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("mixed.nc", NC_CLOBBER | NC_64BIT_OFFSET, 256,
                              nc.ptr()));
      int d_unlim = -1;
      int d_x = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "time", NC_UNLIMITED, &d_unlim));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 3, &d_x));

      int v_scalar = -1;
      int v_fix = -1;
      int v_rec1 = -1;
      int v_rec2 = -1;
      ASSERT_EQ(NC_NOERR,
                nc_def_var(nc, "scalar", NC_DOUBLE, 0, nullptr, &v_scalar));
      const double fill = -999.0;
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "fix", NC_INT, 1, &d_x, &v_fix));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "rec1", NC_INT, 1, &d_unlim, &v_rec1));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "rec2", NC_INT, 1, &d_unlim, &v_rec2));
      ASSERT_EQ(NC_NOERR, nc_put_att_double(nc, v_scalar, NC_FillValue,
                                            NC_DOUBLE, 1, &fill));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));

      const double sv = 42.5;
      ASSERT_EQ(NC_NOERR, nc_put_var_double(nc, v_scalar, &sv));
      const int fv[3] = {10, 20, 30};
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, v_fix, fv));
      for (size_t r = 0; r < 3; ++r) {
        const int rv = static_cast<int>(100 + r);
        ASSERT_EQ(NC_NOERR, nc_put_var1_int(nc, v_rec1, &r, &rv));
      }
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
    }

    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_open_mem("mixed_ro.nc", NC_NOWRITE, memio.size(),
                          const_cast<uint8_t*>(memio.bytes()), nc.ptr()));
    size_t nrecs = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_dimlen(nc, 0, &nrecs));
    EXPECT_EQ(size_t{3}, nrecs);
  }

  // 3) POSIX small chunksizehint (256B) with header > 1000B made of short items
  // so ncx_put_NC hits extent > ncp->chunk without triggering px_get E2BIG,
  // and read-only nc_sync() hits nc_get_NC's else-if(extent > ncp->chunk).
  TempFile small_chunk_file("v1hpg_small_chunk.nc");
  size_t small_chunk = 256;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__create(small_chunk_file.c_str(), NC_CLOBBER, 0,
                                   &small_chunk, nc.ptr()));
    EXPECT_EQ(size_t{256}, small_chunk);
    for (int i = 0; i < 40; ++i) {
      const std::string name = absl::StrCat("a_", i);
      ASSERT_EQ(NC_NOERR, nc_put_att_text(nc, NC_GLOBAL, name.c_str(), 12,
                                          "0123456789ab"));
    }
    int did = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 2, &did));
    int vid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "val", NC_INT, 1, &did, &vid));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  small_chunk = 256;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__open(small_chunk_file.c_str(), NC_NOWRITE,
                                 &small_chunk, nc.ptr()));
    // nc_sync() on read-only dataset calls nc_get_NC with ncp->xsz > 256.
    EXPECT_EQ(NC_NOERR, nc_sync(nc));
    int natts = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_natts(nc, &natts));
    EXPECT_EQ(40, natts);
  }
  size_t medium_chunk = 2048;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__open(small_chunk_file.c_str(), NC_NOWRITE,
                                 &medium_chunk, nc.ptr()));
    EXPECT_EQ(NC_NOERR, nc_sync(nc));
  }

  // 4) NC_SHARE with chunksizehint=256 and an 80-dim var forces dimids
  // (320 bytes) > gsp->extent (256 bytes) inside fault_v1hs() on write/read.
  TempFile share_file("v1hpg_share.nc");
  size_t share_chunk = 256;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__create(share_file.c_str(), NC_CLOBBER | NC_SHARE, 0,
                                   &share_chunk, nc.ptr()));
    std::vector<int> dimids(80, 0);
    for (int i = 0; i < 80; ++i) {
      const std::string dname = absl::StrCat("d", i);
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, dname.c_str(), 1, &dimids[i]));
    }
    int vid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "bigdim", NC_BYTE, 80, dimids.data(), &vid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    // NC_SHARE nc_redef() re-reads the header via nc_get_NC().
    ASSERT_EQ(NC_NOERR, nc_redef(nc));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(nc, NC_GLOBAL, "g", 3, "ok!"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }
  share_chunk = 256;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__open(share_file.c_str(), NC_NOWRITE | NC_SHARE,
                                 &share_chunk, nc.ptr()));
    EXPECT_EQ(NC_NOERR, nc_sync(nc));
  }
}

TEST(V1hpgTest, OpenMemRejectsTruncatedAndCorruptedHeadersAcrossFormats) {
  // Construct representative valid headers in v1, v2, and v5, then truncate at
  // every byte offset >= 32 (remembering memio_open clamps sizehint >= 16 only
  // for sizes >= 32 so nc_get_NC()'s assert(gs.pos < gs.end) always holds).
  for (int ver : {1, 2, 5}) {
    SCOPED_TRACE(ver);
    HeaderBuilder hb(ver);
    hb.PutStandardPreamble(/*numrecs=*/0);
    hb.PutArrayHeader(NC_DIMENSION, 2);
    hb.PutDim("padding_dim_name_is_long", 4);
    hb.PutDim("second_dim", 5);
    hb.PutArrayHeader(NC_ATTRIBUTE, 2);
    hb.PutCharAttr("attr1", "hello");
    const int32_t a2[2] = {1, 2};
    hb.PutInt32Attr("attr2", a2);
    hb.PutArrayHeader(NC_VARIABLE, 2);
    const int64_t dims0[2] = {0, 1};
    hb.PutVarHeader("var_one", dims0, /*has_var_attr=*/true, NC_INT, 80, 1024);
    const int64_t dims1[1] = {1};
    hb.PutVarHeader("var_two", dims1, /*has_var_attr=*/false, NC_SHORT, 12,
                    1104);
    const size_t meta_end = hb.bytes().size();
    hb.PadToMinSize(1200);

    // Baseline succeeds.
    {
      ScopedNcFile ok;
      ASSERT_EQ(NC_NOERR,
                nc_open_mem("valid.nc", NC_NOWRITE, hb.bytes().size(),
                            const_cast<uint8_t*>(hb.bytes().data()), ok.ptr()));
    }

    // Every header truncation from offset 32 up to meta_end - 1 must fail.
    for (size_t cut = 32; cut < meta_end; ++cut) {
      std::vector<uint8_t> trunc(hb.bytes().begin(), hb.bytes().begin() + cut);
      ScopedNcFile fail;
      const int status = nc_open_mem("trunc.nc", NC_NOWRITE, trunc.size(),
                                     trunc.data(), fail.ptr());
      EXPECT_NE(NC_NOERR, status) << "ver=" << ver << " cut=" << cut;
    }
  }

  // Multi-chunk (>4096B) attribute truncation in v1h_get_NC_attrV().
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_ATTRIBUTE, 1);
    hb.PutPaddedString("big");
    hb.PutU32(NC_CHAR);
    hb.PutU32(5000);
    hb.PadToMinSize(4200);  // Truncated mid-5000B payload after 1st chunk!
    ScopedNcFile fail;
    EXPECT_NE(NC_NOERR,
              nc_open_mem("trunc_bigatt.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), fail.ptr()));
  }

  // Wrong NCtype tags on dimarray, attrarray, vararray, and variable attrarray.
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_ATTRIBUTE, 1);  // Wrong tag for dimarray.
    hb.PadToMinSize(32);
    ScopedNcFile nc;
    EXPECT_EQ(EINVAL,
              nc_open_mem("bad_dimtag.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_DIMENSION, 1);  // Wrong tag for attrarray.
    hb.PadToMinSize(32);
    ScopedNcFile nc;
    EXPECT_EQ(EINVAL,
              nc_open_mem("bad_atttag.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_DIMENSION, 1);  // Wrong tag for vararray.
    hb.PadToMinSize(32);
    ScopedNcFile nc;
    EXPECT_EQ(EINVAL,
              nc_open_mem("bad_vartag.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 1);
    hb.PutPaddedString("v0");
    hb.PutU32(0);
    hb.PutArrayHeader(NC_DIMENSION, 1);  // Wrong tag for var->attrs.
    // memio_open sets chunk = ((size / 2) / 8) * 8; pad to 128 bytes so the
    // initial 64-byte chunk covers the whole header before memio_pad_length
    // rejects a second chunk read on a read-only buffer with EPERM.
    hb.PadToMinSize(128);
    ScopedNcFile nc;
    EXPECT_EQ(EINVAL,
              nc_open_mem("bad_varatt_tag.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }

  // Invalid nc_type values (NC_NAT == 0 and NC_MAX_ATOMIC_TYPE + 1 == 13) in
  // attributes and variables.
  for (uint32_t bad_type :
       {uint32_t{NC_NAT}, uint32_t{NC_MAX_ATOMIC_TYPE + 1}}) {
    HeaderBuilder att_hb(1);
    att_hb.PutStandardPreamble(0);
    att_hb.PutEmptyArray();
    att_hb.PutArrayHeader(NC_ATTRIBUTE, 1);
    att_hb.PutPaddedString("bad");
    att_hb.PutU32(bad_type);
    att_hb.PadToMinSize(128);
    ScopedNcFile att_nc;
    EXPECT_EQ(
        NC_EINVAL,
        nc_open_mem("bad_atttype.nc", NC_NOWRITE, att_hb.bytes().size(),
                    const_cast<uint8_t*>(att_hb.bytes().data()), att_nc.ptr()));

    HeaderBuilder var_hb(1);
    var_hb.PutStandardPreamble(0);
    var_hb.PutEmptyArray();
    var_hb.PutEmptyArray();
    var_hb.PutArrayHeader(NC_VARIABLE, 1);
    var_hb.PutPaddedString("badv");
    var_hb.PutU32(0);
    var_hb.PutEmptyArray();
    var_hb.PutU32(bad_type);
    var_hb.PadToMinSize(128);
    ScopedNcFile var_nc;
    EXPECT_EQ(
        NC_EINVAL,
        nc_open_mem("bad_vartype.nc", NC_NOWRITE, var_hb.bytes().size(),
                    const_cast<uint8_t*>(var_hb.bytes().data()), var_nc.ptr()));
  }

  // ndims > NC_MAX_VAR_DIMS -> NC_EMAXDIMS.
  {
    HeaderBuilder hb(5);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 1);
    hb.PutPaddedString("manydims");
    hb.PutU64(static_cast<uint64_t>(NC_MAX_VAR_DIMS) + 1ULL);
    hb.PadToMinSize(80);
    ScopedNcFile nc;
    EXPECT_EQ(NC_EMAXDIMS,
              nc_open_mem("emaxdims.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }

  // CDF-5 integer overflow / OOM checks in dimarray, vararray, NC_string,
  // NC_attr, and 64-bit dimid -> NC_ERANGE.
  {
    HeaderBuilder hb(5);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 0x4000000000000000ULL);
    hb.PadToMinSize(32);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ERANGE,
              nc_open_mem("dim_erange.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    HeaderBuilder hb(5);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 0x4000000000000000ULL);
    hb.PadToMinSize(56);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ERANGE,
              nc_open_mem("var_erange.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    HeaderBuilder hb(5);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 1);
    hb.PutU64(std::numeric_limits<uint64_t>::max());  // new_NC_string -> NULL.
    hb.PadToMinSize(40);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ENOMEM,
              nc_open_mem("str_oom.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    HeaderBuilder hb(5);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_ATTRIBUTE, 1);
    hb.PutPaddedString("huge");
    hb.PutU32(NC_INT);
    hb.PutU64(0x3ffffffffffffffeULL);  // new_x_NC_attr() -> NULL without alloc.
    hb.PadToMinSize(64);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ENOMEM,
              nc_open_mem("att_oom.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    HeaderBuilder hb(5);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 1);
    hb.PutDim("d0", 2);
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 1);
    hb.PutPaddedString("bad_dimid_64");
    hb.PutU64(1);
    hb.PutU64(0x0000000100000000ULL);  // > INT_MAX -> NC_ERANGE in dimids.
    hb.PadToMinSize(128);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ERANGE,
              nc_open_mem("dimid64_erange.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }

  // NC_computeshapes() errors: NC_EBADDIM, NC_EUNLIMPOS, begin checks.
  for (int64_t bad_dimid : {int64_t{-1}, int64_t{5}}) {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 1);
    hb.PutDim("d0", 3);
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 1);
    const int64_t dims[1] = {bad_dimid};
    hb.PutVarHeader("v", dims, false, NC_INT, 12, 128);
    hb.PadToMinSize(160);
    ScopedNcFile nc;
    EXPECT_EQ(NC_EBADDIM,
              nc_open_mem("ebaddim.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 2);
    hb.PutDim("d0", 3);
    hb.PutDim("u1", uint64_t{NC_UNLIMITED});
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 1);
    const int64_t dims[2] = {0, 1};  // NC_UNLIMITED not first.
    hb.PutVarHeader("v", dims, false, NC_INT, 12, 128);
    hb.PadToMinSize(180);
    ScopedNcFile nc;
    EXPECT_EQ(NC_EUNLIMPOS,
              nc_open_mem("eunlimpos.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  // begin_var == 0, begin_var < xsz, begin_var > begin_rec, begin_rec >
  // first_rec->begin.
  for (uint64_t bad_begin : {uint64_t{0}, uint64_t{16}}) {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 1);
    hb.PutVarHeader("v", {}, false, NC_INT, 4, bad_begin);
    hb.PadToMinSize(96);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ENOTNC,
              nc_open_mem("bad_begin.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    // Two fixed vars with decreasing begin: v0.begin = 300 > v1.end = 108.
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 2);
    hb.PutVarHeader("v0", {}, false, NC_INT, 4, 300);
    hb.PutVarHeader("v1", {}, false, NC_INT, 4, 100);
    hb.PadToMinSize(320);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ENOTNC,
              nc_open_mem("begin_var_gt_rec.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
  {
    // Fixed var ends at 208 > first_rec->begin (150).
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 1);
    hb.PutDim("t", NC_UNLIMITED);
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 2);
    hb.PutVarHeader("v0", {}, false, NC_DOUBLE, 8, 200);
    const int64_t tdim[1] = {0};
    hb.PutVarHeader("rv", tdim, false, NC_INT, 4, 150);
    hb.PadToMinSize(256);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ENOTNC,
              nc_open_mem("rec_before_fix.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }

  // NC_check_vlens() failure -> NC_EVARSIZE (CDF-2 large fixed var not last).
  {
    HeaderBuilder hb(2);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 1);
    hb.PutDim("huge", 1200000000ULL);
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 2);
    const int64_t dims0[1] = {0};
    hb.PutVarHeader("v0", dims0, false, NC_INT, 4294967295ULL, 256);
    hb.PutVarHeader("v1", {}, false, NC_INT, 4, 4800000256ULL);
    hb.PadToMinSize(256);
    ScopedNcFile nc;
    EXPECT_EQ(NC_EVARSIZE,
              nc_open_mem("evarsize.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }

  // NC_check_voffs() failure -> NC_ENOTNC (overlapping fixed vars where
  // begin_var < v1->end still passes NC_computeshapes()).
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutArrayHeader(NC_VARIABLE, 2);
    hb.PutVarHeader("v0", {}, false, NC_DOUBLE, 8, 128);
    hb.PutVarHeader("v1", {}, false, NC_DOUBLE, 8, 132);
    hb.PadToMinSize(160);
    ScopedNcFile nc;
    EXPECT_EQ(NC_ENOTNC,
              nc_open_mem("overlap_voffs.nc", NC_NOWRITE, hb.bytes().size(),
                          const_cast<uint8_t*>(hb.bytes().data()), nc.ptr()));
  }
}

TEST(V1hpgTest, DirectGetNcCoversEarlyErrorsSmallTruncationsAndRelFailure) {
  auto run_get = [](MockNcioState& state, int flags, size_t chunk,
                    size_t init_xsz, ScopedNc3Info* out = nullptr) {
    ncio io = MakeMockNcio(&state);
    ScopedNc3Info local;
    ScopedNc3Info* target = out != nullptr ? out : &local;
    target->info.nciop = &io;
    target->info.flags = flags;
    target->info.chunk = chunk;
    target->info.xsz = init_xsz;
    const int status = nc_get_NC(&target->info);
    target->info.nciop = nullptr;
    return status;
  };

  // 1) ncio_filesize failure.
  {
    MockNcioState st;
    st.writable = false;
    st.filesize_status = EIO;
    EXPECT_EQ(EIO, run_get(st, 0, 4096, 0));
  }

  // 2) Files smaller than 4 bytes (0 and 3) -> NC_ENOTNC.
  for (size_t sz : {size_t{0}, size_t{3}}) {
    MockNcioState st;
    st.writable = false;
    st.storage.assign(sz, 0);
    EXPECT_EQ(NC_ENOTNC, run_get(st, 0, 4096, 0));
  }

  // 3) ncio_sync failure and initial fault_v1hs failure.
  {
    MockNcioState st;
    st.writable = false;
    st.storage.assign(32, 0);
    st.sync_status = EIO;
    EXPECT_EQ(EIO, run_get(st, 0, 4096, 0));
  }
  {
    MockNcioState st;
    st.writable = false;
    st.storage.assign(32, 0);
    st.fail_on_get_call = 0;
    st.fail_get_status = EIO;
    EXPECT_EQ(EIO, run_get(st, 0, 4096, 0));
  }

  // 4) Bad magic and bad version bytes -> NC_ENOTNC.
  {
    MockNcioState st;
    st.writable = false;
    st.storage.assign(32, 0);
    std::memcpy(st.storage.data(), "BAD\x01", 4);
    EXPECT_EQ(NC_ENOTNC, run_get(st, 0, 4096, 0));
  }
  for (uint8_t bad_ver : {uint8_t{0x00}, uint8_t{0x03}, uint8_t{0x04}}) {
    MockNcioState st;
    st.writable = false;
    st.storage.assign(32, 0);
    st.storage[0] = 'C';
    st.storage[1] = 'D';
    st.storage[2] = 'F';
    st.storage[3] = bad_ver;
    EXPECT_EQ(NC_ENOTNC, run_get(st, 0, 4096, 0));
  }

  // 5) Small truncations at offsets 9..31 (v1/v2) and 13..31 (v5) safely
  // satisfy assert(gs.pos < gs.end) because chunk >= filesize ensures initial
  // get reads the full file prefix, and subsequent check_v1hs faults fail read
  // past EOF.
  for (int ver : {1, 2, 5}) {
    HeaderBuilder hb(ver);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 1);
    hb.PutDim("d0", 4);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    const size_t min_cut = (ver == 5) ? 13 : 9;
    for (size_t cut = min_cut; cut <= 32 && cut < hb.bytes().size(); ++cut) {
      MockNcioState st;
      st.writable = false;
      st.storage.assign(hb.bytes().begin(), hb.bytes().begin() + cut);
      EXPECT_NE(NC_NOERR, run_get(st, ver == 5 ? NC_64BIT_DATA : 0, 4096, 0))
          << "ver=" << ver << " cut=" << cut;
    }
  }

  // 6) fault_v1hs() rel_v1hs() error on chunk transition while reading.
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 2);
    hb.PutDim("dim_a", 2);
    hb.PutDim("dim_b", 3);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    MockNcioState st;
    st.writable = false;
    st.storage = hb.bytes();
    st.fail_on_rel_call = 0;
    st.fail_rel_status = EIO;
    EXPECT_EQ(EIO, run_get(st, 0, 16, 40));
  }

  // 7) Zero-length name string (nchars == 0) in v1h_get_NC_string().
  {
    HeaderBuilder hb(1);
    hb.PutStandardPreamble(0);
    hb.PutArrayHeader(NC_DIMENSION, 1);
    hb.PutDim("", 7);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    MockNcioState st;
    st.writable = false;
    st.storage = hb.bytes();
    ScopedNc3Info nc3;
    ASSERT_EQ(NC_NOERR, run_get(st, 0, 4096, 0, &nc3));
    ASSERT_EQ(size_t{1}, nc3.info.dims.nelems);
    EXPECT_EQ(size_t{0}, nc3.info.dims.value[0]->name->nchars);
  }

  // 8) nc_get_NC() extent branches: xsz > MIN_NC*_XSZ (both <= chunk and
  // > chunk), plus chunk > 4096 and > filesize clamps.
  for (int flags : {0, NC_64BIT_DATA}) {
    HeaderBuilder hb(flags == NC_64BIT_DATA ? 5 : 1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PadToMinSize(64);
    for (size_t chunk : {size_t{2048}, size_t{16}, size_t{8192}}) {
      MockNcioState st;
      st.writable = false;
      st.storage = hb.bytes();
      ScopedNc3Info nc3;
      const size_t init_xsz = (chunk == 8192) ? 0 : 64;
      ASSERT_EQ(NC_NOERR, run_get(st, flags, chunk, init_xsz, &nc3));
      EXPECT_EQ(size_t{0}, nc3.info.dims.nelems);
    }
  }

  // 9) Empty dataset error sweep in nc_get_NC() hits empty dim/attr/vararray
  // check_v1hs failures cleanly.
  for (int flags : {0, NC_64BIT_DATA}) {
    HeaderBuilder hb(flags == NC_64BIT_DATA ? 5 : 1);
    hb.PutStandardPreamble(0);
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    hb.PutEmptyArray();
    const std::vector<size_t> chunks =
        (flags == 0) ? std::vector<size_t>{9, 12, 16, 20, 24, 28}
                     : std::vector<size_t>{13, 16, 24, 28, 36, 40};
    for (size_t chunk : chunks) {
      for (int fail_idx : {0, 1}) {
        MockNcioState st;
        st.writable = false;
        st.storage = hb.bytes();
        st.fail_on_get_call = fail_idx;
        EXPECT_EQ(NC_EIO, run_get(st, flags, chunk, 0))
            << "flags=" << flags << " chunk=" << chunk << " fail=" << fail_idx;
      }
    }
  }
}

TEST(V1hpgTest, DirectLenNcAndPutNcRoundTripsClampErrorsAndFaultSweep) {
  // Helper to populate a non-empty synthetic NC3_INFO with controllable
  // initial dim name length. When with_long_name is false, every name stays
  // <= 12 chars so psp->extent stays constant at 16 across chunk=16 sweeps.
  auto build_populated = [](ScopedNc3Info& out, int flags, size_t first_dim_len,
                            size_t chunk, bool with_long_name = false) {
    out.reset();
    out.info.flags = flags;
    out.info.chunk = chunk;
    NC_set_numrecs(&out.info, 2);
    const std::string d0_name(first_dim_len, 'a');
    ASSERT_TRUE(AppendDim(&out.info.dims, d0_name.c_str(), NC_UNLIMITED));
    ASSERT_TRUE(AppendDim(
        &out.info.dims,
        with_long_name ? "long_second_dimension_21c" : "second_d", 4));
    ASSERT_TRUE(AppendDim(&out.info.dims, "third_d", 2));

    const uint8_t a1[4] = {'w', 'x', 'y', 'z'};
    const std::string long_attr_payload(40, 'Q');  // > chunk=16 multi-chunk!
    ASSERT_TRUE(AppendAttr(&out.info.attrs, "empty_a", NC_INT, 0, nullptr));
    ASSERT_TRUE(AppendAttr(&out.info.attrs, "pad1", NC_CHAR, 1, a1));
    ASSERT_TRUE(AppendAttr(
        &out.info.attrs,
        with_long_name ? "long_global_attr_name_22c" : "long_attr", NC_CHAR,
        long_attr_payload.size(), long_attr_payload.data()));

    const int fix_dims[2] = {1, 2};
    const int rec_dims[2] = {0, 1};
    ASSERT_TRUE(AppendVar(&out.info.vars, "v_fix", NC_DOUBLE, fix_dims, 64,
                          1024, /*with_attr=*/true));
    ASSERT_TRUE(AppendVar(&out.info.vars, "v_scal", NC_INT, {}, 4, 1088,
                          /*with_attr=*/false));
    ASSERT_TRUE(AppendVar(
        &out.info.vars, with_long_name ? "long_record_variable_name" : "rec_v",
        NC_INT, rec_dims, 16, 1152, /*with_attr=*/false));

    const size_t sizeof_off = (flags == 0) ? 4 : 8;
    out.info.xsz = ncx_len_NC(&out.info, sizeof_off);
  };

  // 1) ncx_len_NC + direct ncx_put_NC with xpp != NULL (OFF_NONE) and
  // xpp == NULL (including chunk=4096 no-clamp and empty chunk=8192 clamp),
  // plus zero-length NC_string name and round-trip via nc_get_NC.
  for (int flags : {0, NC_64BIT_OFFSET, NC_64BIT_DATA}) {
    SCOPED_TRACE(flags);
    ScopedNc3Info src;
    build_populated(src, flags, /*first_dim_len=*/0, 4096,
                    /*with_long_name=*/true);
    const size_t sizeof_off = (flags == 0) ? 4 : 8;
    const size_t hdr_len = ncx_len_NC(&src.info, sizeof_off);
    ASSERT_EQ(hdr_len, src.info.xsz);
    ASSERT_EQ(size_t{0}, hdr_len % 4);

    std::vector<uint8_t> buf(hdr_len, 0xAAU);
    void* xp = buf.data();
    ASSERT_EQ(NC_NOERR, ncx_put_NC(&src.info, &xp, OFF_NONE, buf.size()));

    // xpp == NULL with xsz <= chunk (= 4096) hits the else-if false branch.
    {
      MockNcioState wr_st;
      wr_st.writable = true;
      ncio wr_io = MakeMockNcio(&wr_st);
      src.info.nciop = &wr_io;
      ASSERT_EQ(NC_NOERR, ncx_put_NC(&src.info, nullptr, 0, 0));
      src.info.nciop = nullptr;
      EXPECT_GE(wr_st.storage.size(), hdr_len);
    }

    // Read back with nc_get_NC and verify key metadata.
    MockNcioState ro;
    ro.writable = false;
    ro.storage = buf;
    ro.storage.resize(2048, 0);
    ScopedNc3Info dst;
    ncio io = MakeMockNcio(&ro);
    dst.info.nciop = &io;
    dst.info.chunk = 4096;
    ASSERT_EQ(NC_NOERR, nc_get_NC(&dst.info));
    dst.info.nciop = nullptr;
    EXPECT_EQ(size_t{2}, NC_get_numrecs(&dst.info));
    EXPECT_EQ(size_t{3}, dst.info.dims.nelems);
    EXPECT_EQ(size_t{0}, dst.info.dims.value[0]->name->nchars);
    EXPECT_EQ(size_t{3}, dst.info.attrs.nelems);
    EXPECT_EQ(size_t{3}, dst.info.vars.nelems);

    // Empty dataset with chunk = 8192 (> 4096) hits ncx_put_NC's 4096 clamp.
    ScopedNc3Info empty;
    empty.info.flags = flags;
    empty.info.chunk = 8192;
    empty.info.xsz = (flags == NC_64BIT_DATA) ? MIN_NC5_XSZ : MIN_NC3_XSZ;
    MockNcioState clamp_st;
    ncio clamp_io = MakeMockNcio(&clamp_st);
    empty.info.nciop = &clamp_io;
    EXPECT_EQ(NC_NOERR, ncx_put_NC(&empty.info, nullptr, 0, 0));
    empty.info.nciop = nullptr;
    EXPECT_EQ(size_t{4096}, clamp_st.storage.size());
  }

  // 2) xpp != NULL with offset = 0 and valid MockNcio executes rel_v1hs() on a
  // real ncio handle.
  {
    ScopedNc3Info src;
    src.info.chunk = 4096;
    src.info.xsz = MIN_NC3_XSZ;
    MockNcioState st;
    st.writable = true;
    ncio io = MakeMockNcio(&st);
    void* mapped = nullptr;
    ASSERT_EQ(NC_NOERR, ncio_get(&io, 0, MIN_NC3_XSZ, RGN_WRITE, &mapped));
    src.info.nciop = &io;
    ASSERT_EQ(NC_NOERR, ncx_put_NC(&src.info, &mapped, 0, MIN_NC3_XSZ));
    src.info.nciop = nullptr;
    EXPECT_EQ(1, st.rel_calls);
    ASSERT_EQ(MIN_NC3_XSZ, st.storage.size());
    EXPECT_EQ('C', st.storage[0]);
    EXPECT_EQ('D', st.storage[1]);
    EXPECT_EQ('F', st.storage[2]);
    EXPECT_EQ(uint8_t{0x01U}, st.storage[3]);
  }

  // 3) vsize > 4294967292UL clamping in Classic / 64-bit Offset vs full 64-bit
  // vsize in CDF-5, plus negative varp->begin -> NC_ERANGE.
  for (int flags : {0, NC_64BIT_OFFSET, NC_64BIT_DATA}) {
    ScopedNc3Info src;
    src.info.flags = flags;
    src.info.chunk = 4096;
    ASSERT_TRUE(
        AppendVar(&src.info.vars, "bigv", NC_BYTE, {}, 5000000000LL, 512));
    const size_t sizeof_off = (flags == 0) ? 4 : 8;
    src.info.xsz = ncx_len_NC(&src.info, sizeof_off);
    std::vector<uint8_t> buf(src.info.xsz, 0);
    void* xp = buf.data();
    ASSERT_EQ(NC_NOERR, ncx_put_NC(&src.info, &xp, OFF_NONE, buf.size()));
    if (flags == NC_64BIT_DATA) {
      // CDF-5 stores 64-bit vsize before 8-byte begin -> bytes [sz-16..sz-8).
      EXPECT_EQ(5000000000ULL, ReadBeU64(buf.data() + buf.size() - 16));
    } else {
      const size_t begin_sz = (flags == 0) ? 4 : 8;
      EXPECT_EQ(4294967295U, ReadBeU32(buf.data() + buf.size() - begin_sz - 4));
    }

    // Negative begin -> NC_ERANGE from ncx_put_off_t().
    src.info.vars.value[0]->begin = -1;
    xp = buf.data();
    EXPECT_EQ(NC_ERANGE, ncx_put_NC(&src.info, &xp, OFF_NONE, buf.size()));
  }

  // 4) Empty dataset error sweep in ncx_put_NC() hits empty dim/attr/vararray
  // check_v1hs failures cleanly.
  for (int flags : {0, NC_64BIT_DATA}) {
    const std::vector<size_t> chunks =
        (flags == 0) ? std::vector<size_t>{9, 12, 16, 20, 24, 28}
                     : std::vector<size_t>{13, 16, 24, 28, 36, 40};
    for (size_t chunk : chunks) {
      for (int fail_idx : {0, 1}) {
        ScopedNc3Info empty;
        empty.info.flags = flags;
        empty.info.chunk = chunk;
        empty.info.xsz = (flags == 0) ? MIN_NC3_XSZ : MIN_NC5_XSZ;
        MockNcioState st;
        st.fail_on_get_call = fail_idx;
        ncio io = MakeMockNcio(&st);
        empty.info.nciop = &io;
        EXPECT_EQ(NC_EIO, ncx_put_NC(&empty.info, nullptr, 0, 0))
            << "flags=" << flags << " chunk=" << chunk << " fail=" << fail_idx;
        empty.info.nciop = nullptr;
      }
    }
  }

  // 5) Exhaustive symmetric step-by-step fault injection on BOTH ncx_put_NC()
  // and nc_get_NC() across Classic and CDF-5 with 4-byte phase shifts
  // {1, 5, 9, 13} mod 16 (plus non-empty dimarray header chunks and a
  // >16B dim name for fault_v1hs extent growth).
  for (int flags : {0, NC_64BIT_DATA}) {
    for (size_t dim0_len : {size_t{1}, size_t{5}, size_t{9}, size_t{13}}) {
      ScopedNc3Info src;
      build_populated(src, flags, dim0_len, /*chunk=*/16,
                      /*with_long_name=*/false);

      // Baseline ncx_put_NC pass produces canonical wire image and get count.
      MockNcioState base_put_st;
      ncio base_put_io = MakeMockNcio(&base_put_st);
      src.info.nciop = &base_put_io;
      ASSERT_EQ(NC_NOERR, ncx_put_NC(&src.info, nullptr, 0, 0));
      src.info.nciop = nullptr;
      const int total_put_gets = base_put_st.get_calls;
      ASSERT_GT(total_put_gets, 8);

      for (int fail_get = 0; fail_get < total_put_gets; ++fail_get) {
        MockNcioState st;
        st.fail_on_get_call = fail_get;
        ncio io = MakeMockNcio(&st);
        src.info.nciop = &io;
        EXPECT_EQ(NC_EIO, ncx_put_NC(&src.info, nullptr, 0, 0))
            << "put flags=" << flags << " dim0_len=" << dim0_len
            << " fail_get=" << fail_get;
        src.info.nciop = nullptr;
      }

      // Fail rel_v1hs() on write chunk transition.
      {
        MockNcioState rel_st;
        rel_st.fail_on_rel_call = 0;
        ncio rel_io = MakeMockNcio(&rel_st);
        src.info.nciop = &rel_io;
        EXPECT_EQ(NC_EIO, ncx_put_NC(&src.info, nullptr, 0, 0));
        src.info.nciop = nullptr;
      }

      // Baseline nc_get_NC pass on the serialized header + step fault sweep.
      std::vector<uint8_t> ro_image = base_put_st.storage;
      ro_image.resize(2048, 0);
      MockNcioState base_get_st;
      base_get_st.writable = false;
      base_get_st.storage = ro_image;
      {
        ScopedNc3Info dst;
        ncio base_get_io = MakeMockNcio(&base_get_st);
        dst.info.nciop = &base_get_io;
        dst.info.chunk = 16;
        ASSERT_EQ(NC_NOERR, nc_get_NC(&dst.info));
        dst.info.nciop = nullptr;
        EXPECT_EQ(size_t{3}, dst.info.dims.nelems);
        EXPECT_EQ(size_t{3}, dst.info.attrs.nelems);
        EXPECT_EQ(size_t{3}, dst.info.vars.nelems);
      }
      const int total_read_gets = base_get_st.get_calls;
      ASSERT_GT(total_read_gets, 8);

      for (int fail_get = 0; fail_get < total_read_gets; ++fail_get) {
        MockNcioState st;
        st.writable = false;
        st.storage = ro_image;
        st.fail_on_get_call = fail_get;
        ScopedNc3Info dst;
        ncio io = MakeMockNcio(&st);
        dst.info.nciop = &io;
        dst.info.chunk = 16;
        EXPECT_EQ(NC_EIO, nc_get_NC(&dst.info))
            << "get flags=" << flags << " dim0_len=" << dim0_len
            << " fail_get=" << fail_get;
        dst.info.nciop = nullptr;
      }

      // Fail rel_v1hs() on read chunk transition.
      {
        MockNcioState rel_st;
        rel_st.writable = false;
        rel_st.storage = ro_image;
        rel_st.fail_on_rel_call = 0;
        ScopedNc3Info dst;
        ncio rel_io = MakeMockNcio(&rel_st);
        dst.info.nciop = &rel_io;
        dst.info.chunk = 16;
        EXPECT_EQ(NC_EIO, nc_get_NC(&dst.info));
        dst.info.nciop = nullptr;
      }
    }

    // Hit v1h_put_NC_dimarray/v1h_get_NC_dimarray non-empty NCtype & nelems
    // check_v1hs failures cleanly.
    const std::vector<size_t> dim_hdr_chunks =
        (flags == 0) ? std::vector<size_t>{9, 12} : std::vector<size_t>{13, 16};
    for (size_t chunk : dim_hdr_chunks) {
      ScopedNc3Info src;
      build_populated(src, flags, /*first_dim_len=*/4, chunk,
                      /*with_long_name=*/false);
      MockNcioState put_fail_st;
      put_fail_st.fail_on_get_call = 1;
      ncio put_fail_io = MakeMockNcio(&put_fail_st);
      src.info.nciop = &put_fail_io;
      EXPECT_EQ(NC_EIO, ncx_put_NC(&src.info, nullptr, 0, 0));
      src.info.nciop = nullptr;

      std::vector<uint8_t> buf(src.info.xsz, 0);
      void* xp = buf.data();
      ASSERT_EQ(NC_NOERR, ncx_put_NC(&src.info, &xp, OFF_NONE, buf.size()));
      buf.resize(2048, 0);

      MockNcioState get_fail_st;
      get_fail_st.writable = false;
      get_fail_st.storage = buf;
      get_fail_st.fail_on_get_call = 1;
      ScopedNc3Info dst;
      ncio get_fail_io = MakeMockNcio(&get_fail_st);
      dst.info.nciop = &get_fail_io;
      dst.info.chunk = chunk;
      EXPECT_EQ(NC_EIO, nc_get_NC(&dst.info));
      dst.info.nciop = nullptr;
    }

    // Exercise fault_v1hs() extent growth (extent > gsp->extent) on both
    // ncx_put_NC() and nc_get_NC(), including failing on that enlarged request.
    {
      ScopedNc3Info src;
      build_populated(src, flags, /*first_dim_len=*/1, /*chunk=*/16,
                      /*with_long_name=*/true);
      MockNcioState base_put_st;
      ncio base_put_io = MakeMockNcio(&base_put_st);
      src.info.nciop = &base_put_io;
      ASSERT_EQ(NC_NOERR, ncx_put_NC(&src.info, nullptr, 0, 0));
      src.info.nciop = nullptr;
      for (int fail_get = 0; fail_get < base_put_st.get_calls; ++fail_get) {
        MockNcioState st;
        st.fail_on_get_call = fail_get;
        ncio io = MakeMockNcio(&st);
        src.info.nciop = &io;
        EXPECT_EQ(NC_EIO, ncx_put_NC(&src.info, nullptr, 0, 0));
        src.info.nciop = nullptr;
      }

      std::vector<uint8_t> ro_image = base_put_st.storage;
      ro_image.resize(2048, 0);
      MockNcioState base_get_st;
      base_get_st.writable = false;
      base_get_st.storage = ro_image;
      ScopedNc3Info dst;
      ncio base_get_io = MakeMockNcio(&base_get_st);
      dst.info.nciop = &base_get_io;
      dst.info.chunk = 16;
      ASSERT_EQ(NC_NOERR, nc_get_NC(&dst.info));
      dst.info.nciop = nullptr;
      for (int fail_get = 0; fail_get < base_get_st.get_calls; ++fail_get) {
        MockNcioState st;
        st.writable = false;
        st.storage = ro_image;
        st.fail_on_get_call = fail_get;
        ScopedNc3Info fail_dst;
        ncio io = MakeMockNcio(&st);
        fail_dst.info.nciop = &io;
        fail_dst.info.chunk = 16;
        EXPECT_EQ(NC_EIO, nc_get_NC(&fail_dst.info));
        fail_dst.info.nciop = nullptr;
      }
    }
  }
}

}  // namespace
}  // namespace netcdf
