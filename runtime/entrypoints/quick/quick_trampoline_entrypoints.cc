/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "art_method-inl.h"
#include "base/callee_save_type.h"
#include "base/enums.h"
#include "callee_save_frame.h"
#include "common_throws.h"
#include "class_root-inl.h"
#include "debug_print.h"
#include "debugger.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_types.h"
#include "dex/dex_instruction-inl.h"
#include "dex/method_reference.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "entrypoints/quick/callee_save_frame.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "imt_conflict_table.h"
#include "imtable-inl.h"
#include "instrumentation.h"
#include "interpreter/interpreter.h"
#include "interpreter/interpreter_common.h"
#include "interpreter/shadow_frame-inl.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "linear_alloc.h"
#include "method_handles.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/method.h"
#include "mirror/method_handle_impl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/var_handle.h"
#include "oat.h"
#include "oat_file.h"
#include "oat_quick_method_header.h"
#include "quick_exception_handler.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "stack.h"
#include "thread-inl.h"
#include "var_handles.h"
#include "well_known_classes.h"

namespace art {

// Visits the arguments as saved to the stack by a CalleeSaveType::kRefAndArgs callee save frame.
class QuickArgumentVisitor {
  // Number of bytes for each out register in the caller method's frame.
  static constexpr size_t kBytesStackArgLocation = 4;
  // Frame size in bytes of a callee-save frame for RefsAndArgs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_FrameSize =
      RuntimeCalleeSaveFrame::GetFrameSize(CalleeSaveType::kSaveRefsAndArgs);
  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset =
      RuntimeCalleeSaveFrame::GetGpr1Offset(CalleeSaveType::kSaveRefsAndArgs);
  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset =
      RuntimeCalleeSaveFrame::GetFpr1Offset(CalleeSaveType::kSaveRefsAndArgs);
  // Offset of return address.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_ReturnPcOffset =
      RuntimeCalleeSaveFrame::GetReturnPcOffset(CalleeSaveType::kSaveRefsAndArgs);
#if defined(__arm__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | ...        |    4x6 bytes callee saves
  // | R3         |
  // | R2         |
  // | R1         |
  // | S15        |
  // | :          |
  // | S0         |
  // |            |    4x2 bytes padding
  // | Method*    |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = true;
  static constexpr bool kQuickSoftFloatAbi = false;
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = true;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 3;
  static constexpr size_t kNumQuickFprArgs = 16;
  static constexpr bool kGprFprLockstep = false;
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__aarch64__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | X29        |
  // |  :         |
  // | X20        |
  // | X7         |
  // | :          |
  // | X1         |
  // | D7         |
  // |  :         |
  // | D0         |
  // |            |    padding
  // | Method*    |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = false;
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = false;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 7;  // 7 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 8;  // 8 arguments passed in FPRs.
  static constexpr bool kGprFprLockstep = false;
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__i386__)
  // The callee save frame is pointed to by SP.
  // | argN        |  |
  // | ...         |  |
  // | arg4        |  |
  // | arg3 spill  |  |  Caller's frame
  // | arg2 spill  |  |
  // | arg1 spill  |  |
  // | Method*     | ---
  // | Return      |
  // | EBP,ESI,EDI |    callee saves
  // | EBX         |    arg3
  // | EDX         |    arg2
  // | ECX         |    arg1
  // | XMM3        |    float arg 4
  // | XMM2        |    float arg 3
  // | XMM1        |    float arg 2
  // | XMM0        |    float arg 1
  // | EAX/Method* |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = false;
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = false;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 3;  // 3 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 4;  // 4 arguments passed in FPRs.
  static constexpr bool kGprFprLockstep = false;
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__x86_64__)
  // The callee save frame is pointed to by SP.
  // | argN            |  |
  // | ...             |  |
  // | reg. arg spills |  |  Caller's frame
  // | Method*         | ---
  // | Return          |
  // | R15             |    callee save
  // | R14             |    callee save
  // | R13             |    callee save
  // | R12             |    callee save
  // | R9              |    arg5
  // | R8              |    arg4
  // | RSI/R6          |    arg1
  // | RBP/R5          |    callee save
  // | RBX/R3          |    callee save
  // | RDX/R2          |    arg2
  // | RCX/R1          |    arg3
  // | XMM7            |    float arg 8
  // | XMM6            |    float arg 7
  // | XMM5            |    float arg 6
  // | XMM4            |    float arg 5
  // | XMM3            |    float arg 4
  // | XMM2            |    float arg 3
  // | XMM1            |    float arg 2
  // | XMM0            |    float arg 1
  // | Padding         |
  // | RDI/Method*     |  <- sp
  static constexpr bool kSplitPairAcrossRegisterAndStack = false;
  static constexpr bool kAlignPairRegister = false;
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr bool kQuickDoubleRegAlignedFloatBackFilled = false;
  static constexpr bool kQuickSkipOddFpRegisters = false;
  static constexpr size_t kNumQuickGprArgs = 5;  // 5 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 8;  // 8 arguments passed in FPRs.
  static constexpr bool kGprFprLockstep = false;
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    switch (gpr_index) {
      case 0: return (4 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 1: return (1 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 2: return (0 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 3: return (5 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 4: return (6 * GetBytesPerGprSpillLocation(kRuntimeISA));
      default:
      LOG(FATAL) << "Unexpected GPR index: " << gpr_index;
      UNREACHABLE();
    }
  }
#else
#error "Unsupported architecture"
#endif

 public:
  // Special handling for proxy methods. Proxy methods are instance methods so the
  // 'this' object is the 1st argument. They also have the same frame layout as the
  // kRefAndArgs runtime method. Since 'this' is a reference, it is located in the
  // 1st GPR.
  static StackReference<mirror::Object>* GetProxyThisObjectReference(ArtMethod** sp)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    CHECK((*sp)->IsProxyMethod());
    CHECK_GT(kNumQuickGprArgs, 0u);
    constexpr uint32_t kThisGprIndex = 0u;  // 'this' is in the 1st GPR.
    size_t this_arg_offset = kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset +
        GprIndexToGprOffset(kThisGprIndex);
    uint8_t* this_arg_address = reinterpret_cast<uint8_t*>(sp) + this_arg_offset;
    return reinterpret_cast<StackReference<mirror::Object>*>(this_arg_address);
  }

  static ArtMethod* GetCallingMethod(ArtMethod** sp) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    return GetCalleeSaveMethodCaller(sp, CalleeSaveType::kSaveRefsAndArgs);
  }

  static ArtMethod* GetOuterMethod(ArtMethod** sp) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    uint8_t* previous_sp =
        reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_FrameSize;
    return *reinterpret_cast<ArtMethod**>(previous_sp);
  }

  static uint32_t GetCallingDexPc(ArtMethod** sp) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    constexpr size_t callee_frame_size =
        RuntimeCalleeSaveFrame::GetFrameSize(CalleeSaveType::kSaveRefsAndArgs);
    ArtMethod** caller_sp = reinterpret_cast<ArtMethod**>(
        reinterpret_cast<uintptr_t>(sp) + callee_frame_size);
    uintptr_t outer_pc = QuickArgumentVisitor::GetCallingPc(sp);
    const OatQuickMethodHeader* current_code = (*caller_sp)->GetOatQuickMethodHeader(outer_pc);
    uintptr_t outer_pc_offset = current_code->NativeQuickPcOffset(outer_pc);

    if (current_code->IsOptimized()) {
      CodeInfo code_info = CodeInfo::DecodeInlineInfoOnly(current_code);
      StackMap stack_map = code_info.GetStackMapForNativePcOffset(outer_pc_offset);
      DCHECK(stack_map.IsValid());
      BitTableRange<InlineInfo> inline_infos = code_info.GetInlineInfosOf(stack_map);
      if (!inline_infos.empty()) {
        return inline_infos.back().GetDexPc();
      } else {
        return stack_map.GetDexPc();
      }
    } else {
      return current_code->ToDexPc(caller_sp, outer_pc);
    }
  }

  static uint8_t* GetCallingPcAddr(ArtMethod** sp) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    uint8_t* return_adress_spill =
        reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_ReturnPcOffset;
    return return_adress_spill;
  }

  // For the given quick ref and args quick frame, return the caller's PC.
  static uintptr_t GetCallingPc(ArtMethod** sp) REQUIRES_SHARED(Locks::mutator_lock_) {
    return *reinterpret_cast<uintptr_t*>(GetCallingPcAddr(sp));
  }

  QuickArgumentVisitor(ArtMethod** sp, bool is_static, const char* shorty,
                       uint32_t shorty_len) REQUIRES_SHARED(Locks::mutator_lock_) :
          is_static_(is_static), shorty_(shorty), shorty_len_(shorty_len),
          gpr_args_(reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset),
          fpr_args_(reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset),
          stack_args_(reinterpret_cast<uint8_t*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_FrameSize
              + sizeof(ArtMethod*)),  // Skip ArtMethod*.
          gpr_index_(0), fpr_index_(0), fpr_double_index_(0), stack_index_(0),
          cur_type_(Primitive::kPrimVoid), is_split_long_or_double_(false) {
    static_assert(kQuickSoftFloatAbi == (kNumQuickFprArgs == 0),
                  "Number of Quick FPR arguments unexpected");
    static_assert(!(kQuickSoftFloatAbi && kQuickDoubleRegAlignedFloatBackFilled),
                  "Double alignment unexpected");
    // For register alignment, we want to assume that counters(fpr_double_index_) are even if the
    // next register is even.
    static_assert(!kQuickDoubleRegAlignedFloatBackFilled || kNumQuickFprArgs % 2 == 0,
                  "Number of Quick FPR arguments not even");
    DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), kRuntimePointerSize);
  }

  virtual ~QuickArgumentVisitor() {}

  virtual void Visit() = 0;

  Primitive::Type GetParamPrimitiveType() const {
    return cur_type_;
  }

  uint8_t* GetParamAddress() const {
    if (!kQuickSoftFloatAbi) {
      Primitive::Type type = GetParamPrimitiveType();
      if (UNLIKELY((type == Primitive::kPrimDouble) || (type == Primitive::kPrimFloat))) {
        if (type == Primitive::kPrimDouble && kQuickDoubleRegAlignedFloatBackFilled) {
          if (fpr_double_index_ + 2 < kNumQuickFprArgs + 1) {
            return fpr_args_ + (fpr_double_index_ * GetBytesPerFprSpillLocation(kRuntimeISA));
          }
        } else if (fpr_index_ + 1 < kNumQuickFprArgs + 1) {
          return fpr_args_ + (fpr_index_ * GetBytesPerFprSpillLocation(kRuntimeISA));
        }
        return stack_args_ + (stack_index_ * kBytesStackArgLocation);
      }
    }
    if (gpr_index_ < kNumQuickGprArgs) {
      return gpr_args_ + GprIndexToGprOffset(gpr_index_);
    }
    return stack_args_ + (stack_index_ * kBytesStackArgLocation);
  }

  bool IsSplitLongOrDouble() const {
    if ((GetBytesPerGprSpillLocation(kRuntimeISA) == 4) ||
        (GetBytesPerFprSpillLocation(kRuntimeISA) == 4)) {
      return is_split_long_or_double_;
    } else {
      return false;  // An optimization for when GPR and FPRs are 64bit.
    }
  }

  bool IsParamAReference() const {
    return GetParamPrimitiveType() == Primitive::kPrimNot;
  }

  bool IsParamALongOrDouble() const {
    Primitive::Type type = GetParamPrimitiveType();
    return type == Primitive::kPrimLong || type == Primitive::kPrimDouble;
  }

  uint64_t ReadSplitLongParam() const {
    // The splitted long is always available through the stack.
    return *reinterpret_cast<uint64_t*>(stack_args_
        + stack_index_ * kBytesStackArgLocation);
  }

  void IncGprIndex() {
    gpr_index_++;
    if (kGprFprLockstep) {
      fpr_index_++;
    }
  }

  void IncFprIndex() {
    fpr_index_++;
    if (kGprFprLockstep) {
      gpr_index_++;
    }
  }

  void VisitArguments() REQUIRES_SHARED(Locks::mutator_lock_) {
    // (a) 'stack_args_' should point to the first method's argument
    // (b) whatever the argument type it is, the 'stack_index_' should
    //     be moved forward along with every visiting.
    gpr_index_ = 0;
    fpr_index_ = 0;
    if (kQuickDoubleRegAlignedFloatBackFilled) {
      fpr_double_index_ = 0;
    }
    stack_index_ = 0;
    if (!is_static_) {  // Handle this.
      cur_type_ = Primitive::kPrimNot;
      is_split_long_or_double_ = false;
      Visit();
      stack_index_++;
      if (kNumQuickGprArgs > 0) {
        IncGprIndex();
      }
    }
    for (uint32_t shorty_index = 1; shorty_index < shorty_len_; ++shorty_index) {
      cur_type_ = Primitive::GetType(shorty_[shorty_index]);
      switch (cur_type_) {
        case Primitive::kPrimNot:
        case Primitive::kPrimBoolean:
        case Primitive::kPrimByte:
        case Primitive::kPrimChar:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          is_split_long_or_double_ = false;
          Visit();
          stack_index_++;
          if (gpr_index_ < kNumQuickGprArgs) {
            IncGprIndex();
          }
          break;
        case Primitive::kPrimFloat:
          is_split_long_or_double_ = false;
          Visit();
          stack_index_++;
          if (kQuickSoftFloatAbi) {
            if (gpr_index_ < kNumQuickGprArgs) {
              IncGprIndex();
            }
          } else {
            if (fpr_index_ + 1 < kNumQuickFprArgs + 1) {
              IncFprIndex();
              if (kQuickDoubleRegAlignedFloatBackFilled) {
                // Double should not overlap with float.
                // For example, if fpr_index_ = 3, fpr_double_index_ should be at least 4.
                fpr_double_index_ = std::max(fpr_double_index_, RoundUp(fpr_index_, 2));
                // Float should not overlap with double.
                if (fpr_index_ % 2 == 0) {
                  fpr_index_ = std::max(fpr_double_index_, fpr_index_);
                }
              } else if (kQuickSkipOddFpRegisters) {
                IncFprIndex();
              }
            }
          }
          break;
        case Primitive::kPrimDouble:
        case Primitive::kPrimLong:
          if (kQuickSoftFloatAbi || (cur_type_ == Primitive::kPrimLong)) {
            if (cur_type_ == Primitive::kPrimLong &&
                gpr_index_ == 0 &&
                kAlignPairRegister) {
              // Currently, this is only for ARM, where we align long parameters with
              // even-numbered registers by skipping R1 and using R2 instead.
              IncGprIndex();
            }
            is_split_long_or_double_ = (GetBytesPerGprSpillLocation(kRuntimeISA) == 4) &&
                ((gpr_index_ + 1) == kNumQuickGprArgs);
            if (!kSplitPairAcrossRegisterAndStack && is_split_long_or_double_) {
              // We don't want to split this. Pass over this register.
              gpr_index_++;
              is_split_long_or_double_ = false;
            }
            Visit();
            if (kBytesStackArgLocation == 4) {
              stack_index_+= 2;
            } else {
              CHECK_EQ(kBytesStackArgLocation, 8U);
              stack_index_++;
            }
            if (gpr_index_ < kNumQuickGprArgs) {
              IncGprIndex();
              if (GetBytesPerGprSpillLocation(kRuntimeISA) == 4) {
                if (gpr_index_ < kNumQuickGprArgs) {
                  IncGprIndex();
                }
              }
            }
          } else {
            is_split_long_or_double_ = (GetBytesPerFprSpillLocation(kRuntimeISA) == 4) &&
                ((fpr_index_ + 1) == kNumQuickFprArgs) && !kQuickDoubleRegAlignedFloatBackFilled;
            Visit();
            if (kBytesStackArgLocation == 4) {
              stack_index_+= 2;
            } else {
              CHECK_EQ(kBytesStackArgLocation, 8U);
              stack_index_++;
            }
            if (kQuickDoubleRegAlignedFloatBackFilled) {
              if (fpr_double_index_ + 2 < kNumQuickFprArgs + 1) {
                fpr_double_index_ += 2;
                // Float should not overlap with double.
                if (fpr_index_ % 2 == 0) {
                  fpr_index_ = std::max(fpr_double_index_, fpr_index_);
                }
              }
            } else if (fpr_index_ + 1 < kNumQuickFprArgs + 1) {
              IncFprIndex();
              if (GetBytesPerFprSpillLocation(kRuntimeISA) == 4) {
                if (fpr_index_ + 1 < kNumQuickFprArgs + 1) {
                  IncFprIndex();
                }
              }
            }
          }
          break;
        default:
          LOG(FATAL) << "Unexpected type: " << cur_type_ << " in " << shorty_;
      }
    }
  }

 protected:
  const bool is_static_;
  const char* const shorty_;
  const uint32_t shorty_len_;

 private:
  uint8_t* const gpr_args_;  // Address of GPR arguments in callee save frame.
  uint8_t* const fpr_args_;  // Address of FPR arguments in callee save frame.
  uint8_t* const stack_args_;  // Address of stack arguments in caller's frame.
  uint32_t gpr_index_;  // Index into spilled GPRs.
  // Index into spilled FPRs.
  // In case kQuickDoubleRegAlignedFloatBackFilled, it may index a hole while fpr_double_index_
  // holds a higher register number.
  uint32_t fpr_index_;
  // Index into spilled FPRs for aligned double.
  // Only used when kQuickDoubleRegAlignedFloatBackFilled. Next available double register indexed in
  // terms of singles, may be behind fpr_index.
  uint32_t fpr_double_index_;
  uint32_t stack_index_;  // Index into arguments on the stack.
  // The current type of argument during VisitArguments.
  Primitive::Type cur_type_;
  // Does a 64bit parameter straddle the register and stack arguments?
  bool is_split_long_or_double_;
};

// Returns the 'this' object of a proxy method. This function is only used by StackVisitor. It
// allows to use the QuickArgumentVisitor constants without moving all the code in its own module.
extern "C" mirror::Object* artQuickGetProxyThisObject(ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return QuickArgumentVisitor::GetProxyThisObjectReference(sp)->AsMirrorPtr();
}

// Visits arguments on the stack placing them into the shadow frame.
class BuildQuickShadowFrameVisitor final : public QuickArgumentVisitor {
 public:
  BuildQuickShadowFrameVisitor(ArtMethod** sp, bool is_static, const char* shorty,
                               uint32_t shorty_len, ShadowFrame* sf, size_t first_arg_reg) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), sf_(sf), cur_reg_(first_arg_reg) {}

  void Visit() REQUIRES_SHARED(Locks::mutator_lock_) override;

 private:
  ShadowFrame* const sf_;
  uint32_t cur_reg_;

  DISALLOW_COPY_AND_ASSIGN(BuildQuickShadowFrameVisitor);
};

void BuildQuickShadowFrameVisitor::Visit() {
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimLong:  // Fall-through.
    case Primitive::kPrimDouble:
      if (IsSplitLongOrDouble()) {
        sf_->SetVRegLong(cur_reg_, ReadSplitLongParam());
      } else {
        sf_->SetVRegLong(cur_reg_, *reinterpret_cast<jlong*>(GetParamAddress()));
      }
      ++cur_reg_;
      break;
    case Primitive::kPrimNot: {
        StackReference<mirror::Object>* stack_ref =
            reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
        sf_->SetVRegReference(cur_reg_, stack_ref->AsMirrorPtr());
      }
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
    case Primitive::kPrimFloat:
      sf_->SetVReg(cur_reg_, *reinterpret_cast<jint*>(GetParamAddress()));
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
  ++cur_reg_;
}

// Don't inline. See b/65159206.
NO_INLINE
static void HandleDeoptimization(JValue* result,
                                 ArtMethod* method,
                                 ShadowFrame* deopt_frame,
                                 ManagedStack* fragment)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Coming from partial-fragment deopt.
  Thread* self = Thread::Current();
  if (kIsDebugBuild) {
    // Consistency-check: are the methods as expected? We check that the last shadow frame
    // (the bottom of the call-stack) corresponds to the called method.
    ShadowFrame* linked = deopt_frame;
    while (linked->GetLink() != nullptr) {
      linked = linked->GetLink();
    }
    CHECK_EQ(method, linked->GetMethod()) << method->PrettyMethod() << " "
        << ArtMethod::PrettyMethod(linked->GetMethod());
  }

  if (VLOG_IS_ON(deopt)) {
    // Print out the stack to verify that it was a partial-fragment deopt.
    LOG(INFO) << "Continue-ing from deopt. Stack is:";
    QuickExceptionHandler::DumpFramesWithType(self, true);
  }

  ObjPtr<mirror::Throwable> pending_exception;
  bool from_code = false;
  DeoptimizationMethodType method_type;
  self->PopDeoptimizationContext(/* out */ result,
                                 /* out */ &pending_exception,
                                 /* out */ &from_code,
                                 /* out */ &method_type);

  // Push a transition back into managed code onto the linked list in thread.
  self->PushManagedStackFragment(fragment);

  // Ensure that the stack is still in order.
  if (kIsDebugBuild) {
    class EntireStackVisitor : public StackVisitor {
     public:
      explicit EntireStackVisitor(Thread* self_in) REQUIRES_SHARED(Locks::mutator_lock_)
          : StackVisitor(self_in, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames) {}

      bool VisitFrame() override REQUIRES_SHARED(Locks::mutator_lock_) {
        // Nothing to do here. In a debug build, ValidateFrame will do the work in the walking
        // logic. Just always say we want to continue.
        return true;
      }
    };
    EntireStackVisitor esv(self);
    esv.WalkStack();
  }

  // Restore the exception that was pending before deoptimization then interpret the
  // deoptimized frames.
  if (pending_exception != nullptr) {
    self->SetException(pending_exception);
  }
  interpreter::EnterInterpreterFromDeoptimize(self,
                                              deopt_frame,
                                              result,
                                              from_code,
                                              DeoptimizationMethodType::kDefault);
}

extern "C" uint64_t artQuickToInterpreterBridge(ArtMethod* method, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Ensure we don't get thread suspension until the object arguments are safely in the shadow
  // frame.
  ScopedQuickEntrypointChecks sqec(self);

  if (UNLIKELY(!method->IsInvokable())) {
    method->ThrowInvocationTimeError();
    return 0;
  }

  JValue tmp_value;
  ShadowFrame* deopt_frame = self->PopStackedShadowFrame(
      StackedShadowFrameType::kDeoptimizationShadowFrame, false);
  ManagedStack fragment;

  DCHECK(!method->IsNative()) << method->PrettyMethod();
  uint32_t shorty_len = 0;
  ArtMethod* non_proxy_method = method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
  DCHECK(non_proxy_method->GetCodeItem() != nullptr) << method->PrettyMethod();
  CodeItemDataAccessor accessor(non_proxy_method->DexInstructionData());
  const char* shorty = non_proxy_method->GetShorty(&shorty_len);

  JValue result;
  bool force_frame_pop = false;

  if (UNLIKELY(deopt_frame != nullptr)) {
    HandleDeoptimization(&result, method, deopt_frame, &fragment);
  } else {
    const char* old_cause = self->StartAssertNoThreadSuspension(
        "Building interpreter shadow frame");
    uint16_t num_regs = accessor.RegistersSize();
    // No last shadow coming from quick.
    ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
        CREATE_SHADOW_FRAME(num_regs, /* link= */ nullptr, method, /* dex_pc= */ 0);
    ShadowFrame* shadow_frame = shadow_frame_unique_ptr.get();
    size_t first_arg_reg = accessor.RegistersSize() - accessor.InsSize();
    BuildQuickShadowFrameVisitor shadow_frame_builder(sp, method->IsStatic(), shorty, shorty_len,
                                                      shadow_frame, first_arg_reg);
    shadow_frame_builder.VisitArguments();
    // Push a transition back into managed code onto the linked list in thread.
    self->PushManagedStackFragment(&fragment);
    self->PushShadowFrame(shadow_frame);
    self->EndAssertNoThreadSuspension(old_cause);

    if (NeedsClinitCheckBeforeCall(method)) {
      ObjPtr<mirror::Class> declaring_class = method->GetDeclaringClass();
      if (UNLIKELY(!declaring_class->IsVisiblyInitialized())) {
        // Ensure static method's class is initialized.
        StackHandleScope<1> hs(self);
        Handle<mirror::Class> h_class(hs.NewHandle(declaring_class));
        if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
          DCHECK(Thread::Current()->IsExceptionPending()) << method->PrettyMethod();
          self->PopManagedStackFragment(fragment);
          return 0;
        }
      }
    }

    result = interpreter::EnterInterpreterFromEntryPoint(self, accessor, shadow_frame);
    force_frame_pop = shadow_frame->GetForcePopFrame();
  }

  // Pop transition.
  self->PopManagedStackFragment(fragment);

  // Request a stack deoptimization if needed
  ArtMethod* caller = QuickArgumentVisitor::GetCallingMethod(sp);
  uintptr_t caller_pc = QuickArgumentVisitor::GetCallingPc(sp);
  // If caller_pc is the instrumentation exit stub, the stub will check to see if deoptimization
  // should be done and it knows the real return pc. NB If the upcall is null we don't need to do
  // anything. This can happen during shutdown or early startup.
  if (UNLIKELY(
          caller != nullptr &&
          caller_pc != reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc()) &&
          (self->IsForceInterpreter() || Dbg::IsForcedInterpreterNeededForUpcall(self, caller)))) {
    if (!Runtime::Current()->IsAsyncDeoptimizeable(caller_pc)) {
      LOG(WARNING) << "Got a deoptimization request on un-deoptimizable method "
                   << caller->PrettyMethod();
    } else {
      VLOG(deopt) << "Forcing deoptimization on return from method " << method->PrettyMethod()
                  << " to " << caller->PrettyMethod()
                  << (force_frame_pop ? " for frame-pop" : "");
      DCHECK(!force_frame_pop || result.GetJ() == 0) << "Force frame pop should have no result.";
      if (force_frame_pop && self->GetException() != nullptr) {
        LOG(WARNING) << "Suppressing exception for instruction-retry: "
                     << self->GetException()->Dump();
      }
      // Push the context of the deoptimization stack so we can restore the return value and the
      // exception before executing the deoptimized frames.
      self->PushDeoptimizationContext(
          result,
          shorty[0] == 'L' || shorty[0] == '[',  /* class or array */
          force_frame_pop ? nullptr : self->GetException(),
          /* from_code= */ false,
          DeoptimizationMethodType::kDefault);

      // Set special exception to cause deoptimization.
      self->SetException(Thread::GetDeoptimizationException());
    }
  }

  // No need to restore the args since the method has already been run by the interpreter.
  return result.GetJ();
}

// Visits arguments on the stack placing them into the args vector, Object* arguments are converted
// to jobjects.
class BuildQuickArgumentVisitor final : public QuickArgumentVisitor {
 public:
  BuildQuickArgumentVisitor(ArtMethod** sp, bool is_static, const char* shorty, uint32_t shorty_len,
                            ScopedObjectAccessUnchecked* soa, std::vector<jvalue>* args) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), soa_(soa), args_(args) {}

  void Visit() REQUIRES_SHARED(Locks::mutator_lock_) override;

 private:
  ScopedObjectAccessUnchecked* const soa_;
  std::vector<jvalue>* const args_;

  DISALLOW_COPY_AND_ASSIGN(BuildQuickArgumentVisitor);
};

void BuildQuickArgumentVisitor::Visit() {
  jvalue val;
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimNot: {
      StackReference<mirror::Object>* stack_ref =
          reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
      val.l = soa_->AddLocalReference<jobject>(stack_ref->AsMirrorPtr());
      break;
    }
    case Primitive::kPrimLong:  // Fall-through.
    case Primitive::kPrimDouble:
      if (IsSplitLongOrDouble()) {
        val.j = ReadSplitLongParam();
      } else {
        val.j = *reinterpret_cast<jlong*>(GetParamAddress());
      }
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
    case Primitive::kPrimFloat:
      val.i = *reinterpret_cast<jint*>(GetParamAddress());
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
  args_->push_back(val);
}

// Handler for invocation on proxy methods. On entry a frame will exist for the proxy object method
// which is responsible for recording callee save registers. We explicitly place into jobjects the
// incoming reference arguments (so they survive GC). We invoke the invocation handler, which is a
// field within the proxy object, which will box the primitive arguments and deal with error cases.
extern "C" uint64_t artQuickProxyInvokeHandler(
    ArtMethod* proxy_method, mirror::Object* receiver, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(proxy_method->IsProxyMethod()) << proxy_method->PrettyMethod();
  DCHECK(receiver->GetClass()->IsProxyClass()) << proxy_method->PrettyMethod();
  // Ensure we don't get thread suspension until the object arguments are safely in jobjects.
  const char* old_cause =
      self->StartAssertNoThreadSuspension("Adding to IRT proxy object arguments");
  // Register the top of the managed stack, making stack crawlable.
  DCHECK_EQ((*sp), proxy_method) << proxy_method->PrettyMethod();
  self->VerifyStack();
  // Start new JNI local reference state.
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  // Create local ref. copies of proxy method and the receiver.
  jobject rcvr_jobj = soa.AddLocalReference<jobject>(receiver);

  // Placing arguments into args vector and remove the receiver.
  ArtMethod* non_proxy_method = proxy_method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
  CHECK(!non_proxy_method->IsStatic()) << proxy_method->PrettyMethod() << " "
                                       << non_proxy_method->PrettyMethod();
  std::vector<jvalue> args;
  uint32_t shorty_len = 0;
  const char* shorty = non_proxy_method->GetShorty(&shorty_len);
  BuildQuickArgumentVisitor local_ref_visitor(
      sp, /* is_static= */ false, shorty, shorty_len, &soa, &args);

  local_ref_visitor.VisitArguments();
  DCHECK_GT(args.size(), 0U) << proxy_method->PrettyMethod();
  args.erase(args.begin());

  // Convert proxy method into expected interface method.
  ArtMethod* interface_method = proxy_method->FindOverriddenMethod(kRuntimePointerSize);
  DCHECK(interface_method != nullptr) << proxy_method->PrettyMethod();
  DCHECK(!interface_method->IsProxyMethod()) << interface_method->PrettyMethod();
  self->EndAssertNoThreadSuspension(old_cause);
  DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), kRuntimePointerSize);
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  ObjPtr<mirror::Method> interface_reflect_method =
      mirror::Method::CreateFromArtMethod<kRuntimePointerSize>(soa.Self(), interface_method);
  if (interface_reflect_method == nullptr) {
    soa.Self()->AssertPendingOOMException();
    return 0;
  }
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(interface_reflect_method);

  // All naked Object*s should now be in jobjects, so its safe to go into the main invoke code
  // that performs allocations or instrumentation events.
  instrumentation::Instrumentation* instr = Runtime::Current()->GetInstrumentation();
  if (instr->HasMethodEntryListeners()) {
    instr->MethodEnterEvent(soa.Self(), proxy_method);
    if (soa.Self()->IsExceptionPending()) {
      instr->MethodUnwindEvent(self,
                               soa.Decode<mirror::Object>(rcvr_jobj),
                               proxy_method,
                               0);
      return 0;
    }
  }
  JValue result = InvokeProxyInvocationHandler(soa, shorty, rcvr_jobj, interface_method_jobj, args);
  if (soa.Self()->IsExceptionPending()) {
    if (instr->HasMethodUnwindListeners()) {
      instr->MethodUnwindEvent(self,
                               soa.Decode<mirror::Object>(rcvr_jobj),
                               proxy_method,
                               0);
    }
  } else if (instr->HasMethodExitListeners()) {
    instr->MethodExitEvent(self,
                           proxy_method,
                           {},
                           result);
  }
  return result.GetJ();
}

// Visitor returning a reference argument at a given position in a Quick stack frame.
// NOTE: Only used for testing purposes.
class GetQuickReferenceArgumentAtVisitor final : public QuickArgumentVisitor {
 public:
  GetQuickReferenceArgumentAtVisitor(ArtMethod** sp,
                                     const char* shorty,
                                     uint32_t shorty_len,
                                     size_t arg_pos)
      : QuickArgumentVisitor(sp, /* is_static= */ false, shorty, shorty_len),
        cur_pos_(0u),
        arg_pos_(arg_pos),
        ref_arg_(nullptr) {
          CHECK_LT(arg_pos, shorty_len) << "Argument position greater than the number arguments";
        }

  void Visit() REQUIRES_SHARED(Locks::mutator_lock_) override {
    if (cur_pos_ == arg_pos_) {
      Primitive::Type type = GetParamPrimitiveType();
      CHECK_EQ(type, Primitive::kPrimNot) << "Argument at searched position is not a reference";
      ref_arg_ = reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
    }
    ++cur_pos_;
  }

  StackReference<mirror::Object>* GetReferenceArgument() {
    return ref_arg_;
  }

 private:
  // The position of the currently visited argument.
  size_t cur_pos_;
  // The position of the searched argument.
  const size_t arg_pos_;
  // The reference argument, if found.
  StackReference<mirror::Object>* ref_arg_;

  DISALLOW_COPY_AND_ASSIGN(GetQuickReferenceArgumentAtVisitor);
};

// Returning reference argument at position `arg_pos` in Quick stack frame at address `sp`.
// NOTE: Only used for testing purposes.
extern "C" StackReference<mirror::Object>* artQuickGetProxyReferenceArgumentAt(size_t arg_pos,
                                                                               ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* proxy_method = *sp;
  ArtMethod* non_proxy_method = proxy_method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
  CHECK(!non_proxy_method->IsStatic())
      << proxy_method->PrettyMethod() << " " << non_proxy_method->PrettyMethod();
  uint32_t shorty_len = 0;
  const char* shorty = non_proxy_method->GetShorty(&shorty_len);
  GetQuickReferenceArgumentAtVisitor ref_arg_visitor(sp, shorty, shorty_len, arg_pos);
  ref_arg_visitor.VisitArguments();
  StackReference<mirror::Object>* ref_arg = ref_arg_visitor.GetReferenceArgument();
  return ref_arg;
}

// Visitor returning all the reference arguments in a Quick stack frame.
class GetQuickReferenceArgumentsVisitor final : public QuickArgumentVisitor {
 public:
  GetQuickReferenceArgumentsVisitor(ArtMethod** sp,
                                    bool is_static,
                                    const char* shorty,
                                    uint32_t shorty_len)
      : QuickArgumentVisitor(sp, is_static, shorty, shorty_len) {}

  void Visit() REQUIRES_SHARED(Locks::mutator_lock_) override {
    Primitive::Type type = GetParamPrimitiveType();
    if (type == Primitive::kPrimNot) {
      StackReference<mirror::Object>* ref_arg =
          reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
      ref_args_.push_back(ref_arg);
    }
  }

  std::vector<StackReference<mirror::Object>*> GetReferenceArguments() {
    return ref_args_;
  }

 private:
  // The reference arguments.
  std::vector<StackReference<mirror::Object>*> ref_args_;

  DISALLOW_COPY_AND_ASSIGN(GetQuickReferenceArgumentsVisitor);
};

// Returning all reference arguments in Quick stack frame at address `sp`.
std::vector<StackReference<mirror::Object>*> GetProxyReferenceArguments(ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* proxy_method = *sp;
  ArtMethod* non_proxy_method = proxy_method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
  CHECK(!non_proxy_method->IsStatic())
      << proxy_method->PrettyMethod() << " " << non_proxy_method->PrettyMethod();
  uint32_t shorty_len = 0;
  const char* shorty = non_proxy_method->GetShorty(&shorty_len);
  GetQuickReferenceArgumentsVisitor ref_args_visitor(sp, /*is_static=*/ false, shorty, shorty_len);
  ref_args_visitor.VisitArguments();
  std::vector<StackReference<mirror::Object>*> ref_args = ref_args_visitor.GetReferenceArguments();
  return ref_args;
}

// Read object references held in arguments from quick frames and place in a JNI local references,
// so they don't get garbage collected.
class RememberForGcArgumentVisitor final : public QuickArgumentVisitor {
 public:
  RememberForGcArgumentVisitor(ArtMethod** sp, bool is_static, const char* shorty,
                               uint32_t shorty_len, ScopedObjectAccessUnchecked* soa) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), soa_(soa) {}

  void Visit() REQUIRES_SHARED(Locks::mutator_lock_) override;

  void FixupReferences() REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  ScopedObjectAccessUnchecked* const soa_;
  // References which we must update when exiting in case the GC moved the objects.
  std::vector<std::pair<jobject, StackReference<mirror::Object>*> > references_;

  DISALLOW_COPY_AND_ASSIGN(RememberForGcArgumentVisitor);
};

void RememberForGcArgumentVisitor::Visit() {
  if (IsParamAReference()) {
    StackReference<mirror::Object>* stack_ref =
        reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
    jobject reference =
        soa_->AddLocalReference<jobject>(stack_ref->AsMirrorPtr());
    references_.push_back(std::make_pair(reference, stack_ref));
  }
}

void RememberForGcArgumentVisitor::FixupReferences() {
  // Fixup any references which may have changed.
  for (const auto& pair : references_) {
    pair.second->Assign(soa_->Decode<mirror::Object>(pair.first));
    soa_->Env()->DeleteLocalRef(pair.first);
  }
}

extern "C" const void* artInstrumentationMethodEntryFromCode(ArtMethod* method,
                                                             mirror::Object* this_object,
                                                             Thread* self,
                                                             ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const void* result;
  // Instrumentation changes the stack. Thus, when exiting, the stack cannot be verified, so skip
  // that part.
  ScopedQuickEntrypointChecks sqec(self, kIsDebugBuild, false);
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  DCHECK(!method->IsProxyMethod())
      << "Proxy method " << method->PrettyMethod()
      << " (declaring class: " << method->GetDeclaringClass()->PrettyClass() << ")"
      << " should not hit instrumentation entrypoint.";
  if (instrumentation->IsDeoptimized(method)) {
    result = GetQuickToInterpreterBridge();
  } else {
    // This will get the entry point either from the oat file, the JIT or the appropriate bridge
    // method if none of those can be found.
    result = instrumentation->GetCodeForInvoke(method);
    jit::Jit* jit = Runtime::Current()->GetJit();
    DCHECK_NE(result, GetQuickInstrumentationEntryPoint()) << method->PrettyMethod();
    DCHECK(jit == nullptr ||
           // Native methods come through here in Interpreter entrypoints. We might not have
           // disabled jit-gc but that is fine since we won't return jit-code for native methods.
           method->IsNative() ||
           !jit->GetCodeCache()->GetGarbageCollectCode());
    DCHECK(!method->IsNative() ||
           jit == nullptr ||
           !jit->GetCodeCache()->ContainsPc(result))
        << method->PrettyMethod() << " code will jump to possibly cleaned up jit code!";
  }

  bool interpreter_entry = (result == GetQuickToInterpreterBridge());
  bool is_static = method->IsStatic();
  uint32_t shorty_len;
  const char* shorty =
      method->GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(&shorty_len);

  ScopedObjectAccessUnchecked soa(self);
  RememberForGcArgumentVisitor visitor(sp, is_static, shorty, shorty_len, &soa);
  visitor.VisitArguments();

  instrumentation->PushInstrumentationStackFrame(self,
                                                 is_static ? nullptr : this_object,
                                                 method,
                                                 reinterpret_cast<uintptr_t>(
                                                     QuickArgumentVisitor::GetCallingPcAddr(sp)),
                                                 QuickArgumentVisitor::GetCallingPc(sp),
                                                 interpreter_entry);

  visitor.FixupReferences();
  if (UNLIKELY(self->IsExceptionPending())) {
    return nullptr;
  }
  CHECK(result != nullptr) << method->PrettyMethod();
  return result;
}

extern "C" TwoWordReturn artInstrumentationMethodExitFromCode(Thread* self,
                                                              ArtMethod** sp,
                                                              uint64_t* gpr_result,
                                                              uint64_t* fpr_result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK_EQ(reinterpret_cast<uintptr_t>(self), reinterpret_cast<uintptr_t>(Thread::Current()));
  CHECK(gpr_result != nullptr);
  CHECK(fpr_result != nullptr);
  // Instrumentation exit stub must not be entered with a pending exception.
  CHECK(!self->IsExceptionPending()) << "Enter instrumentation exit stub with pending exception "
                                     << self->GetException()->Dump();
  // Compute address of return PC and check that it currently holds 0.
  constexpr size_t return_pc_offset =
      RuntimeCalleeSaveFrame::GetReturnPcOffset(CalleeSaveType::kSaveEverything);
  uintptr_t* return_pc_addr = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(sp) +
                                                           return_pc_offset);
  CHECK_EQ(*return_pc_addr, 0U);

  // Pop the frame filling in the return pc. The low half of the return value is 0 when
  // deoptimization shouldn't be performed with the high-half having the return address. When
  // deoptimization should be performed the low half is zero and the high-half the address of the
  // deoptimization entry point.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  TwoWordReturn return_or_deoptimize_pc = instrumentation->PopInstrumentationStackFrame(
      self, return_pc_addr, gpr_result, fpr_result);
  if (self->IsExceptionPending() || self->ObserveAsyncException()) {
    return GetTwoWordFailureValue();
  }
  return return_or_deoptimize_pc;
}

static std::string DumpInstruction(ArtMethod* method, uint32_t dex_pc)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (dex_pc == static_cast<uint32_t>(-1)) {
    CHECK(method == jni::DecodeArtMethod(WellKnownClasses::java_lang_String_charAt));
    return "<native>";
  } else {
    CodeItemInstructionAccessor accessor = method->DexInstructions();
    CHECK_LT(dex_pc, accessor.InsnsSizeInCodeUnits());
    return accessor.InstructionAt(dex_pc).DumpString(method->GetDexFile());
  }
}

static void DumpB74410240ClassData(ObjPtr<mirror::Class> klass)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string storage;
  const char* descriptor = klass->GetDescriptor(&storage);
  LOG(FATAL_WITHOUT_ABORT) << "  " << DescribeLoaders(klass->GetClassLoader(), descriptor);
  const OatDexFile* oat_dex_file = klass->GetDexFile().GetOatDexFile();
  if (oat_dex_file != nullptr) {
    const OatFile* oat_file = oat_dex_file->GetOatFile();
    const char* dex2oat_cmdline =
        oat_file->GetOatHeader().GetStoreValueByKey(OatHeader::kDex2OatCmdLineKey);
    LOG(FATAL_WITHOUT_ABORT) << "    OatFile: " << oat_file->GetLocation()
        << "; " << (dex2oat_cmdline != nullptr ? dex2oat_cmdline : "<not recorded>");
  }
}

static void DumpB74410240DebugData(ArtMethod** sp) REQUIRES_SHARED(Locks::mutator_lock_) {
  // Mimick the search for the caller and dump some data while doing so.
  LOG(FATAL_WITHOUT_ABORT) << "Dumping debugging data, please attach a bugreport to b/74410240.";

  constexpr CalleeSaveType type = CalleeSaveType::kSaveRefsAndArgs;
  CHECK_EQ(*sp, Runtime::Current()->GetCalleeSaveMethod(type));

  constexpr size_t callee_frame_size = RuntimeCalleeSaveFrame::GetFrameSize(type);
  auto** caller_sp = reinterpret_cast<ArtMethod**>(
      reinterpret_cast<uintptr_t>(sp) + callee_frame_size);
  constexpr size_t callee_return_pc_offset = RuntimeCalleeSaveFrame::GetReturnPcOffset(type);
  uintptr_t caller_pc = *reinterpret_cast<uintptr_t*>(
      (reinterpret_cast<uint8_t*>(sp) + callee_return_pc_offset));
  ArtMethod* outer_method = *caller_sp;

  if (UNLIKELY(caller_pc == reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc()))) {
    LOG(FATAL_WITHOUT_ABORT) << "Method: " << outer_method->PrettyMethod()
        << " native pc: " << caller_pc << " Instrumented!";
    return;
  }

  const OatQuickMethodHeader* current_code = outer_method->GetOatQuickMethodHeader(caller_pc);
  CHECK(current_code != nullptr);
  CHECK(current_code->IsOptimized());
  uintptr_t native_pc_offset = current_code->NativeQuickPcOffset(caller_pc);
  CodeInfo code_info(current_code);
  StackMap stack_map = code_info.GetStackMapForNativePcOffset(native_pc_offset);
  CHECK(stack_map.IsValid());
  uint32_t dex_pc = stack_map.GetDexPc();

  // Log the outer method and its associated dex file and class table pointer which can be used
  // to find out if the inlined methods were defined by other dex file(s) or class loader(s).
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  LOG(FATAL_WITHOUT_ABORT) << "Outer: " << outer_method->PrettyMethod()
      << " native pc: " << caller_pc
      << " dex pc: " << dex_pc
      << " dex file: " << outer_method->GetDexFile()->GetLocation()
      << " class table: " << class_linker->ClassTableForClassLoader(outer_method->GetClassLoader());
  DumpB74410240ClassData(outer_method->GetDeclaringClass());
  LOG(FATAL_WITHOUT_ABORT) << "  instruction: " << DumpInstruction(outer_method, dex_pc);

  ArtMethod* caller = outer_method;
  BitTableRange<InlineInfo> inline_infos = code_info.GetInlineInfosOf(stack_map);
  for (InlineInfo inline_info : inline_infos) {
    const char* tag = "";
    dex_pc = inline_info.GetDexPc();
    if (inline_info.EncodesArtMethod()) {
      tag = "encoded ";
      caller = inline_info.GetArtMethod();
    } else {
      uint32_t method_index = code_info.GetMethodIndexOf(inline_info);
      if (dex_pc == static_cast<uint32_t>(-1)) {
        tag = "special ";
        CHECK(inline_info.Equals(inline_infos.back()));
        caller = jni::DecodeArtMethod(WellKnownClasses::java_lang_String_charAt);
        CHECK_EQ(caller->GetDexMethodIndex(), method_index);
      } else {
        ObjPtr<mirror::DexCache> dex_cache = caller->GetDexCache();
        ObjPtr<mirror::ClassLoader> class_loader = caller->GetClassLoader();
        caller = class_linker->LookupResolvedMethod(method_index, dex_cache, class_loader);
        CHECK(caller != nullptr);
      }
    }
    LOG(FATAL_WITHOUT_ABORT) << "InlineInfo #" << inline_info.Row()
        << ": " << tag << caller->PrettyMethod()
        << " dex pc: " << dex_pc
        << " dex file: " << caller->GetDexFile()->GetLocation()
        << " class table: "
        << class_linker->ClassTableForClassLoader(caller->GetClassLoader());
    DumpB74410240ClassData(caller->GetDeclaringClass());
    LOG(FATAL_WITHOUT_ABORT) << "  instruction: " << DumpInstruction(caller, dex_pc);
  }
}

// Lazily resolve a method for quick. Called by stub code.
extern "C" const void* artQuickResolutionTrampoline(
    ArtMethod* called, mirror::Object* receiver, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // The resolution trampoline stashes the resolved method into the callee-save frame to transport
  // it. Thus, when exiting, the stack cannot be verified (as the resolved method most likely
  // does not have the same stack layout as the callee-save method).
  ScopedQuickEntrypointChecks sqec(self, kIsDebugBuild, false);
  // Start new JNI local reference state
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  const char* old_cause = self->StartAssertNoThreadSuspension("Quick method resolution set up");

  // Compute details about the called method (avoid GCs)
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  InvokeType invoke_type;
  MethodReference called_method(nullptr, 0);
  const bool called_method_known_on_entry = !called->IsRuntimeMethod();
  ArtMethod* caller = nullptr;
  if (!called_method_known_on_entry) {
    caller = QuickArgumentVisitor::GetCallingMethod(sp);
    called_method.dex_file = caller->GetDexFile();

    {
      uint32_t dex_pc = QuickArgumentVisitor::GetCallingDexPc(sp);
      CodeItemInstructionAccessor accessor(caller->DexInstructions());
      CHECK_LT(dex_pc, accessor.InsnsSizeInCodeUnits());
      const Instruction& instr = accessor.InstructionAt(dex_pc);
      Instruction::Code instr_code = instr.Opcode();
      bool is_range;
      switch (instr_code) {
        case Instruction::INVOKE_DIRECT:
          invoke_type = kDirect;
          is_range = false;
          break;
        case Instruction::INVOKE_DIRECT_RANGE:
          invoke_type = kDirect;
          is_range = true;
          break;
        case Instruction::INVOKE_STATIC:
          invoke_type = kStatic;
          is_range = false;
          break;
        case Instruction::INVOKE_STATIC_RANGE:
          invoke_type = kStatic;
          is_range = true;
          break;
        case Instruction::INVOKE_SUPER:
          invoke_type = kSuper;
          is_range = false;
          break;
        case Instruction::INVOKE_SUPER_RANGE:
          invoke_type = kSuper;
          is_range = true;
          break;
        case Instruction::INVOKE_VIRTUAL:
          invoke_type = kVirtual;
          is_range = false;
          break;
        case Instruction::INVOKE_VIRTUAL_RANGE:
          invoke_type = kVirtual;
          is_range = true;
          break;
        case Instruction::INVOKE_INTERFACE:
          invoke_type = kInterface;
          is_range = false;
          break;
        case Instruction::INVOKE_INTERFACE_RANGE:
          invoke_type = kInterface;
          is_range = true;
          break;
        default:
          DumpB74410240DebugData(sp);
          LOG(FATAL) << "Unexpected call into trampoline: " << instr.DumpString(nullptr);
          UNREACHABLE();
      }
      called_method.index = (is_range) ? instr.VRegB_3rc() : instr.VRegB_35c();
      VLOG(dex) << "Accessed dex file for invoke " << invoke_type << " "
                << called_method.index;
    }
  } else {
    invoke_type = kStatic;
    called_method.dex_file = called->GetDexFile();
    called_method.index = called->GetDexMethodIndex();
  }
  uint32_t shorty_len;
  const char* shorty =
      called_method.dex_file->GetMethodShorty(called_method.GetMethodId(), &shorty_len);
  RememberForGcArgumentVisitor visitor(sp, invoke_type == kStatic, shorty, shorty_len, &soa);
  visitor.VisitArguments();
  self->EndAssertNoThreadSuspension(old_cause);
  const bool virtual_or_interface = invoke_type == kVirtual || invoke_type == kInterface;
  // Resolve method filling in dex cache.
  if (!called_method_known_on_entry) {
    StackHandleScope<1> hs(self);
    mirror::Object* fake_receiver = nullptr;
    HandleWrapper<mirror::Object> h_receiver(
        hs.NewHandleWrapper(virtual_or_interface ? &receiver : &fake_receiver));
    DCHECK_EQ(caller->GetDexFile(), called_method.dex_file);
    called = linker->ResolveMethod<ClassLinker::ResolveMode::kCheckICCEAndIAE>(
        self, called_method.index, caller, invoke_type);
  }
  const void* code = nullptr;
  if (LIKELY(!self->IsExceptionPending())) {
    // Incompatible class change should have been handled in resolve method.
    CHECK(!called->CheckIncompatibleClassChange(invoke_type))
        << called->PrettyMethod() << " " << invoke_type;
    if (virtual_or_interface || invoke_type == kSuper) {
      // Refine called method based on receiver for kVirtual/kInterface, and
      // caller for kSuper.
      ArtMethod* orig_called = called;
      if (invoke_type == kVirtual) {
        CHECK(receiver != nullptr) << invoke_type;
        called = receiver->GetClass()->FindVirtualMethodForVirtual(called, kRuntimePointerSize);
      } else if (invoke_type == kInterface) {
        CHECK(receiver != nullptr) << invoke_type;
        called = receiver->GetClass()->FindVirtualMethodForInterface(called, kRuntimePointerSize);
      } else {
        DCHECK_EQ(invoke_type, kSuper);
        CHECK(caller != nullptr) << invoke_type;
        ObjPtr<mirror::Class> ref_class = linker->LookupResolvedType(
            caller->GetDexFile()->GetMethodId(called_method.index).class_idx_, caller);
        if (ref_class->IsInterface()) {
          called = ref_class->FindVirtualMethodForInterfaceSuper(called, kRuntimePointerSize);
        } else {
          called = caller->GetDeclaringClass()->GetSuperClass()->GetVTableEntry(
              called->GetMethodIndex(), kRuntimePointerSize);
        }
      }

      CHECK(called != nullptr) << orig_called->PrettyMethod() << " "
                               << mirror::Object::PrettyTypeOf(receiver) << " "
                               << invoke_type << " " << orig_called->GetVtableIndex();
    }
    // Now that we know the actual target, update .bss entry in oat file, if
    // any.
    if (!called_method_known_on_entry) {
      // We only put non copied methods in the BSS. Putting a copy can lead to an
      // odd situation where the ArtMethod being executed is unrelated to the
      // receiver of the method.
      called = called->GetCanonicalMethod();
      if (invoke_type == kSuper || invoke_type == kInterface || invoke_type == kVirtual) {
        if (called->GetDexFile() == called_method.dex_file) {
          called_method.index = called->GetDexMethodIndex();
        } else {
          called_method.index = called->FindDexMethodIndexInOtherDexFile(
              *called_method.dex_file, called_method.index);
          DCHECK_NE(called_method.index, dex::kDexNoIndex);
        }
      }
      MaybeUpdateBssMethodEntry(called, called_method);
    }

    // Static invokes need class initialization check but instance invokes can proceed even if
    // the class is erroneous, i.e. in the edge case of escaping instances of erroneous classes.
    bool success = true;
    ObjPtr<mirror::Class> called_class = called->GetDeclaringClass();
    if (NeedsClinitCheckBeforeCall(called) && !called_class->IsVisiblyInitialized()) {
      // Ensure that the called method's class is initialized.
      StackHandleScope<1> hs(soa.Self());
      HandleWrapperObjPtr<mirror::Class> h_called_class(hs.NewHandleWrapper(&called_class));
      success = linker->EnsureInitialized(soa.Self(), h_called_class, true, true);
    }
    if (success) {
      code = called->GetEntryPointFromQuickCompiledCode();
      if (linker->IsQuickResolutionStub(code)) {
        DCHECK_EQ(invoke_type, kStatic);
        // Go to JIT or oat and grab code.
        code = linker->GetQuickOatCodeFor(called);
      }
      if (linker->ShouldUseInterpreterEntrypoint(called, code)) {
        code = GetQuickToInterpreterBridge();
      }
    } else {
      DCHECK(called_class->IsErroneous());
      DCHECK(self->IsExceptionPending());
    }
  }
  CHECK_EQ(code == nullptr, self->IsExceptionPending());
  // Fixup any locally saved objects may have moved during a GC.
  visitor.FixupReferences();
  // Place called method in callee-save frame to be placed as first argument to quick method.
  *sp = called;

  return code;
}

/*
 * This class uses a couple of observations to unite the different calling conventions through
 * a few constants.
 *
 * 1) Number of registers used for passing is normally even, so counting down has no penalty for
 *    possible alignment.
 * 2) Known 64b architectures store 8B units on the stack, both for integral and floating point
 *    types, so using uintptr_t is OK. Also means that we can use kRegistersNeededX to denote
 *    when we have to split things
 * 3) The only soft-float, Arm, is 32b, so no widening needs to be taken into account for floats
 *    and we can use Int handling directly.
 * 4) Only 64b architectures widen, and their stack is aligned 8B anyways, so no padding code
 *    necessary when widening. Also, widening of Ints will take place implicitly, and the
 *    extension should be compatible with Aarch64, which mandates copying the available bits
 *    into LSB and leaving the rest unspecified.
 * 5) Aligning longs and doubles is necessary on arm only, and it's the same in registers and on
 *    the stack.
 * 6) There is only little endian.
 *
 *
 * Actual work is supposed to be done in a delegate of the template type. The interface is as
 * follows:
 *
 * void PushGpr(uintptr_t):   Add a value for the next GPR
 *
 * void PushFpr4(float):      Add a value for the next FPR of size 32b. Is only called if we need
 *                            padding, that is, think the architecture is 32b and aligns 64b.
 *
 * void PushFpr8(uint64_t):   Push a double. We _will_ call this on 32b, it's the callee's job to
 *                            split this if necessary. The current state will have aligned, if
 *                            necessary.
 *
 * void PushStack(uintptr_t): Push a value to the stack.
 */
template<class T> class BuildNativeCallFrameStateMachine {
 public:
#if defined(__arm__)
  static constexpr bool kNativeSoftFloatAbi = true;
  static constexpr size_t kNumNativeGprArgs = 4;  // 4 arguments passed in GPRs, r0-r3
  static constexpr size_t kNumNativeFprArgs = 0;  // 0 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 2;
  static constexpr size_t kRegistersNeededForDouble = 2;
  static constexpr bool kMultiRegistersAligned = true;
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = true;
  static constexpr bool kAlignDoubleOnStack = true;
#elif defined(__aarch64__)
  static constexpr bool kNativeSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumNativeGprArgs = 8;  // 8 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 8;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 1;
  static constexpr size_t kRegistersNeededForDouble = 1;
  static constexpr bool kMultiRegistersAligned = false;
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#elif defined(__i386__)
  static constexpr bool kNativeSoftFloatAbi = false;  // Not using int registers for fp
  static constexpr size_t kNumNativeGprArgs = 0;  // 0 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 0;  // 0 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 2;
  static constexpr size_t kRegistersNeededForDouble = 2;
  static constexpr bool kMultiRegistersAligned = false;  // x86 not using regs, anyways
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#elif defined(__x86_64__)
  static constexpr bool kNativeSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumNativeGprArgs = 6;  // 6 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 8;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 1;
  static constexpr size_t kRegistersNeededForDouble = 1;
  static constexpr bool kMultiRegistersAligned = false;
  static constexpr bool kMultiFPRegistersWidened = false;
  static constexpr bool kMultiGPRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#else
#error "Unsupported architecture"
#endif

 public:
  explicit BuildNativeCallFrameStateMachine(T* delegate)
      : gpr_index_(kNumNativeGprArgs),
        fpr_index_(kNumNativeFprArgs),
        stack_entries_(0),
        delegate_(delegate) {
    // For register alignment, we want to assume that counters (gpr_index_, fpr_index_) are even iff
    // the next register is even; counting down is just to make the compiler happy...
    static_assert(kNumNativeGprArgs % 2 == 0U, "Number of native GPR arguments not even");
    static_assert(kNumNativeFprArgs % 2 == 0U, "Number of native FPR arguments not even");
  }

  virtual ~BuildNativeCallFrameStateMachine() {}

  bool HavePointerGpr() const {
    return gpr_index_ > 0;
  }

  void AdvancePointer(const void* val) {
    if (HavePointerGpr()) {
      gpr_index_--;
      PushGpr(reinterpret_cast<uintptr_t>(val));
    } else {
      stack_entries_++;  // TODO: have a field for pointer length as multiple of 32b
      PushStack(reinterpret_cast<uintptr_t>(val));
      gpr_index_ = 0;
    }
  }

  bool HaveIntGpr() const {
    return gpr_index_ > 0;
  }

  void AdvanceInt(uint32_t val) {
    if (HaveIntGpr()) {
      gpr_index_--;
      if (kMultiGPRegistersWidened) {
        DCHECK_EQ(sizeof(uintptr_t), sizeof(int64_t));
        PushGpr(static_cast<int64_t>(bit_cast<int32_t, uint32_t>(val)));
      } else {
        PushGpr(val);
      }
    } else {
      stack_entries_++;
      if (kMultiGPRegistersWidened) {
        DCHECK_EQ(sizeof(uintptr_t), sizeof(int64_t));
        PushStack(static_cast<int64_t>(bit_cast<int32_t, uint32_t>(val)));
      } else {
        PushStack(val);
      }
      gpr_index_ = 0;
    }
  }

  bool HaveLongGpr() const {
    return gpr_index_ >= kRegistersNeededForLong + (LongGprNeedsPadding() ? 1 : 0);
  }

  bool LongGprNeedsPadding() const {
    return kRegistersNeededForLong > 1 &&     // only pad when using multiple registers
        kAlignLongOnStack &&                  // and when it needs alignment
        (gpr_index_ & 1) == 1;                // counter is odd, see constructor
  }

  bool LongStackNeedsPadding() const {
    return kRegistersNeededForLong > 1 &&     // only pad when using multiple registers
        kAlignLongOnStack &&                  // and when it needs 8B alignment
        (stack_entries_ & 1) == 1;            // counter is odd
  }

  void AdvanceLong(uint64_t val) {
    if (HaveLongGpr()) {
      if (LongGprNeedsPadding()) {
        PushGpr(0);
        gpr_index_--;
      }
      if (kRegistersNeededForLong == 1) {
        PushGpr(static_cast<uintptr_t>(val));
      } else {
        PushGpr(static_cast<uintptr_t>(val & 0xFFFFFFFF));
        PushGpr(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
      }
      gpr_index_ -= kRegistersNeededForLong;
    } else {
      if (LongStackNeedsPadding()) {
        PushStack(0);
        stack_entries_++;
      }
      if (kRegistersNeededForLong == 1) {
        PushStack(static_cast<uintptr_t>(val));
        stack_entries_++;
      } else {
        PushStack(static_cast<uintptr_t>(val & 0xFFFFFFFF));
        PushStack(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
        stack_entries_ += 2;
      }
      gpr_index_ = 0;
    }
  }

  bool HaveFloatFpr() const {
    return fpr_index_ > 0;
  }

  void AdvanceFloat(float val) {
    if (kNativeSoftFloatAbi) {
      AdvanceInt(bit_cast<uint32_t, float>(val));
    } else {
      if (HaveFloatFpr()) {
        fpr_index_--;
        if (kRegistersNeededForDouble == 1) {
          if (kMultiFPRegistersWidened) {
            PushFpr8(bit_cast<uint64_t, double>(val));
          } else {
            // No widening, just use the bits.
            PushFpr8(static_cast<uint64_t>(bit_cast<uint32_t, float>(val)));
          }
        } else {
          PushFpr4(val);
        }
      } else {
        stack_entries_++;
        if (kRegistersNeededForDouble == 1 && kMultiFPRegistersWidened) {
          // Need to widen before storing: Note the "double" in the template instantiation.
          // Note: We need to jump through those hoops to make the compiler happy.
          DCHECK_EQ(sizeof(uintptr_t), sizeof(uint64_t));
          PushStack(static_cast<uintptr_t>(bit_cast<uint64_t, double>(val)));
        } else {
          PushStack(static_cast<uintptr_t>(bit_cast<uint32_t, float>(val)));
        }
        fpr_index_ = 0;
      }
    }
  }

  bool HaveDoubleFpr() const {
    return fpr_index_ >= kRegistersNeededForDouble + (DoubleFprNeedsPadding() ? 1 : 0);
  }

  bool DoubleFprNeedsPadding() const {
    return kRegistersNeededForDouble > 1 &&     // only pad when using multiple registers
        kAlignDoubleOnStack &&                  // and when it needs alignment
        (fpr_index_ & 1) == 1;                  // counter is odd, see constructor
  }

  bool DoubleStackNeedsPadding() const {
    return kRegistersNeededForDouble > 1 &&     // only pad when using multiple registers
        kAlignDoubleOnStack &&                  // and when it needs 8B alignment
        (stack_entries_ & 1) == 1;              // counter is odd
  }

  void AdvanceDouble(uint64_t val) {
    if (kNativeSoftFloatAbi) {
      AdvanceLong(val);
    } else {
      if (HaveDoubleFpr()) {
        if (DoubleFprNeedsPadding()) {
          PushFpr4(0);
          fpr_index_--;
        }
        PushFpr8(val);
        fpr_index_ -= kRegistersNeededForDouble;
      } else {
        if (DoubleStackNeedsPadding()) {
          PushStack(0);
          stack_entries_++;
        }
        if (kRegistersNeededForDouble == 1) {
          PushStack(static_cast<uintptr_t>(val));
          stack_entries_++;
        } else {
          PushStack(static_cast<uintptr_t>(val & 0xFFFFFFFF));
          PushStack(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
          stack_entries_ += 2;
        }
        fpr_index_ = 0;
      }
    }
  }

  uint32_t GetStackEntries() const {
    return stack_entries_;
  }

  uint32_t GetNumberOfUsedGprs() const {
    return kNumNativeGprArgs - gpr_index_;
  }

  uint32_t GetNumberOfUsedFprs() const {
    return kNumNativeFprArgs - fpr_index_;
  }

 private:
  void PushGpr(uintptr_t val) {
    delegate_->PushGpr(val);
  }
  void PushFpr4(float val) {
    delegate_->PushFpr4(val);
  }
  void PushFpr8(uint64_t val) {
    delegate_->PushFpr8(val);
  }
  void PushStack(uintptr_t val) {
    delegate_->PushStack(val);
  }

  uint32_t gpr_index_;      // Number of free GPRs
  uint32_t fpr_index_;      // Number of free FPRs
  uint32_t stack_entries_;  // Stack entries are in multiples of 32b, as floats are usually not
                            // extended
  T* const delegate_;             // What Push implementation gets called
};

// Computes the sizes of register stacks and call stack area. Handling of references can be extended
// in subclasses.
//
// To handle native pointers, use "L" in the shorty for an object reference, which simulates
// them with handles.
class ComputeNativeCallFrameSize {
 public:
  ComputeNativeCallFrameSize() : num_stack_entries_(0) {}

  virtual ~ComputeNativeCallFrameSize() {}

  uint32_t GetStackSize() const {
    return num_stack_entries_ * sizeof(uintptr_t);
  }

  uint8_t* LayoutStackArgs(uint8_t* sp8) const {
    sp8 -= GetStackSize();
    // Align by kStackAlignment; it is at least as strict as native stack alignment.
    sp8 = reinterpret_cast<uint8_t*>(RoundDown(reinterpret_cast<uintptr_t>(sp8), kStackAlignment));
    return sp8;
  }

  virtual void WalkHeader(
      BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm ATTRIBUTE_UNUSED)
      REQUIRES_SHARED(Locks::mutator_lock_) {
  }

  void Walk(const char* shorty, uint32_t shorty_len) REQUIRES_SHARED(Locks::mutator_lock_) {
    BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize> sm(this);

    WalkHeader(&sm);

    for (uint32_t i = 1; i < shorty_len; ++i) {
      Primitive::Type cur_type_ = Primitive::GetType(shorty[i]);
      switch (cur_type_) {
        case Primitive::kPrimNot:
          sm.AdvancePointer(nullptr);
          break;
        case Primitive::kPrimBoolean:
        case Primitive::kPrimByte:
        case Primitive::kPrimChar:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          sm.AdvanceInt(0);
          break;
        case Primitive::kPrimFloat:
          sm.AdvanceFloat(0);
          break;
        case Primitive::kPrimDouble:
          sm.AdvanceDouble(0);
          break;
        case Primitive::kPrimLong:
          sm.AdvanceLong(0);
          break;
        default:
          LOG(FATAL) << "Unexpected type: " << cur_type_ << " in " << shorty;
          UNREACHABLE();
      }
    }

    num_stack_entries_ = sm.GetStackEntries();
  }

  void PushGpr(uintptr_t /* val */) {
    // not optimizing registers, yet
  }

  void PushFpr4(float /* val */) {
    // not optimizing registers, yet
  }

  void PushFpr8(uint64_t /* val */) {
    // not optimizing registers, yet
  }

  void PushStack(uintptr_t /* val */) {
    // counting is already done in the superclass
  }

 protected:
  uint32_t num_stack_entries_;
};

class ComputeGenericJniFrameSize final : public ComputeNativeCallFrameSize {
 public:
  explicit ComputeGenericJniFrameSize(bool critical_native)
    : critical_native_(critical_native) {}

  uintptr_t* ComputeLayout(ArtMethod** managed_sp, const char* shorty, uint32_t shorty_len)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), kRuntimePointerSize);

    Walk(shorty, shorty_len);

    // Add space for cookie.
    DCHECK_ALIGNED(managed_sp, sizeof(uintptr_t));
    static_assert(sizeof(uintptr_t) >= sizeof(IRTSegmentState));
    uint8_t* sp8 = reinterpret_cast<uint8_t*>(managed_sp) - sizeof(uintptr_t);

    // Layout stack arguments.
    sp8 = LayoutStackArgs(sp8);

    // Return the new bottom.
    DCHECK_ALIGNED(sp8, sizeof(uintptr_t));
    return reinterpret_cast<uintptr_t*>(sp8);
  }

  static uintptr_t* GetStartGprRegs(uintptr_t* reserved_area) {
    return reserved_area;
  }

  static uint32_t* GetStartFprRegs(uintptr_t* reserved_area) {
    constexpr size_t num_gprs =
        BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>::kNumNativeGprArgs;
    return reinterpret_cast<uint32_t*>(GetStartGprRegs(reserved_area) + num_gprs);
  }

  static uintptr_t* GetHiddenArgSlot(uintptr_t* reserved_area) {
    // Note: `num_fprs` is 0 on architectures where sizeof(uintptr_t) does not match the
    // FP register size (it is actually 0 on all supported 32-bit architectures).
    constexpr size_t num_fprs =
        BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>::kNumNativeFprArgs;
    return reinterpret_cast<uintptr_t*>(GetStartFprRegs(reserved_area)) + num_fprs;
  }

  static uintptr_t* GetOutArgsSpSlot(uintptr_t* reserved_area) {
    return GetHiddenArgSlot(reserved_area) + 1;
  }

  // Add JNIEnv* and jobj/jclass before the shorty-derived elements.
  void WalkHeader(BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm) override
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  const bool critical_native_;
};

void ComputeGenericJniFrameSize::WalkHeader(
    BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm) {
  // First 2 parameters are always excluded for @CriticalNative.
  if (UNLIKELY(critical_native_)) {
    return;
  }

  // JNIEnv
  sm->AdvancePointer(nullptr);

  // Class object or this as first argument
  sm->AdvancePointer(nullptr);
}

// Class to push values to three separate regions. Used to fill the native call part. Adheres to
// the template requirements of BuildGenericJniFrameStateMachine.
class FillNativeCall {
 public:
  FillNativeCall(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args) :
      cur_gpr_reg_(gpr_regs), cur_fpr_reg_(fpr_regs), cur_stack_arg_(stack_args) {}

  virtual ~FillNativeCall() {}

  void Reset(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args) {
    cur_gpr_reg_ = gpr_regs;
    cur_fpr_reg_ = fpr_regs;
    cur_stack_arg_ = stack_args;
  }

  void PushGpr(uintptr_t val) {
    *cur_gpr_reg_ = val;
    cur_gpr_reg_++;
  }

  void PushFpr4(float val) {
    *cur_fpr_reg_ = val;
    cur_fpr_reg_++;
  }

  void PushFpr8(uint64_t val) {
    uint64_t* tmp = reinterpret_cast<uint64_t*>(cur_fpr_reg_);
    *tmp = val;
    cur_fpr_reg_ += 2;
  }

  void PushStack(uintptr_t val) {
    *cur_stack_arg_ = val;
    cur_stack_arg_++;
  }

 private:
  uintptr_t* cur_gpr_reg_;
  uint32_t* cur_fpr_reg_;
  uintptr_t* cur_stack_arg_;
};

// Visits arguments on the stack placing them into a region lower down the stack for the benefit
// of transitioning into native code.
class BuildGenericJniFrameVisitor final : public QuickArgumentVisitor {
 public:
  BuildGenericJniFrameVisitor(Thread* self,
                              bool is_static,
                              bool critical_native,
                              const char* shorty,
                              uint32_t shorty_len,
                              ArtMethod** managed_sp,
                              uintptr_t* reserved_area)
     : QuickArgumentVisitor(managed_sp, is_static, shorty, shorty_len),
       jni_call_(nullptr, nullptr, nullptr, critical_native),
       sm_(&jni_call_),
       current_vreg_(nullptr) {
    DCHECK_ALIGNED(managed_sp, kStackAlignment);
    DCHECK_ALIGNED(reserved_area, sizeof(uintptr_t));

    ComputeGenericJniFrameSize fsc(critical_native);
    uintptr_t* out_args_sp = fsc.ComputeLayout(managed_sp, shorty, shorty_len);

    // Store hidden argument for @CriticalNative.
    uintptr_t* hidden_arg_slot = fsc.GetHiddenArgSlot(reserved_area);
    constexpr uintptr_t kGenericJniTag = 1u;
    ArtMethod* method = *managed_sp;
    *hidden_arg_slot = critical_native ? (reinterpret_cast<uintptr_t>(method) | kGenericJniTag)
                                       : 0xebad6a89u;  // Bad value.

    // Set out args SP.
    uintptr_t* out_args_sp_slot = fsc.GetOutArgsSpSlot(reserved_area);
    *out_args_sp_slot = reinterpret_cast<uintptr_t>(out_args_sp);

    // Prepare vreg pointer for spilling references.
    static constexpr size_t frame_size =
        RuntimeCalleeSaveFrame::GetFrameSize(CalleeSaveType::kSaveRefsAndArgs);
    current_vreg_ = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(managed_sp) + frame_size + sizeof(ArtMethod*));

    jni_call_.Reset(fsc.GetStartGprRegs(reserved_area),
                    fsc.GetStartFprRegs(reserved_area),
                    out_args_sp);

    // First 2 parameters are always excluded for CriticalNative methods.
    if (LIKELY(!critical_native)) {
      // jni environment is always first argument
      sm_.AdvancePointer(self->GetJniEnv());

      if (is_static) {
        // The `jclass` is a pointer to the method's declaring class.
        // The declaring class must be marked.
        auto* declaring_class = reinterpret_cast<mirror::CompressedReference<mirror::Class>*>(
            method->GetDeclaringClassAddressWithoutBarrier());
        if (kUseReadBarrier) {
          ReadBarrierJni(declaring_class, self);
        }
        sm_.AdvancePointer(declaring_class);
      }  // else "this" reference is already handled by QuickArgumentVisitor.
    }
  }

  void Visit() REQUIRES_SHARED(Locks::mutator_lock_) override;

 private:
  // A class to fill a JNI call. Adds reference/handle-scope management to FillNativeCall.
  class FillJniCall final : public FillNativeCall {
   public:
    FillJniCall(uintptr_t* gpr_regs,
                uint32_t* fpr_regs,
                uintptr_t* stack_args,
                bool critical_native)
        : FillNativeCall(gpr_regs, fpr_regs, stack_args),
          cur_entry_(0),
          critical_native_(critical_native) {}

    void Reset(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args) {
      FillNativeCall::Reset(gpr_regs, fpr_regs, stack_args);
      cur_entry_ = 0U;
    }

    bool CriticalNative() const {
      return critical_native_;
    }

   private:
    size_t cur_entry_;
    const bool critical_native_;
  };

  FillJniCall jni_call_;
  BuildNativeCallFrameStateMachine<FillJniCall> sm_;

  // Pointer to the current vreg in caller's reserved out vreg area.
  // Used for spilling reference arguments.
  uint32_t* current_vreg_;

  DISALLOW_COPY_AND_ASSIGN(BuildGenericJniFrameVisitor);
};

void BuildGenericJniFrameVisitor::Visit() {
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimLong: {
      jlong long_arg;
      if (IsSplitLongOrDouble()) {
        long_arg = ReadSplitLongParam();
      } else {
        long_arg = *reinterpret_cast<jlong*>(GetParamAddress());
      }
      sm_.AdvanceLong(long_arg);
      current_vreg_ += 2u;
      break;
    }
    case Primitive::kPrimDouble: {
      uint64_t double_arg;
      if (IsSplitLongOrDouble()) {
        // Read into union so that we don't case to a double.
        double_arg = ReadSplitLongParam();
      } else {
        double_arg = *reinterpret_cast<uint64_t*>(GetParamAddress());
      }
      sm_.AdvanceDouble(double_arg);
      current_vreg_ += 2u;
      break;
    }
    case Primitive::kPrimNot: {
      mirror::Object* obj =
          reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress())->AsMirrorPtr();
      StackReference<mirror::Object>* spill_ref =
          reinterpret_cast<StackReference<mirror::Object>*>(current_vreg_);
      spill_ref->Assign(obj);
      sm_.AdvancePointer(obj != nullptr ? spill_ref : nullptr);
      current_vreg_ += 1u;
      break;
    }
    case Primitive::kPrimFloat:
      sm_.AdvanceFloat(*reinterpret_cast<float*>(GetParamAddress()));
      current_vreg_ += 1u;
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
      sm_.AdvanceInt(*reinterpret_cast<jint*>(GetParamAddress()));
      current_vreg_ += 1u;
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
}

/*
 * Initializes the reserved area assumed to be directly below `managed_sp` for a native call:
 *
 * On entry, the stack has a standard callee-save frame above `managed_sp`,
 * and the reserved area below it. Starting below `managed_sp`, we reserve space
 * for local reference cookie (not present for @CriticalNative), HandleScope
 * (not present for @CriticalNative) and stack args (if args do not fit into
 * registers). At the bottom of the reserved area, there is space for register
 * arguments, hidden arg (for @CriticalNative) and the SP for the native call
 * (i.e. pointer to the stack args area), which the calling stub shall load
 * to perform the native call. We fill all these fields, perform class init
 * check (for static methods) and/or locking (for synchronized methods) if
 * needed and return to the stub.
 *
 * The return value is the pointer to the native code, null on failure.
 */
extern "C" const void* artQuickGenericJniTrampoline(Thread* self,
                                                    ArtMethod** managed_sp,
                                                    uintptr_t* reserved_area)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Note: We cannot walk the stack properly until fixed up below.
  ArtMethod* called = *managed_sp;
  DCHECK(called->IsNative()) << called->PrettyMethod(true);
  Runtime* runtime = Runtime::Current();
  uint32_t shorty_len = 0;
  const char* shorty = called->GetShorty(&shorty_len);
  bool critical_native = called->IsCriticalNative();
  bool fast_native = called->IsFastNative();
  bool normal_native = !critical_native && !fast_native;

  // Run the visitor and update sp.
  BuildGenericJniFrameVisitor visitor(self,
                                      called->IsStatic(),
                                      critical_native,
                                      shorty,
                                      shorty_len,
                                      managed_sp,
                                      reserved_area);
  {
    ScopedAssertNoThreadSuspension sants(__FUNCTION__);
    visitor.VisitArguments();
  }

  // Fix up managed-stack things in Thread. After this we can walk the stack.
  self->SetTopOfStackTagged(managed_sp);

  self->VerifyStack();

  // We can now walk the stack if needed by JIT GC from MethodEntered() for JIT-on-first-use.
  jit::Jit* jit = runtime->GetJit();
  if (jit != nullptr) {
    jit->MethodEntered(self, called);
  }

  // We can set the entrypoint of a native method to generic JNI even when the
  // class hasn't been initialized, so we need to do the initialization check
  // before invoking the native code.
  if (NeedsClinitCheckBeforeCall(called)) {
    ObjPtr<mirror::Class> declaring_class = called->GetDeclaringClass();
    if (UNLIKELY(!declaring_class->IsVisiblyInitialized())) {
      // Ensure static method's class is initialized.
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(declaring_class));
      if (!runtime->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
        DCHECK(Thread::Current()->IsExceptionPending()) << called->PrettyMethod();
        return nullptr;  // Report error.
      }
    }
  }

  uint32_t cookie;
  uint32_t* sp32;
  // Skip calling JniMethodStart for @CriticalNative.
  if (LIKELY(!critical_native)) {
    // Start JNI, save the cookie.
    if (called->IsSynchronized()) {
      DCHECK(normal_native) << " @FastNative and synchronize is not supported";
      jobject lock = GetGenericJniSynchronizationObject(self, called);
      cookie = JniMethodStartSynchronized(lock, self);
      if (self->IsExceptionPending()) {
        return nullptr;  // Report error.
      }
    } else {
      if (fast_native) {
        cookie = JniMethodFastStart(self);
      } else {
        DCHECK(normal_native);
        cookie = JniMethodStart(self);
      }
    }
    sp32 = reinterpret_cast<uint32_t*>(managed_sp);
    *(sp32 - 1) = cookie;
  }

  // Retrieve the stored native code.
  // Note that it may point to the lookup stub or trampoline.
  // FIXME: This is broken for @CriticalNative as the art_jni_dlsym_lookup_stub
  // does not handle that case. Calls from compiled stubs are also broken.
  void const* nativeCode = called->GetEntryPointFromJni();

  VLOG(third_party_jni) << "GenericJNI: "
                        << called->PrettyMethod()
                        << " -> "
                        << std::hex << reinterpret_cast<uintptr_t>(nativeCode);

  // Return native code.
  return nativeCode;
}

// Defined in quick_jni_entrypoints.cc.
extern uint64_t GenericJniMethodEnd(Thread* self,
                                    uint32_t saved_local_ref_cookie,
                                    jvalue result,
                                    uint64_t result_f,
                                    ArtMethod* called);

/*
 * Is called after the native JNI code. Responsible for cleanup (handle scope, saved state) and
 * unlocking.
 */
extern "C" uint64_t artQuickGenericJniEndTrampoline(Thread* self,
                                                    jvalue result,
                                                    uint64_t result_f) {
  // We're here just back from a native call. We don't have the shared mutator lock at this point
  // yet until we call GoToRunnable() later in GenericJniMethodEnd(). Accessing objects or doing
  // anything that requires a mutator lock before that would cause problems as GC may have the
  // exclusive mutator lock and may be moving objects, etc.
  ArtMethod** sp = self->GetManagedStack()->GetTopQuickFrame();
  DCHECK(self->GetManagedStack()->GetTopQuickFrameTag());
  uint32_t* sp32 = reinterpret_cast<uint32_t*>(sp);
  ArtMethod* called = *sp;
  uint32_t cookie = *(sp32 - 1);
  return GenericJniMethodEnd(self, cookie, result, result_f, called);
}

// We use TwoWordReturn to optimize scalar returns. We use the hi value for code, and the lo value
// for the method pointer.
//
// It is valid to use this, as at the usage points here (returns from C functions) we are assuming
// to hold the mutator lock (see REQUIRES_SHARED(Locks::mutator_lock_) annotations).

template <InvokeType type, bool access_check>
static TwoWordReturn artInvokeCommon(uint32_t method_idx,
                                     ObjPtr<mirror::Object> this_object,
                                     Thread* self,
                                     ArtMethod** sp) {
  ScopedQuickEntrypointChecks sqec(self);
  DCHECK_EQ(*sp, Runtime::Current()->GetCalleeSaveMethod(CalleeSaveType::kSaveRefsAndArgs));
  ArtMethod* caller_method = QuickArgumentVisitor::GetCallingMethod(sp);
  ArtMethod* method = FindMethodFast<type, access_check>(method_idx, this_object, caller_method);
  if (UNLIKELY(method == nullptr)) {
    const DexFile* dex_file = caller_method->GetDexFile();
    uint32_t shorty_len;
    const char* shorty = dex_file->GetMethodShorty(dex_file->GetMethodId(method_idx), &shorty_len);
    {
      // Remember the args in case a GC happens in FindMethodFromCode.
      ScopedObjectAccessUnchecked soa(self->GetJniEnv());
      RememberForGcArgumentVisitor visitor(sp, type == kStatic, shorty, shorty_len, &soa);
      visitor.VisitArguments();
      method = FindMethodFromCode<type, access_check>(method_idx,
                                                      &this_object,
                                                      caller_method,
                                                      self);
      visitor.FixupReferences();
    }

    if (UNLIKELY(method == nullptr)) {
      CHECK(self->IsExceptionPending());
      return GetTwoWordFailureValue();  // Failure.
    }
  }
  DCHECK(!self->IsExceptionPending());
  const void* code = method->GetEntryPointFromQuickCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  DCHECK(code != nullptr) << "Code was null in method: " << method->PrettyMethod()
                          << " location: "
                          << method->GetDexFile()->GetLocation();

  return GetTwoWordSuccessValue(reinterpret_cast<uintptr_t>(code),
                                reinterpret_cast<uintptr_t>(method));
}

// Explicit artInvokeCommon template function declarations to please analysis tool.
#define EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(type, access_check)                                \
  template REQUIRES_SHARED(Locks::mutator_lock_)                                          \
  TwoWordReturn artInvokeCommon<type, access_check>(                                            \
      uint32_t method_idx, ObjPtr<mirror::Object> his_object, Thread* self, ArtMethod** sp)

EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kVirtual, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kVirtual, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kInterface, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kInterface, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kDirect, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kDirect, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kStatic, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kStatic, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kSuper, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kSuper, true);
#undef EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL

// See comments in runtime_support_asm.S
extern "C" TwoWordReturn artInvokeInterfaceTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return artInvokeCommon<kInterface, true>(method_idx, this_object, self, sp);
}

extern "C" TwoWordReturn artInvokeDirectTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return artInvokeCommon<kDirect, true>(method_idx, this_object, self, sp);
}

extern "C" TwoWordReturn artInvokeStaticTrampolineWithAccessCheck(
    uint32_t method_idx,
    mirror::Object* this_object ATTRIBUTE_UNUSED,
    Thread* self,
    ArtMethod** sp) REQUIRES_SHARED(Locks::mutator_lock_) {
  // For static, this_object is not required and may be random garbage. Don't pass it down so that
  // it doesn't cause ObjPtr alignment failure check.
  return artInvokeCommon<kStatic, true>(method_idx, nullptr, self, sp);
}

extern "C" TwoWordReturn artInvokeSuperTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return artInvokeCommon<kSuper, true>(method_idx, this_object, self, sp);
}

extern "C" TwoWordReturn artInvokeVirtualTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return artInvokeCommon<kVirtual, true>(method_idx, this_object, self, sp);
}

// Determine target of interface dispatch. The interface method and this object are known non-null.
// The interface method is the method returned by the dex cache in the conflict trampoline.
extern "C" TwoWordReturn artInvokeInterfaceTrampoline(ArtMethod* interface_method,
                                                      mirror::Object* raw_this_object,
                                                      Thread* self,
                                                      ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);

  Runtime* runtime = Runtime::Current();
  bool resolve_method = ((interface_method == nullptr) || interface_method->IsRuntimeMethod());
  if (UNLIKELY(resolve_method)) {
    // The interface method is unresolved, so resolve it in the dex file of the caller.
    // Fetch the dex_method_idx of the target interface method from the caller.
    StackHandleScope<1> hs(self);
    Handle<mirror::Object> this_object = hs.NewHandle(raw_this_object);
    ArtMethod* caller_method = QuickArgumentVisitor::GetCallingMethod(sp);
    uint32_t dex_method_idx;
    uint32_t dex_pc = QuickArgumentVisitor::GetCallingDexPc(sp);
    const Instruction& instr = caller_method->DexInstructions().InstructionAt(dex_pc);
    Instruction::Code instr_code = instr.Opcode();
    DCHECK(instr_code == Instruction::INVOKE_INTERFACE ||
           instr_code == Instruction::INVOKE_INTERFACE_RANGE)
        << "Unexpected call into interface trampoline: " << instr.DumpString(nullptr);
    if (instr_code == Instruction::INVOKE_INTERFACE) {
      dex_method_idx = instr.VRegB_35c();
    } else {
      DCHECK_EQ(instr_code, Instruction::INVOKE_INTERFACE_RANGE);
      dex_method_idx = instr.VRegB_3rc();
    }

    const DexFile& dex_file = *caller_method->GetDexFile();
    uint32_t shorty_len;
    const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(dex_method_idx),
                                                  &shorty_len);
    {
      // Remember the args in case a GC happens in ClassLinker::ResolveMethod().
      ScopedObjectAccessUnchecked soa(self->GetJniEnv());
      RememberForGcArgumentVisitor visitor(sp, false, shorty, shorty_len, &soa);
      visitor.VisitArguments();
      ClassLinker* class_linker = runtime->GetClassLinker();
      interface_method = class_linker->ResolveMethod<ClassLinker::ResolveMode::kNoChecks>(
          self, dex_method_idx, caller_method, kInterface);
      visitor.FixupReferences();
    }

    if (UNLIKELY(interface_method == nullptr)) {
      CHECK(self->IsExceptionPending());
      return GetTwoWordFailureValue();  // Failure.
    }
    MaybeUpdateBssMethodEntry(interface_method, MethodReference(&dex_file, dex_method_idx));

    // Refresh `raw_this_object` which may have changed after resolution.
    raw_this_object = this_object.Get();
  }

  // The compiler and interpreter make sure the conflict trampoline is never
  // called on a method that resolves to j.l.Object.
  DCHECK(!interface_method->GetDeclaringClass()->IsObjectClass());
  DCHECK(interface_method->GetDeclaringClass()->IsInterface());
  DCHECK(!interface_method->IsRuntimeMethod());
  DCHECK(!interface_method->IsCopied());

  ObjPtr<mirror::Object> obj_this = raw_this_object;
  ObjPtr<mirror::Class> cls = obj_this->GetClass();
  uint32_t imt_index = interface_method->GetImtIndex();
  ImTable* imt = cls->GetImt(kRuntimePointerSize);
  ArtMethod* conflict_method = imt->Get(imt_index, kRuntimePointerSize);
  DCHECK(conflict_method->IsRuntimeMethod());

  if (UNLIKELY(resolve_method)) {
    // Now that we know the interface method, look it up in the conflict table.
    ImtConflictTable* current_table = conflict_method->GetImtConflictTable(kRuntimePointerSize);
    DCHECK(current_table != nullptr);
    ArtMethod* method = current_table->Lookup(interface_method, kRuntimePointerSize);
    if (method != nullptr) {
      return GetTwoWordSuccessValue(
          reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCode()),
          reinterpret_cast<uintptr_t>(method));
    }
    // Interface method is not in the conflict table. Continue looking up in the
    // iftable.
  }

  ArtMethod* method = cls->FindVirtualMethodForInterface(interface_method, kRuntimePointerSize);
  if (UNLIKELY(method == nullptr)) {
    ArtMethod* caller_method = QuickArgumentVisitor::GetCallingMethod(sp);
    ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(
        interface_method, obj_this.Ptr(), caller_method);
    return GetTwoWordFailureValue();
  }

  // We arrive here if we have found an implementation, and it is not in the ImtConflictTable.
  // We create a new table with the new pair { interface_method, method }.

  // Classes in the boot image should never need to update conflict methods in
  // their IMT.
  CHECK(!runtime->GetHeap()->ObjectIsInBootImageSpace(cls.Ptr())) << cls->PrettyClass();
  ArtMethod* new_conflict_method = runtime->GetClassLinker()->AddMethodToConflictTable(
      cls.Ptr(),
      conflict_method,
      interface_method,
      method);
  if (new_conflict_method != conflict_method) {
    // Update the IMT if we create a new conflict method. No fence needed here, as the
    // data is consistent.
    imt->Set(imt_index,
             new_conflict_method,
             kRuntimePointerSize);
  }

  const void* code = method->GetEntryPointFromQuickCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  DCHECK(code != nullptr) << "Code was null in method: " << method->PrettyMethod()
                          << " location: " << method->GetDexFile()->GetLocation();

  return GetTwoWordSuccessValue(reinterpret_cast<uintptr_t>(code),
                                reinterpret_cast<uintptr_t>(method));
}

// Returns uint64_t representing raw bits from JValue.
extern "C" uint64_t artInvokePolymorphic(mirror::Object* raw_receiver, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  DCHECK(raw_receiver != nullptr);
  DCHECK_EQ(*sp, Runtime::Current()->GetCalleeSaveMethod(CalleeSaveType::kSaveRefsAndArgs));

  // Start new JNI local reference state
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  const char* old_cause = self->StartAssertNoThreadSuspension("Making stack arguments safe.");

  // From the instruction, get the |callsite_shorty| and expose arguments on the stack to the GC.
  ArtMethod* caller_method = QuickArgumentVisitor::GetCallingMethod(sp);
  uint32_t dex_pc = QuickArgumentVisitor::GetCallingDexPc(sp);
  const Instruction& inst = caller_method->DexInstructions().InstructionAt(dex_pc);
  DCHECK(inst.Opcode() == Instruction::INVOKE_POLYMORPHIC ||
         inst.Opcode() == Instruction::INVOKE_POLYMORPHIC_RANGE);
  const dex::ProtoIndex proto_idx(inst.VRegH());
  const char* shorty = caller_method->GetDexFile()->GetShorty(proto_idx);
  const size_t shorty_length = strlen(shorty);
  static const bool kMethodIsStatic = false;  // invoke() and invokeExact() are not static.
  RememberForGcArgumentVisitor gc_visitor(sp, kMethodIsStatic, shorty, shorty_length, &soa);
  gc_visitor.VisitArguments();

  // Wrap raw_receiver in a Handle for safety.
  StackHandleScope<3> hs(self);
  Handle<mirror::Object> receiver_handle(hs.NewHandle(raw_receiver));
  raw_receiver = nullptr;
  self->EndAssertNoThreadSuspension(old_cause);

  // Resolve method.
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  ArtMethod* resolved_method = linker->ResolveMethod<ClassLinker::ResolveMode::kCheckICCEAndIAE>(
      self, inst.VRegB(), caller_method, kVirtual);

  Handle<mirror::MethodType> method_type(
      hs.NewHandle(linker->ResolveMethodType(self, proto_idx, caller_method)));
  if (UNLIKELY(method_type.IsNull())) {
    // This implies we couldn't resolve one or more types in this method handle.
    CHECK(self->IsExceptionPending());
    return 0UL;
  }

  DCHECK_EQ(ArtMethod::NumArgRegisters(shorty) + 1u, (uint32_t)inst.VRegA());
  DCHECK_EQ(resolved_method->IsStatic(), kMethodIsStatic);

  // Fix references before constructing the shadow frame.
  gc_visitor.FixupReferences();

  // Construct shadow frame placing arguments consecutively from |first_arg|.
  const bool is_range = (inst.Opcode() == Instruction::INVOKE_POLYMORPHIC_RANGE);
  const size_t num_vregs = is_range ? inst.VRegA_4rcc() : inst.VRegA_45cc();
  const size_t first_arg = 0;
  ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
      CREATE_SHADOW_FRAME(num_vregs, /* link= */ nullptr, resolved_method, dex_pc);
  ShadowFrame* shadow_frame = shadow_frame_unique_ptr.get();
  ScopedStackedShadowFramePusher
      frame_pusher(self, shadow_frame, StackedShadowFrameType::kShadowFrameUnderConstruction);
  BuildQuickShadowFrameVisitor shadow_frame_builder(sp,
                                                    kMethodIsStatic,
                                                    shorty,
                                                    strlen(shorty),
                                                    shadow_frame,
                                                    first_arg);
  shadow_frame_builder.VisitArguments();

  // Push a transition back into managed code onto the linked list in thread.
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);

  // Call DoInvokePolymorphic with |is_range| = true, as shadow frame has argument registers in
  // consecutive order.
  RangeInstructionOperands operands(first_arg + 1, num_vregs - 1);
  Intrinsics intrinsic = static_cast<Intrinsics>(resolved_method->GetIntrinsic());
  JValue result;
  bool success = false;
  if (resolved_method->GetDeclaringClass() == GetClassRoot<mirror::MethodHandle>(linker)) {
    Handle<mirror::MethodHandle> method_handle(hs.NewHandle(
        ObjPtr<mirror::MethodHandle>::DownCast(receiver_handle.Get())));
    if (intrinsic == Intrinsics::kMethodHandleInvokeExact) {
      success = MethodHandleInvokeExact(self,
                                        *shadow_frame,
                                        method_handle,
                                        method_type,
                                        &operands,
                                        &result);
    } else {
      DCHECK_EQ(static_cast<uint32_t>(intrinsic),
                static_cast<uint32_t>(Intrinsics::kMethodHandleInvoke));
      success = MethodHandleInvoke(self,
                                   *shadow_frame,
                                   method_handle,
                                   method_type,
                                   &operands,
                                   &result);
    }
  } else {
    DCHECK_EQ(GetClassRoot<mirror::VarHandle>(linker), resolved_method->GetDeclaringClass());
    Handle<mirror::VarHandle> var_handle(hs.NewHandle(
        ObjPtr<mirror::VarHandle>::DownCast(receiver_handle.Get())));
    mirror::VarHandle::AccessMode access_mode =
        mirror::VarHandle::GetAccessModeByIntrinsic(intrinsic);
    success = VarHandleInvokeAccessor(self,
                                      *shadow_frame,
                                      var_handle,
                                      method_type,
                                      access_mode,
                                      &operands,
                                      &result);
  }

  DCHECK(success || self->IsExceptionPending());

  // Pop transition record.
  self->PopManagedStackFragment(fragment);

  return result.GetJ();
}

// Returns uint64_t representing raw bits from JValue.
extern "C" uint64_t artInvokeCustom(uint32_t call_site_idx, Thread* self, ArtMethod** sp)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  DCHECK_EQ(*sp, Runtime::Current()->GetCalleeSaveMethod(CalleeSaveType::kSaveRefsAndArgs));

  // invoke-custom is effectively a static call (no receiver).
  static constexpr bool kMethodIsStatic = true;

  // Start new JNI local reference state
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);

  const char* old_cause = self->StartAssertNoThreadSuspension("Making stack arguments safe.");

  // From the instruction, get the |callsite_shorty| and expose arguments on the stack to the GC.
  ArtMethod* caller_method = QuickArgumentVisitor::GetCallingMethod(sp);
  uint32_t dex_pc = QuickArgumentVisitor::GetCallingDexPc(sp);
  const DexFile* dex_file = caller_method->GetDexFile();
  const dex::ProtoIndex proto_idx(dex_file->GetProtoIndexForCallSite(call_site_idx));
  const char* shorty = caller_method->GetDexFile()->GetShorty(proto_idx);
  const uint32_t shorty_len = strlen(shorty);

  // Construct the shadow frame placing arguments consecutively from |first_arg|.
  const size_t first_arg = 0;
  const size_t num_vregs = ArtMethod::NumArgRegisters(shorty);
  ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
      CREATE_SHADOW_FRAME(num_vregs, /* link= */ nullptr, caller_method, dex_pc);
  ShadowFrame* shadow_frame = shadow_frame_unique_ptr.get();
  ScopedStackedShadowFramePusher
      frame_pusher(self, shadow_frame, StackedShadowFrameType::kShadowFrameUnderConstruction);
  BuildQuickShadowFrameVisitor shadow_frame_builder(sp,
                                                    kMethodIsStatic,
                                                    shorty,
                                                    shorty_len,
                                                    shadow_frame,
                                                    first_arg);
  shadow_frame_builder.VisitArguments();

  // Push a transition back into managed code onto the linked list in thread.
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);
  self->EndAssertNoThreadSuspension(old_cause);

  // Perform the invoke-custom operation.
  RangeInstructionOperands operands(first_arg, num_vregs);
  JValue result;
  bool success =
      interpreter::DoInvokeCustom(self, *shadow_frame, call_site_idx, &operands, &result);
  DCHECK(success || self->IsExceptionPending());

  // Pop transition record.
  self->PopManagedStackFragment(fragment);

  return result.GetJ();
}

}  // namespace art
