// Copyright 2017 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <unistd.h>

#include "starboard/common/semaphore.h"
#include "starboard/common/time.h"
#include "starboard/nplb/posix_compliance/posix_thread_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace starboard {
namespace nplb {

TEST(Semaphore, PutAndTake) {
  Semaphore semaphore;
  semaphore.Put();
  semaphore.Take();
  EXPECT_FALSE(semaphore.TakeTry());
}

TEST(Semaphore, PutAndTakeTry) {
  Semaphore semaphore;
  semaphore.Put();
  EXPECT_TRUE(semaphore.TakeTry());
}

// Tests the expectation that waiting no time will succeed
// If the semaphore is uncontended.
TEST(Semaphore, TakeWait_Zero) {
  Semaphore semaphore;
  semaphore.Put();
  EXPECT_TRUE(semaphore.TakeWait(0));
}

TEST(Semaphore, InitialValue_One) {
  Semaphore semaphore(1);
  semaphore.Take();
  EXPECT_FALSE(semaphore.TakeTry());
}

class ThreadTakesSemaphore : public posix::AbstractTestThread {
 public:
  explicit ThreadTakesSemaphore(Semaphore* s) : semaphore_(s) {}
  void Run() override { semaphore_->Take(); }

 private:
  Semaphore* semaphore_;
};
TEST(Semaphore, ThreadTakes) {
  Semaphore semaphore;
  ThreadTakesSemaphore thread(&semaphore);
  thread.Start();
  semaphore.Put();
  thread.Join();
}

class ThreadTakesWaitSemaphore : public posix::AbstractTestThread {
 public:
  explicit ThreadTakesWaitSemaphore(int64_t wait_us)
      : thread_started_(false),
        wait_us_(wait_us),
        result_signaled_(false),
        result_wait_time_(0) {}
  void Run() override {
    thread_started_ = true;
    int64_t start_time = CurrentMonotonicTime();
    result_signaled_ = semaphore_.TakeWait(wait_us_);
    result_wait_time_ = CurrentMonotonicTime() - start_time;
  }

  // Use a volatile bool to signal when the thread has started executing
  // instead of a semaphore since some platforms may take a relatively long
  // time after signalling the semaphore to return from the Put.
  volatile bool thread_started_;

  int64_t wait_us_;
  Semaphore semaphore_;
  bool result_signaled_;
  int64_t result_wait_time_;
};

TEST(Semaphore, FLAKY_ThreadTakesWait_PutBeforeTimeExpires) {
  int64_t timeout_time = 250'000;  // 250ms
  int64_t wait_time = 1000;        // 1ms
  ThreadTakesWaitSemaphore thread(timeout_time);

  // Create thread and wait for it to start executing.
  thread.Start();
  while (!thread.thread_started_) {
    usleep(1000);
  }

  usleep(wait_time);
  thread.semaphore_.Put();

  thread.Join();

  EXPECT_TRUE(thread.result_signaled_);
  EXPECT_LT(thread.result_wait_time_, timeout_time);
}

double IsDoubleNear(double first, double second, double diff_threshold) {
  double diff = first - second;
  if (diff < 0) {
    diff = -diff;
  }
  return diff < diff_threshold;
}

TEST(Semaphore, ThreadTakesWait_TimeExpires) {
  const int attempts = 20;  // Retest up to 20 times.

  const int64_t kTimeThreshold = 5'000;  // 5ms

  for (int i = 0; i < attempts; ++i) {
    int64_t wait_time = 20'000;  // 20ms
    ThreadTakesWaitSemaphore thread(wait_time);

    // Create thread and wait for it to start executing.
    thread.Start();
    while (!thread.thread_started_) {
      usleep(1000);
    }

    // It is possible for the thread to be preempted just before processing
    // Semaphore::TakeWait, so sleep for an extra amount of time to avoid the
    // semaphore being legitimately signalled during the wait time (because
    // the thread started TakeWait late).
    usleep(wait_time * 5);
    thread.semaphore_.Put();

    thread.Join();
    EXPECT_FALSE(thread.result_signaled_);

    if (IsDoubleNear(1.0 * wait_time, 1.0 * thread.result_wait_time_,
                     kTimeThreshold * 1.0)) {
      return;  // Test passed.
    }
  }

  EXPECT_TRUE(false) << "Thread waited, but time exceeded expectations.";
}

class ThreadPutsSemaphore : public posix::AbstractTestThread {
 public:
  explicit ThreadPutsSemaphore(Semaphore* s) : semaphore_(s) {}
  void Run() override { semaphore_->Put(); }

 private:
  Semaphore* semaphore_;
};
TEST(Semaphore, ThreadPuts) {
  Semaphore semaphore;
  ThreadPutsSemaphore thread(&semaphore);
  thread.Start();
  thread.Join();
  semaphore.Put();
}

}  // namespace nplb
}  // namespace starboard
