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
// Tests datatype object header message encoding, decoding, versioning,
// bounds/integrity validation, native copy/size/reset/free, shared/committed
// message handling during object copying, and debug output
// (third_party/hdf5/src/H5Odtype.c).
// Note: H5_STRICT_FORMAT_CHECKS is undefined in the google3 build, so strict
// version-rejection branches are compiled out in favor of auto-upgrading.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Odtype.c

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug fields from
// H5C_cache_entry_t / H5O_t pulled in via internal headers.
#ifndef NDEBUG
#define H5ODTYPE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#pragma push_macro("H5_HAVE__FLOAT16")
#undef H5_HAVE__FLOAT16
#define H5O_FRIEND
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Tpkg.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Rprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5ODTYPE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ODTYPE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__SUPERBLOCK_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5T_t* UnwrapType(hid_t type_id) {
  return static_cast<H5T_t*>(H5I_object_verify(type_id, H5I_DATATYPE));
}

void AppendUint16LE(std::vector<uint8_t>& buf, uint16_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xffU));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xffU));
}

void AppendUint32LE(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xffU));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xffU));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xffU));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xffU));
}

void AppendHeader(std::vector<uint8_t>& buf, uint8_t version, uint8_t cls,
                  uint32_t flags24, uint32_t size) {
  buf.push_back(static_cast<uint8_t>((cls & 0x0fU) | ((version & 0x0fU) << 4)));
  buf.push_back(static_cast<uint8_t>(flags24 & 0xffU));
  buf.push_back(static_cast<uint8_t>((flags24 >> 8) & 0xffU));
  buf.push_back(static_cast<uint8_t>((flags24 >> 16) & 0xffU));
  AppendUint32LE(buf, size);
}

std::vector<uint8_t> EncodeMsg(H5F_t* f, const H5T_t* dt) {
  const size_t sz = H5O_msg_raw_size(f, H5O_DTYPE_ID, /*disable_shared=*/true,
                                     dt);
  EXPECT_GT(sz, 0);
  std::vector<uint8_t> buf(sz, 0);
  EXPECT_GE(
      H5O_msg_encode(f, H5O_DTYPE_ID, /*disable_shared=*/true, buf.data(), dt),
      0);
  return buf;
}

std::string ReadTmpFile(FILE* fp) {
  std::fflush(fp);
  std::rewind(fp);
  std::string out;
  char chunk[512];
  while (true) {
    const size_t n = std::fread(chunk, 1, sizeof(chunk), fp);
    if (n == 0) {
      break;
    }
    out.append(chunk, n);
  }
  return out;
}

std::vector<uint8_t> PublicEncodeType(hid_t tid) {
  size_t sz = 0;
  EXPECT_GE(H5Tencode(tid, nullptr, &sz), 0);
  std::vector<uint8_t> buf(sz, 0);
  EXPECT_GE(H5Tencode(tid, buf.data(), &sz), 0);
  return buf;
}

TEST(H5OdtypeTest, PublicEncodeDecodeRoundTripsAcrossClasses) {
  auto check_roundtrip = [](hid_t tid, H5T_class_t expected_cls) {
    std::vector<uint8_t> buf = PublicEncodeType(tid);
    ASSERT_GT(buf.size(), 2);
    ScopedType decoded(H5Tdecode(buf.data(), buf.size()));
    ASSERT_TRUE(decoded.valid());
    EXPECT_EQ(H5Tget_class(decoded.get()), expected_cls);
    EXPECT_GT(H5Tequal(tid, decoded.get()), 0);
  };

  // Integer variants: LE/BE, pad ZERO/ONE, signed/unsigned, offset.
  {
    ScopedType i_be(H5Tcopy(H5T_STD_I32BE));
    ASSERT_GE(H5Tset_pad(i_be.get(), H5T_PAD_ONE, H5T_PAD_ONE), 0);
    ASSERT_GE(H5Tset_offset(i_be.get(), 4), 0);
    ASSERT_GE(H5Tset_precision(i_be.get(), 24), 0);
    check_roundtrip(i_be.get(), H5T_INTEGER);

    ScopedType u_le(H5Tcopy(H5T_STD_U16LE));
    check_roundtrip(u_le.get(), H5T_INTEGER);
  }

  // Float variants: IEEE LE/BE, float16, long double, VAX F32/F64, norms,
  // internal padding.
  {
    check_roundtrip(H5T_IEEE_F32LE, H5T_FLOAT);
    check_roundtrip(H5T_IEEE_F64BE, H5T_FLOAT);
    check_roundtrip(H5T_NATIVE_FLOAT16, H5T_FLOAT);
    check_roundtrip(H5T_NATIVE_LDOUBLE, H5T_FLOAT);
    check_roundtrip(H5T_VAX_F32, H5T_FLOAT);
    check_roundtrip(H5T_VAX_F64, H5T_FLOAT);

    ScopedType flt(H5Tcopy(H5T_IEEE_F32BE));
    ASSERT_GE(H5Tset_pad(flt.get(), H5T_PAD_ONE, H5T_PAD_ONE), 0);
    ASSERT_GE(H5Tset_inpad(flt.get(), H5T_PAD_ONE), 0);
    ASSERT_GE(H5Tset_norm(flt.get(), H5T_NORM_MSBSET), 0);
    check_roundtrip(flt.get(), H5T_FLOAT);

    ASSERT_GE(H5Tset_norm(flt.get(), H5T_NORM_NONE), 0);
    check_roundtrip(flt.get(), H5T_FLOAT);
  }

  // Time variants: LE/BE.
  {
    check_roundtrip(H5T_UNIX_D32LE, H5T_TIME);
    check_roundtrip(H5T_UNIX_D64BE, H5T_TIME);
  }

  // Fixed-length string variants.
  {
    ScopedType str(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(str.get(), 12), 0);
    ASSERT_GE(H5Tset_cset(str.get(), H5T_CSET_UTF8), 0);
    ASSERT_GE(H5Tset_strpad(str.get(), H5T_STR_SPACEPAD), 0);
    check_roundtrip(str.get(), H5T_STRING);
  }

  // Bitfield variants.
  {
    ScopedType bf(H5Tcopy(H5T_STD_B16BE));
    ASSERT_GE(H5Tset_pad(bf.get(), H5T_PAD_ONE, H5T_PAD_ONE), 0);
    ASSERT_GE(H5Tset_offset(bf.get(), 2), 0);
    ASSERT_GE(H5Tset_precision(bf.get(), 10), 0);
    check_roundtrip(bf.get(), H5T_BITFIELD);
  }

  // Opaque variants: empty tag, unaligned length tag, exact 8B tag.
  {
    ScopedType opq(H5Tcreate(H5T_OPAQUE, 16));
    ASSERT_GE(H5Tset_tag(opq.get(), ""), 0);
    check_roundtrip(opq.get(), H5T_OPAQUE);
    ASSERT_GE(H5Tset_tag(opq.get(), "tag1"), 0);
    check_roundtrip(opq.get(), H5T_OPAQUE);
    ASSERT_GE(H5Tset_tag(opq.get(), "exactly8"), 0);
    check_roundtrip(opq.get(), H5T_OPAQUE);
  }

  // Reference variants: v1 object/dsetreg and v4 std ref.
  {
    check_roundtrip(H5T_STD_REF_OBJ, H5T_REFERENCE);
    check_roundtrip(H5T_STD_REF_DSETREG, H5T_REFERENCE);
    check_roundtrip(H5T_STD_REF, H5T_REFERENCE);
  }

  // Enum, Vlen sequence & VL string, Array, Complex.
  {
    ScopedType enm(H5Tenum_create(H5T_STD_I32LE));
    int32_t v0 = 0;
    int32_t v1 = 42;
    ASSERT_GE(H5Tenum_insert(enm.get(), "ALPHA", &v0), 0);
    ASSERT_GE(H5Tenum_insert(enm.get(), "LONGER_ENUM_VALUE", &v1), 0);
    check_roundtrip(enm.get(), H5T_ENUM);

    ScopedType vl_seq(H5Tvlen_create(enm.get()));
    check_roundtrip(vl_seq.get(), H5T_VLEN);

    ScopedType vl_str(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
    ASSERT_GE(H5Tset_cset(vl_str.get(), H5T_CSET_UTF8), 0);
    check_roundtrip(vl_str.get(), H5T_STRING);

    const hsize_t adims[2] = {2, 3};
    ScopedType arr(H5Tarray_create2(vl_seq.get(), 2, adims));
    check_roundtrip(arr.get(), H5T_ARRAY);

    // Compound with multiple sizes (<256, >=256, >=65536) to cover 1/2/4-byte
    // member offset widths in v3+ encoding.
    ScopedType cmp_small(H5Tcreate(H5T_COMPOUND, 128));
    ASSERT_GE(H5Tinsert(cmp_small.get(), "arr", 0, arr.get()), 0);
    ASSERT_GE(H5Tinsert(cmp_small.get(), "num", 96, H5T_IEEE_F64LE), 0);
    check_roundtrip(cmp_small.get(), H5T_COMPOUND);

    ScopedType cmp_mid(H5Tcreate(H5T_COMPOUND, 512));
    ASSERT_GE(H5Tinsert(cmp_mid.get(), "x", 300, H5T_STD_I32LE), 0);
    check_roundtrip(cmp_mid.get(), H5T_COMPOUND);

    ScopedType cmp_big(H5Tcreate(H5T_COMPOUND, 70000));
    ASSERT_GE(H5Tinsert(cmp_big.get(), "big", 66000, H5T_STD_I32LE), 0);
    check_roundtrip(cmp_big.get(), H5T_COMPOUND);

    check_roundtrip(H5T_NATIVE_FLOAT_COMPLEX, H5T_COMPLEX);
    check_roundtrip(H5T_NATIVE_DOUBLE_COMPLEX, H5T_COMPLEX);
  }
}

TEST(H5OdtypeTest, LegacyVersionsEncodeDecodeSizeCopyResetFree) {
  ScopedFile file = CreateInMemoryFile("dtype_versions.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  // Build types and exercise H5O_DTYPE_VERSION_1, VERSION_2, VERSION_3,
  // VERSION_4, VERSION_5 through direct H5O_msg_* entry points.
  ScopedType i_tid(H5Tcopy(H5T_STD_I32LE));
  ScopedType enm_tid(H5Tenum_create(H5T_STD_I32LE));
  int32_t e0 = 1;
  int32_t e1 = 2;
  ASSERT_GE(H5Tenum_insert(enm_tid.get(), "SHORT", &e0), 0);
  ASSERT_GE(H5Tenum_insert(enm_tid.get(), "EIGHTCH", &e1), 0);

  const hsize_t dims[2] = {2, 3};
  ScopedType arr_tid(H5Tarray_create2(i_tid.get(), 2, dims));

  ScopedType cmp_tid(H5Tcreate(H5T_COMPOUND, 32));
  ASSERT_GE(H5Tinsert(cmp_tid.get(), "a", 0, i_tid.get()), 0);
  ASSERT_GE(H5Tinsert(cmp_tid.get(), "longer_field_name", 8, i_tid.get()), 0);

  ScopedType cplx_tid(H5Tcomplex_create(H5T_IEEE_F32LE));

  auto test_direct_msg_ops = [&](H5T_t* dt, unsigned expected_ver) {
    dt->shared->version = expected_ver;
    const size_t raw_sz = H5O_msg_raw_size(f, H5O_DTYPE_ID, true, dt);
    ASSERT_GT(raw_sz, 0);
    std::vector<uint8_t> raw = EncodeMsg(f, dt);
    EXPECT_EQ(raw.size(), raw_sz);

    H5T_t* dec = static_cast<H5T_t*>(
        H5O_msg_decode(f, nullptr, H5O_DTYPE_ID, raw.size(), raw.data()));
    ASSERT_NE(dec, nullptr);
    EXPECT_EQ(dec->shared->version, expected_ver);

    // Copy with dst == nullptr and caller-provided H5T_t* dst.
    H5T_t* copy1 = static_cast<H5T_t*>(H5O_msg_copy(H5O_DTYPE_ID, dec, nullptr));
    ASSERT_NE(copy1, nullptr);
    EXPECT_EQ(H5T_cmp(dec, copy1, false), 0);
    H5O_msg_free(H5O_DTYPE_ID, copy1);

    H5T_t* copy2_buf = H5FL_CALLOC(H5T_t);
    ASSERT_NE(copy2_buf, nullptr);
    H5T_t* copy2 =
        static_cast<H5T_t*>(H5O_msg_copy(H5O_DTYPE_ID, dec, copy2_buf));
    EXPECT_EQ(copy2, copy2_buf);
    ASSERT_GE(H5O_msg_reset(H5O_DTYPE_ID, copy2), 0);
    H5O_msg_free(H5O_DTYPE_ID, copy2);

    H5O_msg_free(H5O_DTYPE_ID, dec);
  };

  // Compound v1, v2, v3.
  test_direct_msg_ops(UnwrapType(cmp_tid.get()), H5O_DTYPE_VERSION_1);
  test_direct_msg_ops(UnwrapType(cmp_tid.get()), H5O_DTYPE_VERSION_2);
  test_direct_msg_ops(UnwrapType(cmp_tid.get()), H5O_DTYPE_VERSION_3);

  // Enum v1, v2, v3 (covers 8-byte name padding vs unpadded).
  test_direct_msg_ops(UnwrapType(enm_tid.get()), H5O_DTYPE_VERSION_1);
  test_direct_msg_ops(UnwrapType(enm_tid.get()), H5O_DTYPE_VERSION_2);
  test_direct_msg_ops(UnwrapType(enm_tid.get()), H5O_DTYPE_VERSION_3);

  // Array v2 (with 3 reserved bytes + permutations) and v3+.
  test_direct_msg_ops(UnwrapType(arr_tid.get()), H5O_DTYPE_VERSION_2);
  test_direct_msg_ops(UnwrapType(arr_tid.get()), H5O_DTYPE_VERSION_3);

  // Complex v5 and immutable H5T_NATIVE_DOUBLE copy -> reset/free.
  test_direct_msg_ops(UnwrapType(cplx_tid.get()), H5O_DTYPE_VERSION_5);
  {
    H5T_t* imm_copy = static_cast<H5T_t*>(H5O_msg_copy(
        H5O_DTYPE_ID, UnwrapType(H5T_NATIVE_DOUBLE), nullptr));
    ASSERT_NE(imm_copy, nullptr);
    EXPECT_EQ(imm_copy->shared->state, H5T_STATE_RDONLY);
    H5O_msg_free(H5O_DTYPE_ID, imm_copy);
  }

  // Calling H5O__dtype_reset with nullptr directly via class pointer is a no-op.
  EXPECT_GE((H5O_MSG_DTYPE->reset)(nullptr), 0);

  // Copy via H5O_msg_copy on v4 reference (H5T_STD_REF) and v5 complex.
  ScopedType ref_obj2(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(ref_obj2.valid());
  test_direct_msg_ops(UnwrapType(ref_obj2.get()), H5O_DTYPE_VERSION_4);
}

TEST(H5OdtypeTest, DecodeBoundsValidationAndVersionUpgrades) {
  ScopedErrorSilence silence;
  ScopedFile file = CreateInMemoryFile("dtype_decode_err.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  auto expect_decode_fail = [&](const std::vector<uint8_t>& buf,
                                H5O_t* oh = nullptr) {
    EXPECT_EQ(H5O_msg_decode(f, oh, H5O_DTYPE_ID, buf.size(), buf.data()),
              nullptr);
  };

  // Truncated common header (<4 bytes, <8 bytes), bad version (0, 6), size 0,
  // unknown class.
  {
    std::vector<uint8_t> h2 = {0x10, 0x00};
    expect_decode_fail(h2);

    std::vector<uint8_t> h6 = {0x10, 0x00, 0x00, 0x00, 0x04, 0x00};
    expect_decode_fail(h6);

    std::vector<uint8_t> bad_ver;
    AppendHeader(bad_ver, 0, H5T_INTEGER, 0, 4);
    AppendUint16LE(bad_ver, 0);
    AppendUint16LE(bad_ver, 32);
    expect_decode_fail(bad_ver);

    bad_ver.clear();
    AppendHeader(bad_ver, 6, H5T_INTEGER, 0, 4);
    expect_decode_fail(bad_ver);

    std::vector<uint8_t> zero_sz;
    AppendHeader(zero_sz, 1, H5T_INTEGER, 0, 0);
    expect_decode_fail(zero_sz);

    std::vector<uint8_t> bad_cls;
    AppendHeader(bad_cls, 1, H5T_NCLASSES, 0, 4);
    expect_decode_fail(bad_cls);
  }

  // INTEGER decode errors: truncated, offset >= size*8, prec == 0,
  // offset+prec-1 >= size*8.
  {
    std::vector<uint8_t> trunc;
    AppendHeader(trunc, 1, H5T_INTEGER, 0, 4);
    trunc.push_back(0);
    expect_decode_fail(trunc);

    auto make_int = [](uint32_t sz, uint16_t off, uint16_t prec) {
      std::vector<uint8_t> b;
      AppendHeader(b, 1, H5T_INTEGER, 0x09, sz);
      AppendUint16LE(b, off);
      AppendUint16LE(b, prec);
      return b;
    };
    expect_decode_fail(make_int(4, 32, 8));
    expect_decode_fail(make_int(4, 0, 0));
    expect_decode_fail(make_int(4, 16, 20));
  }

  // Unusual unused numeric bits check against v1 (non-checksummed) ohdr
  // without vs with RFIC flag, and against v2 (checksummed) ohdr.
  {
    std::vector<uint8_t> unusual;
    AppendHeader(unusual, 1, H5T_INTEGER, 0x08, 4);  // size 4 bytes = 32 bits
    AppendUint16LE(unusual, 0);
    AppendUint16LE(unusual, 4);  // precision 4 -> 28 unused bits!

    // Helper to create a file and open its root group object header.
    auto check_oh_rfic = [&](H5F_libver_t low, H5F_libver_t high, bool rfic,
                             bool expect_ok) {
      ScopedPlist fapl = MakeCoreFapl();
      ASSERT_TRUE(fapl.valid());
      ASSERT_GE(H5Pset_libver_bounds(fapl.get(), low, high), 0);
      if (rfic) {
        ASSERT_GE(H5Pset_relax_file_integrity_checks(
                      fapl.get(), H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS),
                  0);
      }
      ScopedFile rf_file(
          H5Fcreate("rfic.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(rf_file.valid());
      ScopedGroup grp(H5Gcreate2(rf_file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
      const H5O_loc_t* gloc = H5O_get_loc(grp.get());
      ASSERT_NE(gloc, nullptr);
      ScopedApiContext ctx;
      H5O_t* oh = H5O_protect(gloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      absl::Cleanup unprot = [&] {
        H5O_unprotect(gloc, oh, H5AC__NO_FLAGS_SET);
      };
      H5T_t* dec = static_cast<H5T_t*>(H5O_msg_decode(
          gloc->file, oh, H5O_DTYPE_ID, unusual.size(), unusual.data()));
      if (expect_ok) {
        ASSERT_NE(dec, nullptr);
        H5O_msg_free(H5O_DTYPE_ID, dec);
      } else {
        EXPECT_EQ(dec, nullptr);
      }
    };

    // v1 object header without checksum and without RFIC -> rejects unusual bits.
    check_oh_rfic(H5F_LIBVER_EARLIEST, H5F_LIBVER_V18, false, false);
    // v1 object header with RFIC -> allows unusual bits.
    check_oh_rfic(H5F_LIBVER_EARLIEST, H5F_LIBVER_V18, true, true);
    // v2 object header has checksum -> allows unusual bits.
    check_oh_rfic(H5F_LIBVER_V18, H5F_LIBVER_LATEST, false, true);
  }

  // FLOAT decode errors: bad VAX flags, unknown norm, sign OOB, truncated
  // fields, esize==0, epos OOB, epos+esize OOB, msize==0, mpos OOB,
  // mpos+msize OOB, truncated ebias, overlap sign/exp, overlap sign/mant,
  // overlap exp/mant.
  {
    auto make_float = [](uint8_t ver, uint32_t flags, uint32_t sz,
                         uint16_t off, uint16_t prec, uint8_t epos,
                         uint8_t esize, uint8_t mpos, uint8_t msize,
                         uint32_t ebias) {
      std::vector<uint8_t> b;
      AppendHeader(b, ver, H5T_FLOAT, flags, sz);
      AppendUint16LE(b, off);
      AppendUint16LE(b, prec);
      b.push_back(epos);
      b.push_back(esize);
      b.push_back(mpos);
      b.push_back(msize);
      AppendUint32LE(b, ebias);
      return b;
    };

    // Valid IEEE 32LE: flags=(0x20 /*implied*/ | (31<<8)), sz=4, epos=23,esize=8
    expect_decode_fail(
        make_float(3, 0x0040U | 0x20U | (31U << 8), 4, 0, 32, 23, 8, 0, 23, 127));
    expect_decode_fail(
        make_float(1, 0x30U | (31U << 8), 4, 0, 32, 23, 8, 0, 23, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (32U << 8), 4, 0, 32, 23, 8, 0, 23, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (31U << 8), 4, 0, 32, 23, 0, 0, 23, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (31U << 8), 4, 0, 32, 32, 8, 0, 23, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (31U << 8), 4, 0, 32, 26, 8, 0, 23, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (31U << 8), 4, 0, 32, 23, 8, 0, 0, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (31U << 8), 4, 0, 32, 23, 8, 32, 8, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (31U << 8), 4, 0, 32, 23, 8, 25, 10, 127));
    // Overlaps: sign/exp, sign/mant, exp/mant.
    expect_decode_fail(
        make_float(1, 0x20U | (25U << 8), 4, 0, 32, 23, 8, 0, 23, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (10U << 8), 4, 0, 32, 23, 8, 0, 23, 127));
    expect_decode_fail(
        make_float(1, 0x20U | (31U << 8), 4, 0, 32, 20, 8, 0, 23, 127));

    // Float truncation at each step: 8B, 11B, 13B, 15B, 18B.
    std::vector<uint8_t> good_f =
        make_float(1, 0x20U | (31U << 8), 4, 0, 32, 23, 8, 0, 23, 127);
    for (size_t t : {8UL, 11UL, 13UL, 15UL, 18UL}) {
      expect_decode_fail(std::vector<uint8_t>(good_f.begin(), good_f.begin() + t));
    }
  }

  // TIME & BITFIELD & OPAQUE decode errors.
  {
    std::vector<uint8_t> time_trunc;
    AppendHeader(time_trunc, 1, H5T_TIME, 0, 4);
    time_trunc.push_back(0);
    expect_decode_fail(time_trunc);

    std::vector<uint8_t> bf_trunc;
    AppendHeader(bf_trunc, 1, H5T_BITFIELD, 0, 2);
    bf_trunc.push_back(0);
    expect_decode_fail(bf_trunc);

    auto make_bf = [](uint32_t sz, uint16_t off, uint16_t prec) {
      std::vector<uint8_t> b;
      AppendHeader(b, 1, H5T_BITFIELD, 0, sz);
      AppendUint16LE(b, off);
      AppendUint16LE(b, prec);
      return b;
    };
    expect_decode_fail(make_bf(2, 16, 4));
    expect_decode_fail(make_bf(2, 0, 0));
    expect_decode_fail(make_bf(2, 8, 12));

    // Opaque: unaligned tag flag (z & 0x7 != 0), truncated tag bytes.
    std::vector<uint8_t> opq_unaligned;
    AppendHeader(opq_unaligned, 1, H5T_OPAQUE, 3, 4);
    expect_decode_fail(opq_unaligned);

    std::vector<uint8_t> opq_trunc;
    AppendHeader(opq_trunc, 1, H5T_OPAQUE, 16, 4);
    opq_trunc.push_back('a');
    expect_decode_fail(opq_trunc);
  }

  // REFERENCE decode errors: bad rtype, bad v4 ref version.
  {
    std::vector<uint8_t> bad_ref;
    AppendHeader(bad_ref, 4, H5T_REFERENCE, H5R_MAXTYPE, 16);
    expect_decode_fail(bad_ref);

    bad_ref.clear();
    AppendHeader(bad_ref, 4, H5T_REFERENCE,
                 static_cast<uint32_t>(H5R_OBJECT2) | (0x0fU << 4), 16);
    expect_decode_fail(bad_ref);
  }

  // COMPOUND decode errors & v1 intrinsic array upgrade.
  {
    std::vector<uint8_t> cmp0;
    AppendHeader(cmp0, 3, H5T_COMPOUND, 0, 4);
    expect_decode_fail(cmp0);

    // Non-NUL-terminated field name (bounded p_size -> skip=false).
    std::vector<uint8_t> no_nul;
    AppendHeader(no_nul, 3, H5T_COMPOUND, 1, 4);
    no_nul.push_back('a');
    no_nul.push_back('b');
    expect_decode_fail(no_nul);

    // Valid int32LE payload helper (12 bytes).
    std::vector<uint8_t> int32_msg;
    AppendHeader(int32_msg, 1, H5T_INTEGER, 0x08, 4);
    AppendUint16LE(int32_msg, 0);
    AppendUint16LE(int32_msg, 32);

    // Duplicated compound member name fails before H5MM_xstrdup().
    auto append_v3_member = [&](std::vector<uint8_t>& out, const char* name,
                                uint8_t off) {
      for (const char* p = name; *p != '\0'; ++p) {
        out.push_back(static_cast<uint8_t>(*p));
      }
      out.push_back(0);
      out.push_back(off);
      out.insert(out.end(), int32_msg.begin(), int32_msg.end());
    };

    std::vector<uint8_t> dup;
    AppendHeader(dup, 3, H5T_COMPOUND, 2, 16);
    append_v3_member(dup, "same", 0);
    append_v3_member(dup, "same", 4);
    expect_decode_fail(dup);

    // Corrupt member datatype fails recursive decode cleanly (frees memb.name).
    std::vector<uint8_t> bad_memb;
    AppendHeader(bad_memb, 3, H5T_COMPOUND, 1, 8);
    bad_memb.push_back('x');
    bad_memb.push_back(0);
    bad_memb.push_back(0);
    AppendHeader(bad_memb, 1, H5T_NCLASSES, 0, 4);
    expect_decode_fail(bad_memb);

    // v1 compound with padded name, 4B offset, and intrinsic array metadata:
    auto make_v1_array_member_msg = [&](uint8_t ndims, uint32_t dim0,
                                        uint32_t dim1) {
      std::vector<uint8_t> b;
      AppendHeader(b, 1, H5T_COMPOUND, 1, 24);
      // 8-byte padded name "a"
      b.push_back('a');
      b.resize(b.size() + 7, 0);
      AppendUint32LE(b, 0);  // offset
      b.push_back(ndims);
      b.push_back(0);
      b.push_back(0);
      b.push_back(0);         // reserved
      AppendUint32LE(b, 0);   // perm
      AppendUint32LE(b, 0);   // reserved
      AppendUint32LE(b, dim0);
      AppendUint32LE(b, dim1);
      AppendUint32LE(b, 0);
      AppendUint32LE(b, 0);
      b.insert(b.end(), int32_msg.begin(), int32_msg.end());
      return b;
    };

    // Valid v1 intrinsic 2D array member (2x3 ints = 24 bytes):
    std::vector<uint8_t> v1_arr_cmp = make_v1_array_member_msg(2, 2, 3);
    // Decode normally -> upgrades compound to v2!
    H5T_t* upgraded = static_cast<H5T_t*>(H5O_msg_decode(
        f, nullptr, H5O_DTYPE_ID, v1_arr_cmp.size(), v1_arr_cmp.data()));
    ASSERT_NE(upgraded, nullptr);
    EXPECT_EQ(upgraded->shared->version, H5O_DTYPE_VERSION_2);
    EXPECT_EQ(upgraded->shared->u.compnd.memb[0].type->shared->type, H5T_ARRAY);
    H5O_msg_free(H5O_DTYPE_ID, upgraded);

    // Decode with H5O_DECODEIO_NOCHANGE preserves v1 version on array member.
    unsigned nochange_flags = H5O_DECODEIO_NOCHANGE;
    H5T_t* kept_v1 = static_cast<H5T_t*>((H5O_MSG_DTYPE->decode)(
        f, nullptr, 0, &nochange_flags, v1_arr_cmp.size(), v1_arr_cmp.data()));
    ASSERT_NE(kept_v1, nullptr);
    EXPECT_EQ(kept_v1->shared->version, H5O_DTYPE_VERSION_1);
    EXPECT_EQ(kept_v1->shared->u.compnd.memb[0].type->shared->version,
              H5O_DTYPE_VERSION_1);
    H5O_msg_free(H5O_DTYPE_ID, kept_v1);

    // Nested v1 compound containing v1 array-member compound propagates upgrade
    // up through outer compound as well.
    std::vector<uint8_t> nested_v1_cmp;
    AppendHeader(nested_v1_cmp, 1, H5T_COMPOUND, 1, 24);
    nested_v1_cmp.push_back('c');
    nested_v1_cmp.resize(nested_v1_cmp.size() + 7, 0);
    AppendUint32LE(nested_v1_cmp, 0);
    nested_v1_cmp.push_back(0);  // ndims = 0
    nested_v1_cmp.resize(nested_v1_cmp.size() + 3 + 4 + 4 + 16, 0);
    nested_v1_cmp.insert(nested_v1_cmp.end(), v1_arr_cmp.begin(),
                         v1_arr_cmp.end());
    H5T_t* nested_upg = static_cast<H5T_t*>(H5O_msg_decode(
        f, nullptr, H5O_DTYPE_ID, nested_v1_cmp.size(), nested_v1_cmp.data()));
    ASSERT_NE(nested_upg, nullptr);
    EXPECT_EQ(nested_upg->shared->version, H5O_DTYPE_VERSION_2);
    H5O_msg_free(H5O_DTYPE_ID, nested_upg);

    // Invalid ndims > 4 and dim[0] == 0 clean up memb.name without leaking.
    expect_decode_fail(make_v1_array_member_msg(5, 1, 1));
    expect_decode_fail(make_v1_array_member_msg(2, 0, 3));

    // Compound member with higher version (v4 STD_REF inside v2 compound).
    std::vector<uint8_t> cmp_v2_ref;
    AppendHeader(cmp_v2_ref, 2, H5T_COMPOUND, 1, 16);
    cmp_v2_ref.push_back('r');
    cmp_v2_ref.resize(cmp_v2_ref.size() + 7, 0);
    AppendUint32LE(cmp_v2_ref, 0);
    AppendHeader(cmp_v2_ref, 4, H5T_REFERENCE, H5R_OBJECT2 | (1U << 4), 16);
    unsigned iof = 0;
    H5T_t* upgraded_ref_cmp = static_cast<H5T_t*>((H5O_MSG_DTYPE->decode)(
        f, nullptr, 0, &iof, cmp_v2_ref.size(), cmp_v2_ref.data()));
    ASSERT_NE(upgraded_ref_cmp, nullptr);
    EXPECT_GE(upgraded_ref_cmp->shared->version, H5O_DTYPE_VERSION_4);
    H5O_msg_free(H5O_DTYPE_ID, upgraded_ref_cmp);
  }

  // ENUM decode errors and version upgrade.
  {
    std::vector<uint8_t> int32_msg;
    AppendHeader(int32_msg, 1, H5T_INTEGER, 0x08, 4);
    AppendUint16LE(int32_msg, 0);
    AppendUint16LE(int32_msg, 32);

    // Bad parent decode.
    std::vector<uint8_t> bad_parent;
    AppendHeader(bad_parent, 3, H5T_ENUM, 1, 4);
    AppendHeader(bad_parent, 1, H5T_NCLASSES, 0, 4);
    expect_decode_fail(bad_parent);

    // Size mismatch vs parent.
    std::vector<uint8_t> bad_sz;
    AppendHeader(bad_sz, 3, H5T_ENUM, 1, 8);
    bad_sz.insert(bad_sz.end(), int32_msg.begin(), int32_msg.end());
    expect_decode_fail(bad_sz);

    // Non-NUL name, 0-length name, truncated values after names complete.
    std::vector<uint8_t> no_nul;
    AppendHeader(no_nul, 3, H5T_ENUM, 1, 4);
    no_nul.insert(no_nul.end(), int32_msg.begin(), int32_msg.end());
    no_nul.push_back('A');
    expect_decode_fail(no_nul);

    std::vector<uint8_t> empty_name;
    AppendHeader(empty_name, 3, H5T_ENUM, 1, 4);
    empty_name.insert(empty_name.end(), int32_msg.begin(), int32_msg.end());
    empty_name.push_back(0);
    expect_decode_fail(empty_name);

    std::vector<uint8_t> trunc_val;
    AppendHeader(trunc_val, 3, H5T_ENUM, 1, 4);
    trunc_val.insert(trunc_val.end(), int32_msg.begin(), int32_msg.end());
    trunc_val.push_back('A');
    trunc_val.push_back(0);
    trunc_val.push_back(1);  // only 1 byte of 4-byte enum value
    expect_decode_fail(trunc_val);

    // Enum version (1) < parent version (3) -> H5O_DTYPE_CHECK_VERSION upgrade.
    std::vector<uint8_t> ver_mismatch;
    AppendHeader(ver_mismatch, 1, H5T_ENUM, 1, 4);
    AppendHeader(ver_mismatch, 3, H5T_INTEGER, 0x08, 4);
    AppendUint16LE(ver_mismatch, 0);
    AppendUint16LE(ver_mismatch, 32);
    ver_mismatch.push_back('E');
    ver_mismatch.resize(ver_mismatch.size() + 7, 0);
    AppendUint32LE(ver_mismatch, 7);
    unsigned iof = 0;
    H5T_t* upgraded = static_cast<H5T_t*>((H5O_MSG_DTYPE->decode)(
        f, nullptr, 0, &iof, ver_mismatch.size(), ver_mismatch.data()));
    ASSERT_NE(upgraded, nullptr);
    EXPECT_TRUE(iof & H5O_DECODEIO_DIRTY);
    EXPECT_EQ(upgraded->shared->version, H5O_DTYPE_VERSION_3);
    H5O_msg_free(H5O_DTYPE_ID, upgraded);
  }

  // VLEN decode errors and version upgrade.
  {
    std::vector<uint8_t> bad_vl;
    AppendHeader(bad_vl, 3, H5T_VLEN, 0, sizeof(hvl_t));
    AppendHeader(bad_vl, 1, H5T_NCLASSES, 0, 4);
    expect_decode_fail(bad_vl);

    std::vector<uint8_t> vl_upg;
    AppendHeader(vl_upg, 1, H5T_VLEN, 0, sizeof(hvl_t));
    AppendHeader(vl_upg, 4, H5T_REFERENCE, H5R_OBJECT2 | (1U << 4), 16);
    unsigned iof = 0;
    H5T_t* upgraded = static_cast<H5T_t*>((H5O_MSG_DTYPE->decode)(
        f, nullptr, 0, &iof, vl_upg.size(), vl_upg.data()));
    ASSERT_NE(upgraded, nullptr);
    EXPECT_TRUE(iof & H5O_DECODEIO_DIRTY);
    EXPECT_EQ(upgraded->shared->version, H5O_DTYPE_VERSION_4);
    H5O_msg_free(H5O_DTYPE_ID, upgraded);
  }

  // ARRAY decode errors and version upgrades.
  {
    std::vector<uint8_t> int32_msg;
    AppendHeader(int32_msg, 1, H5T_INTEGER, 0x08, 4);
    AppendUint16LE(int32_msg, 0);
    AppendUint16LE(int32_msg, 32);

    // Truncated ndims.
    std::vector<uint8_t> arr_trunc_ndims;
    AppendHeader(arr_trunc_ndims, 3, H5T_ARRAY, 0, 8);
    expect_decode_fail(arr_trunc_ndims);

    // Too many dimensions (> H5S_MAX_RANK == 32).
    std::vector<uint8_t> arr_bad_rank;
    AppendHeader(arr_bad_rank, 3, H5T_ARRAY, 0, 8);
    arr_bad_rank.push_back(33);
    expect_decode_fail(arr_bad_rank);

    // v2 truncated reserved / dims / perms, and size mismatch.
    std::vector<uint8_t> arr_v2_good;
    AppendHeader(arr_v2_good, 2, H5T_ARRAY, 0, 8);
    arr_v2_good.push_back(1);
    arr_v2_good.push_back(0);
    arr_v2_good.push_back(0);
    arr_v2_good.push_back(0);
    AppendUint32LE(arr_v2_good, 2);
    AppendUint32LE(arr_v2_good, 0);  // perm
    arr_v2_good.insert(arr_v2_good.end(), int32_msg.begin(), int32_msg.end());
    for (size_t cut : {10UL, 14UL, 18UL}) {
      expect_decode_fail(
          std::vector<uint8_t>(arr_v2_good.begin(), arr_v2_good.begin() + cut));
    }

    // Bad array parent.
    std::vector<uint8_t> arr_bad_parent;
    AppendHeader(arr_bad_parent, 3, H5T_ARRAY, 0, 8);
    arr_bad_parent.push_back(1);
    AppendUint32LE(arr_bad_parent, 2);
    AppendHeader(arr_bad_parent, 1, H5T_NCLASSES, 0, 4);
    expect_decode_fail(arr_bad_parent);

    // Array size mismatch (claimed 12 bytes, expected 2*4=8).
    std::vector<uint8_t> arr_bad_sz = arr_v2_good;
    arr_bad_sz[4] = 12;
    expect_decode_fail(arr_bad_sz);

    // Array multiplication overflow (ndims=2, dims={0xffffffff, 0xffffffff}).
    std::vector<uint8_t> arr_ovf;
    AppendHeader(arr_ovf, 3, H5T_ARRAY, 0, 8);
    arr_ovf.push_back(2);
    AppendUint32LE(arr_ovf, 0xffffffffU);
    AppendUint32LE(arr_ovf, 0xffffffffU);
    arr_ovf.insert(arr_ovf.end(), int32_msg.begin(), int32_msg.end());
    expect_decode_fail(arr_ovf);

    // Version < 2 (version 1) upgrades to v2 and marks DIRTY.
    std::vector<uint8_t> arr_v1 = arr_v2_good;
    arr_v1[0] = static_cast<uint8_t>((H5T_ARRAY & 0x0fU) | (1U << 4));
    unsigned iof = 0;
    H5T_t* arr_upg = static_cast<H5T_t*>((H5O_MSG_DTYPE->decode)(
        f, nullptr, 0, &iof, arr_v1.size(), arr_v1.data()));
    ASSERT_NE(arr_upg, nullptr);
    EXPECT_TRUE(iof & H5O_DECODEIO_DIRTY);
    EXPECT_EQ(arr_upg->shared->version, H5O_DTYPE_VERSION_2);
    H5O_msg_free(H5O_DTYPE_ID, arr_upg);

    // Parent version (4) > array version (2) triggers upgrade to 4.
    std::vector<uint8_t> arr_parent_upg;
    AppendHeader(arr_parent_upg, 2, H5T_ARRAY, 0, 32);
    arr_parent_upg.push_back(1);
    arr_parent_upg.push_back(0);
    arr_parent_upg.push_back(0);
    arr_parent_upg.push_back(0);
    AppendUint32LE(arr_parent_upg, 2);
    AppendUint32LE(arr_parent_upg, 0);
    AppendHeader(arr_parent_upg, 4, H5T_REFERENCE, H5R_OBJECT2 | (1U << 4), 16);
    iof = 0;
    H5T_t* arr_upg4 = static_cast<H5T_t*>((H5O_MSG_DTYPE->decode)(
        f, nullptr, 0, &iof, arr_parent_upg.size(), arr_parent_upg.data()));
    ASSERT_NE(arr_upg4, nullptr);
    EXPECT_TRUE(iof & H5O_DECODEIO_DIRTY);
    EXPECT_EQ(arr_upg4->shared->version, H5O_DTYPE_VERSION_4);
    H5O_msg_free(H5O_DTYPE_ID, arr_upg4);
  }

  // COMPLEX decode errors and version upgrade.
  {
    std::vector<uint8_t> flt_msg =
        EncodeMsg(f, UnwrapType(H5T_IEEE_F32LE));

    // Not homogeneous.
    std::vector<uint8_t> non_hom;
    AppendHeader(non_hom, 5, H5T_COMPLEX, 0x00, 8);
    non_hom.insert(non_hom.end(), flt_msg.begin(), flt_msg.end());
    expect_decode_fail(non_hom);

    // Non-rectangular form (polar).
    std::vector<uint8_t> non_rect;
    AppendHeader(non_rect, 5, H5T_COMPLEX,
                 0x01 | (static_cast<uint32_t>(H5T_COMPLEX_POLAR) << 1), 8);
    non_rect.insert(non_rect.end(), flt_msg.begin(), flt_msg.end());
    expect_decode_fail(non_rect);

    // Invalid high flag bits set.
    std::vector<uint8_t> bad_flags;
    AppendHeader(bad_flags, 5, H5T_COMPLEX, 0x09, 8);
    bad_flags.insert(bad_flags.end(), flt_msg.begin(), flt_msg.end());
    expect_decode_fail(bad_flags);

    // Invalid base type.
    std::vector<uint8_t> bad_base;
    AppendHeader(bad_base, 5, H5T_COMPLEX, 0x01, 8);
    AppendHeader(bad_base, 1, H5T_NCLASSES, 0, 4);
    expect_decode_fail(bad_base);

    // Complex with version < 5 (v3) marks DIRTY via H5O_DTYPE_CHECK_VERSION.
    std::vector<uint8_t> cplx_v3;
    AppendHeader(cplx_v3, 3, H5T_COMPLEX, 0x01, 8);
    cplx_v3.insert(cplx_v3.end(), flt_msg.begin(), flt_msg.end());
    unsigned iof = 0;
    H5T_t* cplx_upg = static_cast<H5T_t*>((H5O_MSG_DTYPE->decode)(
        f, nullptr, 0, &iof, cplx_v3.size(), cplx_v3.data()));
    ASSERT_NE(cplx_upg, nullptr);
    EXPECT_TRUE(iof & H5O_DECODEIO_DIRTY);
    H5O_msg_free(H5O_DTYPE_ID, cplx_upg);
  }
}

TEST(H5OdtypeTest, EncodeUnsupportedFormatErrors) {
  ScopedErrorSilence silence;
  ScopedFile file = CreateInMemoryFile("dtype_encode_err.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  std::vector<uint8_t> out(256, 0);

  // Integer unsupported order / lsb_pad / msb_pad / sign.
  {
    ScopedType tid(H5Tcopy(H5T_STD_I32LE));
    H5T_t* dt = UnwrapType(tid.get());

    const auto orig_order = dt->shared->u.atomic.order;
    dt->shared->u.atomic.order = H5T_ORDER_VAX;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.order = orig_order;

    const auto orig_lsb = dt->shared->u.atomic.lsb_pad;
    dt->shared->u.atomic.lsb_pad = H5T_PAD_BACKGROUND;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.lsb_pad = orig_lsb;

    const auto orig_msb = dt->shared->u.atomic.msb_pad;
    dt->shared->u.atomic.msb_pad = static_cast<H5T_pad_t>(99);
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.msb_pad = orig_msb;

    const auto orig_sign = dt->shared->u.atomic.u.i.sign;
    dt->shared->u.atomic.u.i.sign = H5T_SGN_ERROR;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.u.i.sign = orig_sign;
  }

  // Float unsupported order / lsb_pad / msb_pad / f.pad / f.norm.
  {
    ScopedType tid(H5Tcopy(H5T_IEEE_F32LE));
    H5T_t* dt = UnwrapType(tid.get());

    const auto orig_order = dt->shared->u.atomic.order;
    dt->shared->u.atomic.order = H5T_ORDER_MIXED;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.order = orig_order;

    const auto orig_lsb = dt->shared->u.atomic.lsb_pad;
    dt->shared->u.atomic.lsb_pad = H5T_PAD_BACKGROUND;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.lsb_pad = orig_lsb;

    const auto orig_msb = dt->shared->u.atomic.msb_pad;
    dt->shared->u.atomic.msb_pad = static_cast<H5T_pad_t>(99);
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.msb_pad = orig_msb;

    const auto orig_fpad = dt->shared->u.atomic.u.f.pad;
    dt->shared->u.atomic.u.f.pad = H5T_PAD_BACKGROUND;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.u.f.pad = orig_fpad;

    const auto orig_norm = dt->shared->u.atomic.u.f.norm;
    dt->shared->u.atomic.u.f.norm = H5T_NORM_ERROR;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), dt), 0);
    dt->shared->u.atomic.u.f.norm = orig_norm;
  }

  // Time & Bitfield unsupported order / padding.
  {
    ScopedType tm_tid(H5Tcopy(H5T_UNIX_D32LE));
    H5T_t* tm = UnwrapType(tm_tid.get());
    const auto orig_order = tm->shared->u.atomic.order;
    tm->shared->u.atomic.order = H5T_ORDER_NONE;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), tm), 0);
    tm->shared->u.atomic.order = orig_order;

    ScopedType bf_tid(H5Tcopy(H5T_STD_B16LE));
    H5T_t* bf = UnwrapType(bf_tid.get());
    const auto bf_order = bf->shared->u.atomic.order;
    bf->shared->u.atomic.order = H5T_ORDER_NONE;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), bf), 0);
    bf->shared->u.atomic.order = bf_order;

    const auto bf_lsb = bf->shared->u.atomic.lsb_pad;
    bf->shared->u.atomic.lsb_pad = H5T_PAD_BACKGROUND;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), bf), 0);
    bf->shared->u.atomic.lsb_pad = bf_lsb;

    const auto bf_msb = bf->shared->u.atomic.msb_pad;
    bf->shared->u.atomic.msb_pad = H5T_PAD_BACKGROUND;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), bf), 0);
    bf->shared->u.atomic.msb_pad = bf_msb;
  }

  // Complex non-rectangular & base type encode failure propagation across
  // compound, enum, vlen, array, complex.
  {
    ScopedType base_tid(H5Tcopy(H5T_STD_I32LE));
    ScopedType cmp_tid(H5Tcreate(H5T_COMPOUND, 8));
    ASSERT_GE(H5Tinsert(cmp_tid.get(), "m", 0, base_tid.get()), 0);
    H5T_t* cmp_m = UnwrapType(cmp_tid.get())->shared->u.compnd.memb[0].type;
    const auto cmp_orig = cmp_m->shared->u.atomic.order;
    cmp_m->shared->u.atomic.order = H5T_ORDER_VAX;
    EXPECT_LT(
        H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), UnwrapType(cmp_tid.get())),
        0);
    cmp_m->shared->u.atomic.order = cmp_orig;

    ScopedType enm_tid(H5Tenum_create(H5T_STD_I32LE));
    H5T_t* enm = UnwrapType(enm_tid.get());
    const auto enm_orig = enm->shared->parent->shared->u.atomic.order;
    enm->shared->parent->shared->u.atomic.order = H5T_ORDER_VAX;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), enm), 0);
    enm->shared->parent->shared->u.atomic.order = enm_orig;

    ScopedType vl_tid(H5Tvlen_create(H5T_STD_I32LE));
    H5T_t* vl = UnwrapType(vl_tid.get());
    const auto vl_orig = vl->shared->parent->shared->u.atomic.order;
    vl->shared->parent->shared->u.atomic.order = H5T_ORDER_VAX;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), vl), 0);
    vl->shared->parent->shared->u.atomic.order = vl_orig;

    const hsize_t d1[1] = {2};
    ScopedType arr_tid(H5Tarray_create2(H5T_STD_I32LE, 1, d1));
    H5T_t* arr = UnwrapType(arr_tid.get());
    const auto arr_orig = arr->shared->parent->shared->u.atomic.order;
    arr->shared->parent->shared->u.atomic.order = H5T_ORDER_VAX;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), arr), 0);
    arr->shared->parent->shared->u.atomic.order = arr_orig;

    ScopedType cplx_tid(H5Tcomplex_create(H5T_IEEE_F32LE));
    H5T_t* cplx = UnwrapType(cplx_tid.get());
    const auto cplx_form = cplx->shared->u.cplx.form;
    cplx->shared->u.cplx.form = H5T_COMPLEX_POLAR;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), cplx), 0);
    cplx->shared->u.cplx.form = cplx_form;

    const auto cplx_base_ord = cplx->shared->parent->shared->u.atomic.order;
    cplx->shared->parent->shared->u.atomic.order = H5T_ORDER_NONE;
    EXPECT_LT(H5O_msg_encode(f, H5O_DTYPE_ID, true, out.data(), cplx), 0);
    cplx->shared->parent->shared->u.atomic.order = cplx_base_ord;
  }
}

TEST(H5OdtypeTest, SharedCommittedSohmAndCopyFile) {
  ScopedErrorSilence silence;
  TempDir tmp("h5odtype_copy");
  const std::string src_path = tmp.child("src.h5").string();
  const std::string dst_path = tmp.child("dst.h5").string();
  const std::string v18_path = tmp.child("v18.h5").string();

  // Check H5O_msg_can_share across immutable, transient, and committed types.
  {
    EXPECT_EQ(H5O_msg_can_share(H5O_DTYPE_ID, UnwrapType(H5T_NATIVE_INT)), 0);
    ScopedType tr(H5Tcopy(H5T_STD_I32LE));
    EXPECT_EQ(H5O_msg_can_share(H5O_DTYPE_ID, UnwrapType(tr.get())), 1);
  }

  // Create source file with SOHM enabled for datatypes.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(
        H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_DTYPE_FLAG, 1), 0);

    ScopedPlist fapl_latest = MakeCoreFapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl_latest.get(), H5F_LIBVER_V18,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile src(H5Fcreate(src_path.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                             H5P_DEFAULT));
    ASSERT_TRUE(src.valid());

    // Committed datatype + dataset + attribute using committed datatype.
    ScopedType named_cmp(H5Tcreate(H5T_COMPOUND, 8));
    ASSERT_GE(H5Tinsert(named_cmp.get(), "x", 0, H5T_STD_I32LE), 0);
    ASSERT_GE(H5Tinsert(named_cmp.get(), "y", 4, H5T_IEEE_F32LE), 0);
    ASSERT_GE(H5Tcommit2(src.get(), "/committed_t", named_cmp.get(),
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_EQ(H5O_msg_can_share(H5O_DTYPE_ID, UnwrapType(named_cmp.get())), 0);

    const hsize_t dims[1] = {2};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ScopedDataset d_named(H5Dcreate2(src.get(), "/d_named", named_cmp.get(),
                                     space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT));
    ASSERT_TRUE(d_named.valid());

    // Two datasets sharing an uncommitted compound type via SOHM.
    ScopedType sohm_cmp(H5Tcreate(H5T_COMPOUND, 12));
    ASSERT_GE(H5Tinsert(sohm_cmp.get(), "a", 0, H5T_STD_I64LE), 0);
    ASSERT_GE(H5Tinsert(sohm_cmp.get(), "b", 8, H5T_IEEE_F32LE), 0);
    ScopedDataset d_sohm1(H5Dcreate2(src.get(), "/d_sohm1", sohm_cmp.get(),
                                     space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT));
    ScopedDataset d_sohm2(H5Dcreate2(src.get(), "/d_sohm2", sohm_cmp.get(),
                                     space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT));
    ASSERT_TRUE(d_sohm1.valid());
    ASSERT_TRUE(d_sohm2.valid());

    // Complex dataset (v5) and transient array dataset (v3) for copy checks.
    ScopedDataset d_cplx(
        H5Dcreate2(src.get(), "/d_cplx", H5T_NATIVE_DOUBLE_COMPLEX, space.get(),
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_cplx.valid());

    // Add attribute with committed type on /d_cplx too.
    ScopedAttr attr(H5Acreate2(d_cplx.get(), "attr_named", named_cmp.get(),
                               space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());

    ASSERT_GE(H5Fflush(src.get(), H5F_SCOPE_GLOBAL), 0);

    // Delete one SOHM link to exercise H5O__dtype_shared_delete.
    ASSERT_GE(H5Ldelete(src.get(), "/d_sohm1", H5P_DEFAULT), 0);
  }

  // Reopen source and copy objects to dst (latest bounds) and v18 (restricted).
  {
    ScopedFile src(H5Fopen(src_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(src.valid());
    ScopedFile dst(H5Fcreate(dst_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(dst.valid());

    // Copy transient/complex dataset (exercises non-committed post_copy_upd),
    // committed dataset (exercises H5O__dtype_set_share + post_copy_upd
    // COMMITTED branch + committed type reopening), and SOHM dataset.
    ASSERT_GE(H5Ocopy(src.get(), "/d_cplx", dst.get(), "/copied_cplx",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(src.get(), "/d_named", dst.get(), "/copied_named",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
    ASSERT_GE(H5Ocopy(src.get(), "/d_sohm2", dst.get(), "/copied_sohm",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);

    // Copy committed type with merge-committed-dtype enabled on pre-existing
    // committed type at destination.
    ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
    ASSERT_TRUE(ocpypl.valid());
    ASSERT_GE(H5Pset_copy_object(ocpypl.get(),
                                 H5O_COPY_MERGE_COMMITTED_DTYPE_FLAG),
              0);
    ASSERT_GE(H5Ocopy(src.get(), "/d_named", dst.get(), "/copied_named_merged",
                      ocpypl.get(), H5P_DEFAULT),
              0);

    // Verify debug output on a committed-dtype object header (/copied_named) and
    // SOHM-dtype object header (/d_sohm2).
    {
      ScopedDataset reopened_named(
          H5Dopen2(dst.get(), "/copied_named", H5P_DEFAULT));
      ASSERT_TRUE(reopened_named.valid());
      const H5O_loc_t* nloc = H5O_get_loc(reopened_named.get());
      FILE* fp = std::tmpfile();
      ASSERT_NE(fp, nullptr);
      absl::Cleanup close_fp = [fp] { std::fclose(fp); };
      {
        ScopedApiContext ctx;
        ASSERT_GE(H5O_debug(nloc->file, nloc->addr, fp, 0, 20), 0);
      }
      EXPECT_THAT(ReadTmpFile(fp), HasSubstr("Shared Message type:"));
    }
    {
      ScopedDataset reopened_sohm(H5Dopen2(src.get(), "/d_sohm2", H5P_DEFAULT));
      ASSERT_TRUE(reopened_sohm.valid());
      const H5O_loc_t* sloc = H5O_get_loc(reopened_sohm.get());
      FILE* fp = std::tmpfile();
      ASSERT_NE(fp, nullptr);
      absl::Cleanup close_fp = [fp] { std::fclose(fp); };
      {
        ScopedApiContext ctx;
        ASSERT_GE(H5O_debug(sloc->file, sloc->addr, fp, 0, 20), 0);
      }
      EXPECT_THAT(ReadTmpFile(fp), HasSubstr("Type class:"));
    }

    // Attempt to copy v5 H5T_COMPLEX dataset to file with V18 high bound
    // (max dtype version 3) -> fails H5O__dtype_pre_copy_file version bounds.
    ScopedPlist fapl_v18(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_libver_bounds(fapl_v18.get(), H5F_LIBVER_V18,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile v18_dst(H5Fcreate(v18_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                 fapl_v18.get()));
    ASSERT_TRUE(v18_dst.valid());
    EXPECT_LT(H5Ocopy(src.get(), "/d_cplx", v18_dst.get(), "/fail_cplx",
                      H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
}

TEST(H5OdtypeTest, DebugOutputCoversAllClassesAndFieldEnums) {
  ScopedFile file = CreateInMemoryFile("dtype_debug.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  auto dump_dtype = [&](const H5T_t* dt) -> std::string {
    FILE* fp = std::tmpfile();
    if (fp == nullptr) {
      return "";
    }
    absl::Cleanup close_fp = [fp] { std::fclose(fp); };
    EXPECT_GE(H5O_debug_id(H5O_DTYPE_ID, f, dt, fp, 0, 24), 0);
    return ReadTmpFile(fp);
  };

  // Integer: unsigned + 1-bit offset + order NONE/VAX/MIXED/ERROR + pads + sign.
  {
    ScopedType tid(H5Tcopy(H5T_STD_I32LE));
    H5T_t* dt = UnwrapType(tid.get());
    dt->shared->u.atomic.offset = 1;
    dt->shared->u.atomic.prec = 1;
    dt->shared->u.atomic.u.i.sign = H5T_SGN_NONE;
    EXPECT_THAT(dump_dtype(dt), HasSubstr("none"));
    EXPECT_THAT(dump_dtype(dt), HasSubstr("1 bit"));

    const auto orig_order = dt->shared->u.atomic.order;
    for (H5T_order_t ord : {H5T_ORDER_LE, H5T_ORDER_BE, H5T_ORDER_VAX,
                            H5T_ORDER_NONE, H5T_ORDER_MIXED, H5T_ORDER_ERROR}) {
      dt->shared->u.atomic.order = ord;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("Byte order:"));
    }
    dt->shared->u.atomic.order = orig_order;

    const auto orig_lsb = dt->shared->u.atomic.lsb_pad;
    const auto orig_msb = dt->shared->u.atomic.msb_pad;
    for (H5T_pad_t pad :
         {H5T_PAD_ZERO, H5T_PAD_ONE, H5T_PAD_BACKGROUND, H5T_PAD_ERROR}) {
      dt->shared->u.atomic.lsb_pad = pad;
      dt->shared->u.atomic.msb_pad = pad;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("High pad type:"));
    }
    dt->shared->u.atomic.lsb_pad = orig_lsb;
    dt->shared->u.atomic.msb_pad = orig_msb;

    const auto orig_sign = dt->shared->u.atomic.u.i.sign;
    for (H5T_sign_t sgn : {H5T_SGN_2, H5T_SGN_NONE, H5T_SGN_ERROR}) {
      dt->shared->u.atomic.u.i.sign = sgn;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("Sign scheme:"));
    }
    dt->shared->u.atomic.u.i.sign = orig_sign;
  }

  // Float: internal pad ZERO/ONE/BG/bit-N/negative, norm IMPLIED/MSBSET/NONE/ERR.
  {
    ScopedType tid(H5Tcopy(H5T_IEEE_F64BE));
    H5T_t* dt = UnwrapType(tid.get());
    const auto orig_pad = dt->shared->u.atomic.u.f.pad;
    const auto orig_norm = dt->shared->u.atomic.u.f.norm;
    for (int pad : {static_cast<int>(H5T_PAD_ZERO),
                    static_cast<int>(H5T_PAD_ONE),
                    static_cast<int>(H5T_PAD_BACKGROUND),
                    static_cast<int>(H5T_PAD_ERROR),
                    7}) {
      dt->shared->u.atomic.u.f.pad = static_cast<H5T_pad_t>(pad);
      EXPECT_THAT(dump_dtype(dt), HasSubstr("Internal pad type:"));
    }
    for (H5T_norm_t nrm :
         {H5T_NORM_IMPLIED, H5T_NORM_MSBSET, H5T_NORM_NONE, H5T_NORM_ERROR}) {
      dt->shared->u.atomic.u.f.norm = nrm;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("Normalization:"));
    }
    dt->shared->u.atomic.u.f.pad = orig_pad;
    dt->shared->u.atomic.u.f.norm = orig_norm;
  }

  // Time, Bitfield, Opaque (1-byte plural/singular check), Reference.
  {
    ScopedType opq1(H5Tcreate(H5T_OPAQUE, 1));
    ASSERT_GE(H5Tset_tag(opq1.get(), "onebyte"), 0);
    EXPECT_THAT(dump_dtype(UnwrapType(opq1.get())), HasSubstr("1 byte\n"));
    EXPECT_THAT(dump_dtype(UnwrapType(H5T_UNIX_D32LE)), HasSubstr("date and time"));
    EXPECT_THAT(dump_dtype(UnwrapType(H5T_STD_B8LE)), HasSubstr("bit field"));
    EXPECT_THAT(dump_dtype(UnwrapType(H5T_STD_REF_OBJ)),
                HasSubstr("Fix dumping reference types!"));
  }

  // Fixed string: cset & pad enums including reserved & error.
  {
    ScopedType tid(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(tid.get(), 4), 0);
    H5T_t* dt = UnwrapType(tid.get());
    const auto orig_cset = dt->shared->u.atomic.u.s.cset;
    const auto orig_pad = dt->shared->u.atomic.u.s.pad;
    for (H5T_cset_t cs :
         {H5T_CSET_ASCII, H5T_CSET_UTF8, H5T_CSET_RESERVED_2, H5T_CSET_ERROR}) {
      dt->shared->u.atomic.u.s.cset = cs;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("Character Set:"));
    }
    for (H5T_str_t sp : {H5T_STR_NULLTERM, H5T_STR_NULLPAD, H5T_STR_SPACEPAD,
                         H5T_STR_RESERVED_3, H5T_STR_ERROR}) {
      dt->shared->u.atomic.u.s.pad = sp;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("String Padding:"));
    }
    dt->shared->u.atomic.u.s.cset = orig_cset;
    dt->shared->u.atomic.u.s.pad = orig_pad;
  }

  // Compound, Enum, Array.
  {
    ScopedType enm_tid(H5Tenum_create(H5T_STD_I32LE));
    int32_t v0 = 5;
    ASSERT_GE(H5Tenum_insert(enm_tid.get(), "FIVE", &v0), 0);
    EXPECT_THAT(dump_dtype(UnwrapType(enm_tid.get())), HasSubstr("Raw bytes of value:"));

    const hsize_t adims[2] = {2, 4};
    ScopedType arr_tid(H5Tarray_create2(H5T_STD_I32LE, 2, adims));
    EXPECT_THAT(dump_dtype(UnwrapType(arr_tid.get())), HasSubstr("{2, 4}"));

    ScopedType cmp_tid(H5Tcreate(H5T_COMPOUND, 12));
    ASSERT_GE(H5Tinsert(cmp_tid.get(), "f0", 0, H5T_STD_I32LE), 0);
    ASSERT_GE(H5Tinsert(cmp_tid.get(), "f1", 4, H5T_IEEE_F64LE), 0);
    EXPECT_THAT(dump_dtype(UnwrapType(cmp_tid.get())), HasSubstr("Member 1:"));
  }

  // Vlen sequence + VL string: vlen type, loc, cset, pad enums.
  {
    ScopedType seq_tid(H5Tvlen_create(H5T_STD_I32LE));
    EXPECT_THAT(dump_dtype(UnwrapType(seq_tid.get())), HasSubstr("sequence"));

    ScopedType vls_tid(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(vls_tid.get(), H5T_VARIABLE), 0);
    H5T_t* dt = UnwrapType(vls_tid.get());
    const auto orig_type = dt->shared->u.vlen.type;
    const auto orig_loc = dt->shared->u.vlen.loc;
    const auto orig_cset = dt->shared->u.vlen.cset;
    const auto orig_pad = dt->shared->u.vlen.pad;

    for (H5T_loc_t loc : {H5T_LOC_MEMORY, H5T_LOC_DISK, H5T_LOC_BADLOC}) {
      dt->shared->u.vlen.loc = loc;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("Location:"));
    }
    for (H5T_cset_t cs :
         {H5T_CSET_ASCII, H5T_CSET_UTF8, H5T_CSET_RESERVED_2, H5T_CSET_ERROR}) {
      dt->shared->u.vlen.cset = cs;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("Character Set:"));
    }
    for (H5T_str_t sp : {H5T_STR_NULLTERM, H5T_STR_NULLPAD, H5T_STR_SPACEPAD,
                         H5T_STR_RESERVED_3, H5T_STR_ERROR}) {
      dt->shared->u.vlen.pad = sp;
      EXPECT_THAT(dump_dtype(dt), HasSubstr("String Padding:"));
    }
    dt->shared->u.vlen.type = H5T_VLEN_BADTYPE;
    EXPECT_THAT(dump_dtype(dt), HasSubstr("Vlen type:"));

    dt->shared->u.vlen.type = orig_type;
    dt->shared->u.vlen.loc = orig_loc;
    dt->shared->u.vlen.cset = orig_cset;
    dt->shared->u.vlen.pad = orig_pad;
  }

  // Complex form enum (RECTANGULAR, POLAR, EXPONENTIAL, invalid) + unknown
  // top-level class.
  {
    ScopedType cplx_tid(H5Tcomplex_create(H5T_IEEE_F32LE));
    H5T_t* dt = UnwrapType(cplx_tid.get());
    const auto orig_form = dt->shared->u.cplx.form;
    for (int fm : {static_cast<int>(H5T_COMPLEX_RECTANGULAR),
                   static_cast<int>(H5T_COMPLEX_POLAR),
                   static_cast<int>(H5T_COMPLEX_EXPONENTIAL), 99}) {
      dt->shared->u.cplx.form = static_cast<H5T_complex_form_t>(fm);
      EXPECT_THAT(dump_dtype(dt), HasSubstr("Form:"));
    }
    dt->shared->u.cplx.form = orig_form;

    const auto orig_cls = dt->shared->type;
    dt->shared->type = H5T_NO_CLASS;
    EXPECT_THAT(dump_dtype(dt), HasSubstr("H5T_CLASS_"));
    dt->shared->type = orig_cls;
  }
}

}  // namespace
}  // namespace hdf5
