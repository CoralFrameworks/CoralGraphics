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

#include <gtest/gtest.h>

#include "CrashReport.h"

/**
 * Entry point for the effect framework's test binary.
 *
 * Most cases need a real device, because the framework is a thin layer over the RHI and the renderer.
 * Each case skips itself when no device can be created, which is what lets the suite pass on a CI machine
 * with no graphics stack instead of failing there and being switched off.
 *
 * The crash handler goes first: a backend that faults inside a driver call otherwise leaves nothing but an
 * exit code, and the stack it prints is what makes such a fault diagnosable without a debugger attached.
 */
int main(int argc, char** argv) {
  coral::test::installCrashHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
