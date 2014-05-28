/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "space_test.h"
#include "large_object_space.h"

namespace art {
namespace gc {
namespace space {

class LargeObjectSpaceTest : public SpaceTest {
 public:
  void LargeObjectTest();
};


void LargeObjectSpaceTest::LargeObjectTest() {
  size_t rand_seed = 0;
  for (size_t i = 0; i < 2; ++i) {
    LargeObjectSpace* los = nullptr;
    if (i == 0) {
      los = space::LargeObjectMapSpace::Create("large object space");
    } else {
      los = space::FreeListSpace::Create("large object space", nullptr, 128 * MB);
    }

    static const size_t num_allocations = 64;
    static const size_t max_allocation_size = 0x100000;
    std::vector<std::pair<mirror::Object*, size_t>> requests;

    for (size_t phase = 0; phase < 2; ++phase) {
      while (requests.size() < num_allocations) {
        size_t request_size = test_rand(&rand_seed) % max_allocation_size;
        size_t allocation_size = 0;
        mirror::Object* obj = los->Alloc(Thread::Current(), request_size, &allocation_size,
                                         nullptr);
        ASSERT_TRUE(obj != nullptr);
        ASSERT_EQ(allocation_size, los->AllocationSize(obj, nullptr));
        ASSERT_GE(allocation_size, request_size);
        // Fill in our magic value.
        byte magic = (request_size & 0xFF) | 1;
        memset(obj, magic, request_size);
        requests.push_back(std::make_pair(obj, request_size));
      }

      // "Randomly" shuffle the requests.
      for (size_t k = 0; k < 10; ++k) {
        for (size_t j = 0; j < requests.size(); ++j) {
          std::swap(requests[j], requests[test_rand(&rand_seed) % requests.size()]);
        }
      }

      // Free 1 / 2 the allocations the first phase, and all the second phase.
      size_t limit = !phase ? requests.size() / 2 : 0;
      while (requests.size() > limit) {
        mirror::Object* obj = requests.back().first;
        size_t request_size = requests.back().second;
        requests.pop_back();
        byte magic = (request_size & 0xFF) | 1;
        for (size_t k = 0; k < request_size; ++k) {
          ASSERT_EQ(reinterpret_cast<const byte*>(obj)[k], magic);
        }
        ASSERT_GE(los->Free(Thread::Current(), obj), request_size);
      }
    }

    size_t bytes_allocated = 0;
    // Checks that the coalescing works.
    mirror::Object* obj = los->Alloc(Thread::Current(), 100 * MB, &bytes_allocated, nullptr);
    EXPECT_TRUE(obj != nullptr);
    los->Free(Thread::Current(), obj);

    EXPECT_EQ(0U, los->GetBytesAllocated());
    EXPECT_EQ(0U, los->GetObjectsAllocated());
    delete los;
  }
}


TEST_F(LargeObjectSpaceTest, LargeObjectTest) {
  LargeObjectTest();
}

}  // namespace space
}  // namespace gc
}  // namespace art