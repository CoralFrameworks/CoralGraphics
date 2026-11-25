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

#ifndef CORAL_EFFECT_TESTS_CRASH_REPORT_H
#define CORAL_EFFECT_TESTS_CRASH_REPORT_H

namespace coral::test {

/**
 * Prints a symbolized stack to stderr when the process dies on an unhandled fault.
 *
 * Call once, first thing in `main`. Windows installs an unhandled-exception filter that walks the stack
 * with dbghelp; other platforms do nothing and keep their normal crash behaviour.
 */
void installCrashHandler();

} // namespace coral::test

#endif // CORAL_EFFECT_TESTS_CRASH_REPORT_H
