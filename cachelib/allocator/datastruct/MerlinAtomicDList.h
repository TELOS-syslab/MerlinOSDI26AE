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
struct CACHELIB_PACKED_ATTR MerlinAtomicDListHook {
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
template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
class MerlinAtomicDList {
 public:
  using Mutex = folly::DistributedMutex;
  using LockHolder = std::unique_lock<Mutex>;
  using CompressedPtrType = typename T::CompressedPtrType;
  using PtrCompressor = typename T::PtrCompressor;
  using RefFlags = typename T::Flags;
  using MerlinAtomicDListObject = serialization::MerlinAtomicDListObject;

  MerlinAtomicDList() = default;
  MerlinAtomicDList(const MerlinAtomicDList&) = delete;
  MerlinAtomicDList& operator=(const MerlinAtomicDList&) = delete;

  explicit MerlinAtomicDList(PtrCompressor compressor) noexcept
      : compressor_(std::move(compressor)) {}

  // Restore MerlinAtomicDList from saved state.
  //
  // @param object              Save MerlinAtomicDList object
  // @param compressor          PtrCompressor object
  MerlinAtomicDList(const MerlinAtomicDListObject& object, PtrCompressor compressor)
      : compressor_(std::move(compressor)),
        head_(compressor_.unCompress(CompressedPtrType{*object.compressedHead()})),
        tail_(compressor_.unCompress(CompressedPtrType{*object.compressedTail()})),
        relaxed_size_(*object.size()) {}

  /**
   * Exports the current state as a thrift object for later restoration.
   */
  MerlinAtomicDListObject saveState() const {
    MerlinAtomicDListObject state;
    *state.compressedHead() = compressor_.compress(head_).saveState();
    *state.compressedTail() = compressor_.compress(tail_).saveState();
    *state.size() = relaxed_size_;
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

  // Links the passed node to the head of the double linked list
  // @param node node to be linked at the head
  void linkAtHead(T& node) noexcept;

  void linkAtHeadMultiple(T& start, T& end, size_t n) noexcept;

  void linkAtHeadFromADList(MerlinAtomicDList<T, HookPtr> &o) noexcept;

  // Links the passed node to the head of the double linked list
  // @param node node to be linked at the head
  T* removeTail() noexcept;

  T* removeNTail(int n) noexcept;

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

  size_t size() const noexcept { return relaxed_size_.load(std::memory_order_relaxed); }

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

  std::cout << "MerlinAtomicDList @" << this << "\n";
  dump(" head_", &head_);
  dump(" tail_", &tail_);
  dump(" relaxed_size_", &relaxed_size_);
  dump(" head_mutex", &head_mutex);
}

  // Iterator interface for the double linked list. Supports both iterating
  // from the tail and head.
  class Iterator {
   public:
    enum class Direction { FROM_HEAD, FROM_TAIL };

    Iterator(T* p,
             Direction d,
             const MerlinAtomicDList<T, HookPtr>& MerlinAtomicDList) noexcept
        : curr_(p), dir_(d), MerlinAtomicDList_(&MerlinAtomicDList) {}
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
      return MerlinAtomicDList_ == other.MerlinAtomicDList_ && curr_ == other.curr_ &&
             dir_ == other.dir_;
    }

    bool operator!=(const Iterator& other) const noexcept {
      return !(*this == other);
    }

    explicit operator bool() const noexcept {
      return curr_ != nullptr && MerlinAtomicDList_ != nullptr;
    }

    T* get() const noexcept { return curr_; }

    // Invalidates this iterator
    void reset() noexcept { curr_ = nullptr; }

    // Reset the iterator back to the beginning
    void resetToBegin() noexcept {
      curr_ = dir_ == Direction::FROM_HEAD ? MerlinAtomicDList_->head_
                                           : MerlinAtomicDList_->tail_;
    }

   protected:
    void goForward() noexcept;
    void goBackward() noexcept;

    // the current position of the iterator in the list
    alignas(64) T* curr_{nullptr};
    // the direction we are iterating.
    alignas(64) Direction dir_{Direction::FROM_HEAD};
    alignas(64) const MerlinAtomicDList<T, HookPtr>* MerlinAtomicDList_{nullptr};
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

  mutable std::shared_mutex head_mutex;
  
  alignas(64) const PtrCompressor compressor_{};
  // head of the linked list
  alignas(64) std::atomic<T*> head_{nullptr};

  // tail of the linked list
  alignas(64) std::atomic<T*> tail_{nullptr};

  // size of the list
  alignas(64) std::atomic<size_t> relaxed_size_{0};
  char padding1_[64 - sizeof(std::atomic<size_t>)] = {0};
  alignas(64) std::atomic<int> dump_once{0};
  char padding2_[64 - sizeof(std::atomic<size_t>)] = {0};
};
} // namespace cachelib
} // namespace facebook


namespace facebook {
namespace cachelib {


template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::sanityCheck() {
    size_t curr_size = 0;
    T* curr = head_.load();
    while (curr != nullptr) {
      curr_size++;
      curr = getNext(*curr);
    }
    XDCHECK_EQ(curr_size, relaxed_size_.load());

    if (curr_size != relaxed_size_.load()) {
      XLOGF(ERR, "curr_size: {}, size: {}", curr_size, relaxed_size_.load());
      printf("curr_size: %zu, size: %zu\n", curr_size, relaxed_size_.load());
      abort();
    }
  }


/* Linked list implemenation */
template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::linkAtHead(T& node) noexcept {
    /*
if(dump_once==0){
    dumpLayout();
    dump_once=1;
}
    */
    setPrev(node, nullptr);
  std::shared_lock lock(head_mutex);

  T* oldHead = head_.load();
  setNext(node, oldHead);

  while (!head_.compare_exchange_weak(oldHead, &node)) {
    setNext(node, oldHead);
  }

  if (oldHead == nullptr) {
    // this is the thread that first makes head_ points to the node
    // other threads must follow this, o.w. oldHead will be nullptr
    XDCHECK_EQ(tail_, nullptr);

    T* tail = nullptr;
    tail_.compare_exchange_weak(tail, &node);
  } else {
    setPrev(*oldHead, &node);
  }

  relaxed_size_.fetch_add(1, std::memory_order_relaxed);
}


template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::linkAtHeadMultiple(T& start,
                                                 T& end,
                                                 size_t n) noexcept {
  std::shared_lock lock(head_mutex);
  
  setPrev(start, nullptr);

  T* oldHead = head_.load();
  setNext(end, oldHead);

  while (!head_.compare_exchange_weak(oldHead, &start)) {
    setNext(end, oldHead);
  }

  if (oldHead == nullptr) {
    // this is the thread that first makes head_ points to the node
    // other threads must follow this, o.w. oldHead will be nullptr
    XDCHECK_EQ(tail_, nullptr);

    T* tail = nullptr;
    tail_.compare_exchange_weak(tail, &end);
  } else {
    setPrev(*oldHead, &end);
  }

  relaxed_size_.fetch_add(n, std::memory_order_relaxed);
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::linkAtHeadFromADList(
    MerlinAtomicDList<T, HookPtr>& o) noexcept {
  std::shared_lock lock(head_mutex);
  
  T* oHead = &o.getHead();
  T* oTail = &o.getTail();

  // setPrev(o.getHead(), nullptr);

  T* oldHead = head_.load();
  setNext(*oTail, oldHead);

  while (!head_.compare_exchange_weak(oldHead, oHead)) {
    setNext(oTail, oldHead);
  }

  if (oldHead == nullptr) {
    // this is the thread that first makes head_ points to the node
    // other threads must follow this, o.w. oldHead will be nullptr
    XDCHECK_EQ(tail_, nullptr);

    T* tail = nullptr;
    tail_.compare_exchange_weak(tail, &oTail);
  } else {
    setPrev(*oldHead, &oTail);
  }

  relaxed_size_.fetch_add(o.size(), std::memory_order_relaxed);
}

/* note that the next of the tail may not be nullptr  */
template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
T* MerlinAtomicDList<T, HookPtr>::removeTail() noexcept {
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
  if (head_ == tail) {
    T* oldHead = tail;
    head_.compare_exchange_weak(oldHead, nullptr);
  }

  setNext(*tail, nullptr);
  setPrev(*tail, nullptr);

  relaxed_size_.fetch_sub(1, std::memory_order_relaxed);

  return tail;
}

/* note that the next of the tail may not be nullptr  */
template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
T* MerlinAtomicDList<T, HookPtr>::removeNTail(int n) noexcept {
    LockHolder l(*mtx_);

  T* tail = tail_.load();
  if (tail == nullptr) {
    // empty list
    return nullptr;
  }

  T* next = tail;
  T* curr = getPrev(*next);

  if (curr == nullptr) {
    return nullptr;
  }

  int i = 1;
  for (; i < n && curr != nullptr; i++) {
    next = curr;
    curr = getPrev(*next);
  }
  
  if (curr == nullptr) {
    // find the next 
    tail_ = next;
    next = getNext(*next);
    setNext(*tail_, nullptr);
    setPrev(*next, nullptr);
    relaxed_size_.fetch_sub(i - 1, std::memory_order_relaxed);
  } else {
    tail_ = curr;
    setNext(*curr, nullptr);
    setPrev(*next, nullptr);
    relaxed_size_.fetch_sub(i, std::memory_order_relaxed);
  }

  return tail;
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::unlink(const T& node) noexcept {
  XDCHECK_GT(relaxed_size_, 0u);

  {
    std::unique_lock lock(head_mutex);
    
    auto* const next = getNext(node);
    
    if (&node == head_) {
      head_ = next;
    }
    
    if (next != nullptr) {
      setPrevFrom(*next, node);
    }
  }

  auto* const prev = getPrev(node);
  
  if (&node == tail_) {
    tail_ = prev;
  }

  if (prev != nullptr) {
    setNextFrom(*prev, node);
  }

  relaxed_size_.fetch_sub(1, std::memory_order_relaxed);
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::remove(T& node) noexcept {
  auto* const prev = getPrev(node);
  auto* const next = getNext(node);
  if (prev == nullptr && next == nullptr) {
    return;
  }

  LockHolder l(*mtx_);
  unlink(node);
  setNext(node, nullptr);
  setPrev(node, nullptr);
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  LockHolder l(*mtx_);

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
template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::Iterator::goForward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = MerlinAtomicDList_->getPrev(*curr_);
  } else {
    curr_ = MerlinAtomicDList_->getNext(*curr_);
  }
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
void MerlinAtomicDList<T, HookPtr>::Iterator::goBackward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = MerlinAtomicDList_->getNext(*curr_);
  } else {
    curr_ = MerlinAtomicDList_->getPrev(*curr_);
  }
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
typename MerlinAtomicDList<T, HookPtr>::Iterator&
MerlinAtomicDList<T, HookPtr>::Iterator::operator++() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goForward();
  }
  return *this;
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
typename MerlinAtomicDList<T, HookPtr>::Iterator&
MerlinAtomicDList<T, HookPtr>::Iterator::operator--() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goBackward();
  }
  return *this;
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
typename MerlinAtomicDList<T, HookPtr>::Iterator MerlinAtomicDList<T, HookPtr>::begin()
    const noexcept {
  return MerlinAtomicDList<T, HookPtr>::Iterator(
      head_, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
typename MerlinAtomicDList<T, HookPtr>::Iterator MerlinAtomicDList<T, HookPtr>::rbegin()
    const noexcept {
  return MerlinAtomicDList<T, HookPtr>::Iterator(
      tail_, Iterator::Direction::FROM_TAIL, *this);
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
typename MerlinAtomicDList<T, HookPtr>::Iterator MerlinAtomicDList<T, HookPtr>::end()
    const noexcept {
  return MerlinAtomicDList<T, HookPtr>::Iterator(
      nullptr, Iterator::Direction::FROM_HEAD, *this);
}

template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
typename MerlinAtomicDList<T, HookPtr>::Iterator MerlinAtomicDList<T, HookPtr>::rend()
    const noexcept {
  return MerlinAtomicDList<T, HookPtr>::Iterator(
      nullptr, Iterator::Direction::FROM_TAIL, *this);
}
} // namespace cachelib
} // namespace facebook

