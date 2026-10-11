#include <dispatch/QueueBuffers.hpp>
#include <dispatch/DispatchBacklog.hpp>
#include <dispatch/DispatchBudget.hpp>
#include <barrier>
#include <TestSupport.hpp>
#include <memory>
#include <thread>
#include <mutex>
using namespace UE4SSXB;
// Real producer, consumer and observer threads, synchronized without sleeps.
// Exercise production buffer transfer, backlog, budgets and count publication.
void pending_count_lifecycle() {
 ConsumerBacklogCount pending;
 std::mutex lock;
 std::vector<std::shared_ptr<int>> queue, spare;
 DispatchBacklog<std::shared_ptr<int>> backlog;
 std::weak_ptr<int> old;
 std::barrier phase(3);
 constexpr int cycles = 100;
 std::thread producer([&] {
   for (int i=0; i<cycles; ++i) {
     { std::scoped_lock held(lock); auto owner=std::make_shared<int>(i);
       old=owner; queue={owner,owner,owner}; }
     phase.arrive_and_wait(); // Producer-only snapshot.
     phase.arrive_and_wait();
     phase.arrive_and_wait(); // Batch transfer complete.
     { std::scoped_lock held(lock); queue.push_back(std::make_shared<int>(-1)); }
     phase.arrive_and_wait(); // Producer refill complete.
     phase.arrive_and_wait(); // Budgeted partial dispatch complete.
     phase.arrive_and_wait();
     phase.arrive_and_wait(); // Consumer discarded old batch.
     phase.arrive_and_wait();
     { std::scoped_lock held(lock); queue.clear(); } // Shutdown clears producer.
     phase.arrive_and_wait();
     phase.arrive_and_wait();
   }
 });
 std::thread consumer([&] {
   for (int i=0; i<cycles; ++i) {
     phase.arrive_and_wait(); phase.arrive_and_wait();
     { std::scoped_lock held(lock); backlog.load(drain_queue(queue,spare));
       pending.remaining(backlog.size()); }
     phase.arrive_and_wait(); phase.arrive_and_wait();
     const auto now=DispatchBudget::Clock::now();
     DispatchBudget budget(now,1,std::chrono::microseconds(0));
     CHECK(dispatch_budgeted(backlog,budget,[&](auto event) {
       pending.remaining(backlog.size());
       std::scoped_lock held(lock);
       CHECK(*event==i && pending.total(queue.size())==3);
       return true;
     },[&] { return now; })==1);
     phase.arrive_and_wait(); phase.arrive_and_wait();
     // Discard canceled entries: no callback count, but queued must reach zero.
     CHECK(dispatch_budgeted(backlog,budget,[&](auto event) {
       pending.remaining(backlog.size()); CHECK(*event==i); return false;
     },[&] { return now; })==0);
     CHECK(old.expired()); // Moved-out backlog entries retain no ownership.
     auto buffer=backlog.release_buffer(); buffer.clear();
     { std::scoped_lock held(lock); retain_empty_buffer(spare,buffer); }
     phase.arrive_and_wait(); phase.arrive_and_wait();
     phase.arrive_and_wait(); phase.arrive_and_wait();
   }
 });
 for (int i=0; i<cycles; ++i) {
   phase.arrive_and_wait();
   { std::scoped_lock held(lock); CHECK(pending.total(queue.size())==3); }
   phase.arrive_and_wait(); phase.arrive_and_wait(); phase.arrive_and_wait();
   phase.arrive_and_wait();
   { std::scoped_lock held(lock); CHECK(pending.total(queue.size())==3 && !old.expired()); }
   phase.arrive_and_wait(); phase.arrive_and_wait();
   { std::scoped_lock held(lock); CHECK(pending.total(queue.size())==1 && old.expired()); }
   phase.arrive_and_wait(); phase.arrive_and_wait();
   { std::scoped_lock held(lock); CHECK(pending.total(queue.size())==0); }
   phase.arrive_and_wait();
 }
 producer.join(); consumer.join();
}
int main() {
 pending_count_lifecycle();
 QueueReadiness ready;
 CHECK(!ready.pending());
 std::mutex mutex;
 std::vector<int> concurrent, concurrentSpare;
 constexpr int count = 10000;
 std::thread producer([&] {
     for (int i=0; i<count; ++i) {
         std::scoped_lock lock(mutex);
         concurrent.push_back(i);
         ready.publish();
     }
 });
 int received = 0;
 while (received < count) {
     if (!ready.pending()) { std::this_thread::yield(); continue; }
     std::vector<int> current;
     {
         std::scoped_lock lock(mutex);
         current=drain_queue(concurrent,concurrentSpare);
         ready.drained();
     }
     for (int value : current) { CHECK(value == received); ++received; }
     current.clear();
     { std::scoped_lock lock(mutex); retain_empty_buffer(concurrentSpare,current); }
 }
 producer.join();
 CHECK(!ready.pending() && concurrent.empty());
 // Enqueue immediately after a drain cannot be cleared by recycling.
 { std::scoped_lock lock(mutex); concurrent.push_back(count); ready.publish(); }
 CHECK(ready.pending());
 { std::scoped_lock lock(mutex); auto last=drain_queue(concurrent,concurrentSpare); ready.drained(); CHECK(last[0]==count); }

 std::vector<int> queue{1,2,3}, spare;
 auto batch=drain_queue(queue,spare);
 CHECK((batch==std::vector<int>{1,2,3}) && queue.empty());
 queue.push_back(4); // Producer adds data while consumer owns old batch.
 retain_empty_buffer(spare,batch);
 CHECK(spare.empty() && (batch==std::vector<int>{1,2,3})); // Cannot recycle live data.
 auto oldCapacity=batch.capacity(); batch.clear(); retain_empty_buffer(spare,batch);
 CHECK(queue.size()==1 && queue[0]==4 && spare.capacity()==oldCapacity);
 auto next=drain_queue(queue,spare);
 CHECK(next.size()==1 && next[0]==4 && queue.empty() && queue.capacity()==oldCapacity);
 std::vector<int> oversized; oversized.reserve(8192);
 retain_empty_buffer(spare,oversized); CHECK(spare.capacity()==0);
 std::vector<std::shared_ptr<int>> owned, reusable;
 auto owner=std::make_shared<int>(7); std::weak_ptr<int> weak=owner;
 owned.push_back(owner); owner.reset();
 auto ownedBatch=drain_queue(owned,reusable);
 CHECK(!weak.expired()); ownedBatch.clear(); CHECK(weak.expired());
 retain_empty_buffer(reusable,ownedBatch); CHECK(weak.expired());
    return LexTests::finish("QueueBuffersTests");
}
