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

/**
 * @file CrashReport.cpp
 *
 * Prints a symbolized stack when the test binary dies on a fault.
 *
 * A graphics backend can fail in ways a debugger is not always at hand for - a driver call that faults
 * on one machine and not another, a null entry point in a table the host cannot check statically. gtest
 * reports only that the process ended, and bisecting with print statements costs a rebuild per guess. A
 * handler that walks the stack with dbghelp turns that into one run: this exists so the next fault names
 * the function and line that caused it.
 *
 * Windows only; on every other platform `installCrashHandler` does nothing, because those builds are
 * debugged with gdb/lldb and their own signal handlers.
 */

#include "CrashReport.h"

#if defined(_WIN32)

#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kMaxFrames = 48;

void writeSymbol(const HANDLE process, const DWORD64 address, const int index) {
  alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO) + 256] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;

  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, symbol) == FALSE) {
    std::fprintf(stderr, "  #%-2d 0x%016llx\n", index, static_cast<unsigned long long>(address));
    return;
  }

  IMAGEHLP_LINE64 line = {};
  line.SizeOfStruct = sizeof(line);
  DWORD lineDisplacement = 0;
  const bool hasLine = SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != FALSE;
  if (hasLine) {
    std::fprintf(stderr, "  #%-2d %s+0x%llx  %s:%lu\n", index, symbol->Name,
                 static_cast<unsigned long long>(displacement), line.FileName, line.LineNumber);
  } else {
    std::fprintf(stderr, "  #%-2d %s+0x%llx\n", index, symbol->Name,
                 static_cast<unsigned long long>(displacement));
  }
}

LONG WINAPI reportFault(EXCEPTION_POINTERS* exception) {
  const HANDLE process = GetCurrentProcess();
  const HANDLE thread = GetCurrentThread();

  std::fprintf(stderr, "\n[crash] exception 0x%08lx at 0x%p\n",
               exception != nullptr && exception->ExceptionRecord != nullptr
                       ? exception->ExceptionRecord->ExceptionCode
                       : 0ul,
               exception != nullptr && exception->ExceptionRecord != nullptr
                       ? exception->ExceptionRecord->ExceptionAddress
                       : nullptr);

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
  if (SymInitialize(process, nullptr, TRUE) != FALSE) {
    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int index = 0; index < kMaxFrames; ++index) {
      if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                      SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE) {
        break;
      }
      if (frame.AddrPC.Offset == 0) break;
      writeSymbol(process, frame.AddrPC.Offset, index);
    }
    (void) SymCleanup(process);
  }
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

namespace coral::test {

void installCrashHandler() {
  SetUnhandledExceptionFilter(&reportFault);
}

} // namespace coral::test

#else

namespace coral::test {

void installCrashHandler() {}

} // namespace coral::test

#endif
