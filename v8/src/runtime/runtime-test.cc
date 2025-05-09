// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <fstream>
#include <memory>

#include "include/v8-function.h"
#include "include/v8-profiler.h"
#include "src/api/api-inl.h"
#include "src/base/numbers/double.h"
#include "src/codegen/compiler.h"
#include "src/codegen/pending-optimization-table.h"
#include "src/compiler-dispatcher/lazy-compile-dispatcher.h"
#include "src/compiler-dispatcher/optimizing-compile-dispatcher.h"
#include "src/debug/debug-evaluate.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/arguments-inl.h"
#include "src/execution/frames-inl.h"
#include "src/execution/frames.h"
#include "src/execution/isolate-inl.h"
#include "src/execution/protectors-inl.h"
#include "src/execution/tiering-manager.h"
#include "src/flags/flags.h"
#include "src/heap/heap-write-barrier-inl.h"
#include "src/heap/pretenuring-handler-inl.h"
#include "src/ic/stub-cache.h"
#include "src/objects/bytecode-array.h"
#include "src/objects/js-collection-inl.h"
#ifdef V8_ENABLE_MAGLEV
#include "src/maglev/maglev-concurrent-dispatcher.h"
#endif  // V8_ENABLE_MAGLEV
#include "src/objects/js-atomics-synchronization-inl.h"
#include "src/objects/js-function-inl.h"
#include "src/objects/js-regexp-inl.h"
#include "src/objects/smi.h"
#include "src/profiler/heap-snapshot-generator.h"
#include "src/regexp/regexp.h"
#include "src/snapshot/snapshot.h"

#ifdef V8_ENABLE_MAGLEV
#include "src/maglev/maglev.h"
#endif  // V8_ENABLE_MAGLEV

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-engine.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {

namespace {
V8_WARN_UNUSED_RESULT Object CrashUnlessFuzzing(Isolate* isolate) {
  CHECK(v8_flags.fuzzing);
  return ReadOnlyRoots(isolate).undefined_value();
}

V8_WARN_UNUSED_RESULT bool CrashUnlessFuzzingReturnFalse(Isolate* isolate) {
  CHECK(v8_flags.fuzzing);
  return false;
}

// Returns |value| unless correctness-fuzzer-supressions is enabled,
// otherwise returns undefined_value.
V8_WARN_UNUSED_RESULT Object ReturnFuzzSafe(Object value, Isolate* isolate) {
  return v8_flags.correctness_fuzzer_suppressions
             ? ReadOnlyRoots(isolate).undefined_value()
             : value;
}

// Assert that the given argument is a number within the Int32 range
// and convert it to int32_t.  If the argument is not an Int32 we crash if not
// in fuzzing mode.
#define CONVERT_INT32_ARG_FUZZ_SAFE(name, index)                   \
  if (!args[index].IsNumber()) return CrashUnlessFuzzing(isolate); \
  int32_t name = 0;                                                \
  if (!args[index].ToInt32(&name)) return CrashUnlessFuzzing(isolate);

// Cast the given object to a boolean and store it in a variable with
// the given name.  If the object is not a boolean we crash if not in
// fuzzing mode.
#define CONVERT_BOOLEAN_ARG_FUZZ_SAFE(name, index)                  \
  if (!args[index].IsBoolean()) return CrashUnlessFuzzing(isolate); \
  bool name = args[index].IsTrue(isolate);

bool IsAsmWasmFunction(Isolate* isolate, JSFunction function) {
  DisallowGarbageCollection no_gc;
#if V8_ENABLE_WEBASSEMBLY
  // For simplicity we include invalid asm.js functions whose code hasn't yet
  // been updated to CompileLazy but is still the InstantiateAsmJs builtin.
  return function.shared().HasAsmWasmData() ||
         function.code().builtin_id() == Builtin::kInstantiateAsmJs;
#else
  return false;
#endif  // V8_ENABLE_WEBASSEMBLY
}

}  // namespace

RUNTIME_FUNCTION(Runtime_ClearMegamorphicStubCache) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  isolate->load_stub_cache()->Clear();
  isolate->store_stub_cache()->Clear();
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_ConstructDouble) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  uint32_t hi = NumberToUint32(args[0]);
  uint32_t lo = NumberToUint32(args[1]);
  uint64_t result = (static_cast<uint64_t>(hi) << 32) | lo;
  return *isolate->factory()->NewNumber(base::uint64_to_double(result));
}

RUNTIME_FUNCTION(Runtime_ConstructConsString) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> left = args.at<String>(0);
  Handle<String> right = args.at<String>(1);

  CHECK(left->IsOneByteRepresentation());
  CHECK(right->IsOneByteRepresentation());

  const bool kIsOneByte = true;
  const int length = left->length() + right->length();
  return *isolate->factory()->NewConsString(left, right, length, kIsOneByte);
}

RUNTIME_FUNCTION(Runtime_ConstructSlicedString) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<String> string = args.at<String>(0);
  int index = args.smi_value_at(1);

  CHECK(string->IsOneByteRepresentation());
  CHECK_LT(index, string->length());

  Handle<String> sliced_string =
      isolate->factory()->NewSubString(string, index, string->length());
  CHECK(sliced_string->IsSlicedString());
  return *sliced_string;
}

RUNTIME_FUNCTION(Runtime_ConstructInternalizedString) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> string = args.at<String>(0);
  CHECK(string->IsOneByteRepresentation());
  Handle<String> internalized = isolate->factory()->InternalizeString(string);
  CHECK(string->IsInternalizedString());
  return *internalized;
}

RUNTIME_FUNCTION(Runtime_ConstructThinString) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> string = args.at<String>(0);
  CHECK(string->IsOneByteRepresentation());
  if (!string->IsConsString()) {
    const bool kIsOneByte = true;
    string =
        isolate->factory()->NewConsString(isolate->factory()->empty_string(),
                                          string, string->length(), kIsOneByte);
  }
  CHECK(string->IsConsString());
  Handle<String> internalized = isolate->factory()->InternalizeString(string);
  CHECK_NE(*internalized, *string);
  CHECK(string->IsThinString());
  return *string;
}

RUNTIME_FUNCTION(Runtime_DeoptimizeFunction) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());

  Handle<Object> function_object = args.at(0);
  if (!function_object->IsJSFunction()) return CrashUnlessFuzzing(isolate);
  Handle<JSFunction> function = Handle<JSFunction>::cast(function_object);

  if (function->HasAttachedOptimizedCode()) {
    Deoptimizer::DeoptimizeFunction(*function);
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DeoptimizeNow) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());

  Handle<JSFunction> function;

  // Find the JavaScript function on the top of the stack.
  JavaScriptStackFrameIterator it(isolate);
  if (!it.done()) function = handle(it.frame()->function(), isolate);
  if (function.is_null()) return CrashUnlessFuzzing(isolate);

  if (function->HasAttachedOptimizedCode()) {
    Deoptimizer::DeoptimizeFunction(*function);
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_RunningInSimulator) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
#if defined(USE_SIMULATOR)
  return ReadOnlyRoots(isolate).true_value();
#else
  return ReadOnlyRoots(isolate).false_value();
#endif
}

RUNTIME_FUNCTION(Runtime_RuntimeEvaluateREPL) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> source = args.at<String>(0);
  Handle<Object> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result,
      DebugEvaluate::Global(isolate, source,
                            debug::EvaluateGlobalMode::kDefault,
                            REPLMode::kYes));

  return *result;
}

RUNTIME_FUNCTION(Runtime_ICsAreEnabled) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(v8_flags.use_ic);
}

RUNTIME_FUNCTION(Runtime_IsConcurrentRecompilationSupported) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      isolate->concurrent_recompilation_enabled());
}

RUNTIME_FUNCTION(Runtime_IsAtomicsWaitAllowed) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(isolate->allow_atomics_wait());
}

namespace {

bool CanOptimizeFunction(CodeKind target_kind, Handle<JSFunction> function,
                         Isolate* isolate, IsCompiledScope* is_compiled_scope) {
  // The following conditions were lifted (in part) from the DCHECK inside
  // JSFunction::MarkForOptimization().

  if (!function->shared().allows_lazy_compilation()) {
    return CrashUnlessFuzzingReturnFalse(isolate);
  }

  // If function isn't compiled, compile it now.
  if (!is_compiled_scope->is_compiled() &&
      !Compiler::Compile(isolate, function, Compiler::CLEAR_EXCEPTION,
                         is_compiled_scope)) {
    return CrashUnlessFuzzingReturnFalse(isolate);
  }

  if (target_kind == CodeKind::TURBOFAN && !v8_flags.turbofan) return false;
  if (target_kind == CodeKind::MAGLEV && !v8_flags.maglev) return false;

  if (function->shared().optimization_disabled() &&
      function->shared().disabled_optimization_reason() ==
          BailoutReason::kNeverOptimize) {
    return CrashUnlessFuzzingReturnFalse(isolate);
  }

  if (IsAsmWasmFunction(isolate, *function)) {
    return CrashUnlessFuzzingReturnFalse(isolate);
  }

  if (v8_flags.testing_d8_test_runner) {
    ManualOptimizationTable::CheckMarkedForManualOptimization(isolate,
                                                              *function);
  }

  if (function->HasAvailableCodeKind(target_kind) ||
      function->HasAvailableHigherTierCodeThan(target_kind) ||
      IsInProgress(function->tiering_state())) {
    DCHECK(function->HasAttachedOptimizedCode() ||
           function->ChecksTieringState());
    return false;
  }

  return true;
}

Object OptimizeFunctionOnNextCall(RuntimeArguments& args, Isolate* isolate,
                                  CodeKind target_kind) {
  if (args.length() != 1 && args.length() != 2) {
    return CrashUnlessFuzzing(isolate);
  }

  Handle<Object> function_object = args.at(0);
  if (!function_object->IsJSFunction()) return CrashUnlessFuzzing(isolate);
  Handle<JSFunction> function = Handle<JSFunction>::cast(function_object);

  IsCompiledScope is_compiled_scope(
      function->shared().is_compiled_scope(isolate));
  if (!CanOptimizeFunction(target_kind, function, isolate,
                           &is_compiled_scope)) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  ConcurrencyMode concurrency_mode = ConcurrencyMode::kSynchronous;
  if (args.length() == 2) {
    Handle<Object> type = args.at(1);
    if (!type->IsString()) return CrashUnlessFuzzing(isolate);
    if (Handle<String>::cast(type)->IsOneByteEqualTo(
            base::StaticCharVector("concurrent")) &&
        isolate->concurrent_recompilation_enabled()) {
      concurrency_mode = ConcurrencyMode::kConcurrent;
    }
  }

  // This function may not have been lazily compiled yet, even though its shared
  // function has.
  if (!function->is_compiled()) {
    DCHECK(function->shared().HasBytecodeArray());
    Code code = *BUILTIN_CODE(isolate, InterpreterEntryTrampoline);
    if (function->shared().HasBaselineCode()) {
      code = function->shared().baseline_code(kAcquireLoad);
    }
    function->set_code(code);
  }

  TraceManualRecompile(*function, target_kind, concurrency_mode);
  JSFunction::EnsureFeedbackVector(isolate, function, &is_compiled_scope);
  function->MarkForOptimization(isolate, target_kind, concurrency_mode);

  return ReadOnlyRoots(isolate).undefined_value();
}

bool EnsureCompiledAndFeedbackVector(Isolate* isolate,
                                     Handle<JSFunction> function,
                                     IsCompiledScope* is_compiled_scope) {
  // Check function allows lazy compilation.
  if (!function->shared().allows_lazy_compilation()) return false;

  // If function isn't compiled, compile it now.
  *is_compiled_scope =
      function->shared().is_compiled_scope(function->GetIsolate());
  if (!is_compiled_scope->is_compiled() &&
      !Compiler::Compile(isolate, function, Compiler::CLEAR_EXCEPTION,
                         is_compiled_scope)) {
    return false;
  }

  // Ensure function has a feedback vector to hold type feedback for
  // optimization.
  JSFunction::EnsureFeedbackVector(isolate, function, is_compiled_scope);
  return true;
}

}  // namespace

RUNTIME_FUNCTION(Runtime_CompileBaseline) {
  HandleScope scope(isolate);
  if (args.length() != 1) {
    return CrashUnlessFuzzing(isolate);
  }
  Handle<Object> function_object = args.at(0);
  if (!function_object->IsJSFunction()) return CrashUnlessFuzzing(isolate);
  Handle<JSFunction> function = Handle<JSFunction>::cast(function_object);

  IsCompiledScope is_compiled_scope =
      function->shared(isolate).is_compiled_scope(isolate);

  if (!function->shared(isolate).IsUserJavaScript()) {
    return CrashUnlessFuzzing(isolate);
  }

  // First compile the bytecode, if we have to.
  if (!is_compiled_scope.is_compiled() &&
      !Compiler::Compile(isolate, function, Compiler::CLEAR_EXCEPTION,
                         &is_compiled_scope)) {
    return CrashUnlessFuzzing(isolate);
  }

  if (!Compiler::CompileBaseline(isolate, function, Compiler::CLEAR_EXCEPTION,
                                 &is_compiled_scope)) {
    return CrashUnlessFuzzing(isolate);
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

// TODO(v8:7700): Remove this function once we no longer need it to measure
// maglev compile times. For normal tierup, OptimizeMaglevOnNextCall should be
// used instead.
#ifdef V8_ENABLE_MAGLEV
RUNTIME_FUNCTION(Runtime_BenchMaglev) {
  HandleScope scope(isolate);
  DCHECK_EQ(args.length(), 2);
  Handle<JSFunction> function = args.at<JSFunction>(0);
  int count = args.smi_value_at(1);

  Handle<Code> code;
  base::ElapsedTimer timer;
  timer.Start();
  code = Maglev::Compile(isolate, function).ToHandleChecked();
  for (int i = 1; i < count; ++i) {
    HandleScope handle_scope(isolate);
    Maglev::Compile(isolate, function);
  }
  PrintF("Maglev compile time: %g ms!\n",
         timer.Elapsed().InMillisecondsF() / count);

  function->set_code(*code);

  return ReadOnlyRoots(isolate).undefined_value();
}
#else
RUNTIME_FUNCTION(Runtime_BenchMaglev) {
  PrintF("Maglev is not enabled.\n");
  return ReadOnlyRoots(isolate).undefined_value();
}
#endif  // V8_ENABLE_MAGLEV

RUNTIME_FUNCTION(Runtime_ActiveTierIsIgnition) {
  HandleScope scope(isolate);
  DCHECK_EQ(args.length(), 1);
  Handle<JSFunction> function = args.at<JSFunction>(0);
  return isolate->heap()->ToBoolean(function->ActiveTierIsIgnition());
}

RUNTIME_FUNCTION(Runtime_ActiveTierIsSparkplug) {
  HandleScope scope(isolate);
  DCHECK_EQ(args.length(), 1);
  Handle<JSFunction> function = args.at<JSFunction>(0);
  return isolate->heap()->ToBoolean(function->ActiveTierIsBaseline());
}

RUNTIME_FUNCTION(Runtime_ActiveTierIsMaglev) {
  HandleScope scope(isolate);
  DCHECK_EQ(args.length(), 1);
  Handle<JSFunction> function = args.at<JSFunction>(0);
  return isolate->heap()->ToBoolean(function->ActiveTierIsMaglev());
}

RUNTIME_FUNCTION(Runtime_ActiveTierIsTurbofan) {
  HandleScope scope(isolate);
  DCHECK_EQ(args.length(), 1);
  Handle<JSFunction> function = args.at<JSFunction>(0);
  return isolate->heap()->ToBoolean(function->ActiveTierIsTurbofan());
}

RUNTIME_FUNCTION(Runtime_IsSparkplugEnabled) {
  DCHECK_EQ(args.length(), 0);
  return isolate->heap()->ToBoolean(v8_flags.sparkplug);
}

RUNTIME_FUNCTION(Runtime_IsMaglevEnabled) {
  DCHECK_EQ(args.length(), 0);
  return isolate->heap()->ToBoolean(v8_flags.maglev);
}

RUNTIME_FUNCTION(Runtime_IsTurbofanEnabled) {
  DCHECK_EQ(args.length(), 0);
  return isolate->heap()->ToBoolean(v8_flags.turbofan);
}

RUNTIME_FUNCTION(Runtime_CurrentFrameIsTurbofan) {
  HandleScope scope(isolate);
  DCHECK_EQ(args.length(), 0);
  JavaScriptStackFrameIterator it(isolate);
  return isolate->heap()->ToBoolean(it.frame()->is_turbofan());
}

#ifdef V8_ENABLE_MAGLEV
RUNTIME_FUNCTION(Runtime_OptimizeMaglevOnNextCall) {
  HandleScope scope(isolate);
  return OptimizeFunctionOnNextCall(args, isolate, CodeKind::MAGLEV);
}
#else
RUNTIME_FUNCTION(Runtime_OptimizeMaglevOnNextCall) {
  PrintF("Maglev is not enabled.\n");
  return ReadOnlyRoots(isolate).undefined_value();
}
#endif  // V8_ENABLE_MAGLEV

// TODO(jgruber): Rename to OptimizeTurbofanOnNextCall.
RUNTIME_FUNCTION(Runtime_OptimizeFunctionOnNextCall) {
  HandleScope scope(isolate);
  return OptimizeFunctionOnNextCall(
      args, isolate,
      v8_flags.optimize_on_next_call_optimizes_to_maglev ? CodeKind::MAGLEV
                                                         : CodeKind::TURBOFAN);
}

RUNTIME_FUNCTION(Runtime_EnsureFeedbackVectorForFunction) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<JSFunction> function = args.at<JSFunction>(0);
  if (function->has_feedback_vector()) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  IsCompiledScope is_compiled_scope;
  EnsureCompiledAndFeedbackVector(isolate, function, &is_compiled_scope);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_PrepareFunctionForOptimization) {
  HandleScope scope(isolate);
  if ((args.length() != 1 && args.length() != 2) || !args[0].IsJSFunction()) {
    return CrashUnlessFuzzing(isolate);
  }
  Handle<JSFunction> function = args.at<JSFunction>(0);

  IsCompiledScope is_compiled_scope;
  if (!EnsureCompiledAndFeedbackVector(isolate, function, &is_compiled_scope)) {
    return CrashUnlessFuzzing(isolate);
  }

  // If optimization is disabled for the function, return without marking it for
  // manual optimization
  if (function->shared().optimization_disabled() &&
      function->shared().disabled_optimization_reason() ==
          BailoutReason::kNeverOptimize) {
    return CrashUnlessFuzzing(isolate);
  }

  if (IsAsmWasmFunction(isolate, *function)) return CrashUnlessFuzzing(isolate);

  // Hold onto the bytecode array between marking and optimization to ensure
  // it's not flushed.
  if (v8_flags.testing_d8_test_runner || v8_flags.allow_natives_syntax) {
    ManualOptimizationTable::MarkFunctionForManualOptimization(
        isolate, function, &is_compiled_scope);
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

namespace {

void FinalizeOptimization(Isolate* isolate) {
  DCHECK(isolate->concurrent_recompilation_enabled());
  isolate->optimizing_compile_dispatcher()->AwaitCompileTasks();
  isolate->optimizing_compile_dispatcher()->InstallOptimizedFunctions();
  isolate->optimizing_compile_dispatcher()->set_finalize(true);

#if V8_ENABLE_MAGLEV
  if (isolate->maglev_concurrent_dispatcher()->is_enabled()) {
    isolate->maglev_concurrent_dispatcher()->AwaitCompileJobs();
    isolate->maglev_concurrent_dispatcher()->FinalizeFinishedJobs();
  }
#endif  // V8_ENABLE_MAGLEV
}

BytecodeOffset OffsetOfNextJumpLoop(Isolate* isolate,
                                    Handle<BytecodeArray> bytecode_array,
                                    int current_offset) {
  interpreter::BytecodeArrayIterator it(bytecode_array, current_offset);

  // First, look for a loop that contains the current bytecode offset.
  for (; !it.done(); it.Advance()) {
    if (it.current_bytecode() != interpreter::Bytecode::kJumpLoop) {
      continue;
    }
    if (!base::IsInRange(current_offset, it.GetJumpTargetOffset(),
                         it.current_offset())) {
      continue;
    }

    return BytecodeOffset(it.current_offset());
  }

  // Fall back to any loop after the current offset.
  it.SetOffset(current_offset);
  for (; !it.done(); it.Advance()) {
    if (it.current_bytecode() == interpreter::Bytecode::kJumpLoop) {
      return BytecodeOffset(it.current_offset());
    }
  }

  return BytecodeOffset::None();
}

}  // namespace

RUNTIME_FUNCTION(Runtime_OptimizeOsr) {
  HandleScope handle_scope(isolate);
  DCHECK(args.length() == 0 || args.length() == 1);

  Handle<JSFunction> function;

  // The optional parameter determines the frame being targeted.
  int stack_depth = 0;
  if (args.length() == 1) {
    if (!args[0].IsSmi()) return CrashUnlessFuzzing(isolate);
    stack_depth = args.smi_value_at(0);
  }

  // Find the JavaScript function on the top of the stack.
  JavaScriptStackFrameIterator it(isolate);
  while (!it.done() && stack_depth--) it.Advance();
  if (!it.done()) {
    if (it.frame()->is_turbofan()) {
      // This can happen if %OptimizeOsr is in inlined function.
      return ReadOnlyRoots(isolate).undefined_value();
    } else if (it.frame()->is_maglev()) {
      function = MaglevFrame::cast(it.frame())->GetInnermostFunction();
    } else {
      function = handle(it.frame()->function(), isolate);
    }
  }
  if (function.is_null()) return CrashUnlessFuzzing(isolate);

  if (V8_UNLIKELY(!v8_flags.turbofan) || V8_UNLIKELY(!v8_flags.use_osr)) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  if (!function->shared().allows_lazy_compilation()) {
    return CrashUnlessFuzzing(isolate);
  }

  if (function->shared().optimization_disabled() &&
      function->shared().disabled_optimization_reason() ==
          BailoutReason::kNeverOptimize) {
    return CrashUnlessFuzzing(isolate);
  }

  if (v8_flags.testing_d8_test_runner) {
    ManualOptimizationTable::CheckMarkedForManualOptimization(isolate,
                                                              *function);
  }

  if (function->HasAvailableOptimizedCode() &&
      !function->code().is_maglevved()) {
    DCHECK(function->HasAttachedOptimizedCode() ||
           function->ChecksTieringState());
    // If function is already optimized, return.
    return ReadOnlyRoots(isolate).undefined_value();
  }

  if (!it.frame()->is_unoptimized() && !it.frame()->is_maglev()) {
    // Nothing to be done.
    return ReadOnlyRoots(isolate).undefined_value();
  }

  IsCompiledScope is_compiled_scope(
      function->shared().is_compiled_scope(isolate));
  JSFunction::EnsureFeedbackVector(isolate, function, &is_compiled_scope);
  isolate->tiering_manager()->RequestOsrAtNextOpportunity(*function);

  // If concurrent OSR is enabled, the testing workflow is a bit tricky. We
  // must guarantee that the next JumpLoop installs the finished OSR'd code
  // object, but we still want to exercise concurrent code paths. To do so,
  // we attempt to find the next JumpLoop, start an OSR job for it now, and
  // immediately force finalization.
  // If this succeeds and we correctly match up the next JumpLoop, once we
  // reach the JumpLoop we'll hit the OSR cache and install the generated code.
  // If not (e.g. because we enter a nested loop first), the next JumpLoop will
  // see the cached OSR code with a mismatched offset, and trigger
  // non-concurrent OSR compilation and installation.
  if (isolate->concurrent_recompilation_enabled() && v8_flags.concurrent_osr) {
    BytecodeOffset osr_offset = BytecodeOffset::None();
    if (it.frame()->is_unoptimized()) {
      UnoptimizedFrame* frame = UnoptimizedFrame::cast(it.frame());
      Handle<BytecodeArray> bytecode_array(frame->GetBytecodeArray(), isolate);
      const int current_offset = frame->GetBytecodeOffset();
      osr_offset =
          OffsetOfNextJumpLoop(isolate, bytecode_array, current_offset);
    } else {
      MaglevFrame* frame = MaglevFrame::cast(it.frame());
      Handle<BytecodeArray> bytecode_array(
          function->shared().GetBytecodeArray(isolate), isolate);
      const int current_offset = frame->GetBytecodeOffsetForOSR().ToInt();
      osr_offset =
          OffsetOfNextJumpLoop(isolate, bytecode_array, current_offset);
    }

    if (osr_offset.IsNone()) {
      // The loop may have been elided by bytecode generation (e.g. for
      // patterns such as `do { ... } while (false);`.
      return ReadOnlyRoots(isolate).undefined_value();
    }

    // Finalize first to ensure all pending tasks are done (since we can't
    // queue more than one OSR job for each function).
    FinalizeOptimization(isolate);

    // Queue the job.
    auto unused_result = Compiler::CompileOptimizedOSR(
        isolate, function, osr_offset, ConcurrencyMode::kConcurrent);
    USE(unused_result);

    // Finalize again to finish the queued job. The next call into
    // Runtime::kCompileOptimizedOSR will pick up the cached InstructionStream
    // object.
    FinalizeOptimization(isolate);
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_BaselineOsr) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());

  // Find the JavaScript function on the top of the stack.
  JavaScriptStackFrameIterator it(isolate);
  Handle<JSFunction> function = handle(it.frame()->function(), isolate);
  if (function.is_null()) return CrashUnlessFuzzing(isolate);
  if (!v8_flags.sparkplug || !v8_flags.use_osr) {
    return ReadOnlyRoots(isolate).undefined_value();
  }
  if (!it.frame()->is_unoptimized()) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  IsCompiledScope is_compiled_scope(
      function->shared().is_compiled_scope(isolate));
  Compiler::CompileBaseline(isolate, function, Compiler::CLEAR_EXCEPTION,
                            &is_compiled_scope);

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_NeverOptimizeFunction) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<Object> function_object = args.at(0);
  PtrComprCageBase cage_base(isolate);
  if (!function_object->IsJSFunction(cage_base)) {
    return CrashUnlessFuzzing(isolate);
  }
  Handle<JSFunction> function = Handle<JSFunction>::cast(function_object);
  Handle<SharedFunctionInfo> sfi(function->shared(cage_base), isolate);
  CodeKind code_kind = sfi->abstract_code(isolate).kind(cage_base);
  if (code_kind != CodeKind::INTERPRETED_FUNCTION &&
      code_kind != CodeKind::BUILTIN) {
    return CrashUnlessFuzzing(isolate);
  }
  // Make sure to finish compilation if there is a parallel lazy compilation in
  // progress, to make sure that the compilation finalization doesn't clobber
  // the SharedFunctionInfo's disable_optimization field.
  if (isolate->lazy_compile_dispatcher() &&
      isolate->lazy_compile_dispatcher()->IsEnqueued(sfi)) {
    isolate->lazy_compile_dispatcher()->FinishNow(sfi);
  }

  sfi->DisableOptimization(isolate, BailoutReason::kNeverOptimize);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_GetOptimizationStatus) {
  HandleScope scope(isolate);
  DCHECK_EQ(args.length(), 1);

  int status = 0;
  if (v8_flags.lite_mode || v8_flags.jitless || !V8_ENABLE_TURBOFAN_BOOL) {
    // These modes cannot optimize. Unit tests should handle these the same
    // way.
    status |= static_cast<int>(OptimizationStatus::kLiteMode);
  }
  if (!isolate->use_optimizer()) {
    status |= static_cast<int>(OptimizationStatus::kNeverOptimize);
  }
  if (v8_flags.always_turbofan || v8_flags.prepare_always_turbofan) {
    status |= static_cast<int>(OptimizationStatus::kAlwaysOptimize);
  }
  if (v8_flags.deopt_every_n_times) {
    status |= static_cast<int>(OptimizationStatus::kMaybeDeopted);
  }
  if (v8_flags.optimize_on_next_call_optimizes_to_maglev) {
    status |= static_cast<int>(
        OptimizationStatus::kOptimizeOnNextCallOptimizesToMaglev);
  }

  Handle<Object> function_object = args.at(0);
  if (function_object->IsUndefined()) return Smi::FromInt(status);
  if (!function_object->IsJSFunction()) return CrashUnlessFuzzing(isolate);

  Handle<JSFunction> function = Handle<JSFunction>::cast(function_object);
  status |= static_cast<int>(OptimizationStatus::kIsFunction);

  switch (function->tiering_state()) {
    case TieringState::kRequestTurbofan_Synchronous:
      status |= static_cast<int>(OptimizationStatus::kMarkedForOptimization);
      break;
    case TieringState::kRequestTurbofan_Concurrent:
      status |= static_cast<int>(
          OptimizationStatus::kMarkedForConcurrentOptimization);
      break;
    case TieringState::kInProgress:
      status |= static_cast<int>(OptimizationStatus::kOptimizingConcurrently);
      break;
    case TieringState::kNone:
    case TieringState::kRequestMaglev_Synchronous:
    case TieringState::kRequestMaglev_Concurrent:
      // TODO(v8:7700): Maglev support.
      break;
  }

  if (function->HasAttachedOptimizedCode()) {
    Code code = function->code();
    if (code.marked_for_deoptimization()) {
      status |= static_cast<int>(OptimizationStatus::kMarkedForDeoptimization);
    } else {
      status |= static_cast<int>(OptimizationStatus::kOptimized);
    }
    if (code.is_maglevved()) {
      status |= static_cast<int>(OptimizationStatus::kMaglevved);
    } else if (code.is_turbofanned()) {
      status |= static_cast<int>(OptimizationStatus::kTurboFanned);
    }
  }
  if (function->HasAttachedCodeKind(CodeKind::BASELINE)) {
    status |= static_cast<int>(OptimizationStatus::kBaseline);
  }
  if (function->ActiveTierIsIgnition()) {
    status |= static_cast<int>(OptimizationStatus::kInterpreted);
  }
  if (!function->is_compiled()) {
    status |= static_cast<int>(OptimizationStatus::kIsLazy);
  }

  // Additionally, detect activations of this frame on the stack, and report the
  // status of the topmost frame.
  JavaScriptFrame* frame = nullptr;
  JavaScriptStackFrameIterator it(isolate);
  while (!it.done()) {
    if (it.frame()->function() == *function) {
      frame = it.frame();
      break;
    }
    it.Advance();
  }
  if (frame != nullptr) {
    status |= static_cast<int>(OptimizationStatus::kIsExecuting);
    if (frame->is_turbofan()) {
      status |=
          static_cast<int>(OptimizationStatus::kTopmostFrameIsTurboFanned);
    } else if (frame->is_interpreted()) {
      status |=
          static_cast<int>(OptimizationStatus::kTopmostFrameIsInterpreted);
    } else if (frame->is_baseline()) {
      status |= static_cast<int>(OptimizationStatus::kTopmostFrameIsBaseline);
    } else if (frame->is_maglev()) {
      status |= static_cast<int>(OptimizationStatus::kTopmostFrameIsMaglev);
    }
  }

  return Smi::FromInt(status);
}

RUNTIME_FUNCTION(Runtime_DisableOptimizationFinalization) {
  DCHECK_EQ(0, args.length());
  if (isolate->concurrent_recompilation_enabled()) {
    isolate->optimizing_compile_dispatcher()->AwaitCompileTasks();
    isolate->optimizing_compile_dispatcher()->InstallOptimizedFunctions();
    isolate->stack_guard()->ClearInstallCode();
    isolate->optimizing_compile_dispatcher()->set_finalize(false);
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_WaitForBackgroundOptimization) {
  DCHECK_EQ(0, args.length());
  if (isolate->concurrent_recompilation_enabled()) {
    isolate->optimizing_compile_dispatcher()->AwaitCompileTasks();
#if V8_ENABLE_MAGLEV
    if (isolate->maglev_concurrent_dispatcher()->is_enabled()) {
      isolate->maglev_concurrent_dispatcher()->AwaitCompileJobs();
    }
#endif  // V8_ENABLE_MAGLEV
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_FinalizeOptimization) {
  DCHECK_EQ(0, args.length());
  if (isolate->concurrent_recompilation_enabled()) {
    FinalizeOptimization(isolate);
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_ForceFlush) {
  HandleScope scope(isolate);
  if (args.length() != 1) return CrashUnlessFuzzing(isolate);

  Handle<Object> function_object = args.at(0);
  if (!function_object->IsJSFunction()) return CrashUnlessFuzzing(isolate);
  Handle<JSFunction> function = Handle<JSFunction>::cast(function_object);

  SharedFunctionInfo::DiscardCompiled(
      isolate, handle(function->shared(isolate), isolate));
  function->ResetIfCodeFlushed();
  return ReadOnlyRoots(isolate).undefined_value();
}

static void ReturnNull(const v8::FunctionCallbackInfo<v8::Value>& info) {
  DCHECK(ValidateCallbackInfo(info));
  info.GetReturnValue().SetNull();
}

RUNTIME_FUNCTION(Runtime_GetUndetectable) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  Local<v8::ObjectTemplate> desc = v8::ObjectTemplate::New(v8_isolate);
  desc->MarkAsUndetectable();
  desc->SetCallAsFunctionHandler(ReturnNull);
  Local<v8::Object> obj =
      desc->NewInstance(v8_isolate->GetCurrentContext()).ToLocalChecked();
  return *Utils::OpenHandle(*obj);
}

static void call_as_function(const v8::FunctionCallbackInfo<v8::Value>& info) {
  DCHECK(ValidateCallbackInfo(info));
  double v1 =
      info[0]->NumberValue(info.GetIsolate()->GetCurrentContext()).ToChecked();
  double v2 =
      info[1]->NumberValue(info.GetIsolate()->GetCurrentContext()).ToChecked();
  info.GetReturnValue().Set(v8::Number::New(info.GetIsolate(), v1 - v2));
}

// Returns a callable object. The object returns the difference of its two
// parameters when it is called.
RUNTIME_FUNCTION(Runtime_GetCallable) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(v8_isolate);
  Local<ObjectTemplate> instance_template = t->InstanceTemplate();
  instance_template->SetCallAsFunctionHandler(call_as_function);
  v8_isolate->GetCurrentContext();
  Local<v8::Object> instance =
      t->GetFunction(v8_isolate->GetCurrentContext())
          .ToLocalChecked()
          ->NewInstance(v8_isolate->GetCurrentContext())
          .ToLocalChecked();
  return *Utils::OpenHandle(*instance);
}

RUNTIME_FUNCTION(Runtime_ClearFunctionFeedback) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<JSFunction> function = args.at<JSFunction>(0);
  function->ClearAllTypeFeedbackInfoForTesting();
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_NotifyContextDisposed) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  isolate->heap()->NotifyContextDisposed(true);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_SetAllocationTimeout) {
  SealHandleScope shs(isolate);
  DCHECK(args.length() == 2 || args.length() == 3);
#ifdef V8_ENABLE_ALLOCATION_TIMEOUT
  CONVERT_INT32_ARG_FUZZ_SAFE(interval, 0);
  HeapAllocator::SetAllocationGcInterval(interval);
  CONVERT_INT32_ARG_FUZZ_SAFE(timeout, 1);
  isolate->heap()->set_allocation_timeout(timeout);
#endif
#ifdef DEBUG
  if (args.length() == 3) {
    // Enable/disable inline allocation if requested.
    CONVERT_BOOLEAN_ARG_FUZZ_SAFE(inline_allocation, 2);
    if (inline_allocation) {
      isolate->heap()->EnableInlineAllocation();
    } else {
      isolate->heap()->DisableInlineAllocation();
    }
  }
#endif
  return ReadOnlyRoots(isolate).undefined_value();
}

namespace {

int FixedArrayLenFromSize(int size) {
  return std::min({(size - FixedArray::kHeaderSize) / kTaggedSize,
                   FixedArray::kMaxRegularLength});
}

int GetSpaceRemainingOnCurrentPage(v8::internal::NewSpace* space) {
  Address top = space->top();
  if ((top & kPageAlignmentMask) == 0) {
    // `top` points to the start of a page signifies that there is not room in
    // the current page.
    return 0;
  }
  return static_cast<int>(Page::FromAddress(space->top())->area_end() - top);
}

void FillUpOneNewSpacePage(Isolate* isolate, Heap* heap) {
  DCHECK(!v8_flags.single_generation);
  PauseAllocationObserversScope pause_observers(heap);
  NewSpace* space = heap->new_space();
  // We cannot rely on `space->limit()` to point to the end of the current page
  // in the case where inline allocations are disabled, it actually points to
  // the current allocation pointer.
  DCHECK_IMPLIES(!heap->IsInlineAllocationEnabled(),
                 space->limit() == space->top());
  int space_remaining = GetSpaceRemainingOnCurrentPage(space);
  while (space_remaining > 0) {
    int length = FixedArrayLenFromSize(space_remaining);
    if (length > 0) {
      Handle<FixedArray> padding =
          isolate->factory()->NewFixedArray(length, AllocationType::kYoung);
      DCHECK(heap->new_space()->Contains(*padding));
      space_remaining -= padding->Size();
    } else {
      // Not enough room to create another fixed array. Create a filler.
      heap->CreateFillerObjectAt(*heap->new_space()->allocation_top_address(),
                                 space_remaining);
      break;
    }
  }
}

}  // namespace

RUNTIME_FUNCTION(Runtime_SimulateNewspaceFull) {
  HandleScope scope(isolate);
  Heap* heap = isolate->heap();
  NewSpace* space = heap->new_space();
  AlwaysAllocateScopeForTesting always_allocate(heap);
  do {
    FillUpOneNewSpacePage(isolate, heap);
  } while (space->AddFreshPage());

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_ScheduleGCInStackCheck) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  isolate->RequestInterrupt(
      [](v8::Isolate* isolate, void*) {
        isolate->RequestGarbageCollectionForTesting(
            v8::Isolate::kFullGarbageCollection);
      },
      nullptr);
  return ReadOnlyRoots(isolate).undefined_value();
}

class FileOutputStream : public v8::OutputStream {
 public:
  explicit FileOutputStream(const char* filename) : os_(filename) {}
  ~FileOutputStream() override { os_.close(); }

  WriteResult WriteAsciiChunk(char* data, int size) override {
    os_.write(data, size);
    return kContinue;
  }

  void EndOfStream() override { os_.close(); }

 private:
  std::ofstream os_;
};

RUNTIME_FUNCTION(Runtime_TakeHeapSnapshot) {
  if (v8_flags.fuzzing) {
    // We don't want to create snapshots in fuzzers.
    return ReadOnlyRoots(isolate).undefined_value();
  }

  std::string filename = "heap.heapsnapshot";

  if (args.length() >= 1) {
    HandleScope hs(isolate);
    Handle<String> filename_as_js_string = args.at<String>(0);
    std::unique_ptr<char[]> buffer = filename_as_js_string->ToCString();
    filename = std::string(buffer.get());
  }

  HeapProfiler* heap_profiler = isolate->heap_profiler();
  // Since this API is intended for V8 devs, we do not treat globals as roots
  // here on purpose.
  v8::HeapProfiler::HeapSnapshotOptions options;
  options.numerics_mode = v8::HeapProfiler::NumericsMode::kExposeNumericValues;
  options.snapshot_mode = v8::HeapProfiler::HeapSnapshotMode::kExposeInternals;
  HeapSnapshot* snapshot = heap_profiler->TakeSnapshot(options);
  FileOutputStream stream(filename.c_str());
  HeapSnapshotJSONSerializer serializer(snapshot);
  serializer.Serialize(&stream);
  return ReadOnlyRoots(isolate).undefined_value();
}

static void DebugPrintImpl(MaybeObject maybe_object, std::ostream& os) {
  if (maybe_object->IsCleared()) {
    os << "[weak cleared]";
  } else {
    Object object = maybe_object.GetHeapObjectOrSmi();
    bool weak = maybe_object.IsWeak();

#ifdef OBJECT_PRINT
    os << "DebugPrint: ";
    if (weak) os << "[weak] ";
    object.Print(os);
    if (object.IsHeapObject()) {
      HeapObject::cast(object).map().Print(os);
    }
#else
    if (weak) os << "[weak] ";
    // ShortPrint is available in release mode. Print is not.
    os << Brief(object);
#endif
  }
  os << std::endl;
}

RUNTIME_FUNCTION(Runtime_DebugPrint) {
  SealHandleScope shs(isolate);

  // This is exposed to tests / fuzzers; handle variable arguments gracefully.
  std::unique_ptr<std::ostream> output_stream(new StdoutStream());
  if (args.length() >= 2) {
    // Args: object, stream.
    if (args[1].IsSmi()) {
      int output_int = Smi::cast(args[1]).value();
      if (output_int == fileno(stderr)) {
        output_stream.reset(new StderrStream());
      }
    }
  }

  MaybeObject maybe_object(*args.address_of_arg_at(0));
  DebugPrintImpl(maybe_object, *output_stream.get());
  return args[0];
}

RUNTIME_FUNCTION(Runtime_DebugPrintPtr) {
  SealHandleScope shs(isolate);
  StdoutStream os;
  DCHECK_EQ(1, args.length());

  MaybeObject maybe_object(*args.address_of_arg_at(0));
  if (!maybe_object.IsCleared()) {
    Object object = maybe_object.GetHeapObjectOrSmi();
    size_t pointer;
    if (object.ToIntegerIndex(&pointer)) {
      MaybeObject from_pointer(static_cast<Address>(pointer));
      DebugPrintImpl(from_pointer, os);
    }
  }
  // We don't allow the converted pointer to leak out to JavaScript.
  return args[0];
}

RUNTIME_FUNCTION(Runtime_PrintWithNameForAssert) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(2, args.length());

  auto name = String::cast(args[0]);

  PrintF(" * ");
  StringCharacterStream stream(name);
  while (stream.HasMore()) {
    uint16_t character = stream.GetNext();
    PrintF("%c", character);
  }
  PrintF(": ");
  args[1].ShortPrint();
  PrintF("\n");

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugTrace) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  isolate->PrintStack(stdout);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugTrackRetainingPath) {
  HandleScope scope(isolate);
  DCHECK_LE(1, args.length());
  DCHECK_GE(2, args.length());
  CHECK(v8_flags.track_retaining_path);
  Handle<HeapObject> object = args.at<HeapObject>(0);
  RetainingPathOption option = RetainingPathOption::kDefault;
  if (args.length() == 2) {
    Handle<String> str = args.at<String>(1);
    const char track_ephemeron_path[] = "track-ephemeron-path";
    if (str->IsOneByteEqualTo(base::StaticCharVector(track_ephemeron_path))) {
      option = RetainingPathOption::kTrackEphemeronPath;
    } else {
      CHECK_EQ(str->length(), 0);
    }
  }
  isolate->heap()->AddRetainingPathTarget(object, option);
  return ReadOnlyRoots(isolate).undefined_value();
}

// This will not allocate (flatten the string), but it may run
// very slowly for very deeply nested ConsStrings.  For debugging use only.
RUNTIME_FUNCTION(Runtime_GlobalPrint) {
  SealHandleScope shs(isolate);

  // This is exposed to tests / fuzzers; handle variable arguments gracefully.
  FILE* output_stream = stdout;
  if (args.length() >= 2) {
    // Args: object, stream.
    if (args[1].IsSmi()) {
      int output_int = Smi::cast(args[1]).value();
      if (output_int == fileno(stderr)) {
        output_stream = stderr;
      }
    }
  }

  if (!args[0].IsString()) {
    return args[0];
  }

  auto string = String::cast(args[0]);
  StringCharacterStream stream(string);
  while (stream.HasMore()) {
    uint16_t character = stream.GetNext();
    PrintF(output_stream, "%c", character);
  }
  fflush(output_stream);
  return string;
}

RUNTIME_FUNCTION(Runtime_SystemBreak) {
  // The code below doesn't create handles, but when breaking here in GDB
  // having a handle scope might be useful.
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  base::OS::DebugBreak();
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_SetForceSlowPath) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  Object arg = args[0];
  if (arg.IsTrue(isolate)) {
    isolate->set_force_slow_path(true);
  } else {
    DCHECK(arg.IsFalse(isolate));
    isolate->set_force_slow_path(false);
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_Abort) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  int message_id = args.smi_value_at(0);
  const char* message = GetAbortReason(static_cast<AbortReason>(message_id));
  base::OS::PrintError("abort: %s\n", message);
  isolate->PrintStack(stderr);
  base::OS::Abort();
  UNREACHABLE();
}

RUNTIME_FUNCTION(Runtime_AbortJS) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> message = args.at<String>(0);
  if (v8_flags.disable_abortjs) {
    base::OS::PrintError("[disabled] abort: %s\n", message->ToCString().get());
    return Object();
  }
  base::OS::PrintError("abort: %s\n", message->ToCString().get());
  isolate->PrintStack(stderr);
  base::OS::Abort();
  UNREACHABLE();
}

RUNTIME_FUNCTION(Runtime_AbortCSADcheck) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<String> message = args.at<String>(0);
  base::OS::PrintError("abort: CSA_DCHECK failed: %s\n",
                       message->ToCString().get());
  isolate->PrintStack(stderr);
  base::OS::Abort();
  UNREACHABLE();
}

RUNTIME_FUNCTION(Runtime_DisassembleFunction) {
  HandleScope scope(isolate);
#ifdef DEBUG
  DCHECK_EQ(1, args.length());
  // Get the function and make sure it is compiled.
  Handle<JSFunction> func = args.at<JSFunction>(0);
  IsCompiledScope is_compiled_scope;
  if (!func->is_compiled() && func->HasAvailableOptimizedCode()) {
    func->set_code(func->feedback_vector().optimized_code());
  }
  CHECK(func->is_compiled() ||
        Compiler::Compile(isolate, func, Compiler::KEEP_EXCEPTION,
                          &is_compiled_scope));
  StdoutStream os;
  func->code().Print(os);
  os << std::endl;
#endif  // DEBUG
  return ReadOnlyRoots(isolate).undefined_value();
}

namespace {

int StackSize(Isolate* isolate) {
  int n = 0;
  for (JavaScriptStackFrameIterator it(isolate); !it.done(); it.Advance()) n++;
  return n;
}

void PrintIndentation(int stack_size) {
  const int max_display = 80;
  if (stack_size <= max_display) {
    PrintF("%4d:%*s", stack_size, stack_size, "");
  } else {
    PrintF("%4d:%*s", stack_size, max_display, "...");
  }
}

}  // namespace

RUNTIME_FUNCTION(Runtime_TraceEnter) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  PrintIndentation(StackSize(isolate));
  JavaScriptFrame::PrintTop(isolate, stdout, true, false);
  PrintF(" {\n");
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_TraceExit) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  Object obj = args[0];
  PrintIndentation(StackSize(isolate));
  PrintF("} -> ");
  obj.ShortPrint();
  PrintF("\n");
  return obj;  // return TOS
}

RUNTIME_FUNCTION(Runtime_HaveSameMap) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(2, args.length());
  auto obj1 = HeapObject::cast(args[0]);
  auto obj2 = HeapObject::cast(args[1]);
  return isolate->heap()->ToBoolean(obj1.map() == obj2.map());
}

RUNTIME_FUNCTION(Runtime_InLargeObjectSpace) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  auto obj = HeapObject::cast(args[0]);
  return isolate->heap()->ToBoolean(
      isolate->heap()->new_lo_space()->Contains(obj) ||
      isolate->heap()->code_lo_space()->Contains(obj) ||
      isolate->heap()->lo_space()->Contains(obj));
}

RUNTIME_FUNCTION(Runtime_HasElementsInALargeObjectSpace) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  auto array = JSArray::cast(args[0]);
  FixedArrayBase elements = array.elements();
  return isolate->heap()->ToBoolean(
      isolate->heap()->new_lo_space()->Contains(elements) ||
      isolate->heap()->lo_space()->Contains(elements));
}

RUNTIME_FUNCTION(Runtime_InYoungGeneration) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  Object obj = args[0];
  return isolate->heap()->ToBoolean(ObjectInYoungGeneration(obj));
}

// Force pretenuring for the allocation site the passed object belongs to.
RUNTIME_FUNCTION(Runtime_PretenureAllocationSite) {
  DisallowGarbageCollection no_gc;

  if (args.length() != 1) return CrashUnlessFuzzing(isolate);
  Object arg = args[0];
  if (!arg.IsJSObject()) return CrashUnlessFuzzing(isolate);
  JSObject object = JSObject::cast(arg);

  Heap* heap = object.GetHeap();
  if (!heap->InYoungGeneration(object)) {
    // Object is not in new space, thus there is no memento and nothing to do.
    return ReturnFuzzSafe(ReadOnlyRoots(isolate).false_value(), isolate);
  }

  PretenuringHandler* pretenuring_handler = heap->pretenuring_handler();
  AllocationMemento memento =
      pretenuring_handler
          ->FindAllocationMemento<PretenuringHandler::kForRuntime>(object.map(),
                                                                   object);
  if (memento.is_null())
    return ReturnFuzzSafe(ReadOnlyRoots(isolate).false_value(), isolate);
  AllocationSite site = memento.GetAllocationSite();
  pretenuring_handler->PretenureAllocationSiteOnNextCollection(site);
  return ReturnFuzzSafe(ReadOnlyRoots(isolate).true_value(), isolate);
}

namespace {

v8::ModifyCodeGenerationFromStringsResult DisallowCodegenFromStringsCallback(
    v8::Local<v8::Context> context, v8::Local<v8::Value> source,
    bool is_code_kind) {
  return {false, {}};
}

}  // namespace

RUNTIME_FUNCTION(Runtime_DisallowCodegenFromStrings) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  bool flag = Oddball::cast(args[0]).ToBool(isolate);
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8_isolate->SetModifyCodeGenerationFromStringsCallback(
      flag ? DisallowCodegenFromStringsCallback : nullptr);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_RegexpHasBytecode) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(2, args.length());
  auto regexp = JSRegExp::cast(args[0]);
  bool is_latin1 = Oddball::cast(args[1]).ToBool(isolate);
  bool result;
  if (regexp.type_tag() == JSRegExp::IRREGEXP) {
    result = regexp.bytecode(is_latin1).IsByteArray();
  } else {
    result = false;
  }
  return isolate->heap()->ToBoolean(result);
}

RUNTIME_FUNCTION(Runtime_RegexpHasNativeCode) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(2, args.length());
  auto regexp = JSRegExp::cast(args[0]);
  bool is_latin1 = Oddball::cast(args[1]).ToBool(isolate);
  bool result;
  if (regexp.type_tag() == JSRegExp::IRREGEXP) {
    result = regexp.code(is_latin1).IsCode();
  } else {
    result = false;
  }
  return isolate->heap()->ToBoolean(result);
}

RUNTIME_FUNCTION(Runtime_RegexpTypeTag) {
  HandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  auto regexp = JSRegExp::cast(args[0]);
  const char* type_str;
  switch (regexp.type_tag()) {
    case JSRegExp::NOT_COMPILED:
      type_str = "NOT_COMPILED";
      break;
    case JSRegExp::ATOM:
      type_str = "ATOM";
      break;
    case JSRegExp::IRREGEXP:
      type_str = "IRREGEXP";
      break;
    case JSRegExp::EXPERIMENTAL:
      type_str = "EXPERIMENTAL";
      break;
  }
  return *isolate->factory()->NewStringFromAsciiChecked(type_str);
}

RUNTIME_FUNCTION(Runtime_RegexpIsUnmodified) {
  HandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  Handle<JSRegExp> regexp = args.at<JSRegExp>(0);
  return isolate->heap()->ToBoolean(
      RegExp::IsUnmodifiedRegExp(isolate, regexp));
}

#define ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(Name) \
  RUNTIME_FUNCTION(Runtime_##Name) {               \
    auto obj = JSObject::cast(args[0]);            \
    return isolate->heap()->ToBoolean(obj.Name()); \
  }

ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasFastElements)
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasSmiElements)
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasObjectElements)
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasSmiOrObjectElements)
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasDoubleElements)
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasHoleyElements)
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasDictionaryElements)
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasPackedElements)
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasSloppyArgumentsElements)
// Properties test sitting with elements tests - not fooling anyone.
ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION(HasFastProperties)

#undef ELEMENTS_KIND_CHECK_RUNTIME_FUNCTION

#define FIXED_TYPED_ARRAYS_CHECK_RUNTIME_FUNCTION(Type, type, TYPE, ctype) \
  RUNTIME_FUNCTION(Runtime_HasFixed##Type##Elements) {                     \
    auto obj = JSObject::cast(args[0]);                                    \
    return isolate->heap()->ToBoolean(obj.HasFixed##Type##Elements());     \
  }

TYPED_ARRAYS(FIXED_TYPED_ARRAYS_CHECK_RUNTIME_FUNCTION)

#undef FIXED_TYPED_ARRAYS_CHECK_RUNTIME_FUNCTION

RUNTIME_FUNCTION(Runtime_IsConcatSpreadableProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsIsConcatSpreadableLookupChainIntact(isolate));
}

RUNTIME_FUNCTION(Runtime_TypedArraySpeciesProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsTypedArraySpeciesLookupChainIntact(isolate));
}

RUNTIME_FUNCTION(Runtime_RegExpSpeciesProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsRegExpSpeciesLookupChainIntact(isolate));
}

RUNTIME_FUNCTION(Runtime_PromiseSpeciesProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsPromiseSpeciesLookupChainIntact(isolate));
}

RUNTIME_FUNCTION(Runtime_ArraySpeciesProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsArraySpeciesLookupChainIntact(isolate));
}

RUNTIME_FUNCTION(Runtime_MapIteratorProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsMapIteratorLookupChainIntact(isolate));
}

RUNTIME_FUNCTION(Runtime_SetIteratorProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsSetIteratorLookupChainIntact(isolate));
}

RUNTIME_FUNCTION(Runtime_StringIteratorProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsStringIteratorLookupChainIntact(isolate));
}

RUNTIME_FUNCTION(Runtime_ArrayIteratorProtector) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(
      Protectors::IsArrayIteratorLookupChainIntact(isolate));
}
// For use by tests and fuzzers. It
//
// 1. serializes a snapshot of the current isolate,
// 2. deserializes the snapshot,
// 3. and runs VerifyHeap on the resulting isolate.
//
// The current isolate should not be modified by this call and can keep running
// once it completes.
RUNTIME_FUNCTION(Runtime_SerializeDeserializeNow) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  Snapshot::SerializeDeserializeAndVerifyForTesting(isolate,
                                                    isolate->native_context());
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_HeapObjectVerify) {
  HandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  Handle<Object> object = args.at(0);
#ifdef VERIFY_HEAP
  object->ObjectVerify(isolate);
#else
  CHECK(object->IsObject());
  if (object->IsHeapObject()) {
    CHECK(HeapObject::cast(*object).map().IsMap());
  } else {
    CHECK(object->IsSmi());
  }
#endif
  return isolate->heap()->ToBoolean(true);
}

RUNTIME_FUNCTION(Runtime_ArrayBufferMaxByteLength) {
  HandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return *isolate->factory()->NewNumber(JSArrayBuffer::kMaxByteLength);
}

RUNTIME_FUNCTION(Runtime_TypedArrayMaxLength) {
  HandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return *isolate->factory()->NewNumber(JSTypedArray::kMaxLength);
}

RUNTIME_FUNCTION(Runtime_CompleteInobjectSlackTracking) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());

  Handle<JSObject> object = args.at<JSObject>(0);
  MapUpdater::CompleteInobjectSlackTracking(isolate, object->map());

  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_TurbofanStaticAssert) {
  SealHandleScope shs(isolate);
  // Always lowered to StaticAssert node in Turbofan, so we never get here in
  // compiled code.
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_IsBeingInterpreted) {
  SealHandleScope shs(isolate);
  // Always lowered to false in Turbofan, so we never get here in compiled code.
  return ReadOnlyRoots(isolate).true_value();
}

RUNTIME_FUNCTION(Runtime_EnableCodeLoggingForTesting) {
  // The {NoopListener} currently does nothing on any callback, but reports
  // {true} on {is_listening_to_code_events()}. Feel free to add assertions to
  // any method to further test the code logging callbacks.
  class NoopListener final : public LogEventListener {
    void CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                         const char* name) final {}
    void CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                         Handle<Name> name) final {}
    void CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                         Handle<SharedFunctionInfo> shared,
                         Handle<Name> script_name) final {}
    void CodeCreateEvent(CodeTag tag, Handle<AbstractCode> code,
                         Handle<SharedFunctionInfo> shared,
                         Handle<Name> script_name, int line, int column) final {
    }
#if V8_ENABLE_WEBASSEMBLY
    void CodeCreateEvent(CodeTag tag, const wasm::WasmCode* code,
                         wasm::WasmName name, const char* source_url,
                         int code_offset, int script_id) final {}
#endif  // V8_ENABLE_WEBASSEMBLY

    void CallbackEvent(Handle<Name> name, Address entry_point) final {}
    void GetterCallbackEvent(Handle<Name> name, Address entry_point) final {}
    void SetterCallbackEvent(Handle<Name> name, Address entry_point) final {}
    void RegExpCodeCreateEvent(Handle<AbstractCode> code,
                               Handle<String> source) final {}
    void CodeMoveEvent(InstructionStream from, InstructionStream to) final {}
    void BytecodeMoveEvent(BytecodeArray from, BytecodeArray to) final {}
    void SharedFunctionInfoMoveEvent(Address from, Address to) final {}
    void NativeContextMoveEvent(Address from, Address to) final {}
    void CodeMovingGCEvent() final {}
    void CodeDisableOptEvent(Handle<AbstractCode> code,
                             Handle<SharedFunctionInfo> shared) final {}
    void CodeDeoptEvent(Handle<Code> code, DeoptimizeKind kind, Address pc,
                        int fp_to_sp_delta) final {}
    void CodeDependencyChangeEvent(Handle<Code> code,
                                   Handle<SharedFunctionInfo> shared,
                                   const char* reason) final {}
    void WeakCodeClearEvent() final {}

    bool is_listening_to_code_events() final { return true; }
  };
  static base::LeakyObject<NoopListener> noop_listener;
#if V8_ENABLE_WEBASSEMBLY
  wasm::GetWasmEngine()->EnableCodeLogging(isolate);
#endif  // V8_ENABLE_WEBASSEMBLY
  isolate->logger()->AddListener(noop_listener.get());
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_NewRegExpWithBacktrackLimit) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());

  Handle<String> pattern = args.at<String>(0);
  Handle<String> flags_string = args.at<String>(1);
  uint32_t backtrack_limit = args.positive_smi_value_at(2);

  JSRegExp::Flags flags =
      JSRegExp::FlagsFromString(isolate, flags_string).value();

  RETURN_RESULT_OR_FAILURE(
      isolate, JSRegExp::New(isolate, pattern, flags, backtrack_limit));
}

RUNTIME_FUNCTION(Runtime_Is64Bit) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  return isolate->heap()->ToBoolean(kSystemPointerSize == 8);
}

RUNTIME_FUNCTION(Runtime_BigIntMaxLengthBits) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  return *isolate->factory()->NewNumber(BigInt::kMaxLengthBits);
}

RUNTIME_FUNCTION(Runtime_IsSameHeapObject) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<HeapObject> obj1 = args.at<HeapObject>(0);
  Handle<HeapObject> obj2 = args.at<HeapObject>(1);
  return isolate->heap()->ToBoolean(obj1->address() == obj2->address());
}

RUNTIME_FUNCTION(Runtime_IsSharedString) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<HeapObject> obj = args.at<HeapObject>(0);
  return isolate->heap()->ToBoolean(obj->IsString() &&
                                    Handle<String>::cast(obj)->IsShared());
}

RUNTIME_FUNCTION(Runtime_IsInPlaceInternalizableString) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<HeapObject> obj = args.at<HeapObject>(0);
  return isolate->heap()->ToBoolean(
      obj->IsString() && String::IsInPlaceInternalizable(String::cast(*obj)));
}

RUNTIME_FUNCTION(Runtime_IsInternalizedString) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<HeapObject> obj = args.at<HeapObject>(0);
  return isolate->heap()->ToBoolean(obj->IsInternalizedString());
}

RUNTIME_FUNCTION(Runtime_SharedGC) {
  SealHandleScope scope(isolate);
  isolate->heap()->CollectGarbageShared(isolate->main_thread_local_heap(),
                                        GarbageCollectionReason::kTesting);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_AtomicsConditionNumWaitersForTesting) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<JSAtomicsCondition> cv = args.at<JSAtomicsCondition>(0);
  return cv->NumWaitersForTesting(isolate);
}

RUNTIME_FUNCTION(Runtime_GetWeakCollectionSize) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  Handle<JSWeakCollection> collection = args.at<JSWeakCollection>(0);

  return Smi::FromInt(
      EphemeronHashTable::cast(collection->table()).NumberOfElements());
}

}  // namespace internal
}  // namespace v8
