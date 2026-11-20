/*
 * Copyright (C) 2026 The Coral Project
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

#ifndef CORAL_GPU_TESTS_TEST_ASSERTS_H
#define CORAL_GPU_TESTS_TEST_ASSERTS_H

#include <gtest/gtest.h>

#include <backend/RHI.h>
#include <utils/Assert.h>

namespace test {

/**
 * Asserts that a Result succeeded, printing the numeric code on failure.
 *
 * Result is an enum class, so gtest's default printer would show an integer with no context; naming
 * the failing operation makes a broken test readable without a debugger.
 */
#define ASSERT_RHI_SUCCESS(expression)                                                                                 \
  do {                                                                                                                 \
    const backend::Result coralResult = (expression);                                                                  \
    ASSERT_EQ(coralResult, backend::Result::Success) << "from " #expression;                                           \
  } while (false)

#define EXPECT_RHI_SUCCESS(expression)                                                                                 \
  do {                                                                                                                 \
    const backend::Result coralResult = (expression);                                                                  \
    EXPECT_EQ(coralResult, backend::Result::Success) << "from " #expression;                                           \
  } while (false)

#define ASSERT_RHI_RESULT(expression, expected)                                                                        \
  do {                                                                                                                 \
    const backend::Result coralResult = (expression);                                                                  \
    ASSERT_EQ(coralResult, expected) << "from " #expression;                                                           \
  } while (false)

/** @return true when @p flags contains @p feature. */
inline bool hasFeature(const backend::Features flags, backend::Feature feature) {
  return (flags & static_cast<backend::Features>(feature)) != 0;
}

} // namespace test

#endif // CORAL_GPU_TESTS_TEST_ASSERTS_H
