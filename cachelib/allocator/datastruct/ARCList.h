#pragma once

#include <memory>
#include <algorithm>

#include "cachelib/allocator/datastruct/DList.h"
#include "cachelib/allocator/datastruct/AtomicFIFOHashTable.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"

namespace facebook {
namespace cachelib {

template <typename T, DListHook<T> T::*HookPtr>
class ARCList {
 public:
  using DListType = DList<T, HookPtr>;
  using ARCListObject = serialization::ARCListObject;

  ARCList() = default;

  explicit ARCList(typename T::PtrCompressor compressor)
      : compressor_(std::move(compressor)),
        t1_(compressor_),
        t2_(compressor_),
        p_(0) {}

  ARCList(const ARCListObject& obj, typename T::PtrCompressor compressor)
      : compressor_(std::move(compressor)),
        t1_(*obj.t1(), compressor_),
        t2_(*obj.t2(), compressor_),
        p_(*obj.p()) {}

  ARCListObject saveState() const {
    ARCListObject o;
    *o.t1() = t1_.saveState();
    *o.t2() = t2_.saveState();
    *o.p() = static_cast<int64_t>(p_);
    return o;
  }

  DListType& getRecency() noexcept { return t1_; }
  DListType& getFrequency() noexcept { return t2_; }

  size_t size() const noexcept { return t1_.size() + t2_.size(); }
  size_t t1Size() const noexcept { return t1_.size(); }
  size_t t2Size() const noexcept { return t2_.size(); }

  void add(T& node) {
    initGhostTables(size());

    uint32_t h = hashNode(node);

    if (b1_.contains(h)) {
      onGhostHitB1();
      t2_.linkAtHead(node);
      return;
    }

    if (b2_.contains(h)) {
      onGhostHitB2();
      t2_.linkAtHead(node);
      return;
    }

    t1_.linkAtHead(node);
  }

  void remove(T& node) {
    t1_.remove(node);
    t2_.remove(node);
  }

  T* getEvictionCandidate() {
    if (size() == 0) {
      return nullptr;
    }

    size_t t1s = t1Size();

    T* victim = nullptr;
    if (t1s > p_) {
      // evict from T1
      victim = t1_.getTail();
      if (victim) {
        t1_.remove(*victim);
        rememberInB1(hashNode(*victim));
      }
    } else {
      // evict from T2
      victim = t2_.getTail();
      if (victim) {
        t2_.remove(*victim);
        rememberInB2(hashNode(*victim));
      }
    }
    return victim;
  }

 private:
  void initGhostTables(size_t cap) {
    if (!b1_.initialized()) {
      LockHolder l(*mtx_);
      b1_.setFIFOSize(cap / 2);
      b2_.setFIFOSize(cap / 2);

      b1_.initHashtable();
      b2_.initHashtable();
    }
}

  
  static uint32_t hashNode(const T& node) {
    return static_cast<uint32_t>(
        folly::hasher<folly::StringPiece>()(node.getKey()));
  }

  void rememberInB1(uint32_t h) { b1_.insert(h); }
  void rememberInB2(uint32_t h) { b2_.insert(h); }

  void onGhostHitB1() {
    p_ = std::min<size_t>(p_ + 1, size());
  }

  void onGhostHitB2() {
    if (p_ > 0) {
      p_--;
    }
  }

 private:
  typename T::PtrCompressor compressor_;

  DListType t1_;
  DListType t2_;

  AtomicFIFOHashTable b1_;
  AtomicFIFOHashTable b2_;

  size_t p_{0};
  mutable folly::cacheline_aligned<Mutex> mtx_;
};

} // namespace cachelib
} // namespace facebook
