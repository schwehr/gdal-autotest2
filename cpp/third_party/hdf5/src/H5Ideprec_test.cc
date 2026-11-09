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
// Tests deprecated H5I type registration routine H5Iregister_type1()
// (third_party/hdf5/src/H5Ideprec.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ideprec.c

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

#if defined(__clang__)
#define H5I_TEST_NO_SANITIZE_ENUM __attribute__((no_sanitize("enum")))
#else
#define H5I_TEST_NO_SANITIZE_ENUM
#endif

// H5I_type_t is defined without a fixed underlying type (enumerators [-2, 17]),
// so C++ -fsanitize=enum traps when HDF5 allocates type slots >= 32 (up to
// H5I_MAX_NUM_TYPES = 127). Keep all dynamic type IDs in int outside these
// shims.
H5I_TEST_NO_SANITIZE_ENUM int RegisterType1Raw(size_t hash_size,
                                                unsigned reserved,
                                                H5I_free_t free_func) {
  return static_cast<int>(H5Iregister_type1(hash_size, reserved, free_func));
}

H5I_TEST_NO_SANITIZE_ENUM herr_t DestroyTypeRaw(int type) {
  return H5Idestroy_type(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM htri_t TypeExistsRaw(int type) {
  return H5Itype_exists(static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM hid_t RegisterRaw(int type, const void* obj) {
  return H5Iregister(static_cast<H5I_type_t>(type), obj);
}

H5I_TEST_NO_SANITIZE_ENUM void* ObjectVerifyRaw(hid_t id, int type) {
  return H5Iobject_verify(id, static_cast<H5I_type_t>(type));
}

H5I_TEST_NO_SANITIZE_ENUM herr_t NmembersRaw(int type, hsize_t* num_members) {
  return H5Inmembers(static_cast<H5I_type_t>(type), num_members);
}

struct FreeLog {
  std::vector<int> values;
};

herr_t RecordFreeCallback(void* obj, void** /*request*/) {
  auto* entry = static_cast<std::pair<FreeLog*, int>*>(obj);
  entry->first->values.push_back(entry->second);
  return 0;
}

TEST(H5IdeprecTest, RegisterTypeWithReservedIdsAndFreeCallback) {
  FreeLog log;
  std::pair<FreeLog*, int> first_obj{&log, 10};
  std::pair<FreeLog*, int> second_obj{&log, 20};

  constexpr unsigned kReservedCount = 5;
  const int custom_type =
      RegisterType1Raw(/*hash_size=*/64, kReservedCount, RecordFreeCallback);
  ASSERT_GE(custom_type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy_custom = [custom_type] {
    if (TypeExistsRaw(custom_type) > 0) {
      DestroyTypeRaw(custom_type);
    }
  };

  EXPECT_GT(TypeExistsRaw(custom_type), 0);

  const hid_t id1 = RegisterRaw(custom_type, &first_obj);
  const hid_t id2 = RegisterRaw(custom_type, &second_obj);
  ASSERT_GE(id1, 0);
  ASSERT_GE(id2, 0);
  EXPECT_EQ(id2 - id1, 1);

  // Reserved lower IDs skip the first kReservedCount slots inside the type.
  const int zero_reserved_type =
      RegisterType1Raw(/*hash_size=*/0, /*reserved=*/0, nullptr);
  ASSERT_GE(zero_reserved_type, static_cast<int>(H5I_NTYPES));
  absl::Cleanup destroy_zero_reserved = [zero_reserved_type] {
    if (TypeExistsRaw(zero_reserved_type) > 0) {
      DestroyTypeRaw(zero_reserved_type);
    }
  };
  int dummy = 42;
  const hid_t id0 = RegisterRaw(zero_reserved_type, &dummy);
  ASSERT_GE(id0, 0);
  EXPECT_EQ(static_cast<uint64_t>(id1 & 0xffffffffULL) -
                static_cast<uint64_t>(id0 & 0xffffffffULL),
            kReservedCount);

  EXPECT_EQ(ObjectVerifyRaw(id1, custom_type), &first_obj);
  EXPECT_EQ(ObjectVerifyRaw(id2, custom_type), &second_obj);

  hsize_t count = 0;
  ASSERT_GE(NmembersRaw(custom_type, &count), 0);
  EXPECT_EQ(count, 2);

  EXPECT_EQ(H5Idec_ref(id1), 0);
  ASSERT_EQ(log.values.size(), 1);
  EXPECT_EQ(log.values[0], 10);

  // H5Idestroy_type frees remaining IDs via RecordFreeCallback.
  ASSERT_GE(DestroyTypeRaw(custom_type), 0);
  EXPECT_EQ(TypeExistsRaw(custom_type), 0);
  ASSERT_EQ(log.values.size(), 2);
  EXPECT_EQ(log.values[1], 20);
}

TEST(H5IdeprecTest, RegisterTypeFailsWhenAllTypeSlotsExhausted) {
  std::vector<int> allocated_types;
  absl::Cleanup cleanup_types = [&allocated_types] {
    for (const int type : allocated_types) {
      DestroyTypeRaw(type);
    }
  };

  ScopedErrorSilence silence;
  while (true) {
    const int type = RegisterType1Raw(/*hash_size=*/16, 0, nullptr);
    if (type == static_cast<int>(H5I_BADID)) {
      break;
    }
    allocated_types.push_back(type);
    ASSERT_LE(allocated_types.size(), 256);
  }

  EXPECT_FALSE(allocated_types.empty());
  EXPECT_EQ(RegisterType1Raw(/*hash_size=*/8, 0, nullptr),
            static_cast<int>(H5I_BADID));
}

}  // namespace
}  // namespace hdf5
