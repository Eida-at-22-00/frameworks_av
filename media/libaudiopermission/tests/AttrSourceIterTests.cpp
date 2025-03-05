/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <android/content/AttributionSourceState.h>
#include <media/AttrSourceIter.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>

using ::android::content::AttributionSourceState;
using ::android::media::permission::AttrSourceIter::begin;
using ::android::media::permission::AttrSourceIter::cbegin;
using ::android::media::permission::AttrSourceIter::cend;
using ::android::media::permission::AttrSourceIter::end;

using ::android::media::permission::AttrSourceIter::ConstIter;

using ::testing::ContainerEq;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Return;

class AttrSourceIterTest : public ::testing::Test {
  public:
    AttrSourceIterTest() {
        mAttr.pid = 1;
        mAttr.uid = 1;
        AttributionSourceState next;
        next.pid = 2;
        next.uid = 2;
        AttributionSourceState nextnext;
        nextnext.pid = 3;
        nextnext.uid = 3;
        next.next = {nextnext};
        mAttr.next = {next};
    }

  protected:
    AttributionSourceState mAttr;
};

TEST_F(AttrSourceIterTest, constIter) {
    const AttributionSourceState& ref = mAttr;
    std::vector<int> mPids;
    std::transform(cbegin(ref), cend(), std::back_inserter(mPids),
                   [](const auto& x) { return x.pid; });
    EXPECT_THAT(mPids, ElementsAreArray({1, 2, 3}));
}

TEST_F(AttrSourceIterTest, nonConstIter) {
    AttributionSourceState expected;
    {
        expected.pid = 2;
        expected.uid = 1;
        AttributionSourceState expectedNext;
        expectedNext.pid = 4;
        expectedNext.uid = 2;
        AttributionSourceState expectedNextNext;
        expectedNextNext.pid = 6;
        expectedNextNext.uid = 3;
        expectedNext.next = {expectedNextNext};
        expected.next = {expectedNext};
    }
    std::for_each(begin(mAttr), end(), [](auto& x) { x.pid = x.pid * 2; });

    EXPECT_THAT(mAttr, Eq(expected));
}

TEST_F(AttrSourceIterTest, nonConstIterReferenceEquals) {
    const AttributionSourceState& ref = mAttr;
    std::vector<const AttributionSourceState*> attrs;
    std::transform(cbegin(ref), cend(), std::back_inserter(attrs),
                   [](const auto& x) { return &x; });
    std::for_each(begin(mAttr), end(), [](auto& x) { x.pid = x.pid * 2; });
    std::vector<const AttributionSourceState*> attrsAfter;
    std::transform(cbegin(ref), cend(), std::back_inserter(attrsAfter),
                   [](const auto& x) { return &x; });
    EXPECT_THAT(attrs, ContainerEq(attrsAfter));
}
