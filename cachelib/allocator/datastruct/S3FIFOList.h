/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/MPMCQueue.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <atomic>
#include <thread>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include "cachelib/allocator/serialize/gen-cpp2/objects_types.h"
#pragma GCC diagnostic pop

#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/allocator/datastruct/AtomicDList.h"
#include "cachelib/allocator/datastruct/AtomicFIFOHashTable.h"
#include "cachelib/allocator/datastruct/DList.h"
#include "cachelib/common/BloomFilter.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook {
namespace cachelib {

template <typename T, AtomicDListHook<T> T::*HookPtr>
class S3FIFOList {
 public:
  using Mutex = folly::DistributedMutex;
  using LockHolder = std::unique_lock<Mutex>;
  using CompressedPtrType = typename T::CompressedPtrType;
  using PtrCompressor = typename T::PtrCompressor;
  using ADList = AtomicDList<T, HookPtr>;
  using RefFlags = typename T::Flags;
  using S3FIFOListObject = serialization::S3FIFOListObject;

  S3FIFOList() = default;
  S3FIFOList(const S3FIFOList&) = delete;
  S3FIFOList& operator=(const S3FIFOList&) = delete;
  ~S3FIFOList() {
  }

  S3FIFOList(PtrCompressor compressor) noexcept {
    pfifo_ = std::make_unique<ADList>(compressor);
    mfifo_ = std::make_unique<ADList>(compressor);
  }

  // Restore S3FIFOList from saved state.
  //
  // @param object              Save S3FIFOList object
  // @param compressor          PtrCompressor object
  S3FIFOList(const S3FIFOListObject& object, PtrCompressor compressor) {
    pfifo_ = std::make_unique<ADList>(*object.pfifo(), compressor);
    mfifo_ = std::make_unique<ADList>(*object.mfifo(), compressor);
  }

  /**
   * Exports the current state as a thrift object for later restoration.
   */
  S3FIFOListObject saveState() const {
    S3FIFOListObject state;
    *state.pfifo() = pfifo_->saveState();
    *state.mfifo() = mfifo_->saveState();
    return state;
  }

  ADList& getListProbationary() const noexcept { return *pfifo_; }

  ADList& getListMain() const noexcept { return *mfifo_; }

  // T* getTail() const noexcept { return pfifo_->getTail(); }

  size_t size() const noexcept { return pfifo_->size() + mfifo_->size(); }

  T* getEvictionCandidate() noexcept;

  void add(T& node) noexcept {
    if (hist_.initialized() && hist_.contains(hashNode(node))) {
      mfifo_->linkAtHead(node);
      markMain(node);
      unmarkProbationary(node);
    } else {
      pfifo_->linkAtHead(node);
      markProbationary(node);
      unmarkMain(node);
    }
  }

    #define s3FreqMask 0x3
    #define s3kFreqMask (s3FreqMask << RefFlags::kMMFlag3)
    #define s3MAX_FREQ 3
        using Value = uint32_t;

  FOLLY_ALWAYS_INLINE int incFreq(T &node) noexcept
        {
            // node.template setFlag<RefFlags::kMMFlag2>();
            // return 1;
            int res = 0;
            auto predicate = [&res](const Value curValue)
            {
                res = curValue & s3kFreqMask;
                if (res == s3kFreqMask)
                {
                    return false;
                }
                return true;
            };

            auto newValue = [](const Value curValue)
            {
                return (curValue + (static_cast<Value>(1) << RefFlags::kMMFlag3));
            };

            node.ref_.template atomicUpdateValue(predicate, newValue);
            int retval = (res >> RefFlags::kMMFlag3) + 1;

            return retval;
        }
        FOLLY_ALWAYS_INLINE int decFreq(T &node) noexcept
        {
            // int ret = node.template isFlagSet<RefFlags::kMMFlag2>();
            // node.template unSetFlag<RefFlags::kMMFlag2>();
            // return ret - 1;
            int res = 0;
            auto predicate = [&res](const Value curValue)
            {
                res = curValue & s3kFreqMask;
                if (res == 0)
                {
                    return false;
                }
                return true;
            };

            auto newValue = [](const Value curValue)
            {
                return (curValue - (static_cast<Value>(1) << RefFlags::kMMFlag3));
            };

            node.ref_.template atomicUpdateValue(predicate, newValue);
            int retval = (res >> RefFlags::kMMFlag3);
            return retval - 1;
        }
        FOLLY_ALWAYS_INLINE int getFreq(const T &node) const noexcept
        {
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag3) & (s3MAX_FREQ);
        }

  // Bit MM_BIT_0 is used to record if the item is hot.
  void markProbationary(T& node) noexcept {
    node.template setFlag<RefFlags::kMMFlag0>();
  }

  void unmarkProbationary(T& node) noexcept {
    node.template unSetFlag<RefFlags::kMMFlag0>();
  }

  bool isProbationary(const T& node) const noexcept {
    return node.template isFlagSet<RefFlags::kMMFlag0>();
  }

  // Bit MM_BIT_2 is used to record if the item is cold.
  void markMain(T& node) noexcept {
    node.template setFlag<RefFlags::kMMFlag2>();
  }

  void unmarkMain(T& node) noexcept {
    node.template unSetFlag<RefFlags::kMMFlag2>();
  }

  bool isMain(const T& node) const noexcept {
    return node.template isFlagSet<RefFlags::kMMFlag2>();
  }


  void markAccessed(T& node) noexcept {
    incFreq(node);
    return;
    node.template setFlag<RefFlags::kMMFlag1>();
  }

  void unmarkAccessed(T& node) noexcept {
    decFreq(node);
    return;
    node.template unSetFlag<RefFlags::kMMFlag1>();
  }

  bool isAccessed(const T& node) const noexcept {
    int freq = getFreq(node);
    if (freq > 0) {
      return true;
    }
    return false;
    return node.template isFlagSet<RefFlags::kMMFlag1>();
  }

 private:
  static uint32_t hashNode(const T& node) noexcept {
    return static_cast<uint32_t>(
        folly::hasher<folly::StringPiece>()(node.getKey()));
  }


  std::unique_ptr<ADList> pfifo_;

  std::unique_ptr<ADList> mfifo_;

  std::unique_ptr<ADList[]> pfifoSublists_;
  std::unique_ptr<ADList[]> mfifoSublists_;

  mutable folly::cacheline_aligned<Mutex> mtx_;

  constexpr static double pRatio_ = 0.05;

  AtomicFIFOHashTable hist_;
};
} // namespace cachelib
} // namespace facebook

#include "cachelib/allocator/datastruct/S3FIFOList-inl.h"
