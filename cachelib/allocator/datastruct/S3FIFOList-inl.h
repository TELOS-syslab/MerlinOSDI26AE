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
      hist_.setFIFOSize(listSize);
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
      if (getFreq(*curr)>1) {
        resetAccessed(*curr);
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
      if (isAccessed(*curr)>0) {
        unmarkAccessed(*curr);
        mfifo_->linkAtHead(*curr);
      } else {
        return curr;
      }
    }
  }
}

} // namespace cachelib
} // namespace facebook
