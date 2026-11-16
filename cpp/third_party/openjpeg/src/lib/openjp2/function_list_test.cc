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
// Tests internal OpenJPEG procedure list utilities
// (src/lib/openjp2/function_list.c).
//
// Caveats:
// - Mutates opj_procedure_list_t internal fields (m_nb_max_procedures,
//   m_nb_procedures, m_procedures) whose layout could change on upgrade.
// - g_trace uses constinit file-scope mutable state because opj_procedure takes
//   a bare `void (*)()` without user_data; ScopedTrace resets it per test.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/function_list.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/function_list.h

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/opj_internal.h"
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"
#include "third_party/openjpeg2/google/util/test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(opj_procedure_list_t) > 0);

// Initial allocation capacity (OPJ_VALIDATION_SIZE) in function_list.c.
constexpr OPJ_UINT32 kInitialCapacity = 10;
constexpr size_t kMaxProcedureCount = 25;

struct ProcedureListDeleter {
  void operator()(opj_procedure_list_t* list) const {
    opj_procedure_list_destroy(list);
  }
};
using ProcedureListPtr =
    std::unique_ptr<opj_procedure_list_t, ProcedureListDeleter>;

struct TraceState {
  std::array<int, kMaxProcedureCount> ids{};
  size_t count = 0;
};

// Closure-less C callbacks registered in opj_procedure_list_t take no
// user_data, so trace state lives in a constinit global reset per test by
// ScopedTrace.
constinit TraceState g_trace{};

// Resets callback trace state before and after each test for
// order-independence.
class ScopedTrace {
 public:
  ScopedTrace() { g_trace = TraceState{}; }
  ~ScopedTrace() { g_trace = TraceState{}; }

  ScopedTrace(const ScopedTrace&) = delete;
  ScopedTrace& operator=(const ScopedTrace&) = delete;

  std::span<const int> calls() const {
    return {g_trace.ids.data(), g_trace.count};
  }
};

template <int kId>
void RecordProcedure() {
  if (g_trace.count < g_trace.ids.size()) {
    g_trace.ids[g_trace.count] = kId;
    ++g_trace.count;
  }
}

template <size_t... Is>
constexpr std::array<opj_procedure, sizeof...(Is)> MakeProceduresImpl(
    std::index_sequence<Is...>) {
  return {&RecordProcedure<static_cast<int>(Is)>...};
}

template <size_t N>
constexpr std::array<opj_procedure, N> MakeProcedures() {
  return MakeProceduresImpl(std::make_index_sequence<N>{});
}

template <size_t... Is>
constexpr std::array<int, sizeof...(Is)> MakeExpectedIdsImpl(
    std::index_sequence<Is...>) {
  return {static_cast<int>(Is)...};
}

template <size_t N>
constexpr std::array<int, N> MakeExpectedIds() {
  return MakeExpectedIdsImpl(std::make_index_sequence<N>{});
}

constexpr auto kProcedures = MakeProcedures<kMaxProcedureCount>();
constexpr auto kExpectedAllIds = MakeExpectedIds<kMaxProcedureCount>();

// Invokes every non-null callback currently stored in `list` in order.
void InvokeAllProcedures(opj_procedure_list_t* list) {
  const OPJ_UINT32 count = opj_procedure_list_get_nb_procedures(list);
  opj_procedure* const first = opj_procedure_list_get_first_procedure(list);
  ASSERT_NE(nullptr, first);
  for (OPJ_UINT32 i = 0; i < count; ++i) {
    ASSERT_NE(nullptr, first[i]);
    first[i]();
  }
}

TEST(FunctionListTest, CreateInitializesEmptyListAndDestroyAcceptsNull) {
  opj_procedure_list_destroy(nullptr);

  ProcedureListPtr list(opj_procedure_list_create());
  ASSERT_NE(nullptr, list);

  EXPECT_EQ(0u, opj_procedure_list_get_nb_procedures(list.get()));
  EXPECT_EQ(kInitialCapacity, list->m_nb_max_procedures);

  opj_procedure* const first =
      opj_procedure_list_get_first_procedure(list.get());
  ASSERT_NE(nullptr, first);
  for (OPJ_UINT32 i = 0; i < kInitialCapacity; ++i) {
    EXPECT_EQ(nullptr, first[i]);
  }
}

TEST(FunctionListTest, AddAndInvokeBeyondInitialCapacityTriggersRealloc) {
  ScopedTrace trace;
  ProcedureListPtr list(opj_procedure_list_create());
  ASSERT_NE(nullptr, list);
  opj_event_mgr_t mgr = MakeEventManager();

  // Add 25 procedures (> OPJ_VALIDATION_SIZE == 10) to trigger opj_realloc()
  // expansion twice (10 -> 20 -> 30).
  for (size_t i = 0; i < kMaxProcedureCount; ++i) {
    ASSERT_EQ(OPJ_TRUE, opj_procedure_list_add_procedure(list.get(),
                                                         kProcedures[i], &mgr));
    EXPECT_EQ(static_cast<OPJ_UINT32>(i + 1),
              opj_procedure_list_get_nb_procedures(list.get()));
  }

  // Note: function_list.c's comment mentions doubling capacity, while the
  // implementation actually increments m_nb_max_procedures by
  // OPJ_VALIDATION_SIZE (10).
  EXPECT_EQ(30u, list->m_nb_max_procedures);

  opj_procedure* const first =
      opj_procedure_list_get_first_procedure(list.get());
  ASSERT_NE(nullptr, first);
  for (size_t i = 0; i < kMaxProcedureCount; ++i) {
    EXPECT_EQ(kProcedures[i], first[i]);
  }

  ASSERT_NO_FATAL_FAILURE(InvokeAllProcedures(list.get()));
  EXPECT_THAT(trace.calls(), ElementsAreArray(kExpectedAllIds));
}

TEST(FunctionListTest, ClearResetsCountPreservesCapacityAndAllowsReuse) {
  ScopedTrace trace;
  ProcedureListPtr list(opj_procedure_list_create());
  ASSERT_NE(nullptr, list);
  opj_event_mgr_t mgr = MakeEventManager();

  // Clearing an already-empty list leaves count and capacity intact.
  opj_procedure_list_clear(list.get());
  EXPECT_EQ(0u, opj_procedure_list_get_nb_procedures(list.get()));

  for (size_t i = 0; i < 3; ++i) {
    ASSERT_EQ(OPJ_TRUE, opj_procedure_list_add_procedure(list.get(),
                                                         kProcedures[i], &mgr));
  }
  ASSERT_EQ(3u, opj_procedure_list_get_nb_procedures(list.get()));

  const OPJ_UINT32 capacity_before_clear = list->m_nb_max_procedures;
  opj_procedure* const ptr_before_clear =
      opj_procedure_list_get_first_procedure(list.get());

  opj_procedure_list_clear(list.get());
  EXPECT_EQ(0u, opj_procedure_list_get_nb_procedures(list.get()));
  EXPECT_EQ(capacity_before_clear, list->m_nb_max_procedures);
  EXPECT_EQ(ptr_before_clear,
            opj_procedure_list_get_first_procedure(list.get()));

  // Re-populate with distinct callbacks and ensure only the new entries run.
  ASSERT_EQ(OPJ_TRUE,
            opj_procedure_list_add_procedure(list.get(), kProcedures[5], &mgr));
  ASSERT_EQ(OPJ_TRUE,
            opj_procedure_list_add_procedure(list.get(), kProcedures[9], &mgr));
  EXPECT_EQ(2u, opj_procedure_list_get_nb_procedures(list.get()));

  ASSERT_NO_FATAL_FAILURE(InvokeAllProcedures(list.get()));
  EXPECT_THAT(trace.calls(), ElementsAre(5, 9));
}

TEST(FunctionListTest, AddProcedureSupportsRepeatedAndNullEntries) {
  ScopedTrace trace;
  ProcedureListPtr list(opj_procedure_list_create());
  ASSERT_NE(nullptr, list);
  opj_event_mgr_t mgr = MakeEventManager();

  ASSERT_EQ(OPJ_TRUE,
            opj_procedure_list_add_procedure(list.get(), kProcedures[2], &mgr));
  ASSERT_EQ(OPJ_TRUE,
            opj_procedure_list_add_procedure(list.get(), nullptr, &mgr));
  ASSERT_EQ(OPJ_TRUE,
            opj_procedure_list_add_procedure(list.get(), kProcedures[2], &mgr));
  ASSERT_EQ(3u, opj_procedure_list_get_nb_procedures(list.get()));

  opj_procedure* const first =
      opj_procedure_list_get_first_procedure(list.get());
  ASSERT_NE(nullptr, first);
  EXPECT_EQ(kProcedures[2], first[0]);
  EXPECT_EQ(nullptr, first[1]);
  EXPECT_EQ(kProcedures[2], first[2]);

  first[0]();
  first[2]();
  EXPECT_THAT(trace.calls(), ElementsAre(2, 2));
}

TEST(FunctionListTest,
     AddProcedureReallocZeroSizeFailureEmitsErrorAndResetsList) {
  ProcedureListPtr list(opj_procedure_list_create());
  ASSERT_NE(nullptr, list);

  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  // function_list.c grows capacity via
  // `m_nb_max_procedures += OPJ_VALIDATION_SIZE` (10) and passes
  // `m_nb_max_procedures * sizeof(opj_procedure)` to opj_realloc().
  // Positioning m_nb_max_procedures at UINT32_MAX - (OPJ_VALIDATION_SIZE - 1)
  // makes OPJ_UINT32 addition wrap modulo 2^32 to 0, so opj_realloc(..., 0)
  // returns nullptr cleanly without exhausting memory or triggering an ASan OOM
  // abort.
  const OPJ_UINT32 step = list->m_nb_max_procedures;
  ASSERT_GT(step, 0u);
  const OPJ_UINT32 wrap_boundary =
      std::numeric_limits<OPJ_UINT32>::max() - (step - 1u);
  list->m_nb_max_procedures = wrap_boundary;
  list->m_nb_procedures = wrap_boundary;

  const OPJ_BOOL added =
      opj_procedure_list_add_procedure(list.get(), kProcedures[0], &mgr);
  // Upstream quirk in function_list.c: the realloc-failure branch calls
  // opj_free(m_procedures) without clearing m_procedures to nullptr, so
  // opj_procedure_list_destroy() would double-free on teardown. Reset
  // m_procedures on failure before RAII teardown (this also covers the
  // m_procedures == nullptr branch in opj_procedure_list_destroy()).
  absl::Cleanup clear_dangling_procedures = [&] {
    if (added == OPJ_FALSE) {
      list->m_procedures = nullptr;
    }
  };

  EXPECT_EQ(OPJ_FALSE, added);
  EXPECT_EQ(0u, opj_procedure_list_get_nb_procedures(list.get()));
  EXPECT_EQ(0u, list->m_nb_max_procedures);
  ASSERT_EQ(1u, events.errors.size());
  EXPECT_THAT(events.errors[0],
              HasSubstr("Not enough memory to add a new validation procedure"));
}

// TODO(schwehr): Cover opj_procedure_list_create() OOM failure branches
// (`!l_validation` and `!l_validation->m_procedures`) via
// :openjpeg2_internal_no_malloc and ScopedOpjAllocFailCountdown.

}  // namespace
}  // namespace openjpeg2
