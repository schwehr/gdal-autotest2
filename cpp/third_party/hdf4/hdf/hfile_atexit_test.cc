#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

extern "C" {
#include "third_party/hdf4/hdf/src/hfile_atexit_priv.h"
}

namespace {

// Static counter to observe callback invocations.
int g_counter = 0;
std::vector<int>* g_trace = nullptr;

int Callback1() {
  g_counter += 1;
  if (g_trace) g_trace->push_back(1);
  return 0;
}

int Callback2() {
  g_counter += 10;
  if (g_trace) g_trace->push_back(2);
  return 0;
}

int Callback3() {
  g_counter += 100;
  if (g_trace) g_trace->push_back(3);
  return 0;
}

class HfileAtexitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    HEclear();
    g_counter = 0;
    g_trace = nullptr;
  }

  void TearDown() override {
    g_counter = 0;
    g_trace = nullptr;
    HEclear();
  }
};

TEST_F(HfileAtexitTest, CreateNullArgumentFails) {
  EXPECT_EQ(FAIL, hfile_atexit_create(nullptr));
}

TEST_F(HfileAtexitTest, CreateAndDestroy) {
  hfile_atexit_t* ha = nullptr;
  EXPECT_EQ(SUCCEED, hfile_atexit_create(&ha));
  ASSERT_NE(nullptr, ha);

  EXPECT_EQ(SUCCEED, hfile_atexit_destroy(&ha));
  EXPECT_EQ(nullptr, ha);

  // Calling destroy with nullptr or pointer to nullptr succeeds.
  EXPECT_EQ(SUCCEED, hfile_atexit_destroy(nullptr));
  hfile_atexit_t* null_ha = nullptr;
  EXPECT_EQ(SUCCEED, hfile_atexit_destroy(&null_ha));
}

TEST_F(HfileAtexitTest, AddNullContextFails) {
  EXPECT_EQ(FAIL, hfile_atexit_add(nullptr, Callback1));
}

TEST_F(HfileAtexitTest, ExecuteAllNullContextFails) {
  EXPECT_EQ(FAIL, hfile_atexit_execute_all(nullptr));
}

TEST_F(HfileAtexitTest, ExecuteAllRunsInOrder) {
  hfile_atexit_t* ha = nullptr;
  ASSERT_EQ(SUCCEED, hfile_atexit_create(&ha));

  std::vector<int> trace;
  g_trace = &trace;

  EXPECT_EQ(SUCCEED, hfile_atexit_add(ha, Callback1));
  EXPECT_EQ(SUCCEED, hfile_atexit_add(ha, Callback2));
  EXPECT_EQ(SUCCEED, hfile_atexit_add(ha, Callback3));

  EXPECT_EQ(SUCCEED, hfile_atexit_execute_all(ha));
  EXPECT_EQ(111, g_counter);
  EXPECT_THAT(trace, ::testing::ElementsAre(1, 2, 3));

  EXPECT_EQ(SUCCEED, hfile_atexit_destroy(&ha));
}

TEST_F(HfileAtexitTest, ResizesBeyondInitialCapacity) {
  hfile_atexit_t* ha = nullptr;
  ASSERT_EQ(SUCCEED, hfile_atexit_create(&ha));

  // Default initial capacity is ATEXIT_ARRAY_INCR (16).
  // Adding more than 16 functions forces a resize.
  constexpr int kNumCallbacks = 40;
  for (int i = 0; i < kNumCallbacks; ++i) {
    EXPECT_EQ(SUCCEED, hfile_atexit_add(ha, Callback1));
  }

  EXPECT_EQ(SUCCEED, hfile_atexit_execute_all(ha));
  EXPECT_EQ(kNumCallbacks, g_counter);

  EXPECT_EQ(SUCCEED, hfile_atexit_destroy(&ha));
}

}  // namespace
