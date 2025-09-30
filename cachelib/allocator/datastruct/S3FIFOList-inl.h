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

namespace facebook {
namespace cachelib {

// used for single thread
template <typename T, AtomicDListHook<T> T::*HookPtr>
T* S3FIFOList<T, HookPtr>::getEvictionCandidate() noexcept {

  size_t listSize = pfifo_->size() + mfifo_->size();
  if (listSize == 0) {
    return nullptr;
  }

  T* curr = nullptr;
  if (!hist_.initialized()) {
    LockHolder l(*mtx_);
    if (!hist_.initialized()) {
      hist_.setFIFOSize(listSize / 2);
      hist_.initHashtable();
    }
  }

  while (true) {
    if (pfifo_->size() > (double)(pfifo_->size() + mfifo_->size()) * pRatio_) {
      // evict from probationary FIFO
      curr = pfifo_->removeTail();
      if (curr == nullptr) {
        if (pfifo_->size() != 0) {
          printf("pfifo_->size() = %zu\n", pfifo_->size());
          abort();
        }
        continue;
      }
      if (pfifo_->isAccessed(*curr)) {
        pfifo_->unmarkAccessed(*curr);
        XDCHECK(isProbationary(*curr));
        unmarkProbationary(*curr);
        markMain(*curr);
        mfifo_->linkAtHead(*curr);
      } else {
        hist_.insert(hashNode(*curr));

        return curr;
      }
    } else {
      curr = mfifo_->removeTail();
      if (curr == nullptr) {
        continue;
      }
      if (mfifo_->isAccessed(*curr)) {
        mfifo_->unmarkAccessed(*curr);
        mfifo_->linkAtHead(*curr);
      } else {
        return curr;
      }
    }
  }
}

template <typename T, AtomicDListHook<T> T::*HookPtr>
T* S3FIFOList<T, HookPtr>::getEvictionCandidate0() noexcept {
  size_t listSize = pfifo_->size() + mfifo_->size();
  if (listSize == 0 && evictCandidateQueue_.size() == 0) {
    return nullptr;
  }

  T* curr = nullptr;
  if (!hist_.initialized()) {
    LockHolder l(*mtx_);
    if (!hist_.initialized()) {
      hist_.setFIFOSize(listSize / 2);
      hist_.initHashtable();
// #define ENABLE_SCALABILITY
#ifdef ENABLE_SCALABILITY
      if (evThread_.get() == nullptr) {
        evThread_ = std::make_unique<std::thread>(&S3FIFOList::threadFunc, this);
      }
#endif
    }
  }

  size_t sz = evictCandidateQueue_.sizeGuess();
  if (sz < nMaxEvictionCandidates_ / (2 + sz % 8)) {
    prepareEvictionCandidates();
  }

  int nTries = 0;
  while (!evictCandidateQueue_.read(curr)) {
    if ((nTries++) % 100 == 0) {
      prepareEvictionCandidates();
    }
  }

  return curr;
}

template <typename T, AtomicDListHook<T> T::*HookPtr>
void S3FIFOList<T, HookPtr>::prepareEvictionCandidates() noexcept {
  if (pfifo_->size() > (double)(pfifo_->size() + mfifo_->size()) * pRatio_) {
    for (int i = 0; i < nCandidateToPrepare(); i++) {
      // evict from probationary FIFO
      evictPFifo();
    }
  } else {
    for (int i = 0; i < nCandidateToPrepare(); i++) {
      evictMFifo();
    }
  }
}

template <typename T, AtomicDListHook<T> T::*HookPtr>
void S3FIFOList<T, HookPtr>::evictPFifo() noexcept {
  T* curr = nullptr;

  // evict from probationary FIFO
  curr = pfifo_->removeTail();
  if (curr != nullptr) {
    if (pfifo_->isAccessed(*curr)) {
      pfifo_->unmarkAccessed(*curr);
      XDCHECK(isProbationary(*curr));
      unmarkProbationary(*curr);
      markMain(*curr);
      mfifo_->linkAtHead(*curr);
    } else {
      hist_.insert(hashNode(*curr));
      if (!evictCandidateQueue_.write(curr)) {
        pfifo_->linkAtHead(*curr);
      }
    }
  }
}


template <typename T, AtomicDListHook<T> T::*HookPtr>
void S3FIFOList<T, HookPtr>::evictMFifo() noexcept {
  T* curr = nullptr;
  curr = mfifo_->removeTail();
  if (curr != nullptr) {
    if (mfifo_->isAccessed(*curr)) {
      mfifo_->unmarkAccessed(*curr);
      mfifo_->linkAtHead(*curr);
    } else {
      if (!evictCandidateQueue_.write(curr)) {
        pfifo_->linkAtHead(*curr);
      }
    }
  }
}

} // namespace cachelib
} // namespace facebook
