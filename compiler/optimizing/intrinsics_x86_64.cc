/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "intrinsics_x86_64.h"

#include "arch/x86_64/instruction_set_features_x86_64.h"
#include "code_generator_x86_64.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "intrinsics.h"
#include "mirror/array-inl.h"
#include "mirror/art_method.h"
#include "mirror/string.h"
#include "thread.h"
#include "utils/x86_64/assembler_x86_64.h"
#include "utils/x86_64/constants_x86_64.h"

namespace art {

namespace x86_64 {

IntrinsicLocationsBuilderX86_64::IntrinsicLocationsBuilderX86_64(CodeGeneratorX86_64* codegen)
  : arena_(codegen->GetGraph()->GetArena()), codegen_(codegen) {
}


X86_64Assembler* IntrinsicCodeGeneratorX86_64::GetAssembler() {
  return reinterpret_cast<X86_64Assembler*>(codegen_->GetAssembler());
}

ArenaAllocator* IntrinsicCodeGeneratorX86_64::GetAllocator() {
  return codegen_->GetGraph()->GetArena();
}

bool IntrinsicLocationsBuilderX86_64::TryDispatch(HInvoke* invoke) {
  Dispatch(invoke);
  const LocationSummary* res = invoke->GetLocations();
  return res != nullptr && res->Intrinsified();
}

#define __ reinterpret_cast<X86_64Assembler*>(codegen->GetAssembler())->

// TODO: trg as memory.
static void MoveFromReturnRegister(Location trg,
                                   Primitive::Type type,
                                   CodeGeneratorX86_64* codegen) {
  if (!trg.IsValid()) {
    DCHECK(type == Primitive::kPrimVoid);
    return;
  }

  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      CpuRegister trg_reg = trg.AsRegister<CpuRegister>();
      if (trg_reg.AsRegister() != RAX) {
        __ movl(trg_reg, CpuRegister(RAX));
      }
      break;
    }
    case Primitive::kPrimLong: {
      CpuRegister trg_reg = trg.AsRegister<CpuRegister>();
      if (trg_reg.AsRegister() != RAX) {
        __ movq(trg_reg, CpuRegister(RAX));
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected void type for valid location " << trg;
      UNREACHABLE();

    case Primitive::kPrimDouble: {
      XmmRegister trg_reg = trg.AsFpuRegister<XmmRegister>();
      if (trg_reg.AsFloatRegister() != XMM0) {
        __ movsd(trg_reg, XmmRegister(XMM0));
      }
      break;
    }
    case Primitive::kPrimFloat: {
      XmmRegister trg_reg = trg.AsFpuRegister<XmmRegister>();
      if (trg_reg.AsFloatRegister() != XMM0) {
        __ movss(trg_reg, XmmRegister(XMM0));
      }
      break;
    }
  }
}

static void MoveArguments(HInvoke* invoke, ArenaAllocator* arena, CodeGeneratorX86_64* codegen) {
  if (invoke->InputCount() == 0) {
    return;
  }

  LocationSummary* locations = invoke->GetLocations();
  InvokeDexCallingConventionVisitor calling_convention_visitor;

  // We're moving potentially two or more locations to locations that could overlap, so we need
  // a parallel move resolver.
  HParallelMove parallel_move(arena);

  for (size_t i = 0; i < invoke->InputCount(); i++) {
    HInstruction* input = invoke->InputAt(i);
    Location cc_loc = calling_convention_visitor.GetNextLocation(input->GetType());
    Location actual_loc = locations->InAt(i);

    parallel_move.AddMove(actual_loc, cc_loc, input->GetType(), nullptr);
  }

  codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
}

// Slow-path for fallback (calling the managed code to handle the intrinsic) in an intrinsified
// call. This will copy the arguments into the positions for a regular call.
//
// Note: The actual parameters are required to be in the locations given by the invoke's location
//       summary. If an intrinsic modifies those locations before a slowpath call, they must be
//       restored!
class IntrinsicSlowPathX86_64 : public SlowPathCodeX86_64 {
 public:
  explicit IntrinsicSlowPathX86_64(HInvoke* invoke) : invoke_(invoke) { }

  void EmitNativeCode(CodeGenerator* codegen_in) OVERRIDE {
    CodeGeneratorX86_64* codegen = down_cast<CodeGeneratorX86_64*>(codegen_in);
    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, invoke_->GetLocations());

    MoveArguments(invoke_, codegen->GetGraph()->GetArena(), codegen);

    if (invoke_->IsInvokeStaticOrDirect()) {
      codegen->GenerateStaticOrDirectCall(invoke_->AsInvokeStaticOrDirect(), CpuRegister(RDI));
      RecordPcInfo(codegen, invoke_, invoke_->GetDexPc());
    } else {
      UNIMPLEMENTED(FATAL) << "Non-direct intrinsic slow-path not yet implemented";
      UNREACHABLE();
    }

    // Copy the result back to the expected output.
    Location out = invoke_->GetLocations()->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister());  // TODO: Replace this when we support output in memory.
      DCHECK(!invoke_->GetLocations()->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      MoveFromReturnRegister(out, invoke_->GetType(), codegen);
    }

    RestoreLiveRegisters(codegen, invoke_->GetLocations());
    __ jmp(GetExitLabel());
  }

 private:
  // The instruction where this slow path is happening.
  HInvoke* const invoke_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicSlowPathX86_64);
};

#undef __
#define __ assembler->

static void CreateFPToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresRegister());
}

static void CreateIntToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresFpuRegister());
}

static void MoveFPToInt(LocationSummary* locations, bool is64bit, X86_64Assembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  __ movd(output.AsRegister<CpuRegister>(), input.AsFpuRegister<XmmRegister>(), is64bit);
}

static void MoveIntToFP(LocationSummary* locations, bool is64bit, X86_64Assembler* assembler) {
  Location input = locations->InAt(0);
  Location output = locations->Out();
  __ movd(output.AsFpuRegister<XmmRegister>(), input.AsRegister<CpuRegister>(), is64bit);
}

void IntrinsicLocationsBuilderX86_64::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitDoubleDoubleToRawLongBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), true, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitDoubleLongBitsToDouble(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), true, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  CreateFPToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  CreateIntToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitFloatFloatToRawIntBits(HInvoke* invoke) {
  MoveFPToInt(invoke->GetLocations(), false, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitFloatIntBitsToFloat(HInvoke* invoke) {
  MoveIntToFP(invoke->GetLocations(), false, GetAssembler());
}

static void CreateIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

static void GenReverseBytes(LocationSummary* locations,
                            Primitive::Type size,
                            X86_64Assembler* assembler) {
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  switch (size) {
    case Primitive::kPrimShort:
      // TODO: Can be done with an xchg of 8b registers. This is straight from Quick.
      __ bswapl(out);
      __ sarl(out, Immediate(16));
      break;
    case Primitive::kPrimInt:
      __ bswapl(out);
      break;
    case Primitive::kPrimLong:
      __ bswapq(out);
      break;
    default:
      LOG(FATAL) << "Unexpected size for reverse-bytes: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitLongReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitLongReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitShortReverseBytes(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitShortReverseBytes(HInvoke* invoke) {
  GenReverseBytes(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}


// TODO: Consider Quick's way of doing Double abs through integer operations, as the immediate we
//       need is 64b.

static void CreateFloatToFloatPlusTemps(ArenaAllocator* arena, HInvoke* invoke) {
  // TODO: Enable memory operations when the assembler supports them.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  // TODO: Allow x86 to work with memory. This requires assembler support, see below.
  // locations->SetInAt(0, Location::Any());               // X86 can work on memory directly.
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresFpuRegister());  // FP reg to hold mask.
}

static void MathAbsFP(LocationSummary* locations,
                      bool is64bit,
                      X86_64Assembler* assembler,
                      CodeGeneratorX86_64* codegen) {
  Location output = locations->Out();

  if (output.IsFpuRegister()) {
    // In-register
    XmmRegister xmm_temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();

    // TODO: Can mask directly with constant area using pand if we can guarantee
    // that the literal is aligned on a 16 byte boundary.  This will avoid a
    // temporary.
    if (is64bit) {
      __ movsd(xmm_temp, codegen->LiteralInt64Address(INT64_C(0x7FFFFFFFFFFFFFFF)));
      __ andpd(output.AsFpuRegister<XmmRegister>(), xmm_temp);
    } else {
      __ movss(xmm_temp, codegen->LiteralInt32Address(INT32_C(0x7FFFFFFF)));
      __ andps(output.AsFpuRegister<XmmRegister>(), xmm_temp);
    }
  } else {
    // TODO: update when assember support is available.
    UNIMPLEMENTED(FATAL) << "Needs assembler support.";
//  Once assembler support is available, in-memory operations look like this:
//    if (is64bit) {
//      DCHECK(output.IsDoubleStackSlot());
//      // No 64b and with literal.
//      __ movq(cpu_temp, Immediate(INT64_C(0x7FFFFFFFFFFFFFFF)));
//      __ andq(Address(CpuRegister(RSP), output.GetStackIndex()), cpu_temp);
//    } else {
//      DCHECK(output.IsStackSlot());
//      // Can use and with a literal directly.
//      __ andl(Address(CpuRegister(RSP), output.GetStackIndex()), Immediate(INT64_C(0x7FFFFFFF)));
//    }
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMathAbsDouble(HInvoke* invoke) {
  CreateFloatToFloatPlusTemps(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAbsDouble(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), true, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitMathAbsFloat(HInvoke* invoke) {
  CreateFloatToFloatPlusTemps(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAbsFloat(HInvoke* invoke) {
  MathAbsFP(invoke->GetLocations(), false, GetAssembler(), codegen_);
}

static void CreateIntToIntPlusTemp(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
}

static void GenAbsInteger(LocationSummary* locations, bool is64bit, X86_64Assembler* assembler) {
  Location output = locations->Out();
  CpuRegister out = output.AsRegister<CpuRegister>();
  CpuRegister mask = locations->GetTemp(0).AsRegister<CpuRegister>();

  if (is64bit) {
    // Create mask.
    __ movq(mask, out);
    __ sarq(mask, Immediate(63));
    // Add mask.
    __ addq(out, mask);
    __ xorq(out, mask);
  } else {
    // Create mask.
    __ movl(mask, out);
    __ sarl(mask, Immediate(31));
    // Add mask.
    __ addl(out, mask);
    __ xorl(out, mask);
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMathAbsInt(HInvoke* invoke) {
  CreateIntToIntPlusTemp(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAbsInt(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), false, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMathAbsLong(HInvoke* invoke) {
  CreateIntToIntPlusTemp(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathAbsLong(HInvoke* invoke) {
  GenAbsInteger(invoke->GetLocations(), true, GetAssembler());
}

static void GenMinMaxFP(LocationSummary* locations,
                        bool is_min,
                        bool is_double,
                        X86_64Assembler* assembler,
                        CodeGeneratorX86_64* codegen) {
  Location op1_loc = locations->InAt(0);
  Location op2_loc = locations->InAt(1);
  Location out_loc = locations->Out();
  XmmRegister out = out_loc.AsFpuRegister<XmmRegister>();

  // Shortcut for same input locations.
  if (op1_loc.Equals(op2_loc)) {
    DCHECK(out_loc.Equals(op1_loc));
    return;
  }

  //  (out := op1)
  //  out <=? op2
  //  if Nan jmp Nan_label
  //  if out is min jmp done
  //  if op2 is min jmp op2_label
  //  handle -0/+0
  //  jmp done
  // Nan_label:
  //  out := NaN
  // op2_label:
  //  out := op2
  // done:
  //
  // This removes one jmp, but needs to copy one input (op1) to out.
  //
  // TODO: This is straight from Quick. Make NaN an out-of-line slowpath?

  XmmRegister op2 = op2_loc.AsFpuRegister<XmmRegister>();

  Label nan, done, op2_label;
  if (is_double) {
    __ ucomisd(out, op2);
  } else {
    __ ucomiss(out, op2);
  }

  __ j(Condition::kParityEven, &nan);

  __ j(is_min ? Condition::kAbove : Condition::kBelow, &op2_label);
  __ j(is_min ? Condition::kBelow : Condition::kAbove, &done);

  // Handle 0.0/-0.0.
  if (is_min) {
    if (is_double) {
      __ orpd(out, op2);
    } else {
      __ orps(out, op2);
    }
  } else {
    if (is_double) {
      __ andpd(out, op2);
    } else {
      __ andps(out, op2);
    }
  }
  __ jmp(&done);

  // NaN handling.
  __ Bind(&nan);
  if (is_double) {
    __ movsd(out, codegen->LiteralInt64Address(INT64_C(0x7FF8000000000000)));
  } else {
    __ movss(out, codegen->LiteralInt32Address(INT32_C(0x7FC00000)));
  }
  __ jmp(&done);

  // out := op2;
  __ Bind(&op2_label);
  if (is_double) {
    __ movsd(out, op2);
  } else {
    __ movss(out, op2);
  }

  // Done.
  __ Bind(&done);
}

static void CreateFPFPToFP(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresFpuRegister());
  // The following is sub-optimal, but all we can do for now. It would be fine to also accept
  // the second input to be the output (we can simply swap inputs).
  locations->SetOut(Location::SameAsFirstInput());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMinDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMinDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), true, true, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitMathMinFloatFloat(HInvoke* invoke) {
  CreateFPFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMinFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), true, false, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  CreateFPFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMaxDoubleDouble(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), false, true, GetAssembler(), codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitMathMaxFloatFloat(HInvoke* invoke) {
  CreateFPFPToFP(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMaxFloatFloat(HInvoke* invoke) {
  GenMinMaxFP(invoke->GetLocations(), false, false, GetAssembler(), codegen_);
}

static void GenMinMax(LocationSummary* locations, bool is_min, bool is_long,
                      X86_64Assembler* assembler) {
  Location op1_loc = locations->InAt(0);
  Location op2_loc = locations->InAt(1);

  // Shortcut for same input locations.
  if (op1_loc.Equals(op2_loc)) {
    // Can return immediately, as op1_loc == out_loc.
    // Note: if we ever support separate registers, e.g., output into memory, we need to check for
    //       a copy here.
    DCHECK(locations->Out().Equals(op1_loc));
    return;
  }

  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  CpuRegister op2 = op2_loc.AsRegister<CpuRegister>();

  //  (out := op1)
  //  out <=? op2
  //  if out is min jmp done
  //  out := op2
  // done:

  if (is_long) {
    __ cmpq(out, op2);
  } else {
    __ cmpl(out, op2);
  }

  __ cmov(is_min ? Condition::kGreater : Condition::kLess, out, op2, is_long);
}

static void CreateIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMinIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMinIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), true, false, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMinLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMinLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), true, true, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMaxIntInt(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMaxIntInt(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), false, false, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMathMaxLongLong(HInvoke* invoke) {
  CreateIntIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathMaxLongLong(HInvoke* invoke) {
  GenMinMax(invoke->GetLocations(), false, true, GetAssembler());
}

static void CreateFPToFPLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetOut(Location::RequiresFpuRegister());
}

void IntrinsicLocationsBuilderX86_64::VisitMathSqrt(HInvoke* invoke) {
  CreateFPToFPLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMathSqrt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();

  GetAssembler()->sqrtsd(out, in);
}

static void InvokeOutOfLineIntrinsic(CodeGeneratorX86_64* codegen, HInvoke* invoke) {
  MoveArguments(invoke, codegen->GetGraph()->GetArena(), codegen);

  DCHECK(invoke->IsInvokeStaticOrDirect());
  codegen->GenerateStaticOrDirectCall(invoke->AsInvokeStaticOrDirect(), CpuRegister(RDI));
  codegen->RecordPcInfo(invoke, invoke->GetDexPc());

  // Copy the result back to the expected output.
  Location out = invoke->GetLocations()->Out();
  if (out.IsValid()) {
    DCHECK(out.IsRegister());
    MoveFromReturnRegister(out, invoke->GetType(), codegen);
  }
}

static void CreateSSE41FPToFPLocations(ArenaAllocator* arena,
                                      HInvoke* invoke,
                                      CodeGeneratorX86_64* codegen) {
  // Do we have instruction support?
  if (codegen->GetInstructionSetFeatures().HasSSE4_1()) {
    CreateFPToFPLocations(arena, invoke);
    return;
  }

  // We have to fall back to a call to the intrinsic.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::FpuRegisterLocation(XMM0));
  // Needs to be RDI for the invoke.
  locations->AddTemp(Location::RegisterLocation(RDI));
}

static void GenSSE41FPToFPIntrinsic(CodeGeneratorX86_64* codegen,
                                   HInvoke* invoke,
                                   X86_64Assembler* assembler,
                                   int round_mode) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen, invoke);
  } else {
    XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
    XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
    __ roundsd(out, in, Immediate(round_mode));
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMathCeil(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitMathCeil(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 2);
}

void IntrinsicLocationsBuilderX86_64::VisitMathFloor(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitMathFloor(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 1);
}

void IntrinsicLocationsBuilderX86_64::VisitMathRint(HInvoke* invoke) {
  CreateSSE41FPToFPLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitMathRint(HInvoke* invoke) {
  GenSSE41FPToFPIntrinsic(codegen_, invoke, GetAssembler(), 0);
}

static void CreateSSE41FPToIntLocations(ArenaAllocator* arena,
                                       HInvoke* invoke,
                                       CodeGeneratorX86_64* codegen) {
  // Do we have instruction support?
  if (codegen->GetInstructionSetFeatures().HasSSE4_1()) {
    LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                              LocationSummary::kNoCall,
                                                              kIntrinsified);
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetOut(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
    return;
  }

  // We have to fall back to a call to the intrinsic.
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetFpuRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(RAX));
  // Needs to be RDI for the invoke.
  locations->AddTemp(Location::RegisterLocation(RDI));
}

void IntrinsicLocationsBuilderX86_64::VisitMathRoundFloat(HInvoke* invoke) {
  CreateSSE41FPToIntLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitMathRoundFloat(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen_, invoke);
    return;
  }

  // Implement RoundFloat as t1 = floor(input + 0.5f);  convert to int.
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  XmmRegister maxInt = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
  XmmRegister inPlusPointFive = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
  Label done, nan;
  X86_64Assembler* assembler = GetAssembler();

  // Generate 0.5 into inPlusPointFive.
  __ movl(out, Immediate(bit_cast<int32_t, float>(0.5f)));
  __ movd(inPlusPointFive, out, false);

  // Add in the input.
  __ addss(inPlusPointFive, in);

  // And truncate to an integer.
  __ roundss(inPlusPointFive, inPlusPointFive, Immediate(1));

  __ movl(out, Immediate(kPrimIntMax));
  // maxInt = int-to-float(out)
  __ cvtsi2ss(maxInt, out);

  // if inPlusPointFive >= maxInt goto done
  __ comiss(inPlusPointFive, maxInt);
  __ j(kAboveEqual, &done);

  // if input == NaN goto nan
  __ j(kUnordered, &nan);

  // output = float-to-int-truncate(input)
  __ cvttss2si(out, inPlusPointFive);
  __ jmp(&done);
  __ Bind(&nan);

  //  output = 0
  __ xorl(out, out);
  __ Bind(&done);
}

void IntrinsicLocationsBuilderX86_64::VisitMathRoundDouble(HInvoke* invoke) {
  CreateSSE41FPToIntLocations(arena_, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitMathRoundDouble(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  if (locations->WillCall()) {
    InvokeOutOfLineIntrinsic(codegen_, invoke);
    return;
  }

  // Implement RoundDouble as t1 = floor(input + 0.5);  convert to long.
  XmmRegister in = locations->InAt(0).AsFpuRegister<XmmRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  XmmRegister maxLong = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
  XmmRegister inPlusPointFive = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
  Label done, nan;
  X86_64Assembler* assembler = GetAssembler();

  // Generate 0.5 into inPlusPointFive.
  __ movq(out, Immediate(bit_cast<int64_t, double>(0.5)));
  __ movd(inPlusPointFive, out, true);

  // Add in the input.
  __ addsd(inPlusPointFive, in);

  // And truncate to an integer.
  __ roundsd(inPlusPointFive, inPlusPointFive, Immediate(1));

  __ movq(out, Immediate(kPrimLongMax));
  // maxLong = long-to-double(out)
  __ cvtsi2sd(maxLong, out, true);

  // if inPlusPointFive >= maxLong goto done
  __ comisd(inPlusPointFive, maxLong);
  __ j(kAboveEqual, &done);

  // if input == NaN goto nan
  __ j(kUnordered, &nan);

  // output = double-to-long-truncate(input)
  __ cvttsd2si(out, inPlusPointFive, true);
  __ jmp(&done);
  __ Bind(&nan);

  //  output = 0
  __ xorq(out, out);
  __ Bind(&done);
}

void IntrinsicLocationsBuilderX86_64::VisitStringCharAt(HInvoke* invoke) {
  // The inputs plus one temp.
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCallOnSlowPath,
                                                            kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorX86_64::VisitStringCharAt(HInvoke* invoke) {
  LocationSummary* locations = invoke->GetLocations();

  // Location of reference to data array
  const int32_t value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count
  const int32_t count_offset = mirror::String::CountOffset().Int32Value();
  // Starting offset within data array
  const int32_t offset_offset = mirror::String::OffsetOffset().Int32Value();
  // Start of char data with array_
  const int32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Int32Value();

  CpuRegister obj = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister idx = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  Location temp_loc = locations->GetTemp(0);
  CpuRegister temp = temp_loc.AsRegister<CpuRegister>();

  // TODO: Maybe we can support range check elimination. Overall, though, I think it's not worth
  //       the cost.
  // TODO: For simplicity, the index parameter is requested in a register, so different from Quick
  //       we will not optimize the code for constants (which would save a register).

  SlowPathCodeX86_64* slow_path = new (GetAllocator()) IntrinsicSlowPathX86_64(invoke);
  codegen_->AddSlowPath(slow_path);

  X86_64Assembler* assembler = GetAssembler();

  __ cmpl(idx, Address(obj, count_offset));
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  __ j(kAboveEqual, slow_path->GetEntryLabel());

  // Get the actual element.
  __ movl(temp, idx);                          // temp := idx.
  __ addl(temp, Address(obj, offset_offset));  // temp := offset + idx.
  __ movl(out, Address(obj, value_offset));    // obj := obj.array.
  // out = out[2*temp].
  __ movzxw(out, Address(out, temp, ScaleFactor::TIMES_2, data_offset));

  __ Bind(slow_path->GetExitLabel());
}

void IntrinsicLocationsBuilderX86_64::VisitStringCompareTo(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kCall,
                                                            kIntrinsified);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(RAX));
}

void IntrinsicCodeGeneratorX86_64::VisitStringCompareTo(HInvoke* invoke) {
  X86_64Assembler* assembler = GetAssembler();
  LocationSummary* locations = invoke->GetLocations();

  // Note that the null check must have been done earlier.
  DCHECK(!invoke->CanDoImplicitNullCheckOn(invoke->InputAt(0)));

  CpuRegister argument = locations->InAt(1).AsRegister<CpuRegister>();
  __ testl(argument, argument);
  SlowPathCodeX86_64* slow_path = new (GetAllocator()) IntrinsicSlowPathX86_64(invoke);
  codegen_->AddSlowPath(slow_path);
  __ j(kEqual, slow_path->GetEntryLabel());

  __ gs()->call(Address::Absolute(
        QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, pStringCompareTo), true));
  __ Bind(slow_path->GetExitLabel());
}

static void GenPeek(LocationSummary* locations, Primitive::Type size, X86_64Assembler* assembler) {
  CpuRegister address = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();  // == address, here for clarity.
  // x86 allows unaligned access. We do not have to check the input or use specific instructions
  // to avoid a SIGBUS.
  switch (size) {
    case Primitive::kPrimByte:
      __ movsxb(out, Address(address, 0));
      break;
    case Primitive::kPrimShort:
      __ movsxw(out, Address(address, 0));
      break;
    case Primitive::kPrimInt:
      __ movl(out, Address(address, 0));
      break;
    case Primitive::kPrimLong:
      __ movq(out, Address(address, 0));
      break;
    default:
      LOG(FATAL) << "Type not recognized for peek: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPeekByte(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPeekByte(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimByte, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPeekIntNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPeekIntNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPeekLongNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPeekLongNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPeekShortNative(HInvoke* invoke) {
  CreateIntToIntLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPeekShortNative(HInvoke* invoke) {
  GenPeek(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

static void CreateIntIntToVoidLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

static void GenPoke(LocationSummary* locations, Primitive::Type size, X86_64Assembler* assembler) {
  CpuRegister address = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister value = locations->InAt(1).AsRegister<CpuRegister>();
  // x86 allows unaligned access. We do not have to check the input or use specific instructions
  // to avoid a SIGBUS.
  switch (size) {
    case Primitive::kPrimByte:
      __ movb(Address(address, 0), value);
      break;
    case Primitive::kPrimShort:
      __ movw(Address(address, 0), value);
      break;
    case Primitive::kPrimInt:
      __ movl(Address(address, 0), value);
      break;
    case Primitive::kPrimLong:
      __ movq(Address(address, 0), value);
      break;
    default:
      LOG(FATAL) << "Type not recognized for poke: " << size;
      UNREACHABLE();
  }
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPokeByte(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPokeByte(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimByte, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPokeIntNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPokeIntNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimInt, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPokeLongNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPokeLongNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimLong, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitMemoryPokeShortNative(HInvoke* invoke) {
  CreateIntIntToVoidLocations(arena_, invoke);
}

void IntrinsicCodeGeneratorX86_64::VisitMemoryPokeShortNative(HInvoke* invoke) {
  GenPoke(invoke->GetLocations(), Primitive::kPrimShort, GetAssembler());
}

void IntrinsicLocationsBuilderX86_64::VisitThreadCurrentThread(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                            LocationSummary::kNoCall,
                                                            kIntrinsified);
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicCodeGeneratorX86_64::VisitThreadCurrentThread(HInvoke* invoke) {
  CpuRegister out = invoke->GetLocations()->Out().AsRegister<CpuRegister>();
  GetAssembler()->gs()->movl(out, Address::Absolute(Thread::PeerOffset<kX86_64WordSize>(), true));
}

static void GenUnsafeGet(LocationSummary* locations, Primitive::Type type,
                         bool is_volatile ATTRIBUTE_UNUSED, X86_64Assembler* assembler) {
  CpuRegister base = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister offset = locations->InAt(2).AsRegister<CpuRegister>();
  CpuRegister trg = locations->Out().AsRegister<CpuRegister>();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      __ movl(trg, Address(base, offset, ScaleFactor::TIMES_1, 0));
      break;

    case Primitive::kPrimLong:
      __ movq(trg, Address(base, offset, ScaleFactor::TIMES_1, 0));
      break;

    default:
      LOG(FATAL) << "Unsupported op size " << type;
      UNREACHABLE();
  }
}

static void CreateIntIntIntToIntLocations(ArenaAllocator* arena, HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafeGet(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetLong(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetObject(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntToIntLocations(arena_, invoke);
}


void IntrinsicCodeGeneratorX86_64::VisitUnsafeGet(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimInt, false, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimInt, true, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetLong(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimLong, false, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetLongVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimLong, true, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetObject(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimNot, false, GetAssembler());
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafeGetObjectVolatile(HInvoke* invoke) {
  GenUnsafeGet(invoke->GetLocations(), Primitive::kPrimNot, true, GetAssembler());
}


static void CreateIntIntIntIntToVoidPlusTempsLocations(ArenaAllocator* arena,
                                                       Primitive::Type type,
                                                       HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  locations->SetInAt(3, Location::RequiresRegister());
  if (type == Primitive::kPrimNot) {
    // Need temp registers for card-marking.
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafePut(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimInt, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutObject(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimNot, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutLong(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke);
}
void IntrinsicLocationsBuilderX86_64::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  CreateIntIntIntIntToVoidPlusTempsLocations(arena_, Primitive::kPrimLong, invoke);
}

// We don't care for ordered: it requires an AnyStore barrier, which is already given by the x86
// memory model.
static void GenUnsafePut(LocationSummary* locations, Primitive::Type type, bool is_volatile,
                         CodeGeneratorX86_64* codegen) {
  X86_64Assembler* assembler = reinterpret_cast<X86_64Assembler*>(codegen->GetAssembler());
  CpuRegister base = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister offset = locations->InAt(2).AsRegister<CpuRegister>();
  CpuRegister value = locations->InAt(3).AsRegister<CpuRegister>();

  if (type == Primitive::kPrimLong) {
    __ movq(Address(base, offset, ScaleFactor::TIMES_1, 0), value);
  } else {
    __ movl(Address(base, offset, ScaleFactor::TIMES_1, 0), value);
  }

  if (is_volatile) {
    __ mfence();
  }

  if (type == Primitive::kPrimNot) {
    codegen->MarkGCCard(locations->GetTemp(0).AsRegister<CpuRegister>(),
                        locations->GetTemp(1).AsRegister<CpuRegister>(),
                        base,
                        value);
  }
}

void IntrinsicCodeGeneratorX86_64::VisitUnsafePut(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimInt, true, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutObject(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutObjectOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutObjectVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimNot, true, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutLong(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutLongOrdered(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, false, codegen_);
}
void IntrinsicCodeGeneratorX86_64::VisitUnsafePutLongVolatile(HInvoke* invoke) {
  GenUnsafePut(invoke->GetLocations(), Primitive::kPrimLong, true, codegen_);
}

static void CreateIntIntIntIntIntToInt(ArenaAllocator* arena, Primitive::Type type,
                                       HInvoke* invoke) {
  LocationSummary* locations = new (arena) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::NoLocation());        // Unused receiver.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetInAt(2, Location::RequiresRegister());
  // expected value must be in EAX/RAX.
  locations->SetInAt(3, Location::RegisterLocation(RAX));
  locations->SetInAt(4, Location::RequiresRegister());

  locations->SetOut(Location::RequiresRegister());
  if (type == Primitive::kPrimNot) {
    // Need temp registers for card-marking.
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafeCASInt(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, Primitive::kPrimInt, invoke);
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafeCASLong(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, Primitive::kPrimLong, invoke);
}

void IntrinsicLocationsBuilderX86_64::VisitUnsafeCASObject(HInvoke* invoke) {
  CreateIntIntIntIntIntToInt(arena_, Primitive::kPrimNot, invoke);
}

static void GenCAS(Primitive::Type type, HInvoke* invoke, CodeGeneratorX86_64* codegen) {
  X86_64Assembler* assembler =
    reinterpret_cast<X86_64Assembler*>(codegen->GetAssembler());
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister base = locations->InAt(1).AsRegister<CpuRegister>();
  CpuRegister offset = locations->InAt(2).AsRegister<CpuRegister>();
  CpuRegister expected = locations->InAt(3).AsRegister<CpuRegister>();
  DCHECK_EQ(expected.AsRegister(), RAX);
  CpuRegister value = locations->InAt(4).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  if (type == Primitive::kPrimLong) {
    __ LockCmpxchgq(Address(base, offset, TIMES_1, 0), value);
  } else {
    // Integer or object.
    if (type == Primitive::kPrimNot) {
      // Mark card for object assuming new value is stored.
      codegen->MarkGCCard(locations->GetTemp(0).AsRegister<CpuRegister>(),
                          locations->GetTemp(1).AsRegister<CpuRegister>(),
                          base,
                          value);
    }

    __ LockCmpxchgl(Address(base, offset, TIMES_1, 0), value);
  }

  // locked cmpxchg has full barrier semantics, and we don't need scheduling
  // barriers at this time.

  // Convert ZF into the boolean result.
  __ setcc(kZero, out);
  __ movzxb(out, out);
}

void IntrinsicCodeGeneratorX86_64::VisitUnsafeCASInt(HInvoke* invoke) {
  GenCAS(Primitive::kPrimInt, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitUnsafeCASLong(HInvoke* invoke) {
  GenCAS(Primitive::kPrimLong, invoke, codegen_);
}

void IntrinsicCodeGeneratorX86_64::VisitUnsafeCASObject(HInvoke* invoke) {
  GenCAS(Primitive::kPrimNot, invoke, codegen_);
}

void IntrinsicLocationsBuilderX86_64::VisitIntegerReverse(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
}

static void SwapBits(CpuRegister reg, CpuRegister temp, int32_t shift, int32_t mask,
                     X86_64Assembler* assembler) {
  Immediate imm_shift(shift);
  Immediate imm_mask(mask);
  __ movl(temp, reg);
  __ shrl(reg, imm_shift);
  __ andl(temp, imm_mask);
  __ andl(reg, imm_mask);
  __ shll(temp, imm_shift);
  __ orl(reg, temp);
}

void IntrinsicCodeGeneratorX86_64::VisitIntegerReverse(HInvoke* invoke) {
  X86_64Assembler* assembler =
    reinterpret_cast<X86_64Assembler*>(codegen_->GetAssembler());
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister reg = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();

  /*
   * Use one bswap instruction to reverse byte order first and then use 3 rounds of
   * swapping bits to reverse bits in a number x. Using bswap to save instructions
   * compared to generic luni implementation which has 5 rounds of swapping bits.
   * x = bswap x
   * x = (x & 0x55555555) << 1 | (x >> 1) & 0x55555555;
   * x = (x & 0x33333333) << 2 | (x >> 2) & 0x33333333;
   * x = (x & 0x0F0F0F0F) << 4 | (x >> 4) & 0x0F0F0F0F;
   */
  __ bswapl(reg);
  SwapBits(reg, temp, 1, 0x55555555, assembler);
  SwapBits(reg, temp, 2, 0x33333333, assembler);
  SwapBits(reg, temp, 4, 0x0f0f0f0f, assembler);
}

void IntrinsicLocationsBuilderX86_64::VisitLongReverse(HInvoke* invoke) {
  LocationSummary* locations = new (arena_) LocationSummary(invoke,
                                                           LocationSummary::kNoCall,
                                                           kIntrinsified);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

static void SwapBits64(CpuRegister reg, CpuRegister temp, CpuRegister temp_mask,
                       int32_t shift, int64_t mask, X86_64Assembler* assembler) {
  Immediate imm_shift(shift);
  __ movq(temp_mask, Immediate(mask));
  __ movq(temp, reg);
  __ shrq(reg, imm_shift);
  __ andq(temp, temp_mask);
  __ andq(reg, temp_mask);
  __ shlq(temp, imm_shift);
  __ orq(reg, temp);
}

void IntrinsicCodeGeneratorX86_64::VisitLongReverse(HInvoke* invoke) {
  X86_64Assembler* assembler =
    reinterpret_cast<X86_64Assembler*>(codegen_->GetAssembler());
  LocationSummary* locations = invoke->GetLocations();

  CpuRegister reg = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister temp1 = locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister temp2 = locations->GetTemp(1).AsRegister<CpuRegister>();

  /*
   * Use one bswap instruction to reverse byte order first and then use 3 rounds of
   * swapping bits to reverse bits in a long number x. Using bswap to save instructions
   * compared to generic luni implementation which has 5 rounds of swapping bits.
   * x = bswap x
   * x = (x & 0x5555555555555555) << 1 | (x >> 1) & 0x5555555555555555;
   * x = (x & 0x3333333333333333) << 2 | (x >> 2) & 0x3333333333333333;
   * x = (x & 0x0F0F0F0F0F0F0F0F) << 4 | (x >> 4) & 0x0F0F0F0F0F0F0F0F;
   */
  __ bswapq(reg);
  SwapBits64(reg, temp1, temp2, 1, INT64_C(0x5555555555555555), assembler);
  SwapBits64(reg, temp1, temp2, 2, INT64_C(0x3333333333333333), assembler);
  SwapBits64(reg, temp1, temp2, 4, INT64_C(0x0f0f0f0f0f0f0f0f), assembler);
}

// Unimplemented intrinsics.

#define UNIMPLEMENTED_INTRINSIC(Name)                                                   \
void IntrinsicLocationsBuilderX86_64::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) { \
}                                                                                       \
void IntrinsicCodeGeneratorX86_64::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) {    \
}

UNIMPLEMENTED_INTRINSIC(StringIndexOf)
UNIMPLEMENTED_INTRINSIC(StringIndexOfAfter)
UNIMPLEMENTED_INTRINSIC(SystemArrayCopyChar)
UNIMPLEMENTED_INTRINSIC(ReferenceGetReferent)

}  // namespace x86_64
}  // namespace art
