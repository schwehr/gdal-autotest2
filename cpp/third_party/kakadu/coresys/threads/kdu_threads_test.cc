// Tests for third_party/kakadu/vendor/coresys/threads/kdu_threads.cpp and
// third_party/kakadu/vendor/coresys/common/kdu_threads.h.

#include "third_party/kakadu/vendor/coresys/common/kdu_threads.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class ThreadSafeThrowingHandler final : public kdu_message {
 public:
  ~ThreadSafeThrowingHandler() noexcept override = default;

  void put_text(const char* text) override {
    if (text == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    last_message_.append(text);
  }

  void flush(bool end_of_message = false) override {
    if (!end_of_message) {
      return;
    }
    throw KDU_ERROR_EXCEPTION;
  }

  std::string TakeMessage() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string out = std::move(last_message_);
    last_message_.clear();
    return out;
  }

 private:
  std::mutex mu_;
  std::string last_message_;
};

class ThreadSafeWarningHandler final : public kdu_message {
 public:
  ~ThreadSafeWarningHandler() noexcept override = default;

  void put_text(const char* text) override {
    if (text == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    messages_.append(text);
  }

  void flush(bool end_of_message = false) override {
    if (!end_of_message) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    ++warning_count_;
    messages_.push_back('\n');
  }

  int WarningCount() {
    std::lock_guard<std::mutex> lock(mu_);
    return warning_count_;
  }

  std::string Messages() {
    std::lock_guard<std::mutex> lock(mu_);
    return messages_;
  }

 private:
  std::mutex mu_;
  int warning_count_ = 0;
  std::string messages_;
};

struct ScopedThreadEntity {
  kdu_thread_entity entity;

  ~ScopedThreadEntity() {
    try {
      if (entity.exists() && entity.is_group_owner() &&
          entity.check_current_thread()) {
        entity.destroy();
      }
    } catch (...) {
    }
  }
};

class KduThreadsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    kdu_customize_errors(&error_handler_);
    kdu_customize_warnings(&warning_handler_);
  }

  void TearDown() override {
    kdu_customize_errors(nullptr);
    kdu_customize_warnings(nullptr);
  }

  ThreadSafeThrowingHandler error_handler_;
  ThreadSafeWarningHandler warning_handler_;
};

struct TrackedThreadObject final : public kdu_thread_object {
  TrackedThreadObject(const char* name, std::atomic<int>* destroyed_count)
      : kdu_thread_object(name), destroyed_count(destroyed_count) {}
  ~TrackedThreadObject() override { destroyed_count->fetch_add(1); }

  std::atomic<int>* destroyed_count;
};

struct ThreadEntryState {
  kdu_thread* owner_thread = nullptr;
  std::atomic<bool> creator_ready{false};
  std::atomic<int> destroyed_objects{0};
  bool self_ref_matched = false;
  bool set_to_self_rejected = false;
  bool added_unnamed = false;
  bool added_named = false;
  bool duplicate_add_ok = false;
  bool found_named = false;
  bool missing_was_null = false;
};

kdu_thread_startproc_result KDU_THREAD_STARTPROC_CALL_CONVENTION
ThreadEntryCallback(void* arg) {
  auto* state = static_cast<ThreadEntryState*>(arg);
  while (!state->creator_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  kdu_thread* self_ref = kdu_thread::get_thread_ref();
  state->self_ref_matched =
      (self_ref != nullptr && self_ref == state->owner_thread &&
       self_ref->check_self());
  if (self_ref != nullptr) {
    state->set_to_self_rejected = !self_ref->set_to_self();
    auto* unnamed = new TrackedThreadObject(nullptr, &state->destroyed_objects);
    auto* named =
        new TrackedThreadObject("worker_ctx", &state->destroyed_objects);
    state->added_unnamed = self_ref->add_thread_object(unnamed);
    state->added_named = self_ref->add_thread_object(named);
    state->duplicate_add_ok = self_ref->add_thread_object(named);
    state->found_named = (self_ref->find_thread_object("worker_ctx") == named);
    state->missing_was_null =
        (self_ref->find_thread_object("absent_ctx") == nullptr);
  }
  return 0;
}

class RecordingThreadQueue final : public kdu_thread_queue {
 public:
  explicit RecordingThreadQueue(int max_jobs, const char* label = "rec_queue")
      : max_jobs_(max_jobs), label_(label) {}

  int get_max_jobs() override { return max_jobs_; }

  void ScheduleBatch(kdu_thread_job* const jobs[], int count,
                     kdu_thread_entity* caller, bool all_scheduled) {
    schedule_jobs(jobs, count, caller, all_scheduled);
  }

  void ScheduleOne(kdu_thread_job* job, kdu_thread_entity* caller,
                   bool all_scheduled, int bind_options) {
    schedule_job(job, caller, all_scheduled, bind_options);
  }

  void BindRange(kdu_thread_job* const jobs[], int count,
                 kdu_uint32 range_start) {
    bind_jobs(jobs, count, range_start);
  }

  void NoteAllScheduled(kdu_thread_entity* caller) {
    note_all_scheduled(caller);
  }

  void Complete(kdu_thread_entity* caller) { all_done(caller); }

  bool notify_join_called() const { return notify_join_called_.load(); }
  bool terminate_called() const { return terminate_called_.load(); }

 protected:
  void notify_join(kdu_thread_entity* /*caller*/) override {
    notify_join_called_.store(true);
  }

  void request_termination(kdu_thread_entity* /*caller*/) override {
    terminate_called_.store(true);
  }

  void identify_self(kdu_message& msg) const override { msg << label_; }

 private:
  int max_jobs_;
  const char* label_;
  std::atomic<bool> notify_join_called_{false};
  std::atomic<bool> terminate_called_{false};
};

class ZeroJobsAutoCompleteQueue final : public kdu_thread_queue {
 public:
  ZeroJobsAutoCompleteQueue() = default;
  int get_max_jobs() override { return 0; }
};

class RecordingThreadContext final : public kdu_thread_context {
 public:
  explicit RecordingThreadContext(int locks) : locks_(locks) {}

  int num_threads_observed() const { return last_threads_.load(); }
  int exception_count() const { return exception_count_.load(); }

  void WaitForNumThreads(int target) {
    std::unique_lock<std::mutex> lock(threads_mu_);
    threads_cv_.wait(lock, [&] { return last_threads_.load() == target; });
  }

 protected:
  int get_num_locks() override { return locks_; }

  void num_threads_changed(int num_threads) override {
    std::lock_guard<std::mutex> lock(threads_mu_);
    last_threads_.store(num_threads);
    threads_cv_.notify_all();
  }

 private:
  std::mutex threads_mu_;
  std::condition_variable threads_cv_;

  void handle_exception(kdu_thread_entity* caller) override {
    exception_count_.fetch_add(1);
    kdu_thread_context::handle_exception(caller);
  }

 private:
  int locks_;
  std::atomic<int> last_threads_{0};
  std::atomic<int> exception_count_{0};
};

class RecordingEvent final : public kdu_thread_entity_event {
 public:
  void handle_signal(kdu_thread_entity* caller) override {
    if (caller != nullptr && caller->exists()) {
      std::lock_guard<std::mutex> lock(mu_);
      ++signal_count_;
      cv_.notify_all();
    }
  }

  int deregister(kdu_thread_entity* caller, bool in_destructor = false,
                 bool inside_thread_group_lock = false) override {
    ++deregister_calls_;
    return kdu_thread_entity_event::deregister(caller, in_destructor,
                                               inside_thread_group_lock);
  }

  void WaitForAtLeast(int count) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return signal_count_ >= count; });
  }

  int SignalCount() {
    std::lock_guard<std::mutex> lock(mu_);
    return signal_count_;
  }

  int DeregisterCalls() const { return deregister_calls_.load(); }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  int signal_count_ = 0;
  std::atomic<int> deregister_calls_{0};
};

struct CounterJob : public kdu_thread_job {
  std::atomic<int>* completed = nullptr;
  std::atomic<int>* remaining = nullptr;
  RecordingThreadQueue* queue = nullptr;
};

void IncrementJobFunc(kdu_thread_job* base, kdu_thread_entity* caller) {
  auto* job = static_cast<CounterJob*>(base);
  if (job->completed != nullptr) {
    job->completed->fetch_add(1);
  }
  if (job->remaining != nullptr && job->queue != nullptr &&
      job->remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
    job->queue->Complete(caller);
  }
}

void ReleaseGate(std::atomic<int>* remaining, RecordingThreadQueue* queue,
                 kdu_thread_entity* caller) {
  if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
    queue->Complete(caller);
  }
}

void WarmupWorkers(kdu_thread_entity& env) {
  RecordingThreadQueue warmup_q(1, "warmup");
  EXPECT_TRUE(env.attach_queue(&warmup_q, nullptr, "warmup", 0,
                               KDU_THREAD_QUEUE_SAFE_CONTEXT));
  struct WarmupJob final : public kdu_thread_job {
    RecordingThreadQueue* queue = nullptr;
    std::atomic<int> remaining{2};
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
  } job;
  job.queue = &warmup_q;
  job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
    auto* j = static_cast<WarmupJob*>(base);
    ReleaseGate(&j->remaining, j->queue, caller);
    std::lock_guard<std::mutex> lk(j->mu);
    j->done = true;
    j->cv.notify_all();
  });
  warmup_q.ScheduleOne(&job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
  ReleaseGate(&job.remaining, &warmup_q, &env);
  {
    std::unique_lock<std::mutex> lk(job.mu);
    job.cv.wait(lk, [&] { return job.done; });
  }
  EXPECT_TRUE(env.join(&warmup_q));
}

TEST_F(KduThreadsTest, KduThreadSelfAndLifecycle) {
  kdu_thread empty_thread;
  EXPECT_FALSE(empty_thread.exists());
  EXPECT_TRUE(!empty_thread);
  EXPECT_FALSE(empty_thread.destroy());

  kdu_thread self_thread;
  EXPECT_TRUE(self_thread.set_to_self());
  EXPECT_TRUE(self_thread.exists());
  EXPECT_FALSE(!self_thread);
  EXPECT_TRUE(self_thread.check_self());
  EXPECT_TRUE(self_thread == self_thread);
  EXPECT_TRUE(self_thread != empty_thread);
  EXPECT_FALSE(self_thread.destroy());

  std::atomic<int> dummy_destroyed{0};
  TrackedThreadObject stack_obj("not_registered", &dummy_destroyed);
  EXPECT_FALSE(self_thread.add_thread_object(&stack_obj));

  kdu_thread self_created;
  EXPECT_TRUE(self_created.create(nullptr, nullptr));
  EXPECT_TRUE(self_created.exists());
  EXPECT_TRUE(self_created.check_self());
  // create(nullptr, nullptr) delegates to set_to_self(), so destroy() returns
  // false and leaves the self-reference intact.
  EXPECT_FALSE(self_created.destroy());
  EXPECT_TRUE(self_created.exists());
}

TEST_F(KduThreadsTest, KduThreadCreateWorkerObjectsAffinityAndPriority) {
  ThreadEntryState state;
  kdu_thread worker;
  state.owner_thread = &worker;

  EXPECT_TRUE(worker.create(ThreadEntryCallback, &state));
  EXPECT_TRUE(worker.exists());
  EXPECT_FALSE(worker.create(ThreadEntryCallback, &state));
  state.creator_ready.store(true, std::memory_order_release);
  EXPECT_TRUE(worker.destroy());
  EXPECT_FALSE(worker.exists());

  EXPECT_TRUE(state.self_ref_matched);
  EXPECT_TRUE(state.set_to_self_rejected);
  EXPECT_TRUE(state.added_unnamed);
  EXPECT_TRUE(state.added_named);
  EXPECT_TRUE(state.duplicate_add_ok);
  EXPECT_TRUE(state.found_named);
  EXPECT_TRUE(state.missing_was_null);
  EXPECT_EQ(2, state.destroyed_objects.load());

  kdu_thread self;
  EXPECT_TRUE(self.set_to_self());
  (void)self.set_cpu_affinity(1, 0);
  int min_prio = 0;
  int max_prio = 0;
  const int cur_prio = self.get_priority(min_prio, max_prio);
  EXPECT_LE(min_prio, max_prio);
  (void)self.set_priority(cur_prio);
}

TEST_F(KduThreadsTest, AffinityBundlesCopyResetAndNodeQueries) {
  (void)kdu_thread_entity_affinity::check_numa_support(1);
  (void)kdu_thread_entity_affinity::check_numa_support(100000);

  const int count_0 =
      kdu_thread_entity_affinity::find_preferred_bundle_count(0);
  const int count_1 =
      kdu_thread_entity_affinity::find_preferred_bundle_count(1);
  const int count_4 =
      kdu_thread_entity_affinity::find_preferred_bundle_count(4);
  const int count_100 =
      kdu_thread_entity_affinity::find_preferred_bundle_count(100);
  EXPECT_GE(count_0, 1);
  EXPECT_GE(count_1, 1);
  EXPECT_GE(count_4, 1);
  EXPECT_GE(count_100, 0);

  kdu_thread_entity_affinity affinity;
  EXPECT_FALSE(affinity.add_preferred_bundle(-1, 1));
  EXPECT_FALSE(affinity.add_preferred_bundle(count_1 + 10, 1));
  for (int idx = 0; idx < count_1; ++idx) {
    EXPECT_TRUE(affinity.add_preferred_bundle(idx, 1));
  }
  EXPECT_EQ(count_1, affinity.get_num_bundles());
  EXPECT_GE(affinity.get_total_threads(), count_1);

  kdu_int64 mask = 0;
  kdu_int32 context = 0;
  EXPECT_EQ(0, affinity.get_bundle_affinity(-1, mask, context));
  EXPECT_EQ(0, affinity.get_bundle_affinity(count_1, mask, context));
  EXPECT_GT(affinity.get_bundle_affinity(0, mask, context), 0);

  affinity.add_thread_bundle(0, 1, 0);
  EXPECT_EQ(count_1, affinity.get_num_bundles());

  kdu_thread_entity_affinity expanded;
  for (int i = 0; i < 6; ++i) {
    expanded.add_thread_bundle(2, 0x3, i * 2);
  }
  EXPECT_EQ(6, expanded.get_num_bundles());
  EXPECT_EQ(12, expanded.get_total_threads());

  kdu_thread_entity_affinity copy_target;
  copy_target.copy_from(expanded);
  EXPECT_EQ(6, copy_target.get_num_bundles());
  EXPECT_EQ(12, copy_target.get_total_threads());

  kdu_thread_entity_affinity smaller;
  smaller.add_thread_bundle(1, 1, 0);
  copy_target.copy_from(smaller);
  EXPECT_EQ(1, copy_target.get_num_bundles());
  EXPECT_EQ(1, copy_target.get_total_threads());

  copy_target.reset();
  EXPECT_EQ(0, copy_target.get_num_bundles());
  EXPECT_EQ(0, copy_target.get_total_threads());

  kdu_thread_entity_affinity node_affinity;
  EXPECT_EQ(0, node_affinity.add_node_bundle(-1, 2));
  EXPECT_EQ(0, node_affinity.add_node_bundle(0, 0));
  EXPECT_GT(node_affinity.add_node_bundle(0, 4), 0);
  EXPECT_EQ(0, node_affinity.add_node_bundle(10000, 4));
}

TEST_F(KduThreadsTest, ThreadEntityUninitializedAndCustomNewDelete) {
  auto heap_entity = std::unique_ptr<kdu_thread_entity>(new kdu_thread_entity);
  EXPECT_FALSE(heap_entity->exists());
  EXPECT_TRUE(!(*heap_entity));
  EXPECT_FALSE(heap_entity->is_group_owner());
  EXPECT_EQ(nullptr, heap_entity->get_self_identity());
  EXPECT_FALSE(heap_entity->check_current_thread());
  EXPECT_FALSE(heap_entity->check_group_member(heap_entity.get()));
  EXPECT_FALSE(heap_entity->change_group_owner_thread());
  EXPECT_EQ(0, heap_entity->get_num_threads());
  EXPECT_FALSE(heap_entity->add_thread());
  kdu_long owner_jobs = -1;
  EXPECT_EQ(0, heap_entity->get_job_count_stats(owner_jobs));
  EXPECT_EQ(0, owner_jobs);
  kdu_thread_entity_affinity aff;
  EXPECT_FALSE(heap_entity->set_cpu_affinity(aff));
  heap_entity->set_min_thread_concurrency(2);
  heap_entity->set_yield_frequency(4);
  heap_entity->advance_work_domains();
  heap_entity->wait_for_condition("uninit", false);
  heap_entity->signal_condition(nullptr, 0);
  EXPECT_FALSE(heap_entity->suspend());

  RecordingThreadQueue queue(1);
  EXPECT_FALSE(heap_entity->attach_queue(&queue, nullptr, "d"));
  EXPECT_EQ(nullptr, heap_entity->add_queue(nullptr, nullptr, "d"));
  EXPECT_TRUE(heap_entity->join(nullptr));
  EXPECT_TRUE(heap_entity->terminate(nullptr));
  EXPECT_TRUE(heap_entity->destroy());

  auto extra = std::unique_ptr<kdu_thread_entity>(heap_entity->new_instance());
  EXPECT_NE(nullptr, extra.get());
}

TEST_F(KduThreadsTest, EntityCreateAffinityOwnerSwitchAndYield) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;

  kdu_thread_entity_affinity aff;
  aff.add_thread_bundle(2, 1, 0);
  env.create(/*cpu_affinity=*/1, /*also_set_owner_affinity=*/true);
  EXPECT_TRUE(env.exists());
  EXPECT_TRUE(env.is_group_owner());
  EXPECT_EQ(0, env.get_thread_id());
  EXPECT_TRUE(env.check_current_thread());
  EXPECT_EQ(&env, env.get_self_identity());
  EXPECT_TRUE(env.check_group_member(&env));
  EXPECT_TRUE(env.set_cpu_affinity(aff));
  env.set_min_thread_concurrency(2);
  EXPECT_FALSE(env.declare_first_owner_wait_safe(true));
  EXPECT_TRUE(env.declare_first_owner_wait_safe(false));

  EXPECT_TRUE(env.add_thread());
  env.set_yield_frequency(-5);
  env.set_yield_frequency(1);
  EXPECT_EQ(2, env.get_num_threads());

  std::thread switcher([&] {
    EXPECT_EQ(nullptr, env.get_self_identity());
    EXPECT_TRUE(env.change_group_owner_thread());
    EXPECT_EQ(&env, env.get_self_identity());
  });
  switcher.join();
  EXPECT_TRUE(env.change_group_owner_thread());
  EXPECT_TRUE(env.destroy());

  ScopedThreadEntity no_owner_aff;
  no_owner_aff.entity.create(/*cpu_affinity=*/1,
                             /*also_set_owner_affinity=*/false);
  EXPECT_TRUE(no_owner_aff.entity.destroy());
}

TEST_F(KduThreadsTest, QueuesDomainsPalettesPrioritiesAndWorkers) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  EXPECT_TRUE(env.add_thread());
  EXPECT_TRUE(env.add_thread());
  EXPECT_TRUE(env.add_thread("tile"));
  EXPECT_EQ(4, env.get_num_threads());
  EXPECT_EQ(1, env.get_num_threads("tile"));
  EXPECT_EQ(0, env.get_num_threads("missing"));
  env.set_yield_frequency(1);
  WarmupWorkers(env);

  ZeroJobsAutoCompleteQueue root_queue;
  EXPECT_TRUE(env.attach_queue(&root_queue, nullptr, nullptr, 0, 0));
  EXPECT_TRUE(root_queue.is_attached());

  // Schedule 18 jobs (> 14, i.e., multiple KD_PLT_SLOTS=7 palettes) via
  // bind_jobs + schedule_jobs, plus sub-queues with high/urgent/background/safe
  // flags and distinct sequencing indices.
  constexpr int kBatchJobs = 18;
  std::atomic<int> completed{0};
  std::vector<CounterJob> batch_jobs(kBatchJobs);
  std::vector<kdu_thread_job*> batch_ptrs(kBatchJobs);
  for (int i = 0; i < kBatchJobs; ++i) {
    batch_jobs[i].set_job_func(IncrementJobFunc);
    batch_jobs[i].completed = &completed;
    batch_ptrs[i] = &batch_jobs[i];
  }

  RecordingThreadQueue q_tile(kBatchJobs, "tile_batch");
  std::atomic<int> tile_rem{kBatchJobs + 1};
  for (int i = 0; i < kBatchJobs; ++i) {
    batch_jobs[i].remaining = &tile_rem;
    batch_jobs[i].queue = &q_tile;
  }
  EXPECT_TRUE(env.attach_queue(&q_tile, &root_queue, "tile", /*seq=*/1,
                               KDU_THREAD_QUEUE_PRIORITY_HIGH));
  q_tile.BindRange(batch_ptrs.data(), 6, 0);
  q_tile.BindRange(batch_ptrs.data() + 6, kBatchJobs - 6, 6);
  q_tile.ScheduleBatch(batch_ptrs.data(), 10, &env, /*all_scheduled=*/false);
  q_tile.ScheduleBatch(batch_ptrs.data() + 10, kBatchJobs - 10, &env,
                       /*all_scheduled=*/true);
  ReleaseGate(&tile_rem, &q_tile, &env);

  RecordingThreadQueue q_urgent(3, "urgent_seq");
  std::atomic<int> urgent_rem{4};
  EXPECT_TRUE(env.attach_queue(&q_urgent, &root_queue, "tile", /*seq=*/2,
                               KDU_THREAD_QUEUE_PRIORITY_URGENT));
  CounterJob urgent_jobs[3];
  for (auto& job : urgent_jobs) {
    job.set_job_func(IncrementJobFunc);
    job.completed = &completed;
    job.remaining = &urgent_rem;
    job.queue = &q_urgent;
  }
  q_urgent.ScheduleOne(&urgent_jobs[0], &env, false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
  q_urgent.ScheduleOne(&urgent_jobs[1], &env, false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
  q_urgent.ScheduleOne(&urgent_jobs[2], &env, true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
  ReleaseGate(&urgent_rem, &q_urgent, &env);

  RecordingThreadQueue q_bg_safe(2, "bg_safe");
  std::atomic<int> bg_rem{3};
  EXPECT_TRUE(env.attach_queue(
      &q_bg_safe, &root_queue, "bkgnd", 0,
      KDU_THREAD_QUEUE_BACKGROUND | KDU_THREAD_QUEUE_SAFE_CONTEXT));
  CounterJob bg_jobs[2];
  for (auto& job : bg_jobs) {
    job.set_job_func(IncrementJobFunc);
    job.completed = &completed;
    job.remaining = &bg_rem;
    job.queue = &q_bg_safe;
  }
  q_bg_safe.ScheduleOne(&bg_jobs[0], &env, false,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
  q_bg_safe.ScheduleOne(&bg_jobs[1], &env, false,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
  q_bg_safe.NoteAllScheduled(&env);
  ReleaseGate(&bg_rem, &q_bg_safe, &env);

  env.advance_work_domains();
  EXPECT_TRUE(env.join(&root_queue, /*descendants_only=*/true));
  EXPECT_TRUE(env.join(&q_tile));
  EXPECT_TRUE(env.join(nullptr));

  EXPECT_EQ(kBatchJobs + 5, completed.load());
  EXPECT_TRUE(env.destroy());

  // Test get_job_count_stats() on an owner-only group (0 worker threads) so the
  // owner executes all jobs synchronously and avoids reading workers'
  // non-atomic job_counter concurrently (TSan race in upstream
  // kdu_threads.cpp).
  // TODO(schwehr): Re-enable multi-worker get_job_count_stats() check once
  // upstream synchronizes job_counter accesses.
  {
    ScopedThreadEntity owner_only_scoped;
    kdu_thread_entity& owner_only = owner_only_scoped.entity;
    owner_only.create();
    RecordingThreadQueue q_stats(2, "stats");
    EXPECT_TRUE(owner_only.attach_queue(&q_stats, nullptr, "stats_dom"));
    std::atomic<int> stats_completed{0};
    std::atomic<int> stats_rem{3};
    CounterJob stats_jobs[2];
    for (auto& job : stats_jobs) {
      job.set_job_func(IncrementJobFunc);
      job.completed = &stats_completed;
      job.remaining = &stats_rem;
      job.queue = &q_stats;
    }
    q_stats.ScheduleOne(&stats_jobs[0], &owner_only, false,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
    q_stats.ScheduleOne(&stats_jobs[1], &owner_only, true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&stats_rem, &q_stats, &owner_only);
    EXPECT_TRUE(owner_only.join(nullptr));
    EXPECT_EQ(2, stats_completed.load());

    kdu_long owner_count = 0;
    EXPECT_EQ(2, owner_only.get_job_count_stats(owner_count));
    EXPECT_EQ(2, owner_count);
    EXPECT_EQ(0, owner_only.get_job_count_stats(owner_count));
    EXPECT_EQ(0, owner_count);
    EXPECT_TRUE(owner_only.destroy());
  }
}

TEST_F(KduThreadsTest, ManyDomainsExceedInlineActiveStore) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  EXPECT_TRUE(env.add_thread("dom_00"));
  EXPECT_TRUE(env.add_thread());

  constexpr int kNumDomains = 20;
  std::atomic<int> completed{0};
  std::mutex mu;
  std::condition_variable cv;
  bool release_jobs = false;
  int finished_queues = 0;

  struct DomainJob final : public kdu_thread_job {
    RecordingThreadQueue* queue = nullptr;
    std::atomic<int>* completed = nullptr;
    std::atomic<int>* remaining = nullptr;
    std::mutex* mu = nullptr;
    std::condition_variable* cv = nullptr;
    bool* release_jobs = nullptr;
    int* finished_queues = nullptr;
  };

  std::vector<std::string> domain_names;
  std::vector<std::unique_ptr<RecordingThreadQueue>> queues;
  std::vector<std::atomic<int>> rems(kNumDomains);
  std::vector<DomainJob> jobs(kNumDomains);
  domain_names.reserve(kNumDomains);
  queues.reserve(kNumDomains);

  auto run_domain_job = [](kdu_thread_job* base, kdu_thread_entity* caller) {
    auto* j = static_cast<DomainJob*>(base);
    {
      std::unique_lock<std::mutex> lock(*j->mu);
      j->cv->wait(lock, [j] { return *j->release_jobs; });
    }
    j->completed->fetch_add(1, std::memory_order_acq_rel);
    ReleaseGate(j->remaining, j->queue, caller);
    std::condition_variable* done_cv = j->cv;
    {
      std::lock_guard<std::mutex> lock(*j->mu);
      ++(*j->finished_queues);
    }
    done_cv->notify_all();
  };

  for (int d = 0; d < kNumDomains; ++d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "dom_%02d", d);
    domain_names.emplace_back(buf);
    queues.push_back(std::make_unique<RecordingThreadQueue>(1));
    rems[d].store(2, std::memory_order_relaxed);
    EXPECT_TRUE(env.attach_queue(queues.back().get(), nullptr,
                                 domain_names.back().c_str(), d % 3));
    jobs[d].set_job_func(run_domain_job);
    jobs[d].queue = queues.back().get();
    jobs[d].completed = &completed;
    jobs[d].remaining = &rems[d];
    jobs[d].mu = &mu;
    jobs[d].cv = &cv;
    jobs[d].release_jobs = &release_jobs;
    jobs[d].finished_queues = &finished_queues;
    queues.back()->ScheduleOne(&jobs[d], &env, true,
                               KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&rems[d], queues.back().get(), &env);
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    release_jobs = true;
    cv.notify_all();
  }
  {
    // TODO(schwehr): Wait for workers to complete all queues before
    // env.join(nullptr) so the group owner does not expand `active_sets` beyond
    // 16 entries: kdu_thread_entity::destroy() sets `this->active_sets = NULL`
    // without freeing it (kdu_threads.cpp:2668), whereas worker entities free
    // `active_sets` in ~kdu_thread_entity().
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&] { return finished_queues == kNumDomains; });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  env.advance_work_domains();
  EXPECT_TRUE(env.join(nullptr));
  EXPECT_EQ(kNumDomains, completed.load());
  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, NotifyJoinTerminateAddQueueAndForceDetach) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  EXPECT_TRUE(env.add_thread());

  // 1) Top-level queue notified on join(nullptr) calls all_done() once idle.
  class SelfCompleteOnNotifyJoinQueue final : public kdu_thread_queue {
   public:
    int get_max_jobs() override { return 1; }
    void ScheduleOne(kdu_thread_job* job, kdu_thread_entity* caller) {
      gate_.fetch_add(2, std::memory_order_acq_rel);
      schedule_job(job, caller, false, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    void MarkJobDone(kdu_thread_entity* caller) {
      if (gate_.fetch_sub(2, std::memory_order_acq_rel) == 2) {
        all_done(caller);
      }
    }
    bool notify_join_called() const { return notify_join_called_.load(); }

   protected:
    void notify_join(kdu_thread_entity* caller) override {
      if (!notify_join_called_.exchange(true) &&
          gate_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        all_done(caller);
      }
    }

   private:
    std::atomic<int> gate_{1};
    std::atomic<bool> notify_join_called_{false};
  };

  SelfCompleteOnNotifyJoinQueue notify_q;
  EXPECT_TRUE(env.attach_queue(&notify_q, nullptr, "nj"));
  struct NotifyJob final : public kdu_thread_job {
    SelfCompleteOnNotifyJoinQueue* queue = nullptr;
    std::atomic<int>* done_cnt = nullptr;
  } one_job;
  std::atomic<int> done_cnt{0};
  one_job.queue = &notify_q;
  one_job.done_cnt = &done_cnt;
  one_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
    auto* nj = static_cast<NotifyJob*>(base);
    nj->done_cnt->fetch_add(1);
    nj->queue->MarkJobDone(caller);
  });
  notify_q.ScheduleOne(&one_job, &env);
  EXPECT_TRUE(env.join(nullptr));
  EXPECT_TRUE(notify_q.notify_join_called());
  EXPECT_EQ(1, done_cnt.load());

  // 2) Child queue terminated via terminate() receives request_termination().
  struct SelfCompleteOnTermQueue final : public kdu_thread_queue {
    explicit SelfCompleteOnTermQueue(std::atomic<bool>* term_flag)
        : term_flag_(term_flag) {}
    int get_max_jobs() override { return 1; }
    void request_termination(kdu_thread_entity* caller) override {
      term_flag_->store(true);
      all_done(caller);
    }
    std::atomic<bool>* term_flag_;
  };

  ZeroJobsAutoCompleteQueue parent_zero;
  std::atomic<bool> child_terminated{false};
  SelfCompleteOnTermQueue term_child(&child_terminated);
  EXPECT_TRUE(env.attach_queue(&parent_zero, nullptr, nullptr));
  EXPECT_TRUE(env.attach_queue(&term_child, &parent_zero, "term"));
  EXPECT_TRUE(env.terminate(&parent_zero, /*descendants_only=*/true));
  EXPECT_TRUE(child_terminated.load());
  EXPECT_TRUE(env.terminate(&parent_zero, /*descendants_only=*/false));
  EXPECT_FALSE(parent_zero.is_attached());

  // 3) Internal queues via add_queue(nullptr, ...) + force_detach.
  kdu_thread_queue* grp_parent = env.add_queue(nullptr, nullptr, "grp");
  ASSERT_NE(nullptr, grp_parent);
  kdu_thread_queue* grp_child = env.add_queue(nullptr, grp_parent, "grp2");
  ASSERT_NE(nullptr, grp_child);
  EXPECT_TRUE(grp_child->is_attached());
  EXPECT_EQ(0, grp_child->get_sequence_idx());

  RecordingThreadQueue detach_q_caller(1, "detach_caller");
  RecordingThreadQueue detach_q_null(1, "detach_null");
  EXPECT_TRUE(env.attach_queue(&detach_q_caller, nullptr, "det1"));
  EXPECT_TRUE(env.attach_queue(&detach_q_null, nullptr, "det2"));
  detach_q_caller.force_detach(&env);
  EXPECT_FALSE(detach_q_caller.is_attached());
  detach_q_null.force_detach(nullptr);
  EXPECT_FALSE(detach_q_null.is_attached());

  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, RunQueueAndRunQueueJobDependencyRescheduling) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  EXPECT_TRUE(env.add_thread());
  EXPECT_TRUE(env.add_thread("run2"));

  class TestRunQueue final : public kdu_run_queue {
   public:
    void Launch(kdu_run_queue_job* job, kdu_thread_entity* caller) {
      schedule_job(job, caller, false, KDU_THREAD_JOB_REBIND_0);
    }
  };
  TestRunQueue helper_run_queue;
  EXPECT_EQ(0, helper_run_queue.get_max_jobs());
  helper_run_queue.activate();
  EXPECT_EQ(1, helper_run_queue.get_max_jobs());
  EXPECT_TRUE(env.attach_queue(&helper_run_queue, nullptr, "run2"));

  struct SimpleRunJob final : public kdu_run_queue_job {
    std::atomic<int> steps{0};
    std::atomic<int> child_done{0};
    std::atomic<int> child_rem{2};
    TestRunQueue* rq = nullptr;
    RecordingThreadQueue* child_q = nullptr;
    CounterJob child_job;
  } simple;
  simple.rq = &helper_run_queue;
  simple.init(&env);
  simple.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
    auto* j = static_cast<SimpleRunJob*>(base);
    if (j->steps.fetch_add(1) == 0) {
      j->child_q = new RecordingThreadQueue(1, "run_dep_child");
      caller->attach_queue(j->child_q, j->rq, "rc");
      j->rq->update_dependencies(1, 1, caller);
      j->child_job.completed = &j->child_done;
      j->child_job.remaining = &j->child_rem;
      j->child_job.queue = j->child_q;
      j->child_job.set_job_func(IncrementJobFunc);
      j->child_q->ScheduleOne(&j->child_job, caller, true,
                              KDU_THREAD_JOB_AUTO_BIND_ONCE);
      ReleaseGate(&j->child_rem, j->child_q, caller);
      if (!j->rq->check_continue(base, caller)) {
        caller->join(j->child_q);
        delete j->child_q;
        j->child_q = nullptr;
        j->rq->update_dependencies(-1, -1, caller);
        return;
      }
    }
    j->signal_done(caller);
  });

  EXPECT_TRUE(helper_run_queue.check_continue(&simple, &env));
  helper_run_queue.Launch(&simple, &env);
  simple.wait_done(&env);
  EXPECT_EQ(2, simple.steps.load());
  EXPECT_EQ(1, simple.child_done.load());
  helper_run_queue.deactivate(&env);
  EXPECT_TRUE(env.join(&helper_run_queue));
  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, ContextLocksAndThreadEventsAndForeignSignal) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  EXPECT_TRUE(env.add_thread());
  WarmupWorkers(env);

  RecordingThreadContext ctx(/*locks=*/4);
  ctx.enter_group(&env);
  EXPECT_EQ(0, ctx.check_group(&env));
  EXPECT_EQ(2, ctx.num_threads_observed());
  EXPECT_TRUE(env.add_thread());
  ctx.WaitForNumThreads(3);
  EXPECT_EQ(3, ctx.num_threads_observed());

  ctx.acquire_lock(0, &env);
  EXPECT_TRUE(ctx.check_lock(0, &env));

  RecordingThreadQueue lock_probe_q(1, "lock_probe");
  EXPECT_TRUE(env.attach_queue(&lock_probe_q, nullptr, "lp", 0,
                               KDU_THREAD_QUEUE_SAFE_CONTEXT));
  struct LockJob final : public kdu_thread_job {
    RecordingThreadContext* ctx = nullptr;
    kdu_thread_entity* owner = nullptr;
    RecordingThreadQueue* queue = nullptr;
    std::atomic<int> remaining{2};
    std::atomic<bool> contended_failed{false};
    std::atomic<bool> lock1_succeeded{false};
    std::atomic<int> worker_group_idx{-1};
  } lock_job;
  lock_job.ctx = &ctx;
  lock_job.owner = &env;
  lock_job.queue = &lock_probe_q;
  lock_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
    auto* lj = static_cast<LockJob*>(base);
    if (caller != lj->owner) {
      lj->worker_group_idx.store(lj->ctx->check_group(caller));
      lj->contended_failed.store(!lj->ctx->try_lock(0, caller));
      if (lj->ctx->try_lock(1, caller)) {
        lj->lock1_succeeded.store(lj->ctx->check_lock(1, caller));
        lj->ctx->release_lock(1, caller);
      }
    }
    ReleaseGate(&lj->remaining, lj->queue, caller);
  });
  lock_probe_q.ScheduleOne(&lock_job, &env, true,
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);
  ReleaseGate(&lock_job.remaining, &lock_probe_q, &env);
  EXPECT_TRUE(env.join(&lock_probe_q));
  EXPECT_GE(lock_job.worker_group_idx.load(), 1);
  EXPECT_TRUE(lock_job.contended_failed.load());
  EXPECT_TRUE(lock_job.lock1_succeeded.load());
  ctx.release_lock(0, &env);
  EXPECT_FALSE(ctx.check_lock(0, &env));
  // TODO(schwehr): Upstream pointer-arithmetic UB in
  // vendor/coresys/threads/kdu_threads.cpp triggers crashes under modern Clang
  // and UBSan:
  // - signal() (`kdu_threads.cpp:4572`): `((kdu_byte *)NULL) + 3` fails UBSan
  //   `nullptr-with-nonzero-offset` (`--config=asan-chlor`).
  // - deregister() (`kdu_threads.cpp:4644,4686,4742,4753`):
  //   `((kdu_byte *)NULL) + old_s + 2` followed by `(old_ptr - old_s)` lets
  //   Clang eliminate `if (ent != NULL)` and hit `assert(t <
  //   group->num_threads)` whenever any registered event is deregistered
  //   (explicitly or on destruction).
  // Cover all UB-free paths on `kdu_thread_entity_event` (`register_event`
  // rejection after `handle_exception`, inactive `signal()`, `handle_signal()`,
  // `activate_event`/`deactivate_event`, and unregistered `deregister()`).
  RecordingEvent ev;
  ev.handle_signal(&env);
  EXPECT_EQ(1, ev.SignalCount());
  kdu_thread_entity_event base_ev;
  base_ev.handle_signal(&env);
  EXPECT_FALSE(base_ev.signal());
  EXPECT_FALSE(ev.signal());
  env.activate_event(&ev);
  env.deactivate_event(&ev);
  EXPECT_EQ(1, ev.deregister(&env));
  EXPECT_EQ(1, ev.deregister(nullptr));
  EXPECT_EQ(2, ev.DeregisterCalls());

  // Calling `register_event()` after `handle_exception()` cleanly returns
  // `false` because `group->events_allowed` is cleared without mutating `ev`.
  {
    ScopedThreadEntity exc_scoped;
    exc_scoped.entity.create();
    exc_scoped.entity.handle_exception(KDU_ERROR_EXCEPTION);
    EXPECT_FALSE(exc_scoped.entity.register_event(&ev));
    EXPECT_EQ(1, ev.deregister(nullptr));
    (void)error_handler_.TakeMessage();
  }

  // Foreign wakeup condition test.
  kdu_thread_entity_condition* owner_cond = env.get_condition();
  ASSERT_NE(nullptr, owner_cond);
  std::mutex wait_mu;
  std::condition_variable wait_cv;
  bool thread_started = false;
  std::thread foreign_waker([&] {
    {
      std::lock_guard<std::mutex> lk(wait_mu);
      thread_started = true;
    }
    wait_cv.notify_all();
    env.signal_condition(owner_cond, KDU_FOREIGN_SIGNAL_EXPECTED);
  });
  {
    std::unique_lock<std::mutex> lk(wait_mu);
    wait_cv.wait(lk, [&] { return thread_started; });
  }
  env.wait_for_condition("foreign_wait", /*expect_foreign_wakeup=*/true);
  foreign_waker.join();

  // Exercise suspend() and unsuspend_thread() from a worker job.
  RecordingThreadQueue suspend_q(1, "suspend_probe");
  EXPECT_TRUE(env.attach_queue(&suspend_q, nullptr, "sp", 0,
                               KDU_THREAD_QUEUE_SAFE_CONTEXT));
  struct SuspendJob final : public kdu_thread_job {
    std::mutex mu;
    std::condition_variable cv;
    kdu_thread_entity* worker = nullptr;
    kdu_thread_entity* owner = nullptr;
    RecordingThreadQueue* queue = nullptr;
    std::atomic<int> remaining{2};
    bool suspended_done = false;
    std::atomic<bool> suspended_returned{false};
  } suspend_job;
  suspend_job.owner = &env;
  suspend_job.queue = &suspend_q;
  suspend_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
    auto* sj = static_cast<SuspendJob*>(base);
    if (caller != sj->owner) {
      {
        std::lock_guard<std::mutex> lk(sj->mu);
        sj->worker = caller;
      }
      sj->cv.notify_all();
      sj->suspended_returned.store(caller->suspend(/*allow_exceptions=*/false));
      {
        std::lock_guard<std::mutex> lk(sj->mu);
        sj->suspended_done = true;
      }
      sj->cv.notify_all();
    }
    ReleaseGate(&sj->remaining, sj->queue, caller);
  });
  EXPECT_TRUE(env.add_thread("sp"));
  suspend_q.ScheduleOne(&suspend_job, &env, true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
  ReleaseGate(&suspend_job.remaining, &suspend_q, &env);
  {
    std::unique_lock<std::mutex> lk(suspend_job.mu);
    suspend_job.cv.wait(lk, [&] { return suspend_job.worker != nullptr; });
    env.unsuspend_thread(suspend_job.worker);
    suspend_job.cv.wait(lk, [&] { return suspend_job.suspended_done; });
  }
  EXPECT_TRUE(env.join(&suspend_q));
  EXPECT_TRUE(suspend_job.suspended_returned.load());

  // Exception handling unwinds locks and reports error via join().
  ctx.acquire_lock(2, &env);
  env.handle_exception(42);
  EXPECT_FALSE(ctx.check_lock(2, &env));
  EXPECT_GE(ctx.exception_count(), 1);
  kdu_exception out_exc = KDU_NULL_EXCEPTION;
  EXPECT_FALSE(env.join(nullptr, false, &out_exc));
  EXPECT_EQ(42, out_exc);

  ctx.leave_group(&env);
  EXPECT_FALSE(env.destroy());
}

TEST_F(KduThreadsTest, DynamicConditionStackAndContendedLockOnWorkers) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  EXPECT_TRUE(env.add_thread());
  EXPECT_TRUE(env.add_thread());

  RecordingThreadContext ctx(2);
  ctx.enter_group(&env);
  ctx.acquire_lock(0, &env);

  // Worker checks contended try_lock(0) while owner is NOT inside
  // ww_process_jobs yet (sync via std::condition_variable before calling join).
  RecordingThreadQueue q(1, "contend_q");
  EXPECT_TRUE(
      env.attach_queue(&q, nullptr, "ct", 0, KDU_THREAD_QUEUE_SAFE_CONTEXT));
  struct ContendJob final : public kdu_thread_job {
    RecordingThreadContext* ctx = nullptr;
    RecordingThreadQueue* queue = nullptr;
    std::atomic<int> remaining{2};
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool contended_false = false;
    int grp_idx = -1;
  } cjob;
  cjob.ctx = &ctx;
  cjob.queue = &q;
  cjob.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
    auto* j = static_cast<ContendJob*>(base);
    j->grp_idx = j->ctx->check_group(caller);
    j->contended_false = !j->ctx->try_lock(0, caller);
    ReleaseGate(&j->remaining, j->queue, caller);
    std::lock_guard<std::mutex> lk(j->mu);
    j->done = true;
    j->cv.notify_all();
  });
  q.ScheduleOne(&cjob, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
  ReleaseGate(&cjob.remaining, &q, &env);
  {
    std::unique_lock<std::mutex> lk(cjob.mu);
    cjob.cv.wait(lk, [&] { return cjob.done; });
  }
  ctx.release_lock(0, &env);
  EXPECT_TRUE(env.join(&q));
  EXPECT_GE(cjob.grp_idx, 1);
  EXPECT_TRUE(cjob.contended_false);
  ctx.leave_group(&env);

  // Nested foreign-wakeup wait_for_condition > KDU_INITIAL_THREAD_CONDITIONS
  // (8) triggers dynamic allocation of `kdu_thread_entity_condition` and
  // cleanup.
  constexpr int kNestDepth = 10;
  std::atomic<int> depth{0};
  auto nested_wait = [&](auto& self_ref, int remaining) -> void {
    if (remaining == 0) {
      return;
    }
    depth.fetch_add(1);
    kdu_thread_entity_condition* cond = env.get_condition();
    std::thread waker(
        [&] { env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED); });
    env.wait_for_condition("nested_dynamic", true);
    waker.join();
    self_ref(self_ref, remaining - 1);
  };
  nested_wait(nested_wait, kNestDepth);
  EXPECT_EQ(kNestDepth, depth.load());
  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, ErrorAndDeadlockPathsThrowAndCleanupCleanly) {
  // 1) KDU_THREAD_QUEUE_SAFE_CONTEXT without worker threads.
  {
    ScopedThreadEntity scoped;
    scoped.entity.create();
    RecordingThreadQueue q(1, "bad_safe");
    EXPECT_THROW(scoped.entity.attach_queue(&q, nullptr, "d", 0,
                                            KDU_THREAD_QUEUE_SAFE_CONTEXT),
                 kdu_exception);
    (void)error_handler_.TakeMessage();
  }

  // 2) Note: Calling `register_event(&ev)` when `events_allowed` is true
  // cannot be cleanly deregistered or destroyed under Clang without hitting
  // upstream UB in `kdu_threads.cpp:4742,4753` (`old_ptr - old_s` after
  // `((kdu_byte *)NULL) + old_s + 2`). See the TODO in
  // `ContextLocksAndThreadEventsAndForeignSignal`.

  // 3) bind_jobs / schedule_jobs / schedule_job misuse and warning paths.
  {
    ScopedThreadEntity scoped;
    scoped.entity.create();
    ScopedThreadEntity other;
    other.entity.create();

    RecordingThreadQueue q0(0, "zero_jobs");
    RecordingThreadQueue q2(2, "two_jobs");
    EXPECT_TRUE(scoped.entity.attach_queue(&q0, nullptr, nullptr));
    EXPECT_TRUE(scoped.entity.attach_queue(&q2, nullptr, "d"));

    CounterJob jobs[3];
    std::atomic<int> cnt{0};
    std::atomic<int> q2_rem{3};
    for (auto& j : jobs) {
      j.set_job_func(IncrementJobFunc);
      j.completed = &cnt;
      j.remaining = &q2_rem;
      j.queue = &q2;
    }
    kdu_thread_job* ptrs[3] = {&jobs[0], &jobs[1], &jobs[2]};

    EXPECT_THROW(q2.BindRange(ptrs, 3, 0), kdu_exception);
    (void)error_handler_.TakeMessage();

    EXPECT_THROW(q0.ScheduleOne(&jobs[0], &scoped.entity, false, 0),
                 kdu_exception);
    (void)error_handler_.TakeMessage();

    EXPECT_THROW(q2.ScheduleOne(&jobs[0], &other.entity, false, 0),
                 kdu_exception);
    (void)error_handler_.TakeMessage();

    EXPECT_THROW(q2.ScheduleBatch(ptrs, 1, &other.entity, false),
                 kdu_exception);
    (void)error_handler_.TakeMessage();

    EXPECT_THROW(q2.ScheduleOne(&jobs[0], &scoped.entity, false, 0),
                 kdu_exception);
    (void)error_handler_.TakeMessage();

    q2.ScheduleOne(&jobs[0], &scoped.entity, false,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_THROW(q2.ScheduleOne(&jobs[1], &scoped.entity, false,
                                KDU_THREAD_JOB_REBIND_0),
                 kdu_exception);
    (void)error_handler_.TakeMessage();

    q2.ScheduleOne(&jobs[1], &scoped.entity, true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&q2_rem, &q2, &scoped.entity);
    EXPECT_THROW(q2.BindRange(ptrs, 1, 0), kdu_exception);
    (void)error_handler_.TakeMessage();

    EXPECT_THROW(q2.ScheduleOne(&jobs[2], &scoped.entity, false,
                                KDU_THREAD_JOB_AUTO_BIND_ONCE),
                 kdu_exception);
    (void)error_handler_.TakeMessage();

    // Duplicate all_scheduled=true logs warning on re-attached queue? Test on
    // fresh queue: call NoteAllScheduled + ScheduleOne(..., all_scheduled=true)
    // is rejected, while calling ScheduleOne(..., all_scheduled=true) on fresh
    // queue twice throws after first because CSTATE_S is cleared; test bind
    // after NoteAllScheduled and ScheduleBatch after all_scheduled.
    EXPECT_THROW(q2.ScheduleBatch(ptrs, 1, &scoped.entity, false),
                 kdu_exception);
    (void)error_handler_.TakeMessage();
    EXPECT_TRUE(scoped.entity.join(nullptr));
  }

  // 4) Foreign signal when waiter set expect_foreign_wakeup = false.
  {
    ScopedThreadEntity scoped;
    scoped.entity.create();
    EXPECT_TRUE(scoped.entity.add_thread());
    WarmupWorkers(scoped.entity);
    RecordingThreadQueue hold_q(1, "hold");
    EXPECT_TRUE(scoped.entity.attach_queue(&hold_q, nullptr, "h", 0,
                                           KDU_THREAD_QUEUE_SAFE_CONTEXT));
    struct MismatchJob final : public kdu_thread_job {
      kdu_thread_entity* owner = nullptr;
      RecordingThreadQueue* queue = nullptr;
      std::atomic<int> remaining{2};
      std::mutex mu;
      std::condition_variable cv;
      kdu_thread_entity_condition* cond = nullptr;
      bool job_started = false;
      bool ready = false;
      std::atomic<bool> mismatch_threw{false};
    } mjob;
    mjob.owner = &scoped.entity;
    mjob.queue = &hold_q;
    mjob.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* mj = static_cast<MismatchJob*>(base);
      if (caller != mj->owner) {
        kdu_thread_entity_condition* local_cond = nullptr;
        {
          std::unique_lock<std::mutex> lk(mj->mu);
          mj->job_started = true;
          mj->cv.notify_all();
          mj->cv.wait(lk, [&] { return mj->ready; });
          local_cond = mj->cond;
        }
        try {
          // Expect `signal_condition` from foreign caller without
          // expect_foreign_wakeup set on owner's wait throws or succeeds if
          // owner already left wait.
          caller->signal_condition(local_cond, KDU_FOREIGN_SIGNAL_EXPECTED);
        } catch (kdu_exception) {
          mj->mismatch_threw.store(true);
          caller->signal_condition(local_cond, 0);
        }
      }
      ReleaseGate(&mj->remaining, mj->queue, caller);
    });
    hold_q.ScheduleOne(&mjob, &scoped.entity, true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mjob.mu);
      mjob.cv.wait(lk, [&] { return mjob.job_started; });
    }
    ReleaseGate(&mjob.remaining, &hold_q, &scoped.entity);
    {
      std::lock_guard<std::mutex> lk(mjob.mu);
      mjob.cond = scoped.entity.get_condition();
      mjob.ready = true;
    }
    mjob.cv.notify_all();
    // Note: waiter catches exception inside ww_process_jobs if thrown on
    // worker, then wakes owner via non-foreign signal_condition.
    EXPECT_NO_THROW(scoped.entity.wait_for_condition("mismatch_wait", false));
    (void)error_handler_.TakeMessage();
    EXPECT_TRUE(scoped.entity.join(&hold_q));
    EXPECT_TRUE(scoped.entity.destroy());
  }

  // 5) Deadlock detector & report with parent + child queue hierarchy.
  {
    ScopedThreadEntity scoped;
    scoped.entity.create();
    RecordingThreadQueue parent_q(1, "deadlock_parent");
    RecordingThreadQueue child_q(1, "deadlock_child");
    EXPECT_TRUE(scoped.entity.attach_queue(&parent_q, nullptr, "dl"));
    EXPECT_TRUE(scoped.entity.attach_queue(&child_q, &parent_q, "dl_child"));

    kdu_exception out_exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(scoped.entity.join(&parent_q, false, &out_exc));
    EXPECT_NE(KDU_NULL_EXCEPTION, out_exc);
    std::string err = error_handler_.TakeMessage();
    EXPECT_NE(std::string::npos, err.find("deadlock"));
    EXPECT_NE(std::string::npos, err.find("deadlock_parent"));
    EXPECT_NE(std::string::npos, err.find("deadlock_child"));
    EXPECT_FALSE(scoped.entity.destroy());
  }
}

TEST_F(KduThreadsTest, AddQueueDependenciesBareQueueAndContextVirtuals) {
  class MinimalThreadContext final : public kdu_thread_context {};
  class BareQueue final : public kdu_thread_queue {
   public:
    int get_max_jobs() override { return 1; }
    void Schedule(kdu_thread_job* job, kdu_thread_entity* caller) {
      schedule_job(job, caller, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    void Finish(kdu_thread_entity* caller) { all_done(caller); }
  };
  // TODO(schwehr): Upstream `kdu_threads.h:2845` declares pure virtual
  // `~kdu_thread_dependency_monitor() = 0;` with no out-of-line definition in
  // `vendor/`, so direct subclasses fail to link (`undefined reference`).
  // Exercise dependency propagation through a parent `kdu_thread_queue`
  // override and call `add_queue(nullptr, ...)` for group-owned queue lifecycle
  // coverage.
  class RecordingParentQueue final : public kdu_thread_queue {
   public:
    bool update_dependencies(kdu_int32 new_dependencies,
                             kdu_int32 delta_max_dependencies,
                             kdu_thread_entity* /*caller*/) override {
      total_deps_ += new_dependencies;
      total_max_delta_ += delta_max_dependencies;
      ++update_calls_;
      return true;
    }
    int total_deps() const { return total_deps_; }
    int total_max_delta() const { return total_max_delta_; }
    int update_calls() const { return update_calls_; }

   private:
    int total_deps_ = 0;
    int total_max_delta_ = 0;
    int update_calls_ = 0;
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();

  MinimalThreadContext minimal_ctx;
  minimal_ctx.enter_group(&env);
  EXPECT_EQ(0, minimal_ctx.check_group(&env));
  EXPECT_TRUE(env.add_thread());

  kdu_thread_queue* owned_q = env.add_queue(nullptr, nullptr, "dep_dom", 1);
  ASSERT_NE(nullptr, owned_q);

  RecordingParentQueue parent_q;
  EXPECT_TRUE(env.attach_queue(&parent_q, nullptr, "dep_parent", 1));

  RecordingThreadQueue child_q(1, "dep_child");
  EXPECT_TRUE(env.attach_queue(&child_q, &parent_q, "dep_dom", 1));

  // Exercise all update_dependencies and propagate_dependencies branches.
  EXPECT_TRUE(child_q.update_dependencies(1, 2, &env));
  EXPECT_TRUE(child_q.update_dependencies(1, -1, &env));
  EXPECT_TRUE(child_q.update_dependencies(0, -2, &env));
  EXPECT_TRUE(child_q.update_dependencies(0, -1, &env));
  EXPECT_TRUE(child_q.update_dependencies(0, 2, &env));
  EXPECT_TRUE(child_q.update_dependencies(0, 1, &env));
  EXPECT_TRUE(child_q.update_dependencies(-2, -1, &env));
  EXPECT_GE(parent_q.update_calls(), 5);
  EXPECT_EQ(0, parent_q.total_deps());
  EXPECT_EQ(0, parent_q.total_max_delta());
  child_q.Complete(&env);

  // Queue without parent or monitor disables propagation permanently.
  BareQueue no_monitor_q;
  EXPECT_TRUE(env.attach_queue(&no_monitor_q, nullptr, "nomon_dom"));
  EXPECT_FALSE(no_monitor_q.update_dependencies(1, 1, &env));
  EXPECT_FALSE(no_monitor_q.update_dependencies(-1, -1, &env));
  no_monitor_q.Finish(&env);

  // Inactive kdu_run_queue rejects update_dependencies; active check_continue
  // clears cancelled accumulated dependencies and returns true.
  kdu_run_queue run_q;
  EXPECT_FALSE(run_q.update_dependencies(1, 0, &env));
  run_q.activate();
  EXPECT_TRUE(env.attach_queue(&run_q, nullptr, "run_cancel"));
  EXPECT_TRUE(run_q.update_dependencies(1, 0, &env));
  EXPECT_TRUE(run_q.update_dependencies(-1, 0, &env));
  kdu_run_queue_job rq_job;
  EXPECT_TRUE(run_q.check_continue(&rq_job, &env));
  run_q.deactivate(&env);

  // Owned queue unlinking via terminate() cleans up owned_q; wait until
  // bare_job is dequeued (slot[0] cleared to KD_PLT_HAZARD_MKR) before
  // Finish() + terminate().
  BareQueue bare_q;
  EXPECT_TRUE(env.attach_queue(&bare_q, nullptr, "bare_dom"));
  struct NotifyJob final : public kdu_thread_job {
    std::mutex mu;
    std::condition_variable cv;
    bool ran = false;
  } bare_job;
  bare_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity*) {
    auto* j = static_cast<NotifyJob*>(base);
    std::condition_variable* cv = &j->cv;
    {
      std::lock_guard<std::mutex> lock(j->mu);
      j->ran = true;
    }
    cv->notify_one();
  });
  bare_q.Schedule(&bare_job, &env);
  {
    std::unique_lock<std::mutex> lock(bare_job.mu);
    bare_job.cv.wait(lock, [&] { return bare_job.ran; });
  }
  bare_q.Finish(&env);

  EXPECT_TRUE(env.terminate(nullptr));
  EXPECT_TRUE(bare_job.ran);
  minimal_ctx.leave_group(&env);
  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, PalettesExhaustionReclaimAndCleanDirty) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  EXPECT_TRUE(env.add_thread("plt_dom"));
  EXPECT_TRUE(env.add_thread());

  constexpr int kNumPalettesBatch = 300;
  RecordingThreadQueue batch_q(kNumPalettesBatch, "batch_palettes");
  EXPECT_TRUE(env.attach_queue(&batch_q, nullptr, "plt_dom"));

  struct CountJob final : public kdu_thread_job {
    std::atomic<int>* count = nullptr;
    std::atomic<int>* remaining = nullptr;
    RecordingThreadQueue* queue = nullptr;
  };
  std::atomic<int> batch_count{0};
  std::atomic<int> batch_rem{kNumPalettesBatch + 1};
  std::vector<CountJob> batch_jobs(kNumPalettesBatch);
  std::vector<kdu_thread_job*> batch_ptrs(kNumPalettesBatch);
  for (int i = 0; i < kNumPalettesBatch; ++i) {
    batch_jobs[i].count = &batch_count;
    batch_jobs[i].remaining = &batch_rem;
    batch_jobs[i].queue = &batch_q;
    batch_jobs[i].set_job_func(
        [](kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* j = static_cast<CountJob*>(base);
          j->count->fetch_add(1, std::memory_order_acq_rel);
          ReleaseGate(j->remaining, j->queue, caller);
        });
    batch_ptrs[i] = &batch_jobs[i];
  }
  // Exceed KD_THREAD_PALETTES (192) to trigger augment_free_palettes().
  batch_q.BindRange(batch_ptrs.data(), kNumPalettesBatch, 0);
  batch_q.ScheduleBatch(batch_ptrs.data(), kNumPalettesBatch, &env,
                        /*all_scheduled=*/true);
  ReleaseGate(&batch_rem, &batch_q, &env);
  EXPECT_TRUE(env.join(&batch_q));
  EXPECT_EQ(kNumPalettesBatch, batch_count.load());

  // Repeatedly schedule a single bound job on the owner to fill the palette
  // swap heap and exercise clean_dirty_palettes().
  RecordingThreadQueue recycle_q(1, "recycle_palettes");
  EXPECT_TRUE(env.attach_queue(&recycle_q, nullptr, "plt_dom"));
  struct RecycleJob final : public kdu_thread_job {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int iterations = 0;
  } recycle_job;
  recycle_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity*) {
    auto* j = static_cast<RecycleJob*>(base);
    std::condition_variable* cv = &j->cv;
    {
      std::lock_guard<std::mutex> lock(j->mu);
      ++j->iterations;
      j->done = true;
    }
    cv->notify_one();
  });
  kdu_thread_job* recycle_ptr = &recycle_job;
  recycle_q.BindRange(&recycle_ptr, 1, 0);
  for (int iter = 0; iter < kNumPalettesBatch; ++iter) {
    {
      std::lock_guard<std::mutex> lock(recycle_job.mu);
      recycle_job.done = false;
    }
    recycle_q.ScheduleOne(&recycle_job, &env, /*all_scheduled=*/false, 0);
    std::unique_lock<std::mutex> lock(recycle_job.mu);
    recycle_job.cv.wait(lock, [&] { return recycle_job.done; });
  }
  recycle_q.NoteAllScheduled(&env);
  recycle_q.Complete(&env);
  EXPECT_TRUE(env.join(&recycle_q));
  EXPECT_EQ(kNumPalettesBatch, recycle_job.iterations);
  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, MultiSequenceDomainsAdvancingAndManySequences) {
  // Part 1: Small domain count where the owner also runs
  // advance_work_domains().
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread("dom_seq"));

    RecordingThreadQueue q1(1, "seq1");
    RecordingThreadQueue q2(1, "seq2");
    RecordingThreadQueue q3(1, "seq3");

    struct GateJob final : public kdu_thread_job {
      std::mutex mu;
      std::condition_variable cv;
      bool allow_finish = false;
      bool started = false;
      std::atomic<int> count{0};
      std::atomic<int> remaining{2};
      RecordingThreadQueue* queue = nullptr;
    } j1, j2, j3;

    auto run_gate = [](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<GateJob*>(base);
      std::condition_variable* cv = &j->cv;
      {
        std::unique_lock<std::mutex> lock(j->mu);
        j->started = true;
        cv->notify_all();
        j->cv.wait(lock, [j] { return j->allow_finish; });
      }
      j->count.fetch_add(1, std::memory_order_acq_rel);
      ReleaseGate(&j->remaining, j->queue, caller);
    };
    j1.queue = &q1;
    j2.queue = &q2;
    j3.queue = &q3;
    j1.set_job_func(run_gate);
    j2.set_job_func(run_gate);
    j3.set_job_func(run_gate);

    EXPECT_TRUE(env.attach_queue(&q1, nullptr, "dom_seq", /*min_seq=*/1));
    q1.ScheduleOne(&j1, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    // Attach q2 with higher sequencing index while seq 1 is still active so
    // a second kd_thread_domain_sequence is appended.
    EXPECT_TRUE(env.attach_queue(&q2, nullptr, "dom_seq", /*min_seq=*/2));
    {
      std::lock_guard<std::mutex> lock(j1.mu);
      j1.allow_finish = true;
    }
    j1.cv.notify_all();
    ReleaseGate(&j1.remaining, &q1, &env);
    EXPECT_TRUE(env.join(&q1));
    env.advance_work_domains();

    // Attach q3 with min_seq=3 to trigger remove_unused_sequences().
    EXPECT_TRUE(env.attach_queue(&q3, nullptr, "dom_seq", /*min_seq=*/3));
    {
      std::lock_guard<std::mutex> lock(j2.mu);
      j2.allow_finish = true;
    }
    {
      std::lock_guard<std::mutex> lock(j3.mu);
      j3.allow_finish = true;
    }
    j2.cv.notify_all();
    j3.cv.notify_all();
    q2.ScheduleOne(&j2, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&j2.remaining, &q2, &env);
    q3.ScheduleOne(&j3, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&j3.remaining, &q3, &env);
    EXPECT_TRUE(env.join(nullptr));
    EXPECT_EQ(1, j1.count.load());
    EXPECT_EQ(1, j2.count.load());
    EXPECT_EQ(1, j3.count.load());
    EXPECT_TRUE(env.destroy());
  }

  // Part 2: 17 domains x 2 active sequences (>32 sets) forces active_sets
  // reallocation twice on workers while workers finish before owner join().
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread("mseq_00"));
    EXPECT_TRUE(env.add_thread());

    constexpr int kNumDoms = 17;
    constexpr int kTotalQueues = kNumDoms * 2;
    std::mutex mu;
    std::condition_variable cv;
    bool release_hold = false;
    int finished = 0;

    struct SeqJob final : public kdu_thread_job {
      RecordingThreadQueue* queue = nullptr;
      std::atomic<int>* remaining = nullptr;
      std::mutex* mu = nullptr;
      std::condition_variable* cv = nullptr;
      bool* release = nullptr;
      int* finished = nullptr;
      bool block_first = false;
    };

    std::vector<std::string> doms;
    doms.reserve(kNumDoms);
    std::vector<std::unique_ptr<RecordingThreadQueue>> queues;
    queues.reserve(kTotalQueues);
    std::vector<std::atomic<int>> rems(kTotalQueues);
    std::vector<SeqJob> jobs(kTotalQueues);

    auto run_seq = [](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<SeqJob*>(base);
      if (j->block_first) {
        std::unique_lock<std::mutex> lock(*j->mu);
        j->cv->wait(lock, [j] { return *j->release; });
      }
      ReleaseGate(j->remaining, j->queue, caller);
      std::condition_variable* done_cv = j->cv;
      {
        std::lock_guard<std::mutex> lock(*j->mu);
        ++(*j->finished);
      }
      done_cv->notify_all();
    };

    for (int d = 0; d < kNumDoms; ++d) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "mseq_%02d", d);
      doms.emplace_back(buf);
      for (int s = 0; s < 2; ++s) {
        int idx = d * 2 + s;
        queues.push_back(std::make_unique<RecordingThreadQueue>(1));
        rems[idx].store(2, std::memory_order_relaxed);
        EXPECT_TRUE(env.attach_queue(queues.back().get(), nullptr,
                                     doms.back().c_str(), s + 1));
        jobs[idx].set_job_func(run_seq);
        jobs[idx].queue = queues.back().get();
        jobs[idx].remaining = &rems[idx];
        jobs[idx].mu = &mu;
        jobs[idx].cv = &cv;
        jobs[idx].release = &release_hold;
        jobs[idx].finished = &finished;
        jobs[idx].block_first = (s == 0);
        queues.back()->ScheduleOne(&jobs[idx], &env, true,
                                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
        ReleaseGate(&rems[idx], queues.back().get(), &env);
      }
    }
    {
      std::lock_guard<std::mutex> lock(mu);
      release_hold = true;
      cv.notify_all();
    }
    {
      // Same workaround as ManyDomainsExceedInlineActiveStore: workers
      // finish all queues before env.join(nullptr) to avoid the upstream
      // group-owner `active_sets` leak in kdu_thread_entity::destroy().
      std::unique_lock<std::mutex> lock(mu);
      cv.wait(lock, [&] { return finished == kTotalQueues; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    for (auto& pq : queues) {
      EXPECT_TRUE(env.join(pq.get()));
    }
    env.advance_work_domains();
    EXPECT_TRUE(env.join(nullptr));
    EXPECT_EQ(kTotalQueues, finished);
    EXPECT_TRUE(env.destroy());
  }
}

TEST_F(KduThreadsTest, YieldFrequencyFirstOwnerSafeNestedJoinsAndLockError) {
  class InstrumentedEntity final : public kdu_thread_entity {
   public:
    explicit InstrumentedEntity(std::atomic<int>* shared_pre_launched = nullptr)
        : shared_pre_launched_(shared_pre_launched) {}

    kdu_thread_entity* new_instance() override {
      ++spawned;
      return new InstrumentedEntity(shared_pre_launched_);
    }
    void pre_launch() override {
      if (shared_pre_launched_ != nullptr) {
        shared_pre_launched_->fetch_add(1, std::memory_order_acq_rel);
      }
      kdu_thread_entity::pre_launch();
    }
    void process_jobs() override { kdu_thread_entity::process_jobs(); }
    int spawned = 0;
    std::atomic<int>* shared_pre_launched_;
  };

  ScopedThreadEntity other_scoped;
  other_scoped.entity.create();

  std::atomic<int> pre_launched{0};
  InstrumentedEntity env(&pre_launched);
  env.create();
  env.set_yield_frequency(1);
  env.set_min_thread_concurrency(2);
  EXPECT_FALSE(env.declare_first_owner_wait_safe(true));
  EXPECT_TRUE(env.declare_first_owner_wait_safe(true));
  EXPECT_TRUE(env.add_thread("pref"));
  EXPECT_TRUE(env.add_thread());
  EXPECT_EQ(2, env.spawned);

  // Ensure both worker threads have entered process_jobs() (and pushed their
  // initial conditions) before any SAFE_CONTEXT wait or deadlock check runs.
  {
    RecordingThreadQueue barrier_q(2, "worker_barrier");
    EXPECT_TRUE(env.attach_queue(&barrier_q, nullptr, "barrier", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    struct BarrierJob final : public kdu_thread_job {
      std::mutex* mu = nullptr;
      std::condition_variable* cv = nullptr;
      int* arrived = nullptr;
      std::atomic<int>* rem = nullptr;
      RecordingThreadQueue* q = nullptr;
    };
    std::mutex bmu;
    std::condition_variable bcv;
    int arrived = 0;
    std::atomic<int> brem{3};
    BarrierJob bjobs[2];
    for (auto& bj : bjobs) {
      bj.mu = &bmu;
      bj.cv = &bcv;
      bj.arrived = &arrived;
      bj.rem = &brem;
      bj.q = &barrier_q;
      bj.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* j = static_cast<BarrierJob*>(base);
        {
          std::unique_lock<std::mutex> lock(*j->mu);
          ++(*j->arrived);
          j->cv->notify_all();
          j->cv->wait(lock, [j] { return *j->arrived == 2; });
        }
        ReleaseGate(j->rem, j->q, caller);
      });
      barrier_q.ScheduleOne(&bj, &env, false, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    barrier_q.NoteAllScheduled(&env);
    {
      std::unique_lock<std::mutex> lock(bmu);
      bcv.wait(lock, [&] { return arrived == 2; });
    }
    ReleaseGate(&brem, &barrier_q, &env);
    EXPECT_TRUE(env.join(&barrier_q));
    EXPECT_EQ(2, pre_launched.load());
  }

  // First-owner-wait-safe SAFE_CONTEXT queue processed by the group owner.
  RecordingThreadQueue safe_q(4, "first_safe");
  EXPECT_TRUE(env.attach_queue(
      &safe_q, nullptr, "pref", 0,
      KDU_THREAD_QUEUE_SAFE_CONTEXT | KDU_THREAD_QUEUE_PRIORITY_HIGH));
  std::atomic<int> safe_done{0};
  std::atomic<int> safe_rem{5};
  struct SimpleJob final : public kdu_thread_job {
    std::atomic<int>* done = nullptr;
    std::atomic<int>* remaining = nullptr;
    RecordingThreadQueue* queue = nullptr;
    kdu_thread_entity* group_owner = nullptr;
    kdu_thread_entity* foreign = nullptr;
    bool self_ok = false;
    bool member_ok = false;
    bool foreign_ok = false;
  };
  SimpleJob safe_jobs[4];
  for (auto& j : safe_jobs) {
    j.done = &safe_done;
    j.remaining = &safe_rem;
    j.queue = &safe_q;
    j.group_owner = &env;
    j.foreign = &other_scoped.entity;
    j.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* sj = static_cast<SimpleJob*>(base);
      sj->self_ok = (caller->get_self_identity() == caller);
      sj->member_ok = caller->check_group_member(sj->group_owner);
      sj->foreign_ok = !caller->check_group_member(sj->foreign);
      sj->done->fetch_add(1, std::memory_order_acq_rel);
      ReleaseGate(sj->remaining, sj->queue, caller);
    });
    safe_q.ScheduleOne(&j, &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
  }
  safe_q.NoteAllScheduled(&env);
  ReleaseGate(&safe_rem, &safe_q, &env);
  EXPECT_TRUE(env.join(&safe_q));
  EXPECT_EQ(4, safe_done.load());
  for (const auto& j : safe_jobs) {
    EXPECT_TRUE(j.self_ok);
    EXPECT_TRUE(j.member_ok);
    EXPECT_TRUE(j.foreign_ok);
  }
  EXPECT_TRUE(env.declare_first_owner_wait_safe(false));

  // Background queue in preferred worker domain exercises
  // dequeue_domain_specific_job() background paths.
  RecordingThreadQueue bkgnd_q(2, "pref_bkgnd");
  EXPECT_TRUE(env.attach_queue(&bkgnd_q, nullptr, "pref", 0,
                               KDU_THREAD_QUEUE_BACKGROUND));
  std::atomic<int> bkgnd_done{0};
  std::atomic<int> bkgnd_rem{2};
  SimpleJob bg_job;
  bg_job.done = &bkgnd_done;
  bg_job.remaining = &bkgnd_rem;
  bg_job.queue = &bkgnd_q;
  bg_job.group_owner = &env;
  bg_job.foreign = &other_scoped.entity;
  bg_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
    auto* sj = static_cast<SimpleJob*>(base);
    kdu_thread::yield();
    sj->done->fetch_add(1, std::memory_order_acq_rel);
    ReleaseGate(sj->remaining, sj->queue, caller);
  });
  bkgnd_q.ScheduleOne(&bg_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
  ReleaseGate(&bkgnd_rem, &bkgnd_q, &env);
  EXPECT_TRUE(env.join(&bkgnd_q));
  EXPECT_EQ(1, bkgnd_done.load());

  // Nested joins >= 9 levels deep inside jobs allocate dynamic
  // kdu_thread_entity_condition entries beyond KDU_INITIAL_THREAD_CONDITIONS.
  constexpr int kDepth = 10;
  std::vector<std::unique_ptr<RecordingThreadQueue>> nested_qs;
  nested_qs.reserve(kDepth);
  struct NestedJob final : public kdu_thread_job {
    int level = 0;
    int max_depth = 0;
    std::vector<std::unique_ptr<RecordingThreadQueue>>* queues = nullptr;
    std::vector<NestedJob>* jobs = nullptr;
    std::vector<std::atomic<int>>* rems = nullptr;
    std::atomic<int>* count = nullptr;
  };
  std::atomic<int> nested_count{0};
  std::vector<std::atomic<int>> nested_rems(kDepth);
  std::vector<NestedJob> nested_jobs(kDepth);
  RecordingThreadQueue* parent = nullptr;
  for (int d = 0; d < kDepth; ++d) {
    nested_rems[d].store(2, std::memory_order_relaxed);
    nested_qs.push_back(std::make_unique<RecordingThreadQueue>(1, "nested"));
    EXPECT_TRUE(env.attach_queue(nested_qs.back().get(), parent, "nested"));
    parent = nested_qs.back().get();
    nested_jobs[d].level = d;
    nested_jobs[d].max_depth = kDepth;
    nested_jobs[d].queues = &nested_qs;
    nested_jobs[d].jobs = &nested_jobs;
    nested_jobs[d].rems = &nested_rems;
    nested_jobs[d].count = &nested_count;
    nested_jobs[d].set_job_func(
        [](kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* nj = static_cast<NestedJob*>(base);
          nj->count->fetch_add(1, std::memory_order_acq_rel);
          int next = nj->level + 1;
          if (next < nj->max_depth) {
            (*nj->queues)[next]->ScheduleOne(&(*nj->jobs)[next], caller, true,
                                             KDU_THREAD_JOB_AUTO_BIND_ONCE);
            ReleaseGate(&(*nj->rems)[next], (*nj->queues)[next].get(), caller);
            caller->join((*nj->queues)[next].get());
          }
          ReleaseGate(&(*nj->rems)[nj->level], (*nj->queues)[nj->level].get(),
                      caller);
        });
  }
  nested_qs[0]->ScheduleOne(&nested_jobs[0], &env, true,
                            KDU_THREAD_JOB_AUTO_BIND_ONCE);
  ReleaseGate(&nested_rems[0], nested_qs[0].get(), &env);
  EXPECT_TRUE(env.join(nested_qs[0].get()));
  EXPECT_EQ(kDepth, nested_count.load());

  // Calling acquire_lock / try_lock with allow_exceptions=true after
  // handle_exception exercises kd_thread_grouperr::throw_error().
  RecordingThreadContext err_ctx(1);
  err_ctx.enter_group(&env);
  env.handle_exception(99);
  bool threw_acquire = false;
  try {
    err_ctx.acquire_lock(0, &env, /*allow_exceptions=*/true);
  } catch (kdu_exception exc) {
    threw_acquire = (exc == 99);
    err_ctx.release_lock(0, &env);
  }
  EXPECT_TRUE(threw_acquire);
  bool threw_try = false;
  try {
    err_ctx.try_lock(0, &env, /*allow_exceptions=*/true);
  } catch (kdu_exception exc) {
    threw_try = (exc == 99);
    err_ctx.release_lock(0, &env);
  }
  EXPECT_TRUE(threw_try);
  err_ctx.leave_group(&env);

  EXPECT_EQ(nullptr, kdu_thread_entity_affinity::check_numa_support(1000000));
  EXPECT_FALSE(env.destroy());
  EXPECT_TRUE(other_scoped.entity.destroy());
}

TEST_F(KduThreadsTest, MultiSequenceWalk35DomainsAndCleanDirtyPalettes) {
  // 1) Multi-sequence domain ordering, out-of-order insertion, and reclaiming
  // unused sequences once a worker pops the completed sequence's terminator.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread("dom_seq"));

    struct GateJob final : public kdu_thread_job {
      std::mutex mu;
      std::condition_variable cv;
      bool started = false;
      bool allow = false;
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* remaining = nullptr;
    };

    auto gate_fn = [](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<GateJob*>(base);
      {
        std::lock_guard<std::mutex> lock(j->mu);
        j->started = true;
      }
      j->cv.notify_all();
      {
        std::unique_lock<std::mutex> lock(j->mu);
        j->cv.wait(lock, [j] { return j->allow; });
      }
      ReleaseGate(j->remaining, j->q, caller);
    };

    RecordingThreadQueue q_s1(1, "seq1");
    RecordingThreadQueue q_s5(1, "seq5");
    RecordingThreadQueue q_s2(1, "seq2");
    RecordingThreadQueue q_s2b(1, "seq2b");
    RecordingThreadQueue q_s3(1, "seq3");
    RecordingThreadQueue q_s4(1, "seq4");

    GateJob j1;
    std::atomic<int> rem1{2};
    j1.q = &q_s1;
    j1.remaining = &rem1;
    j1.set_job_func(gate_fn);

    EXPECT_TRUE(env.attach_queue(&q_s1, nullptr, "dom_seq", /*min_seq=*/1));
    q_s1.ScheduleOne(&j1, &env, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lock(j1.mu);
      j1.cv.wait(lock, [&j1] { return j1.started; });
    }

    // Append seq5 first, then insert seq2, seq3, seq4 before seq5 and match
    // existing seq2 while seq1 is still actively running.
    EXPECT_TRUE(env.attach_queue(&q_s5, nullptr, "dom_seq", /*min_seq=*/5));
    EXPECT_TRUE(env.attach_queue(&q_s2, nullptr, "dom_seq", /*min_seq=*/2));
    EXPECT_TRUE(env.attach_queue(&q_s2b, nullptr, "dom_seq", /*min_seq=*/2));
    EXPECT_TRUE(env.attach_queue(&q_s3, nullptr, "dom_seq", /*min_seq=*/3));
    EXPECT_TRUE(env.attach_queue(&q_s4, nullptr, "dom_seq", /*min_seq=*/4));

    struct RanJob final : public kdu_thread_job {
      std::mutex mu;
      std::condition_variable cv;
      bool ran = false;
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* remaining = nullptr;
    };
    auto ran_fn = [](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<RanJob*>(base);
      {
        std::lock_guard<std::mutex> lock(j->mu);
        j->ran = true;
      }
      j->cv.notify_all();
      ReleaseGate(j->remaining, j->q, caller);
    };

    RanJob j2;
    std::atomic<int> rem2{2};
    j2.q = &q_s2;
    j2.remaining = &rem2;
    j2.set_job_func(ran_fn);
    q_s2.ScheduleOne(&j2, &env, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);

    {
      std::lock_guard<std::mutex> lock(j1.mu);
      j1.allow = true;
    }
    j1.cv.notify_all();
    ReleaseGate(&rem1, &q_s1, &env);
    EXPECT_TRUE(env.join(&q_s1));

    // Wait until the dom_seq worker runs q_s2 so q_s1's terminator has been
    // dequeued before advance_work_domains() reclaims seq1.
    {
      std::unique_lock<std::mutex> lock(j2.mu);
      j2.cv.wait(lock, [&j2] { return j2.ran; });
    }
    env.advance_work_domains();
    ReleaseGate(&rem2, &q_s2, &env);
    EXPECT_TRUE(env.join(&q_s2));

    RanJob j2b;
    RanJob j3;
    RanJob j4;
    RanJob j5;
    std::atomic<int> rem2b{2};
    std::atomic<int> rem3{2};
    std::atomic<int> rem4{2};
    std::atomic<int> rem5{2};
    j2b.q = &q_s2b;
    j2b.remaining = &rem2b;
    j2b.set_job_func(ran_fn);
    j3.q = &q_s3;
    j3.remaining = &rem3;
    j3.set_job_func(ran_fn);
    j4.q = &q_s4;
    j4.remaining = &rem4;
    j4.set_job_func(ran_fn);
    j5.q = &q_s5;
    j5.remaining = &rem5;
    j5.set_job_func(ran_fn);

    q_s2b.ScheduleOne(&j2b, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lock(j2b.mu);
      j2b.cv.wait(lock, [&j2b] { return j2b.ran; });
    }
    ReleaseGate(&rem2b, &q_s2b, &env);
    EXPECT_TRUE(env.join(&q_s2b));

    q_s3.ScheduleOne(&j3, &env, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lock(j3.mu);
      j3.cv.wait(lock, [&j3] { return j3.ran; });
    }
    ReleaseGate(&rem3, &q_s3, &env);
    EXPECT_TRUE(env.join(&q_s3));

    q_s4.ScheduleOne(&j4, &env, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lock(j4.mu);
      j4.cv.wait(lock, [&j4] { return j4.ran; });
    }
    env.advance_work_domains();
    ReleaseGate(&rem4, &q_s4, &env);
    EXPECT_TRUE(env.join(&q_s4));

    q_s5.ScheduleOne(&j5, &env, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&rem5, &q_s5, &env);
    EXPECT_TRUE(env.join(&q_s5));
    EXPECT_TRUE(env.join(nullptr));
  }

  // 2) Two >256-palette waves while a sibling queue remains active to exercise
  // dirty-palette sweeping and wrap-around in clean_dirty_palettes().
  {
    ScopedThreadEntity pal_scoped;
    kdu_thread_entity& penv = pal_scoped.entity;
    penv.create();
    EXPECT_TRUE(penv.add_thread());
    EXPECT_TRUE(penv.add_thread());

    constexpr int kWaveJobs = 320;
    struct WaveJob final : public kdu_thread_job {
      std::atomic<int>* rem = nullptr;
      RecordingThreadQueue* q = nullptr;
    };
    auto wave_fn = [](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<WaveJob*>(base);
      ReleaseGate(j->rem, j->q, caller);
    };

    RecordingThreadQueue q_hold(1, "pal_hold");
    std::atomic<bool> hold_started{false};
    std::atomic<int> hold_rem{2};
    std::mutex hold_mu;
    std::condition_variable hold_cv;
    bool hold_allow = false;
    struct HoldJob final : public kdu_thread_job {
      std::atomic<bool>* started = nullptr;
      std::atomic<int>* rem = nullptr;
      RecordingThreadQueue* q = nullptr;
      std::mutex* mu = nullptr;
      std::condition_variable* cv = nullptr;
      bool* allow = nullptr;
    } hold_job;
    hold_job.started = &hold_started;
    hold_job.rem = &hold_rem;
    hold_job.q = &q_hold;
    hold_job.mu = &hold_mu;
    hold_job.cv = &hold_cv;
    hold_job.allow = &hold_allow;
    hold_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<HoldJob*>(base);
      j->started->store(true);
      {
        std::unique_lock<std::mutex> lock(*j->mu);
        j->cv->wait(lock, [j] { return *j->allow; });
      }
      ReleaseGate(j->rem, j->q, caller);
    });
    EXPECT_TRUE(penv.attach_queue(&q_hold, nullptr, "hold_dom"));
    q_hold.ScheduleOne(&hold_job, &penv, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!hold_started.load()) {
      std::this_thread::yield();
    }

    for (int wave = 0; wave < 2; ++wave) {
      RecordingThreadQueue wave_q(kWaveJobs, "pal_wave");
      EXPECT_TRUE(penv.attach_queue(&wave_q, nullptr, "wave_dom"));
      std::atomic<int> wave_rem{kWaveJobs + 1};
      std::vector<WaveJob> jobs(kWaveJobs);
      std::vector<kdu_thread_job*> ptrs(kWaveJobs);
      for (int i = 0; i < kWaveJobs; ++i) {
        jobs[i].rem = &wave_rem;
        jobs[i].q = &wave_q;
        jobs[i].set_job_func(wave_fn);
        ptrs[i] = &jobs[i];
      }
      wave_q.BindRange(ptrs.data(), kWaveJobs, 0);
      wave_q.ScheduleBatch(ptrs.data(), kWaveJobs, &penv,
                           /*all_scheduled=*/true);
      ReleaseGate(&wave_rem, &wave_q, &penv);
      EXPECT_TRUE(penv.join(&wave_q));
    }

    {
      std::lock_guard<std::mutex> lock(hold_mu);
      hold_allow = true;
    }
    hold_cv.notify_all();
    ReleaseGate(&hold_rem, &q_hold, &penv);
    EXPECT_TRUE(penv.join(nullptr));
  }

  // 3) Expand worker active_sets beyond 32 (> 16 -> 32 -> 64) across 35
  // simultaneous domains processed by workers so the group owner does not leak.
  {
    ScopedThreadEntity d35_scoped;
    kdu_thread_entity& denv = d35_scoped.entity;
    denv.create();
    EXPECT_TRUE(denv.add_thread());
    EXPECT_TRUE(denv.add_thread());

    std::mutex gate_mu;
    std::condition_variable gate_cv;
    int gate_entered = 0;
    bool gate_allow = false;
    std::atomic<int> gate_rem{3};
    struct WorkerGate final : public kdu_thread_job {
      std::mutex* mu = nullptr;
      std::condition_variable* cv = nullptr;
      int* entered = nullptr;
      bool* allow = nullptr;
      std::atomic<int>* rem = nullptr;
      RecordingThreadQueue* q = nullptr;
    };
    auto wgate_fn = [](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<WorkerGate*>(base);
      {
        std::unique_lock<std::mutex> lock(*j->mu);
        ++(*j->entered);
        j->cv->notify_all();
        j->cv->wait(lock, [j] { return *j->allow; });
      }
      ReleaseGate(j->rem, j->q, caller);
    };

    RecordingThreadQueue gate_q(2, "gate_35");
    EXPECT_FALSE(denv.attach_queue(&gate_q, nullptr, nullptr));
    EXPECT_TRUE(denv.attach_queue(&gate_q, nullptr, "gate_35"));
    WorkerGate gjobs[2];
    for (int i = 0; i < 2; ++i) {
      gjobs[i].mu = &gate_mu;
      gjobs[i].cv = &gate_cv;
      gjobs[i].entered = &gate_entered;
      gjobs[i].allow = &gate_allow;
      gjobs[i].rem = &gate_rem;
      gjobs[i].q = &gate_q;
      gjobs[i].set_job_func(wgate_fn);
      gate_q.ScheduleOne(&gjobs[i], &denv, /*all_scheduled=*/(i == 1),
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    ReleaseGate(&gate_rem, &gate_q, &denv);
    {
      std::unique_lock<std::mutex> lock(gate_mu);
      gate_cv.wait(lock, [&gate_entered] { return gate_entered == 2; });
    }

    constexpr int kDomains35 = 35;
    std::vector<std::string> dnames;
    std::vector<std::unique_ptr<RecordingThreadQueue>> queues;
    struct DoneJob final : public kdu_thread_job {
      std::atomic<int>* total_rem = nullptr;
      std::atomic<int> q_rem{2};
      RecordingThreadQueue* q = nullptr;
      std::mutex* mu = nullptr;
      std::condition_variable* cv = nullptr;
    };
    std::atomic<int> rem35{kDomains35};
    std::mutex done_mu;
    std::condition_variable done_cv;
    std::vector<DoneJob> jobs(kDomains35);
    dnames.reserve(kDomains35);
    queues.reserve(kDomains35);
    for (int d = 0; d < kDomains35; ++d) {
      dnames.push_back("d35_" + std::to_string(d));
      queues.push_back(
          std::make_unique<RecordingThreadQueue>(1, dnames.back().c_str()));
      EXPECT_TRUE(denv.attach_queue(queues.back().get(), nullptr,
                                    dnames.back().c_str()));
      jobs[d].total_rem = &rem35;
      jobs[d].q = queues.back().get();
      jobs[d].mu = &done_mu;
      jobs[d].cv = &done_cv;
      jobs[d].set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* j = static_cast<DoneJob*>(base);
        ReleaseGate(&j->q_rem, j->q, caller);
        if (j->total_rem->fetch_sub(1) == 1) {
          std::lock_guard<std::mutex> lock(*j->mu);
          j->cv->notify_all();
        }
      });
      queues.back()->ScheduleOne(&jobs[d], &denv, /*all_scheduled=*/true,
                                 KDU_THREAD_JOB_AUTO_BIND_ONCE);
      ReleaseGate(&jobs[d].q_rem, queues.back().get(), &denv);
    }

    {
      std::lock_guard<std::mutex> lock(gate_mu);
      gate_allow = true;
    }
    gate_cv.notify_all();

    {
      std::unique_lock<std::mutex> lock(done_mu);
      done_cv.wait(lock, [&rem35] { return rem35.load() == 0; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    denv.advance_work_domains();
    EXPECT_TRUE(denv.join(nullptr));
  }
}

TEST_F(KduThreadsTest, WorkerExceptionConcurrentWaitSuspendsAndEdges) {
  // 1) kdu_thread priority, invalid priority, and affinity paths on a live
  // thread.
  // TODO(schwehr): Upstream kdu_thread::get_priority()/set_priority()/
  // set_cpu_affinity() pass an uninitialized pthread_t to pthread_*() when
  // !exists(), causing SIGSEGV on uncreated kdu_thread instances.
  {
    kdu_thread idle_t;
    EXPECT_FALSE(idle_t.exists());

    struct ThreadSync {
      std::mutex mu;
      std::condition_variable cv;
      bool allow_exit = false;
    } sync;
    auto entry = [](void* arg) -> void* {
      auto* s = static_cast<ThreadSync*>(arg);
      std::unique_lock<std::mutex> lock(s->mu);
      s->cv.wait(lock, [s] { return s->allow_exit; });
      return nullptr;
    };
    kdu_thread live_t;
    EXPECT_TRUE(live_t.create(entry, &sync));
    int mn = 0;
    int mx = 0;
    int cur_prio = live_t.get_priority(mn, mx);
    EXPECT_TRUE(live_t.set_priority(cur_prio));
    EXPECT_FALSE(live_t.set_priority(mn - 10));
    {
      std::lock_guard<std::mutex> lock(sync.mu);
      sync.allow_exit = true;
    }
    sync.cv.notify_all();
    EXPECT_TRUE(live_t.destroy());
  }

  // 2) set_cpu_affinity(desc) + pre_launch() throwing kdu_exception.
  {
    struct PreLaunchState {
      std::mutex mu;
      std::condition_variable cv;
      bool should_throw = true;
      bool allow_throw = false;
      bool handled = false;
    } pls;

    class ThrowingPreLaunchEntity final : public kdu_thread_entity {
     public:
      explicit ThrowingPreLaunchEntity(PreLaunchState* state) : state_(state) {}

      void handle_exception(kdu_exception exc_code) override {
        kdu_thread_entity::handle_exception(exc_code);
        if (exc_code == kdu_exception(KDU_ERROR_EXCEPTION)) {
          {
            std::lock_guard<std::mutex> lock(state_->mu);
            state_->handled = true;
          }
          state_->cv.notify_all();
        }
      }

     protected:
      kdu_thread_entity* new_instance() override {
        return new ThrowingPreLaunchEntity(state_);
      }
      void pre_launch() override {
        std::unique_lock<std::mutex> lock(state_->mu);
        state_->cv.wait(lock, [this] { return state_->allow_throw; });
        if (state_->should_throw) {
          throw kdu_exception(KDU_ERROR_EXCEPTION);
        }
      }

     private:
      PreLaunchState* state_;
    };

    ThrowingPreLaunchEntity ent(&pls);
    ent.create();
    kdu_thread_entity_affinity aff;
    ent.set_cpu_affinity(aff);
    EXPECT_TRUE(ent.add_thread());
    {
      std::lock_guard<std::mutex> lock(pls.mu);
      pls.allow_throw = true;
    }
    pls.cv.notify_all();
    {
      std::unique_lock<std::mutex> lock(pls.mu);
      pls.cv.wait(lock, [&pls] { return pls.handled; });
    }
    kdu_exception exc = 0;
    EXPECT_FALSE(ent.join(nullptr, /*descendants_only=*/false, &exc));
    EXPECT_EQ(kdu_exception(KDU_ERROR_EXCEPTION), exc);
    EXPECT_FALSE(ent.destroy());
  }

  // 3) Worker exception concurrent with another worker so owner join enters
  // wait_for_exceptional_join() while exceptional_join_dependencies > 0, plus
  // suspend(false) error path and false return on join(..., nullptr).
  {
    ScopedThreadEntity exc_scoped;
    kdu_thread_entity& xenv = exc_scoped.entity;
    xenv.create();
    EXPECT_TRUE(xenv.add_thread());
    EXPECT_TRUE(xenv.add_thread());

    RecordingThreadQueue q_throw(1, "q_throw");
    RecordingThreadQueue q_hold(1, "q_hold");
    EXPECT_TRUE(xenv.attach_queue(&q_throw, nullptr, "exc_dom"));
    EXPECT_TRUE(xenv.attach_queue(&q_hold, nullptr, "exc_dom"));

    struct HoldState {
      std::mutex mu;
      std::condition_variable cv;
      bool hold_started = false;
      bool threw = false;
    } hs;

    struct ExcJob final : public kdu_thread_job {
      HoldState* state = nullptr;
      bool do_throw = false;
    };

    ExcJob j_hold;
    ExcJob j_throw;
    j_hold.state = &hs;
    j_hold.do_throw = false;
    j_throw.state = &hs;
    j_throw.do_throw = true;

    j_hold.set_job_func(
        [](kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
          auto* j = static_cast<ExcJob*>(base);
          {
            std::lock_guard<std::mutex> lock(j->state->mu);
            j->state->hold_started = true;
          }
          j->state->cv.notify_all();
          {
            std::unique_lock<std::mutex> lock(j->state->mu);
            j->state->cv.wait(lock, [j] { return j->state->threw; });
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        });
    j_throw.set_job_func(
        [](kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
          auto* j = static_cast<ExcJob*>(base);
          {
            std::unique_lock<std::mutex> lock(j->state->mu);
            j->state->cv.wait(lock, [j] { return j->state->hold_started; });
            j->state->threw = true;
          }
          j->state->cv.notify_all();
          throw kdu_exception(KDU_ERROR_EXCEPTION);
        });

    q_hold.ScheduleOne(&j_hold, &xenv, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    q_throw.ScheduleOne(&j_throw, &xenv, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);

    {
      std::unique_lock<std::mutex> lock(hs.mu);
      hs.cv.wait(lock, [&hs] { return hs.threw; });
    }
    kdu_exception exc = 0;
    EXPECT_FALSE(xenv.join(nullptr, /*descendants_only=*/false, &exc));
    EXPECT_EQ(kdu_exception(KDU_ERROR_EXCEPTION), exc);
    EXPECT_FALSE(xenv.suspend(/*allow_exceptions=*/false));
    EXPECT_THROW(xenv.suspend(/*allow_exceptions=*/true), kdu_exception);
    EXPECT_FALSE(xenv.join(nullptr));
  }

  // 4) Nested wait_for_condition (foreign=true -> inner foreign=false),
  // pre-unsuspend + suspended unsuspend, background queue + yield frequency +
  // add_thread() after domain creation + schedule_jobs/bind_jobs edges.
  {
    ScopedThreadEntity senv_scoped;
    kdu_thread_entity& senv = senv_scoped.entity;
    senv.create();
    senv.set_yield_frequency(1);
    senv.set_min_thread_concurrency(2);
    EXPECT_TRUE(senv.add_thread());

    RecordingThreadQueue q_fg(12, "fg_yield");
    RecordingThreadQueue q_bg(12, "bg_yield");
    RecordingThreadQueue q_zero(1, "zero_batch");
    EXPECT_TRUE(senv.attach_queue(&q_fg, nullptr, "mix_dom", 0,
                                  KDU_THREAD_QUEUE_PRIORITY_HIGH));
    EXPECT_TRUE(senv.attach_queue(&q_bg, nullptr, "mix_dom", 0,
                                  KDU_THREAD_QUEUE_BACKGROUND));
    EXPECT_TRUE(senv.attach_queue(&q_zero, nullptr, "mix_dom"));
    // Add a second worker after domain already exists.
    EXPECT_TRUE(senv.add_thread());

    // Zero-count schedule_jobs / bind_jobs on a separate queue (calling
    // schedule_jobs prevents subsequent KDU_THREAD_JOB_AUTO_BIND_ONCE on that
    // queue) and error paths on an unattached queue.
    q_zero.ScheduleBatch(nullptr, 0, &senv, /*all_scheduled=*/true);
    q_zero.BindRange(nullptr, 0, 0);
    q_zero.Complete(&senv);
    RecordingThreadQueue q_unattached(1, "unattached");
    struct NoOpJob final : public kdu_thread_job {
      std::atomic<int>* rem = nullptr;
      RecordingThreadQueue* q = nullptr;
    };
    NoOpJob bad_job;
    bad_job.set_job_func(
        [](kdu_thread_job* /*base*/, kdu_thread_entity* /*caller*/) {});
    kdu_thread_job* bad_ptr = &bad_job;
    EXPECT_THROW(q_unattached.BindRange(&bad_ptr, 1, 0), kdu_exception);
    EXPECT_THROW(q_unattached.ScheduleBatch(&bad_ptr, 1, &senv, false),
                 kdu_exception);

    std::atomic<int> fg_rem{13};
    std::atomic<int> bg_rem{13};
    std::vector<NoOpJob> fg_jobs(12);
    std::vector<NoOpJob> bg_jobs(12);
    auto noop_fn = [](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<NoOpJob*>(base);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      ReleaseGate(j->rem, j->q, caller);
    };
    for (int i = 0; i < 12; ++i) {
      fg_jobs[i].rem = &fg_rem;
      fg_jobs[i].q = &q_fg;
      fg_jobs[i].set_job_func(noop_fn);
      bg_jobs[i].rem = &bg_rem;
      bg_jobs[i].q = &q_bg;
      bg_jobs[i].set_job_func(noop_fn);
      q_fg.ScheduleOne(&fg_jobs[i], &senv, /*all_scheduled=*/(i == 11),
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      q_bg.ScheduleOne(&bg_jobs[i], &senv, /*all_scheduled=*/(i == 11),
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    ReleaseGate(&fg_rem, &q_fg, &senv);
    ReleaseGate(&bg_rem, &q_bg, &senv);
    EXPECT_TRUE(senv.join(nullptr));

    // Pre-unsuspend and delayed-unsuspend of a suspended worker.
    struct SuspState {
      std::mutex mu;
      std::condition_variable cv;
      kdu_thread_entity* worker = nullptr;
      bool pre_done = false;
      bool ready_for_sleep = false;
      bool sleep_done = false;
      std::atomic<int> rem{2};
      RecordingThreadQueue* q = nullptr;
    } ss;
    RecordingThreadQueue q_susp(1, "q_susp");
    ss.q = &q_susp;
    struct SuspJob final : public kdu_thread_job {
      SuspState* s = nullptr;
    } susp_job;
    susp_job.s = &ss;
    susp_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<SuspJob*>(base);
      {
        std::lock_guard<std::mutex> lock(j->s->mu);
        j->s->worker = caller;
      }
      j->s->cv.notify_all();
      {
        std::unique_lock<std::mutex> lock(j->s->mu);
        j->s->cv.wait(lock, [j] { return j->s->pre_done; });
      }
      // Race case: pre_done means unsuspend_thread was already called once.
      EXPECT_TRUE(caller->suspend(true));
      {
        std::lock_guard<std::mutex> lock(j->s->mu);
        j->s->ready_for_sleep = true;
      }
      j->s->cv.notify_all();
      EXPECT_TRUE(caller->suspend(true));
      {
        std::lock_guard<std::mutex> lock(j->s->mu);
        j->s->sleep_done = true;
      }
      j->s->cv.notify_all();
      ReleaseGate(&j->s->rem, j->s->q, caller);
    });

    EXPECT_TRUE(senv.attach_queue(&q_susp, nullptr, "susp_dom"));
    q_susp.ScheduleOne(&susp_job, &senv, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&ss.rem, &q_susp, &senv);
    {
      std::unique_lock<std::mutex> lock(ss.mu);
      ss.cv.wait(lock, [&ss] { return ss.worker != nullptr; });
      // Duplicate pre-unsuspend call before suspend() hits the already-pending
      // unsuspend branch.
      // TODO(schwehr): Upstream kdu_thread_entity::unsuspend_thread(nullptr)
      // dereferences `thread` without a null check (UBSan SIGILL).
      senv.unsuspend_thread(ss.worker);
      senv.unsuspend_thread(ss.worker);
      ss.pre_done = true;
    }
    ss.cv.notify_all();
    {
      std::unique_lock<std::mutex> lock(ss.mu);
      ss.cv.wait(lock, [&ss] { return ss.ready_for_sleep; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    senv.unsuspend_thread(ss.worker);
    // Wait until the worker returns from enter_wait()/return_from_waiting() so
    // join(&q_susp) does not observe n_active == 0 and treat all threads idle
    // with no foreign wakeup expected as deadlock.
    {
      std::unique_lock<std::mutex> lock(ss.mu);
      ss.cv.wait(lock, [&ss] { return ss.sleep_done; });
    }
    kdu_exception susp_exc = 0;
    EXPECT_TRUE(senv.join(&q_susp, /*descendants_only=*/false, &susp_exc))
        << "susp_exc=" << susp_exc;

    // Nested wait_for_condition: outer expect_foreign_wakeup=true on owner,
    // inner expect_foreign_wakeup=false processed while owner is waiting.
    // With 0 workers on nest_env, the owner deterministically executes both
    // j_nested and sig_job inside ww_process_jobs().
    {
      ScopedThreadEntity nest_scoped;
      kdu_thread_entity& nenv = nest_scoped.entity;
      nenv.create();
      kdu_thread_entity_condition* outer_cond = nenv.get_condition();
      RecordingThreadQueue q_outer(1, "q_nested_outer");
      RecordingThreadQueue q_inner_sig(1, "q_nested_sig");
      EXPECT_TRUE(nenv.attach_queue(&q_outer, nullptr, "nest_dom"));
      EXPECT_TRUE(nenv.attach_queue(&q_inner_sig, &q_outer, "nest_dom"));

      struct SigJob final : public kdu_thread_job {
        kdu_thread_entity* target = nullptr;
        kdu_thread_entity_condition* cond = nullptr;
        RecordingThreadQueue* q = nullptr;
      } sig_job;
      struct NestedJob final : public kdu_thread_job {
        kdu_thread_entity* env = nullptr;
        kdu_thread_entity_condition* outer_cond = nullptr;
        RecordingThreadQueue* outer_q = nullptr;
        RecordingThreadQueue* sig_q = nullptr;
        SigJob* sig_job = nullptr;
        bool inner_ran = false;
      } j_nested;
      j_nested.env = &nenv;
      j_nested.outer_cond = outer_cond;
      j_nested.outer_q = &q_outer;
      j_nested.sig_q = &q_inner_sig;
      j_nested.sig_job = &sig_job;
      j_nested.set_job_func([](kdu_thread_job* base,
                               kdu_thread_entity* caller) {
        auto* j = static_cast<NestedJob*>(base);
        EXPECT_EQ(j->env, caller);
        kdu_thread_entity_condition* inner_cond = caller->get_condition();
        j->sig_job->target = caller;
        j->sig_job->cond = inner_cond;
        j->sig_job->q = j->sig_q;
        j->sig_job->set_job_func([](kdu_thread_job* sb, kdu_thread_entity* sc) {
          auto* s = static_cast<SigJob*>(sb);
          s->q->Complete(sc);
          s->target->signal_condition(s->cond);
        });
        j->sig_q->ScheduleOne(j->sig_job, caller, /*all_scheduled=*/true,
                              KDU_THREAD_JOB_AUTO_BIND_ONCE);
        caller->wait_for_condition("inner_false", false);
        j->inner_ran = true;
        j->outer_q->Complete(caller);
        j->env->signal_condition(j->outer_cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      q_outer.ScheduleOne(&j_nested, &nenv, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
      std::thread foreign_waker([&nenv, outer_cond] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        nenv.signal_condition(outer_cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      nenv.wait_for_condition("outer_true", true);
      foreign_waker.join();
      EXPECT_TRUE(j_nested.inner_ran);
      EXPECT_TRUE(nenv.join(nullptr));
    }
  }
}

TEST_F(KduThreadsTest, ErrorHandlingDeadlocksAndSequenceReuse) {
  // 1) Deadlock detection and hierarchical deadlock report in owner-only
  // ww_process_jobs().
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    RecordingThreadQueue q_parent(1, "dl_dom");
    RecordingThreadQueue q_child(1, "dl_dom");
    EXPECT_TRUE(env.attach_queue(&q_parent, nullptr, "dl_dom"));
    EXPECT_TRUE(env.attach_queue(&q_child, &q_parent, "dl_dom"));
    kdu_exception exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(env.join(&q_parent, /*descendants_only=*/false, &exc));
    EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
    // Calling join/terminate again after an error returns false cleanly.
    EXPECT_FALSE(env.terminate(nullptr, false, &exc));
    EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
    EXPECT_FALSE(env.destroy());
  }

  // 2) Deadlock detection in worker process_jobs() after workers are warmed up
  // so all threads have a valid condition stack.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread());
    WarmupWorkers(env);
    RecordingThreadQueue q_dl(1, "dl_worker");
    EXPECT_TRUE(env.attach_queue(&q_dl, nullptr, "dl_worker"));
    kdu_exception exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(env.join(&q_dl, /*descendants_only=*/false, &exc));
    EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
    EXPECT_FALSE(env.destroy());
  }

  // TODO(schwehr): Subtest 3 (wait_for_exceptional_join() blocking path in
  // kdu_threads.cpp:3443-3489) is disabled because upstream stores 0-based
  // `this->thread_idx << 16` in `wakeup_info` (line 3443) while waking up
  // 1-based `wait_managers[old_ejd - 1]` (lines 3474, 4047), which leaves the
  // owner un-woken (`old_ejd >> 16 == 0`) or wakes the wrong thread, hanging.

  // 4) attach_queue / add_queue error paths, destructor warning/force_detach,
  // bind_jobs/schedule error paths, and terminate() on done parent + child.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();

    RecordingThreadQueue q_safe(1, "safe_err");
    EXPECT_THROW(env.attach_queue(&q_safe, nullptr, "safe_err", 0,
                                  KDU_THREAD_QUEUE_SAFE_CONTEXT),
                 kdu_exception);

    ScopedThreadEntity uninit_scoped;
    EXPECT_EQ(nullptr,
              uninit_scoped.entity.add_queue(nullptr, nullptr, "uninit"));
    RecordingThreadQueue q_empty_dom(1, "empty_dom");
    EXPECT_FALSE(env.attach_queue(&q_empty_dom, nullptr, ""));
    EXPECT_FALSE(env.attach_queue(&q_empty_dom, nullptr, nullptr));

    RecordingThreadQueue q_unowned(1, "unowned");
    EXPECT_TRUE(env.attach_queue(&q_unowned, nullptr, "unowned"));
    EXPECT_EQ(nullptr, env.add_queue(nullptr, &q_unowned, "unowned"));
    q_unowned.force_detach(&env);
    EXPECT_TRUE(env.join(&q_unowned));

    // Exercise group-owned add_queue() lifecycle (parent + child).
    kdu_thread_queue* owned_parent = env.add_queue(nullptr, nullptr, "add_ok");
    ASSERT_NE(nullptr, owned_parent);
    kdu_thread_queue* owned_child =
        env.add_queue(nullptr, owned_parent, "add_ok");
    ASSERT_NE(nullptr, owned_child);
    EXPECT_TRUE(env.join(owned_parent));

    // Destroying a non-group-owned queue while still attached triggers a
    // warning and force_detach(nullptr).
    {
      RecordingThreadQueue q_abandoned(1, "abandoned");
      EXPECT_TRUE(env.attach_queue(&q_abandoned, nullptr, "abandoned"));
    }
    EXPECT_GT(warning_handler_.WarningCount(), 0);

    // bind_jobs error paths, note_all_scheduled idempotency, and schedule_job
    // error paths.
    RecordingThreadQueue q_zero(0, "zero");
    EXPECT_TRUE(env.attach_queue(&q_zero, nullptr, "zero"));
    CounterJob j0;
    CounterJob j1;
    EXPECT_THROW(q_zero.ScheduleOne(&j1, &env, false, 0), kdu_exception);
    q_zero.Complete(&env);
    EXPECT_TRUE(env.join(&q_zero));

    RecordingThreadQueue q_api(2, "api_err");
    EXPECT_TRUE(env.attach_queue(&q_api, nullptr, "api_err"));
    RecordingThreadQueue q_api_child(1, "api_err");
    EXPECT_TRUE(env.attach_queue(&q_api_child, &q_api, "api_err"));
    j0.queue = &q_api;
    j0.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<CounterJob*>(base);
      j->queue->Complete(caller);
    });
    j1.set_job_func(
        [](kdu_thread_job* /*base*/, kdu_thread_entity* /*caller*/) {});
    kdu_thread_job* ptrs[2] = {&j0, &j1};
    // Exceeds registered_max_jobs (range_start=1 + num_jobs=2 > 2).
    EXPECT_THROW(q_api.BindRange(ptrs, 2, 1), kdu_exception);
    q_api.ScheduleOne(&j0, &env, /*all_scheduled=*/false,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
    // NoteAllScheduled is idempotent.
    q_api.NoteAllScheduled(&env);
    q_api.NoteAllScheduled(&env);
    // bind_jobs and schedule_job after all_scheduled == true throw.
    EXPECT_THROW(q_api.BindRange(&ptrs[1], 1, 0), kdu_exception);
    EXPECT_THROW(q_api.ScheduleOne(&j1, &env, /*all_scheduled=*/true,
                                   KDU_THREAD_JOB_AUTO_BIND_ONCE),
                 kdu_exception);
    q_api_child.Complete(&env);
    EXPECT_TRUE(env.terminate(&q_api, /*descendants_only=*/true));
    EXPECT_TRUE(env.terminate(&q_api));
    // Re-terminating a detached queue returns true immediately.
    EXPECT_TRUE(env.terminate(&q_api));
    EXPECT_TRUE(env.destroy());
  }

  // 5) Domain sequence reuse (remove_unused_sequences + free_sequences) and
  // worker yield wakeups.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread("seq_dom"));
    EXPECT_TRUE(env.add_thread("seq_dom"));
    env.set_yield_frequency(1);
    WarmupWorkers(env);

    for (kdu_long seq_idx = 1; seq_idx <= 8; ++seq_idx) {
      RecordingThreadQueue q(4, "seq_q");
      EXPECT_TRUE(env.attach_queue(&q, nullptr, "seq_dom", seq_idx));
      std::atomic<int> rem{5};
      std::atomic<int> completed{0};
      CounterJob jobs[4];
      for (auto& job : jobs) {
        job.set_job_func(IncrementJobFunc);
        job.completed = &completed;
        job.remaining = &rem;
        job.queue = &q;
        q.ScheduleOne(&job, &env, /*all_scheduled=*/(&job == &jobs[3]),
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      ReleaseGate(&rem, &q, &env);
      env.advance_work_domains();
      EXPECT_TRUE(env.join(&q));
      EXPECT_EQ(4, completed.load());
    }
    EXPECT_TRUE(env.destroy());
  }

  // 6) kdu_thread_context multi-lock alignment, throwing num_threads_changed,
  // and leave_group(nullptr).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();

    class MultiLockContext final : public kdu_thread_context {
     public:
      int get_num_locks() override { return 4; }
    } multi_ctx;
    multi_ctx.enter_group(&env);
    multi_ctx.acquire_lock(0, &env);
    EXPECT_TRUE(multi_ctx.check_lock(0, &env));
    multi_ctx.release_lock(0, &env);
    multi_ctx.acquire_lock(3, &env);
    EXPECT_TRUE(multi_ctx.check_lock(3, &env));
    multi_ctx.release_lock(3, &env);
    multi_ctx.leave_group(nullptr);

    class ThrowingContext final : public kdu_thread_context {
     public:
      void num_threads_changed(int /*new_num_threads*/) override {
        throw static_cast<kdu_exception>(321);
      }
    } throw_ctx;
    EXPECT_THROW(throw_ctx.enter_group(&env), kdu_exception);
    throw_ctx.leave_group(&env);
    EXPECT_TRUE(env.destroy());
  }

  // 7) Foreign signal without expect_foreign_wakeup=true error, and exceeding
  // KD_WW_MAX_WAITERS when supported by 64-bit bitmask.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();

    // With 0 workers, env executes probe_job synchronously inside
    // wait_for_condition("local_expect", false) after start_waiting() has
    // already registered wwid=0 with expecting_foreign_wakeup=false. Signaling
    // from foreign std::thread inside probe_job deterministically hits
    // 0x18012102.
    struct ProbeState {
      std::mutex mu;
      std::condition_variable cv;
      bool in_wait = false;
      bool foreign_done = false;
      kdu_thread_entity_condition* cond = nullptr;
      kdu_thread_entity* env = nullptr;
    } ps;
    ps.env = &env;
    RecordingThreadQueue q_probe(1, "probe");
    struct ProbeJob final : public kdu_thread_job {
      ProbeState* s = nullptr;
      RecordingThreadQueue* q = nullptr;
    } probe_job;
    probe_job.s = &ps;
    probe_job.q = &q_probe;
    probe_job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<ProbeJob*>(base);
      {
        std::lock_guard<std::mutex> lock(j->s->mu);
        j->s->in_wait = true;
      }
      j->s->cv.notify_all();
      {
        std::unique_lock<std::mutex> lock(j->s->mu);
        j->s->cv.wait(lock, [j] { return j->s->foreign_done; });
      }
      j->q->Complete(caller);
    });
    EXPECT_TRUE(env.attach_queue(&q_probe, nullptr, "probe"));
    q_probe.ScheduleOne(&probe_job, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ps.cond = env.get_condition();
    std::thread foreign([&ps] {
      {
        std::unique_lock<std::mutex> lock(ps.mu);
        ps.cv.wait(lock, [&ps] { return ps.in_wait; });
      }
      EXPECT_THROW(
          ps.env->signal_condition(ps.cond, KDU_FOREIGN_SIGNAL_EXPECTED),
          kdu_exception);
      {
        std::lock_guard<std::mutex> lock(ps.mu);
        ps.foreign_done = true;
      }
      ps.cv.notify_all();
    });
    env.wait_for_condition("local_expect", /*expect_foreign_wakeup=*/false);
    foreign.join();
    EXPECT_TRUE(env.join(&q_probe));
    EXPECT_TRUE(env.destroy());
  }

  // 8) Exceed KD_WW_MAX_WAITERS with concurrent working waiters.
  // Owner is thread 0 (`wwid == 0`), leaving 29 worker WWIDs on 64-bit
  // (`KD_WW_MAX_WAITERS == 30`) or 13 on 32-bit (`14`), so 30 workers
  // (14 on 32-bit) each holding 1 job at a barrier before entering
  // `wait_for_condition` at top level guarantees exactly one worker overflows
  // `acquire_wwid()` without two workers invoking `handle_exception`
  // concurrently (and triggering the unsynchronized `event_lock.lock_if_unheld`
  // read under TSan).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    constexpr int kWorkers = (KDU_MAX_THREADS >= 64) ? 30 : 14;
    for (int i = 0; i < kWorkers; ++i) {
      ASSERT_TRUE(env.add_thread());
    }

    std::mutex barrier_mu;
    std::condition_variable barrier_cv;
    int arrived = 0;
    std::atomic<int> rem{kWorkers};
    struct WaiterJob final : public kdu_thread_job {
      std::mutex* mu = nullptr;
      std::condition_variable* cv = nullptr;
      int* arrived = nullptr;
      int target = 0;
      std::atomic<int>* rem = nullptr;
      RecordingThreadQueue* q = nullptr;
    };
    RecordingThreadQueue q_ww(kWorkers, "ww_limit");
    std::vector<WaiterJob> wjobs(kWorkers);
    EXPECT_TRUE(env.attach_queue(&q_ww, nullptr, "ww_limit"));
    for (int i = 0; i < kWorkers; ++i) {
      wjobs[i].mu = &barrier_mu;
      wjobs[i].cv = &barrier_cv;
      wjobs[i].arrived = &arrived;
      wjobs[i].target = kWorkers;
      wjobs[i].rem = &rem;
      wjobs[i].q = &q_ww;
      wjobs[i].set_job_func([](kdu_thread_job* base,
                               kdu_thread_entity* caller) {
        auto* j = static_cast<WaiterJob*>(base);
        {
          std::unique_lock<std::mutex> lock(*j->mu);
          if (++(*j->arrived) == j->target) {
            j->cv->notify_all();
          } else {
            j->cv->wait(lock, [j] { return *j->arrived >= j->target; });
          }
        }
        try {
          caller->wait_for_condition("ww_slot", /*expect_foreign_wakeup=*/true);
        } catch (kdu_exception) {
          // Do not rethrow into process_jobs(): ww_process_jobs() already
          // invoked handle_exception(), so rethrowing across 30 workers would
          // call handle_exception() concurrently and race on lock_if_unheld().
          ReleaseGate(j->rem, j->q, caller);
          return;
        }
        ReleaseGate(j->rem, j->q, caller);
      });
      q_ww.ScheduleOne(&wjobs[i], &env, /*all_scheduled=*/(i == kWorkers - 1),
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }

    {
      std::unique_lock<std::mutex> lock(barrier_mu);
      barrier_cv.wait(lock,
                      [&arrived, kWorkers] { return arrived >= kWorkers; });
    }
    kdu_exception exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(env.join(&q_ww, false, &exc));
    EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
    EXPECT_FALSE(env.destroy());
  }
}

TEST_F(KduThreadsTest, AdditionalBranchCoverageSweep) {
  // 1) Affinity setup before worker creation, NUMA helpers, bundle affinity,
  // and change_group_owner_thread() with owner affinity mask set.
  {
    (void)kdu_thread_entity_affinity::check_numa_support(1);
    EXPECT_GE(kdu_thread_entity_affinity::find_preferred_bundle_count(1), 1);

    kdu_thread_entity_affinity aff;
    (void)aff.add_node_bundle(0, 1);
    aff.add_thread_bundle(1, 1, 0);
    EXPECT_GE(aff.get_num_bundles(), 1);

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create(/*cpu_affinity=*/1, /*also_set_owner_affinity=*/true);
    EXPECT_TRUE(env.set_cpu_affinity(aff));
    EXPECT_TRUE(env.add_thread());
    WarmupWorkers(env);

    bool switched = false;
    std::thread mover(
        [&env, &switched] { switched = env.change_group_owner_thread(); });
    mover.join();
    EXPECT_TRUE(switched);
    EXPECT_TRUE(env.change_group_owner_thread());
    EXPECT_TRUE(env.destroy());
  }

  // 2) Worker pre_launch() exceptions in worker_startproc (kdu_exception and
  // unknown std::exception caught via catch(...)).
  {
    class ThrowOnWorkerContext final : public kdu_thread_context {
     public:
      explicit ThrowOnWorkerContext(bool throw_kdu) : throw_kdu_(throw_kdu) {}
      void num_threads_changed(int new_num_threads) override {
        if (new_num_threads > 1) {
          if (throw_kdu_) {
            throw static_cast<kdu_exception>(654);
          }
          throw std::runtime_error("pre_launch_non_kdu");
        }
      }
      void handle_exception(kdu_thread_entity* caller) override {
        kdu_thread_context::handle_exception(caller);
        {
          std::lock_guard<std::mutex> lock(mu_);
          handled_ = true;
        }
        cv_.notify_all();
      }
      void WaitHandled() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return handled_; });
      }

     private:
      bool throw_kdu_;
      std::mutex mu_;
      std::condition_variable cv_;
      bool handled_ = false;
    };

    {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      ThrowOnWorkerContext ctx(true);
      ctx.enter_group(&env);
      EXPECT_TRUE(env.add_thread());
      ctx.WaitHandled();
      ctx.leave_group(&env);
      EXPECT_FALSE(env.destroy());
    }
    {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      ThrowOnWorkerContext ctx(false);
      ctx.enter_group(&env);
      EXPECT_TRUE(env.add_thread());
      ctx.WaitHandled();
      ctx.leave_group(&env);
      EXPECT_FALSE(env.destroy());
    }
  }

  // 3) Unhandled exceptions thrown from jobs inside process_jobs() (worker)
  // and ww_process_jobs() (working waiter), plus catch(std::bad_alloc&).
  {
    auto run_worker_throw = [](int mode, kdu_exception expected_exc) {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      EXPECT_TRUE(env.add_thread());
      WarmupWorkers(env);

      RecordingThreadQueue q(1, "wthrow");
      struct ThrowJob final : public kdu_thread_job {
        int mode = 0;
        RecordingThreadQueue* q = nullptr;
        std::mutex mu;
        std::condition_variable cv;
        bool started = false;
      } job;
      job.mode = mode;
      job.q = &q;
      job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
        (void)caller;
        auto* j = static_cast<ThrowJob*>(base);
        {
          std::lock_guard<std::mutex> lock(j->mu);
          j->started = true;
        }
        j->cv.notify_one();
        if (j->mode == 0) {
          throw static_cast<kdu_exception>(777);
        }
        if (j->mode == 1) {
          throw std::bad_alloc();
        }
        throw std::runtime_error("job_non_kdu");
      });
      EXPECT_TRUE(env.attach_queue(&q, nullptr, "wthrow"));
      q.ScheduleOne(&job, &env, /*all_scheduled=*/true,
                    KDU_THREAD_JOB_AUTO_BIND_ONCE);
      {
        std::unique_lock<std::mutex> lock(job.mu);
        job.cv.wait(lock, [&] { return job.started; });
      }
      kdu_exception out = KDU_NULL_EXCEPTION;
      EXPECT_FALSE(env.join(&q, false, &out));
      EXPECT_EQ(expected_exc, out);
      EXPECT_FALSE(env.destroy());
    };
    run_worker_throw(0, static_cast<kdu_exception>(777));
    run_worker_throw(1, KDU_MEMORY_EXCEPTION);
    run_worker_throw(2, KDU_CONVERTED_EXCEPTION);

    auto run_ww_throw = [](int mode, kdu_exception expected_exc) {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      RecordingThreadQueue q(1, "wwthrow");
      struct ThrowJob final : public kdu_thread_job {
        int mode = 0;
        RecordingThreadQueue* q = nullptr;
      } job;
      job.mode = mode;
      job.q = &q;
      job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
        (void)caller;
        auto* j = static_cast<ThrowJob*>(base);
        if (j->mode == 1) {
          throw std::bad_alloc();
        }
        throw std::runtime_error("ww_non_kdu");
      });
      EXPECT_TRUE(env.attach_queue(&q, nullptr, "wwthrow"));
      q.ScheduleOne(&job, &env, /*all_scheduled=*/true,
                    KDU_THREAD_JOB_AUTO_BIND_ONCE);
      kdu_exception out = KDU_NULL_EXCEPTION;
      EXPECT_FALSE(env.join(&q, false, &out));
      EXPECT_EQ(expected_exc, out);
      EXPECT_FALSE(env.destroy());
    };
    run_ww_throw(1, KDU_MEMORY_EXCEPTION);
    run_ww_throw(2, KDU_CONVERTED_EXCEPTION);
  }

  // 4) Worker deadlock detection in process_jobs() while owner waits in
  // wait_for_condition(expect_foreign_wakeup=false).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread());
    WarmupWorkers(env);

    std::mutex mu;
    std::condition_variable cv;
    bool worker_started = false;
    RecordingThreadQueue q(1, "wdeadlock");
    struct DLJob final : public kdu_thread_job {
      std::mutex* mu = nullptr;
      std::condition_variable* cv = nullptr;
      bool* started = nullptr;
      RecordingThreadQueue* q = nullptr;
    } job;
    job.mu = &mu;
    job.cv = &cv;
    job.started = &worker_started;
    job.q = &q;
    job.set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<DLJob*>(base);
      {
        std::lock_guard<std::mutex> lock(*j->mu);
        *j->started = true;
      }
      j->cv->notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      j->q->Complete(caller);
    });
    EXPECT_TRUE(env.attach_queue(&q, nullptr, "wdeadlock"));
    q.ScheduleOne(&job, &env, /*all_scheduled=*/true,
                  KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lock(mu);
      cv.wait(lock, [&worker_started] { return worker_started; });
    }
    EXPECT_THROW(env.wait_for_condition("owner_idle", false), kdu_exception);
    EXPECT_FALSE(env.destroy());
  }

  // 5) Nested wait_for_condition toggling expect_foreign_wakeup from true
  // (outer) to false (inner).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    kdu_thread_entity_condition* outer_cond = env.get_condition();

    RecordingThreadQueue q(2, "nested_fw");
    struct InnerState {
      kdu_thread_entity_condition* outer = nullptr;
      kdu_thread_entity_condition* inner = nullptr;
      RecordingThreadQueue* q = nullptr;
    } is;
    is.outer = outer_cond;
    is.q = &q;

    struct InnerJob final : public kdu_thread_job {
      InnerState* s = nullptr;
      bool signaler = false;
    } j_wait, j_sig;
    j_wait.s = &is;
    j_wait.signaler = false;
    j_sig.s = &is;
    j_sig.signaler = true;

    auto fn = [](kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* j = static_cast<InnerJob*>(base);
      if (!j->signaler) {
        j->s->inner = caller->get_condition();
        caller->wait_for_condition("inner_false", false);
      } else {
        caller->signal_condition(j->s->inner);
        caller->signal_condition(j->s->outer);
        j->s->q->Complete(caller);
      }
    };
    j_wait.set_job_func(fn);
    j_sig.set_job_func(fn);
    kdu_thread_job* jobs[2] = {&j_wait, &j_sig};
    EXPECT_TRUE(env.attach_queue(&q, nullptr, "nested_fw"));
    q.BindRange(jobs, 2, 0);
    q.ScheduleBatch(jobs, 2, &env, /*all_scheduled=*/true);
    env.wait_for_condition("outer_true", /*expect_foreign_wakeup=*/true);
    EXPECT_TRUE(env.join(&q));
    EXPECT_TRUE(env.destroy());
  }

  // 6) Large palette allocation (>256 palettes -> augment_free_palettes +
  // returned_palettes), KDU_THREAD_QUEUE_BACKGROUND queues, update_priorities,
  // and advance_work_domains() advancing terminated domain sequences.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread("adv_dom"));
    EXPECT_TRUE(env.add_thread());
    WarmupWorkers(env);

    // Trigger augment_free_palettes() and returned_palettes reuse.
    {
      RecordingThreadQueue q_big(300, "big_plt");
      EXPECT_TRUE(env.attach_queue(&q_big, nullptr, "big_plt"));
      std::vector<CounterJob> jobs(270);
      std::vector<kdu_thread_job*> ptrs(270);
      std::atomic<int> rem{270};
      for (int i = 0; i < 270; ++i) {
        jobs[i].remaining = &rem;
        jobs[i].queue = &q_big;
        jobs[i].set_job_func(IncrementJobFunc);
        ptrs[i] = &jobs[i];
      }
      q_big.BindRange(ptrs.data(), 270, 0);
      q_big.ScheduleBatch(ptrs.data(), 270, &env, /*all_scheduled=*/true);
      EXPECT_TRUE(env.join(&q_big));
    }

    // Background queues (top-level & child), priority update, and sequence
    // advancement in advance_work_domains().
    RecordingThreadQueue q_s1(1, "s1");
    RecordingThreadQueue q_s2(1, "s2");
    RecordingThreadQueue q_bg_top(1, "bg_top");
    RecordingThreadQueue q_bg_child(1, "bg_child");
    EXPECT_TRUE(env.attach_queue(&q_s1, nullptr, "adv_dom", 1, 0));
    EXPECT_TRUE(env.attach_queue(&q_s2, nullptr, "adv_dom", 2,
                                 KDU_THREAD_QUEUE_PRIORITY_HIGH));
    EXPECT_TRUE(env.attach_queue(&q_bg_top, nullptr, "bg_dom", 1,
                                 KDU_THREAD_QUEUE_BACKGROUND));
    EXPECT_TRUE(env.attach_queue(&q_bg_child, &q_bg_top, "bg_dom", 1,
                                 KDU_THREAD_QUEUE_BACKGROUND));

    CounterJob j_s1, j_s2, j_bg1, j_bg2;
    j_s1.queue = &q_s1;
    j_s1.set_job_func([](kdu_thread_job* b, kdu_thread_entity* c) {
      static_cast<CounterJob*>(b)->queue->Complete(c);
    });
    j_s2.queue = &q_s2;
    j_s2.set_job_func([](kdu_thread_job* b, kdu_thread_entity* c) {
      static_cast<CounterJob*>(b)->queue->Complete(c);
    });
    j_bg1.queue = &q_bg_top;
    j_bg1.set_job_func([](kdu_thread_job* b, kdu_thread_entity* c) {
      static_cast<CounterJob*>(b)->queue->Complete(c);
    });
    j_bg2.queue = &q_bg_child;
    j_bg2.set_job_func([](kdu_thread_job* b, kdu_thread_entity* c) {
      static_cast<CounterJob*>(b)->queue->Complete(c);
    });

    q_s1.ScheduleOne(&j_s1, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&q_s1));
    env.advance_work_domains();

    q_s2.ScheduleOne(&j_s2, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    q_bg_top.ScheduleOne(&j_bg1, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    q_bg_child.ScheduleOne(&j_bg2, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(nullptr));
    env.advance_work_domains();
    EXPECT_TRUE(env.destroy());
  }
}

TEST_F(KduThreadsTest, AdditionalQueueDomainAndBindCoverage) {
  // 1) Queue bind/schedule error branches, multi-palette schedule_jobs (> 7
  // slots), force_detach(), ~kdu_thread_queue() while still attached, and
  // add_queue() validation failures.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread());
    WarmupWorkers(env);

    ScopedThreadEntity other_scoped;
    kdu_thread_entity& other_env = other_scoped.entity;
    other_env.create();

    RecordingThreadQueue q_zero(0, "zero");
    EXPECT_TRUE(env.attach_queue(&q_zero, nullptr, ""));
    CounterJob dummy_job;
    kdu_thread_job* dummy_ptr = &dummy_job;
    q_zero.ScheduleBatch(nullptr, 0, &env, false);
    EXPECT_THROW(q_zero.ScheduleBatch(&dummy_ptr, 1, &env, false),
                 kdu_exception);
    EXPECT_THROW(q_zero.BindRange(&dummy_ptr, 1, 0), kdu_exception);
    EXPECT_THROW(q_zero.ScheduleOne(&dummy_job, &env, false,
                                    KDU_THREAD_JOB_AUTO_BIND_ONCE),
                 kdu_exception);
    EXPECT_TRUE(env.join(&q_zero));

    // Safe-context queue on a 1-thread group (0 workers) throws kdu_exception.
    RecordingThreadQueue q_bad_safe(1, "bad_safe");
    EXPECT_THROW(other_env.attach_queue(&q_bad_safe, nullptr, "bad_safe", 0,
                                        KDU_THREAD_QUEUE_SAFE_CONTEXT),
                 kdu_exception);

    // Cross-group schedule_jobs / schedule_job throw kdu_exception.
    RecordingThreadQueue q_cg(1, "cg");
    EXPECT_TRUE(env.attach_queue(&q_cg, nullptr, "cg"));
    EXPECT_THROW(q_cg.ScheduleBatch(&dummy_ptr, 1, &other_env, false),
                 kdu_exception);
    EXPECT_THROW(q_cg.ScheduleOne(&dummy_job, &other_env, false,
                                  KDU_THREAD_JOB_AUTO_BIND_ONCE),
                 kdu_exception);
    q_cg.NoteAllScheduled(&env);
    q_cg.Complete(&env);
    EXPECT_TRUE(env.join(&q_cg));

    // schedule_job REBIND_0 vs AUTO_BIND_ONCE vs unbound, plus bind_jobs range
    // and post-all_scheduled error branches.
    {
      struct SyncJob final : public kdu_thread_job {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
      };
      auto sync_fn = [](kdu_thread_job* base, kdu_thread_entity*) {
        auto* j = static_cast<SyncJob*>(base);
        {
          std::lock_guard<std::mutex> lock(j->mu);
          j->done = true;
        }
        j->cv.notify_one();
      };

      RecordingThreadQueue q_rb(1, "rebind");
      EXPECT_TRUE(env.attach_queue(&q_rb, nullptr, "rebind"));
      SyncJob j_rb1;
      SyncJob j_rb2;
      j_rb1.set_job_func(sync_fn);
      j_rb2.set_job_func(sync_fn);
      q_rb.ScheduleOne(&j_rb1, &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_REBIND_0);
      {
        std::unique_lock<std::mutex> lock(j_rb1.mu);
        j_rb1.cv.wait(lock, [&j_rb1] { return j_rb1.done; });
      }
      EXPECT_THROW(
          q_rb.ScheduleOne(&j_rb2, &env, false, KDU_THREAD_JOB_AUTO_BIND_ONCE),
          kdu_exception);
      EXPECT_THROW(q_rb.ScheduleOne(&j_rb2, &env, false, 0), kdu_exception);
      kdu_thread_job* rb_ptr = &j_rb2;
      EXPECT_THROW(q_rb.BindRange(&rb_ptr, 1, 1), kdu_exception);
      q_rb.NoteAllScheduled(&env);
      EXPECT_THROW(q_rb.BindRange(&rb_ptr, 1, 0), kdu_exception);
      EXPECT_THROW(q_rb.ScheduleBatch(&rb_ptr, 1, &env, false), kdu_exception);
      EXPECT_THROW(
          q_rb.ScheduleOne(&j_rb2, &env, false, KDU_THREAD_JOB_REBIND_0),
          kdu_exception);
      q_rb.Complete(&env);
      EXPECT_TRUE(env.join(&q_rb));

      RecordingThreadQueue q_ab(1, "autobind_err");
      EXPECT_TRUE(env.attach_queue(&q_ab, nullptr, "autobind_err"));
      SyncJob j_ab1;
      SyncJob j_ab2;
      j_ab1.set_job_func(sync_fn);
      j_ab2.set_job_func(sync_fn);
      q_ab.ScheduleOne(&j_ab1, &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_THROW(
          q_ab.ScheduleOne(&j_ab2, &env, false, KDU_THREAD_JOB_REBIND_0),
          kdu_exception);
      EXPECT_THROW(
          q_ab.ScheduleOne(&j_ab2, &env, false, KDU_THREAD_JOB_AUTO_BIND_ONCE),
          kdu_exception);
      {
        std::unique_lock<std::mutex> lock(j_ab1.mu);
        j_ab1.cv.wait(lock, [&j_ab1] { return j_ab1.done; });
      }
      q_ab.NoteAllScheduled(&env);
      q_ab.Complete(&env);
      EXPECT_TRUE(env.join(&q_ab));
    }

    // Batch > KD_PLT_SLOTS (7) in a single schedule_jobs() call.
    {
      constexpr int kBatch20 = 20;
      RecordingThreadQueue q_b20(kBatch20, "batch20");
      EXPECT_TRUE(env.attach_queue(&q_b20, nullptr, "batch20"));
      std::vector<CounterJob> jobs(kBatch20);
      std::vector<kdu_thread_job*> ptrs(kBatch20);
      std::atomic<int> rem{kBatch20};
      std::atomic<int> completed{0};
      for (int i = 0; i < kBatch20; ++i) {
        jobs[i].remaining = &rem;
        jobs[i].completed = &completed;
        jobs[i].queue = &q_b20;
        jobs[i].set_job_func(IncrementJobFunc);
        ptrs[i] = &jobs[i];
      }
      q_b20.BindRange(ptrs.data(), kBatch20, 0);
      q_b20.ScheduleBatch(ptrs.data(), kBatch20, &env, /*all_scheduled=*/true);
      EXPECT_TRUE(env.join(&q_b20));
      EXPECT_EQ(kBatch20, completed.load());
    }

    // force_detach(caller), force_detach(nullptr), idempotent force_detach(),
    // and ~kdu_thread_queue() while attached (emits dev warning +
    // force_detach).
    {
      RecordingThreadQueue q_fd1(1, "fd1");
      EXPECT_TRUE(env.attach_queue(&q_fd1, nullptr, "fd1"));
      EXPECT_TRUE(q_fd1.is_attached());
      q_fd1.force_detach(&env);
      EXPECT_FALSE(q_fd1.is_attached());
      q_fd1.force_detach(nullptr);

      RecordingThreadQueue q_fd2(1, "fd2");
      EXPECT_TRUE(env.attach_queue(&q_fd2, nullptr, "fd2"));
      q_fd2.force_detach(nullptr);
      EXPECT_FALSE(q_fd2.is_attached());

      const int warnings_before = warning_handler_.WarningCount();
      {
        RecordingThreadQueue q_warn(1, "warn_dtor");
        EXPECT_TRUE(env.attach_queue(&q_warn, nullptr, "warn_dtor"));
      }
      EXPECT_GT(warning_handler_.WarningCount(), warnings_before);
    }

    // add_queue() rejects non-belongs_to_group and foreign-group super_queues.
    {
      RecordingThreadQueue q_stack(1, "stk");
      EXPECT_TRUE(env.attach_queue(&q_stack, nullptr, "stk"));
      EXPECT_EQ(nullptr, env.add_queue(nullptr, &q_stack, "sub"));
      q_stack.force_detach(&env);

      RecordingThreadQueue q_og(1, "og");
      EXPECT_TRUE(other_env.attach_queue(&q_og, nullptr, "og"));
      EXPECT_EQ(nullptr, env.add_queue(nullptr, &q_og, "sub"));
      q_og.force_detach(&other_env);
    }

    // request_termination() + all_done() when CSTATE_T is set, plus background
    // note_join_attempt().
    {
      class TerminableQueue final : public kdu_thread_queue {
       public:
        int get_max_jobs() override { return 1; }
        void request_termination(kdu_thread_entity* caller) override {
          term_calls.fetch_add(1);
          all_done(caller);
        }
        std::atomic<int> term_calls{0};
      };
      TerminableQueue q_parent_term;
      TerminableQueue q_child_term;
      EXPECT_TRUE(env.attach_queue(&q_parent_term, nullptr, "term_dom"));
      EXPECT_TRUE(env.attach_queue(&q_child_term, &q_parent_term, "term_dom"));
      EXPECT_TRUE(env.terminate(&q_parent_term));
      EXPECT_EQ(1, q_parent_term.term_calls.load());
      EXPECT_EQ(1, q_child_term.term_calls.load());

      class JoinNotifiedBgQueue final : public kdu_thread_queue {
       public:
        int get_max_jobs() override { return 1; }
        void notify_join(kdu_thread_entity* caller) override {
          join_notes.fetch_add(1);
          all_done(caller);
        }
        std::atomic<int> join_notes{0};
      } q_bg_auto;
      EXPECT_TRUE(env.attach_queue(&q_bg_auto, nullptr, "bg_note", 0,
                                   KDU_THREAD_QUEUE_BACKGROUND));
      EXPECT_TRUE(env.join(nullptr));
      EXPECT_EQ(1, q_bg_auto.join_notes.load());
    }

    EXPECT_TRUE(other_env.destroy());
    EXPECT_TRUE(env.destroy());
  }

  // 2) Domain sequence chain traversal, remove_unused_sequences(),
  // free_sequences reuse, KDU_THREAD_QUEUE_PRIORITY_HIGH / URGENT ordering,
  // KDU_THREAD_QUEUE_SAFE_CONTEXT with worker execution, >16 active sets
  // reallocation, and KDU_MAX_DOMAINS overflow on attach_queue / add_queue.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread("seqchain"));
    WarmupWorkers(env);

    // Build an active non-terminal seq 0 -> seq 1 chain, attach another seq 0
    // queue while seq 0 is non-terminal, finish seq 0, then advance and
    // reclaim.
    RecordingThreadQueue qs0(1, "qs0");
    RecordingThreadQueue qs1(1, "qs1");
    RecordingThreadQueue qs0b(1, "qs0b");
    EXPECT_TRUE(env.attach_queue(&qs0, nullptr, "seqchain", 0));
    EXPECT_TRUE(env.attach_queue(&qs1, nullptr, "seqchain", 1));
    EXPECT_TRUE(env.attach_queue(&qs0b, nullptr, "seqchain", 0));

    std::atomic<int> completed{0};
    std::atomic<int> r0{1}, r0b{1}, r1{1};
    CounterJob j0, j0b, j1;
    j0.queue = &qs0;
    j0.remaining = &r0;
    j0.completed = &completed;
    j0.set_job_func(IncrementJobFunc);
    j0b.queue = &qs0b;
    j0b.remaining = &r0b;
    j0b.completed = &completed;
    j0b.set_job_func(IncrementJobFunc);
    j1.queue = &qs1;
    j1.remaining = &r1;
    j1.completed = &completed;
    j1.set_job_func(IncrementJobFunc);

    qs0.ScheduleOne(&j0, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    qs0b.ScheduleOne(&j0b, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&qs0));
    EXPECT_TRUE(env.join(&qs0b));
    qs1.ScheduleOne(&j1, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&qs1));
    env.advance_work_domains();

    RecordingThreadQueue qs2(1, "qs2");
    RecordingThreadQueue qs3(1, "qs3");
    EXPECT_TRUE(env.attach_queue(&qs2, nullptr, "seqchain", 2));
    std::atomic<int> r2{1}, r3{1};
    CounterJob j2, j3;
    j2.queue = &qs2;
    j2.remaining = &r2;
    j2.completed = &completed;
    j2.set_job_func(IncrementJobFunc);
    qs2.ScheduleOne(&j2, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&qs2));

    EXPECT_TRUE(env.attach_queue(&qs3, nullptr, "seqchain", 3));
    j3.queue = &qs3;
    j3.remaining = &r3;
    j3.completed = &completed;
    j3.set_job_func(IncrementJobFunc);
    qs3.ScheduleOne(&j3, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&qs3));
    EXPECT_EQ(5, completed.load());

    // Priority delta_idx < 0 and delta_idx == 0 insertions in
    // update_active_sets, plus KDU_THREAD_QUEUE_SAFE_CONTEXT executed by
    // workers.
    RecordingThreadQueue q_pnorm(1, "pnorm");
    RecordingThreadQueue q_purg(1, "purg");
    RecordingThreadQueue q_phi(1, "phi");
    RecordingThreadQueue q_safe(1, "psafe");
    EXPECT_TRUE(env.attach_queue(&q_pnorm, nullptr, "prio_norm", 5, 0));
    EXPECT_TRUE(env.attach_queue(&q_purg, nullptr, "prio_urg", 0,
                                 KDU_THREAD_QUEUE_PRIORITY_URGENT));
    EXPECT_TRUE(env.attach_queue(&q_phi, nullptr, "prio_hi", 6,
                                 KDU_THREAD_QUEUE_PRIORITY_HIGH));
    EXPECT_TRUE(env.attach_queue(&q_safe, nullptr, "prio_safe", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    CounterJob jp[4];
    std::atomic<int> rp[4] = {{1}, {1}, {1}, {1}};
    RecordingThreadQueue* pq[4] = {&q_pnorm, &q_purg, &q_phi, &q_safe};
    for (int i = 0; i < 4; ++i) {
      jp[i].queue = pq[i];
      jp[i].remaining = &rp[i];
      jp[i].completed = &completed;
      jp[i].set_job_func(IncrementJobFunc);
      pq[i]->ScheduleOne(&jp[i], &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    EXPECT_TRUE(env.join(nullptr));
    EXPECT_EQ(9, completed.load());
    EXPECT_TRUE(env.destroy());
  }

  // Dedicated entity to trigger KDU_MAX_DOMAINS overflow on attach_queue().
  {
    ScopedThreadEntity lim_scoped;
    kdu_thread_entity& env = lim_scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread());

    std::vector<std::unique_ptr<RecordingThreadQueue>> qs;
    bool hit_limit = false;
    for (int i = 0; i < KDU_MAX_DOMAINS + 4; ++i) {
      auto q = std::make_unique<RecordingThreadQueue>(1, "dom_lim");
      std::string name = "dom_" + std::to_string(i);
      if (!env.attach_queue(q.get(), nullptr, name.c_str())) {
        hit_limit = true;
        break;
      }
      qs.push_back(std::move(q));
    }
    EXPECT_TRUE(hit_limit);
    EXPECT_EQ(KDU_MAX_DOMAINS, static_cast<int>(qs.size()));
    RecordingThreadQueue q_overflow(1, "dom_overflow");
    EXPECT_FALSE(env.attach_queue(&q_overflow, nullptr, "dom_overflow"));

    for (auto& q : qs) {
      q->force_detach(&env);
    }
    EXPECT_TRUE(env.destroy());
  }
}

TEST_F(KduThreadsTest, Final81PlusLinesCoverage) {
  struct FnJob final : public kdu_thread_job {
    FnJob() { set_job_func(&Run); }
    explicit FnJob(std::function<void(kdu_thread_entity*)> fn)
        : fn_(std::move(fn)) {
      set_job_func(&Run);
    }
    void Set(std::function<void(kdu_thread_entity*)> fn) {
      fn_ = std::move(fn);
    }

   private:
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<FnJob*>(base);
      if (self->fn_) {
        self->fn_(caller);
      }
    }
    std::function<void(kdu_thread_entity*)> fn_;
  };

  // 1. Affinity bundle clamping and no-op/implicit destruction paths.
  EXPECT_GE(kdu_thread_entity_affinity::find_preferred_bundle_count(
                kdu_get_num_processors() + 1),
            0);
  EXPECT_GE(kdu_thread_entity_affinity::find_preferred_bundle_count(64), 0);
  {
    kdu_thread_entity_affinity aff;
    EXPECT_TRUE(aff.add_preferred_bundle(0, 0));
    EXPECT_TRUE(aff.add_preferred_bundle(0, kdu_get_num_processors() + 1));
    EXPECT_TRUE(aff.add_preferred_bundle(0, 64));
    EXPECT_TRUE(aff.add_preferred_bundle(0, 100));
  }
  {
    kdu_thread_entity uncreated;
    uncreated.handle_exception(KDU_NULL_EXCEPTION);
  }
  {
    kdu_thread_entity implicit_destroy;
    implicit_destroy.create();
    EXPECT_TRUE(implicit_destroy.exists());
  }

  // 2. Empty entity wait_for_condition (num_active_sets == 0), rapid foreign
  // signals hitting enter_idle race path, and multi-context linked list.
  {
    RecordingThreadContext ctx1(8);
    RecordingThreadContext ctx2(8);
    RecordingThreadContext ctx3(8);
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());

    for (int iter = 0; iter < 40; ++iter) {
      std::mutex mu;
      std::condition_variable cv;
      bool ready = false;
      kdu_thread_entity_condition* cond = env.get_condition();
      std::thread foreign([&]() {
        {
          std::unique_lock<std::mutex> lk(mu);
          cv.wait(lk, [&] { return ready; });
        }
        if ((iter % 3) == 1) {
          std::this_thread::sleep_for(std::chrono::microseconds(5));
        } else if ((iter % 3) == 2) {
          std::this_thread::sleep_for(std::chrono::microseconds(25));
        }
        env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      {
        std::lock_guard<std::mutex> lk(mu);
        ready = true;
        cv.notify_all();
      }
      env.wait_for_condition("empty_group_wait",
                             /*expect_foreign_wakeup=*/true);
      foreign.join();
    }

    ctx1.enter_group(&env);
    ctx2.enter_group(&env);
    ctx3.enter_group(&env);
    // Group contexts list is ctx3 -> ctx2 -> ctx1; detach middle node ctx2,
    // leaving ctx3 and ctx1 attached for destroy() auto-cleanup.
    ctx2.leave_group(&env);
    EXPECT_TRUE(env.destroy());
  }

  // 3. KDU_MAX_DOMAINS rejection in add_thread().
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    std::vector<std::unique_ptr<RecordingThreadQueue>> qs;
    for (int i = 0; i < KDU_MAX_DOMAINS; ++i) {
      auto q = std::make_unique<RecordingThreadQueue>(1, "dom_fill");
      std::string name = "dom_" + std::to_string(i);
      ASSERT_TRUE(env.attach_queue(q.get(), nullptr, name.c_str()));
      qs.push_back(std::move(q));
    }
    EXPECT_FALSE(env.add_thread("dom_overflow_rejected"));
    for (auto& q : qs) {
      q->force_detach(&env);
    }
    EXPECT_TRUE(env.destroy());
  }

  // 4. Domain sequence lifecycle: idle tail termination, non-tail
  // all_done/all_scheduled/note_all_scheduled termination, non-head terminator
  // skipped in dequeue_{general,domain_specific}_job, remove_unused_sequences,
  // free_sequences reuse, and child min_sequencing_idx clamping.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    // 4a. Idle tail termination on sequence transition (old_active_state == 1).
    RecordingThreadQueue q_idle_1(1, "seq_idle1");
    RecordingThreadQueue q_idle_2(1, "seq_idle2");
    ASSERT_TRUE(env.attach_queue(&q_idle_1, nullptr, "domseq", 1));
    q_idle_1.Complete(&env);
    EXPECT_TRUE(env.join(&q_idle_1));
    ASSERT_TRUE(env.attach_queue(&q_idle_2, nullptr, "domseq", 2));
    q_idle_2.Complete(&env);
    EXPECT_TRUE(env.join(&q_idle_2));

    // 4b. Non-head sequence terminator (`!(seq_flags & 2)`) + non-tail
    // note_all_scheduled / schedule_jobs / all_done + remove_unused_sequences
    // + free_sequences reuse + super_queue sequence_idx clamp.
    RecordingThreadQueue q_s1(1, "s1");
    RecordingThreadQueue q_s2(1, "s2");
    RecordingThreadQueue q_s3(1, "s3");
    RecordingThreadQueue q_s4(1, "s4");
    ASSERT_TRUE(env.attach_queue(&q_s1, nullptr, "domseq", 3));
    ASSERT_TRUE(env.attach_queue(&q_s2, nullptr, "domseq", 4));
    ASSERT_TRUE(env.attach_queue(&q_s3, nullptr, "domseq", 5));
    ASSERT_TRUE(env.attach_queue(&q_s4, nullptr, "domseq", 6));

    std::atomic<int> s3_done{0};
    std::mutex s3_mu;
    std::condition_variable s3_cv;
    bool s3_finished = false;
    FnJob j_s3([&](kdu_thread_entity*) {
      s3_done.fetch_add(1);
      std::lock_guard<std::mutex> lk(s3_mu);
      s3_finished = true;
      s3_cv.notify_all();
    });
    kdu_thread_job* s3_ptr = &j_s3;

    std::mutex s4_mu;
    std::condition_variable s4_cv;
    bool s4_done = false;
    FnJob j_s4([&](kdu_thread_entity*) {
      std::lock_guard<std::mutex> lk(s4_mu);
      s4_done = true;
      s4_cv.notify_all();
    });
    kdu_thread_job* s4_ptr = &j_s4;

    q_s3.BindRange(&s3_ptr, 1, 0);
    q_s3.ScheduleBatch(&s3_ptr, 1, &env, /*all_scheduled=*/true);
    {
      std::unique_lock<std::mutex> lk(s3_mu);
      s3_cv.wait(lk, [&] { return s3_finished; });
    }
    q_s4.BindRange(&s4_ptr, 1, 0);
    q_s4.ScheduleBatch(&s4_ptr, 1, &env, /*all_scheduled=*/true);
    {
      std::unique_lock<std::mutex> lk(s4_mu);
      s4_cv.wait(lk, [&] { return s4_done; });
    }
    q_s3.Complete(&env);
    EXPECT_TRUE(env.join(&q_s3));
    q_s4.Complete(&env);
    EXPECT_TRUE(env.join(&q_s4));

    // Terminate s1 (seq3) via NoteAllScheduled while retaining num_queues==1.
    q_s1.NoteAllScheduled(&env);

    // Finish s2 (seq4) via ScheduleBatch(..., all_scheduled=true) on non-tail.
    RecordingThreadQueue q_s5(1, "s5");
    ASSERT_TRUE(env.attach_queue(&q_s5, nullptr, "domseq", 7));
    std::atomic<int> s2_rem{2};
    FnJob j_s2([&](kdu_thread_entity* caller) {
      ReleaseGate(&s2_rem, &q_s2, caller);
    });
    kdu_thread_job* s2_ptr = &j_s2;
    q_s2.BindRange(&s2_ptr, 1, 0);
    q_s2.ScheduleBatch(&s2_ptr, 1, &env, /*all_scheduled=*/true);
    ReleaseGate(&s2_rem, &q_s2, &env);
    EXPECT_TRUE(env.join(&q_s2));

    // Workers and owner have now advanced off s1 (num_consumers == 0);
    // completing s1 sets num_queues = 0 so attaching q_s6 triggers
    // remove_unused_sequences().
    q_s1.Complete(&env);
    EXPECT_TRUE(env.join(&q_s1));

    RecordingThreadQueue q_s6(1, "s6");
    RecordingThreadQueue q_s7(1, "s7");
    ASSERT_TRUE(env.attach_queue(&q_s6, nullptr, "domseq", 8));
    ASSERT_TRUE(env.attach_queue(&q_s7, nullptr, "domseq", 9));

    // Test child queue inheriting larger super_queue sequence_idx (2991) and
    // non-tail all_done() terminating seq (1851).
    RecordingThreadQueue q_sub(1, "s_sub");
    ASSERT_TRUE(env.attach_queue(&q_sub, &q_s7, "domseq", 1));

    q_sub.Complete(&env);
    q_s6.Complete(&env);  // Non-tail all_done without all_scheduled (1851).
    q_s5.Complete(&env);
    q_s7.Complete(&env);
    EXPECT_TRUE(env.join(&q_sub));
    EXPECT_TRUE(env.join(&q_s5));
    EXPECT_TRUE(env.join(&q_s6));
    EXPECT_TRUE(env.join(&q_s7));
    EXPECT_EQ(1, s3_done.load());
    EXPECT_TRUE(env.destroy());
  }

  // 4b. Deadlock report nested > 8 levels deep clamps indent_level (`2360`).
  {
    ScopedThreadEntity scoped;
    scoped.entity.create();
    std::vector<std::unique_ptr<RecordingThreadQueue>> chain;
    chain.reserve(10);
    for (int i = 0; i < 10; ++i) {
      chain.push_back(std::make_unique<RecordingThreadQueue>(1, "deep"));
      kdu_thread_queue* parent = (i == 0) ? nullptr : chain[i - 1].get();
      ASSERT_TRUE(scoped.entity.attach_queue(chain.back().get(), parent, "dl"));
    }
    kdu_exception out_exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(scoped.entity.join(chain.front().get(), false, &out_exc));
    EXPECT_NE(KDU_NULL_EXCEPTION, out_exc);
    std::string err = error_handler_.TakeMessage();
    EXPECT_NE(std::string::npos, err.find("\t\t\t\t\t\t\t\tdeep"));
    EXPECT_FALSE(scoped.entity.destroy());
  }

  // 5. Working-waiter notifications (`2170`, `2175`, `2176`, `2312`) and worker
  // deadlock test exit while owner is waiting for foreign wakeup (`3959`,
  // `3960`).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    // 5a. Foreign-wakeup owner + idle NWW worker (`3959`, `3960`).
    RecordingThreadQueue q_nudge(1, "nudge");
    ASSERT_TRUE(env.attach_queue(&q_nudge, nullptr, "nudge"));
    std::atomic<int> nudge_rem{2};
    FnJob nudge_job([&](kdu_thread_entity* caller) {
      ReleaseGate(&nudge_rem, &q_nudge, caller);
    });
    q_nudge.ScheduleOne(&nudge_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      kdu_thread_entity_condition* owner_cond = env.get_condition();
      std::thread waker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        env.signal_condition(owner_cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      env.wait_for_condition("owner_foreign_check",
                             /*expect_foreign_wakeup=*/true);
      waker.join();
    }
    ReleaseGate(&nudge_rem, &q_nudge, &env);
    EXPECT_TRUE(env.join(&q_nudge));

    // 5b. 1 worker holds q_hold while owner sleeps inside join(&q_hold),
    // then worker schedules 4 normal jobs onto q_batch so ww_notify > 0 wakes
    // the idle working-waiter owner (`2170`).
    RecordingThreadQueue q_hold(1, "hold");
    RecordingThreadQueue q_batch(4, "batch");
    ASSERT_TRUE(env.attach_queue(&q_hold, nullptr, "hold"));
    ASSERT_TRUE(env.attach_queue(&q_batch, nullptr, "batch"));
    std::atomic<int> done{0};
    std::atomic<int> batch_rem{5};
    std::array<FnJob, 4> batch_jobs;
    kdu_thread_job* batch_ptrs[4] = {};
    for (int i = 0; i < 4; ++i) {
      batch_jobs[i].Set([&](kdu_thread_entity* caller) {
        done.fetch_add(1);
        ReleaseGate(&batch_rem, &q_batch, caller);
      });
      batch_ptrs[i] = &batch_jobs[i];
    }
    q_batch.BindRange(batch_ptrs, 4, 0);
    FnJob trig([&](kdu_thread_entity* caller) {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      q_batch.ScheduleBatch(batch_ptrs, 4, caller, /*all_scheduled=*/true);
      q_hold.Complete(caller);
    });
    q_hold.ScheduleOne(&trig, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&q_hold));
    ReleaseGate(&batch_rem, &q_batch, &env);
    EXPECT_TRUE(env.join(&q_batch));
    EXPECT_EQ(4, done.load());
    EXPECT_TRUE(env.destroy());
  }

  // 5c. SAFE_CONTEXT owner wakeups (`2175`, `2176`, `2312`) while
  // nww_alive==false.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);
    EXPECT_FALSE(env.declare_first_owner_wait_safe(true));

    RecordingThreadQueue q_setup(1, "setup");
    RecordingThreadQueue q_trig(1, "trig");
    RecordingThreadQueue q_block(1, "block");
    RecordingThreadQueue q_safe_batch(2, "safe_batch");
    RecordingThreadQueue q_safe_one(1, "safe_one");
    ASSERT_TRUE(env.attach_queue(&q_setup, nullptr, "setup"));
    ASSERT_TRUE(env.attach_queue(&q_trig, nullptr, "trig"));
    ASSERT_TRUE(env.attach_queue(&q_block, nullptr, "block"));
    ASSERT_TRUE(env.attach_queue(&q_safe_batch, nullptr, "safe_b", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    ASSERT_TRUE(env.attach_queue(&q_safe_one, nullptr, "safe_1", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));

    std::mutex mu;
    std::condition_variable cv;
    bool w1_in_wait = false;
    bool trig_started = false;
    kdu_thread_entity_condition* w1_cond = nullptr;
    std::atomic<int> safe_batch_done{0};
    std::atomic<int> safe_one_done{0};

    FnJob j_setup([&](kdu_thread_entity* caller) {
      {
        std::lock_guard<std::mutex> lk(mu);
        w1_cond = caller->get_condition();
        w1_in_wait = true;
        cv.notify_all();
      }
      caller->wait_for_condition("w1_withdrawn",
                                 /*expect_foreign_wakeup=*/true);
      q_setup.Complete(caller);
    });
    q_setup.ScheduleOne(&j_setup, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return w1_in_wait; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::array<FnJob, 2> sb_jobs;
    kdu_thread_job* sb_ptrs[2] = {&sb_jobs[0], &sb_jobs[1]};
    for (int i = 0; i < 2; ++i) {
      sb_jobs[i].Set([&](kdu_thread_entity*) {
        if (safe_batch_done.fetch_add(1) == 1) {
          std::lock_guard<std::mutex> lk(mu);
          cv.notify_all();
        }
      });
    }
    q_safe_batch.BindRange(sb_ptrs, 2, 0);
    FnJob s1_job([&](kdu_thread_entity*) {
      safe_one_done.fetch_add(1);
      std::lock_guard<std::mutex> lk(mu);
      cv.notify_all();
    });

    FnJob j_trig([&](kdu_thread_entity* caller) {
      {
        std::lock_guard<std::mutex> lk(mu);
        trig_started = true;
        cv.notify_all();
      }
      // Give owner time to go WW-idle inside join(&q_block).
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      q_safe_batch.ScheduleBatch(sb_ptrs, 2, caller, true);
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return safe_batch_done.load() == 2; });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      q_safe_one.ScheduleOne(&s1_job, caller, true,
                             KDU_THREAD_JOB_AUTO_BIND_ONCE);
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return safe_one_done.load() == 1; });
      }
      q_trig.Complete(caller);
      q_block.Complete(caller);
    });
    q_trig.ScheduleOne(&j_trig, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return trig_started; });
    }
    EXPECT_TRUE(env.join(&q_block));
    env.signal_condition(w1_cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    q_safe_batch.Complete(&env);
    q_safe_one.Complete(&env);
    EXPECT_TRUE(env.join(&q_trig));
    EXPECT_TRUE(env.join(&q_safe_batch));
    EXPECT_TRUE(env.join(&q_safe_one));
    EXPECT_TRUE(env.join(&q_setup));
    EXPECT_EQ(2, safe_batch_done.load());
    EXPECT_EQ(1, safe_one_done.load());
    EXPECT_TRUE(env.destroy());
  }

  // 5d. Owner with osafe==false waiting alongside SAFE_CONTEXT jobs and
  // idle workers exercises ww_return_job() (`4146`) without deadlock.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    for (int iter = 0; iter < 60; ++iter) {
      RecordingThreadQueue q_safe(1, "safe_wwret");
      RecordingThreadQueue q_spin(1, "spin_wwret");
      ASSERT_TRUE(env.attach_queue(&q_safe, nullptr, "safe_r", 0,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      ASSERT_TRUE(env.attach_queue(&q_spin, nullptr, "spin_r", 0));

      std::atomic<int> spin_rem{2};
      std::atomic<int> safe_rem{2};
      FnJob spin_job([&](kdu_thread_entity* caller) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(30 + (iter & 7) * 15));
        ReleaseGate(&spin_rem, &q_spin, caller);
      });
      FnJob safe_job([&](kdu_thread_entity* caller) {
        ReleaseGate(&safe_rem, &q_safe, caller);
      });
      q_spin.ScheduleOne(&spin_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
      q_safe.ScheduleOne(&safe_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);

      kdu_thread_entity_condition* cond = env.get_condition();
      std::thread waker([&]() {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      env.wait_for_condition("ww_ret_probe", /*expect_foreign_wakeup=*/true);
      waker.join();
      ReleaseGate(&spin_rem, &q_spin, &env);
      ReleaseGate(&safe_rem, &q_safe, &env);
      EXPECT_TRUE(env.join(&q_spin));
      EXPECT_TRUE(env.join(&q_safe));
    }
    EXPECT_TRUE(env.destroy());
  }

  // 6. Chained parent/child waiters (`1906`) and dual simultaneous join() on
  // one queue (`3559`).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q_parent(1, "parent");
    RecordingThreadQueue q_child(1, "child");
    RecordingThreadQueue q_helper1(1, "helper1");
    RecordingThreadQueue q_helper2(1, "helper2");
    ASSERT_TRUE(env.attach_queue(&q_parent, nullptr, "p"));
    ASSERT_TRUE(env.attach_queue(&q_child, &q_parent, "c"));
    ASSERT_TRUE(env.attach_queue(&q_helper1, nullptr, "h1"));
    ASSERT_TRUE(env.attach_queue(&q_helper2, nullptr, "h2"));
    q_parent.Complete(&env);  // Wait stays open until q_child finishes.

    std::mutex mu;
    std::condition_variable cv;
    bool w1_joining_parent = false;
    bool owner_entering_child = false;
    std::atomic<int> h1_rem{2};
    std::atomic<int> h2_rem{2};

    FnJob j_w1([&](kdu_thread_entity* caller) {
      {
        std::lock_guard<std::mutex> lk(mu);
        w1_joining_parent = true;
        cv.notify_all();
      }
      caller->join(&q_parent);
      ReleaseGate(&h1_rem, &q_helper1, caller);
    });
    FnJob j_w2([&](kdu_thread_entity* caller) {
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return owner_entering_child; });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      q_child.Complete(caller);
      ReleaseGate(&h2_rem, &q_helper2, caller);
    });
    q_helper1.ScheduleOne(&j_w1, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return w1_joining_parent; });
    }
    q_helper2.ScheduleOne(&j_w2, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::lock_guard<std::mutex> lk(mu);
      owner_entering_child = true;
      cv.notify_all();
    }
    EXPECT_TRUE(env.join(&q_child));
    ReleaseGate(&h1_rem, &q_helper1, &env);
    ReleaseGate(&h2_rem, &q_helper2, &env);
    EXPECT_TRUE(env.join(&q_helper1));
    EXPECT_TRUE(env.join(&q_helper2));

    // Dual join() on the same shared queue (`3559`).
    RecordingThreadQueue q_shared(1, "shared");
    RecordingThreadQueue q_d1(1, "d1");
    RecordingThreadQueue q_d2(1, "d2");
    ASSERT_TRUE(env.attach_queue(&q_shared, nullptr, "sh"));
    ASSERT_TRUE(env.attach_queue(&q_d1, nullptr, "d1"));
    ASSERT_TRUE(env.attach_queue(&q_d2, nullptr, "d2"));

    bool w1_joining_shared = false;
    bool owner_joining_shared = false;
    std::atomic<int> d1_rem{2};
    std::atomic<int> d2_rem{2};
    FnJob j_d1([&](kdu_thread_entity* caller) {
      {
        std::lock_guard<std::mutex> lk(mu);
        w1_joining_shared = true;
        cv.notify_all();
      }
      caller->join(&q_shared);
      ReleaseGate(&d1_rem, &q_d1, caller);
    });
    FnJob j_d2([&](kdu_thread_entity* caller) {
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return owner_joining_shared; });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      q_shared.Complete(caller);
      ReleaseGate(&d2_rem, &q_d2, caller);
    });
    q_d1.ScheduleOne(&j_d1, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return w1_joining_shared; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    q_d2.ScheduleOne(&j_d2, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::lock_guard<std::mutex> lk(mu);
      owner_joining_shared = true;
      cv.notify_all();
    }
    EXPECT_TRUE(env.join(&q_shared));
    ReleaseGate(&d1_rem, &q_d1, &env);
    ReleaseGate(&d2_rem, &q_d2, &env);
    EXPECT_TRUE(env.join(&q_d1));
    EXPECT_TRUE(env.join(&q_d2));
    EXPECT_TRUE(env.destroy());
  }

  // 7. Throwing request_termination() (`3402`-`3405`), suspended worker woken
  // by handle_exception() (`3664`), and post-error API rejections (`2089`,
  // `2200`, `2764`, `2765`, `2981`-`2983`, `3627`).
  {
    struct ThrowingTerminateQueue final : public kdu_thread_queue {
      int get_max_jobs() override { return 1; }
      void request_termination(kdu_thread_entity*) override {
        throw KDU_ERROR_EXCEPTION;
      }
    };
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ThrowingTerminateQueue term_q;
    ASSERT_TRUE(env.attach_queue(&term_q, nullptr, "term_throw"));
    EXPECT_THROW(env.terminate(&term_q), kdu_exception);
    term_q.force_detach(&env);
    EXPECT_TRUE(env.destroy());
  }
  {
    // Worker suspended inside suspend(false) woken by another worker's error.
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q_susp(1, "susp");
    RecordingThreadQueue q_fail(1, "fail");
    ASSERT_TRUE(env.attach_queue(&q_susp, nullptr, "s"));
    ASSERT_TRUE(env.attach_queue(&q_fail, nullptr, "f"));

    std::mutex mu;
    std::condition_variable cv;
    bool w1_about_to_suspend = false;
    bool w1_finished = false;
    std::atomic<bool> w1_returned_false{false};
    std::atomic<bool> throw_ran{false};

    FnJob j_susp([&](kdu_thread_entity* caller) {
      {
        std::lock_guard<std::mutex> lk(mu);
        w1_about_to_suspend = true;
        cv.notify_all();
      }
      w1_returned_false.store(!caller->suspend(/*allow_exceptions=*/false));
      {
        std::lock_guard<std::mutex> lk(mu);
        w1_finished = true;
        cv.notify_all();
      }
    });
    FnJob j_fail([&](kdu_thread_entity*) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      throw_ran.store(true);
      throw KDU_ERROR_EXCEPTION;
    });
    q_susp.ScheduleOne(&j_susp, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return w1_about_to_suspend; });
    }
    q_fail.ScheduleOne(&j_fail, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return w1_finished; });
    }
    EXPECT_FALSE(env.destroy());
    EXPECT_TRUE(throw_ran.load());
    EXPECT_TRUE(w1_returned_false.load());
  }
  {
    // Zero-worker error state exercises grouperr paths cleanly without EJD
    // hang.
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    RecordingThreadQueue q_pre(2, "pre");
    RecordingThreadQueue q_err(1, "err");
    RecordingThreadQueue q_after(1, "after");
    ASSERT_TRUE(env.attach_queue(&q_pre, nullptr, "pre"));
    ASSERT_TRUE(env.attach_queue(&q_err, nullptr, "err"));

    FnJob bad_job([](kdu_thread_entity*) { throw KDU_ERROR_EXCEPTION; });
    q_err.ScheduleOne(&bad_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_FALSE(env.join(&q_err));

    env.unsuspend_thread(&env);
    EXPECT_FALSE(env.add_thread());
    EXPECT_THROW(env.attach_queue(&q_after, nullptr, "after"), kdu_exception);

    FnJob dummy_job([](kdu_thread_entity*) {});
    kdu_thread_job* dummy_ptr = &dummy_job;
    EXPECT_THROW(q_pre.ScheduleBatch(&dummy_ptr, 1, &env, false),
                 kdu_exception);
    EXPECT_THROW(q_pre.ScheduleOne(&dummy_job, &env, false,
                                   KDU_THREAD_JOB_AUTO_BIND_ONCE),
                 kdu_exception);
    EXPECT_FALSE(env.destroy());
  }

  // 8. Palette cleaning with out-of-order dirty head (`1599`-`1601`) and
  // high-churn concurrent palette/wake/yield pressure (`1448`, `1453`, `1639`,
  // `1653`-`1654`, `1661`-`1663`, `4009`).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    for (int t = 0; t < 4; ++t) {
      ASSERT_TRUE(env.add_thread());
    }
    env.set_yield_frequency(1);
    WarmupWorkers(env);

    // 8a. Pin `P_first` as dirty on `"dom_pin"` by leaving `q_keep_seq`
    // uncompleted on the same domain sequence after `q_first` joins, then
    // exhaust clean palettes so `P_first` enters `owner->palette_heap` ahead
    // of > 64 dequeued palettes (`1599`-`1601`).
    RecordingThreadQueue q_keep_seq(1, "keep_seq");
    RecordingThreadQueue q_first(1, "first_pin");
    ASSERT_TRUE(env.attach_queue(&q_keep_seq, nullptr, "dom_pin", 1));
    ASSERT_TRUE(env.attach_queue(&q_first, nullptr, "dom_pin", 1));
    std::atomic<int> pin_ran{0};
    std::atomic<int> first_rem{2};
    FnJob j_first([&](kdu_thread_entity* caller) {
      pin_ran.fetch_add(1);
      ReleaseGate(&first_rem, &q_first, caller);
    });
    q_first.ScheduleOne(&j_first, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&first_rem, &q_first, &env);
    EXPECT_TRUE(env.join(&q_first));

    // Pump 600 single-job queues through `"dom_work"` to exhaust the initial
    // 256-palette block and recycle every returned palette (including dirty
    // `P_first`) across multiple `KD_THREAD_PALETTES` (64) sweeps.
    std::atomic<int> work_done{0};
    constexpr int kPinSweepJobs = 600;
    for (int i = 0; i < kPinSweepJobs; ++i) {
      RecordingThreadQueue q_item(1, "pin_sweep");
      ASSERT_TRUE(env.attach_queue(&q_item, nullptr, "dom_work"));
      std::atomic<int> item_rem{2};
      FnJob item_job([&](kdu_thread_entity* caller) {
        work_done.fetch_add(1);
        ReleaseGate(&item_rem, &q_item, caller);
      });
      q_item.ScheduleOne(&item_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
      ReleaseGate(&item_rem, &q_item, &env);
      EXPECT_TRUE(env.join(&q_item));
    }
    EXPECT_EQ(1, pin_ran.load());
    EXPECT_EQ(kPinSweepJobs, work_done.load());

    q_keep_seq.Complete(&env);
    EXPECT_TRUE(env.join(&q_keep_seq));

    // 8b. High-churn concurrent queues so workers actively hold hazard
    // pointers in `thread_job_hzps[t]` during owner `clean_dirty_palettes()`,
    // plus empty-stack wakeup bursts (`1448`, `1453`, `1639`, `1653`-`1654`,
    // `1661`-`1663`, `4009`).
    for (int wave = 0; wave < 4; ++wave) {
      constexpr int kWaveJobs = 100;
      RecordingThreadQueue q_wave(kWaveJobs, "hzp_wave");
      ASSERT_TRUE(env.attach_queue(&q_wave, nullptr, "dom_hzp"));
      std::vector<FnJob> wave_jobs(kWaveJobs);
      std::atomic<int> wave_done{0};
      std::atomic<int> wave_rem{kWaveJobs + 1};
      for (int i = 0; i < kWaveJobs; ++i) {
        wave_jobs[i].Set([&](kdu_thread_entity* caller) {
          wave_done.fetch_add(1);
          ReleaseGate(&wave_rem, &q_wave, caller);
        });
        q_wave.ScheduleOne(&wave_jobs[i], &env, (i + 1 == kWaveJobs),
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      ReleaseGate(&wave_rem, &q_wave, &env);
      EXPECT_TRUE(env.join(&q_wave));
      EXPECT_EQ(kWaveJobs, wave_done.load());
    }

    for (int r = 0; r < 120; ++r) {
      RecordingThreadQueue q_pulse(1, "pulse");
      ASSERT_TRUE(env.attach_queue(&q_pulse, nullptr, "pulse"));
      std::atomic<int> pulse_rem{2};
      FnJob pjob([&](kdu_thread_entity* caller) {
        ReleaseGate(&pulse_rem, &q_pulse, caller);
      });
      q_pulse.ScheduleOne(&pjob, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
      ReleaseGate(&pulse_rem, &q_pulse, &env);
      EXPECT_TRUE(env.join(&q_pulse));
    }
    EXPECT_TRUE(env.destroy());
  }
}

struct FnJob final : public kdu_thread_job {
  explicit FnJob(std::function<void(kdu_thread_entity*)> f) : fn(std::move(f)) {
    set_job_func([](kdu_thread_job* base, kdu_thread_entity* caller) {
      static_cast<FnJob*>(base)->fn(caller);
    });
  }
  std::function<void(kdu_thread_entity*)> fn;
};

TEST_F(KduThreadsTest, Remaining90PercentLines) {
  // 1. Zero-worker remove_unused_sequences across multiple domains/sequences.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    RecordingThreadQueue q1(1, "rm_q1");
    RecordingThreadQueue q2(1, "rm_q2");
    RecordingThreadQueue q3(1, "rm_q3");
    ASSERT_TRUE(
        env.attach_queue(&q1, nullptr, "dom_rm", /*min_sequencing_idx=*/1));
    q1.NoteAllScheduled(&env);
    ASSERT_TRUE(
        env.attach_queue(&q2, nullptr, "dom_rm", /*min_sequencing_idx=*/2));
    env.advance_work_domains();
    q1.Complete(&env);
    ASSERT_TRUE(
        env.attach_queue(&q3, nullptr, "dom_rm", /*min_sequencing_idx=*/3));
    EXPECT_TRUE(env.join(&q1));
    q2.NoteAllScheduled(&env);
    q2.Complete(&env);
    EXPECT_TRUE(env.join(&q2));
    q3.NoteAllScheduled(&env);
    q3.Complete(&env);
    EXPECT_TRUE(env.join(&q3));
    EXPECT_TRUE(env.destroy());
  }

  // 2. Mixed clean + hazard palette initialization on add_thread().
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    RecordingThreadQueue q1(10, "haz_prep");
    ASSERT_TRUE(env.attach_queue(&q1, nullptr, "dom_haz"));
    std::array<CounterJob, 10> jobs{};
    std::array<kdu_thread_job*, 10> ptrs{};
    std::atomic<int> completed{0};
    std::atomic<int> rem{11};
    for (int i = 0; i < 10; ++i) {
      jobs[i].set_job_func(IncrementJobFunc);
      jobs[i].completed = &completed;
      jobs[i].remaining = &rem;
      jobs[i].queue = &q1;
      ptrs[i] = &jobs[i];
    }
    q1.BindRange(ptrs.data(), 10, 0);
    q1.ScheduleBatch(ptrs.data(), 10, &env, /*all_scheduled=*/true);
    ReleaseGate(&rem, &q1, &env);
    EXPECT_TRUE(env.join(&q1));
    EXPECT_EQ(10, completed.load());

    RecordingThreadQueue q2(170, "haz_fill");
    ASSERT_TRUE(env.attach_queue(&q2, nullptr, "dom_haz"));
    EXPECT_TRUE(env.add_thread());
    q2.NoteAllScheduled(&env);
    q2.Complete(&env);
    EXPECT_TRUE(env.join(&q2));
    EXPECT_TRUE(env.destroy());
  }

  // 3. Two idle workers with zero queues hits num_active_sets == 0.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    ASSERT_TRUE(env.add_thread());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(env.destroy());
  }

  // 4. ext_wakers == true in ww_process_jobs() (0 workers) and process_jobs().
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    kdu_thread_entity_condition* cond = env.get_condition();
    ASSERT_NE(nullptr, cond);
    std::thread waker([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    env.wait_for_condition("ext_0w", /*expect_foreign_wakeup=*/true);
    waker.join();
    EXPECT_TRUE(env.destroy());
  }
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);
    RecordingThreadQueue q(1, "ext_1w");
    ASSERT_TRUE(env.attach_queue(&q, nullptr, "dom_ext1"));
    std::mutex mu;
    std::condition_variable cv;
    bool worker_started = false;
    std::atomic<int> rem{2};
    FnJob sleep_job([&](kdu_thread_entity* caller) {
      {
        std::lock_guard<std::mutex> lk(mu);
        worker_started = true;
      }
      cv.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      ReleaseGate(&rem, &q, caller);
    });
    q.ScheduleOne(&sleep_job, &env, /*all_scheduled=*/true,
                  KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return worker_started; });
    }
    kdu_thread_entity_condition* cond = env.get_condition();
    ASSERT_NE(nullptr, cond);
    std::thread waker([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(45));
      env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    env.wait_for_condition("ext_1w", /*expect_foreign_wakeup=*/true);
    waker.join();
    ReleaseGate(&rem, &q, &env);
    EXPECT_TRUE(env.join(&q));
    EXPECT_TRUE(env.destroy());
  }

  // 5. Exception thrown by worker while owner sits in wait_for_condition().
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);
    RecordingThreadQueue q(1, "exc_wfc");
    ASSERT_TRUE(env.attach_queue(&q, nullptr, "dom_exc"));
    std::mutex mu;
    std::condition_variable cv;
    bool worker_started = false;
    bool owner_done = false;
    FnJob exc_job([&](kdu_thread_entity*) {
      {
        std::lock_guard<std::mutex> lk(mu);
        worker_started = true;
      }
      cv.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      throw KDU_ERROR_EXCEPTION;
    });
    q.ScheduleOne(&exc_job, &env, /*all_scheduled=*/true,
                  KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return worker_started; });
    }
    kdu_thread_entity_condition* cond = env.get_condition();
    std::thread backup_waker([&] {
      std::unique_lock<std::mutex> lk(mu);
      if (!cv.wait_for(lk, std::chrono::milliseconds(150),
                       [&] { return owner_done; })) {
        env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      }
    });
    EXPECT_THROW(
        env.wait_for_condition("exc_wfc", /*expect_foreign_wakeup=*/true),
        kdu_exception);
    {
      std::lock_guard<std::mutex> lk(mu);
      owner_done = true;
    }
    cv.notify_all();
    backup_waker.join();
    EXPECT_FALSE(env.join(&q));
    env.destroy();
  }

  // 6. Non-head sequence terminator skipped in dequeue_general_job().
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q_susp(1, "susp_q");
    ASSERT_TRUE(env.attach_queue(&q_susp, nullptr, "dom_susp", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    std::mutex mu;
    std::condition_variable cv;
    kdu_thread_entity* susp_thread = nullptr;
    bool susp_done = false;
    bool term_done = false;
    std::atomic<int> susp_rem{2};
    FnJob susp_job([&](kdu_thread_entity* caller) {
      {
        std::lock_guard<std::mutex> lk(mu);
        susp_thread = caller;
      }
      cv.notify_all();
      caller->suspend(false);
      {
        std::lock_guard<std::mutex> lk(mu);
        susp_done = true;
      }
      cv.notify_all();
      ReleaseGate(&susp_rem, &q_susp, caller);
    });
    q_susp.ScheduleOne(&susp_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return susp_thread != nullptr; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Add two additional workers so at least one remains idle while sweeping.
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    RecordingThreadQueue q_open(1, "open_q");
    RecordingThreadQueue q_term(1, "term_q");
    ASSERT_TRUE(env.attach_queue(&q_open, nullptr, "dom_g", 1,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    ASSERT_TRUE(env.attach_queue(&q_term, nullptr, "dom_g", 2,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    std::atomic<int> term_rem{2};
    FnJob term_job([&](kdu_thread_entity* caller) {
      ReleaseGate(&term_rem, &q_term, caller);
      {
        std::lock_guard<std::mutex> lk(mu);
        term_done = true;
      }
      cv.notify_all();
    });
    q_term.ScheduleOne(&term_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return term_done; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ReleaseGate(&term_rem, &q_term, &env);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    RecordingThreadQueue q_poke(1, "poke_q");
    ASSERT_TRUE(env.attach_queue(&q_poke, nullptr, "dom_z", 10,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    std::atomic<int> poke_rem{2};
    FnJob poke_job([&](kdu_thread_entity* caller) {
      ReleaseGate(&poke_rem, &q_poke, caller);
    });
    q_poke.ScheduleOne(&poke_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&poke_rem, &q_poke, &env);
    EXPECT_TRUE(env.join(&q_poke));

    q_open.NoteAllScheduled(&env);
    q_open.Complete(&env);
    EXPECT_TRUE(env.join(&q_open));
    EXPECT_TRUE(env.join(&q_term));
    env.unsuspend_thread(susp_thread);
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return susp_done; });
    }
    ReleaseGate(&susp_rem, &q_susp, &env);
    EXPECT_TRUE(env.join(&q_susp));
    EXPECT_TRUE(env.destroy());
  }

  // 7. Concurrent hazard-pointer palette sweeps and yield_frequency = 1.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    for (int w = 0; w < 4; ++w) {
      ASSERT_TRUE(env.add_thread());
    }
    env.set_yield_frequency(1);
    WarmupWorkers(env);

    constexpr int kBatchesPerProducer = 25;
    struct SweepBatch {
      RecordingThreadQueue q{16, "sweep_q"};
      std::array<CounterJob, 16> jobs{};
      std::array<kdu_thread_job*, 16> ptrs{};
      std::atomic<int> done{0};
      std::atomic<int> rem{17};
    };
    auto b0 = std::make_unique<std::array<SweepBatch, kBatchesPerProducer>>();
    auto b1 = std::make_unique<std::array<SweepBatch, kBatchesPerProducer>>();
    auto b2 = std::make_unique<std::array<SweepBatch, kBatchesPerProducer>>();

    auto schedule_batches =
        [&](kdu_thread_entity* caller, const char* domain,
            std::array<SweepBatch, kBatchesPerProducer>& bs) {
          for (int iter = 0; iter < kBatchesPerProducer; ++iter) {
            SweepBatch& b = bs[iter];
            EXPECT_TRUE(caller->attach_queue(&b.q, nullptr, domain, iter + 1,
                                             KDU_THREAD_QUEUE_SAFE_CONTEXT));
            for (int i = 0; i < 16; ++i) {
              b.jobs[i].set_job_func(IncrementJobFunc);
              b.jobs[i].completed = &b.done;
              b.jobs[i].remaining = &b.rem;
              b.jobs[i].queue = &b.q;
              b.ptrs[i] = &b.jobs[i];
            }
            b.q.BindRange(b.ptrs.data(), 16, 0);
            b.q.ScheduleBatch(b.ptrs.data(), 8, caller,
                              /*all_scheduled=*/false);
            b.q.ScheduleBatch(b.ptrs.data() + 8, 8, caller,
                              /*all_scheduled=*/true);
            ReleaseGate(&b.rem, &b.q, caller);
          }
        };

    RecordingThreadQueue q_p1(1, "prod1");
    RecordingThreadQueue q_p2(1, "prod2");
    ASSERT_TRUE(env.attach_queue(&q_p1, nullptr, "dom_p1", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    ASSERT_TRUE(env.attach_queue(&q_p2, nullptr, "dom_p2", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    std::atomic<int> p1_rem{2};
    std::atomic<int> p2_rem{2};
    FnJob p1([&](kdu_thread_entity* caller) {
      schedule_batches(caller, "dom_c1", *b1);
      ReleaseGate(&p1_rem, &q_p1, caller);
    });
    FnJob p2([&](kdu_thread_entity* caller) {
      schedule_batches(caller, "dom_c2", *b2);
      ReleaseGate(&p2_rem, &q_p2, caller);
    });
    q_p1.ScheduleOne(&p1, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    q_p2.ScheduleOne(&p2, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    schedule_batches(&env, "dom_co", *b0);
    ReleaseGate(&p1_rem, &q_p1, &env);
    ReleaseGate(&p2_rem, &q_p2, &env);
    EXPECT_TRUE(env.join(&q_p1));
    EXPECT_TRUE(env.join(&q_p2));
    for (int iter = 0; iter < kBatchesPerProducer; ++iter) {
      EXPECT_TRUE(env.join(&(*b0)[iter].q));
      EXPECT_EQ(16, (*b0)[iter].done.load());
      EXPECT_TRUE(env.join(&(*b1)[iter].q));
      EXPECT_EQ(16, (*b1)[iter].done.load());
      EXPECT_TRUE(env.join(&(*b2)[iter].q));
      EXPECT_EQ(16, (*b2)[iter].done.load());
    }
    EXPECT_TRUE(env.destroy());
  }

  // 8. Tight wait_for_condition / signal_condition & ww_return_job races.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    constexpr int kRounds = 250;
    constexpr int kWorkJobs =
        250;  // 2 jobs scheduled on each of 125 even rounds.
    RecordingThreadQueue q_work(kWorkJobs, "ww_work");
    ASSERT_TRUE(env.attach_queue(&q_work, nullptr, "dom_ww_work", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    std::vector<CounterJob> work_jobs(kWorkJobs);
    std::vector<kdu_thread_job*> work_ptrs(kWorkJobs);
    std::atomic<int> work_completed{0};
    std::atomic<int> work_rem{kWorkJobs + 1};
    for (int i = 0; i < kWorkJobs; ++i) {
      work_jobs[i].set_job_func(IncrementJobFunc);
      work_jobs[i].completed = &work_completed;
      work_jobs[i].remaining = &work_rem;
      work_jobs[i].queue = &q_work;
      work_ptrs[i] = &work_jobs[i];
    }
    q_work.BindRange(work_ptrs.data(), kWorkJobs, 0);

    std::atomic<kdu_thread_entity_condition*> shared_cond{nullptr};
    std::atomic<int> round_ack{0};
    std::thread waker([&] {
      for (int r = 1; r <= kRounds; ++r) {
        kdu_thread_entity_condition* c = nullptr;
        while ((c = shared_cond.load(std::memory_order_acquire)) == nullptr) {
          std::this_thread::yield();
        }
        shared_cond.store(nullptr, std::memory_order_release);
        if ((r % 4) == 1) {
          std::this_thread::sleep_for(std::chrono::microseconds(15));
        } else if ((r % 4) == 2) {
          std::this_thread::yield();
        }
        env.signal_condition(c, KDU_FOREIGN_SIGNAL_EXPECTED);
        round_ack.store(r, std::memory_order_release);
      }
    });

    int scheduled = 0;
    for (int r = 1; r <= kRounds; ++r) {
      if ((r & 1) == 0 && scheduled + 2 <= kWorkJobs) {
        q_work.ScheduleBatch(&work_ptrs[scheduled], 2, &env,
                             /*all_scheduled=*/false);
        scheduled += 2;
      }
      kdu_thread_entity_condition* cond = env.get_condition();
      shared_cond.store(cond, std::memory_order_release);
      env.wait_for_condition("race_wfc", /*expect_foreign_wakeup=*/true);
      while (round_ack.load(std::memory_order_acquire) < r) {
        std::this_thread::yield();
      }
    }
    waker.join();
    EXPECT_EQ(scheduled, kWorkJobs);
    q_work.NoteAllScheduled(&env);
    ReleaseGate(&work_rem, &q_work, &env);
    EXPECT_TRUE(env.join(&q_work));
    EXPECT_EQ(kWorkJobs, work_completed.load());
    EXPECT_TRUE(env.destroy());
  }

  // 7) find_preferred_bundle_count(bundle_threads > num_processors).
  {
    const int num_procs = kdu_get_num_processors();
    const int capped = (num_procs + 1 < 64) ? (num_procs + 1) : 64;
    EXPECT_GE(kdu_thread_entity_affinity::find_preferred_bundle_count(capped),
              0);
    EXPECT_GE(kdu_thread_entity_affinity::find_preferred_bundle_count(64), 0);
  }

  // 8) num_threads_changed() throwing std::bad_alloc during worker launch.
  {
    struct BadAllocOnSecondThreadContext final : public kdu_thread_context {
      std::atomic<bool> handled{false};
      void num_threads_changed(int new_num_threads) override {
        if (new_num_threads >= 2) {
          throw std::bad_alloc();
        }
      }
      void handle_exception(kdu_thread_entity* caller) override {
        handled.store(true, std::memory_order_release);
        kdu_thread_context::handle_exception(caller);
      }
    };
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    BadAllocOnSecondThreadContext ctxt;
    ctxt.enter_group(&env);
    EXPECT_TRUE(env.add_thread());
    while (!ctxt.handled.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    EXPECT_FALSE(env.destroy());
  }

  // 9) Domain sequence lifecycle:
  // - terminate old_tail on new sequence when old_active_state == 1
  // - leave sequence with num_consumers == 0 while queue attached, then detach
  //   and add a higher sequence index so get_active_domain_sequence cleans up
  // - dequeue_general_job skipping a terminated non-head sequence in a domain
  // - dequeue_general_job when num_active_sets == 0
  {
    // Part A: num_active_sets == 0 in dequeue_general_job.
    {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      std::atomic<kdu_thread_entity_condition*> shared_cond{nullptr};
      std::thread waker([&env, &shared_cond] {
        kdu_thread_entity_condition* cond = nullptr;
        while ((cond = shared_cond.load(std::memory_order_acquire)) ==
               nullptr) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      shared_cond.store(env.get_condition(), std::memory_order_release);
      env.wait_for_condition("empty_sets", /*expect_foreign_wakeup=*/true);
      waker.join();
      EXPECT_TRUE(env.destroy());
    }

    // Part B: add_domain_sequence terminating old_tail & cleaning unused seqs.
    {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      EXPECT_TRUE(env.add_thread("d_rem"));
      WarmupWorkers(env);

      RecordingThreadQueue q1(1, "q1_seq");
      EXPECT_TRUE(env.attach_queue(&q1, nullptr, "d_rem",
                                   /*min_sequencing_idx=*/1));
      q1.NoteAllScheduled(&env);

      // old_tail (idx=1) has old_active_state == 1 -> terminates on attach.
      RecordingThreadQueue q2(1, "q2_seq");
      EXPECT_TRUE(env.attach_queue(&q2, nullptr, "d_rem",
                                   /*min_sequencing_idx=*/2));
      std::atomic<int> done2{0};
      FnJob j2([&](kdu_thread_entity* caller) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        done2.fetch_add(1);
        q2.Complete(caller);
      });
      q2.ScheduleOne(&j2, &env, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.join(&q2));
      EXPECT_EQ(1, done2.load());

      // Now seq1 had num_consumers drop to 0 while q1 was still attached.
      // Complete q1 so seq1->num_queues == 0, then add seq3 to trigger cleanup.
      q1.Complete(&env);
      EXPECT_TRUE(env.join(&q1));

      RecordingThreadQueue q3(1, "q3_seq");
      EXPECT_TRUE(env.attach_queue(&q3, nullptr, "d_rem",
                                   /*min_sequencing_idx=*/3));
      q3.NoteAllScheduled(&env);
      q3.Complete(&env);
      EXPECT_TRUE(env.join(&q3));
      EXPECT_TRUE(env.destroy());
    }

    // Part C: second sequence in domain terminated while first remains open.
    {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      EXPECT_TRUE(env.add_thread("other_dom"));
      WarmupWorkers(env);

      RecordingThreadQueue q1(1, "head_seq");
      RecordingThreadQueue q2(1, "mid_term_seq");
      RecordingThreadQueue q3(1, "tail_seq");
      EXPECT_TRUE(env.attach_queue(&q1, nullptr, "d_mid", 1));
      EXPECT_TRUE(env.attach_queue(&q2, nullptr, "d_mid", 2));
      q2.NoteAllScheduled(&env);
      EXPECT_TRUE(env.attach_queue(&q3, nullptr, "d_mid", 3));

      // While q1 keeps seq1 active (so seq2 lacks seq_flags & 2), wait in
      // owner so owner sweeps active_sets in dequeue_general_job and sees q2's
      // KD_JOB_TERMINATOR.
      std::atomic<kdu_thread_entity_condition*> shared_cond{nullptr};
      std::thread waker([&env, &shared_cond] {
        kdu_thread_entity_condition* cond = nullptr;
        while ((cond = shared_cond.load(std::memory_order_acquire)) ==
               nullptr) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      shared_cond.store(env.get_condition(), std::memory_order_release);
      env.wait_for_condition("sweep_mid_seq", /*expect_foreign_wakeup=*/true);
      waker.join();

      q1.NoteAllScheduled(&env);
      q1.Complete(&env);
      q2.Complete(&env);
      q3.NoteAllScheduled(&env);
      q3.Complete(&env);
      EXPECT_TRUE(env.join(nullptr));
      EXPECT_TRUE(env.destroy());
    }
  }

  // 10) Trigger KDU_ERROR_DEV when scheduling after all_scheduled=true.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread());
    RecordingThreadQueue q_done(2, "done_sched");
    EXPECT_TRUE(env.attach_queue(&q_done, nullptr, "done_sched"));
    FnJob j([](kdu_thread_entity*) {});
    kdu_thread_job* p = &j;
    q_done.BindRange(&p, 1, 0);
    q_done.NoteAllScheduled(&env);
    EXPECT_THROW(q_done.ScheduleBatch(&p, 1, &env, /*all_scheduled=*/true),
                 kdu_exception);
    q_done.Complete(&env);
    EXPECT_TRUE(env.join(&q_done));
    EXPECT_TRUE(env.destroy());
  }

  // 11) Waking 1 of >=3 idle workers in nww_idle_list (tail->next != NULL),
  // plus yield_freq=1 wake_another under multi-worker churn.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    env.set_yield_frequency(1);
    for (int i = 0; i < 4; ++i) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Single job wakes 1 worker while >=3 remain on nww_idle_list.
    RecordingThreadQueue q_one(1, "wake_one_of_many");
    EXPECT_TRUE(env.attach_queue(&q_one, nullptr, "wake_one"));
    std::atomic<int> one_done{0};
    FnJob j_one([&](kdu_thread_entity* caller) {
      one_done.fetch_add(1);
      q_one.Complete(caller);
    });
    q_one.ScheduleOne(&j_one, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&q_one));
    EXPECT_EQ(1, one_done.load());

    // Rapid small batches to hit nww_yield wake_another.
    constexpr int kBurstCount = 200;
    for (int b = 0; b < 4; ++b) {
      RecordingThreadQueue q_burst(kBurstCount, "yield_burst");
      EXPECT_TRUE(env.attach_queue(&q_burst, nullptr, "yield_burst"));
      std::atomic<int> rem{kBurstCount + 1};
      std::vector<FnJob> jobs;
      jobs.reserve(kBurstCount);
      std::vector<kdu_thread_job*> ptrs(kBurstCount);
      for (int i = 0; i < kBurstCount; ++i) {
        jobs.emplace_back([&](kdu_thread_entity* caller) {
          ReleaseGate(&rem, &q_burst, caller);
        });
        ptrs[i] = &jobs[i];
      }
      q_burst.BindRange(ptrs.data(), kBurstCount, 0);
      q_burst.ScheduleBatch(ptrs.data(), kBurstCount, &env,
                            /*all_scheduled=*/true);
      ReleaseGate(&rem, &q_burst, &env);
      EXPECT_TRUE(env.join(&q_burst));
    }
    EXPECT_TRUE(env.destroy());
  }

  // 12) clean_dirty_palettes with active hazard pointers on workers.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    for (int i = 0; i < 8; ++i) {
      ASSERT_TRUE(env.add_thread("churn"));
    }

    constexpr int kSlots = 16;
    RecordingThreadQueue q_churn(kSlots, "churn_q");
    RecordingThreadQueue q_noise(4000, "noise_q");
    EXPECT_TRUE(env.attach_queue(&q_churn, nullptr, "churn"));
    EXPECT_TRUE(env.attach_queue(&q_noise, nullptr, "churn_noise"));

    std::atomic<bool> stop_noise{false};
    struct SlotJob final : public kdu_thread_job {
      std::atomic<bool> ready{true};
    };
    std::vector<SlotJob> sjobs(kSlots);
    std::vector<kdu_thread_job*> sptrs(kSlots);
    for (int i = 0; i < kSlots; ++i) {
      sjobs[i].set_job_func([](kdu_thread_job* base, kdu_thread_entity*) {
        static_cast<SlotJob*>(base)->ready.store(true,
                                                 std::memory_order_release);
      });
      sptrs[i] = &sjobs[i];
    }
    q_churn.BindRange(sptrs.data(), kSlots, 0);

    // Keep workers actively spinning inside dequeue / do_job with hazard ptrs.
    std::atomic<int> noise_rem{2001};
    std::vector<FnJob> njobs;
    njobs.reserve(2000);
    std::vector<kdu_thread_job*> nptrs(2000);
    for (int i = 0; i < 2000; ++i) {
      njobs.emplace_back(
          [&stop_noise, &noise_rem, &q_noise](kdu_thread_entity* caller) {
            if (!stop_noise.load(std::memory_order_acquire)) {
              std::this_thread::yield();
            }
            ReleaseGate(&noise_rem, &q_noise, caller);
          });
      nptrs[i] = &njobs[i];
    }
    q_noise.BindRange(nptrs.data(), 2000, 0);
    q_noise.ScheduleBatch(nptrs.data(), 2000, &env, /*all_scheduled=*/false);

    // Producer worker drives 1500 single-job schedules through the palette
    // heap so clean_dirty_palettes runs repeatedly while workers hold hzp->ref.
    RecordingThreadQueue q_prod(1, "prod_q");
    EXPECT_TRUE(env.attach_queue(&q_prod, nullptr, "prod_dom"));
    FnJob producer([&](kdu_thread_entity* caller) {
      for (int iter = 0; iter < 1500; ++iter) {
        SlotJob& sj = sjobs[iter % kSlots];
        while (!sj.ready.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        sj.ready.store(false, std::memory_order_release);
        q_churn.ScheduleOne(&sj, caller, /*all_scheduled=*/false, 0);
      }
      stop_noise.store(true, std::memory_order_release);
      q_prod.Complete(caller);
    });
    q_prod.ScheduleOne(&producer, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&q_prod));
    for (int i = 0; i < kSlots; ++i) {
      while (!sjobs[i].ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
    q_churn.NoteAllScheduled(&env);
    q_churn.Complete(&env);
    q_noise.NoteAllScheduled(&env);
    ReleaseGate(&noise_rem, &q_noise, &env);
    EXPECT_TRUE(env.join(nullptr));
    EXPECT_TRUE(env.destroy());
  }

  // 13) ww_process_jobs breaking on exception in ww_reserve_job /
  // start_hunting.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    EXPECT_TRUE(env.add_thread("exc_dom"));
    EXPECT_TRUE(env.add_thread("exc_dom"));
    WarmupWorkers(env);

    constexpr int kSpinJobs = 1000;
    RecordingThreadQueue q_spin(kSpinJobs, "spin_q");
    RecordingThreadQueue q_boom(1, "boom_q");
    EXPECT_TRUE(env.attach_queue(&q_spin, nullptr, "spin_dom"));
    EXPECT_TRUE(env.attach_queue(&q_boom, nullptr, "boom_dom"));

    std::atomic<int> spin_count{0};
    std::atomic<int> spin_rem{kSpinJobs + 1};
    std::vector<FnJob> sjobs;
    sjobs.reserve(kSpinJobs);
    for (int i = 0; i < kSpinJobs; ++i) {
      sjobs.emplace_back([&](kdu_thread_entity* caller) {
        spin_count.fetch_add(1, std::memory_order_acq_rel);
        ReleaseGate(&spin_rem, &q_spin, caller);
      });
      q_spin.ScheduleOne(&sjobs[i], &env,
                         /*all_scheduled=*/(i == kSpinJobs - 1),
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    FnJob boom([&spin_count](kdu_thread_entity*) {
      while (spin_count.load(std::memory_order_acquire) < 64) {
        std::this_thread::yield();
      }
      throw kdu_exception(KDU_ERROR_EXCEPTION);
    });
    q_boom.ScheduleOne(&boom, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    kdu_exception exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(env.join(&q_boom, false, &exc));
    EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
    EXPECT_FALSE(env.destroy());
  }

  // 14) ww_process_jobs catch(std::bad_alloc&) and catch(...) via acquire_wwid
  // failure with custom throwing kdu_message handlers.
  {
    struct CustomThrowHandler final : public kdu_message {
      enum class Kind { kBadAlloc, kOther };
      explicit CustomThrowHandler(Kind kind) : kind_(kind) {}
      struct NonKduError {};
      void put_text(const char*) override {}
      void flush(bool end_of_message = false) override {
        if (!end_of_message) return;
        if (kind_ == Kind::kBadAlloc) {
          throw std::bad_alloc();
        }
        throw NonKduError{};
      }
      Kind kind_;
    };
    struct ScopedHandler {
      CustomThrowHandler custom;
      kdu_message* restore = nullptr;
      ScopedHandler(CustomThrowHandler::Kind k, kdu_message* orig)
          : custom(k), restore(orig) {
        kdu_customize_errors(&custom);
      }
      ~ScopedHandler() { kdu_customize_errors(restore); }
    };

    for (CustomThrowHandler::Kind kind : {CustomThrowHandler::Kind::kBadAlloc,
                                          CustomThrowHandler::Kind::kOther}) {
      ScopedHandler guard(kind, &error_handler_);
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      constexpr int kWorkers = (KDU_MAX_THREADS >= 64) ? 30 : 14;
      for (int i = 0; i < kWorkers; ++i) {
        ASSERT_TRUE(env.add_thread());
      }
      std::mutex mu;
      std::condition_variable cv;
      int arrived = 0;
      std::atomic<int> rem{kWorkers};
      struct WwJob final : public kdu_thread_job {
        std::mutex* mu = nullptr;
        std::condition_variable* cv = nullptr;
        int* arrived = nullptr;
        int target = 0;
        std::atomic<int>* rem = nullptr;
        RecordingThreadQueue* q = nullptr;
      };
      RecordingThreadQueue q_ww(kWorkers, "ww_catch");
      std::vector<WwJob> jobs(kWorkers);
      EXPECT_TRUE(env.attach_queue(&q_ww, nullptr, "ww_catch"));
      for (int i = 0; i < kWorkers; ++i) {
        jobs[i].mu = &mu;
        jobs[i].cv = &cv;
        jobs[i].arrived = &arrived;
        jobs[i].target = kWorkers;
        jobs[i].rem = &rem;
        jobs[i].q = &q_ww;
        jobs[i].set_job_func(
            [](kdu_thread_job* base, kdu_thread_entity* caller) {
              auto* j = static_cast<WwJob*>(base);
              {
                std::unique_lock<std::mutex> lock(*j->mu);
                if (++(*j->arrived) == j->target) {
                  j->cv->notify_all();
                } else {
                  j->cv->wait(lock, [j] { return *j->arrived >= j->target; });
                }
              }
              try {
                caller->wait_for_condition("ww_slot",
                                           /*expect_foreign_wakeup=*/true);
              } catch (...) {
                // Do not rethrow into process_jobs() so only the overflowing
                // ww_process_jobs() thread invokes handle_exception().
                ReleaseGate(j->rem, j->q, caller);
                return;
              }
              ReleaseGate(j->rem, j->q, caller);
            });
        q_ww.ScheduleOne(&jobs[i], &env, /*all_scheduled=*/(i == kWorkers - 1),
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&arrived, kWorkers] { return arrived >= kWorkers; });
      }
      kdu_exception exc = KDU_NULL_EXCEPTION;
      EXPECT_FALSE(env.join(&q_ww, false, &exc));
      const kdu_exception expected_exc =
          (kind == CustomThrowHandler::Kind::kBadAlloc)
              ? KDU_MEMORY_EXCEPTION
              : KDU_CONVERTED_EXCEPTION;
      EXPECT_EQ(expected_exc, exc);
      EXPECT_FALSE(env.destroy());
    }
  }

  // 15) add_thread failure when new_instance() returns nullptr (lines 2786,
  // 2846-2869 cleanup), both without and with a newly created domain.
  {
    class NullInstanceEntity final : public kdu_thread_entity {
     protected:
      kdu_thread_entity* new_instance() override { return nullptr; }
    };
    NullInstanceEntity env;
    env.create();
    EXPECT_FALSE(env.add_thread());
    EXPECT_FALSE(env.add_thread("null_dom"));
    EXPECT_TRUE(env.destroy());
  }
}

TEST_F(KduThreadsTest, DomainSequenceReuseAndCleanup) {
  // 17) Domain-sequence retirement and reuse from free_sequences.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread("seq_a"));
    ASSERT_TRUE(env.add_thread("seq_b"));

    RecordingThreadQueue q0(1, "seq0");
    RecordingThreadQueue q1(1, "seq1");
    RecordingThreadQueue q2(1, "seq2");
    EXPECT_TRUE(env.attach_queue(&q0, nullptr, "seq_a",
                                 /*min_sequencing_idx=*/0, /*queue_flags=*/0));
    EXPECT_TRUE(env.attach_queue(&q1, nullptr, "seq_b",
                                 /*min_sequencing_idx=*/1, /*queue_flags=*/0));
    FnJob j0([&q0](kdu_thread_entity* caller) { q0.Complete(caller); });
    q0.ScheduleOne(&j0, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(&q0));

    EXPECT_TRUE(env.attach_queue(&q2, nullptr, "seq_a",
                                 /*min_sequencing_idx=*/2, /*queue_flags=*/0));
    FnJob j1([&q1](kdu_thread_entity* caller) { q1.Complete(caller); });
    FnJob j2([&q2](kdu_thread_entity* caller) { q2.Complete(caller); });
    q1.ScheduleOne(&j1, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    q2.ScheduleOne(&j2, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(env.join(nullptr));
    EXPECT_TRUE(env.destroy());
  }
}

TEST_F(KduThreadsTest, ForeignSignalAndUnattachedErrors) {
  // 18) Affinity/priority edge paths, unattached queue join/terminate after
  // exception, and KDU_FOREIGN_SIGNAL_EXCEPTION / EXPECTED mismatch.
  {
    kdu_thread self;
    EXPECT_TRUE(self.set_to_self());
    (void)self.set_cpu_affinity(0, 0);
    (void)self.set_cpu_affinity(1, 64);
    int min_prio = 0;
    int max_prio = 0;
    const int cur_prio = self.get_priority(min_prio, max_prio);
    (void)self.set_priority(min_prio - 10);
    (void)self.set_priority(max_prio + 10);
    (void)self.set_priority(cur_prio);

    kdu_thread_entity_affinity extra_aff;
    EXPECT_TRUE(extra_aff.add_preferred_bundle(0, 0));
    EXPECT_EQ(0, extra_aff.add_node_bundle(0, -1));
    (void)extra_aff.add_node_bundle(1, 64);

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread("sig_w"));
    ASSERT_TRUE(env.add_thread("busy_w"));

    RecordingThreadQueue q_sig(1, "sig_q");
    RecordingThreadQueue q_busy(1, "busy_q");
    ASSERT_TRUE(env.attach_queue(&q_sig, nullptr, "sig_w"));
    ASSERT_TRUE(env.attach_queue(&q_busy, nullptr, "busy_w"));

    std::atomic<kdu_thread_entity_condition*> cond_ptr{nullptr};
    std::atomic<bool> busy_started{false};
    std::atomic<bool> waiter_done{false};
    std::atomic<bool> waiter_threw{false};
    std::atomic<bool> stop_busy{false};
    FnJob busy_job([&](kdu_thread_entity* caller) {
      busy_started.store(true, std::memory_order_release);
      while (!stop_busy.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      q_busy.Complete(caller);
    });
    q_busy.ScheduleOne(&busy_job, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!busy_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    FnJob waiter_job([&](kdu_thread_entity* caller) {
      kdu_thread_entity_condition* c = caller->get_condition();
      cond_ptr.store(c, std::memory_order_release);
      try {
        caller->wait_for_condition("foreign_exc_wait",
                                   /*expect_foreign_wakeup=*/false);
      } catch (...) {
        waiter_threw.store(true, std::memory_order_release);
        waiter_done.store(true, std::memory_order_release);
        q_sig.Complete(caller);
        throw;
      }
      waiter_done.store(true, std::memory_order_release);
      q_sig.Complete(caller);
    });
    q_sig.ScheduleOne(&waiter_job, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);

    kdu_thread_entity_condition* c = nullptr;
    while ((c = cond_ptr.load(std::memory_order_acquire)) == nullptr) {
      std::this_thread::yield();
    }
    env.signal_condition(c, KDU_FOREIGN_SIGNAL_EXCEPTION);
    while (!waiter_done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    stop_busy.store(true, std::memory_order_release);
    kdu_exception exc = KDU_NULL_EXCEPTION;
    const bool joined = env.join(nullptr, false, &exc);
    if (waiter_threw.load(std::memory_order_acquire)) {
      EXPECT_FALSE(joined);
      EXPECT_FALSE(env.destroy());
    } else {
      EXPECT_TRUE(joined);
      RecordingThreadQueue q_unattached(1, "unattached");
      env.handle_exception(KDU_ERROR_EXCEPTION);
      exc = KDU_NULL_EXCEPTION;
      EXPECT_FALSE(env.join(&q_unattached, false, &exc));
      EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
      exc = KDU_NULL_EXCEPTION;
      EXPECT_FALSE(env.terminate(&q_unattached, false, &exc));
      EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
      EXPECT_FALSE(env.destroy());
    }
  }
}

TEST_F(KduThreadsTest, MultiProducerCleanDirtyPalettes) {
  // 19) Multi-producer clean_dirty_palettes under sustained hazard-pointer
  // pressure (lines 1617, 1639, 1649, 1653-1654, 1661-1663).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    for (int i = 0; i < 8; ++i) {
      ASSERT_TRUE(env.add_thread("hp"));
    }

    constexpr int kProducers = 4;
    constexpr int kSlots = 32;
    constexpr int kNoiseJobs = 4000;
    struct alignas(64) SlotJob final : public kdu_thread_job {
      std::atomic<bool> ready{true};
    };

    RecordingThreadQueue q_noise(kNoiseJobs, "hp_noise");
    ASSERT_TRUE(env.attach_queue(&q_noise, nullptr, "hp_n"));
    std::atomic<bool> stop_noise{false};
    std::atomic<int> noise_rem{kNoiseJobs + 1};
    std::vector<FnJob> njobs;
    njobs.reserve(kNoiseJobs);
    std::vector<kdu_thread_job*> nptrs(kNoiseJobs);
    for (int i = 0; i < kNoiseJobs; ++i) {
      njobs.emplace_back([&](kdu_thread_entity* caller) {
        if (!stop_noise.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        ReleaseGate(&noise_rem, &q_noise, caller);
      });
      nptrs[i] = &njobs[i];
    }
    q_noise.BindRange(nptrs.data(), kNoiseJobs, 0);
    q_noise.ScheduleBatch(nptrs.data(), kNoiseJobs, &env,
                          /*all_scheduled=*/true);

    std::vector<std::unique_ptr<RecordingThreadQueue>> cqueues;
    std::vector<std::unique_ptr<RecordingThreadQueue>> pqueues;
    std::vector<std::vector<SlotJob>> sjobs;
    cqueues.reserve(kProducers);
    pqueues.reserve(kProducers);
    sjobs.reserve(kProducers);
    std::vector<std::unique_ptr<FnJob>> pjobs;
    pjobs.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
      cqueues.push_back(std::make_unique<RecordingThreadQueue>(kSlots, "hp_c"));
      pqueues.push_back(std::make_unique<RecordingThreadQueue>(1, "hp_p"));
      ASSERT_TRUE(env.attach_queue(cqueues[p].get(), nullptr, "hp"));
      ASSERT_TRUE(env.attach_queue(pqueues[p].get(), nullptr, "hp"));
      sjobs.emplace_back(kSlots);
      std::vector<kdu_thread_job*> sptrs(kSlots);
      for (int i = 0; i < kSlots; ++i) {
        sjobs[p][i].set_job_func([](kdu_thread_job* base, kdu_thread_entity*) {
          static_cast<SlotJob*>(base)->ready.store(true,
                                                   std::memory_order_release);
        });
        sptrs[i] = &sjobs[p][i];
      }
      cqueues[p]->BindRange(sptrs.data(), kSlots, 0);
      pjobs.push_back(
          std::make_unique<FnJob>([&, p](kdu_thread_entity* caller) {
            for (int iter = 0; iter < 1500; ++iter) {
              SlotJob& sj = sjobs[p][iter % kSlots];
              while (!sj.ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
              }
              sj.ready.store(false, std::memory_order_release);
              cqueues[p]->ScheduleOne(&sj, caller, /*all_scheduled=*/false, 0);
            }
            pqueues[p]->Complete(caller);
          }));
    }
    for (int p = 0; p < kProducers; ++p) {
      pqueues[p]->ScheduleOne(pjobs[p].get(), &env, /*all_scheduled=*/true,
                              KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }

    for (int p = 0; p < kProducers; ++p) {
      EXPECT_TRUE(env.join(pqueues[p].get()));
      for (int i = 0; i < kSlots; ++i) {
        while (!sjobs[p][i].ready.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
      }
      cqueues[p]->NoteAllScheduled(&env);
      cqueues[p]->Complete(&env);
    }
    stop_noise.store(true, std::memory_order_release);
    ReleaseGate(&noise_rem, &q_noise, &env);
    EXPECT_TRUE(env.join(nullptr));
    EXPECT_TRUE(env.destroy());
  }
}

TEST_F(KduThreadsTest, EntityEventsAndRemainingSafePaths) {
  {
    // Unregistered base/derived event lifecycle and no-ops before registering.
    kdu_thread_entity_event base_ev;
    base_ev.handle_signal(nullptr);
    EXPECT_FALSE(base_ev.signal());
    EXPECT_EQ(1, base_ev.deregister(nullptr));
  }

  {
    // Terminate descendants_only=true and terminate(nullptr), worker
    // change_group_owner_thread() failure, owner affinity at create(),
    // and handle_exception() disabling events_allowed.
    kdu_thread_entity env;
    env.create(/*cpu_affinity=*/1, /*also_set_owner_affinity=*/true);
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);
    RecordingThreadQueue parent_q(1, "parent_q");
    RecordingThreadQueue child_q(1, "child_q");
    ASSERT_TRUE(env.attach_queue(&parent_q, nullptr, "parent_dom"));
    ASSERT_TRUE(env.attach_queue(&child_q, &parent_q, "child_dom", 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
    std::atomic<int> count{0};
    std::atomic<bool> worker_owner_switch_ok{true};
    FnJob c_job([&](kdu_thread_entity* caller) {
      if (caller != nullptr && !caller->is_group_owner()) {
        worker_owner_switch_ok.store(caller->change_group_owner_thread());
      } else {
        worker_owner_switch_ok.store(false);
      }
      count.fetch_add(1);
      child_q.Complete(caller);
    });
    FnJob p_job([&](kdu_thread_entity* caller) {
      count.fetch_add(1);
      parent_q.Complete(caller);
    });
    try {
      child_q.ScheduleOne(&c_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
      parent_q.ScheduleOne(&p_job, &env, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.terminate(&parent_q, /*descendants_only=*/true));
      EXPECT_TRUE(env.terminate(nullptr));
    } catch (kdu_exception exc) {
      ADD_FAILURE() << "Unexpected kdu_exception " << exc << ": "
                    << error_handler_.TakeMessage();
      (void)env.terminate(nullptr);
    }
    EXPECT_EQ(2, count.load());
    EXPECT_FALSE(worker_owner_switch_ok.load());

    kdu_thread_entity_event ev;
    env.activate_event(&ev);
    env.deactivate_event(&ev);
    env.handle_exception(KDU_ERROR_EXCEPTION);
    EXPECT_FALSE(env.register_event(&ev));
    EXPECT_FALSE(env.destroy());

    kdu_thread self;
    self.set_to_self();
    (void)self.set_cpu_affinity(0, 0);
  }
}

TEST_F(KduThreadsTest, AdditionalEdgePathsAndSequenceTerminators) {
  // 1) dequeue_general_job() when num_active_sets == 0 on foreign wakeup wait.
  {
    ScopedThreadEntity empty_env;
    empty_env.entity.create();
    kdu_thread_entity_condition* cond = empty_env.entity.get_condition();
    ASSERT_NE(nullptr, cond);
    std::mutex mu;
    std::condition_variable cv;
    bool started = false;
    std::thread waker([&] {
      {
        std::lock_guard<std::mutex> lk(mu);
        started = true;
      }
      cv.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      empty_env.entity.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&] { return started; });
    }
    empty_env.entity.wait_for_condition("no_active_sets",
                                        /*expect_foreign_wakeup=*/true);
    waker.join();
  }

  // 2) Single-threaded deterministic KD_JOB_TERMINATOR handling in
  // dequeue_general_job(): non-first sequence (`!(seq_flags & 2)`) and
  // first sequence (`seq_flags & 2`) advancement.
  {
    ScopedThreadEntity seq_env;
    seq_env.entity.create();

    RecordingThreadQueue q0(1, "seq_q0");
    RecordingThreadQueue q1(1, "seq_q1");
    RecordingThreadQueue q_wrap(1, "seq_wrap");
    ASSERT_TRUE(seq_env.entity.attach_queue(&q0, nullptr, "seq_dom", 0));
    ASSERT_TRUE(seq_env.entity.attach_queue(&q1, nullptr, "seq_dom", 1));
    ASSERT_TRUE(seq_env.entity.attach_queue(&q_wrap, nullptr, "seq_dom", 2));

    FnJob j1([&](kdu_thread_entity* c) { q1.Complete(c); });
    q1.ScheduleOne(&j1, &seq_env.entity, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(seq_env.entity.join(&q1));

    FnJob j_wrap([&](kdu_thread_entity* c) { q_wrap.Complete(c); });
    q_wrap.ScheduleOne(&j_wrap, &seq_env.entity, true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(seq_env.entity.join(&q_wrap));

    RecordingThreadQueue q2(1, "seq_q2");
    ASSERT_TRUE(seq_env.entity.attach_queue(&q2, nullptr, "seq_dom", 3));
    FnJob j2([&](kdu_thread_entity* c) { q2.Complete(c); });
    q2.ScheduleOne(&j2, &seq_env.entity, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(seq_env.entity.join(&q2));

    FnJob j0([&](kdu_thread_entity* c) { q0.Complete(c); });
    q0.ScheduleOne(&j0, &seq_env.entity, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(seq_env.entity.join(&q0));

    // Also cover seq_flags & 2 first-sequence terminator removal when a
    // two-job next sequence wraps back to index 0.
    RecordingThreadQueue q_a(1, "first_a");
    RecordingThreadQueue q_b(2, "first_b");
    ASSERT_TRUE(seq_env.entity.attach_queue(&q_a, nullptr, "dom_first", 0));
    ASSERT_TRUE(seq_env.entity.attach_queue(&q_b, nullptr, "dom_first", 1));
    FnJob ja([&](kdu_thread_entity* c) { q_a.Complete(c); });
    q_a.ScheduleOne(&ja, &seq_env.entity, true, KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(seq_env.entity.join(&q_a));

    std::atomic<int> b_done{0};
    FnJob jb1([&](kdu_thread_entity*) { b_done.fetch_add(1); });
    FnJob jb2([&](kdu_thread_entity* c) {
      b_done.fetch_add(1);
      q_b.Complete(c);
    });
    kdu_thread_job* b_jobs[2] = {&jb1, &jb2};
    q_b.BindRange(b_jobs, 2, 0);
    q_b.ScheduleBatch(b_jobs, 2, &seq_env.entity, true);
    EXPECT_TRUE(seq_env.entity.join(&q_b));
    EXPECT_EQ(2, b_done.load());
  }
}

TEST_F(KduThreadsTest, YieldFrequencyAndMultiWorkerContention) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  env.set_yield_frequency(1);
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());
  WarmupWorkers(env);

  // Run multi-domain batches with yield_freq=1 plus SAFE_CONTEXT and BACKGROUND
  // queues while owner waits so NWW wakeups, yield transitions, ww_return_job,
  // and palette recycling paths exercise concurrently.
  for (int wave = 0; wave < 24; ++wave) {
    RecordingThreadQueue norm_q(64, "wave_norm");
    RecordingThreadQueue safe_q(8, "wave_safe");
    RecordingThreadQueue bkg_q(16, "wave_bkg");
    ASSERT_TRUE(env.attach_queue(&norm_q, nullptr, "wave_a"));
    ASSERT_TRUE(env.attach_queue(
        &safe_q, nullptr, "wave_b", wave,
        KDU_THREAD_QUEUE_SAFE_CONTEXT | KDU_THREAD_QUEUE_PRIORITY_HIGH));
    ASSERT_TRUE(env.attach_queue(&bkg_q, nullptr, "wave_c", wave,
                                 KDU_THREAD_QUEUE_BACKGROUND));

    std::atomic<int> norm_rem{64};
    std::vector<FnJob> norm_jobs;
    norm_jobs.reserve(64);
    std::array<kdu_thread_job*, 64> norm_ptrs;
    for (int i = 0; i < 64; ++i) {
      norm_jobs.emplace_back([&](kdu_thread_entity* c) {
        if (norm_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          norm_q.Complete(c);
        }
      });
      norm_ptrs[i] = &norm_jobs.back();
    }
    norm_q.BindRange(norm_ptrs.data(), 64, 0);

    std::atomic<int> safe_rem{8};
    std::vector<FnJob> safe_jobs;
    safe_jobs.reserve(8);
    std::array<kdu_thread_job*, 8> safe_ptrs;
    for (int i = 0; i < 8; ++i) {
      safe_jobs.emplace_back([&](kdu_thread_entity* c) {
        if (safe_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          safe_q.Complete(c);
        }
      });
      safe_ptrs[i] = &safe_jobs.back();
    }
    safe_q.BindRange(safe_ptrs.data(), 8, 0);

    std::atomic<int> bkg_rem{16};
    std::vector<FnJob> bkg_jobs;
    bkg_jobs.reserve(16);
    std::array<kdu_thread_job*, 16> bkg_ptrs;
    for (int i = 0; i < 16; ++i) {
      bkg_jobs.emplace_back([&](kdu_thread_entity* c) {
        if (bkg_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          bkg_q.Complete(c);
        }
      });
      bkg_ptrs[i] = &bkg_jobs.back();
    }
    bkg_q.BindRange(bkg_ptrs.data(), 16, 0);

    safe_q.ScheduleBatch(safe_ptrs.data(), 8, &env, true);
    bkg_q.ScheduleBatch(bkg_ptrs.data(), 16, &env, true);
    norm_q.ScheduleBatch(norm_ptrs.data(), 64, &env, true);
    EXPECT_TRUE(env.join(&safe_q));
    EXPECT_TRUE(env.join(&bkg_q));
    EXPECT_TRUE(env.join(&norm_q));
    EXPECT_EQ(0, safe_rem.load());
    EXPECT_EQ(0, bkg_rem.load());
    EXPECT_EQ(0, norm_rem.load());
  }
}

TEST_F(KduThreadsTest, RemovableSequencesAndFastMultiWorkerContention) {
  // 1) Advance work_domains past seq0 while q0 still holds a reference
  // (`num_queues == 1`), complete q0 inside seq1's second job, then attach q2
  // so `have_removable_sequences` triggers `remove_unused_sequences()`.
  {
    ScopedThreadEntity rem_scoped;
    kdu_thread_entity& rem_env = rem_scoped.entity;
    rem_env.create();

    RecordingThreadQueue q0(1, "rem_q0");
    RecordingThreadQueue q1(2, "rem_q1");
    ASSERT_TRUE(rem_env.attach_queue(&q0, nullptr, "rem_dom", 0));
    ASSERT_TRUE(rem_env.attach_queue(&q1, nullptr, "rem_dom", 1));

    FnJob j0([&](kdu_thread_entity*) {});
    q0.ScheduleOne(&j0, &rem_env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);

    FnJob j1a([&](kdu_thread_entity*) {});
    FnJob j1b([&](kdu_thread_entity* c) {
      q0.Complete(c);
      q1.Complete(c);
    });
    kdu_thread_job* q1_jobs[2] = {&j1a, &j1b};
    q1.BindRange(q1_jobs, 2, 0);
    q1.ScheduleBatch(q1_jobs, 2, &rem_env, /*all_scheduled=*/true);

    EXPECT_TRUE(rem_env.join(&q1));
    EXPECT_TRUE(rem_env.join(&q0));

    RecordingThreadQueue q2(1, "rem_q2");
    ASSERT_TRUE(rem_env.attach_queue(&q2, nullptr, "rem_dom", 2));
    FnJob j2([&](kdu_thread_entity* c) { q2.Complete(c); });
    q2.ScheduleOne(&j2, &rem_env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    EXPECT_TRUE(rem_env.join(&q2));
  }

  // 2) Exercise clean_dirty_palettes() hazard scanning/collision, NWW
  // idle-list unwoken-count contention, and join() completion CAS races across
  // 4 active workers.
  {
    ScopedThreadEntity stress_scoped;
    kdu_thread_entity& env = stress_scoped.entity;
    env.create();
    env.set_yield_frequency(1);
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    // 400 single-job ScheduleOne calls exhaust KD_THREAD_PALETTES repeatedly
    // while workers continuously scan palettes holding hazard pointers.
    constexpr int kStreamJobs = 400;
    for (int round = 0; round < 3; ++round) {
      RecordingThreadQueue stream_q(kStreamJobs, "stream_q");
      ASSERT_TRUE(env.attach_queue(&stream_q, nullptr, "stream_dom", round));
      std::atomic<int> remaining{kStreamJobs};
      std::vector<FnJob> jobs;
      jobs.reserve(kStreamJobs);
      for (int i = 0; i < kStreamJobs; ++i) {
        jobs.emplace_back([&](kdu_thread_entity* c) {
          kdu_thread::micro_pause();
          if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            stream_q.Complete(c);
          }
        });
      }
      for (int i = 0; i < kStreamJobs; ++i) {
        stream_q.ScheduleOne(&jobs[i], &env, i + 1 == kStreamJobs,
                             KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      EXPECT_TRUE(env.join(&stream_q));
      EXPECT_EQ(0, remaining.load());
    }

    // Rapid micro-bursts across normal + SAFE_CONTEXT queues induce sleep/wake
    // transitions, join CAS retries, and worker-yield wakeups.
    for (int burst = 0; burst < 120; ++burst) {
      RecordingThreadQueue q_fast(1, "fast_q");
      ASSERT_TRUE(env.attach_queue(&q_fast, nullptr, "fast_dom", burst));
      FnJob jf([&](kdu_thread_entity* c) { q_fast.Complete(c); });
      q_fast.ScheduleOne(&jf, &env, /*all_scheduled=*/false,
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.join(&q_fast));

      if ((burst & 3) == 0) {
        RecordingThreadQueue q_n(4, "bn");
        RecordingThreadQueue q_s(2, "bs");
        ASSERT_TRUE(env.attach_queue(&q_n, nullptr, "bn_dom", burst));
        ASSERT_TRUE(env.attach_queue(&q_s, nullptr, "bs_dom", burst,
                                     KDU_THREAD_QUEUE_SAFE_CONTEXT));
        std::atomic<int> rem_n{4};
        std::atomic<int> rem_s{2};
        std::array<FnJob, 4> jn = {
            FnJob([&](kdu_thread_entity* c) {
              if (rem_n.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                q_n.Complete(c);
              }
            }),
            FnJob([&](kdu_thread_entity* c) {
              if (rem_n.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                q_n.Complete(c);
              }
            }),
            FnJob([&](kdu_thread_entity* c) {
              if (rem_n.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                q_n.Complete(c);
              }
            }),
            FnJob([&](kdu_thread_entity* c) {
              if (rem_n.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                q_n.Complete(c);
              }
            }),
        };
        std::array<kdu_thread_job*, 4> pn = {&jn[0], &jn[1], &jn[2], &jn[3]};
        q_n.BindRange(pn.data(), 4, 0);

        FnJob js0([&](kdu_thread_entity* c) {
          if (rem_s.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            q_s.Complete(c);
          }
        });
        FnJob js1([&](kdu_thread_entity* c) {
          if (rem_s.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            q_s.Complete(c);
          }
        });
        kdu_thread_job* ps[2] = {&js0, &js1};
        q_s.BindRange(ps, 2, 0);

        q_n.ScheduleBatch(pn.data(), 4, &env, true);
        q_s.ScheduleBatch(ps, 2, &env, true);
        EXPECT_TRUE(env.join(&q_s));
        EXPECT_TRUE(env.join(&q_n));
        EXPECT_EQ(0, rem_n.load());
        EXPECT_EQ(0, rem_s.load());
      }
    }
  }
}

TEST_F(KduThreadsTest, PaletteRecyclingForeignNoDeadlockAndContention) {
  // 1) Clamp `find_preferred_bundle_count` against bounds and processor count.
  EXPECT_LE(0, kdu_thread_entity_affinity::find_preferred_bundle_count(-1));
  EXPECT_LE(0, kdu_thread_entity_affinity::find_preferred_bundle_count(0));
  EXPECT_LE(0, kdu_thread_entity_affinity::find_preferred_bundle_count(1));
  EXPECT_LE(0, kdu_thread_entity_affinity::find_preferred_bundle_count(2));
  EXPECT_LE(1, kdu_thread_entity_affinity::find_preferred_bundle_count(3));
  EXPECT_LE(0, kdu_thread_entity_affinity::find_preferred_bundle_count(32));
  EXPECT_LE(0, kdu_thread_entity_affinity::find_preferred_bundle_count(64));
  EXPECT_LE(0, kdu_thread_entity_affinity::find_preferred_bundle_count(65));

  // 2) Drive `clean_dirty_palettes` hazard reclamation (`q_main` palettes only)
  //    plus multi-producer `wake_non_waiting_workers` exhaustion, yield/wake,
  //    and working-waiter contention.
  {
    struct CountJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      CountJob() { set_job_func(&CountJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<CountJob*>(base);
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    struct SigWaitJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      kdu_thread_entity_condition* cond = nullptr;
      int delay_spins = 0;
      SigWaitJob() { set_job_func(&SigWaitJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<SigWaitJob*>(base);
        for (int s = 0; s < self->delay_spins; ++s) {
          std::this_thread::yield();
        }
        if (self->cond != nullptr) {
          caller->signal_condition(self->cond, 0);
        }
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    struct WaiterWorkerJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      std::atomic<kdu_thread_entity_condition*>* cond_out = nullptr;
      WaiterWorkerJob() { set_job_func(&WaiterWorkerJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<WaiterWorkerJob*>(base);
        kdu_thread_entity_condition* c = caller->get_condition();
        self->cond_out->store(c, std::memory_order_release);
        caller->wait_for_condition("worker_ww",
                                   /*expect_foreign_wakeup=*/false);
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    constexpr int kWorkers = 12;
    for (int i = 0; i < kWorkers; ++i) {
      ASSERT_TRUE(env.add_thread());
    }
    env.set_yield_frequency(1);
    WarmupWorkers(env);

    // Prime `returned_palettes` with hazard-marked entries.
    {
      constexpr int kPrimeJobs = 3000;
      RecordingThreadQueue q_prime(kPrimeJobs, "prime");
      ASSERT_TRUE(env.attach_queue(&q_prime, nullptr, "prime_dom"));
      std::atomic<int> prime_rem{kPrimeJobs + 1};
      std::vector<CountJob> prime_jobs(kPrimeJobs);
      for (int i = 0; i < kPrimeJobs; ++i) {
        prime_jobs[i].q = &q_prime;
        prime_jobs[i].rem = &prime_rem;
        q_prime.ScheduleOne(&prime_jobs[i], &env,
                            /*all_scheduled=*/(i + 1 == kPrimeJobs),
                            KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      if (prime_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_prime.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_prime));
      EXPECT_EQ(0, prime_rem.load());
    }

    // Main streams `ScheduleOne` waves exclusively from its own `palette_heap`
    // while all 12 workers dequeue those exact palettes, so worker hazard
    // pointers (`thread_job_hzps`) match main's `clean_dirty_palettes` list.
    for (int wave = 0; wave < 6; ++wave) {
      constexpr int kMainJobs = 2400;
      RecordingThreadQueue q_main(kMainJobs, "main_stream");
      ASSERT_TRUE(env.attach_queue(&q_main, nullptr, "main_stream_dom"));
      std::atomic<int> main_rem{kMainJobs + 1};
      std::vector<CountJob> main_jobs(kMainJobs);
      for (int i = 0; i < kMainJobs; ++i) {
        main_jobs[i].q = &q_main;
        main_jobs[i].rem = &main_rem;
        q_main.ScheduleOne(&main_jobs[i], &env,
                           /*all_scheduled=*/(i + 1 == kMainJobs),
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      if (main_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_main.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_main));
      EXPECT_EQ(0, main_rem.load());
    }

    // Flood idle workers with oversized batches + overlapping yields to hit
    // `wake_non_waiting_workers` exhaustion and `nww_trans_complete` wakeups.
    for (int round = 0; round < 20; ++round) {
      std::this_thread::sleep_for(std::chrono::microseconds(150));
      constexpr int kFlood = 48;
      RecordingThreadQueue q_flood(kFlood, "wake_flood");
      ASSERT_TRUE(env.attach_queue(&q_flood, nullptr, "wake_flood_dom"));
      std::atomic<int> flood_rem{kFlood + 1};
      std::vector<CountJob> jobs(kFlood);
      std::array<kdu_thread_job*, kFlood> ptrs{};
      for (int i = 0; i < kFlood; ++i) {
        jobs[i].q = &q_flood;
        jobs[i].rem = &flood_rem;
        ptrs[i] = &jobs[i];
      }
      q_flood.BindRange(ptrs.data(), kFlood, 0);
      q_flood.ScheduleBatch(ptrs.data(), kFlood, &env, /*all_scheduled=*/false);
      // Stagger a second batch while workers yield in `nww_trans_complete`.
      constexpr int kFollow = 8;
      RecordingThreadQueue q_follow(kFollow, "wake_follow");
      ASSERT_TRUE(env.attach_queue(&q_follow, nullptr, "wake_follow_dom"));
      std::atomic<int> follow_rem{kFollow + 1};
      std::vector<CountJob> fjobs(kFollow);
      std::array<kdu_thread_job*, kFollow> fptrs{};
      for (int i = 0; i < kFollow; ++i) {
        fjobs[i].q = &q_follow;
        fjobs[i].rem = &follow_rem;
        fptrs[i] = &fjobs[i];
      }
      q_follow.BindRange(fptrs.data(), kFollow, 0);
      q_follow.ScheduleBatch(fptrs.data(), kFollow, &env,
                             /*all_scheduled=*/true);
      if (follow_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_follow.Complete(&env);
      }
      q_flood.NoteAllScheduled(&env);
      if (flood_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_flood.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_flood));
      EXPECT_TRUE(env.join(&q_follow));
    }

    // Overlap worker working-waiters and main `wait_for_condition` + `join`
    // to exercise `check_working_waiters_idle`, `ww_return_job`, and
    // `enter_wait` signal races.
    for (int round = 0; round < 120; ++round) {
      RecordingThreadQueue q_ww(4, "ww_race");
      ASSERT_TRUE(env.attach_queue(&q_ww, nullptr, "ww_race_dom"));
      std::atomic<int> ww_rem{5};
      std::atomic<kdu_thread_entity_condition*> w_cond{nullptr};
      WaiterWorkerJob wj;
      wj.q = &q_ww;
      wj.rem = &ww_rem;
      wj.cond_out = &w_cond;

      kdu_thread_entity_condition* owner_cond = env.get_condition();
      std::array<SigWaitJob, 3> sj;
      for (int k = 0; k < 3; ++k) {
        sj[k].q = &q_ww;
        sj[k].rem = &ww_rem;
        sj[k].delay_spins = (k == 2) ? (round % 4) : 0;
        sj[k].cond = (k == 2) ? owner_cond : nullptr;
      }
      q_ww.ScheduleOne(&wj, &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      while (w_cond.load(std::memory_order_acquire) == nullptr) {
        std::this_thread::yield();
      }
      q_ww.ScheduleOne(&sj[0], &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      q_ww.ScheduleOne(&sj[1], &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      q_ww.ScheduleOne(&sj[2], &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      env.signal_condition(w_cond.load(std::memory_order_acquire), 0);
      env.wait_for_condition("owner_ww", /*expect_foreign_wakeup=*/false);
      if (ww_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_ww.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_ww));
    }

    EXPECT_TRUE(env.destroy());
  }

  // 3) Pinned domain-sequence overlap (`dequeue_domain_specific_job` `seq_flags
  //    & 2` and `add_domain_sequence`) and `ww_reserve_job` exception exit.
  {
    struct SeqCountJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      SeqCountJob() { set_job_func(&SeqCountJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<SeqCountJob*>(base);
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread("seq_dom"));
    ASSERT_TRUE(env.add_thread("seq_dom"));
    env.set_yield_frequency(1);
    WarmupWorkers(env);

    for (int round = 0; round < 60; ++round) {
      RecordingThreadQueue q_a(4, "seq_a");
      RecordingThreadQueue q_b(4, "seq_b");
      ASSERT_TRUE(env.attach_queue(&q_a, nullptr, "seq_dom", round * 2, 0));
      std::atomic<int> rem_a{5};
      std::array<SeqCountJob, 4> ja;
      std::array<kdu_thread_job*, 4> pa{};
      for (int i = 0; i < 4; ++i) {
        ja[i].q = &q_a;
        ja[i].rem = &rem_a;
        pa[i] = &ja[i];
      }
      q_a.BindRange(pa.data(), 4, 0);
      q_a.ScheduleBatch(pa.data(), 4, &env, /*all_scheduled=*/true);
      if (rem_a.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_a.Complete(&env);
      }

      ASSERT_TRUE(env.attach_queue(&q_b, nullptr, "seq_dom", round * 2 + 1, 0));
      std::atomic<int> rem_b{5};
      std::array<SeqCountJob, 4> jb;
      std::array<kdu_thread_job*, 4> pb{};
      for (int i = 0; i < 4; ++i) {
        jb[i].q = &q_b;
        jb[i].rem = &rem_b;
        pb[i] = &jb[i];
      }
      q_b.BindRange(pb.data(), 4, 0);
      q_b.ScheduleBatch(pb.data(), 4, &env, /*all_scheduled=*/true);
      if (rem_b.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_b.Complete(&env);
      }
      EXPECT_TRUE(env.join(nullptr));
    }
    EXPECT_TRUE(env.destroy());
  }

  for (int round = 0; round < 8; ++round) {
    struct ThrowJob final : public kdu_thread_job {
      int spins = 0;
      ThrowJob() { set_job_func(&ThrowJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity*) {
        auto* self = static_cast<ThrowJob*>(base);
        for (int s = 0; s < self->spins; ++s) {
          std::this_thread::yield();
        }
        throw std::runtime_error("ww_reserve_exc");
      }
    };
    struct YieldNoopJob final : public kdu_thread_job {
      YieldNoopJob() { set_job_func(&YieldNoopJob::Run); }
      static void Run(kdu_thread_job*, kdu_thread_entity*) {
        std::this_thread::yield();
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    for (int w = 0; w < 4; ++w) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);

    RecordingThreadQueue q_err(8, "ww_err");
    ASSERT_TRUE(env.attach_queue(&q_err, nullptr, "ww_err_dom"));
    std::array<YieldNoopJob, 7> yj;
    ThrowJob tj;
    tj.spins = round % 3;
    std::array<kdu_thread_job*, 8> ptrs{};
    for (int i = 0; i < 7; ++i) {
      ptrs[i] = &yj[i];
    }
    ptrs[7] = &tj;
    q_err.BindRange(ptrs.data(), 8, 0);
    q_err.ScheduleBatch(ptrs.data(), 8, &env, /*all_scheduled=*/true);
    kdu_exception exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(env.join(&q_err, false, &exc));
    EXPECT_EQ(KDU_CONVERTED_EXCEPTION, exc);
    EXPECT_FALSE(env.destroy());
  }

  // 4) Asynchronous hierarchical `terminate()` (worker completes queue after
  //    `request_termination()` returns without synchronous `all_done()`).
  {
    class AsyncTermQueue final : public kdu_thread_queue {
     public:
      int get_max_jobs() override { return 2; }
      void ScheduleOne(kdu_thread_job* job, kdu_thread_entity* caller) {
        schedule_job(job, caller, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      void Complete(kdu_thread_entity* caller) { all_done(caller); }
      void request_termination(kdu_thread_entity* /*caller*/) override {
        term_requested.store(true, std::memory_order_release);
      }
      std::atomic<bool> term_requested{false};
    };

    struct TermWorkerJob final : public kdu_thread_job {
      AsyncTermQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      std::atomic<bool> started{false};
      TermWorkerJob() { set_job_func(&TermWorkerJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<TermWorkerJob*>(base);
        self->started.store(true, std::memory_order_release);
        while (!self->q->term_requested.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    AsyncTermQueue q_root;
    AsyncTermQueue q_child;
    AsyncTermQueue q_done_child;
    ASSERT_TRUE(env.attach_queue(&q_root, nullptr, "term_root"));
    ASSERT_TRUE(env.attach_queue(&q_child, &q_root, "term_child"));
    ASSERT_TRUE(env.attach_queue(&q_done_child, &q_root, "term_done_child"));
    q_done_child.Complete(&env);

    std::atomic<int> rem_child{2};
    TermWorkerJob j_child;
    j_child.q = &q_child;
    j_child.rem = &rem_child;
    q_child.ScheduleOne(&j_child, &env);
    if (rem_child.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      q_child.Complete(&env);
    }
    while (!j_child.started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    EXPECT_TRUE(env.terminate(&q_root, /*descendants_only=*/true));
    EXPECT_TRUE(q_child.term_requested.load());
    EXPECT_FALSE(q_child.is_attached());
    EXPECT_TRUE(q_root.is_attached());

    std::atomic<int> rem_root{2};
    TermWorkerJob j_root;
    j_root.q = &q_root;
    j_root.rem = &rem_root;
    q_root.ScheduleOne(&j_root, &env);
    if (rem_root.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      q_root.Complete(&env);
    }
    while (!j_root.started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    EXPECT_TRUE(env.terminate(&q_root, /*descendants_only=*/false));
    EXPECT_TRUE(q_root.term_requested.load());
    EXPECT_FALSE(q_root.is_attached());
    EXPECT_TRUE(env.destroy());
  }

  // 5) Middle-list reclamation in `kd_thread_domain::remove_unused_sequences()`
  //    while a worker still holds a consumer reference on sequence 0.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread("mid_rm"));
    ASSERT_TRUE(env.add_thread("mid_rm"));
    WarmupWorkers(env);

    std::mutex mu;
    std::condition_variable cv;
    bool j0_started = false;
    bool release_j0 = false;

    struct HoldSeq0Job final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      std::mutex* mu = nullptr;
      std::condition_variable* cv = nullptr;
      bool* started = nullptr;
      bool* release = nullptr;
      HoldSeq0Job() { set_job_func(&HoldSeq0Job::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<HoldSeq0Job*>(base);
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
        std::unique_lock<std::mutex> lock(*self->mu);
        *self->started = true;
        self->cv->notify_all();
        self->cv->wait(lock, [self] { return *self->release; });
      }
    };

    struct QuickSeqJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      std::atomic<bool>* started = nullptr;
      QuickSeqJob() { set_job_func(&QuickSeqJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<QuickSeqJob*>(base);
        self->started->store(true, std::memory_order_release);
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    RecordingThreadQueue q0(1, "s0"), q1(1, "s1"), q2(1, "s2");
    ASSERT_TRUE(env.attach_queue(&q0, nullptr, "mid_rm", 0, 0));
    ASSERT_TRUE(env.attach_queue(&q1, nullptr, "mid_rm", 1, 0));
    ASSERT_TRUE(env.attach_queue(&q2, nullptr, "mid_rm", 2, 0));

    std::atomic<int> rem0{2}, rem1{2}, rem2{2};
    HoldSeq0Job j0;
    j0.q = &q0;
    j0.rem = &rem0;
    j0.mu = &mu;
    j0.cv = &cv;
    j0.started = &j0_started;
    j0.release = &release_j0;
    q0.ScheduleOne(&j0, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    if (rem0.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      q0.Complete(&env);
    }
    {
      std::unique_lock<std::mutex> lock(mu);
      cv.wait(lock, [&] { return j0_started; });
    }
    EXPECT_TRUE(env.join(&q0));

    std::atomic<bool> j1_started{false};
    QuickSeqJob j1;
    j1.q = &q1;
    j1.rem = &rem1;
    j1.started = &j1_started;
    q1.ScheduleOne(&j1, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    if (rem1.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      q1.Complete(&env);
    }
    while (!j1_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    EXPECT_TRUE(env.join(&q1));

    std::atomic<bool> j2_started{false};
    QuickSeqJob j2;
    j2.q = &q2;
    j2.rem = &rem2;
    j2.started = &j2_started;
    q2.ScheduleOne(&j2, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    if (rem2.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      q2.Complete(&env);
    }
    while (!j2_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    EXPECT_TRUE(env.join(&q2));

    {
      std::lock_guard<std::mutex> lock(mu);
      release_j0 = true;
    }
    cv.notify_all();
    EXPECT_TRUE(env.destroy());
  }

  // 6) Exercise `check_working_waiters_idle()` with an active foreign-wakeup
  //    waiter while owner enters `join()` with no remaining runnable jobs.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q_ww_idle(1, "ww_idle");
    ASSERT_TRUE(env.attach_queue(&q_ww_idle, nullptr, "ww_idle_dom"));
    std::atomic<int> rem{2};
    std::atomic<kdu_thread_entity_condition*> w_cond{nullptr};

    struct ForeignWaitJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      std::atomic<kdu_thread_entity_condition*>* cond_out = nullptr;
      ForeignWaitJob() { set_job_func(&ForeignWaitJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<ForeignWaitJob*>(base);
        self->cond_out->store(caller->get_condition(),
                              std::memory_order_release);
        caller->wait_for_condition("ww_idle_check",
                                   /*expect_foreign_wakeup=*/true);
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    } job;
    job.q = &q_ww_idle;
    job.rem = &rem;
    job.cond_out = &w_cond;
    q_ww_idle.ScheduleOne(&job, &env, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
    if (rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      q_ww_idle.Complete(&env);
    }

    while (w_cond.load(std::memory_order_acquire) == nullptr) {
      std::this_thread::yield();
    }
    std::thread wake_thread([&env, &w_cond]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      env.signal_condition(w_cond.load(std::memory_order_acquire),
                           KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    EXPECT_TRUE(env.join(&q_ww_idle));
    wake_thread.join();
    EXPECT_TRUE(env.destroy());
  }

  // 7) Exercise `find_preferred_bundle_count()` clamping against `num_procs`,
  //    `initialize()` with mixed clean and dirty returned palettes, and
  //    unregistered event guards (`register_event()` after `handle_exception`).
  {
    const int num_procs = kdu_get_num_processors();
    const int bundle_target = (num_procs + 1 < 64) ? (num_procs + 1) : 64;
    EXPECT_GE(
        kdu_thread_entity_affinity::find_preferred_bundle_count(bundle_target),
        0);
  }
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();

    struct InlineJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      InlineJob() { set_job_func(&InlineJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<InlineJob*>(base);
        self->q->Complete(caller);
      }
    };

    auto run_drain_queue = [&](int max_jobs) {
      RecordingThreadQueue q(max_jobs, "plt_mix");
      ASSERT_TRUE(env.attach_queue(&q, nullptr, "plt_mix_dom"));
      InlineJob j;
      j.q = &q;
      q.ScheduleOne(&j, &env, /*all_scheduled=*/true,
                    KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.join(&q));
    };

    // Exhaust the second 256-palette block (`free_palettes == NULL`) while
    // leaving 1 clean palette nestled among 255 dirty ones in
    // `returned_palettes`, so `add_thread()` encounters both clean and dirty
    // returned palettes during `initialize()`.
    for (int i = 0; i < 100; ++i) {
      run_drain_queue(1);
    }
    run_drain_queue(2);
    for (int i = 0; i < 154; ++i) {
      run_drain_queue(1);
    }
    ASSERT_TRUE(env.add_thread());
    EXPECT_TRUE(env.destroy());
  }
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    env.handle_exception(kdu_exception(-1));

    kdu_thread_entity_event ev;
    ev.handle_signal(&env);
    EXPECT_FALSE(ev.signal());
    EXPECT_FALSE(env.register_event(&ev));
    env.activate_event(&ev);
    env.deactivate_event(&ev);
    EXPECT_EQ(1, ev.deregister(&env, false, false));
    EXPECT_EQ(1, ev.deregister(nullptr, false, false));
    EXPECT_FALSE(env.destroy());
  }
  {
    // (8b-1) Drive > 256 per-queue jobs across 8 workers with zero in-job
    // yields to stress `clean_dirty_palettes()` hazard detection, followed by a
    // short yield burst to exercise `nww_yield()` wakeups.
    struct BulkJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      bool yield_in_job = false;
      int sleep_us = 0;
      BulkJob() { set_job_func(&BulkJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<BulkJob*>(base);
        if (self->sleep_us > 0) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(self->sleep_us));
        } else if (self->yield_in_job) {
          std::this_thread::yield();
        }
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    for (int w = 0; w < 8; ++w) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);
    env.set_yield_frequency(1);
    env.declare_first_owner_wait_safe(false);

    constexpr int kBulkJobs = 1024;
    for (int round = 0; round < 4; ++round) {
      RecordingThreadQueue q_bulk(kBulkJobs, "bulk_plt");
      ASSERT_TRUE(env.attach_queue(&q_bulk, nullptr, "bulk_dom", 0,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      std::vector<BulkJob> bulk_jobs(kBulkJobs);
      std::atomic<int> rem{kBulkJobs + 1};
      for (int i = 0; i < kBulkJobs; ++i) {
        bulk_jobs[i].q = &q_bulk;
        bulk_jobs[i].rem = &rem;
        bulk_jobs[i].yield_in_job = false;
        bulk_jobs[i].sleep_us = 0;
        q_bulk.ScheduleOne(&bulk_jobs[i], &env, /*all_scheduled=*/false,
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      q_bulk.NoteAllScheduled(&env);
      if (rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_bulk.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_bulk));
    }

    constexpr int kYieldJobs = 64;
    for (int round = 0; round < 4; ++round) {
      RecordingThreadQueue q_yield(kYieldJobs, "yield_burst");
      ASSERT_TRUE(env.attach_queue(&q_yield, nullptr, "yield_dom", 0,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      std::vector<BulkJob> yield_jobs(kYieldJobs);
      std::atomic<int> rem{kYieldJobs + 1};
      for (int i = 0; i < kYieldJobs; ++i) {
        yield_jobs[i].q = &q_yield;
        yield_jobs[i].rem = &rem;
        yield_jobs[i].yield_in_job = (i % 4 == 0);
        yield_jobs[i].sleep_us = 0;
        q_yield.ScheduleOne(&yield_jobs[i], &env, /*all_scheduled=*/false,
                            KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      q_yield.NoteAllScheduled(&env);
      if (rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_yield.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_yield));
    }

    // (8b-2) Contend `ww_reserve_job` / `ww_return_job` and `join` CAS between
    // `q_safe` (unsafe for owner) and single-job `q_fg`.
    for (int round = 0; round < 64; ++round) {
      constexpr int kSafeJobs = 24;
      RecordingThreadQueue q_safe(kSafeJobs, "safe_bg");
      RecordingThreadQueue q_fg(1, "fg_join");
      ASSERT_TRUE(env.attach_queue(&q_safe, nullptr, "safe_dom", round,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      ASSERT_TRUE(env.attach_queue(&q_fg, nullptr, "fg_dom", round));

      std::vector<BulkJob> safe_jobs(kSafeJobs);
      std::array<kdu_thread_job*, kSafeJobs> safe_ptrs{};
      std::atomic<int> safe_rem{kSafeJobs + 1};
      for (int i = 0; i < kSafeJobs; ++i) {
        safe_jobs[i].q = &q_safe;
        safe_jobs[i].rem = &safe_rem;
        safe_ptrs[i] = &safe_jobs[i];
      }
      q_safe.BindRange(safe_ptrs.data(), kSafeJobs, 0);
      q_safe.ScheduleBatch(safe_ptrs.data(), kSafeJobs, &env,
                           /*all_scheduled=*/true);
      if (safe_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_safe.Complete(&env);
      }

      BulkJob fg_job;
      std::atomic<int> fg_rem{2};
      fg_job.q = &q_fg;
      fg_job.rem = &fg_rem;
      kdu_thread_job* fg_ptr = &fg_job;
      q_fg.BindRange(&fg_ptr, 1, 0);
      q_fg.ScheduleBatch(&fg_ptr, 1, &env, /*all_scheduled=*/true);
      if (fg_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_fg.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_fg));
      EXPECT_TRUE(env.join(&q_safe));
    }

    // Exercise tight domain sequence rollover overlapping 1-job completion
    // and working-waiter join wakeup races (`enter_wait` / `terminate`).
    for (int round = 0; round < 256; ++round) {
      RecordingThreadQueue q_a(1, "seq_a");
      ASSERT_TRUE(env.attach_queue(&q_a, nullptr, "roll_dom", round * 2));
      BulkJob ja;
      std::atomic<int> rem_a{2};
      ja.q = &q_a;
      ja.rem = &rem_a;
      q_a.ScheduleOne(&ja, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
      if (rem_a.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_a.Complete(&env);
      }

      RecordingThreadQueue q_b(1, "seq_b");
      ASSERT_TRUE(env.attach_queue(&q_b, nullptr, "roll_dom", round * 2 + 1));
      BulkJob jb;
      std::atomic<int> rem_b{2};
      jb.q = &q_b;
      jb.rem = &rem_b;
      q_b.ScheduleOne(&jb, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
      if (rem_b.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_b.Complete(&env);
      }

      EXPECT_TRUE(env.join(&q_a));
      EXPECT_TRUE(env.join(&q_b));
    }

    // (8b-3) Exercise foreign wakeup overlapping worker idle transition.
    for (int round = 0; round < 16; ++round) {
      RecordingThreadQueue q_pre(1, "pre_fw");
      ASSERT_TRUE(env.attach_queue(&q_pre, nullptr, "fw_dom"));
      BulkJob pre_job;
      std::atomic<int> pre_rem{2};
      pre_job.q = &q_pre;
      pre_job.rem = &pre_rem;
      q_pre.ScheduleOne(&pre_job, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
      if (pre_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_pre.Complete(&env);
      }
      kdu_thread_entity_condition* cond = env.get_condition();
      std::thread waker([&] {
        std::this_thread::yield();
        env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      env.wait_for_condition("fw_race", true);
      waker.join();
      EXPECT_TRUE(env.join(&q_pre));
    }
    EXPECT_TRUE(env.destroy());
  }
  {
    // (8b-5) Dedicated 2-worker contention block tailored for small-machine
    // NPROC=2 scheduling to exercise domain-specific dequeue, nww_yield wakeup,
    // ww_return_job, check_working_waiters_idle, completion_state CAS, and
    // sequence rollover without worker starvation.
    struct YieldableJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      bool should_yield = false;
      YieldableJob() { set_job_func(&YieldableJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<YieldableJob*>(base);
        if (self->should_yield) {
          std::this_thread::yield();
        }
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    env.set_yield_frequency(1);
    WarmupWorkers(env);
    env.declare_first_owner_wait_safe(false);

    for (int round = 0; round < 16; ++round) {
      constexpr int kJobs = 64;
      RecordingThreadQueue q_y(kJobs, "two_w_yield");
      ASSERT_TRUE(env.attach_queue(&q_y, nullptr, "two_w_dom", round));
      std::vector<YieldableJob> jobs(kJobs);
      std::atomic<int> rem{kJobs + 1};
      for (int i = 0; i < kJobs; ++i) {
        jobs[i].q = &q_y;
        jobs[i].rem = &rem;
        jobs[i].should_yield = (i % 4 == 0);
        q_y.ScheduleOne(&jobs[i], &env, /*all_scheduled=*/false,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      q_y.NoteAllScheduled(&env);
      if (rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_y.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_y));
    }

    for (int round = 0; round < 128; ++round) {
      constexpr int kSafeJobs = 24;
      RecordingThreadQueue q_safe(kSafeJobs, "two_w_safe_bg");
      RecordingThreadQueue q_fg(1, "two_w_fg_join");
      ASSERT_TRUE(env.attach_queue(&q_safe, nullptr, "two_w_safe_dom", round,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      ASSERT_TRUE(env.attach_queue(&q_fg, nullptr, "two_w_fg_dom", round));
      std::vector<YieldableJob> safe_jobs(kSafeJobs);
      std::atomic<int> safe_rem{kSafeJobs + 1};
      for (int i = 0; i < kSafeJobs; ++i) {
        safe_jobs[i].q = &q_safe;
        safe_jobs[i].rem = &safe_rem;
        q_safe.ScheduleOne(&safe_jobs[i], &env, /*all_scheduled=*/false,
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      q_safe.NoteAllScheduled(&env);
      if (safe_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_safe.Complete(&env);
      }

      YieldableJob fg_job;
      std::atomic<int> fg_rem{2};
      fg_job.q = &q_fg;
      fg_job.rem = &fg_rem;
      q_fg.ScheduleOne(&fg_job, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      if (fg_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_fg.Complete(&env);
      }

      EXPECT_TRUE(env.join(&q_fg));
      EXPECT_TRUE(env.join(&q_safe));
    }

    for (int round = 0; round < 256; ++round) {
      RecordingThreadQueue q_a(1, "two_w_seq_a");
      ASSERT_TRUE(env.attach_queue(&q_a, nullptr, "two_w_roll_dom", round * 2));
      YieldableJob ja;
      std::atomic<int> rem_a{2};
      ja.q = &q_a;
      ja.rem = &rem_a;
      q_a.ScheduleOne(&ja, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
      if (rem_a.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_a.Complete(&env);
      }

      RecordingThreadQueue q_b(1, "two_w_seq_b");
      ASSERT_TRUE(
          env.attach_queue(&q_b, nullptr, "two_w_roll_dom", round * 2 + 1));
      YieldableJob jb;
      std::atomic<int> rem_b{2};
      jb.q = &q_b;
      jb.rem = &rem_b;
      q_b.ScheduleOne(&jb, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
      if (rem_b.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_b.Complete(&env);
      }

      EXPECT_TRUE(env.join(&q_a));
      EXPECT_TRUE(env.join(&q_b));
    }

    for (int round = 0; round < 32; ++round) {
      RecordingThreadQueue q_pre(1, "two_w_pre_fw");
      ASSERT_TRUE(env.attach_queue(&q_pre, nullptr, "two_w_fw_dom"));
      YieldableJob pre_job;
      std::atomic<int> pre_rem{2};
      pre_job.q = &q_pre;
      pre_job.rem = &pre_rem;
      q_pre.ScheduleOne(&pre_job, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
      if (pre_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_pre.Complete(&env);
      }
      kdu_thread_entity_condition* cond = env.get_condition();
      std::thread waker([&] {
        std::this_thread::yield();
        env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
      });
      env.wait_for_condition("two_w_fw_race", true);
      waker.join();
      EXPECT_TRUE(env.join(&q_pre));
    }

    EXPECT_TRUE(env.destroy());
  }
  {
    // (8b-6) Deterministic hazard-held palette sweep (`1654, 1661-1663`),
    // sequence list recycling (`1161, 1162`), secondary terminated sequence
    // (`4495`), and idle domain-isolated dequeue (`4310`).
    struct HoldJob final : public kdu_thread_job {
      std::atomic<bool>* started = nullptr;
      std::atomic<bool>* release = nullptr;
      RecordingThreadQueue* q = nullptr;
      HoldJob() { set_job_func(&HoldJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<HoldJob*>(base);
        self->started->store(true, std::memory_order_release);
        while (!self->release->load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        self->q->Complete(caller);
      }
    };
    struct FastJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      FastJob() { set_job_func(&FastJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<FastJob*>(base);
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    for (int w = 0; w < 8; ++w) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);

    // 1. Keep 4x896=3584 bound jobs (128 palettes per bucket via
    // `ScheduleBatch`) rotating over 256 waves (32,768 palette swaps) so all 8
    // workers contend inside `get_job()` while `schedule_jobs` triggers
    // `clean_dirty_palettes` Phase 2b (`1639, 1653, 1654, 1661-1663`).
    {
      struct ChurnJob final : public kdu_thread_job {
        std::atomic<int>* bucket_rem = nullptr;
        ChurnJob() { set_job_func(&ChurnJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
          auto* self = static_cast<ChurnJob*>(base);
          self->bucket_rem->fetch_sub(1, std::memory_order_acq_rel);
        }
      };

      constexpr int kBuckets = 4;
      constexpr int kBucketSize = 896;  // 128 * KD_PLT_SLOTS
      constexpr int kTotalJobs = kBuckets * kBucketSize;
      constexpr int kWaves = 256;
      RecordingThreadQueue q_churn(kTotalJobs, "batch_churn");
      ASSERT_TRUE(env.attach_queue(&q_churn, nullptr, "churn_dom", 0));
      std::vector<ChurnJob> jobs(kTotalJobs);
      std::vector<kdu_thread_job*> ptrs(kTotalJobs);
      std::array<std::atomic<int>, kBuckets> bucket_rem{};
      for (int b = 0; b < kBuckets; ++b) {
        for (int j = 0; j < kBucketSize; ++j) {
          const int idx = b * kBucketSize + j;
          jobs[idx].bucket_rem = &bucket_rem[b];
          ptrs[idx] = &jobs[idx];
        }
      }
      q_churn.BindRange(ptrs.data(), kTotalJobs, 0);
      for (int b = 0; b < kBuckets; ++b) {
        bucket_rem[b].store(kBucketSize, std::memory_order_relaxed);
        q_churn.ScheduleBatch(&ptrs[b * kBucketSize], kBucketSize, &env,
                              /*all_scheduled=*/false);
      }
      for (int wave = 0; wave < kWaves; ++wave) {
        const int b = wave & 3;
        while (bucket_rem[b].load(std::memory_order_acquire) != 0) {
          std::this_thread::yield();
        }
        bucket_rem[b].store(kBucketSize, std::memory_order_relaxed);
        q_churn.ScheduleBatch(&ptrs[b * kBucketSize], kBucketSize, &env,
                              /*all_scheduled=*/false);
      }
      for (int b = 0; b < kBuckets; ++b) {
        while (bucket_rem[b].load(std::memory_order_acquire) != 0) {
          std::this_thread::yield();
        }
      }
      q_churn.NoteAllScheduled(&env);
      q_churn.Complete(&env);
      EXPECT_TRUE(env.join(&q_churn));
    }

    // 2. Keep a head sequence active in a 2-worker group while the second
    // worker runs secondary sequences with `(n_idle + n_trans) == 0` so
    // `dequeue_domain_specific_job` sees `KD_JOB_TERMINATOR` with
    // `!(seq_flags & 2)` (`4495`).
    {
      ScopedThreadEntity two_scoped;
      kdu_thread_entity& two_env = two_scoped.entity;
      two_env.create();
      ASSERT_TRUE(two_env.add_thread());
      ASSERT_TRUE(two_env.add_thread());
      WarmupWorkers(two_env);

      std::atomic<bool> lead_started{false};
      std::atomic<bool> release_lead{false};
      RecordingThreadQueue q_lead(1, "lead_seq");
      ASSERT_TRUE(two_env.attach_queue(&q_lead, nullptr, "sec_dom", 0));
      HoldJob lead_job;
      lead_job.started = &lead_started;
      lead_job.release = &release_lead;
      lead_job.q = &q_lead;
      q_lead.ScheduleOne(&lead_job, &two_env, /*all_scheduled=*/false,
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
      while (!lead_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (int r = 1; r <= 16; ++r) {
        constexpr int kSecJobs = 32;
        RecordingThreadQueue q_sec(kSecJobs, "sec_seq");
        ASSERT_TRUE(two_env.attach_queue(&q_sec, nullptr, "sec_dom", r));
        std::vector<FastJob> sec_jobs(kSecJobs);
        std::atomic<int> sec_rem{kSecJobs + 1};
        for (int i = 0; i < kSecJobs; ++i) {
          sec_jobs[i].q = &q_sec;
          sec_jobs[i].rem = &sec_rem;
          q_sec.ScheduleOne(&sec_jobs[i], &two_env, /*all_scheduled=*/false,
                            KDU_THREAD_JOB_AUTO_BIND_ONCE);
        }
        q_sec.NoteAllScheduled(&two_env);
        if (sec_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          q_sec.Complete(&two_env);
        }
        EXPECT_TRUE(two_env.join(&q_sec));
      }

      q_lead.NoteAllScheduled(&two_env);
      release_lead.store(true, std::memory_order_release);
      EXPECT_TRUE(two_env.join(&q_lead));
      EXPECT_TRUE(two_env.destroy());
    }

    // 3. Deterministic domain sequence recycling (`free_sequences` at
    // `1161, 1162`): keep the current tail un-terminated
    // (`all_scheduled=false`) when appending `seq_{r}`, then finish & join
    // `q[r-1]` so `seq_{r-1}` becomes removable and is recycled from
    // `free_sequences` on `r >= 2`.
    {
      constexpr int kChain = 10;
      std::vector<std::unique_ptr<RecordingThreadQueue>> qs;
      qs.reserve(kChain);
      std::vector<FastJob> jobs(kChain);
      std::vector<std::atomic<int>> rems(kChain);
      for (int r = 0; r < kChain; ++r) {
        qs.push_back(std::make_unique<RecordingThreadQueue>(1, "cyc_chain"));
        rems[r].store(2, std::memory_order_relaxed);
      }

      ASSERT_TRUE(env.attach_queue(qs[0].get(), nullptr, "cyc_dom", 0));
      jobs[0].q = qs[0].get();
      jobs[0].rem = &rems[0];
      qs[0]->ScheduleOne(&jobs[0], &env, /*all_scheduled=*/false,
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);

      for (int r = 1; r < kChain; ++r) {
        ASSERT_TRUE(env.attach_queue(qs[r].get(), nullptr, "cyc_dom", r));
        jobs[r].q = qs[r].get();
        jobs[r].rem = &rems[r];
        qs[r]->ScheduleOne(&jobs[r], &env, /*all_scheduled=*/false,
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);

        qs[r - 1]->NoteAllScheduled(&env);
        if (rems[r - 1].fetch_sub(1, std::memory_order_acq_rel) == 1) {
          qs[r - 1]->Complete(&env);
        }
        EXPECT_TRUE(env.join(qs[r - 1].get()));
      }

      qs[kChain - 1]->NoteAllScheduled(&env);
      if (rems[kChain - 1].fetch_sub(1, std::memory_order_acq_rel) == 1) {
        qs[kChain - 1]->Complete(&env);
      }
      EXPECT_TRUE(env.join(qs[kChain - 1].get()));
    }

    // 4. Concurrent worker completion (`all_done`) vs `attach_queue(seq+1)`
    // (`1176`).
    {
      struct SelfCompleteJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        SelfCompleteJob() { set_job_func(&SelfCompleteJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<SelfCompleteJob*>(base);
          self->q->Complete(caller);
        }
      };
      for (int round = 0; round < 256; ++round) {
        RecordingThreadQueue q_a(1, "race_a");
        ASSERT_TRUE(env.attach_queue(&q_a, nullptr, "race1176", round * 2));
        SelfCompleteJob ja;
        ja.q = &q_a;
        q_a.ScheduleOne(&ja, &env, /*all_scheduled=*/false,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);

        RecordingThreadQueue q_b(1, "race_b");
        ASSERT_TRUE(env.attach_queue(&q_b, nullptr, "race1176", round * 2 + 1));
        SelfCompleteJob jb;
        jb.q = &q_b;
        q_b.ScheduleOne(&jb, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
        EXPECT_TRUE(env.join(&q_a));
        EXPECT_TRUE(env.join(&q_b));
      }
    }

    EXPECT_TRUE(env.destroy());
  }
  {
    // (8b-7) Single-worker high-frequency join completion CAS (`3522`) and
    // wakeup/enter_wait race (`4178, 4179`), plus `ww_return_job` (`4146`).
    struct SelfFinishJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      int spin_iters = 0;
      SelfFinishJob() { set_job_func(&SelfFinishJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<SelfFinishJob*>(base);
        for (int k = 0; k < self->spin_iters; ++k) {
          std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        self->q->Complete(caller);
      }
    };
    struct YieldSafeJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      YieldSafeJob() { set_job_func(&YieldSafeJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<YieldSafeJob*>(base);
        std::this_thread::yield();
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);
    env.declare_first_owner_wait_safe(false);

    {
      // Scheduling via ScheduleBatch/ScheduleOne after `all_done` (before
      // `all_scheduled`) hits `2114, 2122, 2123` and `2266, 2274, 2275`.
      RecordingThreadQueue q_done_err(2, "post_done_err");
      ASSERT_TRUE(env.attach_queue(&q_done_err, nullptr, "done_err_dom", 0));
      YieldSafeJob ejobs[2];
      kdu_thread_job* ptrs[2] = {&ejobs[0], &ejobs[1]};
      q_done_err.BindRange(ptrs, 2, 0);
      q_done_err.Complete(&env);
      EXPECT_THROW(
          q_done_err.ScheduleBatch(&ptrs[0], 1, &env, /*all_scheduled=*/false),
          kdu_exception);
      EXPECT_THROW(q_done_err.ScheduleOne(&ejobs[1], &env,
                                          /*all_scheduled=*/false, 0),
                   kdu_exception);
      q_done_err.NoteAllScheduled(&env);
      EXPECT_TRUE(env.join(&q_done_err));
    }

    for (int round = 0; round < 2048; ++round) {
      RecordingThreadQueue q_j(1, "fast_join_race");
      ASSERT_TRUE(env.attach_queue(&q_j, nullptr, "fj_dom", round));
      SelfFinishJob j;
      j.q = &q_j;
      j.spin_iters = (round & 63) * 16;
      q_j.ScheduleOne(&j, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.join(&q_j));
    }

    for (int round = 0; round < 64; ++round) {
      struct DualSpinJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<int>* rem = nullptr;
        DualSpinJob() { set_job_func(&DualSpinJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<DualSpinJob*>(base);
          if (caller->is_group_owner()) {
            std::this_thread::sleep_for(std::chrono::microseconds(120));
          } else {
            std::this_thread::sleep_for(std::chrono::microseconds(30));
          }
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q->Complete(caller);
          }
        }
      };
      RecordingThreadQueue q_busy(2, "ww_busy_idle");
      ASSERT_TRUE(env.attach_queue(&q_busy, nullptr, "wwb_dom", round));
      DualSpinJob jobs[2];
      std::atomic<int> rem{3};
      jobs[0].q = &q_busy;
      jobs[0].rem = &rem;
      jobs[1].q = &q_busy;
      jobs[1].rem = &rem;
      kdu_thread_job* ptrs[2] = {&jobs[0], &jobs[1]};
      q_busy.BindRange(ptrs, 2, 0);
      q_busy.ScheduleBatch(ptrs, 2, &env, /*all_scheduled=*/true);
      if (rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_busy.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_busy));
    }

    ASSERT_TRUE(env.add_thread());
    env.set_yield_frequency(1);
    for (int round = 0; round < 128; ++round) {
      constexpr int kSafe = 8;
      RecordingThreadQueue q_safe(kSafe, "ret_safe");
      RecordingThreadQueue q_fg(1, "ret_fg");
      ASSERT_TRUE(env.attach_queue(&q_safe, nullptr, "rs_dom", round,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      ASSERT_TRUE(env.attach_queue(&q_fg, nullptr, "rf_dom", round));
      std::vector<YieldSafeJob> sjobs(kSafe);
      std::atomic<int> srem{kSafe + 1};
      for (int i = 0; i < kSafe; ++i) {
        sjobs[i].q = &q_safe;
        sjobs[i].rem = &srem;
        q_safe.ScheduleOne(&sjobs[i], &env, /*all_scheduled=*/false,
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      q_safe.NoteAllScheduled(&env);
      if (srem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_safe.Complete(&env);
      }
      SelfFinishJob fjob;
      fjob.q = &q_fg;
      q_fg.ScheduleOne(&fjob, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.join(&q_fg));
      EXPECT_TRUE(env.join(&q_safe));
    }
    EXPECT_TRUE(env.destroy());
  }
  {
    // (8b-8) Idle list put-back (`1486-1488`), `ww_return_job` (`4146`), and
    // concurrent `wake_non_waiting_workers` unwoken-count CAS (`1450`).
    struct SpinFinishJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      int sleep_us = 0;
      SpinFinishJob() { set_job_func(&SpinFinishJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<SpinFinishJob*>(base);
        if (self->sleep_us > 0) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(self->sleep_us));
        }
        if (self->rem == nullptr ||
            self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };
    struct ParentSpawnJob final : public kdu_thread_job {
      RecordingThreadQueue* parent_q = nullptr;
      std::atomic<int>* parent_rem = nullptr;
      ParentSpawnJob() { set_job_func(&ParentSpawnJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<ParentSpawnJob*>(base);
        constexpr int kSub = 8;
        RecordingThreadQueue q_sub(kSub, "sub_wake");
        caller->attach_queue(&q_sub, self->parent_q, "sub_dom", 0);
        std::vector<SpinFinishJob> children(kSub);
        std::atomic<int> child_rem{kSub + 1};
        for (int i = 0; i < kSub; ++i) {
          children[i].q = &q_sub;
          children[i].rem = &child_rem;
          q_sub.ScheduleOne(&children[i], caller, /*all_scheduled=*/false,
                            KDU_THREAD_JOB_AUTO_BIND_ONCE);
        }
        q_sub.NoteAllScheduled(caller);
        if (child_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          q_sub.Complete(caller);
        }
        caller->join(&q_sub);
        if (self->parent_rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->parent_q->Complete(caller);
        }
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    for (int w = 0; w < 8; ++w) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);
    env.declare_first_owner_wait_safe(false);

    // 1. Put all 8 workers to sleep between single-job schedules so
    // `wake_non_waiting_workers(1)` puts remaining sleepers back onto
    // `nww_idle_list` (`1486-1488`).
    for (int r = 0; r < 16; ++r) {
      std::this_thread::sleep_for(std::chrono::microseconds(250));
      RecordingThreadQueue q_one(1, "wake_one");
      ASSERT_TRUE(env.attach_queue(&q_one, nullptr, "w1_dom", r));
      SpinFinishJob j;
      j.q = &q_one;
      q_one.ScheduleOne(&j, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.join(&q_one));
    }

    // 2. Wake fewer workers via `q_safe(2)` + `q_fg(1)` while idle workers
    // remain so `ww_return_job()` returns `> 0` (`4146`).
    for (int r = 0; r < 32; ++r) {
      std::this_thread::sleep_for(std::chrono::microseconds(250));
      RecordingThreadQueue q_safe(2, "ret_idle_safe");
      RecordingThreadQueue q_fg(1, "ret_idle_fg");
      ASSERT_TRUE(env.attach_queue(&q_safe, nullptr, "ri_s_dom", r,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      ASSERT_TRUE(env.attach_queue(&q_fg, nullptr, "ri_f_dom", r));
      std::vector<SpinFinishJob> sj(2);
      std::atomic<int> srem{3};
      for (int i = 0; i < 2; ++i) {
        sj[i].q = &q_safe;
        sj[i].rem = &srem;
        sj[i].sleep_us = 25;
        q_safe.ScheduleOne(&sj[i], &env, /*all_scheduled=*/false,
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      q_safe.NoteAllScheduled(&env);
      if (srem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_safe.Complete(&env);
      }
      SpinFinishJob fj;
      fj.q = &q_fg;
      fj.sleep_us = 25;
      q_fg.ScheduleOne(&fj, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.join(&q_fg));
      EXPECT_TRUE(env.join(&q_safe));
    }

    // 3. Concurrent multi-worker child-queue scheduling to hit `old_ptr & 1`
    // in `wake_non_waiting_workers` (`1450`).
    for (int r = 0; r < 8; ++r) {
      constexpr int kParents = 4;
      RecordingThreadQueue q_par(kParents, "par_spawn");
      ASSERT_TRUE(env.attach_queue(&q_par, nullptr, "par_dom", r));
      std::vector<ParentSpawnJob> pjobs(kParents);
      std::atomic<int> prem{kParents + 1};
      for (int i = 0; i < kParents; ++i) {
        pjobs[i].parent_q = &q_par;
        pjobs[i].parent_rem = &prem;
        q_par.ScheduleOne(&pjobs[i], &env, /*all_scheduled=*/false,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      q_par.NoteAllScheduled(&env);
      if (prem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_par.Complete(&env);
      }
      EXPECT_TRUE(env.join(&q_par));
    }

    EXPECT_TRUE(env.destroy());
  }
  {
    // (8b-4) Exception raised on worker while owner spins in `ww_process_jobs`
    // skipping `SAFE_CONTEXT` queues so `ww_reserve_job` sees `exception`.
    struct BgSpinJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<int>* rem = nullptr;
      BgSpinJob() { set_job_func(&BgSpinJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<BgSpinJob*>(base);
        for (int y = 0; y < 4; ++y) {
          std::this_thread::yield();
        }
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q->Complete(caller);
        }
      }
    };
    struct ExcJob final : public kdu_thread_job {
      ExcJob() { set_job_func(&ExcJob::Run); }
      static void Run(kdu_thread_job* /*base*/, kdu_thread_entity* caller) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        caller->handle_exception(kdu_exception(-7));
      }
    };

    for (int round = 0; round < 64; ++round) {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      ASSERT_TRUE(env.add_thread());
      ASSERT_TRUE(env.add_thread());
      WarmupWorkers(env);
      env.declare_first_owner_wait_safe(false);

      constexpr int kBgJobs = 64;
      RecordingThreadQueue q_bg(kBgJobs, "bg_keep_busy");
      ASSERT_TRUE(env.attach_queue(&q_bg, nullptr, "bg_dom", 0,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      std::vector<BgSpinJob> bg_jobs(kBgJobs);
      std::atomic<int> bg_rem{kBgJobs + 1};
      for (int i = 0; i < kBgJobs; ++i) {
        bg_jobs[i].q = &q_bg;
        bg_jobs[i].rem = &bg_rem;
        q_bg.ScheduleOne(&bg_jobs[i], &env, /*all_scheduled=*/false,
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
      q_bg.NoteAllScheduled(&env);
      if (bg_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        q_bg.Complete(&env);
      }

      RecordingThreadQueue q_exc(1, "exc_ww");
      ASSERT_TRUE(env.attach_queue(&q_exc, nullptr, "exc_dom", 0,
                                   KDU_THREAD_QUEUE_SAFE_CONTEXT));
      ExcJob exc_job;
      q_exc.ScheduleOne(&exc_job, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
      kdu_exception code = 0;
      EXPECT_FALSE(env.join(&q_exc, false, &code));
      EXPECT_EQ(kdu_exception(-7), code);
      EXPECT_FALSE(env.destroy());
    }
  }
  {
    // (8b-9) Cover find_preferred_bundle_count clamping to num_processors,
    // ~kdu_thread_queue warning on still-attached queue (force_detach cleanup),
    // NWW yield wake_another (4009), and ww_return_job wakeups (4146).
    const int num_procs = kdu_get_num_processors();
    const int capped_bundle =
        (num_procs > 0 && num_procs < 64) ? (num_procs + 1) : 64;
    EXPECT_GE(
        kdu_thread_entity_affinity::find_preferred_bundle_count(capped_bundle),
        0);

    {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      ASSERT_TRUE(env.add_thread());
      const int warn_before = warning_handler_.WarningCount();
      {
        RecordingThreadQueue q_dangling(1, "dangling_attached");
        ASSERT_TRUE(env.attach_queue(&q_dangling, nullptr, "dang_dom", 0));
      }
      EXPECT_GT(warning_handler_.WarningCount(), warn_before);
      EXPECT_TRUE(env.destroy());
    }

    struct NoopCountJob final : public kdu_thread_job {
      std::atomic<int>* done = nullptr;
      NoopCountJob() { set_job_func(&NoopCountJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
        auto* self = static_cast<NoopCountJob*>(base);
        self->done->fetch_add(1, std::memory_order_release);
      }
    };
    struct SafeTriggerJob final : public kdu_thread_job {
      RecordingThreadQueue* q_safe = nullptr;
      RecordingThreadQueue* q_wait = nullptr;
      std::atomic<int>* rem = nullptr;
      int sleep_us = 35;
      SafeTriggerJob() { set_job_func(&SafeTriggerJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<SafeTriggerJob*>(base);
        std::this_thread::sleep_for(std::chrono::microseconds(self->sleep_us));
        if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->q_safe->Complete(caller);
          self->q_wait->NoteAllScheduled(caller);
          self->q_wait->Complete(caller);
        }
      }
    };

    const auto run_section = [&](const char* name, auto&& fn) {
      try {
        fn();
      } catch (kdu_exception exc) {
        ADD_FAILURE() << name << " threw kdu_exception " << exc << ": "
                      << error_handler_.TakeMessage();
      }
    };

    // (8b-9a) NWW yield wake_another (4009) using 2..3 workers + Owner sleeping
    // passively in join(&q) so all workers run concurrently with IDLE_CNT==0
    // and o_active >= 4*o_trans + 2 when yield_frequency==1 triggers.
    run_section("8b-9a", [&] {
      struct BarrierYieldJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<int>* entered = nullptr;
        std::atomic<int>* rem = nullptr;
        int need_entered = 0;
        BarrierYieldJob() { set_job_func(&BarrierYieldJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<BarrierYieldJob*>(base);
          if (self->entered->fetch_add(1, std::memory_order_acq_rel) <
              self->need_entered) {
            for (int spin = 0;
                 spin < 4000 && self->entered->load(std::memory_order_acquire) <
                                    self->need_entered;
                 ++spin) {
              std::this_thread::yield();
            }
          }
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q->Complete(caller);
          }
        }
      };
      for (int num_workers : {2, 3}) {
        constexpr int kBatch = 32;
        std::vector<BarrierYieldJob> jobs(kBatch);
        std::atomic<int> entered{0};
        std::atomic<int> rem{kBatch};
        ScopedThreadEntity yield_scoped;
        kdu_thread_entity& yenv = yield_scoped.entity;
        yenv.create();
        for (int t = 0; t < num_workers; ++t) {
          ASSERT_TRUE(yenv.add_thread());
        }
        WarmupWorkers(yenv);
        yenv.declare_first_owner_wait_safe(false);
        yenv.set_yield_frequency(1);
        for (int wave = 0; wave < 64; ++wave) {
          entered.store(0, std::memory_order_relaxed);
          rem.store(kBatch, std::memory_order_relaxed);
          RecordingThreadQueue q(kBatch, "yield_wake_q");
          ASSERT_TRUE(yenv.attach_queue(&q, nullptr, "yw_dom", wave));
          std::array<kdu_thread_job*, kBatch> ptrs{};
          for (int i = 0; i < kBatch; ++i) {
            jobs[i].q = &q;
            jobs[i].entered = &entered;
            jobs[i].rem = &rem;
            jobs[i].need_entered = num_workers;
            ptrs[i] = &jobs[i];
          }
          q.BindRange(ptrs.data(), kBatch, 0);
          q.ScheduleBatch(ptrs.data(), kBatch, &yenv, /*all_scheduled=*/true);
          EXPECT_TRUE(yenv.join(&q));
        }
        EXPECT_TRUE(yenv.destroy());
      }
    });

    // (8b-9b) ww_return_job wakeups (4146) using 8 workers + 12 empty polling
    // domains (<= 16 active domains) and a worker-owner handshake so T drops to
    // 0 while I > 0 and J > A + T hold deterministically on return from do_job.
    run_section("8b-9b", [&] {
      struct HandshakeRetJob final : public kdu_thread_job {
        RecordingThreadQueue* q_safe = nullptr;
        RecordingThreadQueue* q_wait = nullptr;
        std::atomic<int>* rem = nullptr;
        std::atomic<int>* workers_in_job = nullptr;
        std::atomic<bool>* owner_exiting = nullptr;
        HandshakeRetJob() { set_job_func(&HandshakeRetJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<HandshakeRetJob*>(base);
          if (caller->is_group_owner()) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
            while (self->workers_in_job->load(std::memory_order_acquire) < 2 &&
                   std::chrono::steady_clock::now() < deadline) {
              std::this_thread::yield();
            }
            self->owner_exiting->store(true, std::memory_order_release);
          } else {
            self->workers_in_job->fetch_add(1, std::memory_order_acq_rel);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
            while (!self->owner_exiting->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
              std::this_thread::yield();
            }
          }
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q_safe->Complete(caller);
            self->q_wait->NoteAllScheduled(caller);
            self->q_wait->Complete(caller);
          }
        }
      };

      constexpr int kRounds = 16;
      constexpr int kSafeJobs = 8;
      constexpr int kEmptyDoms = 12;
      std::vector<std::vector<HandshakeRetJob>> all_jobs(kRounds);
      std::array<std::atomic<int>, kRounds> round_rem{};
      std::array<std::atomic<int>, kRounds> workers_in_job{};
      std::array<std::atomic<bool>, kRounds> owner_exiting{};
      std::vector<std::unique_ptr<RecordingThreadQueue>> q_empty(kEmptyDoms);
      std::vector<std::unique_ptr<RecordingThreadQueue>> q_safe_list(kRounds);
      std::vector<std::unique_ptr<RecordingThreadQueue>> q_wait_list(kRounds);
      std::vector<std::unique_ptr<RecordingThreadQueue>> q_unsafe_list(kRounds);
      std::vector<std::vector<NoopCountJob>> unsafe_jobs(kRounds);
      std::array<std::atomic<int>, kRounds> unsafe_done{};

      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      for (int t = 0; t < 8; ++t) {
        ASSERT_TRUE(env.add_thread());
      }
      WarmupWorkers(env);
      env.set_yield_frequency(1);
      env.declare_first_owner_wait_safe(false);

      for (int d = 0; d < kEmptyDoms; ++d) {
        q_empty[d] = std::make_unique<RecordingThreadQueue>(1, "ww_ret_empty");
        std::string dname = "ww_empty_dom_" + std::to_string(d);
        ASSERT_TRUE(env.attach_queue(q_empty[d].get(), nullptr, dname.c_str(),
                                     d, KDU_THREAD_QUEUE_SAFE_CONTEXT));
      }

      for (int round = 0; round < kRounds; ++round) {
        q_safe_list[round] =
            std::make_unique<RecordingThreadQueue>(kSafeJobs, "ww_ret_safe");
        q_wait_list[round] =
            std::make_unique<RecordingThreadQueue>(1, "ww_ret_wait");
        RecordingThreadQueue& q_safe = *q_safe_list[round];
        RecordingThreadQueue& q_wait = *q_wait_list[round];
        ASSERT_TRUE(env.attach_queue(&q_safe, nullptr, "safe_ret_dom", round,
                                     KDU_THREAD_QUEUE_SAFE_CONTEXT));
        ASSERT_TRUE(env.attach_queue(&q_wait, nullptr, "wait_ret_dom", round));
        all_jobs[round].resize(kSafeJobs);
        round_rem[round].store(kSafeJobs, std::memory_order_relaxed);
        workers_in_job[round].store(0, std::memory_order_relaxed);
        owner_exiting[round].store(false, std::memory_order_relaxed);
        std::array<kdu_thread_job*, kSafeJobs> ptrs{};
        for (int i = 0; i < kSafeJobs; ++i) {
          all_jobs[round][i].q_safe = &q_safe;
          all_jobs[round][i].q_wait = &q_wait;
          all_jobs[round][i].rem = &round_rem[round];
          all_jobs[round][i].workers_in_job = &workers_in_job[round];
          all_jobs[round][i].owner_exiting = &owner_exiting[round];
          ptrs[i] = &all_jobs[round][i];
        }
        q_safe.BindRange(ptrs.data(), kSafeJobs, 0);
        q_safe.ScheduleBatch(ptrs.data(), kSafeJobs, &env,
                             /*all_scheduled=*/true);
        EXPECT_TRUE(env.join(&q_wait));
        EXPECT_TRUE(env.join(&q_safe));

        q_unsafe_list[round] =
            std::make_unique<RecordingThreadQueue>(kSafeJobs, "ww_ret_unsafe");
        RecordingThreadQueue& q_unsafe = *q_unsafe_list[round];
        ASSERT_TRUE(
            env.attach_queue(&q_unsafe, nullptr, "unsafe_ret_dom", round));
        unsafe_jobs[round].resize(kSafeJobs);
        unsafe_done[round].store(0, std::memory_order_relaxed);
        std::array<kdu_thread_job*, kSafeJobs> uptrs{};
        for (int i = 0; i < kSafeJobs; ++i) {
          unsafe_jobs[round][i].done = &unsafe_done[round];
          uptrs[i] = &unsafe_jobs[round][i];
        }
        q_unsafe.BindRange(uptrs.data(), kSafeJobs, 0);
        q_unsafe.ScheduleBatch(uptrs.data(), kSafeJobs, &env,
                               /*all_scheduled=*/true);
        while (unsafe_done[round].load(std::memory_order_acquire) < kSafeJobs) {
          std::this_thread::yield();
        }
        q_unsafe.Complete(&env);
        EXPECT_TRUE(env.join(&q_unsafe));
      }
      for (int d = 0; d < kEmptyDoms; ++d) {
        q_empty[d]->NoteAllScheduled(&env);
        q_empty[d]->Complete(&env);
        EXPECT_TRUE(env.join(q_empty[d].get()));
      }
      EXPECT_TRUE(env.destroy());
    });

    // (8b-9c) Hit ww_process_jobs exception break (4150):
    // Owner runs SAFE_CONTEXT jobs inside env.join(&q_exc) (hammering
    // start_hunting -> ww_reserve_job -> do_job), while Worker 0 calls
    // handle_exception(-7) mid-loop so ww_reserve_job sets exception = true.
    run_section("8b-9c", [&] {
      constexpr int kExcJobs = 32;
      struct SafeExcRaceJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<int>* rem = nullptr;
        std::atomic<int>* owner_jobs = nullptr;
        std::atomic<bool>* threw = nullptr;
        int idx = 0;
        int throw_after_owner_jobs = 1;
        SafeExcRaceJob() { set_job_func(&SafeExcRaceJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<SafeExcRaceJob*>(base);
          if (caller->is_group_owner()) {
            self->owner_jobs->fetch_add(1, std::memory_order_acq_rel);
          } else {
            if (!self->threw->load(std::memory_order_acquire) &&
                self->owner_jobs->load(std::memory_order_acquire) >=
                    self->throw_after_owner_jobs) {
              if (!self->threw->exchange(true, std::memory_order_acq_rel)) {
                caller->handle_exception(kdu_exception(-7));
              }
            }
            for (int spin = 0; spin < (self->idx & 3); ++spin) {
              std::this_thread::yield();
            }
          }
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q->Complete(caller);
          }
        }
      };
      for (int round = 0; round < 64; ++round) {
        std::vector<SafeExcRaceJob> jobs(kExcJobs);
        std::atomic<int> rem{kExcJobs};
        std::atomic<int> owner_jobs{0};
        std::atomic<bool> threw{false};
        RecordingThreadQueue q_exc(kExcJobs, "ww_exc_race");

        ScopedThreadEntity scoped;
        kdu_thread_entity& env = scoped.entity;
        env.create();
        ASSERT_TRUE(env.add_thread());
        WarmupWorkers(env);
        env.declare_first_owner_wait_safe(false);

        ASSERT_TRUE(env.attach_queue(&q_exc, nullptr, "ww_exc_dom", round,
                                     KDU_THREAD_QUEUE_SAFE_CONTEXT));
        std::array<kdu_thread_job*, kExcJobs> ptrs{};
        for (int i = 0; i < kExcJobs; ++i) {
          jobs[i].q = &q_exc;
          jobs[i].rem = &rem;
          jobs[i].owner_jobs = &owner_jobs;
          jobs[i].threw = &threw;
          jobs[i].idx = i;
          jobs[i].throw_after_owner_jobs = 1 + (round & 7);
          ptrs[i] = &jobs[i];
        }
        q_exc.BindRange(ptrs.data(), kExcJobs, 0);
        q_exc.ScheduleBatch(ptrs.data(), kExcJobs, &env,
                            /*all_scheduled=*/true);

        kdu_exception code = 0;
        if (!env.join(&q_exc, false, &code)) {
          EXPECT_EQ(kdu_exception(-7), code);
        }
        if (!threw.load(std::memory_order_acquire)) {
          env.handle_exception(kdu_exception(-7));
        }
        EXPECT_FALSE(env.destroy());
      }
    });
  }
  {
    const auto run_section = [&](const char* name, auto&& fn) {
      try {
        fn();
      } catch (kdu_exception exc) {
        ADD_FAILURE() << name << " threw kdu_exception " << exc << ": "
                      << error_handler_.TakeMessage();
      }
    };

    // (8b-10) Deterministic old_tail->terminate (1176), concurrent
    // wake_non_waiting_workers CAS contention (1448-1488), tight join/Complete
    // & cond_signalled races (1503-1520, 3522, 4175-4180), and multi-palette
    // ScheduleBatch(512) bursts (1654, 1661-1663).
    run_section("8b-10.1", [&] {
      // 1a. Deterministic 1176: attach q4(seq=0),
      // ScheduleOne(all_scheduled=false), wait & call q4.Complete(&env) WITHOUT
      // NoteAllScheduled so q4 is all_done yet unterminated as tail, then
      // attach q5(seq=1) -> old_tail->terminate.
      struct SimpleCountJob final : public kdu_thread_job {
        std::atomic<int>* done = nullptr;
        SimpleCountJob() { set_job_func(&SimpleCountJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
          auto* self = static_cast<SimpleCountJob*>(base);
          self->done->fetch_add(1, std::memory_order_release);
        }
      };
      RecordingThreadQueue q1(1, "term_q1");
      RecordingThreadQueue q2(1, "term_q2");
      RecordingThreadQueue q3(1, "term_q3");
      RecordingThreadQueue q4(1, "term_q4");
      RecordingThreadQueue q5(1, "term_q5");
      RecordingThreadQueue qs0(1, "term_same0");
      RecordingThreadQueue qs1(1, "term_same1");
      SimpleCountJob j1, j2, j3, j4, j5, js0, js1;
      std::atomic<int> done{0};
      j1.done = &done;
      j2.done = &done;
      j3.done = &done;
      j4.done = &done;
      j5.done = &done;
      js0.done = &done;
      js1.done = &done;

      {
        ScopedThreadEntity scoped;
        kdu_thread_entity& env = scoped.entity;
        env.create();
        ASSERT_TRUE(env.add_thread());
        ASSERT_TRUE(env.add_thread());

        // Tail q1 completed WITHOUT all_scheduled -> attaching q2(seq=1)
        // triggers old_tail->terminate (1176).
        ASSERT_TRUE(env.attach_queue(&q1, nullptr, "term_dom", 0));
        q1.ScheduleOne(&j1, &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (done.load(std::memory_order_acquire) < 1) {
          std::this_thread::yield();
        }
        q1.Complete(&env);
        ASSERT_TRUE(env.attach_queue(&q2, nullptr, "term_dom", 1));
        EXPECT_TRUE(env.join(&q1));
        q2.ScheduleOne(&j2, &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (done.load(std::memory_order_acquire) < 2) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        q2.Complete(&env);
        ASSERT_TRUE(env.attach_queue(&q3, nullptr, "term_dom", 2));
        EXPECT_TRUE(env.join(&q2));
        q3.ScheduleOne(&j3, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (done.load(std::memory_order_acquire) < 3) {
          std::this_thread::yield();
        }
        q3.Complete(&env);
        EXPECT_TRUE(env.join(&q3));

        // Same-sequence tail q4 + qs0 before advancing to seq=1 (q5).
        ASSERT_TRUE(env.attach_queue(&q4, nullptr, "term_dom2", 0));
        ASSERT_TRUE(env.attach_queue(&qs0, nullptr, "term_dom2", 0));
        q4.ScheduleOne(&j4, &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
        qs0.ScheduleOne(&js0, &env, /*all_scheduled=*/false,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (done.load(std::memory_order_acquire) < 5) {
          std::this_thread::yield();
        }
        q4.Complete(&env);
        qs0.Complete(&env);
        ASSERT_TRUE(env.attach_queue(&q5, nullptr, "term_dom2", 1));
        EXPECT_TRUE(env.join(&q4));
        EXPECT_TRUE(env.join(&qs0));
        q5.ScheduleOne(&j5, &env, /*all_scheduled=*/false,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (done.load(std::memory_order_acquire) < 6) {
          std::this_thread::yield();
        }
        q5.Complete(&env);
        ASSERT_TRUE(env.attach_queue(&qs1, nullptr, "term_dom2", 2));
        EXPECT_TRUE(env.join(&q5));
        qs1.ScheduleOne(&js1, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (done.load(std::memory_order_acquire) < 7) {
          std::this_thread::yield();
        }
        qs1.Complete(&env);
        EXPECT_TRUE(env.join(&qs1));
        EXPECT_TRUE(env.destroy());
      }

      // 1a-race. Race Worker Complete(caller) (3 -> 1) against Owner
      // attach_queue(seq_idx = r + 1) to hit add_domain_sequence line 1176.
      {
        struct RaceTermJob final : public kdu_thread_job {
          RecordingThreadQueue* q = nullptr;
          std::atomic<bool>* started = nullptr;
          std::atomic<bool>* go = nullptr;
          int busy_iters = 0;
          RaceTermJob() { set_job_func(&RaceTermJob::Run); }
          static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
            auto* self = static_cast<RaceTermJob*>(base);
            self->started->store(true, std::memory_order_release);
            while (!self->go->load(std::memory_order_acquire)) {
              std::this_thread::yield();
            }
            for (volatile int i = 0; i < self->busy_iters; ++i) {
            }
            self->q->Complete(caller);
          }
        };
        struct NoopCountJob final : public kdu_thread_job {
          std::atomic<int>* done = nullptr;
          NoopCountJob() { set_job_func(&NoopCountJob::Run); }
          static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
            static_cast<NoopCountJob*>(base)->done->fetch_add(
                1, std::memory_order_release);
          }
        };
        // Keep <= 12 sequences concurrently active per thread entity so
        // active_sets stays within its inline 16-entry active_store (avoiding
        // Kakadu's >16 expansion leak), while keeping free_sequences == NULL
        // inside each group so add_domain_sequence hits `new` before line 1172.
        constexpr int kGroups = 12;
        constexpr int kPairs = 6;
        for (int g = 0; g < kGroups; ++g) {
          std::vector<std::unique_ptr<RecordingThreadQueue>> q_prevs(kPairs);
          std::vector<std::unique_ptr<RecordingThreadQueue>> q_nexts(kPairs);
          std::vector<RaceTermJob> race_jobs(kPairs);
          std::vector<NoopCountJob> next_jobs(kPairs);
          std::array<std::atomic<bool>, kPairs> started{};
          std::array<std::atomic<bool>, kPairs> go{};
          std::atomic<int> next_done{0};
          for (int r = 0; r < kPairs; ++r) {
            q_prevs[r] = std::make_unique<RecordingThreadQueue>(1, "race_prev");
            q_nexts[r] = std::make_unique<RecordingThreadQueue>(1, "race_next");
            race_jobs[r].q = q_prevs[r].get();
            race_jobs[r].started = &started[r];
            race_jobs[r].go = &go[r];
            race_jobs[r].busy_iters = ((g * kPairs + r) & 15) * 40;
            next_jobs[r].done = &next_done;
          }

          ScopedThreadEntity scoped;
          kdu_thread_entity& env = scoped.entity;
          env.create();
          ASSERT_TRUE(env.add_thread());
          WarmupWorkers(env);
          for (int r = 0; r < kPairs; ++r) {
            const kdu_long seq_a = static_cast<kdu_long>(2 * r);
            const kdu_long seq_b = seq_a + 1;
            ASSERT_TRUE(env.attach_queue(q_prevs[r].get(), nullptr, "race_term",
                                         seq_a));
            q_prevs[r]->ScheduleOne(&race_jobs[r], &env,
                                    /*all_scheduled=*/false,
                                    KDU_THREAD_JOB_AUTO_BIND_ONCE);
            while (!started[r].load(std::memory_order_acquire)) {
              std::this_thread::yield();
            }
            go[r].store(true, std::memory_order_release);
            ASSERT_TRUE(env.attach_queue(q_nexts[r].get(), nullptr, "race_term",
                                         seq_b));
            q_nexts[r]->ScheduleOne(&next_jobs[r], &env, /*all_scheduled=*/true,
                                    KDU_THREAD_JOB_AUTO_BIND_ONCE);
          }
          while (next_done.load(std::memory_order_acquire) < kPairs) {
            std::this_thread::yield();
          }
          for (int r = 0; r < kPairs; ++r) {
            EXPECT_TRUE(env.join(q_prevs[r].get()));
            q_nexts[r]->Complete(&env);
            EXPECT_TRUE(env.join(q_nexts[r].get()));
          }
          EXPECT_TRUE(env.destroy());
        }
      }

      // 1b. Deterministic check_working_waiters_idle (1503, 1512, 1520):
      // Owner holds SAFE_CONTEXT OwnerSpinJob while Worker 0 calls
      // wait_for_condition(..., false) -> finish_deadlock_test scans active
      // working waiter (Owner) and Worker 1 wakes Worker 0 cleanly.
      for (int r = 0; r < 8; ++r) {
        struct OwnerHoldJob final : public kdu_thread_job {
          RecordingThreadQueue* q = nullptr;
          std::atomic<bool>* owner_running = nullptr;
          std::atomic<bool>* worker_checked = nullptr;
          OwnerHoldJob() { set_job_func(&OwnerHoldJob::Run); }
          static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
            auto* self = static_cast<OwnerHoldJob*>(base);
            if (caller->is_group_owner()) {
              self->owner_running->store(true, std::memory_order_release);
              const auto deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(5);
              while (!self->worker_checked->load(std::memory_order_acquire) &&
                     std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
              }
            }
            self->q->Complete(caller);
          }
        };
        struct WakeCondJob final : public kdu_thread_job {
          kdu_thread_entity_condition* cond = nullptr;
          RecordingThreadQueue* q = nullptr;
          WakeCondJob() { set_job_func(&WakeCondJob::Run); }
          static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
            auto* self = static_cast<WakeCondJob*>(base);
            std::this_thread::sleep_for(std::chrono::microseconds(150));
            caller->signal_condition(self->cond, 0);
            self->q->Complete(caller);
          }
        };
        struct WorkerWaitJob final : public kdu_thread_job {
          RecordingThreadQueue* q_parent = nullptr;
          std::atomic<bool>* owner_running = nullptr;
          std::atomic<bool>* worker_checked = nullptr;
          WorkerWaitJob() { set_job_func(&WorkerWaitJob::Run); }
          static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
            auto* self = static_cast<WorkerWaitJob*>(base);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
            while (!self->owner_running->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
              std::this_thread::yield();
            }
            RecordingThreadQueue q_sig(1, "ww_idle_sig");
            caller->attach_queue(&q_sig, nullptr, "ww_sig_dom", 0);
            kdu_thread_entity_condition* cond = caller->get_condition();
            WakeCondJob wjob;
            wjob.cond = cond;
            wjob.q = &q_sig;
            q_sig.ScheduleOne(&wjob, caller, /*all_scheduled=*/true,
                              KDU_THREAD_JOB_AUTO_BIND_ONCE);
            caller->wait_for_condition("ww_idle_check",
                                       /*expect_foreign_wakeup=*/false);
            self->worker_checked->store(true, std::memory_order_release);
            caller->join(&q_sig);
            self->q_parent->Complete(caller);
          }
        };

        std::atomic<bool> owner_running{false};
        std::atomic<bool> worker_checked{false};
        RecordingThreadQueue q_hold(1, "ww_hold");
        RecordingThreadQueue q_worker(1, "ww_worker");
        OwnerHoldJob hold_job;
        hold_job.q = &q_hold;
        hold_job.owner_running = &owner_running;
        hold_job.worker_checked = &worker_checked;
        WorkerWaitJob worker_job;
        worker_job.q_parent = &q_worker;
        worker_job.owner_running = &owner_running;
        worker_job.worker_checked = &worker_checked;

        ScopedThreadEntity ww_scoped;
        kdu_thread_entity& wenv = ww_scoped.entity;
        wenv.create();
        ASSERT_TRUE(wenv.add_thread());
        ASSERT_TRUE(wenv.add_thread());
        WarmupWorkers(wenv);
        wenv.declare_first_owner_wait_safe(false);

        ASSERT_TRUE(wenv.attach_queue(&q_worker, nullptr, "ww_w_dom", r));
        ASSERT_TRUE(wenv.attach_queue(&q_hold, nullptr, "ww_h_dom", r,
                                      KDU_THREAD_QUEUE_SAFE_CONTEXT));
        q_worker.ScheduleOne(&worker_job, &wenv, /*all_scheduled=*/true,
                             KDU_THREAD_JOB_AUTO_BIND_ONCE);
        q_hold.ScheduleOne(&hold_job, &wenv, /*all_scheduled=*/true,
                           KDU_THREAD_JOB_AUTO_BIND_ONCE);
        EXPECT_TRUE(wenv.join(&q_worker));
        EXPECT_TRUE(wenv.join(&q_hold));
        EXPECT_TRUE(wenv.destroy());
      }
    });

    run_section("8b-10.2", [&] {
      // 2. Concurrent wake_non_waiting_workers & nww_idle_list CAS races
      // (1448, 1450, 1453, 1486, 1487, 1488).
      struct FanJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<int>* rem = nullptr;
        FanJob() { set_job_func(&FanJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<FanJob*>(base);
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q->Complete(caller);
          }
        }
      };
      struct GenJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        FanJob* children = nullptr;
        int child_count = 0;
        std::atomic<int>* rem = nullptr;
        std::atomic<int>* gens_scheduling_rem = nullptr;
        GenJob() { set_job_func(&GenJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<GenJob*>(base);
          for (int i = 0; i < self->child_count; ++i) {
            self->q->ScheduleOne(&self->children[i], caller,
                                 /*all_scheduled=*/false, /*options=*/0);
          }
          self->gens_scheduling_rem->fetch_sub(1, std::memory_order_release);
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q->Complete(caller);
          }
        }
      };

      constexpr int kGenCount = 4;
      constexpr int kChildrenPerGen = 8;
      constexpr int kOwnerChildren = 32;
      constexpr int kTotalChildren =
          kGenCount * kChildrenPerGen + kOwnerChildren;

      std::vector<GenJob> gen_jobs(kGenCount);
      std::vector<FanJob> children(kTotalChildren);

      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      for (int t = 0; t < 12; ++t) {
        ASSERT_TRUE(env.add_thread());
      }
      WarmupWorkers(env);

      for (int round = 0; round < 32; ++round) {
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
        RecordingThreadQueue q_single(2, "deep_sleep_wake");
        ASSERT_TRUE(env.attach_queue(&q_single, nullptr, "ds_wake_dom", round));
        std::atomic<int> ds_rem{3};
        FanJob ds_j0, ds_j1;
        ds_j0.q = &q_single;
        ds_j0.rem = &ds_rem;
        ds_j1.q = &q_single;
        ds_j1.rem = &ds_rem;
        // Fire 1 job first so wake_non_waiting_workers(1) pops all 12 parked
        // sleepers and CAS-pushes remainders back onto nww_idle_list, while a
        // worker-producer simultaneously schedules another wave.
        q_single.ScheduleOne(&ds_j0, &env, /*all_scheduled=*/false,
                             KDU_THREAD_JOB_AUTO_BIND_ONCE);
        q_single.ScheduleOne(&ds_j1, &env, /*all_scheduled=*/true,
                             KDU_THREAD_JOB_AUTO_BIND_ONCE);
        if (ds_rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          q_single.Complete(&env);
        }
        EXPECT_TRUE(env.join(&q_single));
      }

      for (int round = 0; round < 32; ++round) {
        if (round % 4 == 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(600));
        }
        RecordingThreadQueue q(kGenCount + kTotalChildren, "gen_race_q");
        ASSERT_TRUE(env.attach_queue(&q, nullptr, "gen_race_dom", round));
        std::atomic<int> rem{kGenCount + kTotalChildren + 1};
        std::atomic<int> gens_scheduling_rem{kGenCount};
        std::vector<kdu_thread_job*> all_c_ptrs(kTotalChildren);
        for (int i = 0; i < kTotalChildren; ++i) {
          children[i].q = &q;
          children[i].rem = &rem;
          all_c_ptrs[i] = &children[i];
        }
        std::array<kdu_thread_job*, kGenCount> gptrs{};
        for (int g = 0; g < kGenCount; ++g) {
          gen_jobs[g].q = &q;
          gen_jobs[g].children = &children[g * kChildrenPerGen];
          gen_jobs[g].child_count = kChildrenPerGen;
          gen_jobs[g].rem = &rem;
          gen_jobs[g].gens_scheduling_rem = &gens_scheduling_rem;
          gptrs[g] = &gen_jobs[g];
        }
        q.BindRange(gptrs.data(), kGenCount, 0);
        q.BindRange(all_c_ptrs.data(), kTotalChildren, kGenCount);
        q.ScheduleBatch(gptrs.data(), kGenCount, &env, /*all_scheduled=*/false);

        const int owner_base = kGenCount * kChildrenPerGen;
        for (int b = 0; b < kOwnerChildren; b += 4) {
          std::array<kdu_thread_job*, 4> optrs{
              &children[owner_base + b], &children[owner_base + b + 1],
              &children[owner_base + b + 2], &children[owner_base + b + 3]};
          q.ScheduleBatch(optrs.data(), 4, &env, /*all_scheduled=*/false);
          if ((b % 8) == 0) {
            std::this_thread::yield();
          }
        }
        while (gens_scheduling_rem.load(std::memory_order_acquire) > 0) {
          std::this_thread::yield();
        }
        q.NoteAllScheduled(&env);
        if (rem.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          q.Complete(&env);
        }
        EXPECT_TRUE(env.join(&q));
      }
      EXPECT_TRUE(env.destroy());
    });

    run_section("8b-10.3", [&] {
      // 3. Tight zero-sleep join vs Complete & signal_condition vs
      // wait_for_condition races (1503, 1512, 1520, 3522, 4175, 4178-4180).
      struct QuickJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<int>* rem = nullptr;
        QuickJob() { set_job_func(&QuickJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<QuickJob*>(base);
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q->Complete(caller);
          }
        }
      };
      struct SigJob final : public kdu_thread_job {
        kdu_thread_entity_condition* cond = nullptr;
        RecordingThreadQueue* q = nullptr;
        int yields = 0;
        SigJob() { set_job_func(&SigJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<SigJob*>(base);
          for (int y = 0; y < self->yields; ++y) {
            std::this_thread::yield();
          }
          caller->signal_condition(self->cond, 0);
          self->q->Complete(caller);
        }
      };

      std::array<QuickJob, 4> qjobs{};
      SigJob sig_job;
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      for (int t = 0; t < 4; ++t) {
        ASSERT_TRUE(env.add_thread());
      }
      WarmupWorkers(env);

      for (int wave = 0; wave < 512; ++wave) {
        env.declare_first_owner_wait_safe((wave % 2) == 0);
        const int njobs = ((wave % 3) == 0) ? 4 : 1;
        RecordingThreadQueue q(njobs, "quick_join");
        ASSERT_TRUE(env.attach_queue(&q, nullptr, "qj_dom", wave));
        std::atomic<int> rem{njobs};
        std::array<kdu_thread_job*, 4> ptrs{};
        for (int i = 0; i < njobs; ++i) {
          qjobs[i].q = &q;
          qjobs[i].rem = &rem;
          ptrs[i] = &qjobs[i];
        }
        q.BindRange(ptrs.data(), njobs, 0);
        q.ScheduleBatch(ptrs.data(), njobs, &env, /*all_scheduled=*/true);
        EXPECT_TRUE(env.join(&q));
      }

      for (int round = 0; round < 128; ++round) {
        env.declare_first_owner_wait_safe((round % 2) == 0);
        RecordingThreadQueue q_sig(1, "cond_sig");
        ASSERT_TRUE(env.attach_queue(&q_sig, nullptr, "csig_dom", round));
        kdu_thread_entity_condition* cond = env.get_condition();
        ASSERT_NE(nullptr, cond);
        sig_job.cond = cond;
        sig_job.q = &q_sig;
        sig_job.yields = round % 3;
        q_sig.ScheduleOne(&sig_job, &env, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
        env.wait_for_condition("cond_race", /*expect_foreign_wakeup=*/false);
        EXPECT_TRUE(env.join(&q_sig));
      }
      EXPECT_TRUE(env.destroy());

      // Exact NPROC=2 pair (1 worker + 1 owner) for join CAS (3522) and
      // cond_signalled wakeup races (4175, 4178-4180).
      QuickJob one_qjob;
      SigJob one_sjob;
      ScopedThreadEntity pair_scoped;
      kdu_thread_entity& penv = pair_scoped.entity;
      penv.create();
      ASSERT_TRUE(penv.add_thread());
      WarmupWorkers(penv);
      penv.declare_first_owner_wait_safe(false);
      for (int wave = 0; wave < 1024; ++wave) {
        RecordingThreadQueue q(1, "pair_quick_join");
        ASSERT_TRUE(penv.attach_queue(&q, nullptr, "pqj_dom", wave));
        std::atomic<int> rem{1};
        one_qjob.q = &q;
        one_qjob.rem = &rem;
        q.ScheduleOne(&one_qjob, &penv, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
        EXPECT_TRUE(penv.join(&q));
      }
      for (int round = 0; round < 128; ++round) {
        RecordingThreadQueue q_sig(1, "pair_cond_sig");
        ASSERT_TRUE(penv.attach_queue(&q_sig, nullptr, "pcsig_dom", round));
        kdu_thread_entity_condition* cond = penv.get_condition();
        ASSERT_NE(nullptr, cond);
        one_sjob.cond = cond;
        one_sjob.q = &q_sig;
        one_sjob.yields = round % 2;
        q_sig.ScheduleOne(&one_sjob, &penv, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
        penv.wait_for_condition("pair_cond_race",
                                /*expect_foreign_wakeup=*/false);
        EXPECT_TRUE(penv.join(&q_sig));
      }
      EXPECT_TRUE(penv.destroy());
    });

    run_section("8b-10.4", [&] {
      // 4. Low-thread-count (3 workers + Owner) continuous get_job hazard-ref
      // overlap against clean_dirty_palettes (1654, 1661-1663) with zero
      // inter-wave barrier stalls.
      struct BatchCountJob final : public kdu_thread_job {
        std::atomic<int>* done = nullptr;
        BatchCountJob() { set_job_func(&BatchCountJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
          auto* self = static_cast<BatchCountJob*>(base);
          self->done->fetch_add(1, std::memory_order_release);
        }
      };
      constexpr int kBuckets = 8;
      constexpr int kBucketJobs = 64;
      constexpr int kTotalJobs = kBuckets * kBucketJobs;
      std::vector<BatchCountJob> jobs(kTotalJobs);
      std::vector<kdu_thread_job*> ptrs(kTotalJobs);
      std::array<std::atomic<int>, kBuckets> done{};
      RecordingThreadQueue q_stream(kTotalJobs, "stream_palette_q");

      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      for (int t = 0; t < 3; ++t) {
        ASSERT_TRUE(env.add_thread());
      }
      WarmupWorkers(env);

      std::vector<std::unique_ptr<RecordingThreadQueue>> poll_qs;
      for (int p = 0; p < 10; ++p) {
        auto pq = std::make_unique<RecordingThreadQueue>(1, "batch_poll_dom");
        std::string pname = "bp_dom_" + std::to_string(p);
        ASSERT_TRUE(env.attach_queue(pq.get(), nullptr, pname.c_str(), p));
        poll_qs.push_back(std::move(pq));
      }
      ASSERT_TRUE(env.attach_queue(&q_stream, nullptr, "bp_stream_dom", 0));

      for (int b = 0; b < kBuckets; ++b) {
        for (int j = 0; j < kBucketJobs; ++j) {
          const int idx = b * kBucketJobs + j;
          jobs[idx].done = &done[b];
          ptrs[idx] = &jobs[idx];
        }
      }
      q_stream.BindRange(ptrs.data(), kTotalJobs, 0);
      for (int b = 0; b < kBuckets; ++b) {
        done[b].store(kBucketJobs, std::memory_order_relaxed);
      }
      for (int wave = 0; wave < 512; ++wave) {
        const int b = wave & (kBuckets - 1);
        while (done[b].load(std::memory_order_acquire) < kBucketJobs) {
          std::this_thread::yield();
        }
        done[b].store(0, std::memory_order_relaxed);
        q_stream.ScheduleBatch(&ptrs[b * kBucketJobs], kBucketJobs, &env,
                               /*all_scheduled=*/false);
      }
      for (int b = 0; b < kBuckets; ++b) {
        while (done[b].load(std::memory_order_acquire) < kBucketJobs) {
          std::this_thread::yield();
        }
      }
      q_stream.NoteAllScheduled(&env);
      q_stream.Complete(&env);
      EXPECT_TRUE(env.join(&q_stream));
      for (auto& pq : poll_qs) {
        pq->NoteAllScheduled(&env);
        pq->Complete(&env);
        EXPECT_TRUE(env.join(pq.get()));
      }
      EXPECT_TRUE(env.destroy());
    });

    // High-frequency clean_dirty_palettes contention across 16 workers and
    // multiple concurrent domains polling get_job while owner + workers churn
    // > 32 * KD_THREAD_PALETTES REBIND_0 schedules.
    struct ChurnJob final : public kdu_thread_job {
      std::atomic<int>* done = nullptr;
      ChurnJob() { set_job_func(&ChurnJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
        auto* self = static_cast<ChurnJob*>(base);
        self->done->fetch_add(1, std::memory_order_release);
      }
    };
    struct PollScheduleJob final : public kdu_thread_job {
      std::atomic<int>* sub_rem = nullptr;
      RecordingThreadQueue* outer_q = nullptr;
      std::string dom;
      PollScheduleJob() { set_job_func(&PollScheduleJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<PollScheduleJob*>(base);
        constexpr int kLocalQueues = 4;
        constexpr int kLocalRounds = 256;
        std::array<std::unique_ptr<RecordingThreadQueue>, kLocalQueues> qls;
        std::array<ChurnJob, kLocalQueues> sj{};
        std::array<std::atomic<int>, kLocalQueues> local_done{};
        int attached_cnt = 0;
        for (int m = 0; m < kLocalQueues; ++m) {
          qls[m] =
              std::make_unique<RecordingThreadQueue>(1, "worker_rebind_churn");
          std::string dname = self->dom + "_" + std::to_string(m);
          if (!caller->attach_queue(qls[m].get(), nullptr, dname.c_str(), m)) {
            break;
          }
          sj[m].done = &local_done[m];
          ++attached_cnt;
        }
        if (attached_cnt == kLocalQueues) {
          for (int r = 0; r < kLocalRounds; ++r) {
            for (int m = 0; m < kLocalQueues; ++m) {
              while (local_done[m].load(std::memory_order_acquire) < r) {
                std::this_thread::yield();
              }
              qls[m]->ScheduleOne(&sj[m], caller, /*all_scheduled=*/false,
                                  KDU_THREAD_JOB_REBIND_0);
            }
          }
          for (int m = 0; m < kLocalQueues; ++m) {
            while (local_done[m].load(std::memory_order_acquire) <
                   kLocalRounds) {
              std::this_thread::yield();
            }
          }
        }
        for (int m = 0; m < attached_cnt; ++m) {
          qls[m]->NoteAllScheduled(caller);
          qls[m]->Complete(caller);
          caller->join(qls[m].get());
        }
        if (self->sub_rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
          self->outer_q->Complete(caller);
        }
      }
    };

    constexpr int kOwnerQueues = 16;
    constexpr int kOwnerRounds = 512;
    constexpr int kHelpers = 4;
    std::array<ChurnJob, kOwnerQueues> owner_jobs{};
    std::array<std::atomic<int>, kOwnerQueues> owner_done{};
    for (int k = 0; k < kOwnerQueues; ++k) {
      owner_jobs[k].done = &owner_done[k];
    }
    std::array<PollScheduleJob, kHelpers> helpers{};
    std::atomic<int> helpers_rem{kHelpers};

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    for (int t = 0; t < 16; ++t) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);

    RecordingThreadQueue q_helpers(kHelpers, "helper_schedulers");
    std::array<std::unique_ptr<RecordingThreadQueue>, kOwnerQueues> q_owners{};
    std::vector<std::unique_ptr<RecordingThreadQueue>> idle_doms;
    for (int d = 0; d < 8; ++d) {
      auto q = std::make_unique<RecordingThreadQueue>(1, "poll_dom");
      std::string dname = "poll_dom_" + std::to_string(d);
      ASSERT_TRUE(env.attach_queue(q.get(), nullptr, dname.c_str(), d));
      idle_doms.push_back(std::move(q));
    }
    ASSERT_TRUE(env.attach_queue(&q_helpers, nullptr, "helpers_dom", 0));
    for (int k = 0; k < kOwnerQueues; ++k) {
      q_owners[k] =
          std::make_unique<RecordingThreadQueue>(1, "owner_rebind_churn");
      std::string dname = "owner_churn_dom_" + std::to_string(k);
      ASSERT_TRUE(
          env.attach_queue(q_owners[k].get(), nullptr, dname.c_str(), k + 1));
    }

    for (int h = 0; h < kHelpers; ++h) {
      helpers[h].sub_rem = &helpers_rem;
      helpers[h].outer_q = &q_helpers;
      helpers[h].dom = "sub_dom_" + std::to_string(h);
    }
    std::array<kdu_thread_job*, kHelpers> hptrs{};
    for (int h = 0; h < kHelpers; ++h) {
      hptrs[h] = &helpers[h];
    }
    q_helpers.BindRange(hptrs.data(), kHelpers, 0);
    q_helpers.ScheduleBatch(hptrs.data(), kHelpers, &env,
                            /*all_scheduled=*/true);

    for (int r = 0; r < kOwnerRounds; ++r) {
      for (int k = 0; k < kOwnerQueues; ++k) {
        while (owner_done[k].load(std::memory_order_acquire) < r) {
          std::this_thread::yield();
        }
        q_owners[k]->ScheduleOne(&owner_jobs[k], &env, /*all_scheduled=*/false,
                                 KDU_THREAD_JOB_REBIND_0);
      }
    }
    for (int k = 0; k < kOwnerQueues; ++k) {
      while (owner_done[k].load(std::memory_order_acquire) < kOwnerRounds) {
        std::this_thread::yield();
      }
      q_owners[k]->NoteAllScheduled(&env);
      q_owners[k]->Complete(&env);
      EXPECT_TRUE(env.join(q_owners[k].get()));
    }

    // Wait for worker helpers to complete `q_helpers` before `env.join`,
    // avoiding upstream group-owner `active_sets` leak in `destroy()`.
    while (helpers_rem.load(std::memory_order_acquire) > 0) {
      std::this_thread::yield();
    }
    for (auto& q : idle_doms) {
      q->NoteAllScheduled(&env);
      q->Complete(&env);
      EXPECT_TRUE(env.join(q.get()));
    }
    EXPECT_TRUE(env.join(&q_helpers));
    EXPECT_TRUE(env.destroy());
  }
  {
    const auto run_section = [&](const char* name, auto&& fn) {
      try {
        fn();
      } catch (kdu_exception exc) {
        ADD_FAILURE() << name << " threw kdu_exception " << exc << ": "
                      << error_handler_.TakeMessage();
      }
    };

    // (8b-10.5) Non-first active sequence KD_JOB_TERMINATOR (line 4495):
    // seq=0 stays active without all_scheduled while seq=1 finishes so Worker
    // polls seq=1 with !(seq_flags & 2) and receives KD_JOB_TERMINATOR.
    run_section("8b-10.5", [&] {
      struct FlagJob final : public kdu_thread_job {
        std::atomic<bool>* done = nullptr;
        FlagJob() { set_job_func(&FlagJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
          static_cast<FlagJob*>(base)->done->store(true,
                                                   std::memory_order_release);
        }
      };
      for (int round = 0; round < 8; ++round) {
        RecordingThreadQueue q0(1, "nf_seq0");
        RecordingThreadQueue q1(1, "nf_seq1");
        FlagJob job1;
        std::atomic<bool> q1_done{false};
        job1.done = &q1_done;

        ScopedThreadEntity scoped;
        kdu_thread_entity& env = scoped.entity;
        env.create();
        ASSERT_TRUE(env.add_thread());
        WarmupWorkers(env);
        ASSERT_TRUE(env.attach_queue(&q0, nullptr, "nf_dom", 0));
        ASSERT_TRUE(env.attach_queue(&q1, nullptr, "nf_dom", 1));
        q1.ScheduleOne(&job1, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (!q1_done.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
        q1.Complete(&env);
        q0.NoteAllScheduled(&env);
        q0.Complete(&env);
        EXPECT_TRUE(env.join(&q1));
        EXPECT_TRUE(env.join(&q0));
        EXPECT_TRUE(env.destroy());
      }
    });

    // (8b-10.6) Concurrent NoteAllScheduled vs ScheduleOne(all_scheduled=true)
    // (2266, 2274, 2275) and ScheduleBatch(all_scheduled=true) (2114, 2122,
    // 2123) using fine-grained busy-wait timing without scheduler yield stalls.
    run_section("8b-10.6", [&] {
      struct TriggerNoteJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<bool>* entered = nullptr;
        std::atomic<bool>* go = nullptr;
        std::atomic<bool>* note_done = nullptr;
        int busy_spins = 0;
        TriggerNoteJob() { set_job_func(&TriggerNoteJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<TriggerNoteJob*>(base);
          self->entered->store(true, std::memory_order_release);
          while (!self->go->load(std::memory_order_acquire)) {
          }
          for (volatile int s = 0; s < self->busy_spins; ++s) {
          }
          self->q->NoteAllScheduled(caller);
          self->note_done->store(true, std::memory_order_release);
        }
      };
      struct IncJob final : public kdu_thread_job {
        std::atomic<int>* count = nullptr;
        IncJob() { set_job_func(&IncJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
          static_cast<IncJob*>(base)->count->fetch_add(
              1, std::memory_order_release);
        }
      };

      // Part A: ScheduleOne 2048-hop auto-bind list vs Worker NoteAllScheduled.
      {
        constexpr int kPreNoops = 2046;
        constexpr int kTotalSlots = 1 + kPreNoops + 1;
        ScopedThreadEntity scoped;
        kdu_thread_entity& env = scoped.entity;
        env.create();
        ASSERT_TRUE(env.add_thread());
        WarmupWorkers(env);
        for (int r = 0; r < 160; ++r) {
          RecordingThreadQueue q(kTotalSlots, "so_race");
          TriggerNoteJob trig;
          std::vector<IncJob> noops(kPreNoops);
          IncJob last_job;
          std::atomic<bool> entered{false};
          std::atomic<bool> go{false};
          std::atomic<bool> note_done{false};
          std::atomic<int> noop_done{0};
          trig.q = &q;
          trig.entered = &entered;
          trig.go = &go;
          trig.note_done = &note_done;
          trig.busy_spins = (r < 64) ? (r * 100) : ((r - 64) * 1500);
          for (int i = 0; i < kPreNoops; ++i) {
            noops[i].count = &noop_done;
          }
          last_job.count = &noop_done;

          ASSERT_TRUE(env.attach_queue(&q, nullptr, "so_race_dom", r));
          q.ScheduleOne(&trig, &env, /*all_scheduled=*/false,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
          while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          for (int i = 0; i < kPreNoops; ++i) {
            q.ScheduleOne(&noops[i], &env, /*all_scheduled=*/false,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
          }
          go.store(true, std::memory_order_release);
          bool scheduled_last = true;
          try {
            q.ScheduleOne(&last_job, &env, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
          } catch (kdu_exception) {
            scheduled_last = false;
            error_handler_.TakeMessage();
          }
          const int expect_done = kPreNoops + (scheduled_last ? 1 : 0);
          while (!note_done.load(std::memory_order_acquire) ||
                 noop_done.load(std::memory_order_acquire) < expect_done) {
            std::this_thread::yield();
          }
          q.Complete(&env);
          EXPECT_TRUE(env.join(&q));
        }
        EXPECT_TRUE(env.destroy());
      }

      // Part B: ScheduleBatch vs Worker NoteAllScheduled (2114, 2122, 2123).
      {
        constexpr int kBatch = 8;
        constexpr int kTotalSlots = 1 + kBatch;
        ScopedThreadEntity scoped;
        kdu_thread_entity& env = scoped.entity;
        env.create();
        ASSERT_TRUE(env.add_thread());
        WarmupWorkers(env);
        for (int r = 0; r < 1024; ++r) {
          RecordingThreadQueue q(kTotalSlots, "sb_race");
          TriggerNoteJob trig;
          std::array<IncJob, kBatch> noops{};
          std::array<kdu_thread_job*, kTotalSlots> ptrs{};
          std::atomic<bool> entered{false};
          std::atomic<bool> go{false};
          std::atomic<bool> note_done{false};
          std::atomic<int> noop_done{0};
          trig.q = &q;
          trig.entered = &entered;
          trig.go = &go;
          trig.note_done = &note_done;
          trig.busy_spins = (r < 512) ? ((r & 127) * 2) : ((r & 127) * 8);
          ptrs[0] = &trig;
          for (int i = 0; i < kBatch; ++i) {
            noops[i].count = &noop_done;
            ptrs[1 + i] = &noops[i];
          }

          ASSERT_TRUE(env.attach_queue(&q, nullptr, "sb_race_dom", r));
          q.BindRange(ptrs.data(), kTotalSlots, 0);
          q.ScheduleBatch(&ptrs[0], 1, &env, /*all_scheduled=*/false);
          while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          go.store(true, std::memory_order_release);
          bool scheduled_batch = true;
          try {
            q.ScheduleBatch(&ptrs[1], kBatch, &env, /*all_scheduled=*/true);
          } catch (kdu_exception) {
            scheduled_batch = false;
            error_handler_.TakeMessage();
          }
          const int expect_done = scheduled_batch ? kBatch : 0;
          while (!note_done.load(std::memory_order_acquire) ||
                 noop_done.load(std::memory_order_acquire) < expect_done) {
            std::this_thread::yield();
          }
          q.Complete(&env);
          EXPECT_TRUE(env.join(&q));
        }
        EXPECT_TRUE(env.destroy());
      }
    });

    // (8b-10.7) Race Owner wait_for_condition(..., true) + foreign wakeup
    // thread against Worker wait_for_condition(..., false) ->
    // check_working_waiters_idle misses (1503, 1512, 1520) with zero deadlock
    // risk.
    run_section("8b-10.7", [&] {
      struct WorkerRaceJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<bool>* stop = nullptr;
        std::atomic<bool>* worker_done = nullptr;
        std::atomic<int>* owner_waiting = nullptr;
        std::atomic<kdu_thread_entity_condition*>* w_cond = nullptr;
        WorkerRaceJob() { set_job_func(&WorkerRaceJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<WorkerRaceJob*>(base);
          while (!self->stop->load(std::memory_order_acquire)) {
            while (!self->stop->load(std::memory_order_acquire) &&
                   self->owner_waiting->load(std::memory_order_acquire) == 0) {
              std::this_thread::yield();
            }
            if (self->stop->load(std::memory_order_acquire)) {
              break;
            }
            kdu_thread_entity_condition* cond = caller->get_condition();
            self->w_cond->store(cond, std::memory_order_release);
            caller->wait_for_condition("ww_idle_race",
                                       /*expect_foreign_wakeup=*/false);
          }
          self->w_cond->store(nullptr, std::memory_order_release);
          self->worker_done->store(true, std::memory_order_release);
          self->q->Complete(caller);
        }
      };

      std::atomic<bool> stop{false};
      std::atomic<bool> worker_done{false};
      std::atomic<int> owner_waiting{0};
      std::atomic<kdu_thread_entity_condition*> owner_cond{nullptr};
      std::atomic<kdu_thread_entity_condition*> worker_cond{nullptr};
      RecordingThreadQueue q_ww(1, "ww_race_q");
      WorkerRaceJob wjob;
      wjob.q = &q_ww;
      wjob.stop = &stop;
      wjob.worker_done = &worker_done;
      wjob.owner_waiting = &owner_waiting;
      wjob.w_cond = &worker_cond;

      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      ASSERT_TRUE(env.add_thread());
      WarmupWorkers(env);

      ASSERT_TRUE(env.attach_queue(&q_ww, nullptr, "ww_race_dom", 0));
      q_ww.ScheduleOne(&wjob, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);

      std::atomic<int> race_round{0};
      std::thread ext_waker([&] {
        while (!stop.load(std::memory_order_acquire)) {
          if (owner_waiting.load(std::memory_order_acquire) > 0) {
            kdu_thread_entity_condition* oc =
                owner_cond.load(std::memory_order_acquire);
            if (oc != nullptr) {
              const int cur_r = race_round.load(std::memory_order_relaxed);
              std::this_thread::sleep_for(
                  std::chrono::microseconds(2 + (cur_r & 31)));
              env.signal_condition(oc, 0);
            }
          } else {
            std::this_thread::yield();
          }
        }
      });

      for (int r = 0; r < 768; ++r) {
        race_round.store(r, std::memory_order_relaxed);
        kdu_thread_entity_condition* oc = env.get_condition();
        owner_cond.store(oc, std::memory_order_release);
        owner_waiting.store(1, std::memory_order_release);
        env.wait_for_condition("ext_idle_race", /*expect_foreign_wakeup=*/true);
        owner_waiting.store(0, std::memory_order_release);
        owner_cond.store(nullptr, std::memory_order_release);
        kdu_thread_entity_condition* wc =
            worker_cond.load(std::memory_order_acquire);
        if (wc != nullptr) {
          env.signal_condition(wc, 0);
        }
      }
      stop.store(true, std::memory_order_release);
      ext_waker.join();
      while (!worker_done.load(std::memory_order_acquire)) {
        kdu_thread_entity_condition* wc =
            worker_cond.load(std::memory_order_acquire);
        if (wc != nullptr) {
          env.signal_condition(wc, 0);
        }
        std::this_thread::yield();
      }
      EXPECT_TRUE(env.join(&q_ww));
      EXPECT_TRUE(env.destroy());
    });

    // (8b-10.8) find_preferred_bundle_count(63) and deterministic ww_return_job
    // wake_non_waiting_workers (4146) via domain-filtered working waiter +
    // idle NWW worker on another domain.
    run_section("8b-10.8", [&] {
      EXPECT_GE(kdu_thread_entity_affinity::find_preferred_bundle_count(63), 0);

      struct DomZWaitJob final : public kdu_thread_job {
        std::atomic<bool>* wait_gate = nullptr;
        std::atomic<bool>* in_wait_fn = nullptr;
        std::atomic<bool>* exited = nullptr;
        std::atomic<kdu_thread_entity_condition*>* cond_out = nullptr;
        DomZWaitJob() { set_job_func(&DomZWaitJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<DomZWaitJob*>(base);
          self->cond_out->store(caller->get_condition(),
                                std::memory_order_release);
          while (!self->wait_gate->load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          // Enters ww_process_jobs with J=4 ("domY"), A=0, T=0, I=1 ("domX"):
          // ww_reserve_job succeeds, "domZ" worker finds no job ->
          // ww_return_job wakes the idle NWW "domX" worker at 4146.
          self->in_wait_fn->store(true, std::memory_order_release);
          caller->wait_for_condition("ret_4146",
                                     /*expect_foreign_wakeup=*/false);
          self->exited->store(true, std::memory_order_release);
        }
      };
      struct CompleteOnLastJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<int>* rem = nullptr;
        CompleteOnLastJob() { set_job_func(&CompleteOnLastJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<CompleteOnLastJob*>(base);
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q->Complete(caller);
          }
        }
      };

      for (int r = 0; r < 4; ++r) {
        RecordingThreadQueue q_z(1, "domZ_q");
        RecordingThreadQueue q_y(4, "domY_q");
        DomZWaitJob job_z;
        std::array<CompleteOnLastJob, 4> jobs_y{};
        std::atomic<bool> wait_gate{false};
        std::atomic<bool> in_wait_fn{false};
        std::atomic<bool> exited{false};
        std::atomic<kdu_thread_entity_condition*> cond_z{nullptr};
        std::atomic<int> rem_y{4};
        job_z.wait_gate = &wait_gate;
        job_z.in_wait_fn = &in_wait_fn;
        job_z.exited = &exited;
        job_z.cond_out = &cond_z;

        ScopedThreadEntity scoped;
        kdu_thread_entity& env = scoped.entity;
        env.create();
        ASSERT_TRUE(env.add_thread("domX"));
        ASSERT_TRUE(env.add_thread("domZ"));

        ASSERT_TRUE(env.attach_queue(&q_z, nullptr, "domZ", r));
        q_z.ScheduleOne(&job_z, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (cond_z.load(std::memory_order_acquire) == nullptr) {
          std::this_thread::yield();
        }

        ASSERT_TRUE(env.attach_queue(&q_y, nullptr, "domY", r));
        std::array<kdu_thread_job*, 4> yptrs{};
        for (int i = 0; i < 4; ++i) {
          jobs_y[i].q = &q_y;
          jobs_y[i].rem = &rem_y;
          yptrs[i] = &jobs_y[i];
        }
        q_y.BindRange(yptrs.data(), 4, 0);
        q_y.ScheduleBatch(yptrs.data(), 4, &env, /*all_scheduled=*/true);
        // Let "domX" wake, poll empty, and return to NWW sleep (I=1, T=0).
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        wait_gate.store(true, std::memory_order_release);
        while (!in_wait_fn.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        env.signal_condition(cond_z.load(std::memory_order_acquire), 0);
        while (!exited.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        q_z.Complete(&env);
        EXPECT_TRUE(env.join(&q_z));
        // Owner executes the "domY" jobs during join so q_y completes cleanly.
        EXPECT_TRUE(env.join(&q_y));
        EXPECT_TRUE(env.destroy());
      }
    });

    // (8b-10.9) High-frequency REBIND_0 palette storm with 3 active workers
    // to reliably trigger clean_dirty_palettes hazard collisions (1654,
    // 1661-1663) and join CSTATE_W CAS retry (3522).
    run_section("8b-10.9", [&] {
      struct QuickRebindJob final : public kdu_thread_job {
        std::atomic<int>* done = nullptr;
        QuickRebindJob() { set_job_func(&QuickRebindJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
          static_cast<QuickRebindJob*>(base)->done->fetch_add(
              1, std::memory_order_release);
        }
      };
      struct BgChurnJob final : public kdu_thread_job {
        RecordingThreadQueue* q = nullptr;
        std::atomic<int>* rem = nullptr;
        BgChurnJob() { set_job_func(&BgChurnJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
          auto* self = static_cast<BgChurnJob*>(base);
          if (self->rem->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            self->q->Complete(caller);
          }
        }
      };
      constexpr int kStormBatches = 4;
      constexpr int kStormWavesPerBatch = 128;
      constexpr int kStormQueues = 4;
      constexpr int kBgJobs = 256;
      std::array<QuickRebindJob, kStormQueues> jobs{};
      std::array<std::atomic<int>, kStormQueues> done{};
      std::array<std::unique_ptr<RecordingThreadQueue>, kStormQueues> qs{};
      for (int k = 0; k < kStormQueues; ++k) {
        jobs[k].done = &done[k];
      }
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      for (int t = 0; t < 4; ++t) {
        ASSERT_TRUE(env.add_thread());
      }
      WarmupWorkers(env);
      for (int k = 0; k < kStormQueues; ++k) {
        qs[k] = std::make_unique<RecordingThreadQueue>(1, "storm_rebind");
        std::string dname = "storm_dom_" + std::to_string(k);
        ASSERT_TRUE(env.attach_queue(qs[k].get(), nullptr, dname.c_str(), k));
      }
      for (int b = 0; b < kStormBatches; ++b) {
        RecordingThreadQueue q_bg(kBgJobs, "storm_bg");
        std::vector<BgChurnJob> bg_jobs(kBgJobs);
        std::vector<kdu_thread_job*> bg_ptrs(kBgJobs);
        std::atomic<int> bg_rem{kBgJobs};
        for (int i = 0; i < kBgJobs; ++i) {
          bg_jobs[i].q = &q_bg;
          bg_jobs[i].rem = &bg_rem;
          bg_ptrs[i] = &bg_jobs[i];
        }
        ASSERT_TRUE(env.attach_queue(&q_bg, nullptr, "storm_bg_dom", b));
        q_bg.BindRange(bg_ptrs.data(), kBgJobs, 0);
        q_bg.ScheduleBatch(bg_ptrs.data(), kBgJobs, &env,
                           /*all_scheduled=*/true);
        const int base_w = b * kStormWavesPerBatch;
        for (int w = 0; w < kStormWavesPerBatch; ++w) {
          for (int k = 0; k < kStormQueues; ++k) {
            while (done[k].load(std::memory_order_acquire) < base_w + w) {
              std::this_thread::yield();
            }
            qs[k]->ScheduleOne(&jobs[k], &env, /*all_scheduled=*/false,
                               KDU_THREAD_JOB_REBIND_0);
          }
        }
        EXPECT_TRUE(env.join(&q_bg));
      }
      const int total_waves = kStormBatches * kStormWavesPerBatch;
      for (int k = 0; k < kStormQueues; ++k) {
        while (done[k].load(std::memory_order_acquire) < total_waves) {
          std::this_thread::yield();
        }
        qs[k]->NoteAllScheduled(&env);
        qs[k]->Complete(&env);
        EXPECT_TRUE(env.join(qs[k].get()));
      }
      EXPECT_TRUE(env.destroy());

      // Tight Owner join(&q) vs Worker Complete(caller) race (3522).
      {
        struct SelfCompleteJob final : public kdu_thread_job {
          RecordingThreadQueue* q = nullptr;
          SelfCompleteJob() { set_job_func(&SelfCompleteJob::Run); }
          static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
            static_cast<SelfCompleteJob*>(base)->q->Complete(caller);
          }
        };
        ScopedThreadEntity join_scoped;
        kdu_thread_entity& jenv = join_scoped.entity;
        jenv.create();
        ASSERT_TRUE(jenv.add_thread());
        WarmupWorkers(jenv);
        for (int r = 0; r < 512; ++r) {
          RecordingThreadQueue qj(1, "join_cas");
          SelfCompleteJob job;
          job.q = &qj;
          ASSERT_TRUE(jenv.attach_queue(&qj, nullptr, "jcas_dom", r));
          qj.ScheduleOne(&job, &jenv, /*all_scheduled=*/true,
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
          for (volatile int spin = 0; spin < (r & 127) * 500; ++spin) {
          }
          EXPECT_TRUE(jenv.join(&qj));
        }
        EXPECT_TRUE(jenv.destroy());
      }
    });
  }
}

TEST_F(KduThreadsTest, WorkingWaiterBackgroundJobReturn) {
  // 1. Deterministic line 4146 (`ww_return_job() > 0` wakeup in
  // `ww_process_jobs`): hold one worker active (A=1) while two extra workers
  // remain idle (I=2), schedule a `KDU_THREAD_QUEUE_BACKGROUND` queue (`J=4`),
  // and wait on Owner (`expect_foreign_wakeup=false`). Because `A=1`,
  // `ww_reserve_job` sets `allow_bkgnd_jobs=false`, `dequeue_general_job` skips
  // the background domain, and `ww_return_job` wakes one idle worker (`4146`).
  for (int round = 0; round < 6; ++round) {
    struct HoldState {
      RecordingThreadQueue* hold_q = nullptr;
      kdu_thread_entity_condition* owner_cond = nullptr;
      std::atomic<bool> hold_started{false};
      std::atomic<bool> about_to_wait{false};
      std::atomic<int> bg_done{0};
      std::atomic<int> bg_rem{3};
      std::atomic<bool> owner_done{false};
      std::atomic<bool> hold_exited{false};
    } st;

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    for (int w = 0; w < 3; ++w) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    RecordingThreadQueue hold_q(1, "hold4146");
    RecordingThreadQueue bg_q(3, "bg4146");
    ASSERT_TRUE(env.attach_queue(&hold_q, nullptr, "fg4146", 0, 0));
    ASSERT_TRUE(env.attach_queue(&bg_q, nullptr, "bg4146", 0,
                                 KDU_THREAD_QUEUE_BACKGROUND));

    st.hold_q = &hold_q;
    st.owner_cond = env.get_condition();

    struct HoldJob final : public kdu_thread_job {
      HoldState* s = nullptr;
      HoldJob() { set_job_func(&HoldJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        HoldState* s = static_cast<HoldJob*>(base)->s;
        s->hold_started.store(true, std::memory_order_release);
        while (!s->about_to_wait.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        s->hold_q->Complete(caller);
        while (!s->owner_done.load(std::memory_order_acquire)) {
          caller->signal_condition(s->owner_cond, 0);
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        s->hold_exited.store(true, std::memory_order_release);
      }
    } hold_job;
    hold_job.s = &st;

    struct BgJob final : public kdu_thread_job {
      HoldState* s = nullptr;
      RecordingThreadQueue* q = nullptr;
      BgJob() { set_job_func(&BgJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<BgJob*>(base);
        self->s->bg_done.fetch_add(1, std::memory_order_acq_rel);
        ReleaseGate(&self->s->bg_rem, self->q, caller);
      }
    };
    std::array<BgJob, 3> bg_jobs;
    std::array<kdu_thread_job*, 3> bg_ptrs{};
    for (int i = 0; i < 3; ++i) {
      bg_jobs[i].s = &st;
      bg_jobs[i].q = &bg_q;
      bg_ptrs[i] = &bg_jobs[i];
    }

    hold_q.ScheduleOne(&hold_job, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!st.hold_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    bg_q.BindRange(bg_ptrs.data(), 3, 0);
    bg_q.ScheduleBatch(bg_ptrs.data(), 3, &env, /*all_scheduled=*/true);
    st.about_to_wait.store(true, std::memory_order_release);
    env.wait_for_condition("owner_4146", /*expect_foreign_wakeup=*/false);
    st.owner_done.store(true, std::memory_order_release);
    while (!st.hold_exited.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    EXPECT_TRUE(env.join(&bg_q));
    EXPECT_TRUE(env.join(&hold_q));
    EXPECT_EQ(3, st.bg_done.load(std::memory_order_acquire));
    EXPECT_TRUE(env.join(nullptr));
  }
}

TEST_F(KduThreadsTest, WorkingWaiterPreSignalledEnterWait) {
  // 2. Lines 4178-4179 (`enter_wait()` when `cond` is pre-signalled while
  // waiter is still active, `ww_active == true`): worker 0 calls
  // `wait_for_condition` while Owner holds `W_0=1` executing `OwnerExecJob`.
  for (int round = 0; round < 72; ++round) {
    struct ActiveWaitState {
      RecordingThreadQueue* worker_q = nullptr;
      RecordingThreadQueue* owner_q = nullptr;
      kdu_thread_entity_condition* owner_cond = nullptr;
      std::atomic<kdu_thread_entity_condition*> worker_cond{nullptr};
      std::atomic<bool> worker_in_job{false};
      std::atomic<bool> owner_in_exec{false};
      std::atomic<bool> worker_calling_wait{false};
      std::atomic<bool> worker_woke{false};
      std::atomic<bool> owner_done{false};
      std::atomic<bool> worker_exited{false};
      int spins = 0;
      int sleep_us = 0;
    } st;
    if (round < 2) {
      st.sleep_us = 1000;
    } else if (round < 46) {
      st.spins = (round - 2) * 48;
    } else {
      st.spins = 2200 + (round - 46) * 1400;
    }

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);
    kdu_thread_entity_condition* owner_cond = env.get_condition();

    RecordingThreadQueue worker_q(1, "w4178");
    RecordingThreadQueue owner_q(1, "o4178");
    ASSERT_TRUE(env.attach_queue(&worker_q, nullptr, "dom_w4178", 0, 0));
    ASSERT_TRUE(env.attach_queue(&owner_q, nullptr, "dom_o4178", 0, 0));

    st.worker_q = &worker_q;
    st.owner_q = &owner_q;
    st.owner_cond = owner_cond;

    struct WorkerWait4178Job final : public kdu_thread_job {
      ActiveWaitState* s = nullptr;
      WorkerWait4178Job() { set_job_func(&WorkerWait4178Job::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        ActiveWaitState* s = static_cast<WorkerWait4178Job*>(base)->s;
        s->worker_cond.store(caller->get_condition(),
                             std::memory_order_release);
        s->worker_in_job.store(true, std::memory_order_release);
        for (int spin = 0; !s->owner_in_exec.load(std::memory_order_acquire);
             ++spin) {
          if ((spin & 2047) == 2047) {
            std::this_thread::yield();
          }
        }
        s->worker_calling_wait.store(true, std::memory_order_release);
        caller->wait_for_condition("w_cond_4178",
                                   /*expect_foreign_wakeup=*/false);
        s->worker_woke.store(true, std::memory_order_release);
        s->worker_q->Complete(caller);
        while (!s->owner_done.load(std::memory_order_acquire)) {
          caller->signal_condition(s->owner_cond, 0);
          std::this_thread::yield();
        }
        s->worker_exited.store(true, std::memory_order_release);
      }
    } worker_job;
    worker_job.s = &st;

    struct OwnerExec4178Job final : public kdu_thread_job {
      ActiveWaitState* s = nullptr;
      OwnerExec4178Job() { set_job_func(&OwnerExec4178Job::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        ActiveWaitState* s = static_cast<OwnerExec4178Job*>(base)->s;
        s->owner_in_exec.store(true, std::memory_order_release);
        for (int spin = 0;
             !s->worker_calling_wait.load(std::memory_order_acquire); ++spin) {
          if ((spin & 2047) == 2047) {
            std::this_thread::yield();
          }
        }
        if (s->sleep_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(s->sleep_us));
        } else {
          for (volatile int k = 0; k < s->spins; ++k) {
          }
        }
        kdu_thread_entity_condition* worker_cond =
            s->worker_cond.load(std::memory_order_acquire);
        while (!s->worker_woke.load(std::memory_order_acquire)) {
          caller->signal_condition(worker_cond, 0);
          std::this_thread::yield();
        }
        s->owner_q->Complete(caller);
      }
    } owner_job;
    owner_job.s = &st;

    worker_q.ScheduleOne(&worker_job, &env, /*all_scheduled=*/true,
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!st.worker_in_job.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    owner_q.ScheduleOne(&owner_job, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
    env.wait_for_condition("o_cond_4178", /*expect_foreign_wakeup=*/false);
    st.owner_done.store(true, std::memory_order_release);
    while (!st.worker_exited.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    EXPECT_TRUE(env.join(&owner_q));
    EXPECT_TRUE(env.join(&worker_q));
    EXPECT_TRUE(st.worker_woke.load(std::memory_order_acquire));
    EXPECT_TRUE(env.join(nullptr));
  }
}

TEST_F(KduThreadsTest, WorkingWaiterIdleRaceAndPaletteStorm) {
  // 3. Lines 1503, 1512, 1520 (`check_working_waiters_idle`): Owner and a
  // worker working-waiter (`wwid=1`) enter foreign wait loops concurrently
  // while a foreign thread wakes the worker across 0..31us delays so
  // `check_working_waiters_idle()` hits `num_idle==0` (`1503`), `tid < 0`
  // (`1512`), and `!cond->link` (`1520`).
  {
    kdu_thread_entity env;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue w_q(1, "w1512");
    ASSERT_TRUE(env.attach_queue(&w_q, nullptr, "dom1512", 0, 0));

    struct Wait1512State {
      RecordingThreadQueue* q = nullptr;
      kdu_thread_entity_condition* owner_cond = nullptr;
      std::atomic<kdu_thread_entity_condition*> worker_cond{nullptr};
      std::atomic<bool> worker_stop{false};
      std::atomic<bool> waker_stop{false};
      std::atomic<int> w_iter{0};
      std::atomic<int> w_done{0};
      std::atomic<int> o_iter{0};
      std::atomic<int> o_done{0};
      std::mutex o_sig_mu;
      bool stop_owner_signals = false;
    } st;
    st.q = &w_q;
    st.owner_cond = env.get_condition();

    struct Wait1512Job final : public kdu_thread_job {
      Wait1512State* s = nullptr;
      Wait1512Job() { set_job_func(&Wait1512Job::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        Wait1512State* s = static_cast<Wait1512Job*>(base)->s;
        s->worker_cond.store(caller->get_condition(),
                             std::memory_order_release);
        while (!s->worker_stop.load(std::memory_order_acquire)) {
          s->w_iter.fetch_add(1, std::memory_order_acq_rel);
          caller->wait_for_condition("w_1512", /*expect_foreign_wakeup=*/true);
          s->w_done.fetch_add(1, std::memory_order_acq_rel);
        }
        s->q->Complete(caller);
      }
    } w_job;
    w_job.s = &st;
    w_q.ScheduleOne(&w_job, &env, /*all_scheduled=*/true,
                    KDU_THREAD_JOB_AUTO_BIND_ONCE);
    // Ensure a worker dequeues w_job before main enters wait_for_condition,
    // preventing main from self-executing Wait1512Job and deadlocking.
    while (st.worker_cond.load(std::memory_order_acquire) == nullptr) {
      std::this_thread::yield();
    }

    std::thread ext_waker([&] {
      int spins = 0;
      while (!st.waker_stop.load(std::memory_order_acquire)) {
        kdu_thread_entity_condition* wc =
            st.worker_cond.load(std::memory_order_acquire);
        if (wc != nullptr && st.w_iter.load(std::memory_order_acquire) >
                                 st.w_done.load(std::memory_order_acquire)) {
          env.signal_condition(wc, KDU_FOREIGN_SIGNAL_EXPECTED);
        }
        for (volatile int s = 0; s < ((spins++ & 31) * 20); ++s) {
        }
        {
          std::lock_guard<std::mutex> lk(st.o_sig_mu);
          if (!st.stop_owner_signals &&
              st.o_iter.load(std::memory_order_acquire) >
                  st.o_done.load(std::memory_order_acquire)) {
            env.signal_condition(st.owner_cond, KDU_FOREIGN_SIGNAL_EXPECTED);
          }
        }
        if ((spins & 7) == 0) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(1 + (spins & 7)));
        } else if ((spins & 3) == 0) {
          std::this_thread::yield();
        }
      }
    });

    for (int r = 0; r < 1600; ++r) {
      st.o_iter.fetch_add(1, std::memory_order_acq_rel);
      env.wait_for_condition("o_1512", /*expect_foreign_wakeup=*/true);
      st.o_done.fetch_add(1, std::memory_order_acq_rel);
    }
    {
      std::lock_guard<std::mutex> lk(st.o_sig_mu);
      st.stop_owner_signals = true;
    }
    st.worker_stop.store(true, std::memory_order_release);
    EXPECT_TRUE(env.join(&w_q));
    st.waker_stop.store(true, std::memory_order_release);
    ext_waker.join();
    EXPECT_TRUE(env.join(nullptr));
  }

  // 4. Lines 1639, 1653, 1654, 1661-1663 (`clean_dirty_palettes` Phase 2
  // hazard collision): keep 6 workers dequeuing across two alternating queues
  // with `> KD_THREAD_PALETTES` (`160`) individually-scheduled palettes per
  // wave so un-dequeued palettes force Phase 2 while workers hold `hzp->ref`.
  {
    kdu_thread_entity env;
    env.create();
    ASSERT_TRUE(env.exists());
    for (int i = 0; i < 6; ++i) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);

    constexpr int kWaveJobs = 192;
    constexpr int kWavesPerBank = 20;
    RecordingThreadQueue q0(kWaveJobs * kWavesPerBank, "storm0");
    RecordingThreadQueue q1(kWaveJobs * kWavesPerBank, "storm1");
    ASSERT_TRUE(env.attach_queue(&q0, nullptr, "storm1654", 0, 0));
    ASSERT_TRUE(env.attach_queue(&q1, nullptr, "storm1654", 0, 0));

    struct StormJob final : public kdu_thread_job {
      std::atomic<int>* rem = nullptr;
      std::atomic<int>* total_rem = nullptr;
      StormJob() { set_job_func(&StormJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
        auto* self = static_cast<StormJob*>(base);
        self->rem->fetch_sub(1, std::memory_order_release);
        self->total_rem->fetch_sub(1, std::memory_order_release);
      }
    };
    std::array<std::vector<StormJob>, 2> banks;
    banks[0].resize(kWaveJobs * kWavesPerBank);
    banks[1].resize(kWaveJobs * kWavesPerBank);
    std::array<std::atomic<int>, 2> bank_rem{0, 0};
    std::array<std::atomic<int>, 2> total_rem{kWaveJobs * kWavesPerBank,
                                              kWaveJobs * kWavesPerBank};
    std::array<RecordingThreadQueue*, 2> qs{&q0, &q1};

    for (int b = 0; b < 2; ++b) {
      for (int i = 0; i < kWaveJobs * kWavesPerBank; ++i) {
        banks[b][i].rem = &bank_rem[b];
        banks[b][i].total_rem = &total_rem[b];
      }
    }

    for (int p = 0; p < 2 * kWavesPerBank; ++p) {
      const int b = p & 1;
      const int wave_in_bank = p >> 1;
      while (bank_rem[b].load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
      }
      bank_rem[b].store(kWaveJobs, std::memory_order_release);
      const bool last_wave = (wave_in_bank == kWavesPerBank - 1);
      for (int j = 0; j < kWaveJobs; ++j) {
        const bool all_sched = last_wave && (j == kWaveJobs - 1);
        qs[b]->ScheduleOne(&banks[b][wave_in_bank * kWaveJobs + j], &env,
                           all_sched, KDU_THREAD_JOB_AUTO_BIND_ONCE);
      }
    }
    for (int b = 0; b < 2; ++b) {
      while (total_rem[b].load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
      }
      qs[b]->Complete(&env);
      EXPECT_TRUE(env.join(qs[b]));
    }
    EXPECT_TRUE(env.join(nullptr));
  }
}

TEST_F(KduThreadsTest, ScheduleNoteAllScheduledRace) {
  // Dense micro-spin sweeps for `schedule_one` (`2266, 2274, 2275`) and
  // `schedule_jobs` (`2114, 2122, 2123`) racing with `note_all_scheduled`.
  {
    kdu_thread_entity env;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    for (int mode = 0; mode < 2; ++mode) {
      for (int r = 0; r < 1600; ++r) {
        RecordingThreadQueue q(2, mode == 0 ? "r_one" : "r_batch");
        ASSERT_TRUE(env.attach_queue(&q, nullptr, "r_sched", 0, 0));

        struct TrigNoteJob final : public kdu_thread_job {
          RecordingThreadQueue* q = nullptr;
          std::atomic<bool> started{false};
          std::atomic<bool> go{false};
          std::atomic<bool> done{false};
          int spins = 0;
          TrigNoteJob() { set_job_func(&TrigNoteJob::Run); }
          static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
            auto* self = static_cast<TrigNoteJob*>(base);
            self->started.store(true, std::memory_order_release);
            for (int w = 0; !self->go.load(std::memory_order_acquire); ++w) {
              if ((w & 2047) == 2047) {
                std::this_thread::yield();
              }
            }
            for (volatile int s = 0; s < self->spins; ++s) {
            }
            self->q->NoteAllScheduled(caller);
            self->done.store(true, std::memory_order_release);
          }
        } trig;
        trig.q = &q;
        const int sweep_spins = ((r >> 1) & 63) * 2;
        trig.spins = (r & 1) ? sweep_spins : 0;

        struct DoneJob final : public kdu_thread_job {
          std::atomic<bool> ran{false};
          DoneJob() { set_job_func(&DoneJob::Run); }
          static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
            static_cast<DoneJob*>(base)->ran.store(true,
                                                   std::memory_order_release);
          }
        } race;

        q.ScheduleOne(&trig, &env, /*all_scheduled=*/false,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
        while (!trig.started.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        kdu_thread_job* ptr = &race;
        if (mode == 1) {
          q.BindRange(&ptr, 1, 0);
        }
        trig.go.store(true, std::memory_order_release);
        if ((r & 1) == 0) {
          for (volatile int s = 0; s < sweep_spins; ++s) {
          }
        }

        bool threw = false;
        try {
          if (mode == 0) {
            q.ScheduleOne(&race, &env, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
          } else {
            q.ScheduleBatch(&ptr, 1, &env, /*all_scheduled=*/true);
          }
        } catch (kdu_exception) {
          threw = true;
        }

        while (!trig.done.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        if (!threw) {
          while (!race.ran.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
        }
        q.Complete(&env);
        EXPECT_TRUE(env.join(&q));
      }
    }
    error_handler_.TakeMessage();
    EXPECT_TRUE(env.join(nullptr));
  }
}

TEST_F(KduThreadsTest, DeterministicThreadEdgeCases) {
  // 1) handle_exception on a non-existing entity returns cleanly (line 3649).
  {
    kdu_thread_entity non_existent;
    EXPECT_FALSE(non_existent.exists());
    non_existent.handle_exception(KDU_ERROR_EXCEPTION);
  }

  // 2) Completing seq=1 with zero consumers on the tail before attaching seq=2
  // triggers `old_tail->terminate(caller->hzp)` in `add_domain_sequence`
  // (`old_active_state == 1`, line 1176).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());

    ZeroJobsAutoCompleteQueue root_q;
    RecordingThreadQueue q0(1, "d1176_q0");
    RecordingThreadQueue q1(1, "d1176_q1");
    RecordingThreadQueue q2(1, "d1176_q2");
    ASSERT_TRUE(env.attach_queue(&root_q, nullptr, nullptr, 0, 0));
    ASSERT_TRUE(env.attach_queue(&q0, &root_q, "d1176", 0, 0));
    ASSERT_TRUE(env.attach_queue(&q1, &q0, "d1176", 1, 0));
    q1.Complete(&env);
    ASSERT_TRUE(env.attach_queue(&q2, &q0, "d1176", 2, 0));
    q2.Complete(&env);
    q0.Complete(&env);
    EXPECT_TRUE(env.join(nullptr));
  }

  // 3) Drain `free_palettes`, return completed palettes via `release_palettes`,
  // and spawn extra workers so `add_thread()` exercises dirty/clean sorting.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    struct SimpleJob final : public kdu_thread_job {
      std::atomic<int>* done = nullptr;
      SimpleJob() { set_job_func(&SimpleJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
        static_cast<SimpleJob*>(base)->done->fetch_add(
            1, std::memory_order_relaxed);
      }
    };

    constexpr int kJobsPerWave = 14;
    std::atomic<int> done{0};
    for (int wave = 0; wave < 12; ++wave) {
      RecordingThreadQueue q(kJobsPerWave, "ret_plt");
      ASSERT_TRUE(env.attach_queue(&q, nullptr, "ret_dom", 0, 0));
      std::array<SimpleJob, kJobsPerWave> jobs;
      std::array<kdu_thread_job*, kJobsPerWave> ptrs;
      for (int i = 0; i < kJobsPerWave; ++i) {
        jobs[i].done = &done;
        ptrs[i] = &jobs[i];
      }
      q.BindRange(ptrs.data(), kJobsPerWave, 0);
      q.ScheduleBatch(ptrs.data(), kJobsPerWave, &env, /*all_scheduled=*/true);
      while (done.load(std::memory_order_acquire) < (wave + 1) * kJobsPerWave) {
        std::this_thread::yield();
      }
      q.Complete(&env);
      EXPECT_TRUE(env.join(&q));
    }
    for (int w = 0; w < 4; ++w) {
      ASSERT_TRUE(env.add_thread());
    }
    EXPECT_TRUE(env.join(nullptr));
    EXPECT_EQ(12 * kJobsPerWave, done.load());
  }
}

TEST_F(KduThreadsTest, ScheduleAllScheduledWarningRace) {
  // Multi-worker cache-isolated start-gate race hitting `KDU_WARNING_DEV`
  // (`0x17051201` / `0x17051202`, `old_state == new_state` in `schedule_jobs`
  // & `schedule_job`) directly on `kdu_thread_queue` (zero wrapper locking).
  struct DirectRaceQueue final : public kdu_thread_queue {
    int max_jobs = 3;
    explicit DirectRaceQueue(int mj = 3) : max_jobs(mj) {}
    int get_max_jobs() override { return max_jobs; }
    void BindJobs(kdu_thread_job** jobs, int count) {
      bind_jobs(jobs, count, 0);
    }
    void SchedOne(kdu_thread_job* job, kdu_thread_entity* caller,
                  bool all_scheduled) {
      schedule_job(job, caller, all_scheduled, 0);
    }
    void SchedBatch(kdu_thread_job** jobs, int count, kdu_thread_entity* caller,
                    bool all_scheduled) {
      schedule_jobs(jobs, count, caller, all_scheduled);
    }
    void NoteAll(kdu_thread_entity* caller) { note_all_scheduled(caller); }
    void Complete(kdu_thread_entity* caller) { all_done(caller); }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(env.add_thread());
  }
  WarmupWorkers(env);
  env.set_yield_frequency(0);

  struct alignas(64) SyncState {
    alignas(64) std::atomic<int> ack0{0};
    alignas(64) std::atomic<int> ack1{0};
    alignas(64) std::atomic<int> go{0};
    alignas(64) std::atomic<int> done0{0};
    alignas(64) std::atomic<int> done1{0};
  };

  struct NoOpJob final : public kdu_thread_job {
    std::atomic<int>* done_count = nullptr;
    NoOpJob() { set_job_func(&NoOpJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
      static_cast<NoOpJob*>(base)->done_count->fetch_add(
          1, std::memory_order_release);
    }
  };

  constexpr int kRounds = 8;
  constexpr int kQueues = 64;

  struct CtrlJob final : public kdu_thread_job {
    int worker_idx = 0;
    int mode = 0;
    int num_queues = 0;
    SyncState* sync = nullptr;
    std::deque<DirectRaceQueue>* qs = nullptr;
    kdu_thread_job** job_ptrs = nullptr;
    std::atomic<int>* expected_counts = nullptr;
    CtrlJob() { set_job_func(&CtrlJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<CtrlJob*>(base);
      auto& ack = (self->worker_idx == 0) ? self->sync->ack0 : self->sync->ack1;
      auto& done =
          (self->worker_idx == 0) ? self->sync->done0 : self->sync->done1;
      for (int qi = 0; qi < self->num_queues; ++qi) {
        ack.store(qi + 1, std::memory_order_release);
        while (self->sync->go.load(std::memory_order_acquire) < qi + 1) {
        }
        int ws = 0;
        if (self->mode < 2) {
          ws = (self->worker_idx == 0) ? (qi < 32 ? qi : 0)
                                       : (qi >= 32 ? qi - 32 : 0);
        } else if (self->worker_idx == 0) {
          ws = (qi >= 32) ? (qi - 32) * 2 : 0;
        }
        volatile int local_sink = 0;
        for (int k = 0; k < ws; ++k) {
          local_sink = local_sink + k;
        }
        (void)local_sink;
        bool threw = false;
        try {
          if (self->mode == 0) {
            (*self->qs)[qi].SchedOne(self->job_ptrs[qi], caller,
                                     /*all_scheduled=*/true);
          } else if (self->mode == 1) {
            kdu_thread_job* p = self->job_ptrs[qi];
            (*self->qs)[qi].SchedBatch(&p, 1, caller, /*all_scheduled=*/true);
          } else if (self->worker_idx == 0) {
            (*self->qs)[qi].NoteAll(caller);
          }
        } catch (kdu_exception) {
          threw = true;
        }
        if (self->mode < 2 && !threw) {
          self->expected_counts[qi].fetch_add(1, std::memory_order_relaxed);
        }
        done.store(qi + 1, std::memory_order_release);
      }
    }
  };

  for (int round = 0; round < kRounds; ++round) {
    const int mode = round % 4;
    std::deque<DirectRaceQueue> qs;
    std::vector<NoOpJob> jobs0(kQueues);
    std::vector<NoOpJob> jobs1(kQueues);
    std::vector<NoOpJob> jobs2(kQueues);
    std::vector<kdu_thread_job*> w0_ptrs(kQueues);
    std::vector<kdu_thread_job*> w1_ptrs(kQueues);
    std::vector<kdu_thread_job*> own_ptrs(kQueues);
    std::vector<std::atomic<int>> done_counts(kQueues);
    std::vector<std::atomic<int>> expected_counts(kQueues);
    for (int qi = 0; qi < kQueues; ++qi) {
      done_counts[qi].store(0, std::memory_order_relaxed);
      expected_counts[qi].store(0, std::memory_order_relaxed);
      jobs0[qi].done_count = &done_counts[qi];
      jobs1[qi].done_count = &done_counts[qi];
      jobs2[qi].done_count = &done_counts[qi];
      if (mode < 2) {
        qs.emplace_back(3);
        ASSERT_TRUE(env.attach_queue(&qs.back(), nullptr, "warn_dom", 0, 0));
        kdu_thread_job* bind3[3] = {&jobs0[qi], &jobs1[qi], &jobs2[qi]};
        qs.back().BindJobs(bind3, 3);
      } else {
        qs.emplace_back(1);
        ASSERT_TRUE(env.attach_queue(&qs.back(), nullptr, "warn_dom", 0, 0));
        kdu_thread_job* bind1[1] = {&jobs2[qi]};
        qs.back().BindJobs(bind1, 1);
      }
      w0_ptrs[qi] = &jobs0[qi];
      w1_ptrs[qi] = &jobs1[qi];
      own_ptrs[qi] = &jobs2[qi];
    }

    SyncState sync;
    CtrlJob ctrl[2];
    ctrl[0].worker_idx = 0;
    ctrl[0].job_ptrs = w0_ptrs.data();
    ctrl[1].worker_idx = 1;
    ctrl[1].job_ptrs = w1_ptrs.data();
    for (int c = 0; c < 2; ++c) {
      ctrl[c].mode = mode;
      ctrl[c].num_queues = kQueues;
      ctrl[c].sync = &sync;
      ctrl[c].qs = &qs;
      ctrl[c].expected_counts = expected_counts.data();
    }

    DirectRaceQueue q_ctrl(2);
    ASSERT_TRUE(env.attach_queue(&q_ctrl, nullptr, "ctrl_dom", 0, 0));
    kdu_thread_job* ctrl_ptrs[2] = {&ctrl[0], &ctrl[1]};
    q_ctrl.BindJobs(ctrl_ptrs, 2);
    q_ctrl.SchedBatch(ctrl_ptrs, 2, &env, /*all_scheduled=*/true);

    for (int qi = 0; qi < kQueues; ++qi) {
      while (sync.ack0.load(std::memory_order_acquire) < qi + 1 ||
             sync.ack1.load(std::memory_order_acquire) < qi + 1) {
        std::this_thread::yield();
      }
      sync.go.store(qi + 1, std::memory_order_release);
      const int os = (mode < 2) ? (qi & 7) * 4 : (qi < 32 ? qi * 2 : 0);
      volatile int local_sink = 0;
      for (int k = 0; k < os; ++k) {
        local_sink = local_sink + k;
      }
      (void)local_sink;
      bool o_threw = false;
      try {
        if ((mode & 1) == 0) {
          qs[qi].SchedOne(own_ptrs[qi], &env, /*all_scheduled=*/true);
        } else {
          kdu_thread_job* p = own_ptrs[qi];
          qs[qi].SchedBatch(&p, 1, &env, /*all_scheduled=*/true);
        }
      } catch (kdu_exception) {
        o_threw = true;
      }
      if (!o_threw) {
        expected_counts[qi].fetch_add(1, std::memory_order_relaxed);
      }
      while (sync.done0.load(std::memory_order_acquire) < qi + 1 ||
             sync.done1.load(std::memory_order_acquire) < qi + 1) {
        std::this_thread::yield();
      }
    }

    q_ctrl.Complete(&env);
    EXPECT_TRUE(env.join(&q_ctrl));
    for (int qi = 0; qi < kQueues; ++qi) {
      const int exp = expected_counts[qi].load(std::memory_order_acquire);
      while (done_counts[qi].load(std::memory_order_acquire) < exp) {
        std::this_thread::yield();
      }
      qs[qi].Complete(&env);
      EXPECT_TRUE(env.join(&qs[qi]));
    }
    error_handler_.TakeMessage();
  }
  EXPECT_TRUE(env.join(nullptr));
}

TEST_F(KduThreadsTest, ConcurrentWakeAndDirtyPaletteChurn) {
  // Owner + 2 worker spawners concurrently schedule small waves (hitting
  // `wake_non_waiting_workers` 1450, 1486-1488), then run uninterrupted burst
  // batches alongside independent wave queues joined concurrently by Owner and
  // workers so `clean_dirty_palettes` wraps `dirty_head` (`1617`) while both
  // workers and working-waiter callers hold hazard pointers (`1639`, `1649`).
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(env.add_thread());
  }
  WarmupWorkers(env);
  env.set_yield_frequency(0);

  constexpr int kSmallWaves = 64;
  constexpr int kSmallBatch = 14;
  constexpr int kBurstBatches = 500;
  constexpr int kBurstBatch = 14;
  constexpr int kWaveQueues = 32;
  constexpr int kWaveJobs = 112;
  constexpr int kJobsPerProducer =
      kSmallWaves * kSmallBatch + kBurstBatches * kBurstBatch;

  struct ChurnJob final : public kdu_thread_job {
    std::atomic<int>* rem = nullptr;
    ChurnJob() { set_job_func(&ChurnJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
      static_cast<ChurnJob*>(base)->rem->fetch_sub(1,
                                                   std::memory_order_acq_rel);
    }
  };

  struct CountJob final : public kdu_thread_job {
    std::atomic<int>* count = nullptr;
    std::atomic<int>* per_wave = nullptr;
    CountJob() { set_job_func(&CountJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
      auto* self = static_cast<CountJob*>(base);
      self->count->fetch_add(1, std::memory_order_relaxed);
      self->per_wave->fetch_add(1, std::memory_order_release);
    }
  };

  RecordingThreadQueue q_ctrl(2, "churn_ctrl");
  RecordingThreadQueue q_owner(kJobsPerProducer, "churn_owner");
  RecordingThreadQueue q_spawner0(kJobsPerProducer, "churn_spawner0");
  RecordingThreadQueue q_spawner1(kJobsPerProducer, "churn_spawner1");
  ASSERT_TRUE(env.attach_queue(&q_ctrl, nullptr, "ctrl_dom", 0, 0));
  ASSERT_TRUE(env.attach_queue(&q_owner, nullptr, "churn_dom", 0, 0));
  ASSERT_TRUE(env.attach_queue(&q_spawner0, nullptr, "churn_dom", 0, 0));
  ASSERT_TRUE(env.attach_queue(&q_spawner1, nullptr, "churn_dom", 0, 0));

  std::deque<RecordingThreadQueue> own_wave_qs;
  std::deque<RecordingThreadQueue> s0_wave_qs;
  std::deque<RecordingThreadQueue> s1_wave_qs;
  std::atomic<int> wave_job_count{0};
  std::vector<std::atomic<int>> own_wave_done(kWaveQueues);
  std::vector<std::atomic<int>> s0_wave_done(kWaveQueues);
  std::vector<std::atomic<int>> s1_wave_done(kWaveQueues);
  std::vector<CountJob> own_w_jobs(kWaveQueues * kWaveJobs);
  std::vector<CountJob> s0_w_jobs(kWaveQueues * kWaveJobs);
  std::vector<CountJob> s1_w_jobs(kWaveQueues * kWaveJobs);
  std::vector<kdu_thread_job*> own_w_ptrs(kWaveQueues * kWaveJobs);
  std::vector<kdu_thread_job*> s0_w_ptrs(kWaveQueues * kWaveJobs);
  std::vector<kdu_thread_job*> s1_w_ptrs(kWaveQueues * kWaveJobs);
  for (int w = 0; w < kWaveQueues; ++w) {
    own_wave_done[w].store(0, std::memory_order_relaxed);
    s0_wave_done[w].store(0, std::memory_order_relaxed);
    s1_wave_done[w].store(0, std::memory_order_relaxed);
  }
  for (int i = 0; i < kWaveQueues * kWaveJobs; ++i) {
    const int w = i / kWaveJobs;
    own_w_jobs[i].count = &wave_job_count;
    own_w_jobs[i].per_wave = &own_wave_done[w];
    s0_w_jobs[i].count = &wave_job_count;
    s0_w_jobs[i].per_wave = &s0_wave_done[w];
    s1_w_jobs[i].count = &wave_job_count;
    s1_w_jobs[i].per_wave = &s1_wave_done[w];
    own_w_ptrs[i] = &own_w_jobs[i];
    s0_w_ptrs[i] = &s0_w_jobs[i];
    s1_w_ptrs[i] = &s1_w_jobs[i];
  }
  for (int w = 0; w < kWaveQueues; ++w) {
    own_wave_qs.emplace_back(kWaveJobs, "own_w");
    ASSERT_TRUE(env.attach_queue(&own_wave_qs.back(), nullptr, "w_own", 0, 0));
    own_wave_qs.back().BindRange(&own_w_ptrs[w * kWaveJobs], kWaveJobs, 0);
    s0_wave_qs.emplace_back(kWaveJobs, "s0_w");
    ASSERT_TRUE(env.attach_queue(&s0_wave_qs.back(), nullptr, "w_s0", 0, 0));
    s0_wave_qs.back().BindRange(&s0_w_ptrs[w * kWaveJobs], kWaveJobs, 0);
    s1_wave_qs.emplace_back(kWaveJobs, "s1_w");
    ASSERT_TRUE(env.attach_queue(&s1_wave_qs.back(), nullptr, "w_s1", 0, 0));
    s1_wave_qs.back().BindRange(&s1_w_ptrs[w * kWaveJobs], kWaveJobs, 0);
  }

  std::atomic<int> wave_go{0};
  std::atomic<int> wave_rem{0};
  std::atomic<bool> burst_go{false};
  std::atomic<bool> spawner0_done{false};
  std::atomic<bool> spawner1_done{false};
  std::atomic<int> completed_total{0};

  std::vector<ChurnJob> o_jobs(kJobsPerProducer);
  std::vector<kdu_thread_job*> o_ptrs(kJobsPerProducer);
  std::vector<ChurnJob> s0_jobs(kJobsPerProducer);
  std::vector<kdu_thread_job*> s0_ptrs(kJobsPerProducer);
  std::vector<ChurnJob> s1_jobs(kJobsPerProducer);
  std::vector<kdu_thread_job*> s1_ptrs(kJobsPerProducer);
  for (int i = 0; i < kJobsPerProducer; ++i) {
    o_jobs[i].rem = &wave_rem;
    o_ptrs[i] = &o_jobs[i];
    s0_jobs[i].rem = &wave_rem;
    s0_ptrs[i] = &s0_jobs[i];
    s1_jobs[i].rem = &wave_rem;
    s1_ptrs[i] = &s1_jobs[i];
  }
  q_owner.BindRange(o_ptrs.data(), kJobsPerProducer, 0);
  q_spawner0.BindRange(s0_ptrs.data(), kJobsPerProducer, 0);
  q_spawner1.BindRange(s1_ptrs.data(), kJobsPerProducer, 0);

  struct SpawnerJob final : public kdu_thread_job {
    RecordingThreadQueue* q = nullptr;
    kdu_thread_job* const* ptrs = nullptr;
    std::deque<RecordingThreadQueue>* wave_qs = nullptr;
    kdu_thread_job* const* wave_ptrs = nullptr;
    std::atomic<int>* wave_done = nullptr;
    std::atomic<int>* wave_go = nullptr;
    std::atomic<bool>* burst_go = nullptr;
    std::atomic<bool>* done = nullptr;
    int small_waves = 0;
    int small_batch = 0;
    int burst_batches = 0;
    int burst_batch = 0;
    int wave_queues = 0;
    int wave_jobs = 0;
    SpawnerJob() { set_job_func(&SpawnerJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<SpawnerJob*>(base);
      int offset = 0;
      for (int w = 0; w < self->small_waves; ++w) {
        while (self->wave_go->load(std::memory_order_acquire) < w + 1) {
        }
        self->q->ScheduleBatch(self->ptrs + offset, self->small_batch, caller,
                               /*all_scheduled=*/false);
        offset += self->small_batch;
      }
      while (!self->burst_go->load(std::memory_order_acquire)) {
      }
      for (int b = 0; b < self->burst_batches; ++b) {
        self->q->ScheduleBatch(self->ptrs + offset, self->burst_batch, caller,
                               /*all_scheduled=*/false);
        offset += self->burst_batch;
      }
      for (int w = 0; w < self->wave_queues; ++w) {
        (*self->wave_qs)[w].ScheduleBatch(&self->wave_ptrs[w * self->wave_jobs],
                                          self->wave_jobs, caller,
                                          /*all_scheduled=*/true);
        while (self->wave_done[w].load(std::memory_order_acquire) <
               self->wave_jobs) {
          std::this_thread::yield();
        }
        (*self->wave_qs)[w].Complete(caller);
        EXPECT_TRUE(caller->join(&(*self->wave_qs)[w]));
      }
      self->done->store(true, std::memory_order_release);
    }
  };

  std::array<SpawnerJob, 2> spawners;
  spawners[0].q = &q_spawner0;
  spawners[0].ptrs = s0_ptrs.data();
  spawners[0].wave_qs = &s0_wave_qs;
  spawners[0].wave_ptrs = s0_w_ptrs.data();
  spawners[0].wave_done = s0_wave_done.data();
  spawners[0].done = &spawner0_done;
  spawners[1].q = &q_spawner1;
  spawners[1].ptrs = s1_ptrs.data();
  spawners[1].wave_qs = &s1_wave_qs;
  spawners[1].wave_ptrs = s1_w_ptrs.data();
  spawners[1].wave_done = s1_wave_done.data();
  spawners[1].done = &spawner1_done;
  for (SpawnerJob& s : spawners) {
    s.wave_go = &wave_go;
    s.burst_go = &burst_go;
    s.small_waves = kSmallWaves;
    s.small_batch = kSmallBatch;
    s.burst_batches = kBurstBatches;
    s.burst_batch = kBurstBatch;
    s.wave_queues = kWaveQueues;
    s.wave_jobs = kWaveJobs;
  }
  kdu_thread_job* ctrl_ptrs[2] = {&spawners[0], &spawners[1]};
  q_ctrl.BindRange(ctrl_ptrs, 2, 0);
  q_ctrl.ScheduleBatch(ctrl_ptrs, 2, &env, /*all_scheduled=*/true);

  int o_offset = 0;
  for (int w = 0; w < kSmallWaves; ++w) {
    std::this_thread::sleep_for(std::chrono::microseconds((w & 3) + 1));
    wave_rem.store(3 * kSmallBatch, std::memory_order_release);
    wave_go.store(w + 1, std::memory_order_release);
    q_owner.ScheduleBatch(o_ptrs.data() + o_offset, kSmallBatch, &env,
                          /*all_scheduled=*/false);
    o_offset += kSmallBatch;
    while (wave_rem.load(std::memory_order_acquire) > 0) {
      std::this_thread::yield();
    }
    completed_total.fetch_add(3 * kSmallBatch, std::memory_order_relaxed);
  }

  constexpr int kBurstTotalJobs = 3 * kBurstBatches * kBurstBatch;
  wave_rem.store(kBurstTotalJobs, std::memory_order_release);
  burst_go.store(true, std::memory_order_release);
  for (int b = 0; b < kBurstBatches; ++b) {
    q_owner.ScheduleBatch(o_ptrs.data() + o_offset, kBurstBatch, &env,
                          /*all_scheduled=*/false);
    o_offset += kBurstBatch;
  }
  for (int w = 0; w < kWaveQueues; ++w) {
    own_wave_qs[w].ScheduleBatch(&own_w_ptrs[w * kWaveJobs], kWaveJobs, &env,
                                 /*all_scheduled=*/true);
    while (own_wave_done[w].load(std::memory_order_acquire) < kWaveJobs) {
      std::this_thread::yield();
    }
    own_wave_qs[w].Complete(&env);
    EXPECT_TRUE(env.join(&own_wave_qs[w]));
  }
  while (wave_rem.load(std::memory_order_acquire) > 0) {
    std::this_thread::yield();
  }
  completed_total.fetch_add(kBurstTotalJobs, std::memory_order_relaxed);

  while (!spawner0_done.load(std::memory_order_acquire) ||
         !spawner1_done.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  q_ctrl.Complete(&env);
  q_owner.Complete(&env);
  q_spawner0.Complete(&env);
  q_spawner1.Complete(&env);
  EXPECT_TRUE(env.join(nullptr));
  EXPECT_EQ(3 * kJobsPerProducer, completed_total.load());
  EXPECT_EQ(3 * kWaveQueues * kWaveJobs, wave_job_count.load());
}

TEST_F(KduThreadsTest, DomainSequenceCleanup) {
  // Exercises `kd_thread_domain::remove_unused_sequences` (`1297-1321`) and
  // `free_sequences` reuse by attaching overlapping child sequences on the
  // same domain before completing earlier queues so `add_domain_sequence`
  // terminates prior sequences while consumers advance past them.
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  ASSERT_TRUE(env.add_thread());
  WarmupWorkers(env);

  ZeroJobsAutoCompleteQueue root_q;
  ASSERT_TRUE(env.attach_queue(&root_q, nullptr, nullptr, 0, 0));

  struct SeqJob final : public kdu_thread_job {
    std::atomic<bool> ran{false};
    SeqJob() { set_job_func(&SeqJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
      static_cast<SeqJob*>(base)->ran.store(true, std::memory_order_release);
    }
  };

  for (int wave = 0; wave < 3; ++wave) {
    constexpr int kBatch = 4;
    std::array<RecordingThreadQueue, kBatch> queues{
        RecordingThreadQueue(1, "seq0"), RecordingThreadQueue(1, "seq1"),
        RecordingThreadQueue(1, "seq2"), RecordingThreadQueue(1, "seq3")};
    std::array<SeqJob, kBatch> jobs;
    for (int i = 0; i < kBatch; ++i) {
      const kdu_long seq = static_cast<kdu_long>(wave * kBatch + i + 1);
      ASSERT_TRUE(env.attach_queue(&queues[i], &root_q, "dom_cleanup", seq, 0));
    }
    for (int i = 0; i < kBatch; ++i) {
      queues[i].ScheduleOne(&jobs[i], &env, /*all_scheduled=*/true,
                            KDU_THREAD_JOB_AUTO_BIND_ONCE);
      while (!jobs[i].ran.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      queues[i].Complete(&env);
      EXPECT_TRUE(env.join(&queues[i]));
      env.advance_work_domains();
    }
  }
  EXPECT_TRUE(env.join(&root_q, /*descendants_only=*/true));
  EXPECT_TRUE(env.join(nullptr));
}

TEST_F(KduThreadsTest, ExceptionalJoinWaitAndConcurrentChurn) {
  // 1) Exercise Owner calling `join(nullptr, false, nullptr)` (NULL exc_code)
  // and post-exception `join(&q_sub)` / `terminate(nullptr)` / `destroy()`
  // without having a worker concurrently enter `wait_for_exceptional_join`
  // (upstream Kakadu `kdu_threads.cpp:3419` omits `+1` in `wakeup_info`).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q_sub(1, "ej_sub");
    RecordingThreadQueue q_err(1, "ej_err");
    ASSERT_TRUE(env.attach_queue(&q_sub, nullptr, "ej_dom_s", 0, 0));
    ASSERT_TRUE(env.attach_queue(&q_err, nullptr, "ej_err_dom", 0, 0));

    std::atomic<bool> threw{false};
    FnJob j_err([&](kdu_thread_entity* /*caller*/) {
      threw.store(true, std::memory_order_release);
      throw KDU_ERROR_EXCEPTION;
    });
    q_err.ScheduleOne(&j_err, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!threw.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    EXPECT_FALSE(env.join(nullptr, /*descendants_only=*/false, nullptr));
    EXPECT_FALSE(env.join(&q_sub, /*descendants_only=*/false, nullptr));
    EXPECT_FALSE(env.terminate(nullptr));
    EXPECT_FALSE(env.destroy());
  }

  // 2) Owner calls `join(nullptr)` after `enter_group` when a worker throws,
  // then leaves the group on the same owner thread before destroying.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q_err(1, "ctx_err");
    ASSERT_TRUE(env.attach_queue(&q_err, nullptr, "ctx_dom", 0, 0));

    kdu_thread_context ctx;
    ctx.enter_group(&env);
    std::atomic<bool> threw{false};
    FnJob j_err([&](kdu_thread_entity* /*caller*/) {
      threw.store(true, std::memory_order_release);
      throw KDU_ERROR_EXCEPTION;
    });
    q_err.ScheduleOne(&j_err, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!threw.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    kdu_exception exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(env.join(nullptr, /*descendants_only=*/false, &exc));
    EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
    ctx.leave_group(&env);
    EXPECT_FALSE(env.destroy());
  }
}

TEST_F(KduThreadsTest, DeterministicEdgePathsAndYieldFreq) {
  // 1) Start a worker before any queue/domain is attached (`work_domains[0] ==
  // nullptr`), exercise `find_preferred_bundle_count` bounds, and advance the
  // tail sequence `active_state` (`old_tail->terminate`).
  {
    const int num_procs = kdu_get_num_processors();
    EXPECT_GE(num_procs, 1);
    EXPECT_GE(kdu_thread_entity_affinity::find_preferred_bundle_count(-1), 1);
    EXPECT_LE(kdu_thread_entity_affinity::find_preferred_bundle_count(-1),
              num_procs);
    EXPECT_GE(kdu_thread_entity_affinity::find_preferred_bundle_count(0), 1);
    EXPECT_LE(kdu_thread_entity_affinity::find_preferred_bundle_count(0),
              num_procs);
    EXPECT_GE(
        kdu_thread_entity_affinity::find_preferred_bundle_count(num_procs + 1),
        0);
    EXPECT_LE(
        kdu_thread_entity_affinity::find_preferred_bundle_count(num_procs + 1),
        num_procs);

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    RecordingThreadQueue q1(1, "seq1");
    ASSERT_TRUE(env.attach_queue(&q1, nullptr, "seq_dom", 1, 0));
    std::atomic<int> count{0};
    std::atomic<int> rem1{2};
    FnJob j1([&](kdu_thread_entity* c) {
      count.fetch_add(1);
      ReleaseGate(&rem1, &q1, c);
    });
    q1.ScheduleOne(&j1, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&rem1, &q1, &env);
    EXPECT_TRUE(env.join(&q1));

    RecordingThreadQueue q2(1, "seq2");
    ASSERT_TRUE(env.attach_queue(&q2, nullptr, "seq_dom", 5, 0));
    std::atomic<int> rem2{2};
    FnJob j2([&](kdu_thread_entity* c) {
      count.fetch_add(1);
      ReleaseGate(&rem2, &q2, c);
    });
    q2.ScheduleOne(&j2, &env, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    ReleaseGate(&rem2, &q2, &env);
    EXPECT_TRUE(env.join(&q2));
    EXPECT_EQ(2, count.load());

    // Trigger `old_tail->terminate` (active_state == 1 -> 0 when new tail
    // attaches with greater sequencing index) and `remove_unused_sequences`
    // interior sequence unlinking behind an active head sequence.
    {
      ScopedThreadEntity seq_scoped;
      kdu_thread_entity& senv = seq_scoped.entity;
      senv.create();
      ASSERT_TRUE(senv.exists());

      // Case A: tail active_state == 1 before attaching higher sequence index.
      RecordingThreadQueue st1(1, "st1");
      RecordingThreadQueue st2(1, "st2");
      ASSERT_TRUE(senv.attach_queue(&st1, nullptr, "tail_dom", 10, 0));
      st1.NoteAllScheduled(&senv);
      st1.Complete(&senv);
      ASSERT_TRUE(senv.attach_queue(&st2, nullptr, "tail_dom", 20, 0));
      st2.NoteAllScheduled(&senv);
      st2.Complete(&senv);
      EXPECT_TRUE(senv.join(&st1));
      EXPECT_TRUE(senv.join(&st2));

      // Case B: interior sequences (`max_jobs = 1`) complete behind an active
      // head sequence and before the tail sequence.
      RecordingThreadQueue rm1(1, "rm1");
      RecordingThreadQueue rm2(1, "rm2");
      RecordingThreadQueue rm3(1, "rm3");
      RecordingThreadQueue rm4(1, "rm4");
      RecordingThreadQueue rm5(1, "rm5");
      ASSERT_TRUE(senv.attach_queue(&rm1, nullptr, "mid_dom", 1, 0));
      ASSERT_TRUE(senv.attach_queue(&rm2, nullptr, "mid_dom", 2, 0));
      ASSERT_TRUE(senv.attach_queue(&rm3, nullptr, "mid_dom", 3, 0));
      ASSERT_TRUE(senv.attach_queue(&rm4, nullptr, "mid_dom", 4, 0));
      rm2.NoteAllScheduled(&senv);
      rm2.Complete(&senv);
      rm3.NoteAllScheduled(&senv);
      rm3.Complete(&senv);
      ASSERT_TRUE(senv.attach_queue(&rm5, nullptr, "mid_dom", 5, 0));
      senv.advance_work_domains();
      rm1.NoteAllScheduled(&senv);
      rm1.Complete(&senv);
      rm4.NoteAllScheduled(&senv);
      rm4.Complete(&senv);
      rm5.NoteAllScheduled(&senv);
      rm5.Complete(&senv);
      EXPECT_TRUE(senv.join(nullptr));

      // Case B2: with an active worker pinned on head sequence `idx=1`,
      // complete an interior sequence `idx=2` behind it before joining.
      {
        ScopedThreadEntity wseq_scoped;
        kdu_thread_entity& wenv = wseq_scoped.entity;
        wenv.create();
        ASSERT_TRUE(wenv.exists());
        ASSERT_TRUE(wenv.add_thread());
        WarmupWorkers(wenv);

        RecordingThreadQueue qh(1, "wh");
        RecordingThreadQueue qm(1, "wm");
        RecordingThreadQueue qt(1, "wt");
        ASSERT_TRUE(wenv.attach_queue(&qh, nullptr, "wmid_dom", 1, 0));
        ASSERT_TRUE(wenv.attach_queue(&qm, nullptr, "wmid_dom", 2, 0));
        ASSERT_TRUE(wenv.attach_queue(&qt, nullptr, "wmid_dom", 3, 0));

        std::atomic<bool> head_started{false};
        std::atomic<bool> release_head{false};
        std::atomic<int> rem_h{2};
        FnJob jh([&](kdu_thread_entity* c) {
          head_started.store(true, std::memory_order_release);
          while (!release_head.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          ReleaseGate(&rem_h, &qh, c);
        });
        qh.ScheduleOne(&jh, &wenv, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
        ReleaseGate(&rem_h, &qh, &wenv);
        while (!head_started.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        qm.NoteAllScheduled(&wenv);
        qm.Complete(&wenv);
        RecordingThreadQueue qt2(1, "wt2");
        ASSERT_TRUE(wenv.attach_queue(&qt2, nullptr, "wmid_dom", 4, 0));
        wenv.advance_work_domains();

        release_head.store(true, std::memory_order_release);
        qt.NoteAllScheduled(&wenv);
        qt.Complete(&wenv);
        qt2.NoteAllScheduled(&wenv);
        qt2.Complete(&wenv);
        EXPECT_TRUE(wenv.join(nullptr));
      }

      // Case C: nested wait (`join` -> job -> `wait_for_condition` with
      // foreign wakeup) and nested `join` -> `join` (`expect_foreign_wakeup`
      // false) exercise `cond->link` in `check_working_waiters_idle`.
      RecordingThreadQueue q_outer(1, "outer_wait");
      ASSERT_TRUE(senv.attach_queue(&q_outer, nullptr, "wait_dom", 0, 0));
      std::atomic<int> outer_rem{2};
      FnJob j_outer([&](kdu_thread_entity* c) {
        kdu_thread_entity_condition* inner_cond = senv.get_condition();
        std::thread foreign([&] {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          senv.signal_condition(inner_cond, KDU_FOREIGN_SIGNAL_EXPECTED);
        });
        senv.wait_for_condition("nested_outer_inner", true);
        foreign.join();
        ReleaseGate(&outer_rem, &q_outer, c);
      });
      q_outer.ScheduleOne(&j_outer, &senv, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
      ReleaseGate(&outer_rem, &q_outer, &senv);
      EXPECT_TRUE(senv.join(&q_outer));

      {
        ScopedThreadEntity nest_scoped;
        kdu_thread_entity& nenv = nest_scoped.entity;
        nenv.create();
        ASSERT_TRUE(nenv.exists());
        ASSERT_TRUE(nenv.add_thread());
        WarmupWorkers(nenv);

        RecordingThreadQueue n_outer(1, "n_outer");
        RecordingThreadQueue n_inner(1, "n_inner");
        ASSERT_TRUE(nenv.attach_queue(&n_outer, nullptr, "n_dom1", 0, 0));
        ASSERT_TRUE(nenv.attach_queue(&n_inner, nullptr, "n_dom2", 0, 0));

        std::atomic<int> in_rem{2};
        FnJob j_in([&](kdu_thread_entity* c) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          ReleaseGate(&in_rem, &n_inner, c);
        });
        std::atomic<int> out_rem{2};
        FnJob j_out([&](kdu_thread_entity* c) {
          n_inner.ScheduleOne(&j_in, c, /*all_scheduled=*/true,
                              KDU_THREAD_JOB_AUTO_BIND_ONCE);
          ReleaseGate(&in_rem, &n_inner, c);
          EXPECT_TRUE(c->join(&n_inner));
          ReleaseGate(&out_rem, &n_outer, c);
        });
        n_outer.ScheduleOne(&j_out, &nenv, /*all_scheduled=*/true,
                            KDU_THREAD_JOB_AUTO_BIND_ONCE);
        ReleaseGate(&out_rem, &n_outer, &nenv);
        EXPECT_TRUE(nenv.join(&n_outer));
      }
    }
  }

  // 2) Owner executes `j_long1` inside `env.join(&q_long)` (`ww_process_jobs`)
  // while `q_long` still has `j_long2` pending and the single worker throws
  // `KDU_ERROR_EXCEPTION`, deterministically hitting `if (exception) break;`
  // (`ww_process_jobs`) without a worker entering `wait_for_exceptional_join`.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q_err(1, "err_q");
    RecordingThreadQueue q_long(2, "long_q");
    ASSERT_TRUE(env.attach_queue(&q_err, nullptr, "dom_e", 0, 0));
    ASSERT_TRUE(env.attach_queue(&q_long, nullptr, "dom_l", 0, 0));

    std::atomic<bool> err_started{false};
    std::atomic<bool> long_started{false};
    std::atomic<bool> err_thrown{false};
    FnJob j_err([&](kdu_thread_entity* /*c*/) {
      err_started.store(true, std::memory_order_release);
      while (!long_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      err_thrown.store(true, std::memory_order_release);
      throw KDU_ERROR_EXCEPTION;
    });
    q_err.ScheduleOne(&j_err, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!err_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    std::atomic<bool> second_ran{false};
    FnJob j_long1([&](kdu_thread_entity* /*c*/) {
      long_started.store(true, std::memory_order_release);
      while (!err_thrown.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      // Allow the worker thread to record `KDU_ERROR_EXCEPTION` before
      // `ww_process_jobs` loops around to check `exception`.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    FnJob j_long2([&](kdu_thread_entity* /*c*/) {
      second_ran.store(true, std::memory_order_relaxed);
    });
    kdu_thread_job* long_jobs[2] = {&j_long1, &j_long2};
    q_long.BindRange(long_jobs, 2, 0);
    q_long.ScheduleBatch(long_jobs, 2, &env, /*all_scheduled=*/true);

    kdu_exception exc = KDU_NULL_EXCEPTION;
    EXPECT_FALSE(env.join(&q_long, /*descendants_only=*/false, &exc));
    EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
    EXPECT_FALSE(second_ran.load(std::memory_order_relaxed));
    EXPECT_FALSE(env.destroy());
  }

  // 3) Multi-worker bursts with `set_yield_frequency(1)` and overlapping
  // multi-queue palette turnover across domains exercises `nww_yield()` and
  // `clean_dirty_palettes()`.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    env.set_yield_frequency(1);
    for (int i = 0; i < 6; ++i) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);

    // Small job bursts encourage some workers to sleep while others yield.
    std::atomic<int> completed{0};
    for (int b = 0; b < 40; ++b) {
      RecordingThreadQueue yield_q(4, "yield_b");
      ASSERT_TRUE(env.attach_queue(&yield_q, nullptr, "ydom", b + 1, 0));
      std::atomic<int> rem{5};
      FnJob b0([&](kdu_thread_entity* c) {
        completed.fetch_add(1, std::memory_order_relaxed);
        ReleaseGate(&rem, &yield_q, c);
      });
      FnJob b1([&](kdu_thread_entity* c) {
        completed.fetch_add(1, std::memory_order_relaxed);
        ReleaseGate(&rem, &yield_q, c);
      });
      FnJob b2([&](kdu_thread_entity* c) {
        completed.fetch_add(1, std::memory_order_relaxed);
        ReleaseGate(&rem, &yield_q, c);
      });
      FnJob b3([&](kdu_thread_entity* c) {
        completed.fetch_add(1, std::memory_order_relaxed);
        ReleaseGate(&rem, &yield_q, c);
      });
      kdu_thread_job* ptrs[4] = {&b0, &b1, &b2, &b3};
      yield_q.BindRange(ptrs, 4, 0);
      yield_q.ScheduleBatch(ptrs, 4, &env, /*all_scheduled=*/true);
      ReleaseGate(&rem, &yield_q, &env);
      EXPECT_TRUE(env.join(&yield_q));
    }
    EXPECT_EQ(160, completed.load(std::memory_order_acquire));

    // Batch 16 overlapping queues per wave across 4 waves (512 jobs, > 250
    // palettes) so workers actively dequeue while owner recycles dirty
    // palettes.
    constexpr int kWaves = 4;
    constexpr int kBatch = 16;
    constexpr int kJobsPerQ = 8;
    std::atomic<int> churn_done{0};
    const char* doms[4] = {"c0", "c1", "c2", "c3"};
    for (int w = 0; w < kWaves; ++w) {
      std::deque<RecordingThreadQueue> qs;
      std::deque<std::atomic<int>> rems;
      std::deque<FnJob> jobs;
      for (int q = 0; q < kBatch; ++q) {
        qs.emplace_back(kJobsPerQ, "churn");
        rems.emplace_back(kJobsPerQ + 1);
        ASSERT_TRUE(env.attach_queue(&qs[q], nullptr, doms[q & 3], 0, 0));
      }
      for (int q = 0; q < kBatch; ++q) {
        std::vector<kdu_thread_job*> jptrs(kJobsPerQ);
        for (int j = 0; j < kJobsPerQ; ++j) {
          jobs.emplace_back([&, q](kdu_thread_entity* c) {
            churn_done.fetch_add(1, std::memory_order_relaxed);
            ReleaseGate(&rems[q], &qs[q], c);
          });
          jptrs[j] = &jobs.back();
        }
        qs[q].BindRange(jptrs.data(), kJobsPerQ, 0);
        qs[q].ScheduleBatch(jptrs.data(), kJobsPerQ, &env, true);
        ReleaseGate(&rems[q], &qs[q], &env);
      }
      EXPECT_TRUE(env.join(nullptr));
    }
    EXPECT_EQ(kWaves * kBatch * kJobsPerQ, churn_done.load());
  }

  // 4) Non-default second argument to `kdu_thread_entity::create`.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create(0, 1);
    EXPECT_TRUE(env.exists());
    EXPECT_TRUE(env.destroy());
  }

  // 5) `dequeue_general_job` empty-sets return (`4310`) and working-waiter
  //    foreign-wakeup deadlock bypass (`4218-4219`).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());

    kdu_thread_entity_condition* cond = env.get_condition();
    std::thread foreign_waker([&env, cond] {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    env.wait_for_condition("empty_foreign_wait",
                           /*expect_foreign_wakeup=*/true);
    foreign_waker.join();
    EXPECT_TRUE(env.join(nullptr));
  }

  // 6) Worker `process_jobs` foreign-wakeup deadlock bypass (`3955-3957`).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q(1, "worker_ext_wake");
    ASSERT_TRUE(env.attach_queue(&q, nullptr, "w_ext_dom", 0, 0));
    std::atomic<bool> job_started{false};
    std::atomic<bool> release_job{false};
    std::atomic<bool> job_finished{false};
    FnJob j([&](kdu_thread_entity* /*caller*/) {
      job_started.store(true, std::memory_order_release);
      while (!release_job.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      job_finished.store(true, std::memory_order_release);
    });
    q.ScheduleOne(&j, &env, /*all_scheduled=*/true,
                  KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!job_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    kdu_thread_entity_condition* cond = env.get_condition();
    std::thread foreign_waker([&env, cond, &release_job, &job_finished] {
      // Allow the owner to enter `wait_for_condition` and sleep cleanly.
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
      release_job.store(true, std::memory_order_release);
      while (!job_finished.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      // Keep foreign wait active while worker hits `process_jobs` deadlock
      // check and sees `num_waiting_for_foreign_wakeup > 0`.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    env.wait_for_condition("worker_foreign_wait",
                           /*expect_foreign_wakeup=*/true);
    foreign_waker.join();
    q.Complete(&env);
    EXPECT_TRUE(env.join(&q));
  }

  // 7) `ww_return_job` worker wakeup (`4146`) and yield transition wakeup
  //    (`4009`).
  {
    struct BriefSleepJob final : public kdu_thread_job {
      std::atomic<int>* done = nullptr;
      int sleep_us = 100;
      BriefSleepJob() { set_job_func(&BriefSleepJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
        auto* self = static_cast<BriefSleepJob*>(base);
        if (self->sleep_us > 0) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(self->sleep_us));
        }
        self->done->fetch_add(1, std::memory_order_release);
      }
    };

    // 7a) Keep owner (`first_wait_safe == false`) spinning inside `join` on
    //     `q_hold` while a background worker schedules 5-job `SAFE_CONTEXT`
    //     waves (`J = 5` -> `ww_reserve_job` temporarily drops `J` to 4).
    {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      ASSERT_TRUE(env.exists());
      for (int t = 0; t < 8; ++t) {
        ASSERT_TRUE(env.add_thread());
      }
      WarmupWorkers(env);
      EXPECT_FALSE(env.declare_first_owner_wait_safe(false));

      RecordingThreadQueue q_hold(1, "ww_ret_hold");
      RecordingThreadQueue q_sched(1, "ww_ret_sched");
      ASSERT_TRUE(env.attach_queue(&q_hold, nullptr, "ww_hold_d", 0, 0));
      ASSERT_TRUE(env.attach_queue(&q_sched, nullptr, "ww_sched_d", 0, 0));

      std::atomic<bool> sched_done{false};
      FnJob hold_job([&](kdu_thread_entity* caller) {
        while (!sched_done.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        q_hold.Complete(caller);
      });
      FnJob sched_job([&](kdu_thread_entity* caller) {
        for (int w = 0; w < 24; ++w) {
          RecordingThreadQueue q_safe(5, "ww_ret_s5");
          EXPECT_TRUE(caller->attach_queue(&q_safe, nullptr, "ww_s5_dom", 0,
                                           KDU_THREAD_QUEUE_SAFE_CONTEXT));
          std::atomic<int> sdone{0};
          std::array<BriefSleepJob, 5> sjobs;
          std::array<kdu_thread_job*, 5> sptrs;
          for (int i = 0; i < 5; ++i) {
            sjobs[i].done = &sdone;
            sjobs[i].sleep_us = 60;
            sptrs[i] = &sjobs[i];
          }
          q_safe.BindRange(sptrs.data(), 5, 0);
          q_safe.ScheduleBatch(sptrs.data(), 5, caller,
                               /*all_scheduled=*/true);
          while (sdone.load(std::memory_order_acquire) < 5) {
            std::this_thread::yield();
          }
          q_safe.Complete(caller);
          EXPECT_TRUE(caller->join(&q_safe));
        }
        sched_done.store(true, std::memory_order_release);
        q_sched.Complete(caller);
      });

      q_hold.ScheduleOne(&hold_job, &env, /*all_scheduled=*/true,
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
      q_sched.ScheduleOne(&sched_job, &env, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
      EXPECT_TRUE(env.join(&q_hold));
      EXPECT_TRUE(env.join(&q_sched));
    }

    // 7b) Yield frequency chain-wake (`4009`) plus negative reset.
    {
      ScopedThreadEntity scoped;
      kdu_thread_entity& env = scoped.entity;
      env.create();
      ASSERT_TRUE(env.exists());
      for (int t = 0; t < 4; ++t) {
        ASSERT_TRUE(env.add_thread());
      }
      WarmupWorkers(env);
      env.set_yield_frequency(1);

      for (int w = 0; w < 24; ++w) {
        RecordingThreadQueue q_a(4, "yw_a");
        RecordingThreadQueue q_b(4, "yw_b");
        ASSERT_TRUE(env.attach_queue(&q_a, nullptr, "yw_da", 0, 0));
        ASSERT_TRUE(env.attach_queue(&q_b, nullptr, "yw_db", 0, 0));
        std::atomic<int> done_a{0};
        std::atomic<int> done_b{0};
        std::array<BriefSleepJob, 4> jobs_a;
        std::array<BriefSleepJob, 4> jobs_b;
        std::array<kdu_thread_job*, 4> ptrs_a;
        std::array<kdu_thread_job*, 4> ptrs_b;
        for (int i = 0; i < 4; ++i) {
          jobs_a[i].done = &done_a;
          jobs_a[i].sleep_us = (i == 0) ? 10 : 80;
          ptrs_a[i] = &jobs_a[i];
          jobs_b[i].done = &done_b;
          jobs_b[i].sleep_us = 20;
          ptrs_b[i] = &jobs_b[i];
        }
        q_a.BindRange(ptrs_a.data(), 4, 0);
        q_b.BindRange(ptrs_b.data(), 4, 0);
        q_a.ScheduleBatch(ptrs_a.data(), 4, &env, /*all_scheduled=*/true);
        std::this_thread::sleep_for(std::chrono::microseconds((w % 8) * 20));
        q_b.ScheduleBatch(ptrs_b.data(), 4, &env, /*all_scheduled=*/true);
        while (done_a.load(std::memory_order_acquire) < 4 ||
               done_b.load(std::memory_order_acquire) < 4) {
          std::this_thread::yield();
        }
        q_a.Complete(&env);
        q_b.Complete(&env);
        EXPECT_TRUE(env.join(&q_a));
        EXPECT_TRUE(env.join(&q_b));
      }
      env.set_yield_frequency(-1);
    }
  }

  // 8) Condition pre-signalled vs `wake_waiter` race (`4178-4179`).
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    for (int r = 0; r < 40; ++r) {
      RecordingThreadQueue q(1, "cond_race");
      ASSERT_TRUE(env.attach_queue(&q, nullptr, "cr_dom", 0, 0));
      kdu_thread_entity_condition* cond = env.get_condition();
      std::atomic<bool> start_wait{false};
      FnJob sig_job([&](kdu_thread_entity* caller) {
        while (!start_wait.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        const int spin = (r & 15) * 8;
        volatile int sink = 0;
        for (int k = 0; k < spin; ++k) {
          sink = sink + k;
        }
        (void)sink;
        caller->signal_condition(cond, 0);
      });
      q.ScheduleOne(&sig_job, &env, /*all_scheduled=*/true,
                    KDU_THREAD_JOB_AUTO_BIND_ONCE);
      start_wait.store(true, std::memory_order_release);
      env.wait_for_condition("cond_race_wait", false);
      q.Complete(&env);
      EXPECT_TRUE(env.join(&q));
    }
  }

  // 8b) Deterministic zero-worker wait_for_condition deadlock (`4216-4217`),
  // mixed clean/hazard palette reuse in `initialize` (`2599, 2604-2605`),
  // `join` completion_state CAS retry (`3522`), and `clean_dirty_palettes`
  // hazard contention (`1654, 1661-1663`).
  {
    error_handler_.TakeMessage();
    {
      ScopedThreadEntity dead_env;
      dead_env.entity.create();
      ASSERT_TRUE(dead_env.entity.exists());
      bool caught_deadlock = false;
      try {
        dead_env.entity.wait_for_condition("deadlock_no_workers", false);
      } catch (kdu_exception) {
        caught_deadlock = true;
      }
      EXPECT_TRUE(caught_deadlock);
      EXPECT_FALSE(error_handler_.TakeMessage().empty());
    }

    {
      ScopedThreadEntity race_env;
      race_env.entity.create();
      ASSERT_TRUE(race_env.entity.exists());
      ASSERT_TRUE(race_env.entity.add_thread());
      WarmupWorkers(race_env.entity);

      for (int r = 0; r < 192; ++r) {
        RecordingThreadQueue q(1, "join_cas");
        ASSERT_TRUE(race_env.entity.attach_queue(&q, nullptr, "jcas_d", 0, 0));
        std::atomic<bool> go{false};
        FnJob j([&](kdu_thread_entity* caller) {
          while (!go.load(std::memory_order_acquire)) {
          }
          const int stagger = r & 7;
          volatile int sink = 0;
          for (int s = 0; s < stagger; ++s) {
            sink = sink + s;
          }
          (void)sink;
          q.Complete(caller);
        });
        q.ScheduleOne(&j, &race_env.entity, /*all_scheduled=*/false,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);
        go.store(true, std::memory_order_release);
        EXPECT_TRUE(race_env.entity.join(&q));
      }

      // Recycle 64 bound jobs >256 times so `clean_dirty_palettes` executes
      // repeatedly while worker 1 holds `hzp->ref` inside `get_job`.
      struct RecycleJob final : public kdu_thread_job {
        std::atomic<bool> ready{true};
        std::atomic<int>* completed = nullptr;
        RecycleJob() { set_job_func(&RecycleJob::Run); }
        static void Run(kdu_thread_job* base, kdu_thread_entity*) {
          auto* self = static_cast<RecycleJob*>(base);
          self->completed->fetch_add(1, std::memory_order_relaxed);
          self->ready.store(true, std::memory_order_release);
        }
      };
      constexpr int kRecSlots = 64;
      constexpr int kRecIters = 12;
      RecordingThreadQueue q_rec(kRecSlots, "rec_plt");
      ASSERT_TRUE(race_env.entity.attach_queue(&q_rec, nullptr, "rec_d", 0, 0));
      std::atomic<int> rec_completed{0};
      std::array<RecycleJob, kRecSlots> rec_jobs;
      std::array<kdu_thread_job*, kRecSlots> rec_ptrs;
      for (int i = 0; i < kRecSlots; ++i) {
        rec_jobs[i].completed = &rec_completed;
        rec_ptrs[i] = &rec_jobs[i];
      }
      q_rec.BindRange(rec_ptrs.data(), kRecSlots, 0);
      for (int round = 0; round < kRecIters; ++round) {
        for (int i = 0; i < kRecSlots; ++i) {
          while (!rec_jobs[i].ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          rec_jobs[i].ready.store(false, std::memory_order_relaxed);
          q_rec.ScheduleOne(&rec_jobs[i], &race_env.entity,
                            /*all_scheduled=*/false, 0);
        }
      }
      for (int i = 0; i < kRecSlots; ++i) {
        while (!rec_jobs[i].ready.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
      }
      EXPECT_EQ(kRecSlots * kRecIters,
                rec_completed.load(std::memory_order_relaxed));
      q_rec.Complete(&race_env.entity);
      EXPECT_TRUE(race_env.entity.join(&q_rec));
    }
  }

  // 9) Two-thread `all_scheduled=true` race warnings (`schedule_jobs`
  //    `0x17051201` and `schedule_job` `0x17051202`), matched to `NPROC=2`.
  {
    struct WarnRaceQueue final : public kdu_thread_queue {
      int get_max_jobs() override { return 2; }
      void BindJobs(kdu_thread_job** jobs, int count) {
        bind_jobs(jobs, count, 0);
      }
      void SchedOne(kdu_thread_job* job, kdu_thread_entity* caller) {
        schedule_job(job, caller, /*all_scheduled=*/true, 0);
      }
      void SchedBatch(kdu_thread_job** jobs, int count,
                      kdu_thread_entity* caller) {
        schedule_jobs(jobs, count, caller, /*all_scheduled=*/true);
      }
      void Complete(kdu_thread_entity* caller) { all_done(caller); }
    };

    struct WarnNoOpJob final : public kdu_thread_job {
      std::atomic<int>* done = nullptr;
      WarnNoOpJob() { set_job_func(&WarnNoOpJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* /*caller*/) {
        static_cast<WarnNoOpJob*>(base)->done->fetch_add(
            1, std::memory_order_release);
      }
    };

    struct alignas(64) WarnSync {
      alignas(64) std::atomic<int> ack{0};
      alignas(64) std::atomic<int> go{0};
      alignas(64) std::atomic<int> done{0};
    };

    struct WarnCtrlJob final : public kdu_thread_job {
      bool batch_mode = false;
      int num_queues = 0;
      WarnSync* sync = nullptr;
      std::deque<WarnRaceQueue>* qs = nullptr;
      kdu_thread_job** job_ptrs = nullptr;
      std::atomic<int>* expected_counts = nullptr;
      WarnCtrlJob() { set_job_func(&WarnCtrlJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<WarnCtrlJob*>(base);
        for (int qi = 0; qi < self->num_queues; ++qi) {
          self->sync->ack.store(qi + 1, std::memory_order_release);
          while (self->sync->go.load(std::memory_order_acquire) < qi + 1) {
            std::this_thread::yield();
          }
          const int ws = (qi & 7);
          volatile int wsink = 0;
          for (int k = 0; k < ws; ++k) {
            wsink = wsink + k;
          }
          (void)wsink;
          bool threw = false;
          try {
            if (!self->batch_mode) {
              (*self->qs)[qi].SchedOne(self->job_ptrs[qi], caller);
            } else {
              kdu_thread_job* p = self->job_ptrs[qi];
              (*self->qs)[qi].SchedBatch(&p, 1, caller);
            }
          } catch (kdu_exception) {
            threw = true;
          }
          if (!threw) {
            self->expected_counts[qi].fetch_add(1, std::memory_order_relaxed);
          }
          self->sync->done.store(qi + 1, std::memory_order_release);
        }
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);
    env.set_yield_frequency(0);

    constexpr int kWarnQueues = 16;
    constexpr int kMaxWarnRounds = 2;
    for (bool batch_mode : {false, true}) {
      const char* needle = batch_mode ? "kdu_thread_queue::schedule_jobs'"
                                      : "kdu_thread_queue::schedule_job'";
      for (int round = 0; round < kMaxWarnRounds; ++round) {
        if (warning_handler_.Messages().find(needle) != std::string::npos) {
          break;
        }
        std::deque<WarnRaceQueue> qs;
        std::array<WarnNoOpJob, kWarnQueues> jobs0;
        std::array<WarnNoOpJob, kWarnQueues> jobs1;
        std::array<kdu_thread_job*, kWarnQueues> w_ptrs;
        std::array<kdu_thread_job*, kWarnQueues> own_ptrs;
        std::array<std::atomic<int>, kWarnQueues> expected_counts{};
        std::array<std::atomic<int>, kWarnQueues> done_counts{};

        for (int qi = 0; qi < kWarnQueues; ++qi) {
          jobs0[qi].done = &done_counts[qi];
          jobs1[qi].done = &done_counts[qi];
          qs.emplace_back();
          ASSERT_TRUE(env.attach_queue(&qs.back(), nullptr, "warn_rdom", 0, 0));
          kdu_thread_job* bind2[2] = {&jobs0[qi], &jobs1[qi]};
          qs.back().BindJobs(bind2, 2);
          w_ptrs[qi] = &jobs0[qi];
          own_ptrs[qi] = &jobs1[qi];
        }

        WarnSync sync;
        WarnCtrlJob ctrl;
        ctrl.batch_mode = batch_mode;
        ctrl.num_queues = kWarnQueues;
        ctrl.sync = &sync;
        ctrl.qs = &qs;
        ctrl.job_ptrs = w_ptrs.data();
        ctrl.expected_counts = expected_counts.data();

        WarnRaceQueue q_ctrl;
        ASSERT_TRUE(env.attach_queue(&q_ctrl, nullptr, "warn_ctrld", 0, 0));
        kdu_thread_job* ctrl_ptr[1] = {&ctrl};
        q_ctrl.BindJobs(ctrl_ptr, 1);
        q_ctrl.SchedBatch(ctrl_ptr, 1, &env);

        for (int qi = 0; qi < kWarnQueues; ++qi) {
          while (sync.ack.load(std::memory_order_acquire) < qi + 1) {
            std::this_thread::yield();
          }
          sync.go.store(qi + 1, std::memory_order_release);
          const int os = ((qi >> 3) & 7);
          volatile int osink = 0;
          for (int k = 0; k < os; ++k) {
            osink = osink + k;
          }
          (void)osink;
          bool o_threw = false;
          try {
            if (!batch_mode) {
              qs[qi].SchedOne(own_ptrs[qi], &env);
            } else {
              kdu_thread_job* p = own_ptrs[qi];
              qs[qi].SchedBatch(&p, 1, &env);
            }
          } catch (kdu_exception) {
            o_threw = true;
          }
          if (!o_threw) {
            expected_counts[qi].fetch_add(1, std::memory_order_relaxed);
          }
          while (sync.done.load(std::memory_order_acquire) < qi + 1) {
            std::this_thread::yield();
          }
        }

        q_ctrl.Complete(&env);
        EXPECT_TRUE(env.join(&q_ctrl));
        for (int qi = 0; qi < kWarnQueues; ++qi) {
          const int exp = expected_counts[qi].load(std::memory_order_acquire);
          while (done_counts[qi].load(std::memory_order_acquire) < exp) {
            std::this_thread::yield();
          }
          qs[qi].Complete(&env);
          EXPECT_TRUE(env.join(&qs[qi]));
        }
        error_handler_.TakeMessage();
      }
    }
    EXPECT_TRUE(env.join(nullptr));
  }
}

TEST_F(KduThreadsTest, MaxDomainsLimitReturnsFalseOnWorkerAdd) {
  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());

  std::deque<RecordingThreadQueue> qs;
  for (int d = 0; d < KDU_MAX_DOMAINS; ++d) {
    qs.emplace_back(1, "dom_lim");
    std::string dom = "lim_dom_" + std::to_string(d);
    ASSERT_TRUE(env.attach_queue(&qs.back(), nullptr, dom.c_str(), 0, 0));
  }

  // With KDU_MAX_DOMAINS active domain sequences on the group owner, creating
  // a worker bound to an additional domain fails in
  // get_initial_domain_sequence().
  EXPECT_FALSE(env.add_thread("lim_dom_overflow"));

  for (auto& q : qs) {
    q.NoteAllScheduled(&env);
    q.Complete(&env);
    EXPECT_TRUE(env.join(&q));
  }
  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, WorkerDetectsDeadlockWhileOwnerWaits) {
  struct WaitDlJob final : public kdu_thread_job {
    RecordingThreadQueue* q = nullptr;
    std::atomic<bool>* worker_in_job = nullptr;
    std::atomic<bool>* owner_entering = nullptr;
    WaitDlJob() { set_job_func(&WaitDlJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<WaitDlJob*>(base);
      self->worker_in_job->store(true, std::memory_order_release);
      while (!self->owner_entering->load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      // Give the owner time to reach enter_wait() before completing this last
      // NWW job so the worker triggers deadlock detection in process_jobs().
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      self->q->Complete(caller);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  ASSERT_TRUE(env.add_thread());
  WarmupWorkers(env);

  RecordingThreadQueue q(1, "worker_dl_q");
  ASSERT_TRUE(env.attach_queue(&q, nullptr, "worker_dl_dom", 0, 0));

  std::atomic<bool> worker_in_job{false};
  std::atomic<bool> owner_entering{false};
  WaitDlJob job;
  job.q = &q;
  job.worker_in_job = &worker_in_job;
  job.owner_entering = &owner_entering;
  q.ScheduleOne(&job, &env, /*all_scheduled=*/true,
                KDU_THREAD_JOB_AUTO_BIND_ONCE);

  while (!worker_in_job.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  owner_entering.store(true, std::memory_order_release);
  kdu_exception wait_exc = KDU_NULL_EXCEPTION;
  try {
    env.wait_for_condition("worker_deadlock", /*expect_foreign_wakeup=*/false);
  } catch (kdu_exception e) {
    wait_exc = e;
  }
  EXPECT_EQ(KDU_ERROR_EXCEPTION, wait_exc);

  kdu_exception exc = KDU_NULL_EXCEPTION;
  EXPECT_FALSE(env.join(&q, /*descendants_only=*/false, &exc));
  EXPECT_EQ(KDU_ERROR_EXCEPTION, exc);
  EXPECT_FALSE(env.destroy());
  EXPECT_NE(std::string::npos,
            error_handler_.TakeMessage().find("Deadlock detected"));
}

TEST_F(KduThreadsTest, ScheduleBatchAndOneWideWindowWarningRace) {
  struct DirectQueue final : public kdu_thread_queue {
    int max_jobs = 1;
    explicit DirectQueue(int mj) : max_jobs(mj) {}
    int get_max_jobs() override { return max_jobs; }
    void BindJobs(kdu_thread_job** jobs, int count) {
      bind_jobs(jobs, count, 0);
    }
    void SchedOne(kdu_thread_job* job, kdu_thread_entity* caller,
                  bool all_scheduled, int opts) {
      schedule_job(job, caller, all_scheduled, opts);
    }
    void SchedBatch(kdu_thread_job** jobs, int count, kdu_thread_entity* caller,
                    bool all_scheduled) {
      schedule_jobs(jobs, count, caller, all_scheduled);
    }
    void NoteAll(kdu_thread_entity* caller) { note_all_scheduled(caller); }
    void Complete(kdu_thread_entity* caller) { all_done(caller); }
  };

  struct CountRaceJob final : public kdu_thread_job {
    std::atomic<int>* done_count = nullptr;
    CountRaceJob() { set_job_func(&CountRaceJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity*) {
      static_cast<CountRaceJob*>(base)->done_count->fetch_add(
          1, std::memory_order_release);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());
  WarmupWorkers(env);
  env.set_yield_frequency(0);

  // Part A: Worker calls note_all_scheduled() while owner sweeps `o_spin`
  // across 0..511 in steps of 1 after signaling `o_ready` so owner's
  // `schedule_jobs(..., all_scheduled=true)` crosses `note_all_scheduled()`
  // inside the CSTATE_S check/CAS window for KDU_WARNING_DEV 0x17051201.
  {
    constexpr int kRounds = 64;
    constexpr int kQueues = 32;
    constexpr int kBatch = 1;
    struct WorkerNoteAllJob final : public kdu_thread_job {
      std::atomic<int>* w_ready = nullptr;
      std::atomic<int>* o_ready = nullptr;
      std::atomic<int>* done = nullptr;
      DirectQueue** q_ptrs = nullptr;
      WorkerNoteAllJob() { set_job_func(&WorkerNoteAllJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<WorkerNoteAllJob*>(base);
        for (int qi = 0; qi < kQueues; ++qi) {
          self->w_ready->store(qi + 1, std::memory_order_release);
          while (self->o_ready->load(std::memory_order_acquire) < qi + 1) {
          }
          self->q_ptrs[qi]->NoteAll(caller);
          self->done->store(qi + 1, std::memory_order_release);
        }
      }
    };

    for (int round = 0; round < kRounds; ++round) {
      std::deque<DirectQueue> qs;
      DirectQueue* q_ptrs[kQueues];
      RecordingThreadQueue q_keep(1, "wb_keep");
      ASSERT_TRUE(env.attach_queue(&q_keep, nullptr, "wb_dom", 0, 0));
      std::vector<CountRaceJob> jobs(kQueues * kBatch);
      std::vector<kdu_thread_job*> ptrs(kQueues * kBatch);
      kdu_thread_job** batch_ptrs[kQueues];
      std::vector<std::atomic<int>> job_done(kQueues);
      std::vector<int> expected(kQueues, 0);
      for (int qi = 0; qi < kQueues; ++qi) {
        job_done[qi].store(0, std::memory_order_relaxed);
        for (int i = 0; i < kBatch; ++i) {
          const int idx = qi * kBatch + i;
          jobs[idx].done_count = &job_done[qi];
          ptrs[idx] = &jobs[idx];
        }
        qs.emplace_back(kBatch);
        ASSERT_TRUE(env.attach_queue(&qs.back(), nullptr, "wb_dom", 0, 0));
        q_ptrs[qi] = &qs.back();
        batch_ptrs[qi] = &ptrs[qi * kBatch];
        q_ptrs[qi]->BindJobs(batch_ptrs[qi], kBatch);
      }

      std::atomic<int> w_ready{0};
      std::atomic<int> o_ready{0};
      std::atomic<int> done{0};
      WorkerNoteAllJob wjob;
      wjob.w_ready = &w_ready;
      wjob.o_ready = &o_ready;
      wjob.done = &done;
      wjob.q_ptrs = q_ptrs;
      DirectQueue q_ctrl(1);
      ASSERT_TRUE(env.attach_queue(&q_ctrl, nullptr, "wb_ctrl", 0, 0));
      kdu_thread_job* ctrl_ptr = &wjob;
      q_ctrl.BindJobs(&ctrl_ptr, 1);
      q_ctrl.SchedBatch(&ctrl_ptr, 1, &env, /*all_scheduled=*/true);

      for (int qi = 0; qi < kQueues; ++qi) {
        while (w_ready.load(std::memory_order_acquire) < qi + 1) {
          std::this_thread::yield();
        }
        o_ready.store(qi + 1, std::memory_order_release);
        volatile int sink = 0;
        const int o_spin = (qi * 16) + (round & 15);
        for (int s = 0; s < o_spin; ++s) {
          sink = sink + s;
        }
        (void)sink;
        bool threw = false;
        try {
          q_ptrs[qi]->SchedBatch(batch_ptrs[qi], kBatch, &env,
                                 /*all_scheduled=*/true);
        } catch (kdu_exception) {
          threw = true;
        }
        if (!threw) {
          expected[qi] = kBatch;
        }
        while (done.load(std::memory_order_acquire) < qi + 1) {
          std::this_thread::yield();
        }
      }
      q_ctrl.Complete(&env);
      EXPECT_TRUE(env.join(&q_ctrl));
      for (int qi = 0; qi < kQueues; ++qi) {
        while (job_done[qi].load(std::memory_order_acquire) < expected[qi]) {
          std::this_thread::yield();
        }
        q_ptrs[qi]->Complete(&env);
        EXPECT_TRUE(env.join(q_ptrs[qi]));
      }
      q_keep.NoteAllScheduled(&env);
      q_keep.Complete(&env);
      EXPECT_TRUE(env.join(&q_keep));
      error_handler_.TakeMessage();
    }
  }

  // Part B: Owner calls schedule_job(..., AUTO_BIND_ONCE) at palette index 255
  // so traversing 255 palette_refs nodes widens the race window against
  // worker note_all_scheduled() for KDU_WARNING_DEV 0x17051202.
  {
    constexpr int kRounds = 3;
    constexpr int kQueues = 16;
    constexpr int kPreBind = 1535;
    struct WorkerNoteJob final : public kdu_thread_job {
      std::atomic<int>* ack = nullptr;
      std::atomic<int>* go = nullptr;
      std::atomic<int>* done = nullptr;
      std::deque<DirectQueue>* qs = nullptr;
      int round = 0;
      WorkerNoteJob() { set_job_func(&WorkerNoteJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<WorkerNoteJob*>(base);
        for (int qi = 0; qi < kQueues; ++qi) {
          self->ack->store(qi + 1, std::memory_order_release);
          while (self->go->load(std::memory_order_acquire) < qi + 1) {
          }
          volatile int sink = 0;
          const int spin_iters =
              ((qi & 15) * 16 + (self->round % kRounds) * 2) * 8;
          for (int s = 0; s < spin_iters; ++s) {
            sink = sink + s;
          }
          (void)sink;
          (*self->qs)[qi].NoteAll(caller);
          self->done->store(qi + 1, std::memory_order_release);
        }
      }
    };

    for (int round = 0; round < kRounds; ++round) {
      std::deque<DirectQueue> qs;
      RecordingThreadQueue q_keep(1, "wo_keep");
      ASSERT_TRUE(env.attach_queue(&q_keep, nullptr, "wo_dom", 0, 0));
      std::vector<CountRaceJob> pre_jobs(kQueues * kPreBind);
      std::vector<CountRaceJob> last_jobs(kQueues);
      std::vector<std::atomic<int>> job_done(kQueues);
      std::vector<std::atomic<int>> expected(kQueues);
      for (int qi = 0; qi < kQueues; ++qi) {
        job_done[qi].store(0, std::memory_order_relaxed);
        expected[qi].store(kPreBind, std::memory_order_relaxed);
        last_jobs[qi].done_count = &job_done[qi];
        qs.emplace_back(kPreBind + 1);
        ASSERT_TRUE(env.attach_queue(&qs.back(), nullptr, "wo_dom", 0, 0));
        for (int i = 0; i < kPreBind; ++i) {
          CountRaceJob& pjob = pre_jobs[qi * kPreBind + i];
          pjob.done_count = &job_done[qi];
          qs.back().SchedOne(&pjob, &env, /*all_scheduled=*/false,
                             KDU_THREAD_JOB_AUTO_BIND_ONCE);
        }
      }

      std::atomic<int> ack{0};
      std::atomic<int> go{0};
      std::atomic<int> done{0};
      WorkerNoteJob wjob;
      wjob.ack = &ack;
      wjob.go = &go;
      wjob.done = &done;
      wjob.qs = &qs;
      wjob.round = round;
      DirectQueue q_ctrl(1);
      ASSERT_TRUE(env.attach_queue(&q_ctrl, nullptr, "wo_ctrl", 0, 0));
      kdu_thread_job* ctrl_ptr = &wjob;
      q_ctrl.BindJobs(&ctrl_ptr, 1);
      q_ctrl.SchedBatch(&ctrl_ptr, 1, &env, /*all_scheduled=*/true);

      for (int qi = 0; qi < kQueues; ++qi) {
        while (ack.load(std::memory_order_acquire) < qi + 1) {
          std::this_thread::yield();
        }
        go.store(qi + 1, std::memory_order_release);
        bool threw = false;
        try {
          qs[qi].SchedOne(&last_jobs[qi], &env, /*all_scheduled=*/true,
                          KDU_THREAD_JOB_AUTO_BIND_ONCE);
        } catch (kdu_exception) {
          threw = true;
        }
        if (!threw) {
          expected[qi].fetch_add(1, std::memory_order_release);
        }
        while (done.load(std::memory_order_acquire) < qi + 1) {
          std::this_thread::yield();
        }
      }
      q_ctrl.Complete(&env);
      EXPECT_TRUE(env.join(&q_ctrl));
      for (int qi = 0; qi < kQueues; ++qi) {
        const int exp = expected[qi].load(std::memory_order_acquire);
        while (job_done[qi].load(std::memory_order_acquire) < exp) {
          std::this_thread::yield();
        }
        qs[qi].Complete(&env);
        EXPECT_TRUE(env.join(&qs[qi]));
      }
      q_keep.NoteAllScheduled(&env);
      q_keep.Complete(&env);
      EXPECT_TRUE(env.join(&q_keep));
      error_handler_.TakeMessage();
    }
  }

  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, HighContentionDirtyPaletteHazardAndYieldWakeup) {
  struct CountJob final : public kdu_thread_job {
    std::atomic<int>* done = nullptr;
    CountJob() { set_job_func(&CountJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity*) {
      static_cast<CountJob*>(base)->done->fetch_add(1,
                                                    std::memory_order_release);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  for (int t = 0; t < 12; ++t) {
    ASSERT_TRUE(env.add_thread());
  }
  WarmupWorkers(env);
  env.set_yield_frequency(1);

  // Keep total domains <= 16 so the group owner never allocates active_sets[].
  std::vector<std::unique_ptr<RecordingThreadQueue>> poll_qs;
  for (int d = 0; d < 8; ++d) {
    auto q = std::make_unique<RecordingThreadQueue>(1, "poll_q");
    std::string dom = "hz_poll_" + std::to_string(d);
    ASSERT_TRUE(env.attach_queue(q.get(), nullptr, dom.c_str(), d, 0));
    poll_qs.push_back(std::move(q));
  }

  constexpr int kWaveJobs = 1540;
  for (int wave = 0; wave < 24; ++wave) {
    RecordingThreadQueue q_main(kWaveJobs, "hz_main");
    ASSERT_TRUE(env.attach_queue(&q_main, nullptr, "hz_main_dom", wave, 0));
    std::vector<CountJob> jobs(kWaveJobs);
    std::vector<kdu_thread_job*> ptrs(kWaveJobs);
    std::atomic<int> done{0};
    for (int i = 0; i < kWaveJobs; ++i) {
      jobs[i].done = &done;
      ptrs[i] = &jobs[i];
    }
    q_main.BindRange(ptrs.data(), kWaveJobs, 0);
    for (int off = 0; off < kWaveJobs; off += 7) {
      q_main.ScheduleBatch(ptrs.data() + off, 7, &env,
                           /*all_scheduled=*/(off + 7 == kWaveJobs));
      // Pace production close to consumption so owner palette cleaning
      // intersects live worker hazard pointers (thread_job_hzps).
      while ((off + 7) - done.load(std::memory_order_acquire) > 280) {
        std::this_thread::yield();
      }
    }
    while (done.load(std::memory_order_acquire) < kWaveJobs) {
      std::this_thread::yield();
    }
    q_main.Complete(&env);
    EXPECT_TRUE(env.join(&q_main));
  }

  for (auto& q : poll_qs) {
    q->NoteAllScheduled(&env);
    q->Complete(&env);
    EXPECT_TRUE(env.join(q.get()));
  }
  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, WwProcessJobsExceptionBreak) {
  struct ThrowGateJob final : public kdu_thread_job {
    RecordingThreadQueue* q = nullptr;
    std::atomic<bool>* worker_ready = nullptr;
    std::atomic<bool>* owner_in_job = nullptr;
    std::atomic<bool>* exc_reported = nullptr;
    ThrowGateJob() { set_job_func(&ThrowGateJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<ThrowGateJob*>(base);
      self->worker_ready->store(true, std::memory_order_release);
      while (!self->owner_in_job->load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      self->q->Complete(caller);
      caller->handle_exception(KDU_ERROR_EXCEPTION);
      self->exc_reported->store(true, std::memory_order_release);
    }
  };
  struct OwnerHoldJob final : public kdu_thread_job {
    RecordingThreadQueue* q = nullptr;
    std::atomic<bool>* owner_in_job = nullptr;
    std::atomic<bool>* exc_reported = nullptr;
    OwnerHoldJob() { set_job_func(&OwnerHoldJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<OwnerHoldJob*>(base);
      self->owner_in_job->store(true, std::memory_order_release);
      while (!self->exc_reported->load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      self->q->Complete(caller);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  ASSERT_TRUE(env.add_thread());
  WarmupWorkers(env);

  RecordingThreadQueue q_throw(1, "ww_exc_throw");
  RecordingThreadQueue q_own(1, "ww_exc_own");
  ASSERT_TRUE(env.attach_queue(&q_throw, nullptr, "ww_exc_throw_dom", 0, 0));
  ASSERT_TRUE(env.attach_queue(&q_own, nullptr, "ww_exc_own_dom", 0, 0));

  std::atomic<bool> worker_ready{false};
  std::atomic<bool> owner_in_job{false};
  std::atomic<bool> exc_reported{false};

  ThrowGateJob tjob;
  tjob.q = &q_throw;
  tjob.worker_ready = &worker_ready;
  tjob.owner_in_job = &owner_in_job;
  tjob.exc_reported = &exc_reported;
  q_throw.ScheduleOne(&tjob, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);

  while (!worker_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  OwnerHoldJob ojob;
  ojob.q = &q_own;
  ojob.owner_in_job = &owner_in_job;
  ojob.exc_reported = &exc_reported;
  q_own.ScheduleOne(&ojob, &env, /*all_scheduled=*/true,
                    KDU_THREAD_JOB_AUTO_BIND_ONCE);

  try {
    env.wait_for_condition("ww_exc_break", /*expect_foreign_wakeup=*/false);
  } catch (kdu_exception) {
  }

  kdu_exception exc1 = KDU_NULL_EXCEPTION;
  kdu_exception exc2 = KDU_NULL_EXCEPTION;
  EXPECT_FALSE(env.join(&q_throw, /*descendants_only=*/false, &exc1));
  EXPECT_FALSE(env.join(&q_own, /*descendants_only=*/false, &exc2));
  EXPECT_EQ(KDU_ERROR_EXCEPTION, exc1);
  EXPECT_FALSE(env.destroy());
  error_handler_.TakeMessage();
}

TEST_F(KduThreadsTest, WwProcessJobsWakeWaiterAndJoinCasRace) {
  struct WakeDummyJob final : public kdu_thread_job {
    RecordingThreadQueue* q = nullptr;
    WakeDummyJob() { set_job_func(&WakeDummyJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      static_cast<WakeDummyJob*>(base)->q->Complete(caller);
    }
  };
  struct GateSignalJob final : public kdu_thread_job {
    RecordingThreadQueue* q = nullptr;
    kdu_thread_entity_condition* cond = nullptr;
    std::atomic<bool>* w_ready = nullptr;
    std::atomic<bool>* owner_in_ww = nullptr;
    std::atomic<bool>* cond_signalled_now = nullptr;
    int spin = 0;
    GateSignalJob() { set_job_func(&GateSignalJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<GateSignalJob*>(base);
      self->w_ready->store(true, std::memory_order_release);
      while (!self->owner_in_ww->load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      volatile int sink = 0;
      for (int s = 0; s < self->spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
      caller->signal_condition(self->cond);
      self->cond_signalled_now->store(true, std::memory_order_release);
      self->q->Complete(caller);
    }
  };
  struct WakeTriggerJob final : public kdu_thread_job {
    RecordingThreadQueue* q_trig = nullptr;
    RecordingThreadQueue* q_wake = nullptr;
    kdu_thread_job* wake_ptr = nullptr;
    std::atomic<bool>* trig_ready = nullptr;
    std::atomic<bool>* cond_signalled_now = nullptr;
    int post_spin = 0;
    WakeTriggerJob() { set_job_func(&WakeTriggerJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<WakeTriggerJob*>(base);
      self->trig_ready->store(true, std::memory_order_release);
      while (!self->cond_signalled_now->load(std::memory_order_acquire)) {
      }
      volatile int sink = 0;
      for (int s = 0; s < self->post_spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
      // Move idle wwid=0 from (0,1)->(1,0) between lines 4169 and 4175 so
      // !wake_waiter(0) executes wait_managers[0].enter_wait() (lines
      // 4178-4179).
      self->q_wake->ScheduleBatch(&self->wake_ptr, 1, caller,
                                  /*all_scheduled=*/true);
      self->q_trig->Complete(caller);
    }
  };
  struct OwnerSyncJob final : public kdu_thread_job {
    RecordingThreadQueue* q = nullptr;
    std::atomic<bool>* owner_in_ww = nullptr;
    OwnerSyncJob() { set_job_func(&OwnerSyncJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<OwnerSyncJob*>(base);
      self->owner_in_ww->store(true, std::memory_order_release);
      self->q->Complete(caller);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());
  WarmupWorkers(env);

  for (int i = 0; i < 300; ++i) {
    RecordingThreadQueue q_gate(1, "race_gate");
    RecordingThreadQueue q_trig(1, "race_trig");
    RecordingThreadQueue q_own(1, "race_own");
    RecordingThreadQueue q_wake(1, "race_wake");
    ASSERT_TRUE(env.attach_queue(&q_gate, nullptr, "ww_race_g", i, 0));
    ASSERT_TRUE(env.attach_queue(&q_trig, nullptr, "ww_race_t", i, 0));
    ASSERT_TRUE(env.attach_queue(&q_own, nullptr, "ww_race_o", i, 0));
    ASSERT_TRUE(env.attach_queue(&q_wake, nullptr, "ww_race_w", i, 0));

    WakeDummyJob wjob;
    wjob.q = &q_wake;
    kdu_thread_job* wake_ptr = &wjob;
    q_wake.BindRange(&wake_ptr, 1, 0);

    kdu_thread_entity_condition* cond = env.get_condition();
    std::atomic<bool> w_ready{false};
    std::atomic<bool> trig_ready{false};
    std::atomic<bool> owner_in_ww{false};
    std::atomic<bool> cond_signalled_now{false};

    GateSignalJob gjob;
    gjob.q = &q_gate;
    gjob.cond = cond;
    gjob.w_ready = &w_ready;
    gjob.owner_in_ww = &owner_in_ww;
    gjob.cond_signalled_now = &cond_signalled_now;
    gjob.spin = (i % 6) * 200;
    q_gate.ScheduleOne(&gjob, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);

    WakeTriggerJob tjob;
    tjob.q_trig = &q_trig;
    tjob.q_wake = &q_wake;
    tjob.wake_ptr = wake_ptr;
    tjob.trig_ready = &trig_ready;
    tjob.cond_signalled_now = &cond_signalled_now;
    tjob.post_spin = i / 6;
    q_trig.ScheduleOne(&tjob, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);

    while (!w_ready.load(std::memory_order_acquire) ||
           !trig_ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    OwnerSyncJob ojob;
    ojob.q = &q_own;
    ojob.owner_in_ww = &owner_in_ww;
    q_own.ScheduleOne(&ojob, &env, /*all_scheduled=*/true,
                      KDU_THREAD_JOB_AUTO_BIND_ONCE);

    env.wait_for_condition("ww_wake_race", /*expect_foreign_wakeup=*/false);
    EXPECT_TRUE(env.join(&q_gate));
    EXPECT_TRUE(env.join(&q_trig));
    EXPECT_TRUE(env.join(&q_own));
    EXPECT_TRUE(env.join(&q_wake));
  }

  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, ForeignWakeupDeadlockBreak) {
  // Path 1: Owner reaches second finish_deadlock_test(ext_wakers=true) in
  // ww_process_jobs() with zero workers before foreign thread signals.
  {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());

    kdu_thread_entity_condition* cond = env.get_condition();
    ASSERT_NE(nullptr, cond);
    std::atomic<bool> about_to_wait{false};
    std::thread foreign_waker([&] {
      while (!about_to_wait.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    about_to_wait.store(true, std::memory_order_release);
    env.wait_for_condition("ww_foreign_dl_break",
                           /*expect_foreign_wakeup=*/true);
    foreign_waker.join();
    EXPECT_TRUE(env.destroy());
  }

  // Path 2: Last active worker finishes after owner is idle with
  // expect_foreign_wakeup=true, hitting process_jobs() second
  // finish_deadlock_test(ext_wakers=true) before foreign thread signals.
  {
    struct SlowFinishJob final : public kdu_thread_job {
      RecordingThreadQueue* q = nullptr;
      std::atomic<bool>* worker_started = nullptr;
      std::atomic<bool>* owner_waiting = nullptr;
      SlowFinishJob() { set_job_func(&SlowFinishJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<SlowFinishJob*>(base);
        self->worker_started->store(true, std::memory_order_release);
        while (!self->owner_waiting->load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        self->q->Complete(caller);
      }
    };

    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    RecordingThreadQueue q(1, "nww_ext_dl");
    ASSERT_TRUE(env.attach_queue(&q, nullptr, "nww_ext_dom", 0, 0));
    kdu_thread_entity_condition* cond = env.get_condition();
    ASSERT_NE(nullptr, cond);

    std::atomic<bool> worker_started{false};
    std::atomic<bool> owner_waiting{false};
    SlowFinishJob job;
    job.q = &q;
    job.worker_started = &worker_started;
    job.owner_waiting = &owner_waiting;
    q.ScheduleOne(&job, &env, /*all_scheduled=*/true,
                  KDU_THREAD_JOB_AUTO_BIND_ONCE);

    while (!worker_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::thread foreign_waker([&] {
      while (!owner_waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    owner_waiting.store(true, std::memory_order_release);
    env.wait_for_condition("nww_foreign_dl_break",
                           /*expect_foreign_wakeup=*/true);
    foreign_waker.join();
    EXPECT_TRUE(env.join(&q));
    EXPECT_TRUE(env.destroy());
  }
}

TEST_F(KduThreadsTest, JoinCompletionStateCasRace) {
  class NotifyRaceQueue final : public kdu_thread_queue {
   public:
    std::atomic<bool> in_notify{false};
    int spin = 0;
    int get_max_jobs() override { return 1; }
    void SchedOne(kdu_thread_job* job, kdu_thread_entity* caller) {
      schedule_job(job, caller, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    void Complete(kdu_thread_entity* caller) { all_done(caller); }

   protected:
    void notify_join(kdu_thread_entity* /*caller*/) override {
      in_notify.store(true, std::memory_order_release);
      volatile int sink = 0;
      for (int s = 0; s < spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
    }
  };
  class ChildRaceQueue final : public kdu_thread_queue {
   public:
    int get_max_jobs() override { return 1; }
    void SchedOne(kdu_thread_job* job, kdu_thread_entity* caller) {
      schedule_job(job, caller, /*all_scheduled=*/true,
                   KDU_THREAD_JOB_AUTO_BIND_ONCE);
    }
    void Complete(kdu_thread_entity* caller) { all_done(caller); }
  };
  constexpr int kNumChildren = 3;
  struct ChildCompleteJob final : public kdu_thread_job {
    NotifyRaceQueue* parent = nullptr;
    ChildRaceQueue* child = nullptr;
    std::atomic<int>* workers_ready = nullptr;
    std::atomic<int>* children_done = nullptr;
    std::atomic<bool>* exited = nullptr;
    int spin = 0;
    ChildCompleteJob() { set_job_func(&ChildCompleteJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<ChildCompleteJob*>(base);
      self->workers_ready->fetch_add(1, std::memory_order_release);
      while (!self->parent->in_notify.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      volatile int sink = 0;
      for (int s = 0; s < self->spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
      self->child->Complete(caller);
      self->children_done->fetch_add(1, std::memory_order_release);
      self->exited->store(true, std::memory_order_release);
    }
  };
  struct WaitNotifyJob final : public kdu_thread_job {
    NotifyRaceQueue* q = nullptr;
    std::atomic<int>* workers_ready = nullptr;
    std::atomic<int>* children_done = nullptr;
    std::atomic<bool>* worker_done = nullptr;
    WaitNotifyJob() { set_job_func(&WaitNotifyJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<WaitNotifyJob*>(base);
      self->workers_ready->fetch_add(1, std::memory_order_release);
      while (self->children_done->load(std::memory_order_acquire) <
             kNumChildren) {
        std::this_thread::yield();
      }
      self->q->Complete(caller);
      self->worker_done->store(true, std::memory_order_release);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  for (int t = 0; t < kNumChildren + 1; ++t) {
    ASSERT_TRUE(env.add_thread());
  }
  WarmupWorkers(env);

  for (int i = 0; i < 1024; ++i) {
    NotifyRaceQueue q;
    q.spin = i % 256;
    ChildRaceQueue children[kNumChildren];
    ASSERT_TRUE(env.attach_queue(&q, nullptr, "jcas_dom", i, 0));
    for (int k = 0; k < kNumChildren; ++k) {
      ASSERT_TRUE(env.attach_queue(&children[k], &q, "jcas_dom", i, 0));
    }
    std::atomic<int> workers_ready{0};
    std::atomic<int> children_done{0};
    std::atomic<bool> child_exited[kNumChildren]{};
    std::atomic<bool> worker_done{false};
    ChildCompleteJob cjobs[kNumChildren];
    for (int k = 0; k < kNumChildren; ++k) {
      cjobs[k].parent = &q;
      cjobs[k].child = &children[k];
      cjobs[k].workers_ready = &workers_ready;
      cjobs[k].children_done = &children_done;
      cjobs[k].exited = &child_exited[k];
      cjobs[k].spin = k * 32;
      children[k].SchedOne(&cjobs[k], &env);
    }
    WaitNotifyJob job;
    job.q = &q;
    job.workers_ready = &workers_ready;
    job.children_done = &children_done;
    job.worker_done = &worker_done;
    q.SchedOne(&job, &env);
    while (workers_ready.load(std::memory_order_acquire) < kNumChildren + 1) {
      std::this_thread::yield();
    }
    EXPECT_TRUE(env.join(nullptr));
    while (!worker_done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int k = 0; k < kNumChildren; ++k) {
      while (!child_exited[k].load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }
  }

  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, MultiWorkerSinglePaletteHazardContention) {
  struct CountJob final : public kdu_thread_job {
    std::atomic<int>* done = nullptr;
    CountJob() { set_job_func(&CountJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity*) {
      static_cast<CountJob*>(base)->done->fetch_add(1,
                                                    std::memory_order_release);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  for (int t = 0; t < 31; ++t) {
    ASSERT_TRUE(env.add_thread());
  }
  WarmupWorkers(env);
  env.set_yield_frequency(0);

  // Part 1: Owner pushes single-job palettes with minimal throttling while 31
  // workers dequeue concurrently.
  {
    constexpr int kTotalJobs = 4096;
    RecordingThreadQueue q(kTotalJobs, "single_plt");
    ASSERT_TRUE(env.attach_queue(&q, nullptr, "single_plt_dom", 0, 0));
    std::vector<CountJob> jobs(kTotalJobs);
    std::vector<kdu_thread_job*> ptrs(kTotalJobs);
    std::atomic<int> done{0};
    for (int i = 0; i < kTotalJobs; ++i) {
      jobs[i].done = &done;
      ptrs[i] = &jobs[i];
    }
    q.BindRange(ptrs.data(), kTotalJobs, 0);
    for (int i = 0; i < kTotalJobs; ++i) {
      q.ScheduleBatch(ptrs.data() + i, 1, &env,
                      /*all_scheduled=*/(i + 1 == kTotalJobs));
      while ((i + 1) - done.load(std::memory_order_acquire) > 256) {
        std::this_thread::yield();
      }
    }
    while (done.load(std::memory_order_acquire) < kTotalJobs) {
      std::this_thread::yield();
    }
    q.Complete(&env);
    EXPECT_TRUE(env.join(&q));
    EXPECT_EQ(kTotalJobs, done.load(std::memory_order_acquire));
  }

  // Part 2: Nine concurrent enqueuers (owner + 8 workers) simultaneously push
  // 16384 single-job batches each without throttling onto one shared queue
  // while the remaining 23 workers dequeue continuously, holding hazard
  // pointers across both `enqueue_palettes` tail CAS retries and `get_job`
  // head CAS retries during `clean_dirty_palettes`.
  {
    constexpr int kWorkerEnqueuers = 6;
    constexpr int kTotalEnqueuers = kWorkerEnqueuers + 1;
    constexpr int kJobsPerEnqueuer = 3072;
    constexpr int kTotalJobs = kTotalEnqueuers * kJobsPerEnqueuer;

    struct EnqueuerJob final : public kdu_thread_job {
      RecordingThreadQueue* target = nullptr;
      kdu_thread_job** jobs = nullptr;
      int count = 0;
      std::atomic<int>* enqueuers_done = nullptr;
      EnqueuerJob() { set_job_func(&EnqueuerJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
        auto* self = static_cast<EnqueuerJob*>(base);
        for (int i = 0; i < self->count; ++i) {
          self->target->ScheduleBatch(self->jobs + i, 1, caller, false);
        }
        self->enqueuers_done->fetch_add(1, std::memory_order_release);
      }
    };

    RecordingThreadQueue q2(kTotalJobs, "multi_enq");
    RecordingThreadQueue q_enq(kWorkerEnqueuers, "enq_drivers");
    ASSERT_TRUE(env.attach_queue(&q2, nullptr, "multi_enq_dom", 0, 0));
    ASSERT_TRUE(env.attach_queue(&q_enq, nullptr, "enq_drv_dom", 0, 0));

    std::vector<CountJob> jobs(kTotalJobs);
    std::vector<kdu_thread_job*> ptrs(kTotalJobs);
    std::atomic<int> done{0};
    for (int i = 0; i < kTotalJobs; ++i) {
      jobs[i].done = &done;
      ptrs[i] = &jobs[i];
    }
    q2.BindRange(ptrs.data(), kTotalJobs, 0);

    std::atomic<int> enqueuers_done{0};
    EnqueuerJob drivers[kWorkerEnqueuers];
    kdu_thread_job* driver_ptrs[kWorkerEnqueuers];
    for (int e = 0; e < kWorkerEnqueuers; ++e) {
      drivers[e].target = &q2;
      drivers[e].jobs = ptrs.data() + e * kJobsPerEnqueuer;
      drivers[e].count = kJobsPerEnqueuer;
      drivers[e].enqueuers_done = &enqueuers_done;
      driver_ptrs[e] = &drivers[e];
    }
    q_enq.BindRange(driver_ptrs, kWorkerEnqueuers, 0);
    q_enq.ScheduleBatch(driver_ptrs, kWorkerEnqueuers, &env,
                        /*all_scheduled=*/true);

    kdu_thread_job** owner_ptrs =
        ptrs.data() + kWorkerEnqueuers * kJobsPerEnqueuer;
    for (int i = 0; i < kJobsPerEnqueuer; ++i) {
      q2.ScheduleBatch(owner_ptrs + i, 1, &env, false);
    }
    while (enqueuers_done.load(std::memory_order_acquire) < kWorkerEnqueuers) {
      std::this_thread::yield();
    }
    q2.NoteAllScheduled(&env);
    while (done.load(std::memory_order_acquire) < kTotalJobs) {
      std::this_thread::yield();
    }
    q_enq.Complete(&env);
    q2.Complete(&env);
    EXPECT_TRUE(env.join(&q_enq));
    EXPECT_TRUE(env.join(&q2));
    EXPECT_EQ(kTotalJobs, done.load(std::memory_order_acquire));
  }

  // Part 3: Synchronized wave stampedes keep ~80+ single-job palettes queued
  // in `q3` across 31 contending workers while owner crosses 256-palette
  // `clean_dirty_palettes` boundaries mid-stampede.
  {
    constexpr int kWorkers = 31;
    constexpr int kWaves = 32;
    constexpr int kPreload = 96;
    constexpr int kBurst = 192;
    constexpr int kWaveJobs = kPreload + kBurst;
    constexpr int kTotalJobs = kWaves * kWaveJobs;

    struct HoldJob final : public kdu_thread_job {
      std::atomic<int>* started = nullptr;
      std::atomic<bool>* go = nullptr;
      std::atomic<int>* exited = nullptr;
      HoldJob() { set_job_func(&HoldJob::Run); }
      static void Run(kdu_thread_job* base, kdu_thread_entity*) {
        auto* self = static_cast<HoldJob*>(base);
        self->started->fetch_add(1, std::memory_order_release);
        while (!self->go->load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        self->exited->fetch_add(1, std::memory_order_release);
      }
    };

    RecordingThreadQueue q3(kTotalJobs, "burst_q");
    RecordingThreadQueue q_hold(kWaves * kWorkers, "burst_hold");
    ASSERT_TRUE(env.attach_queue(&q3, nullptr, "burst_q_dom", 0, 0));
    ASSERT_TRUE(env.attach_queue(&q_hold, nullptr, "burst_h_dom", 0, 0));

    std::vector<CountJob> jobs(kTotalJobs);
    std::vector<kdu_thread_job*> ptrs(kTotalJobs);
    std::atomic<int> done{0};
    for (int i = 0; i < kTotalJobs; ++i) {
      jobs[i].done = &done;
      ptrs[i] = &jobs[i];
    }
    q3.BindRange(ptrs.data(), kTotalJobs, 0);

    for (int w = 0; w < kWaves; ++w) {
      std::atomic<int> started{0};
      std::atomic<bool> go{false};
      std::atomic<int> exited{0};
      HoldJob hjobs[kWorkers];
      kdu_thread_job* hptrs[kWorkers];
      for (int t = 0; t < kWorkers; ++t) {
        hjobs[t].started = &started;
        hjobs[t].go = &go;
        hjobs[t].exited = &exited;
        hptrs[t] = &hjobs[t];
      }
      q_hold.BindRange(hptrs, kWorkers, w * kWorkers);
      q_hold.ScheduleBatch(hptrs, kWorkers, &env, false);
      while (started.load(std::memory_order_acquire) < kWorkers) {
        std::this_thread::yield();
      }

      const int base = w * kWaveJobs;
      for (int i = 0; i < kPreload; ++i) {
        q3.ScheduleBatch(ptrs.data() + base + i, 1, &env, false);
      }
      go.store(true, std::memory_order_release);
      while (done.load(std::memory_order_acquire) < base + 16) {
      }
      for (int i = kPreload; i < kWaveJobs; ++i) {
        q3.ScheduleBatch(
            ptrs.data() + base + i, 1, &env,
            /*all_scheduled=*/(w + 1 == kWaves && i + 1 == kWaveJobs));
      }
      while (done.load(std::memory_order_acquire) < base + kWaveJobs ||
             exited.load(std::memory_order_acquire) < kWorkers) {
        std::this_thread::yield();
      }
    }
    q_hold.Complete(&env);
    q3.Complete(&env);
    EXPECT_TRUE(env.join(&q_hold));
    EXPECT_TRUE(env.join(&q3));
    EXPECT_EQ(kTotalJobs, done.load(std::memory_order_acquire));
  }

  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, TerminateIdleDomainSequenceOnNewSequence) {
  struct NoteSeqJob final : public kdu_thread_job {
    RecordingThreadQueue* q_seq = nullptr;
    RecordingThreadQueue* q_ctrl = nullptr;
    std::atomic<bool>* ready = nullptr;
    std::atomic<bool>* go = nullptr;
    int spin = 0;
    NoteSeqJob() { set_job_func(&NoteSeqJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<NoteSeqJob*>(base);
      self->ready->store(true, std::memory_order_release);
      while (!self->go->load(std::memory_order_acquire)) {
      }
      volatile int sink = 0;
      for (int s = 0; s < self->spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
      self->q_seq->NoteAllScheduled(caller);
      self->q_ctrl->Complete(caller);
    }
  };

  for (int g = 0; g < 8; ++g) {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    ASSERT_TRUE(env.add_thread());
    WarmupWorkers(env);

    // Hold seq_idx=0 open without `all_scheduled` so `remove_unused_sequences`
    // cannot populate `free_sequences`, forcing `add_domain_sequence` to run
    // `new kd_thread_domain_sequence` inside the active_state window.
    RecordingThreadQueue q_hold(1, "seq_hold");
    ASSERT_TRUE(env.attach_queue(&q_hold, nullptr, "term_seq", 0, 0));

    for (int i = 0; i < 64; ++i) {
      RecordingThreadQueue q0(1, "seq_old");
      RecordingThreadQueue q1(1, "seq_new");
      RecordingThreadQueue q_ctrl(1, "seq_ctrl");
      ASSERT_TRUE(env.attach_queue(&q0, nullptr, "term_seq", i * 2 + 2, 0));
      ASSERT_TRUE(env.attach_queue(&q_ctrl, nullptr, "term_ctrl", i, 0));

      std::atomic<bool> ready{false};
      std::atomic<bool> go{false};
      NoteSeqJob job;
      job.q_seq = &q0;
      job.q_ctrl = &q_ctrl;
      job.ready = &ready;
      job.go = &go;
      job.spin = (g * 64 + i) * 4;
      q_ctrl.ScheduleOne(&job, &env, /*all_scheduled=*/true,
                         KDU_THREAD_JOB_AUTO_BIND_ONCE);
      while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      go.store(true, std::memory_order_release);
      ASSERT_TRUE(env.attach_queue(&q1, nullptr, "term_seq", i * 2 + 3, 0));
      EXPECT_TRUE(env.join(&q_ctrl));
      q0.Complete(&env);
      q1.NoteAllScheduled(&env);
      q1.Complete(&env);
      EXPECT_TRUE(env.join(&q0));
      EXPECT_TRUE(env.join(&q1));
    }

    RecordingThreadQueue q_det0(1, "seq_det0");
    RecordingThreadQueue q_det1(1, "seq_det1");
    ASSERT_TRUE(env.attach_queue(&q_det0, nullptr, "term_seq", 2000, 0));
    q_det0.NoteAllScheduled(&env);
    ASSERT_TRUE(env.attach_queue(&q_det1, nullptr, "term_seq", 2001, 0));
    q_det0.Complete(&env);
    q_det1.NoteAllScheduled(&env);
    q_det1.Complete(&env);
    EXPECT_TRUE(env.join(&q_det0));
    EXPECT_TRUE(env.join(&q_det1));

    q_hold.NoteAllScheduled(&env);
    q_hold.Complete(&env);
    EXPECT_TRUE(env.join(&q_hold));
    EXPECT_TRUE(env.destroy());
  }
}

TEST_F(KduThreadsTest, YieldWakeAnotherWorker) {
  struct BarrierJob final : public kdu_thread_job {
    std::atomic<int>* started = nullptr;
    std::atomic<bool>* release = nullptr;
    std::atomic<int>* done = nullptr;
    BarrierJob() { set_job_func(&BarrierJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity*) {
      auto* self = static_cast<BarrierJob*>(base);
      self->started->fetch_add(1, std::memory_order_acq_rel);
      while (!self->release->load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      self->done->fetch_add(1, std::memory_order_release);
    }
  };
  struct FastDoneJob final : public kdu_thread_job {
    std::atomic<int>* done = nullptr;
    FastDoneJob() { set_job_func(&FastDoneJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity*) {
      static_cast<FastDoneJob*>(base)->done->fetch_add(
          1, std::memory_order_release);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  for (int t = 0; t < 5; ++t) {
    ASSERT_TRUE(env.add_thread());
  }
  WarmupWorkers(env);
  env.set_yield_frequency(1);

  for (int wave = 0; wave < 150; ++wave) {
    RecordingThreadQueue q(8, "yield_wake");
    ASSERT_TRUE(env.attach_queue(&q, nullptr, "yw_dom", wave, 0));

    std::atomic<int> started{0};
    std::atomic<bool> release{false};
    std::atomic<int> done{0};
    BarrierJob barriers[5];
    FastDoneJob fasts[3];
    kdu_thread_job* ptrs[8];
    for (int i = 0; i < 5; ++i) {
      barriers[i].started = &started;
      barriers[i].release = &release;
      barriers[i].done = &done;
      ptrs[i] = &barriers[i];
    }
    for (int i = 0; i < 3; ++i) {
      fasts[i].done = &done;
      ptrs[5 + i] = &fasts[i];
    }
    q.BindRange(ptrs, 8, 0);
    q.ScheduleBatch(ptrs, 5, &env, /*all_scheduled=*/false);
    while (started.load(std::memory_order_acquire) < 5) {
      std::this_thread::yield();
    }
    q.ScheduleBatch(ptrs + 5, 1, &env, /*all_scheduled=*/false);
    release.store(true, std::memory_order_release);
    while (done.load(std::memory_order_acquire) < 6) {
      std::this_thread::yield();
    }
    q.ScheduleBatch(ptrs + 6, 2, &env, /*all_scheduled=*/true);
    while (done.load(std::memory_order_acquire) < 8) {
      std::this_thread::yield();
    }
    q.Complete(&env);
    EXPECT_TRUE(env.join(&q));
  }

  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, WwProcessJobsExceptionAfterStartHunting) {
  struct ExcRaceJob final : public kdu_thread_job {
    std::atomic<bool>* ready = nullptr;
    std::atomic<bool>* go = nullptr;
    int spin = 0;
    ExcRaceJob() { set_job_func(&ExcRaceJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<ExcRaceJob*>(base);
      self->ready->store(true, std::memory_order_release);
      while (!self->go->load(std::memory_order_acquire)) {
      }
      volatile int sink = 0;
      for (int s = 0; s < self->spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
      caller->handle_exception(99);
    }
  };

  for (int round = 0; round < 24; ++round) {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    for (int t = 0; t < 8; ++t) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);

    RecordingThreadQueue q_ctrl(1, "exc_ctrl");
    ASSERT_TRUE(env.attach_queue(&q_ctrl, nullptr, "exc_dom", 0, 0));
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    ExcRaceJob job;
    job.ready = &ready;
    job.go = &go;
    job.spin = round * 64;
    q_ctrl.ScheduleOne(&job, &env, /*all_scheduled=*/true,
                       KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    try {
      (void)env.join(&q_ctrl);
    } catch (kdu_exception) {
    }
    EXPECT_FALSE(env.destroy());
  }
}

TEST_F(KduThreadsTest, WwProcessJobsReturnJobWakesIdleWorker) {
  struct ProbeJob final : public kdu_thread_job {
    std::atomic<bool>* probe_ran = nullptr;
    int post_spin = 0;
    ProbeJob() { set_job_func(&ProbeJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity*) {
      auto* self = static_cast<ProbeJob*>(base);
      self->probe_ran->store(true, std::memory_order_release);
      volatile int sink = 0;
      for (int s = 0; s < self->post_spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
    }
  };
  struct SafeGateJob final : public kdu_thread_job {
    std::atomic<int>* started = nullptr;
    std::atomic<bool>* release_gate = nullptr;
    std::atomic<int>* done = nullptr;
    SafeGateJob() { set_job_func(&SafeGateJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity*) {
      auto* self = static_cast<SafeGateJob*>(base);
      self->started->fetch_add(1, std::memory_order_release);
      while (!self->release_gate->load(std::memory_order_acquire)) {
      }
      self->done->fetch_add(1, std::memory_order_release);
    }
  };
  constexpr int kActiveSafeQueues = 8;
  constexpr int kIdleSafeQueues = 16;
  constexpr int kWaves = 96;
  struct ControllerJob final : public kdu_thread_job {
    RecordingThreadQueue* q_safe[kActiveSafeQueues]{};
    RecordingThreadQueue* q_idle[kIdleSafeQueues]{};
    RecordingThreadQueue* q_ping = nullptr;
    RecordingThreadQueue* q_hold = nullptr;
    std::atomic<bool>* owner_joining = nullptr;
    ControllerJob() { set_job_func(&ControllerJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<ControllerJob*>(base);
      while (!self->owner_joining->load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      constexpr int kWaveJobs = kActiveSafeQueues;
      constexpr int kFreeWorkers = 7;
      ProbeJob probes[kWaves];
      for (int w = 0; w < kWaves; ++w) {
        SafeGateJob jobs[kWaveJobs];
        kdu_thread_job* ptrs[kWaveJobs];
        std::atomic<int> started{0};
        std::atomic<bool> release_gate{false};
        std::atomic<int> done{0};
        std::atomic<bool> probe_ran{false};
        for (int j = 0; j < kWaveJobs; ++j) {
          jobs[j].started = &started;
          jobs[j].release_gate = &release_gate;
          jobs[j].done = &done;
          ptrs[j] = &jobs[j];
        }
        probes[w].probe_ran = &probe_ran;
        probes[w].post_spin = (w & 1) ? (((w >> 1) % 8) * 16) : 0;
        kdu_thread_job* probe_ptr = &probes[w];
        for (int d = 0; d < kActiveSafeQueues; ++d) {
          self->q_safe[d]->BindRange(&ptrs[d], 1, w);
        }
        self->q_ping->BindRange(&probe_ptr, 1, w);
        // Pin all 7 free workers inside SafeGateJob while 1 SAFE_CONTEXT job
        // remains queued, then wake owner with ProbeJob. Once owner runs
        // ProbeJob and enters `dequeue_general_job(false)` across 24 safe
        // domains after `ww_reserve_job`, releasing the 7 workers lets >=1 go
        // idle while `ControllerJob` holds `done < kWaveJobs` until `owner`
        // reaches `ww_return_job() > 0` and wakes 1 worker at line 4146.
        for (int d = 0; d < kActiveSafeQueues; ++d) {
          self->q_safe[d]->ScheduleBatch(&ptrs[d], 1, caller,
                                         /*all_scheduled=*/(w + 1 == kWaves));
        }
        while (started.load(std::memory_order_acquire) < kFreeWorkers) {
          std::this_thread::yield();
        }
        self->q_ping->ScheduleBatch(&probe_ptr, 1, caller,
                                    /*all_scheduled=*/(w + 1 == kWaves));
        while (!probe_ran.load(std::memory_order_acquire)) {
        }
        volatile int sink = 0;
        const int release_spin = (w & 1) ? 0 : (((w >> 1) % 32) * 4);
        for (int s = 0; s < release_spin; ++s) {
          sink = sink + s;
        }
        (void)sink;
        release_gate.store(true, std::memory_order_release);
        while (done.load(std::memory_order_acquire) < kWaveJobs) {
          std::this_thread::yield();
        }
      }
      self->q_ping->Complete(caller);
      for (int d = 0; d < kActiveSafeQueues; ++d) {
        self->q_safe[d]->Complete(caller);
      }
      for (int d = 0; d < kIdleSafeQueues; ++d) {
        self->q_idle[d]->NoteAllScheduled(caller);
        self->q_idle[d]->Complete(caller);
      }
      self->q_hold->Complete(caller);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  for (int t = 0; t < 8; ++t) {
    ASSERT_TRUE(env.add_thread());
  }
  WarmupWorkers(env);
  env.set_yield_frequency(0);

  RecordingThreadQueue q_hold(1, "ret_hold");
  std::deque<RecordingThreadQueue> q_safe;
  for (int d = 0; d < kActiveSafeQueues; ++d) {
    q_safe.emplace_back(kWaves, "ret_safe");
  }
  std::deque<RecordingThreadQueue> q_idle;
  for (int d = 0; d < kIdleSafeQueues; ++d) {
    q_idle.emplace_back(0, "ret_idle");
  }
  RecordingThreadQueue q_ping(kWaves, "ret_ping");
  ASSERT_TRUE(env.attach_queue(&q_hold, nullptr, "ret_dom", 0, 0));
  const char* safe_doms[kActiveSafeQueues] = {
      "ret_safe_0", "ret_safe_1", "ret_safe_2", "ret_safe_3",
      "ret_safe_4", "ret_safe_5", "ret_safe_6", "ret_safe_7"};
  const char* idle_doms[kIdleSafeQueues] = {
      "ret_idle_0",  "ret_idle_1",  "ret_idle_2",  "ret_idle_3",
      "ret_idle_4",  "ret_idle_5",  "ret_idle_6",  "ret_idle_7",
      "ret_idle_8",  "ret_idle_9",  "ret_idle_10", "ret_idle_11",
      "ret_idle_12", "ret_idle_13", "ret_idle_14", "ret_idle_15"};
  for (int d = 0; d < kActiveSafeQueues; ++d) {
    ASSERT_TRUE(env.attach_queue(&q_safe[d], nullptr, safe_doms[d], 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
  }
  for (int d = 0; d < kIdleSafeQueues; ++d) {
    ASSERT_TRUE(env.attach_queue(&q_idle[d], nullptr, idle_doms[d], 0,
                                 KDU_THREAD_QUEUE_SAFE_CONTEXT));
  }
  ASSERT_TRUE(env.attach_queue(&q_ping, nullptr, "ret_ping_dom", 0, 0));

  std::atomic<bool> owner_joining{false};
  ControllerJob ctrl;
  for (int d = 0; d < kActiveSafeQueues; ++d) {
    ctrl.q_safe[d] = &q_safe[d];
  }
  for (int d = 0; d < kIdleSafeQueues; ++d) {
    ctrl.q_idle[d] = &q_idle[d];
  }
  ctrl.q_ping = &q_ping;
  ctrl.q_hold = &q_hold;
  ctrl.owner_joining = &owner_joining;
  q_hold.ScheduleOne(&ctrl, &env, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);
  owner_joining.store(true, std::memory_order_release);
  EXPECT_TRUE(env.join(&q_hold));
  for (int d = 0; d < kActiveSafeQueues; ++d) {
    EXPECT_TRUE(env.join(&q_safe[d]));
  }
  for (int d = 0; d < kIdleSafeQueues; ++d) {
    EXPECT_TRUE(env.join(&q_idle[d]));
  }
  EXPECT_TRUE(env.join(&q_ping));
  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, WwProcessJobsWakeWaiterPreemptedEnterWait) {
  struct NoopJob final : public kdu_thread_job {
    std::atomic<bool>* done = nullptr;
    NoopJob() { set_job_func(&NoopJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity*) {
      static_cast<NoopJob*>(base)->done->store(true, std::memory_order_release);
    }
  };
  struct UnsafeWakerJob final : public kdu_thread_job {
    RecordingThreadQueue* q_unsafe = nullptr;
    NoopJob* unsafe_job = nullptr;
    std::atomic<bool>* ready = nullptr;
    std::atomic<bool>* done = nullptr;
    int spin = 0;
    UnsafeWakerJob() { set_job_func(&UnsafeWakerJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<UnsafeWakerJob*>(base);
      while (!self->ready->load(std::memory_order_acquire)) {
      }
      volatile int sink = 0;
      for (int s = 0; s < self->spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
      self->q_unsafe->ScheduleOne(self->unsafe_job, caller,
                                  /*all_scheduled=*/true,
                                  KDU_THREAD_JOB_AUTO_BIND_ONCE);
      self->done->store(true, std::memory_order_release);
    }
  };

  ScopedThreadEntity scoped;
  kdu_thread_entity& env = scoped.entity;
  env.create();
  ASSERT_TRUE(env.exists());
  ASSERT_TRUE(env.add_thread());
  WarmupWorkers(env);

  for (int i = 0; i < 512; ++i) {
    RecordingThreadQueue q_w0(1, "preempt_w0");
    RecordingThreadQueue q_unsafe(1, "preempt_unsafe");
    ASSERT_TRUE(env.attach_queue(&q_w0, nullptr, "pw0_dom", i, 0));
    ASSERT_TRUE(env.attach_queue(&q_unsafe, nullptr, "pu_dom", i, 0));

    kdu_thread_entity_condition* cond = env.get_condition();
    ASSERT_NE(nullptr, cond);
    std::atomic<bool> ready{false};
    std::atomic<bool> w0_done{false};
    std::atomic<bool> unsafe_done{false};
    NoopJob unsafe_job;
    unsafe_job.done = &unsafe_done;
    UnsafeWakerJob w0_job;
    w0_job.q_unsafe = &q_unsafe;
    w0_job.unsafe_job = &unsafe_job;
    w0_job.ready = &ready;
    w0_job.done = &w0_done;
    w0_job.spin = (i % 256) * 4;
    q_w0.ScheduleOne(&w0_job, &env, /*all_scheduled=*/true,
                     KDU_THREAD_JOB_AUTO_BIND_ONCE);

    std::thread foreign_waker([&] {
      while (!ready.load(std::memory_order_acquire)) {
      }
      volatile int sink = 0;
      const int f_spin = 150 + (i / 256) * 150;
      for (int s = 0; s < f_spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
      env.signal_condition(cond, KDU_FOREIGN_SIGNAL_EXPECTED);
    });
    ready.store(true, std::memory_order_release);
    env.wait_for_condition("preempt_ww", /*expect_foreign_wakeup=*/true);
    foreign_waker.join();
    // Wait until W0 finishes ScheduleOne and unsafe_job runs before calling
    // Complete(), avoiding concurrent add_jobs/all_done on q_unsafe.
    while (!w0_done.load(std::memory_order_acquire) ||
           !unsafe_done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    q_unsafe.Complete(&env);
    q_w0.Complete(&env);
    EXPECT_TRUE(env.join(&q_w0));
    EXPECT_TRUE(env.join(&q_unsafe));
  }

  EXPECT_TRUE(env.destroy());
}

TEST_F(KduThreadsTest, WwProcessJobsExceptionReserveBreak) {
  constexpr int kWorkers = 12;
  struct ThrowGateJob final : public kdu_thread_job {
    std::atomic<bool>* t_ready = nullptr;
    std::atomic<bool>* owner_exiting_job = nullptr;
    std::atomic<int>* workers_finished = nullptr;
    std::atomic<bool>* exc_done = nullptr;
    ThrowGateJob() { set_job_func(&ThrowGateJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<ThrowGateJob*>(base);
      self->t_ready->store(true, std::memory_order_release);
      while (!self->owner_exiting_job->load(std::memory_order_acquire) &&
             self->workers_finished->load(std::memory_order_acquire) <
                 kWorkers) {
      }
      caller->handle_exception(KDU_ERROR_EXCEPTION);
      self->exc_done->store(true, std::memory_order_release);
    }
  };
  struct RoleJob final : public kdu_thread_job {
    std::atomic<bool>* owner_exiting_job = nullptr;
    std::atomic<int>* workers_finished = nullptr;
    int owner_spin = 0;
    RoleJob() { set_job_func(&RoleJob::Run); }
    static void Run(kdu_thread_job* base, kdu_thread_entity* caller) {
      auto* self = static_cast<RoleJob*>(base);
      if (!caller->is_group_owner()) {
        volatile int sink = 0;
        for (int s = 0; s < 4000; ++s) {
          sink = sink + s;
        }
        (void)sink;
        self->workers_finished->fetch_add(1, std::memory_order_release);
        return;
      }
      while (self->workers_finished->load(std::memory_order_acquire) <
             kWorkers - 1) {
      }
      self->owner_exiting_job->store(true, std::memory_order_release);
      volatile int sink = 0;
      for (int s = 0; s < self->owner_spin; ++s) {
        sink = sink + s;
      }
      (void)sink;
    }
  };

  for (int i = 0; i < 32; ++i) {
    ScopedThreadEntity scoped;
    kdu_thread_entity& env = scoped.entity;
    env.create();
    ASSERT_TRUE(env.exists());
    for (int t = 0; t < kWorkers; ++t) {
      ASSERT_TRUE(env.add_thread());
    }
    WarmupWorkers(env);
    env.set_yield_frequency(0);

    RecordingThreadQueue q_throw(1, "exc_res_t");
    RecordingThreadQueue q_jobs(kWorkers, "exc_res_j");
    ASSERT_TRUE(env.attach_queue(&q_throw, nullptr, "exc_res_t_dom", 0, 0));
    ASSERT_TRUE(env.attach_queue(&q_jobs, nullptr, "exc_res_j_dom", 0, 0));

    std::atomic<bool> t_ready{false};
    std::atomic<bool> owner_exiting_job{false};
    std::atomic<int> workers_finished{0};
    std::atomic<bool> exc_done{false};

    ThrowGateJob tjob;
    tjob.t_ready = &t_ready;
    tjob.owner_exiting_job = &owner_exiting_job;
    tjob.workers_finished = &workers_finished;
    tjob.exc_done = &exc_done;
    q_throw.ScheduleOne(&tjob, &env, /*all_scheduled=*/true,
                        KDU_THREAD_JOB_AUTO_BIND_ONCE);
    while (!t_ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    RoleJob rjobs[kWorkers];
    kdu_thread_job* rptrs[kWorkers];
    for (int j = 0; j < kWorkers; ++j) {
      rjobs[j].owner_exiting_job = &owner_exiting_job;
      rjobs[j].workers_finished = &workers_finished;
      rjobs[j].owner_spin = i * 64;
      rptrs[j] = &rjobs[j];
    }
    q_jobs.BindRange(rptrs, kWorkers, 0);
    q_jobs.ScheduleBatch(rptrs, kWorkers, &env, /*all_scheduled=*/true);

    try {
      env.wait_for_condition("exc_res", /*expect_foreign_wakeup=*/false);
    } catch (kdu_exception) {
    }
    while (!exc_done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    EXPECT_FALSE(env.destroy());
    error_handler_.TakeMessage();
  }
}

}  // namespace
}  // namespace kdu_core
