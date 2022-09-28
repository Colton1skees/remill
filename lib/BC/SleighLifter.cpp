/*
 * Copyright (c) 2022-present Trail of Bits, Inc.
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
 */

#include <glog/logging.h>
#include <lib/Arch/Sleigh/Arch.h>
#include <lib/Arch/Sleigh/ControlFlowStructuring.h>
#include <remill/BC/ABI.h>
#include <remill/BC/IntrinsicTable.h>
#include <remill/BC/SleighLifter.h>
#include <remill/BC/Util.h>

#include <cassert>
#include <unordered_map>
#include <unordered_set>

namespace remill {


namespace {


void print_vardata(Sleigh &engine, std::stringstream &s, VarnodeData &data) {
  s << '(' << data.space->getName() << ',';
  data.space->printOffset(s, data.offset);
  s << ',' << dec << data.size << ')';
  auto maybe_name = engine.getRegisterName(data.space, data.offset, data.size);
  if (!maybe_name.empty()) {
    s << ":" << maybe_name;
  }
}
std::string DumpPcode(Sleigh &engine, const remill::sleigh::RemillPcodeOp &op) {
  std::stringstream ss;
  ss << get_opname(op.op);
  if (op.outvar) {
    auto ov = *op.outvar;
    print_vardata(engine, ss, ov);
    ss << " = ";
  }
  for (size_t i = 0; i < op.vars.size(); ++i) {
    auto iv = op.vars[i];
    print_vardata(engine, ss, iv);
  }
  return ss.str();
}

static size_t kBranchTakenArgNum = 2;
static size_t kNextPcArgNum = 3;

static bool isFloatOp(OpCode opc) {
  return opc >= OpCode::CPUI_FLOAT_EQUAL && opc <= OpCode::CPUI_FLOAT_ROUND;
}

static const std::string kEqualityClaimName = "claim_eq";

static bool isVarnodeInConstantSpace(VarnodeData vnode) {
  auto spc = vnode.getAddr().getSpace();
  return spc->constant_space_index == spc->getIndex();
}

static llvm::Value *ExtractOverflowBitFromCallToIntrinsic(
    llvm::Intrinsic::IndependentIntrinsics intrinsic, llvm::Value *lhs,
    llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
  llvm::Type *overloaded_types[1] = {lhs->getType()};
  llvm::Function *target_instrinsic = llvm::Intrinsic::getDeclaration(
      bldr.GetInsertBlock()->getModule(), intrinsic, overloaded_types);
  std::array<llvm::Value *, 2> intrinsic_args = {lhs, rhs};
  llvm::Value *res_val = bldr.CreateCall(target_instrinsic, intrinsic_args);
  // The value at index 1 is the overflow bit.
  return bldr.CreateExtractValue(res_val, {1});
}
}  // namespace

class SleighLifter::PcodeToLLVMEmitIntoBlock : public PcodeEmit {
 private:
  class Parameter {
   public:
    virtual ~Parameter(void) = default;

    virtual std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                                       llvm::Type *ty) = 0;

    virtual LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                                      llvm::Value *inner_lifted) = 0;
  };


  using ParamPtr = std::shared_ptr<Parameter>;


  class RegisterValue : public Parameter {
   private:
    llvm::Value *register_pointer;

   public:
    // TODO(Ian): allow this to be fallible and have better error handling
    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      return bldr.CreateLoad(ty, register_pointer);
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {
      bldr.CreateStore(inner_lifted, register_pointer);
      return LiftStatus::kLiftedInstruction;
    }

   public:
    RegisterValue(llvm::Value *register_pointer)
        : register_pointer(register_pointer) {}

    static ParamPtr CreatRegister(llvm::Value *register_pointer) {
      return std::make_shared<RegisterValue>(register_pointer);
    }

    virtual ~RegisterValue() {}
  };


  class Memory : public Parameter {
   public:
    virtual ~Memory() {}
    Memory(llvm::Value *memory_ref_ptr, llvm::Value *index,
           const IntrinsicTable *intrinsics, llvm::Type *memory_ptr_type)
        : memory_ref_ptr(memory_ref_ptr),
          index(index),
          intrinsics(intrinsics),
          memory_ptr_type(memory_ptr_type) {}

    static ParamPtr CreateMemory(llvm::Value *memory_ref_ptr,
                                 llvm::Value *index,
                                 const IntrinsicTable *intrinsics,
                                 llvm::Type *memory_ptr_type) {
      return std::make_shared<Memory>(memory_ref_ptr, index, intrinsics,
                                      memory_ptr_type);
    }

   private:
    llvm::Value *memory_ref_ptr;
    llvm::Value *index;
    const IntrinsicTable *intrinsics;
    llvm::Type *memory_ptr_type;

    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      auto mem = bldr.CreateLoad(this->memory_ptr_type, this->memory_ref_ptr);
      auto res = remill::LoadFromMemory(
          *this->intrinsics, bldr.GetInsertBlock(), ty, mem, this->index);
      if (res) {
        return res;
      } else {
        return std::nullopt;
      }
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {
      auto mem = bldr.CreateLoad(this->memory_ptr_type, this->memory_ref_ptr);
      auto new_mem =
          remill::StoreToMemory(*this->intrinsics, bldr.GetInsertBlock(),
                                inner_lifted, mem, this->index);
      if (new_mem) {
        bldr.CreateStore(new_mem, this->memory_ref_ptr);
        return LiftStatus::kLiftedInstruction;
      } else {
        return LiftStatus::kLiftedInvalidInstruction;
      }
    }
  };

  class ConstantValue : public Parameter {
   private:
    llvm::Value *cst;

   public:
    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      if (ty != cst->getType()) {
        return std::nullopt;
      }
      return this->cst;
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    ConstantValue(llvm::Value *cst) : cst(cst) {}

    static ParamPtr CreatConstant(llvm::Value *cst) {
      return std::make_shared<ConstantValue>(cst);
    }
    virtual ~ConstantValue() {}
  };


  llvm::BasicBlock *target_block;
  llvm::Value *state_pointer;
  llvm::LLVMContext &context;
  const Instruction &insn;
  LiftStatus status;
  SleighLifter &insn_lifter_parent;


  class UniqueRegSpace {
   private:
    std::unordered_map<uint64_t, llvm::Value *> cached_unique_ptrs;
    llvm::LLVMContext &context;

   public:
    UniqueRegSpace(llvm::LLVMContext &context) : context(context) {}

    llvm::Value *GetUniquePtr(uint64_t offset, uint64_t size,
                              llvm::IRBuilder<> &bldr) {
      if (this->cached_unique_ptrs.find(offset) !=
          this->cached_unique_ptrs.end()) {
        return this->cached_unique_ptrs.find(offset)->second;
      }

      std::stringstream ss;
      ss << "unique_" << std::hex << offset << ":" << std::dec << size;
      auto ptr =
          bldr.CreateAlloca(llvm::IntegerType::get(this->context, 8 * size), 0,
                            nullptr, ss.str());
      this->cached_unique_ptrs.insert({offset, ptr});
      return ptr;
    }
  };

  class ConstantReplacementContext {
   private:
    std::map<uint64_t, ParamPtr> current_replacements;
    std::set<uint64_t> used_values;

   public:
    void ApplyEqualityClaim(llvm::IRBuilder<> &bldr,
                            SleighLifter::PcodeToLLVMEmitIntoBlock &lifter,
                            VarnodeData lhs_constant,
                            VarnodeData rhs_unfolded_value) {
      assert(isVarnodeInConstantSpace(lhs_constant));
      this->current_replacements.insert(
          {lhs_constant.offset, lifter.LiftParamPtr(bldr, rhs_unfolded_value)});
    }


    void ApplyNonEqualityClaim() {
      this->current_replacements.clear();
      this->used_values.clear();
    }

    llvm::Value *LiftOffsetOrReplace(llvm::IRBuilder<> &bldr,
                                     VarnodeData target,
                                     llvm::Type *target_type) {
      if (this->current_replacements.find(target.offset) !=
          this->current_replacements.end()) {

        if (this->used_values.find(target.offset) != this->used_values.end()) {
          DLOG(ERROR) << "Ambigous value substitution via claim eq: "
                      << target.offset;
        }
        auto replacement = this->current_replacements.find(target.offset)
                               ->second->LiftAsInParam(bldr, target_type);
        if (!replacement.has_value()) {
          LOG(FATAL) << "Failure to lift replacement value for: "
                     << target.offset << " as "
                     << remill::LLVMThingToString(target_type);
        }
        this->used_values.insert(target.offset);
        return *replacement;
      }

      return llvm::ConstantInt::get(target_type, target.offset);
    }
  };

  UniqueRegSpace uniques;
  UniqueRegSpace unknown_regs;

  ConstantReplacementContext replacement_cont;
  // Generic sleigh arch
  std::vector<std::string> user_op_names;

  llvm::BasicBlock *exit_block;

  size_t curr_id;

  const std::optional<remill::sleigh::BranchTakenVar> &to_lift_btaken;

  void UpdateStatus(LiftStatus new_status, OpCode opc) {
    if (new_status != LiftStatus::kLiftedInstruction) {
      this->status = new_status;
      DLOG(ERROR) << "Failed to lift insn with opcode: " << get_opname(opc);
    }
  }

 public:
  PcodeToLLVMEmitIntoBlock(
      llvm::BasicBlock *target_block, llvm::Value *state_pointer,
      const Instruction &insn, SleighLifter &insn_lifter_parent,
      std::vector<std::string> user_op_names_, llvm::BasicBlock *exit_block_,
      const std::optional<remill::sleigh::BranchTakenVar> &to_lift_btaken_)
      : target_block(target_block),
        state_pointer(state_pointer),
        context(target_block->getContext()),
        insn(insn),
        status(remill::LiftStatus::kLiftedInstruction),
        insn_lifter_parent(insn_lifter_parent),
        uniques(target_block->getContext()),
        unknown_regs(target_block->getContext()),
        user_op_names(user_op_names_),
        exit_block(exit_block_),
        curr_id(0),
        to_lift_btaken(to_lift_btaken_) {}


  ParamPtr CreateMemoryAddress(llvm::Value *offset) {
    const auto mem_ptr_ref = this->insn_lifter_parent.LoadRegAddress(
        this->target_block, this->state_pointer, kMemoryVariableName);
    // compute pointer into memory at offset


    return Memory::CreateMemory(mem_ptr_ref.first, offset,
                                this->insn_lifter_parent.GetIntrinsicTable(),
                                this->insn_lifter_parent.GetMemoryType());
  }

  std::optional<ParamPtr> LiftNormalRegister(llvm::IRBuilder<> &bldr,
                                             std::string reg_name) {
    for (auto &c : reg_name) {
      c = toupper(c);
    }
    const auto &remappings =
        this->insn_lifter_parent.decoder.GetStateRegRemappings();

    if (auto el = remappings.find(reg_name); el != remappings.end()) {
      DLOG(INFO) << "Remapping to " << el->second;
      reg_name = el->second;
    }

    if (this->insn_lifter_parent.ArchHasRegByName(reg_name)) {
      // TODO(Ian): will probably need to adjust the pointer here in certain circumstances
      auto reg_ptr = this->insn_lifter_parent.LoadRegAddress(
          bldr.GetInsertBlock(), this->state_pointer, reg_name);
      return RegisterValue::CreatRegister(reg_ptr.first);
    } else {
      return std::nullopt;
    }
  }

  ParamPtr LiftNormalRegisterOrCreateUnique(llvm::IRBuilder<> &bldr,
                                            std::string reg_name,
                                            VarnodeData target_vnode) {
    if (auto res = this->LiftNormalRegister(bldr, reg_name)) {
      return *res;
    }

    std::stringstream ss;
    print_vardata(this->insn_lifter_parent.GetEngine(), ss, target_vnode);
    DLOG(ERROR) << "Creating unique for unkown register: " << ss.str();

    return RegisterValue::CreatRegister(this->unknown_regs.GetUniquePtr(
        target_vnode.offset, target_vnode.size, bldr));
  }

  //TODO(Ian): Maybe this should be a failable function that returns an unsupported insn in certain failures
  // So the times we need to replace an offset via a context are 3 fold.
  // 1. in Branches where the offset is retrieved directly from the varnode. This isnt handled here.
  // 2. In ram offsets
  // 3. In constant offsets
  ParamPtr LiftParamPtr(llvm::IRBuilder<> &bldr, VarnodeData vnode) {
    auto space_name = vnode.getAddr().getSpace()->getName();
    if (space_name == "ram") {
      // compute pointer into memory at offset

      auto constant_offset = this->replacement_cont.LiftOffsetOrReplace(
          bldr, vnode, this->insn_lifter_parent.GetWordType());

      return this->CreateMemoryAddress(constant_offset);
    } else if (space_name == "register") {
      auto reg_name = this->insn_lifter_parent.GetEngine().getRegisterName(
          vnode.space, vnode.offset, vnode.size);

      DLOG(INFO) << "Looking for reg name " << reg_name << " from offset "
                 << vnode.offset;
      return this->LiftNormalRegisterOrCreateUnique(bldr, reg_name, vnode);
    } else if (space_name == "const") {

      auto cst_v = this->replacement_cont.LiftOffsetOrReplace(
          bldr, vnode, llvm::IntegerType::get(this->context, vnode.size * 8));

      return ConstantValue::CreatConstant(cst_v);
    } else if (space_name == "unique") {
      auto reg_ptr = this->uniques.GetUniquePtr(vnode.offset, vnode.size, bldr);
      return RegisterValue::CreatRegister(reg_ptr);
    } else {
      LOG(FATAL) << "Unhandled memory space: " << space_name;
    }
  }


  llvm::Value *FixResultForOutVarnode(llvm::IRBuilder<> &bldr,
                                      llvm::Value *orig, VarnodeData outvnode) {
    CHECK(orig->getType()->isIntegerTy());
    auto out_bits = outvnode.size * 8;
    if (out_bits == orig->getType()->getIntegerBitWidth()) {
      return orig;
    }

    auto target_ty = llvm::IntegerType::get(bldr.getContext(), out_bits);

    return bldr.CreateZExtOrTrunc(orig, target_ty);
  }

  std::optional<llvm::Value *> LiftInParam(llvm::IRBuilder<> &bldr,
                                           VarnodeData vnode, llvm::Type *ty) {
    return this->LiftParamPtr(bldr, vnode)->LiftAsInParam(bldr, ty);
  }

  std::optional<llvm::Value *> LiftIntegerInParam(llvm::IRBuilder<> &bldr,
                                                  VarnodeData vnode) {
    return this->LiftInParam(
        bldr, vnode, llvm::IntegerType::get(this->context, vnode.size * 8));
  }

  LiftStatus
  LiftRequireOutParam(std::function<LiftStatus(VarnodeData)> inner_lift,
                      VarnodeData *outvar) {
    if (outvar) {
      return inner_lift(*outvar);
    } else {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }
  }


  LiftStatus LiftStoreIntoOutParam(llvm::IRBuilder<> &bldr,
                                   llvm::Value *inner_lifted,
                                   VarnodeData *outvar) {
    return this->LiftRequireOutParam(
        [&bldr, this, inner_lifted](VarnodeData out_param_data) {
          auto ptr = this->LiftParamPtr(bldr, out_param_data);
          return ptr->StoreIntoParam(bldr, inner_lifted);
        },
        outvar);
  }

  LiftStatus LiftUnaryOpWithFloatIntrinsic(
      llvm::IRBuilder<> &bldr,
      llvm::Intrinsic::IndependentIntrinsics intrinsic_id, VarnodeData *outvar,
      VarnodeData input_var) {
    auto inval = this->LiftInParam(bldr, input_var,
                                   llvm::Type::getFloatTy(this->context));

    if (!inval) {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    llvm::Function *intrinsic = llvm::Intrinsic::getDeclaration(
        bldr.GetInsertBlock()->getModule(), intrinsic_id);
    llvm::Value *intrinsic_args[] = {*inval};
    return this->LiftStoreIntoOutParam(
        bldr, bldr.CreateCall(intrinsic, intrinsic_args), outvar);
  }


  LiftStatus RedirectControlFlow(llvm::IRBuilder<> &bldr,
                                 llvm::Value *target_addr) {

    bldr.CreateStore(target_addr, this->GetNextPcRef());
    this->TerminateBlock();
    return LiftStatus::kLiftedInstruction;
  }

  LiftStatus LiftUnaryOp(llvm::IRBuilder<> &bldr, OpCode opc,
                         VarnodeData *outvar, VarnodeData input_var) {
    // TODO(Ian): when we lift a param we need to specify the type we want


    switch (opc) {
      case OpCode::CPUI_BOOL_NEGATE: {
        auto byte_type = llvm::IntegerType::get(this->context, 8);
        auto bneg_inval = this->LiftInParam(bldr, input_var, byte_type);
        ;
        if (bneg_inval.has_value()) {
          // TODO(Ian): is there a more optimization friendly way to get logical not on a byte?
          return this->LiftStoreIntoOutParam(
              bldr,
              bldr.CreateZExt(
                  bldr.CreateICmpEQ(*bneg_inval,
                                    llvm::ConstantInt::get(byte_type, 0)),
                  byte_type),
              outvar);
        }
        break;
      }
      case OpCode::CPUI_COPY:
      case OpCode::CPUI_CAST: {
        auto copy_inval = this->LiftInParam(
            bldr, input_var,
            llvm::IntegerType::get(this->context, input_var.size * 8));
        if (copy_inval.has_value()) {
          return this->LiftStoreIntoOutParam(bldr, *copy_inval, outvar);
        }
        break;
      }

      case OpCode::CPUI_BRANCH:
      case OpCode::CPUI_CALL: {
        // directs dont read the address of the variable, the offset is the jump
        // TODO(Ian): handle other address spaces
        if (isVarnodeInConstantSpace(input_var)) {
          DLOG(ERROR) << "Internal control flow not supported";
          return LiftStatus::kLiftedUnsupportedInstruction;
        }

        auto input_val = this->replacement_cont.LiftOffsetOrReplace(
            bldr, input_var,
            llvm::IntegerType::get(this->context, input_var.size * 8));

        return this->RedirectControlFlow(bldr, input_val);
      }
      case OpCode::CPUI_RETURN:
      case OpCode::CPUI_BRANCHIND:
      case OpCode::CPUI_CALLIND: {
        auto copy_inval = this->LiftInParam(
            bldr, input_var,
            llvm::IntegerType::get(this->context, input_var.size * 8));
        if (!copy_inval) {
          return LiftStatus::kLiftedUnsupportedInstruction;
        }
        return this->RedirectControlFlow(bldr, *copy_inval);
      }
        // TODO(alex): Maybe extract this into a method like `LiftIntegerUnOp`?
        // Let's see how much duplication there is.
      case OpCode::CPUI_INT_ZEXT:
      case OpCode::CPUI_INT_SEXT: {
        auto zext_inval = this->LiftIntegerInParam(bldr, input_var);
        if (zext_inval.has_value()) {
          auto *zext_type =
              llvm::IntegerType::get(this->context, outvar->size * 8);
          auto *zext_op = (opc == OpCode::CPUI_INT_ZEXT)
                              ? bldr.CreateZExt(*zext_inval, zext_type)
                              : bldr.CreateSExt(*zext_inval, zext_type);
          return this->LiftStoreIntoOutParam(bldr, zext_op, outvar);
        }
        break;
      }
      case OpCode::CPUI_INT_2COMP: {
        auto two_comp_inval = this->LiftIntegerInParam(bldr, input_var);
        if (two_comp_inval.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr, bldr.CreateNeg(*two_comp_inval), outvar);
        }
        break;
      }
      case OpCode::CPUI_INT_NEGATE: {
        auto negate_inval = this->LiftIntegerInParam(bldr, input_var);
        if (negate_inval.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr, bldr.CreateNot(*negate_inval), outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_NEG: {
        auto negate_inval = this->LiftInParam(
            bldr, input_var, llvm::Type::getFloatTy(this->context));
        if (negate_inval.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr, bldr.CreateFNeg(*negate_inval), outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_ABS: {
        return this->LiftUnaryOpWithFloatIntrinsic(bldr, llvm::Intrinsic::fabs,
                                                   outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_SQRT: {
        return this->LiftUnaryOpWithFloatIntrinsic(bldr, llvm::Intrinsic::sqrt,
                                                   outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_CEIL: {
        return this->LiftUnaryOpWithFloatIntrinsic(bldr, llvm::Intrinsic::ceil,
                                                   outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_FLOOR: {
        return this->LiftUnaryOpWithFloatIntrinsic(bldr, llvm::Intrinsic::floor,
                                                   outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_ROUND: {
        return this->LiftUnaryOpWithFloatIntrinsic(bldr, llvm::Intrinsic::round,
                                                   outvar, input_var);
      }
      case OpCode::CPUI_FLOAT_NAN: {
        auto nan_inval = this->LiftInParam(
            bldr, input_var, llvm::Type::getFloatTy(this->context));
        if (nan_inval.has_value()) {
          // LLVM trunk has an `isnan` intrinsic but to support older versions, I think we need to do this.
          auto *isnan_check = bldr.CreateZExt(
              bldr.CreateNot(bldr.CreateFCmpOEQ(*nan_inval, *nan_inval)),
              llvm::IntegerType::get(this->context, outvar->size * 8));
          return this->LiftStoreIntoOutParam(bldr, isnan_check, outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_INT2FLOAT: {
        auto int2float_inval = this->LiftIntegerInParam(bldr, input_var);
        if (int2float_inval.has_value()) {
          auto *converted = bldr.CreateSIToFP(
              *int2float_inval, llvm::Type::getFloatTy(this->context));
          return this->LiftStoreIntoOutParam(bldr, converted, outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_FLOAT2FLOAT: {
        auto float2float_inval = this->LiftInParam(
            bldr, input_var, llvm::Type::getFloatTy(this->context));
        if (float2float_inval.has_value()) {
          // This is a no-op until we make a helper to select an appropriate float type for a given node size.
          return this->LiftStoreIntoOutParam(
              bldr,
              bldr.CreateFPTrunc(*float2float_inval,
                                 llvm::Type::getFloatTy(this->context)),
              outvar);
        }
        break;
      }
      case OpCode::CPUI_FLOAT_TRUNC: {
        auto trunc_inval = this->LiftInParam(
            bldr, input_var, llvm::Type::getFloatTy(this->context));
        if (trunc_inval.has_value()) {
          auto *converted = bldr.CreateFPToSI(
              *trunc_inval,
              llvm::IntegerType::get(this->context, outvar->size * 8));
          return this->LiftStoreIntoOutParam(bldr, converted, outvar);
        }
        break;
      }
      case OpCode::CPUI_POPCOUNT: {
        auto ctpop_inval = this->LiftIntegerInParam(bldr, input_var);
        if (ctpop_inval.has_value()) {
          llvm::Type *overloaded_types[1] = {(*ctpop_inval)->getType()};
          llvm::Function *ctpop_intrinsic = llvm::Intrinsic::getDeclaration(
              bldr.GetInsertBlock()->getModule(), llvm::Intrinsic::ctpop,
              overloaded_types);

          std::array<llvm::Value *, 1> ctpop_args = {*ctpop_inval};
          llvm::Value *ctpop_val = this->FixResultForOutVarnode(
              bldr, bldr.CreateCall(ctpop_intrinsic, ctpop_args), *outvar);


          return this->LiftStoreIntoOutParam(bldr, ctpop_val, outvar);
        }
        break;
      }
      default: break;
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }
  using BinaryOperator = std::function<llvm::Value *(
      llvm::Value *, llvm::Value *, llvm::IRBuilder<> &)>;
  static std::map<OpCode, BinaryOperator> INTEGER_BINARY_OPS;
  static std::map<OpCode, BinaryOperator> BOOL_BINARY_OPS;
  static std::unordered_set<OpCode> INTEGER_COMP_OPS;


  LiftStatus TerminateBlockWithCondition(llvm::Value *condition) {
    llvm::IRBuilder<> ir(this->target_block);
    this->target_block = llvm::BasicBlock::Create(
        this->context, "continuation", this->target_block->getParent());
    ir.CreateCondBr(condition, this->exit_block, this->target_block);
    return LiftStatus::kLiftedInstruction;
  }

  void TerminateBlock() {
    if (this->target_block->getTerminator() == nullptr) {
      llvm::IRBuilder ir(this->target_block);
      ir.CreateBr(this->exit_block);
    }
  }


  LiftStatus LiftCBranch(llvm::IRBuilder<> &bldr, VarnodeData *outvar,
                         VarnodeData lhs, VarnodeData rhs) {
    auto should_branch = this->LiftInParam(
        bldr, rhs, llvm::IntegerType::get(this->context, rhs.size * 8));


    if (!should_branch) {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    if (isVarnodeInConstantSpace(lhs)) {
      DLOG(ERROR) << "Internal control flow not supported";
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    // directs dont read the address of the variable, the offset is the jump
    // TODO(Ian): handle other address spaces
    auto jump_addr = this->replacement_cont.LiftOffsetOrReplace(
        bldr, lhs, llvm::IntegerType::get(this->context, lhs.size * 8));

    // TODO(Ian): this should probably technically be != 0
    auto trunc_should_branch = bldr.CreateTrunc(
        *should_branch, llvm::IntegerType::get(this->context, 1));


    auto pc_reg_param = this->LiftNormalRegister(bldr, "PC");
    assert(pc_reg_param.has_value());
    auto pc_reg_ptr = *pc_reg_param;
    auto orig_pc_value =
        pc_reg_ptr->LiftAsInParam(bldr, this->insn_lifter_parent.GetWordType());

    if (orig_pc_value.has_value()) {
      auto next_pc_value =
          bldr.CreateSelect(trunc_should_branch, jump_addr, *orig_pc_value);

      bldr.CreateStore(next_pc_value, this->GetNextPcRef());
    }

    return this->TerminateBlockWithCondition(trunc_should_branch);
  }

  LiftStatus LiftIntegerBinOp(llvm::IRBuilder<> &bldr, OpCode opc,
                              VarnodeData *outvar, VarnodeData lhs,
                              VarnodeData rhs) {


    if (INTEGER_BINARY_OPS.find(opc) != INTEGER_BINARY_OPS.end()) {
      auto &op_func = INTEGER_BINARY_OPS.find(opc)->second;
      auto lifted_lhs = this->LiftIntegerInParam(bldr, lhs);
      auto lifted_rhs = this->LiftIntegerInParam(bldr, rhs);
      if (lifted_lhs.has_value() && lifted_rhs.has_value()) {
        DLOG(INFO) << "Binop with lhs: "
                   << remill::LLVMThingToString(*lifted_lhs);
        DLOG(INFO) << "Binop with rhs: "
                   << remill::LLVMThingToString(*lifted_rhs);
        auto orig_res = op_func(*lifted_lhs, *lifted_rhs, bldr);
        if (INTEGER_COMP_OPS.find(opc) != INTEGER_COMP_OPS.end()) {
          // Comparison operators always return a byte
          if (orig_res->getType()->getIntegerBitWidth() != 8) {
            orig_res = bldr.CreateZExt(
                orig_res, llvm::IntegerType::get(bldr.getContext(), 8));
          }
        }
        DLOG(INFO) << "Res: " << remill::LLVMThingToString(orig_res);
        DLOG(INFO) << "Res ty: "
                   << remill::LLVMThingToString(orig_res->getType());
        return this->LiftStoreIntoOutParam(bldr, orig_res, outvar);
      }
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }


  LiftStatus LiftBoolBinOp(llvm::IRBuilder<> &bldr, OpCode opc,
                           VarnodeData *outvar, VarnodeData lhs,
                           VarnodeData rhs) {

    // We make sure to only attempt to lift params for operands where we know they are booleans
    // Otherwise lifting a value as a byte could be an incorrect size for something like a unique.
    if (this->BOOL_BINARY_OPS.find(opc) == this->BOOL_BINARY_OPS.end()) {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    auto lifted_lhs =
        this->LiftInParam(bldr, lhs, llvm::IntegerType::get(this->context, 8));
    auto lifted_rhs =
        this->LiftInParam(bldr, rhs, llvm::IntegerType::get(this->context, 8));
    if (!lifted_lhs.has_value() || !lifted_rhs.has_value()) {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    auto computed_value =
        this->BOOL_BINARY_OPS.find(opc)->second(*lifted_lhs, *lifted_rhs, bldr);

    return this->LiftStoreIntoOutParam(bldr, computed_value, outvar);
  }

  std::optional<BinaryOperator> FindFloatBinOpFunc(OpCode opc) {
    switch (opc) {
      case CPUI_FLOAT_EQUAL: {
        return [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
          return bldr.CreateZExt(bldr.CreateFCmpOEQ(lhs, rhs),
                                 llvm::IntegerType::get(bldr.getContext(), 8));
        };
      }
      case CPUI_FLOAT_NOTEQUAL: {
        return [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
          return bldr.CreateZExt(bldr.CreateFCmpONE(lhs, rhs),
                                 llvm::IntegerType::get(bldr.getContext(), 8));
        };
      }
      case CPUI_FLOAT_LESS: {
        return [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
          return bldr.CreateZExt(bldr.CreateFCmpOLT(lhs, rhs),
                                 llvm::IntegerType::get(bldr.getContext(), 8));
        };
      }
      case CPUI_FLOAT_LESSEQUAL: {
        return [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
          return bldr.CreateZExt(bldr.CreateFCmpOLE(lhs, rhs),
                                 llvm::IntegerType::get(bldr.getContext(), 8));
        };
      }
      case CPUI_FLOAT_ADD: {
        return [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
          return bldr.CreateFAdd(lhs, rhs);
        };
      }
      case CPUI_FLOAT_SUB: {
        return [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
          return bldr.CreateFSub(lhs, rhs);
        };
      }
      case CPUI_FLOAT_MULT: {
        return [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
          return bldr.CreateFMul(lhs, rhs);
        };
      }
      case CPUI_FLOAT_DIV: {
        return [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
          return bldr.CreateFDiv(lhs, rhs);
        };
      }
      default: return std::nullopt;
    }
  }

  std::optional<llvm::Value *> LiftFloatInParam(llvm::IRBuilder<> &bldr,
                                                VarnodeData vnode) {
    return this->LiftInParam(bldr, vnode,
                             llvm::Type::getFloatTy(this->context));
  }

  LiftStatus LiftFloatBinOp(llvm::IRBuilder<> &bldr, OpCode opc,
                            VarnodeData *outvar, VarnodeData lhs,
                            VarnodeData rhs) {
    std::optional<BinaryOperator> op_func = this->FindFloatBinOpFunc(opc);
    if (!op_func) {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }


    // TODO(alex): I think we need some helper here to achieve something similar to what `llvm::IntegerType::get`
    // gives us, except for floating point types.
    //
    // So we need to check the size of the node and return either a 32-bit float, brain float, double, etc.

    auto lifted_lhs = this->LiftFloatInParam(bldr, lhs);
    auto lifted_rhs = this->LiftFloatInParam(bldr, rhs);

    if (!lifted_lhs || !lifted_rhs) {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    return this->LiftStoreIntoOutParam(
        bldr, (*op_func)(*lifted_lhs, *lifted_rhs, bldr), outvar);
  }


  LiftStatus LiftBinOp(llvm::IRBuilder<> &bldr, OpCode opc, VarnodeData *outvar,
                       VarnodeData lhs, VarnodeData rhs) {

    if (opc == OpCode::CPUI_CBRANCH) {
      return this->LiftCBranch(bldr, outvar, lhs, rhs);
    }

    auto res = this->LiftIntegerBinOp(bldr, opc, outvar, lhs, rhs);
    if (res == LiftStatus::kLiftedInstruction) {
      return res;
    }

    auto sres = this->LiftBoolBinOp(bldr, opc, outvar, lhs, rhs);
    if (sres == LiftStatus::kLiftedInstruction) {
      return sres;
    }

    sres = this->LiftFloatBinOp(bldr, opc, outvar, lhs, rhs);
    if (sres == LiftStatus::kLiftedInstruction) {
      return sres;
    }

    if (opc == OpCode::CPUI_LOAD && outvar) {
      auto out_op = *outvar;
      auto addr_operand = rhs;
      auto lifted_addr_offset = this->LiftInParam(
          bldr, addr_operand, this->insn_lifter_parent.GetWordType());

      if (!lifted_addr_offset) {
        return LiftStatus::kLiftedUnsupportedInstruction;
      }
      auto out_type = llvm::IntegerType::get(this->context, out_op.size * 8);
      auto lifted_addr = this->CreateMemoryAddress(*lifted_addr_offset);

      auto loaded_value = lifted_addr->LiftAsInParam(bldr, out_type);

      if (!loaded_value) {
        return LiftStatus::kLiftedUnsupportedInstruction;
      }


      auto lifted_out = this->LiftParamPtr(bldr, out_op);
      return lifted_out->StoreIntoParam(bldr, *loaded_value);
    }

    if (opc == OpCode::CPUI_PIECE && outvar) {
      assert(rhs.size + lhs.size == outvar->size);

      // Treat them as integers
      auto lifted_lhs = this->LiftInParam(
          bldr, lhs, llvm::IntegerType::get(this->context, lhs.size * 8));
      auto lifted_rhs = this->LiftInParam(
          bldr, rhs, llvm::IntegerType::get(this->context, rhs.size * 8));

      if (lifted_lhs.has_value() && lifted_rhs.has_value()) {
        // Widen the most significant operand and then left shift it to make room for the least significant operand.
        auto *ms_operand = bldr.CreateZExt(
            *lifted_lhs, llvm::IntegerType::get(this->context, outvar->size));
        auto *shifted_ms_operand = bldr.CreateShl(
            ms_operand, llvm::ConstantInt::get(
                            llvm::Type::getInt8Ty(this->context), rhs.size));

        // Now concatenate them with an OR.
        auto *concat = bldr.CreateOr(shifted_ms_operand, *lifted_rhs);
        return this->LiftStoreIntoOutParam(bldr, concat, outvar);
      }
    }

    if (opc == OpCode::CPUI_SUBPIECE && outvar) {
      auto lifted_lhs = this->LiftInParam(
          bldr, lhs, llvm::IntegerType::get(this->context, lhs.size * 8));

      if (lifted_lhs.has_value()) {
        DLOG(INFO) << "SUBPIECE: " << remill::LLVMThingToString(*lifted_lhs);
        auto new_size = lhs.size - rhs.offset;
        auto *subpiece_lhs = bldr.CreateTrunc(
            *lifted_lhs, llvm::IntegerType::get(this->context, new_size * 8));

        if (new_size < outvar->size) {
          subpiece_lhs = bldr.CreateZExt(
              subpiece_lhs,
              llvm::IntegerType::get(this->context, 8 * outvar->size));
        } else if (new_size > outvar->size) {
          subpiece_lhs = bldr.CreateTrunc(
              subpiece_lhs,
              llvm::IntegerType::get(this->context, 8 * outvar->size));
        }

        return this->LiftStoreIntoOutParam(bldr, subpiece_lhs, outvar);
      }
    }

    if (opc == OpCode::CPUI_INDIRECT && outvar) {
      // TODO(alex): This isn't clear to me from the documentation.
      // I'll probably need to find some code that generates this op in order to understand how to handle it.
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    if (opc == OpCode::CPUI_NEW && outvar) {
      // NOTE(alex): We shouldn't encounter this op as it only get generated when lifting Java or
      // Dalvik bytecode
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    return LiftStatus::kLiftedUnsupportedInstruction;
  }

  LiftStatus LiftThreeOperandOp(llvm::IRBuilder<> &bldr, OpCode opc,
                                VarnodeData *outvar, VarnodeData param0,
                                VarnodeData param1, VarnodeData param2) {
    switch (opc) {
      case OpCode::CPUI_STORE: {
        auto addr_operand = param1;
        auto lifted_addr_offset = this->LiftInParam(
            bldr, addr_operand, this->insn_lifter_parent.GetWordType());

        if (lifted_addr_offset) {
          auto store_param = this->LiftInParam(
              bldr, param2,
              llvm::IntegerType::get(this->context, param2.size * 8));

          if (store_param.has_value()) {
            auto lifted_addr = this->CreateMemoryAddress(*lifted_addr_offset);
            return lifted_addr->StoreIntoParam(bldr, *store_param);
          }
        }
        break;
      }
      case OpCode::CPUI_PTRADD: {
        auto lifted_addr = this->LiftInParam(
                 bldr, param0, this->insn_lifter_parent.GetWordType()),
             lifted_index = this->LiftIntegerInParam(bldr, param1);
        auto *elem_size = llvm::ConstantInt::get(
            llvm::IntegerType::get(this->context, param2.size * 8),
            param2.offset);
        if (lifted_addr.has_value() && lifted_index.has_value()) {
          auto *offset = bldr.CreateMul(*lifted_index, elem_size),
               *ptr_add = bldr.CreateAdd(*lifted_addr, offset);
          return this->LiftStoreIntoOutParam(bldr, ptr_add, outvar);
        }
        break;
      }
      case OpCode::CPUI_PTRSUB: {
        auto lifted_addr = this->LiftInParam(
                 bldr, param0, this->insn_lifter_parent.GetWordType()),
             lifted_offset = this->LiftIntegerInParam(bldr, param1);
        if (lifted_addr.has_value() && lifted_offset.has_value()) {
          return this->LiftStoreIntoOutParam(
              bldr, bldr.CreateAdd(*lifted_addr, *lifted_offset), outvar);
        }
        break;
      }
      default: break;
    }

    return LiftStatus::kLiftedUnsupportedInstruction;
  }

  LiftStatus LiftVariadicOp(llvm::IRBuilder<> &bldr, OpCode opc,
                            VarnodeData *outvar, VarnodeData *vars,
                            int4 isize) {
    switch (opc) {
      // We shouldnt encounter this afaik MULTIEQUAL is a decompiler concept?
      case OpCode::CPUI_MULTIEQUAL: {
        llvm::Type *phi_type =
            llvm::IntegerType::get(this->context, vars[0].size * 8);
        llvm::PHINode *phi_node = bldr.CreatePHI(phi_type, isize);
        for (int4 i = 0; i < isize; ++i) {
          VarnodeData &var = vars[i];
          auto inval = this->LiftInParam(
              bldr, var, llvm::IntegerType::get(this->context, var.size * 8));
          if (!inval.has_value()) {
            return LiftStatus::kLiftedUnsupportedInstruction;
          }
          // TODO(alex): This isn't right, just using the current block to get things building.
          // We need to track the incoming basic blocks for each value.
          phi_node->addIncoming(*inval, bldr.GetInsertBlock());
        }
        return this->LiftStoreIntoOutParam(bldr, phi_node, outvar);
      }
      case OpCode::CPUI_CPOOLREF: {
        // NOTE(alex): We shouldn't encounter this op as it only get generated when lifting Java or
        // Dalvik bytecode
        return LiftStatus::kLiftedUnsupportedInstruction;
      }
      default: break;
    }

    return LiftStatus::kLiftedUnsupportedInstruction;
  }


  std::optional<std::string> GetOtherFuncName(VarnodeData *ivars, int4 isize) {
    if (isize < 1 || ivars[0].offset >= this->user_op_names.size()) {
      return std::nullopt;
    }

    return this->user_op_names[ivars[0].offset];
  }

  static const size_t kEqualityClaimArity = 3;
  LiftStatus HandleCallOther(llvm::IRBuilder<> &bldr, VarnodeData *outvar,
                             VarnodeData *vars, int4 isize) {
    auto other_func_name = this->GetOtherFuncName(vars, isize);
    if (other_func_name.has_value()) {
      if (other_func_name == kEqualityClaimName &&
          isize == kEqualityClaimArity) {
        DLOG(INFO) << "Applying eq claim";
        this->replacement_cont.ApplyEqualityClaim(bldr, *this, vars[1],
                                                  vars[2]);
        return kLiftedInstruction;
      }
      DLOG(ERROR) << "Unsupported pcode intrinsic: " << *other_func_name;
    }
    return kLiftedUnsupportedInstruction;
  }

  llvm::Argument *GetBranchTakenRef() {
    return this->exit_block->getParent()->getArg(kBranchTakenArgNum);
  }

  llvm::Argument *GetNextPcRef() {
    return this->exit_block->getParent()->getArg(kNextPcArgNum);
  }

  LiftStatus LiftBranchTaken(llvm::IRBuilder<> &bldr,
                             const sleigh::BranchTakenVar &btaken_var) {


    auto maybe_should_branch =
        this->LiftIntegerInParam(bldr, btaken_var.target_vnode);
    if (!maybe_should_branch) {
      DLOG(ERROR) << "Failed to lift iparam branch taken var";
      return LiftStatus::kLiftedLifterError;
    }

    auto should_branch = bldr.CreateZExtOrTrunc(
        *maybe_should_branch, llvm::IntegerType::get(this->context, 8));
    auto branch_taken_ref = this->GetBranchTakenRef();
    bldr.CreateStore(should_branch, branch_taken_ref);
    return LiftStatus::kLiftedInstruction;
  }


  void LiftBtakenIfReached(llvm::IRBuilder<> &bldr, OpCode opc) {

    if (this->to_lift_btaken && curr_id == this->to_lift_btaken->index) {
      this->UpdateStatus(this->LiftBranchTaken(bldr, *this->to_lift_btaken),
                         opc);
    }
  }

  void LiftPcodeOp(llvm::IRBuilder<> &bldr, OpCode opc, VarnodeData *outvar,
                   VarnodeData *vars, int4 isize) {
    // The MULTIEQUAL op has variadic operands
    if (opc == OpCode::CPUI_MULTIEQUAL || opc == OpCode::CPUI_CPOOLREF) {
      this->UpdateStatus(this->LiftVariadicOp(bldr, opc, outvar, vars, isize),
                         opc);
      return;
    }

    if (opc == OpCode::CPUI_CALLOTHER) {
      this->UpdateStatus(this->HandleCallOther(bldr, outvar, vars, isize), opc);
      return;
    }

    switch (isize) {
      case 1: {
        this->UpdateStatus(this->LiftUnaryOp(bldr, opc, outvar, vars[0]), opc);
        break;
      }
      case 2: {
        this->UpdateStatus(this->LiftBinOp(bldr, opc, outvar, vars[0], vars[1]),
                           opc);
        return;
      }
      case 3: {
        this->UpdateStatus(this->LiftThreeOperandOp(bldr, opc, outvar, vars[0],
                                                    vars[1], vars[2]),
                           opc);

        return;
      }
      default:
        this->UpdateStatus(LiftStatus::kLiftedUnsupportedInstruction, opc);
        return;
    }
  }

  virtual void dump(const Address &addr, OpCode opc, VarnodeData *outvar,
                    VarnodeData *vars, int4 isize) final override {
    llvm::IRBuilder bldr(this->target_block);
    this->LiftBtakenIfReached(bldr, opc);
    this->LiftPcodeOp(bldr, opc, outvar, vars, isize);
    this->curr_id++;
  }

  LiftStatus GetStatus() {
    return this->status;
  }
};  // namespace remill

std::unordered_set<OpCode>
    SleighLifter::PcodeToLLVMEmitIntoBlock::INTEGER_COMP_OPS = {
        CPUI_INT_EQUAL,   CPUI_INT_NOTEQUAL,  CPUI_INT_LESS,
        CPUI_INT_SLESS,   CPUI_INT_LESSEQUAL, CPUI_INT_SLESSEQUAL,
        CPUI_INT_SBORROW, CPUI_INT_SCARRY,    CPUI_INT_CARRY};

// NOTE(Ian): we store a mapping from pcode op to supported boolean operation so that we can easily check if
// we want to lift the operands to this op as a boolean and also find the right post lifting operation to apply.
std::map<OpCode, SleighLifter::PcodeToLLVMEmitIntoBlock::BinaryOperator>
    SleighLifter::PcodeToLLVMEmitIntoBlock::BOOL_BINARY_OPS = {
        {OpCode::CPUI_BOOL_AND,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateAnd(lhs, rhs);
         }},
        {OpCode::CPUI_BOOL_OR,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateOr(lhs, rhs);
         }},
        {OpCode::CPUI_BOOL_XOR,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateXor(lhs, rhs);
         }}};
std::map<OpCode, SleighLifter::PcodeToLLVMEmitIntoBlock::BinaryOperator>
    SleighLifter::PcodeToLLVMEmitIntoBlock::INTEGER_BINARY_OPS = {
        {OpCode::CPUI_INT_AND,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateAnd(lhs, rhs);
         }},
        {OpCode::CPUI_INT_OR,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateOr(lhs, rhs);
         }},
        {OpCode::CPUI_INT_XOR,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateXor(lhs, rhs);
         }},
        {OpCode::CPUI_INT_LEFT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           if (lhs->getType() != rhs->getType()) {
             rhs = bldr.CreateZExtOrTrunc(rhs, lhs->getType());
           }
           return bldr.CreateShl(lhs, rhs);
         }},
        {OpCode::CPUI_INT_RIGHT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           if (lhs->getType() != rhs->getType()) {
             rhs = bldr.CreateZExtOrTrunc(rhs, lhs->getType());
           }
           return bldr.CreateLShr(lhs, rhs);
         }},
        {OpCode::CPUI_INT_SRIGHT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           if (lhs->getType() != rhs->getType()) {
             rhs = bldr.CreateZExtOrTrunc(rhs, lhs->getType());
           }
           return bldr.CreateAShr(lhs, rhs);
         }},
        {OpCode::CPUI_INT_ADD,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateAdd(lhs, rhs);
         }},
        {OpCode::CPUI_INT_SUB,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateSub(lhs, rhs);
         }},
        {OpCode::CPUI_INT_MULT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateMul(lhs, rhs);
         }},
        {OpCode::CPUI_INT_DIV,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateUDiv(lhs, rhs);
         }},
        {OpCode::CPUI_INT_SDIV,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateSDiv(lhs, rhs);
         }},
        {OpCode::CPUI_INT_REM,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateURem(lhs, rhs);
         }},
        {OpCode::CPUI_INT_SREM,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateSRem(lhs, rhs);
         }},
        {OpCode::CPUI_INT_EQUAL,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           // TODO(alex): Should these by using `trunc`?
           // The docs seem to indicate that it's not ok to `zext` to a smaller type.
           return bldr.CreateZExt(bldr.CreateICmpEQ(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_NOTEQUAL,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpNE(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_LESS,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpULT(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_SLESS,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpSLT(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_LESSEQUAL,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpSLE(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_SLESSEQUAL,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateZExt(bldr.CreateICmpULE(lhs, rhs),
                                  llvm::IntegerType::get(bldr.getContext(), 8));
         }},
        {OpCode::CPUI_INT_CARRY,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return ExtractOverflowBitFromCallToIntrinsic(
               llvm::Intrinsic::uadd_with_overflow, lhs, rhs, bldr);
         }},
        {OpCode::CPUI_INT_SCARRY,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return ExtractOverflowBitFromCallToIntrinsic(
               llvm::Intrinsic::sadd_with_overflow, lhs, rhs, bldr);
         }},
        {OpCode::CPUI_INT_SBORROW,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return ExtractOverflowBitFromCallToIntrinsic(
               llvm::Intrinsic::ssub_with_overflow, lhs, rhs, bldr);
         }},
};

SleighLifter::SleighLifter(const remill::Arch &arch_,
                           const remill::sleigh::SleighDecoder &dec_,
                           const IntrinsicTable &intrinsics_)
    : InstructionLifter(&arch_, intrinsics_),
      sleigh_context(new sleigh::SingleInstructionSleighContext(
          dec_.GetSLAName(), dec_.GetPSpec())),
      decoder(dec_) {
  this->decoder.InitializeSleighContext(*sleigh_context);
}


const std::string_view SleighLifter::kInstructionFunctionPrefix =
    "sleigh_remill_instruction_function";

void SleighLifter::SetISelAttributes(llvm::Function *target_func) {
  target_func->setLinkage(llvm::GlobalValue::InternalLinkage);
  target_func->removeFnAttr(llvm::Attribute::NoInline);
  target_func->addFnAttr(llvm::Attribute::InlineHint);
  target_func->addFnAttr(llvm::Attribute::AlwaysInline);
}


llvm::Function *
SleighLifter::DefineInstructionFunction(Instruction &inst,
                                        llvm::Module *target_mod) {

  std::stringstream nm;
  nm << SleighLifter::kInstructionFunctionPrefix << "_" << std::hex << inst.pc;
  auto &context = target_mod->getContext();
  auto ptr_ty = llvm::PointerType::get(context, 0);
  std::array<llvm::Type *, 4> params = {inst.arch->StatePointerType(),
                                        inst.arch->MemoryPointerType(), ptr_ty,
                                        ptr_ty};
  auto ty =
      llvm::FunctionType::get(inst.arch->MemoryPointerType(), params, false);
  auto func = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, 0,
                                     nm.str(), target_mod);

  auto memory = remill::NthArgument(func, 1);
  auto state = remill::NthArgument(func, 0);
  memory->setName("memory");
  state->setName("state");
  func->getArg(kBranchTakenArgNum)->setName("btaken");
  func->getArg(kNextPcArgNum)->setName("npc");
  auto block = llvm::BasicBlock::Create(context, "entry_block", func);
  llvm::IRBuilder<> ir(block);

  ir.CreateStore(memory, ir.CreateAlloca(memory->getType(), nullptr, "MEMORY"));

  return func;
}

std::pair<LiftStatus, std::optional<llvm::Function *>>
SleighLifter::LiftIntoInternalBlockWithSleighState(
    Instruction &inst, llvm::Module *target_mod, bool is_delayed,
    const std::optional<sleigh::BranchTakenVar> &btaken) {

  this->sleigh_context->resetContext();
  this->decoder.InitializeSleighContext(*this->sleigh_context);

  sleigh::PcodeDecoder pcode_record(this->GetEngine());
  sleigh_context->oneInstruction(inst.pc, pcode_record, inst.bytes);
  for (const auto &op : pcode_record.ops) {
    DLOG(INFO) << "Pcodeop: " << DumpPcode(this->GetEngine(), op);

    if (isFloatOp(op.op)) {
      return {LiftStatus::kLiftedUnsupportedInstruction, std::nullopt};
    }
  }

  DLOG(INFO) << "Secondary lift of bytes: " << llvm::toHex(inst.bytes);
  auto target_func = this->DefineInstructionFunction(inst, target_mod);


  llvm::BasicBlock *target_block = &target_func->getEntryBlock();
  llvm::IRBuilder<> ir(target_block);
  auto internal_state_pointer =
      remill::NthArgument(target_func, kStatePointerArgNum);


  auto exit_block = llvm::BasicBlock::Create(target_mod->getContext(),
                                             "exit_block", target_func);

  llvm::IRBuilder<> exit_builder(exit_block);


  exit_builder.CreateRet(remill::LoadMemoryPointer(
      exit_builder.GetInsertBlock(), *this->GetIntrinsicTable()));


  //TODO(Ian): make a safe to use sleighinstruction context that wraps a context with an arch to preform reset reinits


  SleighLifter::PcodeToLLVMEmitIntoBlock lifter(
      target_block, internal_state_pointer, inst, *this,
      this->sleigh_context->getUserOpNames(), exit_block, btaken);

  sleigh_context->oneInstruction(inst.pc, lifter, inst.bytes);


  lifter.TerminateBlock();

  // Setup like an ISEL
  SleighLifter::SetISelAttributes(target_func);
  remill::InitFunctionAttributes(target_func);

  return {lifter.GetStatus(), target_func};
}

LiftStatus SleighLifter::LiftIntoBlockWithSleighState(
    Instruction &inst, llvm::BasicBlock *block, llvm::Value *state_ptr,
    bool is_delayed, const std::optional<sleigh::BranchTakenVar> &btaken) {
  if (!inst.IsValid()) {
    DLOG(ERROR) << "Invalid function" << inst.Serialize();
    return kLiftedInvalidInstruction;
  }


  // Call the instruction function
  auto res = this->LiftIntoInternalBlockWithSleighState(
      inst, block->getModule(), is_delayed, btaken);

  if (res.first != LiftStatus::kLiftedInstruction || !res.second.has_value()) {
    return res.first;
  }

  auto target_func = *res.second;


  // Setup PC and NEXT_PC
  const auto [pc_ref, pc_ref_type] =
      LoadRegAddress(block, state_ptr, kPCVariableName);
  const auto [next_pc_ref, next_pc_ref_type] =
      LoadRegAddress(block, state_ptr, kNextPCVariableName);

  llvm::IRBuilder<> intoblock_builer(block);
  const auto next_pc =
      intoblock_builer.CreateLoad(this->GetWordType(), next_pc_ref);

  ;

  intoblock_builer.CreateStore(
      this->decoder.LiftPcFromCurrPc(intoblock_builer, next_pc,
                                     inst.bytes.size()),
      pc_ref);
  intoblock_builer.CreateStore(
      intoblock_builer.CreateAdd(
          next_pc,
          llvm::ConstantInt::get(this->GetWordType(), inst.bytes.size())),
      next_pc_ref);


  std::array<llvm::Value *, 4> args = {
      state_ptr, remill::LoadMemoryPointer(block, *this->GetIntrinsicTable()),
      remill::LoadBranchTakenRef(block),
      remill::LoadNextProgramCounterRef(block)};

  intoblock_builer.CreateStore(intoblock_builer.CreateCall(target_func, args),
                               remill::LoadMemoryPointerRef(block));

  //NOTE(Ian): If we made it past decoding we should be able to decode the bytes again
  DLOG(INFO) << res.first;

  return res.first;
}

Sleigh &SleighLifter::GetEngine(void) const {
  return this->sleigh_context->GetEngine();
}

SleighLifterWithState::SleighLifterWithState(
    std::optional<sleigh::BranchTakenVar> btaken_,
    std::shared_ptr<SleighLifter> lifter_)
    : btaken(btaken_),
      lifter(std::move(lifter_)) {}

// Lift a single instruction into a basic block. `is_delayed` signifies that
// this instruction will execute within the delay slot of another instruction.
LiftStatus
SleighLifterWithState::LiftIntoBlock(Instruction &inst, llvm::BasicBlock *block,
                                     llvm::Value *state_ptr, bool is_delayed) {
  return this->lifter->LiftIntoBlockWithSleighState(inst, block, state_ptr,
                                                    is_delayed, this->btaken);
}


// Load the address of a register.
std::pair<llvm::Value *, llvm::Type *>
SleighLifterWithState::LoadRegAddress(llvm::BasicBlock *block,
                                      llvm::Value *state_ptr,
                                      std::string_view reg_name) const {
  return this->lifter->LoadRegAddress(block, state_ptr, reg_name);
}

// Load the value of a register.
llvm::Value *
SleighLifterWithState::LoadRegValue(llvm::BasicBlock *block,
                                    llvm::Value *state_ptr,
                                    std::string_view reg_name) const {
  return this->lifter->LoadRegValue(block, state_ptr, reg_name);
}

llvm::Type *SleighLifterWithState::GetMemoryType() {
  return this->lifter->GetMemoryType();
}

void SleighLifterWithState::ClearCache(void) const {
  this->lifter->ClearCache();
}

}  // namespace remill