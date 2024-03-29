/*
 * The code is part of the SIndex project.
 *
 *    Copyright (C) 2020 Institute of Parallel and Distributed Systems (IPADS),
 * Shanghai Jiao Tong University. All rights reserved.
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
 *
 */

#include "sindex.h"
#include "sindex_buffer_impl.h"
#include "sindex_group_impl.h"
#include "sindex_model.h"
#include "sindex_root_impl.h"

#include "../lock.h"
#include "mkl.h"
#include "sia.hpp"

#if !defined(SINDEX_IMPL_H)
#define SINDEX_IMPL_H

namespace sindex {

template <class key_t, class val_t, bool seq>
SIndex<key_t, val_t, seq>::SIndex(const std::vector<key_t> &keys,
                                  const std::vector<val_t> &vals,
                                  size_t worker_num, size_t bg_n)
    : bg_num(bg_n) {
  config.worker_n = worker_num;
  // sanity checks
  INVARIANT(config.root_error_bound > 0);
  INVARIANT(config.root_memory_constraint > 0);
  INVARIANT(config.group_error_bound > 0);
  INVARIANT(config.group_error_tolerance > 0);
  INVARIANT(config.buffer_size_bound > 0);
  INVARIANT(config.buffer_size_tolerance > 0);
  INVARIANT(config.buffer_compact_threshold > 0);
  INVARIANT(config.worker_n > 0);

  for (size_t key_i = 1; key_i < keys.size(); key_i++) {
    assert(keys[key_i] >= keys[key_i - 1]);
  }
  rcu_init();

  // malloc memory for root & init root
  root = new root_t();
  root->init(keys, vals);
  init_accelerator();
  start_bg();
}

template <class key_t, class val_t, bool seq>
SIndex<key_t, val_t, seq>::~SIndex() {
  terminate_bg();
}

template <class key_t, class val_t, bool seq>
inline bool SIndex<key_t, val_t, seq>::get(const key_t &key, val_t &val,
                                           const uint32_t worker_id) {
  rcu_progress(worker_id);
  return root->get(key, val) == result_t::ok;
}

template <class key_t, class val_t, bool seq>
inline bool SIndex<key_t, val_t, seq>::put(const key_t &key, const val_t &val,
                                           const uint32_t worker_id) {
  result_t res;
  rcu_progress(worker_id);
  while ((res = root->put(key, val, worker_id)) == result_t::retry) {
    rcu_progress(worker_id);
  }
  return res == result_t::ok;
}

template <class key_t, class val_t, bool seq>
inline bool SIndex<key_t, val_t, seq>::insert(const key_t &key, const val_t &val,
                                           const uint32_t worker_id) {
  result_t res;
  rcu_progress(worker_id);
  while ((res = root->put(key, val, worker_id)) == result_t::retry) {
    rcu_progress(worker_id);
  }
  return res == result_t::ok;
}

template <class key_t, class val_t, bool seq>
inline bool SIndex<key_t, val_t, seq>::remove(const key_t &key,
                                              const uint32_t worker_id) {
  rcu_progress(worker_id);
  return root->remove(key) == result_t::ok;
}

template <class key_t, class val_t, bool seq>
inline size_t SIndex<key_t, val_t, seq>::scan(
    const key_t &begin, const size_t n,
    std::vector<std::pair<key_t, val_t>> &result, const uint32_t worker_id) {
  rcu_progress(worker_id);
  return root->scan(begin, n, result);
}

template <class key_t, class val_t, bool seq>
size_t SIndex<key_t, val_t, seq>::range_scan(
    const key_t &begin, const key_t &end,
    std::vector<std::pair<key_t, val_t>> &result, const uint32_t worker_id) {
  rcu_progress(worker_id);
  return root->range_scan(begin, end, result);
}

template <class key_t, class val_t, bool seq>
void *SIndex<key_t, val_t, seq>::background(void *this_) {
  volatile SIndex &index = *(SIndex *)this_;
  if (index.bg_num == 0) return nullptr;

  size_t bg_num = index.bg_num;
  std::vector<pthread_t> threads(bg_num);
  std::vector<bg_info_t> info(bg_num);

  for (size_t bg_i = 0; bg_i < bg_num; bg_i++) {
    info[bg_i].bg_i = bg_i;
    info[bg_i].bg_n = bg_num;
    info[bg_i].root_ptr = &(index.root);
    info[bg_i].started = false;
    info[bg_i].finished = false;
    info[bg_i].running = true;
    info[bg_i].should_update_array = false;

    int ret = pthread_create(&threads[bg_i], nullptr, root_t::do_adjustment,
                             &info[bg_i]);
    if (ret) {
      COUT_N_EXIT("Error: unable to create bg task thread, " << ret);
    }
  }

  while (index.bg_running) {
    DEBUG_THIS("------ [bg] new round of structure update");

    for (size_t bg_i = 0; bg_i < bg_num; bg_i++) {
      info[bg_i].started = true;
    }

    // wait for workers to finish
    while (true) {
      sleep(1);

      bool finished = true;
      for (size_t bg_i = 0; bg_i < bg_num; bg_i++) {
        if (!info[bg_i].finished) {
          DEBUG_THIS("--- [bg] thread(" << bg_i << ") not finished");
          finished = false;
          break;
        }
      }

      if (finished) {
        break;
      }
    }

    // now worker has finished
    bool should_update_array = false;
    for (size_t bg_i = 0; bg_i < bg_num; bg_i++) {
      should_update_array =
          should_update_array || info[bg_i].should_update_array;
      info[bg_i].finished = false;
      info[bg_i].should_update_array = false;
    }

    if (should_update_array) {
      root_t *old_root = index.root;
      index.root = old_root->create_new_root();
      memory_fence();
      rcu_barrier();
      index.root->trim_root();
      delete old_root;

      float avg_group_error = 0, max_group_error = 0;
      for (size_t group_i = 0; group_i < index.root->group_n; group_i++) {
        float err = index.root->get_group_ptr(group_i)->get_mean_error();
        avg_group_error += err;
        if (err > max_group_error) {
          max_group_error = err;
        }
      }
      avg_group_error /= index.root->group_n;
      DEBUG_THIS("--- [root] group_n: " << index.root->group_n);
      DEBUG_THIS("--- [root] avg_group_error: " << avg_group_error);
      DEBUG_THIS("--- [root] max_group_error: " << max_group_error);
    }

    memory_fence();  // ensure the background theads and the workers all see a
    rcu_barrier();   // correct final stage of root.groups
  }

  for (size_t bg_i = 0; bg_i < bg_num; bg_i++) {
    info[bg_i].running = false;
  }

  for (size_t bg_i = 0; bg_i < bg_num; bg_i++) {
    DEBUG_THIS("--- [bg] joining bg thread(" << bg_i << ")");
    void *status;
    int rc = pthread_join(threads[bg_i], &status);
    if (rc) {
      COUT_N_EXIT("Error: unable to join," << rc);
    }
  }

  return nullptr;
}

template <class key_t, class val_t, bool seq>
void SIndex<key_t, val_t, seq>::start_bg() {
  bg_running = true;
  int ret = pthread_create(&bg_master, nullptr, background, this);
  if (ret) {
    COUT_N_EXIT("Error: unable to create background thread," << ret);
  }
}

template <class key_t, class val_t, bool seq>
void SIndex<key_t, val_t, seq>::terminate_bg() {
  config.exited = true;
  bg_running = false;
}

}  // namespace sindex

#endif  // SINDEX_IMPL_H
