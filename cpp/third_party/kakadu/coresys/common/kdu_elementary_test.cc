// Unit tests for third_party/kakadu/vendor/coresys/common/kdu_elementary.h.

#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

template <typename InterlockedT, typename ValueT>
void ExerciseInterlockedInt(ValueT initial, ValueT delta, ValueT or_mask,
                            ValueT and_mask, ValueT swapped) {
  InterlockedT atom;
  atom.set(initial);
  EXPECT_EQ(atom.get(), initial);

  InterlockedT assigned;
  assigned.set(0);
  assigned = atom;
  EXPECT_EQ(assigned.get(), initial);

  InterlockedT::store_barrier();
  InterlockedT::load_barrier();
  InterlockedT::full_barrier();

  atom.barrier_set(initial);
  EXPECT_EQ(atom.get_barrier(), initial);

  const ValueT after_get_add = atom.get_add(delta);
  EXPECT_EQ(after_get_add, initial);
  EXPECT_EQ(atom.get(), static_cast<ValueT>(initial + delta));

  const ValueT after_add_get = atom.add_get(delta);
  const ValueT expected_sum = static_cast<ValueT>(initial + delta + delta);
  EXPECT_EQ(after_add_get, expected_sum);
  EXPECT_EQ(atom.get(), expected_sum);

  EXPECT_EQ(atom.exchange(swapped), expected_sum);
  EXPECT_EQ(atom.get(), swapped);

  EXPECT_EQ(atom.exchange_add(delta), swapped);
  const ValueT plus_delta = static_cast<ValueT>(swapped + delta);
  EXPECT_EQ(atom.get(), plus_delta);

  EXPECT_EQ(atom.exchange_or(or_mask), plus_delta);
  const ValueT or_val = static_cast<ValueT>(plus_delta | or_mask);
  EXPECT_EQ(atom.get(), or_val);

  EXPECT_EQ(atom.exchange_and(and_mask), or_val);
  const ValueT and_val = static_cast<ValueT>(or_val & and_mask);
  EXPECT_EQ(atom.get(), and_val);

  const ValueT wrong_ref = static_cast<ValueT>(and_val ^ ValueT{1});
  EXPECT_FALSE(atom.compare_and_set(wrong_ref, initial));
  EXPECT_EQ(atom.get(), and_val);

  // compare_and_set uses weak CAS internally; loop to handle spurious failure.
  bool cas_ok = false;
  for (int attempt = 0; attempt < 32 && !cas_ok; ++attempt) {
    cas_ok = atom.compare_and_set(and_val, initial);
  }
  EXPECT_TRUE(cas_ok);
  EXPECT_EQ(atom.get(), initial);
}

TEST(KduElementaryTest, TimespecAndClock) {
  kdu_timespec ts1{};
  kdu_timespec ts2{};
  EXPECT_TRUE(ts1.get_time());
  kdu_clock clock;
  EXPECT_TRUE(clock.measures_real_time());
  kdu_clock::sleep(0.0);
  kdu_clock::sleep(0.001);
  EXPECT_GE(clock.get_ellapsed_seconds(), 0.0);
  clock.reset();
  EXPECT_GE(clock.get_ellapsed_seconds(), 0.0);
  EXPECT_TRUE(ts2.get_time());
  const double dt = static_cast<double>(ts2.tv_sec - ts1.tv_sec) +
                    1e-9 * static_cast<double>(ts2.tv_nsec - ts1.tv_nsec);
  EXPECT_GE(dt, 0.0);
}

TEST(KduElementaryTest, InterlockedIntegers) {
  ExerciseInterlockedInt<kdu_interlocked_int32, kdu_int32>(10, 5, 0x0F00,
                                                           0x0FFF, 100);
  ExerciseInterlockedInt<kdu_interlocked_uint32, kdu_uint32>(10U, 5U, 0x0F00U,
                                                             0x0FFFU, 100U);
  ExerciseInterlockedInt<kdu_interlocked_int64, kdu_int64>(
      10000000000LL, 5000000000LL, 0x0F00LL, 0x7FFFFFFFFFFFFFFFLL,
      20000000000LL);
  ExerciseInterlockedInt<kdu_interlocked_uint64, kdu_uint64>(
      10000000000ULL, 5000000000ULL, 0x0F00ULL, 0xFFFFFFFFFFFFFFFFULL,
      20000000000ULL);
}

TEST(KduElementaryTest, InterlockedPtrAndLongPtr) {
  kdu_byte buffer[16]{};
  void* const p0 = &buffer[0];
  void* const p4 = &buffer[4];
  void* const p8 = &buffer[8];

  kdu_interlocked_ptr ptr;
  ptr.set(p0);
  EXPECT_EQ(ptr.get(), p0);

  kdu_interlocked_ptr copied;
  copied.set(nullptr);
  copied = ptr;
  EXPECT_EQ(copied.get(), p0);

  ptr.barrier_set(p4);
  EXPECT_EQ(ptr.get_barrier(), p4);
  EXPECT_EQ(ptr.exchange(p0), p4);
  EXPECT_EQ(ptr.get(), p0);
  EXPECT_EQ(ptr.exchange_add(4), p0);
  EXPECT_EQ(ptr.get(), p4);

  EXPECT_FALSE(ptr.compare_and_set(p0, p8));
  EXPECT_EQ(ptr.get(), p4);
  bool cas_ok = false;
  for (int i = 0; i < 32 && !cas_ok; ++i) {
    cas_ok = ptr.compare_and_set(p4, p8);
  }
  EXPECT_TRUE(cas_ok);
  EXPECT_EQ(ptr.get(), p8);

  EXPECT_FALSE(ptr.validate(p0));
  bool val_ok = false;
  for (int i = 0; i < 32 && !val_ok; ++i) {
    val_ok = ptr.validate(p8);
  }
  EXPECT_TRUE(val_ok);

  kdu_interlocked_long_ptr lptr;
  lptr.set(12345);
  EXPECT_EQ(lptr.get(), 12345);

  kdu_interlocked_long_ptr lptr_copy;
  lptr_copy.set(0);
  lptr_copy = lptr;
  EXPECT_EQ(lptr_copy.get(), 12345);

  lptr.barrier_set(67890);
  EXPECT_EQ(lptr.get_barrier(), 67890);

  lptr.set_ptr(p4);
  EXPECT_EQ(lptr.get_ptr(), p4);
  lptr.barrier_set_ptr(p8);
  EXPECT_EQ(lptr.get_ptr_barrier(), p8);
}

struct SyncTestContext {
  kdu_mutex mutex;
  kdu_semaphore ready_sem;
  kdu_semaphore work_sem;
  kdu_event auto_event;
  kdu_event manual_event;
  bool auto_flag = false;
  bool worker_saw_self_ref = false;
};

kdu_thread_startproc_result KDU_THREAD_STARTPROC_CALL_CONVENTION
SemaphoreAndEventWorker(void* arg) {
  auto* ctx = static_cast<SyncTestContext*>(arg);
  ctx->worker_saw_self_ref = (kdu_thread::get_thread_ref() != nullptr);

  // Synchronize via semaphore.
  ctx->ready_sem.signal();
  ctx->work_sem.wait();

  // Wait for unprotected_set on auto_event while holding mutex.
  ctx->mutex.lock();
  ctx->ready_sem.signal();
  while (!ctx->auto_flag) {
    ctx->auto_event.wait(ctx->mutex);
  }
  ctx->mutex.unlock();

  // Wait on manual_event woken via protected_set.
  ctx->mutex.lock();
  ctx->ready_sem.signal();
  ctx->manual_event.wait(ctx->mutex);
  ctx->mutex.unlock();
  return KDU_THREAD_STARTPROC_ZERO_RESULT;
}

TEST(KduElementaryTest, ThreadSelfComparePauseYieldAndObjectName) {
  kdu_thread empty;
  EXPECT_FALSE(empty.exists());
  EXPECT_TRUE(!empty);
  EXPECT_FALSE(empty.destroy());
  EXPECT_FALSE(empty.check_self());

  kdu_thread self_a;
  kdu_thread self_b;
  EXPECT_TRUE(self_a.set_to_self());
  EXPECT_TRUE(self_b.set_to_self());
  EXPECT_TRUE(self_a.exists());
  EXPECT_FALSE(!self_a);
  EXPECT_TRUE(self_a.check_self());
  EXPECT_TRUE(self_a.equals(self_b));
  EXPECT_TRUE(self_a == self_b);
  EXPECT_FALSE(self_a != self_b);
  EXPECT_FALSE(self_a.equals(empty));

  EXPECT_TRUE(kdu_thread::micro_pause());
  EXPECT_TRUE(kdu_thread::yield());
}

TEST(KduElementaryTest, MutexSemaphoreAndEventLifecycleAndCrossThread) {
  kdu_mutex uninit_mutex;
  EXPECT_FALSE(uninit_mutex.exists());
  EXPECT_TRUE(!uninit_mutex);
  EXPECT_FALSE(uninit_mutex.lock());
  EXPECT_FALSE(uninit_mutex.try_lock());
  EXPECT_FALSE(uninit_mutex.unlock());
  EXPECT_FALSE(uninit_mutex.destroy());

  kdu_semaphore uninit_sem;
  EXPECT_FALSE(uninit_sem.exists());
  EXPECT_TRUE(!uninit_sem);
  EXPECT_FALSE(uninit_sem.wait());
  EXPECT_FALSE(uninit_sem.signal());
  EXPECT_FALSE(uninit_sem.destroy());

  kdu_event uninit_ev;
  EXPECT_FALSE(uninit_ev.exists());
  EXPECT_TRUE(!uninit_ev);
  EXPECT_FALSE(uninit_ev.protected_set());
  EXPECT_FALSE(uninit_ev.unprotected_set());
  EXPECT_FALSE(uninit_ev.reset());
  EXPECT_FALSE(uninit_ev.wait(uninit_mutex));
  EXPECT_FALSE(uninit_ev.timed_wait(uninit_mutex, 100));
  EXPECT_FALSE(uninit_ev.destroy());

  SyncTestContext ctx;
  ASSERT_TRUE(ctx.mutex.create());
  EXPECT_TRUE(ctx.mutex.exists());
  EXPECT_TRUE(ctx.mutex.try_lock());
  EXPECT_FALSE(ctx.mutex.try_lock());
  EXPECT_TRUE(ctx.mutex.unlock());

  ASSERT_TRUE(ctx.ready_sem.create(0));
  ASSERT_TRUE(ctx.work_sem.create(0));
  ASSERT_TRUE(ctx.auto_event.create(/*manual_reset=*/false));
  ASSERT_TRUE(ctx.manual_event.create(/*manual_reset=*/true));

  // Manual reset event remains signalled across waits until reset().
  ctx.mutex.lock();
  EXPECT_TRUE(ctx.manual_event.protected_set());
  EXPECT_TRUE(ctx.manual_event.wait(ctx.mutex));
  EXPECT_TRUE(ctx.manual_event.timed_wait(ctx.mutex, 1000));
  EXPECT_TRUE(ctx.manual_event.reset());
  EXPECT_FALSE(ctx.manual_event.timed_wait(ctx.mutex, 1000));
  ctx.mutex.unlock();

  // Auto-reset event resets after a single wait().
  ctx.mutex.lock();
  EXPECT_TRUE(ctx.auto_event.protected_set());
  EXPECT_TRUE(ctx.auto_event.wait(ctx.mutex));
  EXPECT_FALSE(ctx.auto_event.timed_wait(ctx.mutex, 1000));
  ctx.mutex.unlock();

  kdu_thread worker;
  ASSERT_TRUE(worker.create(SemaphoreAndEventWorker, &ctx));
  EXPECT_TRUE(worker.exists());
  EXPECT_FALSE(worker.set_to_self());
  EXPECT_FALSE(worker.check_self());

  EXPECT_TRUE(ctx.ready_sem.wait());
  EXPECT_TRUE(ctx.work_sem.signal());

  // Wait until worker signals before entering auto_event.wait(), then lock and
  // unlock mutex so worker is guaranteed inside wait before unprotected_set().
  EXPECT_TRUE(ctx.ready_sem.wait());
  ctx.mutex.lock();
  ctx.auto_flag = true;
  ctx.mutex.unlock();
  EXPECT_TRUE(ctx.auto_event.unprotected_set());

  EXPECT_TRUE(ctx.ready_sem.wait());
  ctx.mutex.lock();
  EXPECT_TRUE(ctx.manual_event.protected_set());
  ctx.mutex.unlock();

  EXPECT_TRUE(worker.destroy());
  EXPECT_TRUE(ctx.worker_saw_self_ref);

  EXPECT_TRUE(ctx.manual_event.destroy());
  EXPECT_TRUE(ctx.auto_event.destroy());
  // Upstream kdu_semaphore::destroy() initializes `result = false` and never
  // sets it to `true` on success in the POSIX semaphores branch
  // (`vendor/coresys/common/kdu_elementary.h:2978-2994`).
  ctx.work_sem.destroy();
  ctx.ready_sem.destroy();
  EXPECT_FALSE(ctx.work_sem.exists());
  EXPECT_FALSE(ctx.ready_sem.exists());
  EXPECT_TRUE(ctx.mutex.destroy());
}

}  // namespace
}  // namespace kdu_core
