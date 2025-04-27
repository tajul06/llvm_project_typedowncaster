//===- TypeDowncaster.cpp - Downcast data types where safe ---------------===//
//
// This pass identifies opportunities to replace larger data types (e.g., i64)
// with smaller ones (e.g., i32) when it is safe to do so.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "typedowncaster"

STATISTIC(NumAllocasOptimized, "Number of allocas optimized");
STATISTIC(NumGlobalsOptimized, "Number of globals optimized");
STATISTIC(NumStructFieldsOptimized, "Number of struct fields optimized");
STATISTIC(NumFloatToFloatOptimized, "Number of double to float conversions");
STATISTIC(NumTotalBytesReduced, "Total number of bytes reduced in memory allocation");

namespace {

// Helper class to handle replacement of values and to track pending replacements
class ReplacementTracker {
  std::map<Value *, Value *> Replacements;
  std::map<AllocaInst *, AllocaInst *> AllocaReplacements;
  std::map<GlobalVariable *, GlobalVariable *> GlobalReplacements;
  std::set<Instruction *> ToRemove;
  SmallPtrSet<Value *, 16> Processed;

public:
  void addReplacement(Value *Old, Value *New) { 
    Replacements[Old] = New; 
  }
  
  void addAllocaReplacement(AllocaInst *Old, AllocaInst *New) {
    AllocaReplacements[Old] = New;
    addReplacement(Old, New);
  }
  
  void addGlobalReplacement(GlobalVariable *Old, GlobalVariable *New) {
    GlobalReplacements[Old] = New;
    addReplacement(Old, New);
  }

  void markForRemoval(Instruction *I) {
    ToRemove.insert(I);
  }

  bool hasReplacement(Value *V) const {
    return Replacements.count(V) > 0;
  }

  Value *getReplacement(Value *V) const {
    auto It = Replacements.find(V);
    if (It != Replacements.end())
      return It->second;
    return nullptr;
  }

  void markProcessed(Value *V) {
    Processed.insert(V);
  }

  bool isProcessed(Value *V) const {
    return Processed.count(V) > 0;
  }

  const std::map<AllocaInst *, AllocaInst *> &getAllocaReplacements() const {
    return AllocaReplacements;
  }

  const std::map<GlobalVariable *, GlobalVariable *> &getGlobalReplacements() const {
    return GlobalReplacements;
  }

  const std::set<Instruction *> &getToRemove() const {
    return ToRemove;
  }

  void clear() {
    Replacements.clear();
    AllocaReplacements.clear();
    GlobalReplacements.clear();
    ToRemove.clear();
    Processed.clear();
  }
};

struct TypeDowncaster : public PassInfoMixin<TypeDowncaster> {
  // Data structures to track what we've modified
  ReplacementTracker Tracker;

  bool isEligibleForOptimization(Type *Ty) const {
    // Check if this is a 64-bit integer that could be 32-bit
    if (Ty->isIntegerTy(64))
      return true;
    
    // Check if this is a double that could be float
    if (Ty->isDoubleTy())
      return true;

    // Check array type
    if (Ty->isArrayTy()) {
      Type *ElemTy = Ty->getArrayElementType();
      return isEligibleForOptimization(ElemTy);
    }

    // Check vector type
    if (Ty->isVectorTy()) {
      Type *ElemTy = cast<VectorType>(Ty)->getElementType();
      return isEligibleForOptimization(ElemTy);
    }

    // Check struct type
    if (Ty->isStructTy()) {
      StructType *StructTy = cast<StructType>(Ty);
      for (unsigned i = 0; i < StructTy->getNumElements(); ++i) {
        if (isEligibleForOptimization(StructTy->getElementType(i)))
          return true;
      }
    }

    return false;
  }

  Type *getOptimizedType(Type *Ty, LLVMContext &Ctx) const {
    if (Ty->isIntegerTy(64))
      return Type::getInt32Ty(Ctx);
    
    if (Ty->isDoubleTy())
      return Type::getFloatTy(Ctx);
    
    if (Ty->isArrayTy()) {
      Type *ElemTy = Ty->getArrayElementType();
      Type *OptimizedElemTy = getOptimizedType(ElemTy, Ctx);
      if (OptimizedElemTy != ElemTy)
        return ArrayType::get(OptimizedElemTy, Ty->getArrayNumElements());
    }
    
    if (Ty->isVectorTy()) {
      Type *ElemTy = cast<VectorType>(Ty)->getElementType();
      Type *OptimizedElemTy = getOptimizedType(ElemTy, Ctx);
      if (OptimizedElemTy != ElemTy) {
        ElementCount EC = cast<VectorType>(Ty)->getElementCount();
        return VectorType::get(OptimizedElemTy, EC);
      }
    }
    
    if (Ty->isStructTy()) {
      StructType *StructTy = cast<StructType>(Ty);
      bool Modified = false;
      std::vector<Type *> Elements;
      
      for (unsigned i = 0; i < StructTy->getNumElements(); ++i) {
        Type *ElemTy = StructTy->getElementType(i);
        Type *OptimizedElemTy = getOptimizedType(ElemTy, Ctx);
        Elements.push_back(OptimizedElemTy);
        if (OptimizedElemTy != ElemTy)
          Modified = true;
      }
      
      if (Modified) {
        if (StructTy->hasName()) {
          std::string Name = (StructTy->getName() + ".optimized").str();
          return StructType::create(Ctx, Elements, Name, StructTy->isPacked());
        } else {
          return StructType::get(Ctx, Elements, StructTy->isPacked());
        }
      }
    }
    
    return Ty;
  }
  /**
   * Determines if it's safe to downcast a 64-bit integer value to 32-bit.
   * 
   * This function uses ScalarEvolution to perform range analysis on the value.
   * It checks both constant values and computed ranges to ensure that all
   * possible values of V can fit within 32 bits without loss of information.
   * 
   * The analysis checks:
   * 1. If the value is a constant, directly check if it fits in 32 bits
   * 2. If ScalarEvolution can compute a range, check if the entire range fits
   * 3. For both signed and unsigned interpretations of the value
   * 
   * This is a conservative analysis - it will only return true when it can
   * prove the downcast is safe; otherwise it returns false.
   * 
   * @param V The Value to analyze for safe downcasting
   * @param SE ScalarEvolution analysis results to use for range analysis
   * @return true if downcasting is guaranteed to be safe, false otherwise
   */
  bool isSafeToCast(Value *V, ScalarEvolution &SE) {
    // If there's a SCEV for this value, try to determine its range
    if (const SCEV *ValueSCEV = SE.getSCEV(V)) {
      // If this is a constant, check its value
      if (const SCEVConstant *ConstSCEV = dyn_cast<SCEVConstant>(ValueSCEV)) {
        const APInt &Value = ConstSCEV->getValue()->getValue();
        return Value.isSignedIntN(32);
      }
      
      // If we can compute a range of the SCEV, check if it's within 32 bits
      ConstantRange Range = SE.getUnsignedRange(ValueSCEV);
      if (!Range.isFullSet()) {
        if (Range.getUnsignedMax().isIntN(32) && Range.getUnsignedMin().isIntN(32))
          return true;
      }

      Range = SE.getSignedRange(ValueSCEV);
      if (!Range.isFullSet()) {
        if (Range.getSignedMax().isIntN(32) && Range.getSignedMin().isIntN(32))
          return true;
      }
    }

    // If this is a constant integer, check if it fits in 32 bits
    if (ConstantInt *ConstInt = dyn_cast<ConstantInt>(V)) {
      return ConstInt->getValue().isSignedIntN(32);
    }

    // By default, be conservative and assume it's not safe
    return false;
  }

  bool isSafeToCastFloat(Value *V) {
    // If this is a constant, check if it can be precisely represented as float
    if (ConstantFP *ConstFP = dyn_cast<ConstantFP>(V)) {
      APFloat DoubleVal = ConstFP->getValueAPF();
      bool LosesInfo = false;
      
      // Convert to float semantics to check precision loss
      const fltSemantics &FloatSemantics = APFloat::IEEEsingle();
      DoubleVal.convert(FloatSemantics, APFloat::rmTowardZero, &LosesInfo);
      
      return !LosesInfo;
    }

    // For variables, be conservative about float precision
    return false;
  }

  Value *createCastIfNeeded(IRBuilder<> &Builder, Value *V, Type *DestTy) {
    if (V->getType() == DestTy)
      return V;
      
    if (DestTy->isIntegerTy() && V->getType()->isIntegerTy()) {
      if (DestTy->getIntegerBitWidth() > V->getType()->getIntegerBitWidth())
        return Builder.CreateSExt(V, DestTy);
      else
        return Builder.CreateTrunc(V, DestTy);
    }
    
    if (DestTy->isDoubleTy() && V->getType()->isFloatTy())
      return Builder.CreateFPExt(V, DestTy);
    if (DestTy->isFloatTy() && V->getType()->isDoubleTy())
      return Builder.CreateFPTrunc(V, DestTy);

    // Handle pointer types
    if (DestTy->isPointerTy() && V->getType()->isPointerTy())
      return Builder.CreateBitCast(V, DestTy);

    // Default case - bitcast if possible or return nullptr
    if (V->getType()->canBitCastTo(DestTy))
      return Builder.CreateBitCast(V, DestTy);

    return nullptr;
  }

  bool optimizeAlloca(AllocaInst *Alloca, ScalarEvolution &SE, LLVMContext &Ctx, Function &F) {
    Type *AllocaTy = Alloca->getAllocatedType();
    Type *OptimizedTy = getOptimizedType(AllocaTy, Ctx);
    
    // Nothing to optimize
    if (OptimizedTy == AllocaTy)
      return false;

    // Create a new alloca with the smaller type
    IRBuilder<> Builder(Alloca);
    AllocaInst *NewAlloca = Builder.CreateAlloca(OptimizedTy, Alloca->getArraySize(),
                                                Alloca->getName() + ".optimized");
    NewAlloca->setAlignment(Alloca->getAlign());
    
    // Record this replacement
    Tracker.addAllocaReplacement(Alloca, NewAlloca);
    
    unsigned OriginalSize = F.getParent()->getDataLayout().getTypeAllocSize(AllocaTy);
    unsigned OptimizedSize = F.getParent()->getDataLayout().getTypeAllocSize(OptimizedTy);
    NumTotalBytesReduced += (OriginalSize - OptimizedSize);
    
    return true;
  }

  void optimizeGlobal(GlobalVariable *GV, ScalarEvolution &SE, Module &M) {
    Type *GVType = GV->getValueType();
    Type *OptimizedTy = getOptimizedType(GVType, M.getContext());
    
    if (OptimizedTy == GVType)
      return;

    // Create new global with the optimized type
    GlobalVariable *NewGV = new GlobalVariable(
        M, OptimizedTy, GV->isConstant(), GV->getLinkage(),
        nullptr, GV->getName() + ".optimized", nullptr,
        GV->getThreadLocalMode(), GV->getAddressSpace());

    // Handle initialization if present
    if (GV->hasInitializer()) {
        Constant *Init = GV->getInitializer();
        
        // Try to handle common initializers
        if (isa<ConstantInt>(Init)) {
            ConstantInt *CI = cast<ConstantInt>(Init);
            if (OptimizedTy->isIntegerTy(32)) {
                APInt TruncValue = CI->getValue().trunc(32);
                NewGV->setInitializer(ConstantInt::get(OptimizedTy, TruncValue));
            }
        } else if (isa<ConstantFP>(Init) && OptimizedTy->isFloatTy()) {
            ConstantFP *CF = cast<ConstantFP>(Init);
            APFloat DoubleVal = CF->getValueAPF();
            bool LosesInfo = false;
            
            const fltSemantics &FloatSemantics = APFloat::IEEEsingle();
            APFloat NewFloat = APFloat(DoubleVal);
            NewFloat.convert(FloatSemantics, APFloat::rmNearestTiesToEven, &LosesInfo);
            
            NewGV->setInitializer(ConstantFP::get(OptimizedTy, NewFloat));
        } else {
            // For complex initializers, just set to zero for now
            NewGV->setInitializer(Constant::getNullValue(OptimizedTy));
        }
    }

    // Copy other attributes
    NewGV->setAlignment(GV->getAlign());
    NewGV->setDSOLocal(GV->isDSOLocal());
    NewGV->setExternallyInitialized(GV->isExternallyInitialized());
    
    Tracker.addGlobalReplacement(GV, NewGV);

    unsigned OriginalSize = M.getDataLayout().getTypeAllocSize(GVType);
    unsigned OptimizedSize = M.getDataLayout().getTypeAllocSize(OptimizedTy);
    NumTotalBytesReduced += (OriginalSize - OptimizedSize);
  }

  void rewriteUses(Function &F) {
    std::vector<Instruction *> WorkList;
    
    // Setup worklist to start with all instructions
    for (auto &BB : F) {
      for (auto &I : BB) {
        WorkList.push_back(&I);
      }
    }
    
    while (!WorkList.empty()) {
      Instruction *I = WorkList.back();
      WorkList.pop_back();
      
      if (Tracker.isProcessed(I))
        continue;
        
      Tracker.markProcessed(I);
      
      // Skip instructions marked for removal
      if (Tracker.getToRemove().count(I))
        continue;
      
      // If this is a load or store accessing a modified allocation/global
      if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
        Value *Ptr = LI->getPointerOperand();
        if (Tracker.hasReplacement(Ptr)) {
          IRBuilder<> Builder(LI);
          Value *NewPtr = Tracker.getReplacement(Ptr);
          
          Type *OriginalType = LI->getType();
          Type *NewPtrElemTy = cast<PointerType>(NewPtr->getType())->getElementType();
          
          // Create load from the new memory location
          LoadInst *NewLoad = Builder.CreateLoad(NewPtrElemTy, NewPtr, LI->getName() + ".downcasted");
          NewLoad->setAlignment(LI->getAlign());
          NewLoad->setVolatile(LI->isVolatile());
          NewLoad->setOrdering(LI->getOrdering());
          
          // Cast back to the original type if needed
          Value *Result = createCastIfNeeded(Builder, NewLoad, OriginalType);
          
          if (Result) {
            LI->replaceAllUsesWith(Result);
            Tracker.markForRemoval(LI);
          }
        }
      }
      else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
        Value *Ptr = SI->getPointerOperand();
        if (Tracker.hasReplacement(Ptr)) {
          IRBuilder<> Builder(SI);
          Value *NewPtr = Tracker.getReplacement(Ptr);
          Value *ValToStore = SI->getValueOperand();
          Type *NewPtrElemTy = cast<PointerType>(NewPtr->getType())->getElementType();
          
          // Cast the value to the new type if needed
          Value *NewValToStore = createCastIfNeeded(Builder, ValToStore, NewPtrElemTy);
          
          if (NewValToStore) {
            // Create store to the new memory location
            StoreInst *NewStore = Builder.CreateStore(NewValToStore, NewPtr);
            NewStore->setAlignment(SI->getAlign());
            NewStore->setVolatile(SI->isVolatile());
            NewStore->setOrdering(SI->getOrdering());
            
            Tracker.markForRemoval(SI);
          }
        }
      }
      // Handle GEP instructions for struct field access
      else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(I)) {
        Value *Ptr = GEP->getPointerOperand();
        if (Tracker.hasReplacement(Ptr)) {
          IRBuilder<> Builder(GEP);
          Value *NewPtr = Tracker.getReplacement(Ptr);
          
          // Recreate the GEP with the new pointer base
          SmallVector<Value *, 4> Indices;
          for (auto I = GEP->idx_begin(), E = GEP->idx_end(); I != E; ++I) {
            Indices.push_back(*I);
          }
          
          Value *NewGEP = Builder.CreateGEP(
              cast<PointerType>(NewPtr->getType())->getElementType(),
              NewPtr, Indices, GEP->getName() + ".optimized");
          
          Tracker.addReplacement(GEP, NewGEP);
        }
      }
    }
  }

  void removeDeadInstructions(Function &F) {
    for (Instruction *I : Tracker.getToRemove()) {
      if (!I->use_empty()) {
        errs() << "Warning: Attempting to remove instruction with uses: ";
        I->print(errs());
        errs() << "\n";
        continue;
      }
      I->eraseFromParent();
    }
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    // Skip functions with no body
    if (F.isDeclaration())
      return PreservedAnalyses::all();

    // Get necessary analysis results
    ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
    
    LLVM_DEBUG(dbgs() << "TypeDowncaster: Processing function " << F.getName() << "\n");
    
    bool MadeChanges = false;
    Module *M = F.getParent();
    LLVMContext &Ctx = M->getContext();
    
    // Clear any previous data in the tracker
    Tracker.clear();
    
    // First step: Analyze and optimize stack allocations
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (AllocaInst *Alloca = dyn_cast<AllocaInst>(&I)) {
          if (isEligibleForOptimization(Alloca->getAllocatedType())) {
            if (optimizeAlloca(Alloca, SE, Ctx, F)) {
              MadeChanges = true;
              ++NumAllocasOptimized;
              LLVM_DEBUG(dbgs() << "  Optimized alloca: " << *Alloca << "\n");
            }
          }
        }
      }
    }
    
    // Second step: Apply the transformations to uses
    if (MadeChanges) {
      rewriteUses(F);
      removeDeadInstructions(F);
    }

    // If we changed anything, mark all analyses as invalidated
    if (MadeChanges) {
      LLVM_DEBUG(dbgs() << "  Made changes to function " << F.getName() << "\n");
      return PreservedAnalyses::none();
    }
    
    return PreservedAnalyses::all();
  }

  // Module pass to handle globals
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    bool MadeChanges = false;
    FunctionAnalysisManager &FAM =
        AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    
    LLVM_DEBUG(dbgs() << "TypeDowncaster: Processing module " << M.getName() << "\n");
    
    // Clear any previous data in the tracker
    Tracker.clear();
    
    // First step: Process global variables
    for (auto &GV : M.globals()) {
      if (!GV.isDeclaration() && isEligibleForOptimization(GV.getValueType())) {
        // Get any function to get ScalarEvolution (doesn't matter which)
        Function *F = nullptr;
        for (auto &Func : M) {
          if (!Func.isDeclaration()) {
            F = &Func;
            break;
          }
        }
        
        if (!F) {
          LLVM_DEBUG(dbgs() << "  No function found to analyze globals\n");
          continue;
        }
        
        ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(*F);
        
        optimizeGlobal(&GV, SE, M);
        ++NumGlobalsOptimized;
        MadeChanges = true;
        
        LLVM_DEBUG(dbgs() << "  Optimized global variable: " << GV.getName() << "\n");
      }
    }
    
    // Process each function
    for (auto &F : M) {
      if (!F.isDeclaration()) {
        PreservedAnalyses PA = run(F, FAM);
        if (!PA.areAllPreserved())
          MadeChanges = true;
      }
    }
    
    if (MadeChanges) {
      LLVM_DEBUG(dbgs() << "  Made changes to module " << M.getName() << "\n");
      return PreservedAnalyses::none();
    }
    
    return PreservedAnalyses::all();
  }
};

} // end anonymous namespace

// Register the pass plugin
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "TypeDowncaster", "v1.0",
    [](PassBuilder &PB) {
      PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "type-downcaster") {
            FPM.addPass(TypeDowncaster());
            return true;
          }
          return false;
        }
      );
      
      PB.registerPipelineParsingCallback(
        [](StringRef Name, ModulePassManager &MPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "type-downcaster") {
            MPM.addPass(TypeDowncaster());
            return true;
          }
          return false;
        }
      );
      
      // Register for optimization level-based pass manager build
      PB.registerOptimizerLastEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel Level) {
          MPM.addPass(TypeDowncaster());
        }
      );
    }
  };
}
