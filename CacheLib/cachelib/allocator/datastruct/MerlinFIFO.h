#pragma once

#include <folly/logging/xlog.h>

#include <algorithm>
#include <atomic>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include "cachelib/allocator/serialize/gen-cpp2/objects_types.h"
#pragma GCC diagnostic pop

#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook {
namespace cachelib {

template <typename T>
struct CACHELIB_PACKED_ATTR MerlinFIFOHook {
  using Time = uint32_t;
  using CompressedPtrType = typename T::CompressedPtrType;
  using PtrCompressor = typename T::PtrCompressor;

  void setNext(T* const n, const PtrCompressor& compressor) noexcept {
    next_ = compressor.compress(n);
  }

  void setNext(CompressedPtrType next) noexcept { next_ = next; }

  void setPrev(T* const p, const PtrCompressor& compressor) noexcept {
    prev_ = compressor.compress(p);
  }

  void setPrev(CompressedPtrType prev) noexcept { prev_ = prev; }

  CompressedPtrType getNext() const noexcept { return CompressedPtrType(next_); }

  T* getNext(const PtrCompressor& compressor) const noexcept {
    return compressor.unCompress(next_);
  }

  CompressedPtrType getPrev() const noexcept { return CompressedPtrType(prev_); }

  T* getPrev(const PtrCompressor& compressor) const noexcept {
    return compressor.unCompress(prev_);
  }

  // set and get the time when the node was updated in the lru.
  void setUpdateTime(Time time) noexcept { updateTime_ = time; }

  Time getUpdateTime() const noexcept {
    // Suppress TSAN here because we don't care if an item is promoted twice by
    // two get operations running concurrently. It should be very rarely and is
    // just a minor inefficiency if it happens.
    folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
    return updateTime_;
  }

 private:
  CompressedPtrType next_{}; // next node in the linked list
  CompressedPtrType prev_{}; // previous node in the linked list
  // timestamp when this was last updated to the head of the list
  Time updateTime_{0};
};

// uses a double linked list to implement an LRU. T must be have a public
// member of type Hook and HookPtr must point to that.
template <typename T, MerlinFIFOHook<T> T::*HookPtr>
class MerlinFIFO {
 public:
  using Mutex = folly::DistributedMutex;
  using LockHolder = std::unique_lock<Mutex>;
  using CompressedPtrType = typename T::CompressedPtrType;
  using PtrCompressor = typename T::PtrCompressor;
  using RefFlags = typename T::Flags;
  using MerlinFIFOObject = serialization::MerlinFIFOObject;

  MerlinFIFO() = default;
  MerlinFIFO(const MerlinFIFO&) = delete;
  MerlinFIFO& operator=(const MerlinFIFO&) = delete;

  explicit MerlinFIFO(PtrCompressor compressor) noexcept
      : compressor_(std::move(compressor)) {}

  // Restore MerlinFIFO from saved state.
  //
  // @param object              Save MerlinFIFO object
  // @param compressor          PtrCompressor object
  MerlinFIFO(const MerlinFIFOObject& object, PtrCompressor compressor)
      : compressor_(std::move(compressor)),
        head_(compressor_.unCompress(CompressedPtrType{*object.compressedHead()})),
        tail_(compressor_.unCompress(CompressedPtrType{*object.compressedTail()})),
        size_(*object.size()) {}

  /**
   * Exports the current state as a thrift object for later restoration.
   */
  MerlinFIFOObject saveState() const {
    MerlinFIFOObject state;
    *state.compressedHead() = compressor_.compress(head_).saveState();
    *state.compressedTail() = compressor_.compress(tail_).saveState();
    *state.size() = size_;
    return state;
  }

  T* getNext(const T& node) const noexcept {
    return (node.*HookPtr).getNext(compressor_);
  }

  T* getPrev(const T& node) const noexcept {
    return (node.*HookPtr).getPrev(compressor_);
  }

  void setNext(T& node, T* next) noexcept {
    (node.*HookPtr).setNext(next, compressor_);
  }

  void setNextFrom(T& node, const T& other) noexcept {
    (node.*HookPtr).setNext((other.*HookPtr).getNext());
  }

  void setPrev(T& node, T* prev) noexcept {
    (node.*HookPtr).setPrev(prev, compressor_);
  }

  void setPrevFrom(T& node, const T& other) noexcept {
    (node.*HookPtr).setPrev((other.*HookPtr).getPrev());
  }

  bool try_lock_head() noexcept{
    // return true if succeed，return false if failed
    return !flag_.test_and_set(std::memory_order_acquire);
  }
  void unlock_head() noexcept{
    flag_.clear(std::memory_order_release);
  }
  void lock_head() noexcept{
    // test_and_set return old value
    while (flag_.test_and_set(std::memory_order_acquire)) {
    #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();  // reduce HT/SMT contention
    #endif
    }
  }

  // Links the passed node to the head of the double linked list
  // @param node node to be linked at the head
  void linkAtHead(T& node) noexcept;

  // Links the passed node to the head of the double linked list
  // @param node node to be linked at the head
  T* removeTail() noexcept;

  // removes the node completely from the linked list and cleans up the node
  // appropriately by setting its next and prev as nullptr.
  void remove(T& node) noexcept;

  // Unlinks the destination node and replaces it with the source node
  //
  // @param oldNode   destination node
  // @param newNode   source node
  void replace(T& oldNode, T& newNode) noexcept;

  T* getHead() const noexcept { return head_.load(); }
  T* getTail() const noexcept { return tail_.load(); }

  size_t size() const noexcept { return head_insert_size_ - tail_remove_size_; }

  void sanityCheck(); 

  void dumpLayout() const {
  constexpr uintptr_t CL = 64;

  auto base = reinterpret_cast<uintptr_t>(this);

  auto dump = [&](const char* name, const void* p) {
    auto addr = reinterpret_cast<uintptr_t>(p);
    std::cout << name
              << " addr=" << p
              << " CL=" << ((addr & ~(CL - 1)))
              << " offset=" << (addr & (CL - 1))
              << "\n";
  };

  std::cout << "MerlinFIFO @" << this << "\n";
  dump(" head_", &head_);
  dump(" tail_", &tail_);
  dump(" size_", &size_);
}

  // Iterator interface for the double linked list. Supports both iterating
  // from the tail and head.
  class Iterator {
   public:
    enum class Direction { FROM_HEAD, FROM_TAIL };

    Iterator(T* p,
             Direction d,
             const MerlinFIFO<T, HookPtr>& MerlinFIFO) noexcept
        : curr_(p), dir_(d), MerlinFIFO_(&MerlinFIFO) {}
    virtual ~Iterator() = default;

    // copyable and movable
    Iterator(const Iterator&) = default;
    Iterator& operator=(const Iterator&) = default;
    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) noexcept = default;

    // moves the iterator forward and backward. Calling ++ once the iterator
    // has reached the end is undefined.
    Iterator& operator++() noexcept;
    Iterator& operator--() noexcept;

    T* operator->() const noexcept { return curr_; }
    T& operator*() const noexcept { return *curr_; }

    bool operator==(const Iterator& other) const noexcept {
      return MerlinFIFO_ == other.MerlinFIFO_ && curr_ == other.curr_ &&
             dir_ == other.dir_;
    }

    bool operator!=(const Iterator& other) const noexcept {
      return !(*this == other);
    }

    explicit operator bool() const noexcept {
      return curr_ != nullptr && MerlinFIFO_ != nullptr;
    }

    T* get() const noexcept { return curr_; }

    // Invalidates this iterator
    void reset() noexcept { curr_ = nullptr; }

    // Reset the iterator back to the beginning
    void resetToBegin() noexcept {
      curr_ = dir_ == Direction::FROM_HEAD ? MerlinFIFO_->head_
                                           : MerlinFIFO_->tail_;
    }

   protected:
    void goForward() noexcept;
    void goBackward() noexcept;

    // the current position of the iterator in the list
    alignas(64) T* curr_{nullptr};
    // the direction we are iterating.
    alignas(64) Direction dir_{Direction::FROM_HEAD};
    alignas(64) const MerlinFIFO<T, HookPtr>* MerlinFIFO_{nullptr};
  };

  // provides an iterator starting from the head of the linked list.
  Iterator begin() const noexcept;

  // provides an iterator starting from the tail of the linked list.
  Iterator rbegin() const noexcept;

  // Iterator to compare against for the end.
  Iterator end() const noexcept;
  Iterator rend() const noexcept;

 private:
  // unlinks the node from the linked list. Does not correct the next and
  // previous.
  void unlink(const T& node) noexcept;

  mutable folly::cacheline_aligned<Mutex> mtx_;

  alignas(64) std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
  
  alignas(64) const PtrCompressor compressor_{};
  // head of the linked list
  alignas(64) std::atomic<T*> head_{nullptr};

  // tail of the linked list
  alignas(64) std::atomic<T*> tail_{nullptr};

  // size of the list
  alignas(64) std::atomic<size_t> head_insert_size_{0};
  alignas(64) std::atomic<size_t> tail_remove_size_{0};
  alignas(64) std::atomic<size_t> size_{0};
  char padding1_[64 - sizeof(std::atomic<size_t>)] = {0};
  alignas(64) std::atomic<int> dump_once{0};
  char padding2_[64 - sizeof(std::atomic<size_t>)] = {0};
};
} // namespace cachelib
} // namespace facebook


namespace facebook {
namespace cachelib {


template <typename T, MerlinFIFOHook<T> T::*HookPtr>
void MerlinFIFO<T, HookPtr>::sanityCheck() {
    size_t curr_size = 0;
    T* curr = head_.load();
    while (curr != nullptr) {
      curr_size++;
      curr = getNext(*curr);
    }
    XDCHECK_EQ(curr_size, size_.load());

    if (curr_size != size_.load()) {
      XLOGF(ERR, "curr_size: {}, size: {}", curr_size, size_.load());
      printf("curr_size: %zu, size: %zu\n", curr_size, size_.load());
      abort();
    }
  }


/* Linked list implemenation */
template <typename T, MerlinFIFOHook<T> T::*HookPtr>
void MerlinFIFO<T, HookPtr>::linkAtHead(T& node) noexcept {
// get lock for head modification
    setPrev(node, nullptr);
  T* oldHead = head_.load();
  setNext(node, oldHead);
  head_ = &node;
  if (oldHead == nullptr) {
    // this is the thread that first makes head_ points to the node
    // other threads must follow this, o.w. oldHead will be nullptr
    XDCHECK_EQ(tail_, nullptr);

    T* tail = nullptr;
    while (!tail_.compare_exchange_weak(tail, &node)) {
      if(tail != nullptr) {
        break;
      }
    }
    //tail_.compare_exchange_weak(tail, &node);
  } else {
    setPrev(*oldHead, &node);
  }
  head_insert_size_.fetch_add(1, std::memory_order_relaxed);
}

/* note that the next of the tail may not be nullptr  */
template <typename T, MerlinFIFOHook<T> T::*HookPtr>
T* MerlinFIFO<T, HookPtr>::removeTail() noexcept {
  T* tail = tail_.load();
  if (tail == nullptr) {
    // empty list
    return nullptr;
  }
  T* prev = getPrev(*tail);

  // if tail has not changed, the prev is correct
  while (!tail_.compare_exchange_weak(tail, prev)) {
    if(tail == nullptr) {
      // empty list
      return nullptr;
    }
    prev = getPrev(*tail);
  }

  // if the tail was also the head
  prev = getPrev(*tail);
  if (prev == nullptr) {
    lock_head();
    if(head_ == tail){
        head_ = nullptr;
    }
    unlock_head();
  }

  setNext(*tail, nullptr);
  setPrev(*tail, nullptr);

  tail_remove_size_.fetch_add(1, std::memory_order_relaxed);

  return tail;
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
void MerlinFIFO<T, HookPtr>::unlink(const T& node) noexcept {
  XDCHECK_GT(size_, 0u);

  {//not optimized
    lock_head();
    
    auto* const next = getNext(node);
    
    if (&node == head_) {
      head_ = next;
    }
    
    if (next != nullptr) {
      setPrevFrom(*next, node);
    }
    unlock_head();
  }

  auto* const prev = getPrev(node);
  
  if (&node == tail_) {
    tail_ = prev;
  }

  if (prev != nullptr) {
    setNextFrom(*prev, node);
  }

  //remove the node from the linked list
  //which is the same as removing the tail
  tail_remove_size_.fetch_add(1, std::memory_order_relaxed);
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
void MerlinFIFO<T, HookPtr>::remove(T& node) noexcept {
  auto* const prev = getPrev(node);
  auto* const next = getNext(node);
  if (prev == nullptr && next == nullptr) {
    return;
  }

  LockHolder l(*mtx_);
  // manage the head and tail links
  unlink(node);
  setNext(node, nullptr);
  setPrev(node, nullptr);
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
void MerlinFIFO<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  LockHolder l(*mtx_);
  printf("replace node %p prev %p next %p\n", &oldNode, getPrev(oldNode), getNext(oldNode));
  // Update head and tail links if needed
  if (&oldNode == head_) {
    head_ = &newNode;
  }
  if (&oldNode == tail_) {
    tail_ = &newNode;
  }

  // Make the previous and next nodes point to the new node
  auto* const prev = getPrev(oldNode);
  auto* const next = getNext(oldNode);
  if (prev != nullptr) {
    setNext(*prev, &newNode);
  }
  if (next != nullptr) {
    setPrev(*next, &newNode);
  }

  // Make the new node point to the previous and next nodes
  setPrev(newNode, prev);
  setNext(newNode, next);

  // Cleanup the old node
  setPrev(oldNode, nullptr);
  setNext(oldNode, nullptr);
}

/* Iterator Implementation */
template <typename T, MerlinFIFOHook<T> T::*HookPtr>
void MerlinFIFO<T, HookPtr>::Iterator::goForward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = MerlinFIFO_->getPrev(*curr_);
  } else {
    curr_ = MerlinFIFO_->getNext(*curr_);
  }
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
void MerlinFIFO<T, HookPtr>::Iterator::goBackward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = MerlinFIFO_->getNext(*curr_);
  } else {
    curr_ = MerlinFIFO_->getPrev(*curr_);
  }
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
typename MerlinFIFO<T, HookPtr>::Iterator&
MerlinFIFO<T, HookPtr>::Iterator::operator++() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goForward();
  }
  return *this;
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
typename MerlinFIFO<T, HookPtr>::Iterator&
MerlinFIFO<T, HookPtr>::Iterator::operator--() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goBackward();
  }
  return *this;
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
typename MerlinFIFO<T, HookPtr>::Iterator MerlinFIFO<T, HookPtr>::begin()
    const noexcept {
  return MerlinFIFO<T, HookPtr>::Iterator(
      head_, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
typename MerlinFIFO<T, HookPtr>::Iterator MerlinFIFO<T, HookPtr>::rbegin()
    const noexcept {
  return MerlinFIFO<T, HookPtr>::Iterator(
      tail_, Iterator::Direction::FROM_TAIL, *this);
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
typename MerlinFIFO<T, HookPtr>::Iterator MerlinFIFO<T, HookPtr>::end()
    const noexcept {
  return MerlinFIFO<T, HookPtr>::Iterator(
      nullptr, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, MerlinFIFOHook<T> T::*HookPtr>
typename MerlinFIFO<T, HookPtr>::Iterator MerlinFIFO<T, HookPtr>::rend()
    const noexcept {
  return MerlinFIFO<T, HookPtr>::Iterator(
      nullptr, Iterator::Direction::FROM_TAIL, *this);
}
} // namespace cachelib
} // namespace facebook

