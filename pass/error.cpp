#include <iostream>
#include <sstream>
#include <string>

#include "llvm/Module.h"
#include "llvm/IRBuilder.h"

#include "accept.h"

using namespace llvm;

//cl::opt<bool> optInjectError ("inject-error",
//    cl::desc("ACCEPT: enable error injection"));

namespace {
  void replaceAllUsesWithExcept(Instruction* oldInst, Value* newInst,
      std::vector<Value*>& except) {
    std::vector<User*> users;
    for (Value::use_iterator ui = oldInst->use_begin();
        ui != oldInst->use_end(); ++ui)
      users.push_back(*ui);
  
    for (int i = 0; i < users.size(); ++i) {
      User* user = users[i];

      bool shouldSkip = false;
      for (int j = 0; j < except.size(); ++j) {
        if (except[j] == cast<Instruction>(user)) {
          shouldSkip = true;
          break;
        }
      }
      if (shouldSkip) continue;
  
      if (auto *C = dyn_cast<Constant>(user)) {
        if (!isa<GlobalValue>(C)) {
          int j;
          for (j = 0; j < user->getNumOperands(); ++j)
            if (user->getOperand(j) == oldInst) break;

          C->replaceUsesOfWithOnConstant(oldInst, newInst,
              &(user->getOperandUse(j)));
          continue;
        }
      }
  
      user->replaceUsesOfWith(oldInst, newInst);
    }
  
    if (BasicBlock *BB = dyn_cast<BasicBlock>(oldInst))
      BB->replaceSuccessorsPhiUsesWith(cast<BasicBlock>(newInst));
  }

  std::string get_mangled_fn_name(Module* m, std::string unmangled_fn_name) {
    const Module::FunctionListType& fn_list = m->getFunctionList();
    for (Module::FunctionListType::const_iterator fi = fn_list.begin();
        fi != fn_list.end(); ++fi) {
      const Function* fn = fi;
      std::string fn_name = fn->getName().str();
      if (fn_name.find(unmangled_fn_name) != std::string::npos) return fn_name;
    }
    return "Error: did not find the error injection function";
  }
}

struct ErrorInjection : public FunctionPass {
  static char ID;
  ACCEPTPass *transformPass;
  ApproxInfo *AI;
  Module *module;

  ErrorInjection();
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual const char *getPassName() const;
  virtual bool doInitialization(llvm::Module &M);
  virtual bool doFinalization(llvm::Module &M);
  virtual bool runOnFunction(llvm::Function &F);

  bool injectError(Function &F);

  bool instructionErrorInjection(Function& F);
  bool injectErrorInst(Instruction* inst, Instruction* nextInst,
      Function* injectFn);
  bool injectHooks(Instruction* inst, Instruction* nextInst, int param,
      Function* injectFn);
  bool injectHooksBinOp(Instruction* inst, Instruction* nextInst, int param,
      Function* injectFn);
};

void ErrorInjection::getAnalysisUsage(AnalysisUsage &AU) const {
  FunctionPass::getAnalysisUsage(AU);
  AU.addRequired<ApproxInfo>();
}

ErrorInjection::ErrorInjection() : FunctionPass(ID) {
  initializeErrorInjectionPass(*PassRegistry::getPassRegistry());
  module = 0;
}

const char *ErrorInjection::getPassName() const {
  return "ACCEPT error injection pass";
}

bool ErrorInjection::doInitialization(Module &M) {
  module = &M;
  transformPass = (ACCEPTPass*)sharedAcceptTransformPass;
  return false;
}

bool ErrorInjection::doFinalization(Module &M) {
  return false;
}

bool ErrorInjection::runOnFunction(Function &F) {
  AI = &getAnalysis<ApproxInfo>();

  // Skip optimizing functions that seem to be in standard libraries.
  if (transformPass->shouldSkipFunc(F))
    return false;

  bool modified = instructionErrorInjection(F);
  return modified;
}

bool ErrorInjection::instructionErrorInjection(Function& F) {
  const std::string injectFn_unmangled_name = "injectInst";
  const std::string injectFn_mangled_name =
      get_mangled_fn_name(module, injectFn_unmangled_name);
  Function* injectFn = module->getFunction(injectFn_mangled_name);

  bool modified = false;
  std::vector<Instruction*> all_insts;
  for (Function::iterator fi = F.begin(); fi != F.end(); ++fi) {
    BasicBlock *bb = fi;
    for (BasicBlock::iterator bi = bb->begin(); bi != bb->end(); ++bi) {
      Instruction *inst = bi;
      all_insts.push_back(inst);
    }
  }

  int n_insts = all_insts.size();
  for (int i = 0; i < n_insts; ++i) {
    Instruction* nextInst = ((i == n_insts - 1) ? NULL : all_insts[i + 1]);
    if (injectErrorInst(all_insts[i], nextInst, injectFn)) modified = true;
  }

  return modified;
}

bool ErrorInjection::injectHooksBinOp(Instruction* inst, Instruction* nextInst,
    int param, Function* injectFn) {
  if (dyn_cast<Constant>(inst)) return false;
  Type* orig_type = inst->getType();
  std::string orig_type_str;
  if (orig_type == Type::getHalfTy(module->getContext()))
    orig_type_str = "Half";
  else if (orig_type == Type::getFloatTy(module->getContext()))
    orig_type_str = "Float";
  else if (orig_type == Type::getDoubleTy(module->getContext()))
    orig_type_str = "Double";
  else if (orig_type == Type::getInt1Ty(module->getContext()))
    orig_type_str = "Int1";
  else if (orig_type == Type::getInt8Ty(module->getContext()))
    orig_type_str = "Int8";
  else if (orig_type == Type::getInt16Ty(module->getContext()))
    orig_type_str = "Int16";
  else if (orig_type == Type::getInt32Ty(module->getContext()))
    orig_type_str = "Int32";
  else if (orig_type == Type::getInt64Ty(module->getContext()))
    orig_type_str = "Int64";
  else
    return false;

  IRBuilder<> builder(module->getContext());
  builder.SetInsertPoint(nextInst);

  Value* ret_to_be_casted = inst;
  Value* op1_to_be_casted = inst->getOperand(0);
  Value* op2_to_be_casted = inst->getOperand(1);
  Type* dst_type = NULL;
  if (orig_type_str == "Half" || orig_type_str == "Float"
      || orig_type_str == "Double") {
    if (orig_type_str == "Half")
      dst_type = Type::getInt16Ty(module->getContext());
    else if (orig_type_str == "Float")
      dst_type = Type::getInt32Ty(module->getContext());
    else
      dst_type = Type::getInt64Ty(module->getContext());

    ret_to_be_casted = builder.CreateBitCast(inst, dst_type);
    op1_to_be_casted = builder.CreateBitCast(inst->getOperand(0), dst_type);
    op2_to_be_casted = builder.CreateBitCast(inst->getOperand(1), dst_type);
  }

  Type* int64ty = Type::getInt64Ty(module->getContext());
  Value* param_ret = builder.CreateZExtOrBitCast(ret_to_be_casted, int64ty);
  Value* param_op1 = builder.CreateZExtOrBitCast(op1_to_be_casted, int64ty);
  Value* param_op2 = builder.CreateZExtOrBitCast(op2_to_be_casted, int64ty);

  Value* opcode_global_str = builder.CreateGlobalString(inst->getOpcodeName());
  Value* param_opcode = builder.CreateBitCast(opcode_global_str,
      Type::getInt8PtrTy(module->getContext()));
  Value* type_global_str = builder.CreateGlobalString(orig_type_str.c_str());
  Value* param_orig_type = builder.CreateBitCast(type_global_str,
      Type::getInt8PtrTy(module->getContext()));

  Value* param_knob = ConstantInt::get(int64ty, param, false);

  SmallVector<Value *, 6> Args;
  Args.push_back(param_opcode);
  Args.push_back(param_knob);
  Args.push_back(param_ret);
  Args.push_back(param_op1);
  Args.push_back(param_op2);
  Args.push_back(param_orig_type);
  CallInst* call = builder.CreateCall(injectFn, Args);

  Value* final_result;
  if (dst_type && dst_type != int64ty) {
    Value* trunc = builder.CreateTrunc(call, dst_type);
    final_result = builder.CreateBitCast(trunc, orig_type);
  } else {
    final_result = builder.CreateTruncOrBitCast(call, orig_type);
  }

  std::vector<Value*> except;
  if (ret_to_be_casted != inst)
    except.push_back(ret_to_be_casted);
  else
    except.push_back(param_ret);
  except.push_back(call);
  replaceAllUsesWithExcept(inst, final_result, except);

  return true;
}

bool ErrorInjection::injectHooks(Instruction* inst, Instruction* nextInst,
    int param, Function* injectFn) {
  if (isa<BinaryOperator>(inst)) {
    bool mod = injectHooksBinOp(inst, nextInst, param, injectFn);
    return mod;
  }

  return false;
}

bool ErrorInjection::injectErrorInst(Instruction* inst, Instruction* nextInst,
    Function* injectFn) {
  std::stringstream ss;
  ss << "instruction " << inst->getOpcodeName() << " at " <<
      srcPosDesc(*module, inst->getDebugLoc());
  std::string instName = ss.str();

  LogDescription *desc = AI->logAdd("Instruction", inst);
  ACCEPT_LOG << instName << "\n";

  bool approx = isApprox(inst);

  if (transformPass->relax) { // we're injecting error
    int param = transformPass->relaxConfig[instName];
    if (param) {
      ACCEPT_LOG << "injecting error " << param << "\n";
      // param tells which error injection will be done e.g. bit flipping
      return injectHooks(inst, nextInst, param, injectFn);
    } else {
      ACCEPT_LOG << "not injecting error\n";
      return false;
    }
  } else { // we're just logging
    if (approx) {
      ACCEPT_LOG << "can inject error\n";
      transformPass->relaxConfig[instName] = 0;
    } else {
      ACCEPT_LOG << "cannot inject error\n";
    }
  }
  return false;
}

char ErrorInjection::ID = 0;
INITIALIZE_PASS_BEGIN(ErrorInjection, "error", "ACCEPT error injection pass", false, false)
INITIALIZE_PASS_DEPENDENCY(ApproxInfo)
INITIALIZE_PASS_END(ErrorInjection, "error", "ACCEPT error injection pass", false, false)
FunctionPass *llvm::createErrorInjectionPass() { return new ErrorInjection(); }
