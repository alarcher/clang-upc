//===--- CGDeclCXX.cpp - Emit LLVM Code for C++ declarations --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code dealing with code generation of C++ declarations
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CGCXXABI.h"
#include "CGObjCRuntime.h"
#include "clang/Frontend/CodeGenOptions.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Path.h"

using namespace clang;
using namespace CodeGen;

static void EmitDeclInit(CodeGenFunction &CGF, const VarDecl &D,
                         llvm::Constant *DeclPtr) {
  assert(D.hasGlobalStorage() && "VarDecl must have global storage!");
  assert(!D.getType()->isReferenceType() && 
         "Should not call EmitDeclInit on a reference!");
  
  ASTContext &Context = CGF.getContext();

  CharUnits alignment = Context.getDeclAlign(&D);
  QualType type = D.getType();

  // Deduce UPC strict or relaxed from context, if needed
  if (Context.getLangOpts().UPC) {
      Qualifiers Quals = type.getQualifiers();
      if (Quals.hasShared() && !Quals.hasStrict() && !Quals.hasRelaxed()) {
        if (D.isUPCInitStrict())
          Quals.addStrict();
        else
          Quals.addRelaxed();

        type = Context.getQualifiedType(type.getUnqualifiedType(), Quals);
      }
  }

  LValue lv;
  if(type.getQualifiers().hasShared())
    lv = CGF.EmitSharedVarDeclLValue(DeclPtr, alignment, type);
  else
    lv = CGF.MakeAddrLValue(DeclPtr, type, alignment);

  const Expr *Init = D.getInit();
  switch (CGF.getEvaluationKind(type)) {
  case TEK_Scalar: {
    CodeGenModule &CGM = CGF.CGM;
    if (lv.isObjCStrong())
      CGM.getObjCRuntime().EmitObjCGlobalAssign(CGF, CGF.EmitScalarExpr(Init),
                                                DeclPtr, D.getTLSKind());
    else if (lv.isObjCWeak())
      CGM.getObjCRuntime().EmitObjCWeakAssign(CGF, CGF.EmitScalarExpr(Init),
                                              DeclPtr);
    else
      CGF.EmitScalarInit(Init, &D, lv, false);
    return;
  }
  case TEK_Complex:
    CGF.EmitComplexExprIntoLValue(Init, lv, /*isInit*/ true);
    return;
  case TEK_Aggregate:
    CGF.EmitAggExpr(Init, AggValueSlot::forLValue(lv,AggValueSlot::IsDestructed,
                                          AggValueSlot::DoesNotNeedGCBarriers,
                                                  AggValueSlot::IsNotAliased));
    return;
  }
  llvm_unreachable("bad evaluation kind");
}

/// Emit code to cause the destruction of the given variable with
/// static storage duration.
static void EmitDeclDestroy(CodeGenFunction &CGF, const VarDecl &D,
                            llvm::Constant *addr) {
  CodeGenModule &CGM = CGF.CGM;

  // FIXME:  __attribute__((cleanup)) ?
  
  QualType type = D.getType();
  QualType::DestructionKind dtorKind = type.isDestructedType();

  switch (dtorKind) {
  case QualType::DK_none:
    return;

  case QualType::DK_cxx_destructor:
    break;

  case QualType::DK_objc_strong_lifetime:
  case QualType::DK_objc_weak_lifetime:
    // We don't care about releasing objects during process teardown.
    assert(!D.getTLSKind() && "should have rejected this");
    return;
  }

  llvm::Constant *function;
  llvm::Constant *argument;

  // Special-case non-array C++ destructors, where there's a function
  // with the right signature that we can just call.
  const CXXRecordDecl *record = nullptr;
  if (dtorKind == QualType::DK_cxx_destructor &&
      (record = type->getAsCXXRecordDecl())) {
    assert(!record->hasTrivialDestructor());
    CXXDestructorDecl *dtor = record->getDestructor();

    function = CGM.GetAddrOfCXXDestructor(dtor, Dtor_Complete);
    argument = llvm::ConstantExpr::getBitCast(
        addr, CGF.getTypes().ConvertType(type)->getPointerTo());

  // Otherwise, the standard logic requires a helper function.
  } else {
    function = CodeGenFunction(CGM)
        .generateDestroyHelper(addr, type, CGF.getDestroyer(dtorKind),
                               CGF.needsEHCleanup(dtorKind), &D);
    argument = llvm::Constant::getNullValue(CGF.Int8PtrTy);
  }

  CGM.getCXXABI().registerGlobalDtor(CGF, D, function, argument);
}

/// Emit code to cause the variable at the given address to be considered as
/// constant from this point onwards.
static void EmitDeclInvariant(CodeGenFunction &CGF, const VarDecl &D,
                              llvm::Constant *Addr) {
  // Don't emit the intrinsic if we're not optimizing.
  if (!CGF.CGM.getCodeGenOpts().OptimizationLevel)
    return;

  // Grab the llvm.invariant.start intrinsic.
  llvm::Intrinsic::ID InvStartID = llvm::Intrinsic::invariant_start;
  llvm::Constant *InvariantStart = CGF.CGM.getIntrinsic(InvStartID);

  // Emit a call with the size in bytes of the object.
  CharUnits WidthChars = CGF.getContext().getTypeSizeInChars(D.getType());
  uint64_t Width = WidthChars.getQuantity();
  llvm::Value *Args[2] = { llvm::ConstantInt::getSigned(CGF.Int64Ty, Width),
                           llvm::ConstantExpr::getBitCast(Addr, CGF.Int8PtrTy)};
  CGF.Builder.CreateCall(InvariantStart, Args);
}

void CodeGenFunction::EmitCXXGlobalVarDeclInit(const VarDecl &D,
                                               llvm::Constant *DeclPtr,
                                               bool PerformInit) {

  const Expr *Init = D.getInit();
  QualType T = D.getType();

  if (!T->isReferenceType()) {
    if (PerformInit)
      EmitDeclInit(*this, D, DeclPtr);
    if (CGM.isTypeConstant(D.getType(), true))
      EmitDeclInvariant(*this, D, DeclPtr);
    else
      EmitDeclDestroy(*this, D, DeclPtr);
    return;
  }

  assert(PerformInit && "cannot have constant initializer which needs "
         "destruction for reference");
  unsigned Alignment = getContext().getDeclAlign(&D).getQuantity();
  RValue RV = EmitReferenceBindingToExpr(Init);
  EmitStoreOfScalar(RV.getScalarVal(), DeclPtr, false, Alignment, T);
}

static llvm::Function *
CreateGlobalInitOrDestructFunction(CodeGenModule &CGM,
                                   llvm::FunctionType *ty,
                                   const Twine &name,
                                   bool TLS = false);

/// Create a stub function, suitable for being passed to atexit,
/// which passes the given address to the given destructor function.
static llvm::Constant *createAtExitStub(CodeGenModule &CGM, const VarDecl &VD,
                                        llvm::Constant *dtor,
                                        llvm::Constant *addr) {
  // Get the destructor function type, void(*)(void).
  llvm::FunctionType *ty = llvm::FunctionType::get(CGM.VoidTy, false);
  SmallString<256> FnName;
  {
    llvm::raw_svector_ostream Out(FnName);
    CGM.getCXXABI().getMangleContext().mangleDynamicAtExitDestructor(&VD, Out);
  }
  llvm::Function *fn =
      CreateGlobalInitOrDestructFunction(CGM, ty, FnName.str());

  CodeGenFunction CGF(CGM);

  CGF.StartFunction(&VD, CGM.getContext().VoidTy, fn,
                    CGM.getTypes().arrangeNullaryFunction(), FunctionArgList());

  llvm::CallInst *call = CGF.Builder.CreateCall(dtor, addr);
 
 // Make sure the call and the callee agree on calling convention.
  if (llvm::Function *dtorFn =
        dyn_cast<llvm::Function>(dtor->stripPointerCasts()))
    call->setCallingConv(dtorFn->getCallingConv());

  CGF.FinishFunction();

  return fn;
}

/// Register a global destructor using the C atexit runtime function.
void CodeGenFunction::registerGlobalDtorWithAtExit(const VarDecl &VD,
                                                   llvm::Constant *dtor,
                                                   llvm::Constant *addr) {
  // Create a function which calls the destructor.
  llvm::Constant *dtorStub = createAtExitStub(CGM, VD, dtor, addr);

  // extern "C" int atexit(void (*f)(void));
  llvm::FunctionType *atexitTy =
    llvm::FunctionType::get(IntTy, dtorStub->getType(), false);

  llvm::Constant *atexit =
    CGM.CreateRuntimeFunction(atexitTy, "atexit");
  if (llvm::Function *atexitFn = dyn_cast<llvm::Function>(atexit))
    atexitFn->setDoesNotThrow();

  EmitNounwindRuntimeCall(atexit, dtorStub);
}

void CodeGenFunction::EmitCXXGuardedInit(const VarDecl &D,
                                         llvm::GlobalVariable *DeclPtr,
                                         bool PerformInit) {
  // If we've been asked to forbid guard variables, emit an error now.
  // This diagnostic is hard-coded for Darwin's use case;  we can find
  // better phrasing if someone else needs it.
  if (CGM.getCodeGenOpts().ForbidGuardVariables)
    CGM.Error(D.getLocation(),
              "this initialization requires a guard variable, which "
              "the kernel does not support");

  CGM.getCXXABI().EmitGuardedInit(*this, D, DeclPtr, PerformInit);
}

static llvm::Function *
CreateGlobalInitOrDestructFunction(CodeGenModule &CGM,
                                   llvm::FunctionType *FTy,
                                   const Twine &Name, bool TLS) {
  llvm::Function *Fn =
    llvm::Function::Create(FTy, llvm::GlobalValue::InternalLinkage,
                           Name, &CGM.getModule());
  if (!CGM.getLangOpts().AppleKext && !TLS) {
    // Set the section if needed.
    if (const char *Section = 
          CGM.getTarget().getStaticInitSectionSpecifier())
      Fn->setSection(Section);
  }

  Fn->setCallingConv(CGM.getRuntimeCC());

  if (!CGM.getLangOpts().Exceptions)
    Fn->setDoesNotThrow();

  if (!CGM.getSanitizerBlacklist().isIn(*Fn)) {
    if (CGM.getLangOpts().Sanitize.Address)
      Fn->addFnAttr(llvm::Attribute::SanitizeAddress);
    if (CGM.getLangOpts().Sanitize.Thread)
      Fn->addFnAttr(llvm::Attribute::SanitizeThread);
    if (CGM.getLangOpts().Sanitize.Memory)
      Fn->addFnAttr(llvm::Attribute::SanitizeMemory);
  }

  return Fn;
}

/// Create a global pointer to a function that will initialize a global
/// variable.  The user has requested that this pointer be emitted in a specific
/// section.
void CodeGenModule::EmitPointerToInitFunc(const VarDecl *D,
                                          llvm::GlobalVariable *GV,
                                          llvm::Function *InitFunc,
                                          InitSegAttr *ISA) {
  llvm::GlobalVariable *PtrArray = new llvm::GlobalVariable(
      TheModule, InitFunc->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, InitFunc, "__cxx_init_fn_ptr");
  PtrArray->setSection(ISA->getSection());
  addUsedGlobal(PtrArray);

  // If the GV is already in a comdat group, then we have to join it.
  llvm::Comdat *C = GV->getComdat();

  // LinkOnce and Weak linkage are lowered down to a single-member comdat group.
  // Make an explicit group so we can join it.
  if (!C && (GV->hasWeakLinkage() || GV->hasLinkOnceLinkage())) {
    C = TheModule.getOrInsertComdat(GV->getName());
    GV->setComdat(C);
  }
  if (C)
    PtrArray->setComdat(C);
}

void
CodeGenModule::EmitCXXGlobalVarDeclInitFunc(const VarDecl *D,
                                            llvm::GlobalVariable *Addr,
                                            bool PerformInit) {
  llvm::FunctionType *FTy = llvm::FunctionType::get(VoidTy, false);
  SmallString<256> FnName;
  {
    llvm::raw_svector_ostream Out(FnName);
    getCXXABI().getMangleContext().mangleDynamicInitializer(D, Out);
  }

  // Create a variable initialization function.
  StringRef InitFunctionName = getContext().getLangOpts().UPC ?
    StringRef("__upc_global_var_init") : FnName.str();

  llvm::Function *Fn =
      CreateGlobalInitOrDestructFunction(*this, FTy, InitFunctionName);

  auto *ISA = D->getAttr<InitSegAttr>();
  CodeGenFunction(*this).GenerateCXXGlobalVarDeclInitFunc(Fn, D, Addr,
                                                          PerformInit);
  if (D->getTLSKind()) {
    // FIXME: Should we support init_priority for thread_local?
    // FIXME: Ideally, initialization of instantiated thread_local static data
    // members of class templates should not trigger initialization of other
    // entities in the TU.
    // FIXME: We only need to register one __cxa_thread_atexit function for the
    // entire TU.
    CXXThreadLocalInits.push_back(Fn);
  } else if (PerformInit && ISA) {
    EmitPointerToInitFunc(D, Addr, Fn, ISA);
    DelayedCXXInitPosition.erase(D);
  } else if (auto *IPA = D->getAttr<InitPriorityAttr>()) {
    OrderGlobalInits Key(IPA->getPriority(), PrioritizedCXXGlobalInits.size());
    PrioritizedCXXGlobalInits.push_back(std::make_pair(Key, Fn));
    DelayedCXXInitPosition.erase(D);
  } else if (D->getTemplateSpecializationKind() != TSK_ExplicitSpecialization &&
             D->getTemplateSpecializationKind() != TSK_Undeclared) {
    // C++ [basic.start.init]p2:
    //   Definitions of explicitly specialized class template static data
    //   members have ordered initialization. Other class template static data
    //   members (i.e., implicitly or explicitly instantiated specializations)
    //   have unordered initialization.
    //
    // As a consequence, we can put them into their own llvm.global_ctors entry.
    //
    // In addition, put the initializer into a COMDAT group with the global
    // being initialized.  On most platforms, this is a minor startup time
    // optimization.  In the MS C++ ABI, there are no guard variables, so this
    // COMDAT key is required for correctness.
    AddGlobalCtor(Fn, 65535, Addr);
    DelayedCXXInitPosition.erase(D);
  } else {
    llvm::DenseMap<const Decl *, unsigned>::iterator I =
      DelayedCXXInitPosition.find(D);
    if (I == DelayedCXXInitPosition.end()) {
      CXXGlobalInits.push_back(Fn);
    } else {
      assert(CXXGlobalInits[I->second] == nullptr);
      CXXGlobalInits[I->second] = Fn;
      DelayedCXXInitPosition.erase(I);
    }
  }
}

void CodeGenModule::EmitCXXThreadLocalInitFunc() {
  llvm::Function *InitFn = nullptr;
  if (!CXXThreadLocalInits.empty()) {
    // Generate a guarded initialization function.
    llvm::FunctionType *FTy = llvm::FunctionType::get(VoidTy, false);
    InitFn = CreateGlobalInitOrDestructFunction(*this, FTy, "__tls_init",
                                                /*TLS*/ true);
    llvm::GlobalVariable *Guard = new llvm::GlobalVariable(
        getModule(), Int8Ty, false, llvm::GlobalVariable::InternalLinkage,
        llvm::ConstantInt::get(Int8Ty, 0), "__tls_guard");
    Guard->setThreadLocal(true);
    CodeGenFunction(*this)
        .GenerateCXXGlobalInitFunc(InitFn, CXXThreadLocalInits, Guard);
  }

  getCXXABI().EmitThreadLocalInitFuncs(CXXThreadLocals, InitFn);

  CXXThreadLocalInits.clear();
  CXXThreadLocals.clear();
}

void
CodeGenModule::EmitCXXGlobalInitFunc() {
  while (!CXXGlobalInits.empty() && !CXXGlobalInits.back())
    CXXGlobalInits.pop_back();

  if (CXXGlobalInits.empty() && PrioritizedCXXGlobalInits.empty())
    return;

  llvm::FunctionType *FTy = llvm::FunctionType::get(VoidTy, false);

  // Create our global initialization function.
  if (!PrioritizedCXXGlobalInits.empty()) {
    SmallVector<llvm::Constant*, 8> LocalCXXGlobalInits;
    llvm::array_pod_sort(PrioritizedCXXGlobalInits.begin(), 
                         PrioritizedCXXGlobalInits.end());
    // Iterate over "chunks" of ctors with same priority and emit each chunk
    // into separate function. Note - everything is sorted first by priority,
    // second - by lex order, so we emit ctor functions in proper order.
    for (SmallVectorImpl<GlobalInitData >::iterator
           I = PrioritizedCXXGlobalInits.begin(),
           E = PrioritizedCXXGlobalInits.end(); I != E; ) {
      SmallVectorImpl<GlobalInitData >::iterator
        PrioE = std::upper_bound(I + 1, E, *I, GlobalInitPriorityCmp());

      LocalCXXGlobalInits.clear();
      unsigned Priority = I->first.priority;
      // Compute the function suffix from priority. Prepend with zeroes to make
      // sure the function names are also ordered as priorities.
      std::string PrioritySuffix = llvm::utostr(Priority);
      // Priority is always <= 65535 (enforced by sema).
      PrioritySuffix = std::string(6-PrioritySuffix.size(), '0')+PrioritySuffix;
      llvm::Function *Fn = 
        CreateGlobalInitOrDestructFunction(*this, FTy,
                                           "_GLOBAL__I_" + PrioritySuffix);
      
      for (; I < PrioE; ++I)
        LocalCXXGlobalInits.push_back(I->second);

      CodeGenFunction(*this).GenerateCXXGlobalInitFunc(Fn, LocalCXXGlobalInits);
      AddGlobalCtor(Fn, Priority);
    }
  }

  // Include the filename in the symbol name. Including "sub_" matches gcc and
  // makes sure these symbols appear lexicographically behind the symbols with
  // priority emitted above.
  SourceManager &SM = Context.getSourceManager();
  SmallString<128> FileName(llvm::sys::path::filename(
      SM.getFileEntryForID(SM.getMainFileID())->getName()));
  for (size_t i = 0; i < FileName.size(); ++i) {
    // Replace everything that's not [a-zA-Z0-9._] with a _. This set happens
    // to be the set of C preprocessing numbers.
    if (!isPreprocessingNumberBody(FileName[i]))
      FileName[i] = '_';
  }
  llvm::Function *Fn = CreateGlobalInitOrDestructFunction(
    *this, FTy, getContext().getLangOpts().UPC?
                  "__upc_init_decls" :
                  llvm::Twine("_GLOBAL__sub_I_", FileName));

  CodeGenFunction(*this).GenerateCXXGlobalInitFunc(Fn, CXXGlobalInits);
  AddGlobalCtor(Fn);

  CXXGlobalInits.clear();
  PrioritizedCXXGlobalInits.clear();
}

void CodeGenModule::EmitCXXGlobalDtorFunc() {
  if (CXXGlobalDtors.empty())
    return;

  llvm::FunctionType *FTy = llvm::FunctionType::get(VoidTy, false);

  // Create our global destructor function.
  llvm::Function *Fn =
    CreateGlobalInitOrDestructFunction(*this, FTy, "_GLOBAL__D_a");

  CodeGenFunction(*this).GenerateCXXGlobalDtorsFunc(Fn, CXXGlobalDtors);
  AddGlobalDtor(Fn);
}

/// Emit the code necessary to initialize the given global variable.
void CodeGenFunction::GenerateCXXGlobalVarDeclInitFunc(llvm::Function *Fn,
                                                       const VarDecl *D,
                                                 llvm::GlobalVariable *Addr,
                                                       bool PerformInit) {
  // Check if we need to emit debug info for variable initializer.
  if (D->hasAttr<NoDebugAttr>())
    DebugInfo = nullptr; // disable debug info indefinitely for this function

  StartFunction(GlobalDecl(D), getContext().VoidTy, Fn,
                getTypes().arrangeNullaryFunction(),
                FunctionArgList(), D->getLocation(),
                D->getInit()->getExprLoc());

  // Use guarded initialization if the global variable is weak. This
  // occurs for, e.g., instantiated static data members and
  // definitions explicitly marked weak.
  if (Addr->hasWeakLinkage() || Addr->hasLinkOnceLinkage()) {
    EmitCXXGuardedInit(*D, Addr, PerformInit);
  } else {
    EmitCXXGlobalVarDeclInit(*D, Addr, PerformInit);
  }

  FinishFunction();
}

void
CodeGenFunction::GenerateCXXGlobalInitFunc(llvm::Function *Fn,
                                           ArrayRef<llvm::Constant *> Decls,
                                           llvm::GlobalVariable *Guard) {
  {
    ArtificialLocation AL(*this, Builder);
    StartFunction(GlobalDecl(), getContext().VoidTy, Fn,
                  getTypes().arrangeNullaryFunction(), FunctionArgList());
    // Emit an artificial location for this function.
    AL.Emit();

    llvm::BasicBlock *ExitBlock = nullptr;
    if (Guard) {
      // If we have a guard variable, check whether we've already performed
      // these initializations. This happens for TLS initialization functions.
      llvm::Value *GuardVal = Builder.CreateLoad(Guard);
      llvm::Value *Uninit = Builder.CreateIsNull(GuardVal,
                                                 "guard.uninitialized");
      // Mark as initialized before initializing anything else. If the
      // initializers use previously-initialized thread_local vars, that's
      // probably supposed to be OK, but the standard doesn't say.
      Builder.CreateStore(llvm::ConstantInt::get(GuardVal->getType(),1), Guard);
      llvm::BasicBlock *InitBlock = createBasicBlock("init");
      ExitBlock = createBasicBlock("exit");
      Builder.CreateCondBr(Uninit, InitBlock, ExitBlock);
      EmitBlock(InitBlock);
    }

    RunCleanupsScope Scope(*this);

    // When building in Objective-C++ ARC mode, create an autorelease pool
    // around the global initializers.
    if (getLangOpts().ObjCAutoRefCount && getLangOpts().CPlusPlus) {
      llvm::Value *token = EmitObjCAutoreleasePoolPush();
      EmitObjCAutoreleasePoolCleanup(token);
    }

    for (unsigned i = 0, e = Decls.size(); i != e; ++i)
      if (Decls[i])
        EmitRuntimeCall(Decls[i]);

    Scope.ForceCleanup();

    if (ExitBlock) {
      Builder.CreateBr(ExitBlock);
      EmitBlock(ExitBlock);
    }
  }

  FinishFunction();
}

void CodeGenFunction::GenerateCXXGlobalDtorsFunc(llvm::Function *Fn,
                  const std::vector<std::pair<llvm::WeakVH, llvm::Constant*> >
                                                &DtorsAndObjects) {
  {
    ArtificialLocation AL(*this, Builder);
    StartFunction(GlobalDecl(), getContext().VoidTy, Fn,
                  getTypes().arrangeNullaryFunction(), FunctionArgList());
    // Emit an artificial location for this function.
    AL.Emit();

    // Emit the dtors, in reverse order from construction.
    for (unsigned i = 0, e = DtorsAndObjects.size(); i != e; ++i) {
      llvm::Value *Callee = DtorsAndObjects[e - i - 1].first;
      llvm::CallInst *CI = Builder.CreateCall(Callee,
                                          DtorsAndObjects[e - i - 1].second);
      // Make sure the call and the callee agree on calling convention.
      if (llvm::Function *F = dyn_cast<llvm::Function>(Callee))
        CI->setCallingConv(F->getCallingConv());
    }
  }

  FinishFunction();
}

/// generateDestroyHelper - Generates a helper function which, when
/// invoked, destroys the given object.
llvm::Function *CodeGenFunction::generateDestroyHelper(
    llvm::Constant *addr, QualType type, Destroyer *destroyer,
    bool useEHCleanupForArray, const VarDecl *VD) {
  FunctionArgList args;
  ImplicitParamDecl dst(getContext(), nullptr, SourceLocation(), nullptr,
                        getContext().VoidPtrTy);
  args.push_back(&dst);

  const CGFunctionInfo &FI = CGM.getTypes().arrangeFreeFunctionDeclaration(
      getContext().VoidTy, args, FunctionType::ExtInfo(), /*variadic=*/false);
  llvm::FunctionType *FTy = CGM.getTypes().GetFunctionType(FI);
  llvm::Function *fn = 
    CreateGlobalInitOrDestructFunction(CGM, FTy, "__cxx_global_array_dtor");

  StartFunction(VD, getContext().VoidTy, fn, FI, args);

  emitDestroy(addr, type, destroyer, useEHCleanupForArray);
  
  FinishFunction();
  
  return fn;
}

void CodeGenModule::CreateOpenMPCXXInit(const VarDecl *Var,
                                        CXXRecordDecl *Ty,
                                        llvm::Function *&InitFunction,
                                        llvm::Value *&Ctor,
                                        llvm::Value *&CCtor,
                                        llvm::Value *&Dtor) {
  // Find default constructor, copy constructor and destructor.
  Ctor = 0;
  CCtor = 0;
  Dtor = 0;
  InitFunction = 0;
//  CXXConstructorDecl *CtorDecl = 0;//, *CCtorDecl = 0;
//  CXXDestructorDecl *DtorDecl = 0;
/*  for (CXXRecordDecl::ctor_iterator CI = Ty->ctor_begin(),
                                    CE = Ty->ctor_end();
       CI != CE; ++CI) {
    unsigned Quals;
    if ((*CI)->isDefaultConstructor())
      CtorDecl = *CI;
//    else if ((*CI)->isCopyConstructor(Quals) &&
//             ((Quals & Qualifiers::Const) == Qualifiers::Const))
//      CCtorDecl = *CI;
  }*/
//  if (CXXDestructorDecl *D = Ty->getDestructor())
//    DtorDecl = D;
  // Generate wrapper for default constructor.
  const Expr *Init = Var->getAnyInitializer();
  if (Init) {
    CodeGenFunction CGF(*this);
    FunctionArgList Args;
    ImplicitParamDecl Dst(getContext(), 0, SourceLocation(), 0,
                          getContext().VoidPtrTy);
    Args.push_back(&Dst);

    const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
        getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
    llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
    llvm::Function *Fn =
           CreateGlobalInitOrDestructFunction(*this, FTy,
                                              Twine("__kmpc_ctor_",
                                                    getMangledName(Var)));
    CGF.StartFunction(GlobalDecl(), getContext().VoidPtrTy, Fn, FI,
                      Args, SourceLocation());
//    CGF.EmitCXXConstructExpr(cast<CXXConstructExpr>(Var->getInit()),
//                             AggValueSlot::forAddr(Fn->arg_begin(),
//                                                   CGF.getContext().getTypeAlignInChars(Var->getType()),
//                                                   Var->getType().getQualifiers(),
//                                                   AggValueSlot::IsNotDestructed,
//                                                   AggValueSlot::DoesNotNeedGCBarriers,
//                                                   AggValueSlot::IsNotAliased));
//    //CGF.EmitCXXConstructorCall(CtorDecl, Ctor_Complete, false, false,
//    //                           Fn->arg_begin(), 0, 0);
    llvm::Value *Arg = CGF.EmitScalarConversion(Fn->arg_begin(), getContext().VoidPtrTy, getContext().getPointerType(Var->getType()));
    CGF.EmitAnyExprToMem(Init, Arg, Init->getType().getQualifiers(), true);
    CGF.Builder.CreateStore(Fn->arg_begin(), CGF.ReturnValue);
    CGF.FinishFunction();
    Ctor = Fn;
  }
/*  if (CCtorDecl) {
    FunctionArgList Args;
    if (!OpenMPCCtorHelperDecl) {
      llvm::SmallVector<QualType, 2> TArgs(2, getContext().VoidPtrTy);
      FunctionProtoType::ExtProtoInfo EPI;
      OpenMPCCtorHelperDecl =
        FunctionDecl::Create(getContext(), Ty->getTranslationUnitDecl(),
                             SourceLocation(), SourceLocation(),
                             DeclarationName(),
                             getContext().getFunctionType(
                                            getContext().VoidPtrTy,
                                            TArgs, EPI),
                             0, SC_None, SC_None, false, false);
    }
    ImplicitParamDecl Dst1(OpenMPCCtorHelperDecl, SourceLocation(), 0,
                           getContext().VoidPtrTy);
    ImplicitParamDecl Dst2(OpenMPCCtorHelperDecl, SourceLocation(), 0,
                           getContext().VoidPtrTy);
    DeclRefExpr E(&Dst2, true, getContext().VoidPtrTy, VK_LValue,
                  SourceLocation());
    ImplicitCastExpr ECast(ImplicitCastExpr::OnStack,
                           CCtorDecl->getThisType(getContext()), CK_BitCast,
                           &E, VK_LValue);
    UnaryOperator EUn(&ECast, UO_Deref, getContext().getRecordType(Ty),
                      VK_LValue, OK_Ordinary, SourceLocation());
    Stmt *S = &EUn;
    CallExpr::const_arg_iterator EI(&S);
    Args.push_back(&Dst1);
    Args.push_back(&Dst2);

    const CGFunctionInfo &FI =
          getTypes().arrangeFunctionDeclaration(getContext().VoidPtrTy, Args,
                                                FunctionType::ExtInfo(), false);
    llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
    llvm::Function *Fn =
            CreateGlobalInitOrDestructFunction(*this, FTy,
                                               Twine("__kmpc_cctor_",
                                                     Var->getName()));
    CGF.StartFunction(GlobalDecl(), getContext().VoidPtrTy, Fn, FI,
                      Args, SourceLocation());
    CGF.EmitCXXConstructorCall(CCtorDecl, Ctor_Complete, false, false,
                               Fn->arg_begin(), EI, EI + 1);
    CGF.Builder.CreateStore(&Fn->getArgumentList().back(), CGF.ReturnValue);
    CGF.FinishFunction();
    CCtor = Fn;
  }*/
  QualType QTy = Var->getType();
  QualType::DestructionKind DtorKind = QTy.isDestructedType();
  if (DtorKind != QualType::DK_none && !Ty->hasTrivialDestructor()) {
    CodeGenFunction CGF(*this);
    FunctionArgList Args;
    ImplicitParamDecl Dst(getContext(), 0, SourceLocation(), 0,
                          getContext().VoidPtrTy);
    Args.push_back(&Dst);

    const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
        getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
    llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
    llvm::Function *Fn =
           CreateGlobalInitOrDestructFunction(*this, FTy,
                                              Twine("__kmpc_dtor_",
                                                    getMangledName(Var)));
    CGF.StartFunction(GlobalDecl(), getContext().VoidPtrTy, Fn, FI,
                      Args, SourceLocation());
    //CGF.EmitCXXDestructorCall(DtorDecl, Dtor_Complete, false, false,
    //                          Fn->arg_begin());
    CGF.emitDestroy(Fn->arg_begin(), QTy, CGF.getDestroyer(DtorKind), CGF.needsEHCleanup(DtorKind));
    CGF.Builder.CreateStore(Fn->arg_begin(), CGF.ReturnValue);
    CGF.FinishFunction();
    Dtor = Fn;
  }// else {
    //DtorDecl = 0;
  //}

  if (Init || (DtorKind != QualType::DK_none && !Ty->hasTrivialDestructor())) {
    if (!Ctor) {
      FunctionArgList Args;
      ImplicitParamDecl Dst(getContext(), 0, SourceLocation(), 0,
                            getContext().VoidPtrTy);
      Args.push_back(&Dst);

      const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
          getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
      llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
      Ctor = llvm::Constant::getNullValue(FTy->getPointerTo());
    }
    if (!CCtor) {
      FunctionArgList Args;
      ImplicitParamDecl Dst1(getContext(), 0, SourceLocation(), 0,
                             getContext().VoidPtrTy);
      ImplicitParamDecl Dst2(getContext(), 0, SourceLocation(), 0,
                             getContext().VoidPtrTy);
      Args.push_back(&Dst1);
      Args.push_back(&Dst2);
      const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
          getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
      llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
      CCtor = llvm::Constant::getNullValue(FTy->getPointerTo());
    }
    if (DtorKind == QualType::DK_none || Ty->hasTrivialDestructor()) {
      FunctionArgList Args;
      ImplicitParamDecl Dst(getContext(), 0, SourceLocation(), 0,
                            getContext().VoidPtrTy);
      Args.push_back(&Dst);

      const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
          getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
      llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
      Dtor = llvm::Constant::getNullValue(FTy->getPointerTo());
    }
    llvm::FunctionType *FTy = llvm::FunctionType::get(VoidTy, false);
    InitFunction =
              CreateGlobalInitOrDestructFunction(*this, FTy,
                                                 Twine("__omp_threadprivate_",
                                                       getMangledName(Var)));
  }
}

void CodeGenModule::CreateOpenMPArrCXXInit(const VarDecl *Var,
                                           CXXRecordDecl *Ty,
                                           llvm::Function *&InitFunction,
                                           llvm::Value *&Ctor,
                                           llvm::Value *&CCtor,
                                           llvm::Value *&Dtor) {
  // Find default constructor, copy constructor and destructor.
  Ctor = 0;
  CCtor = 0;
  Dtor = 0;
  InitFunction = 0;
  //CXXConstructorDecl *CtorDecl = 0, *CCtorDecl = 0;
  CXXDestructorDecl *DtorDecl = 0;
/*  for (CXXRecordDecl::ctor_iterator CI = Ty->ctor_begin(),
                                    CE = Ty->ctor_end();
       CI != CE; ++CI) {
    unsigned Quals;
    if ((*CI)->isDefaultConstructor())
      CtorDecl = *CI;
    else if ((*CI)->isCopyConstructor(Quals) &&
             ((Quals & Qualifiers::Const) == Qualifiers::Const))
      CCtorDecl = *CI;
  }
*/
  if (CXXDestructorDecl *D = Ty->getDestructor())
    DtorDecl = D;
  // Generate wrapper for default constructor.
  const Expr *Init = Var->getAnyInitializer();
  if (Init) {
    FunctionArgList Args;
    ImplicitParamDecl Dst(getContext(), 0, SourceLocation(), 0,
                          getContext().VoidPtrTy);
    Args.push_back(&Dst);

    const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
        getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
    llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
    llvm::Function *Fn =
           CreateGlobalInitOrDestructFunction(*this, FTy,
                                              Twine("__kmpc_ctor_vec_",
                                                    getMangledName(Var)));
    CodeGenFunction CGF(*this);
    CGF.StartFunction(GlobalDecl(), getContext().VoidPtrTy, Fn, FI,
                      Args, SourceLocation());
//    CGF.EmitAggExpr(Var->getInit(),
//                    AggValueSlot::forAddr(Fn->arg_begin(),
//                                          CGF.getContext().getTypeAlignInChars(Var->getType()),
//                                          Var->getType().getQualifiers(),
//                                          AggValueSlot::IsNotDestructed,
//                                          AggValueSlot::DoesNotNeedGCBarriers,
//                                          AggValueSlot::IsNotAliased));
//    CGF.Builder.CreateStore(Fn->arg_begin(), CGF.ReturnValue);
    llvm::Value *Arg = CGF.EmitScalarConversion(Fn->arg_begin(), getContext().VoidPtrTy, getContext().getPointerType(Var->getType()));
    CGF.EmitAnyExprToMem(Init, Arg, Init->getType().getQualifiers(), true);
    CGF.Builder.CreateStore(Fn->arg_begin(), CGF.ReturnValue);
    CGF.FinishFunction();
    Ctor = Fn;
  }
/*  if (CCtorDecl) {
    FunctionArgList Args;
    if (!OpenMPCCtorHelperDecl) {
      llvm::SmallVector<QualType, 2> TArgs(2, getContext().VoidPtrTy);
      FunctionProtoType::ExtProtoInfo EPI;
      OpenMPCCtorHelperDecl =
        FunctionDecl::Create(getContext(), Ty->getTranslationUnitDecl(),
                             SourceLocation(), SourceLocation(),
                             DeclarationName(),
                             getContext().getFunctionType(
                                            getContext().VoidPtrTy,
                                            TArgs, EPI),
                             0, SC_None, SC_None, false, false);
    }
    ImplicitParamDecl Dst1(OpenMPCCtorHelperDecl, SourceLocation(), 0,
                           getContext().VoidPtrTy);
    ImplicitParamDecl Dst2(OpenMPCCtorHelperDecl, SourceLocation(), 0,
                           getContext().VoidPtrTy);
    DeclRefExpr E(&Dst2, true, getContext().VoidPtrTy, VK_LValue,
                  SourceLocation());
    ImplicitCastExpr ECast(ImplicitCastExpr::OnStack,
                           CCtorDecl->getThisType(getContext()), CK_BitCast,
                           &E, VK_LValue);
    UnaryOperator EUn(&ECast, UO_Deref, getContext().getRecordType(Ty),
                      VK_LValue, OK_Ordinary, SourceLocation());
    Stmt *S = &EUn;
    CallExpr::const_arg_iterator EI(&S);
    Args.push_back(&Dst1);
    Args.push_back(&Dst2);

    const CGFunctionInfo &FI =
          getTypes().arrangeFunctionDeclaration(getContext().VoidPtrTy, Args,
                                                FunctionType::ExtInfo(), false);
    llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
    llvm::Function *Fn =
            CreateGlobalInitOrDestructFunction(*this, FTy,
                                               Twine("__kmpc_cctor_",
                                                     Var->getName()));
    CGF.StartFunction(GlobalDecl(), getContext().VoidPtrTy, Fn, FI,
                      Args, SourceLocation());
    CGF.EmitCXXConstructorCall(CCtorDecl, Ctor_Complete, false, false,
                               Fn->arg_begin(), EI, EI + 1);
    CGF.Builder.CreateStore(&Fn->getArgumentList().back(), CGF.ReturnValue);
    CGF.FinishFunction();
    CCtor = Fn;
  }*/
  if (DtorDecl && !DtorDecl->isTrivial()) {
    FunctionArgList Args;
    ImplicitParamDecl Dst(getContext(), 0, SourceLocation(), 0,
                          getContext().VoidPtrTy);
    Args.push_back(&Dst);

    const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
        getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
    llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
    llvm::Function *Fn =
           CreateGlobalInitOrDestructFunction(*this, FTy,
                                              Twine("__kmpc_dtor_vec_",
                                                    getMangledName(Var)));
    CodeGenFunction CGF(*this);
    CGF.StartFunction(GlobalDecl(), getContext().VoidPtrTy, Fn, FI,
                      Args, SourceLocation());
    CGF.emitDestroy(Fn->arg_begin(), Var->getType(),
                    CGF.getDestroyer(QualType::DK_cxx_destructor),
                    false
                    /*CGF.needsEHCleanup(QualType::DK_cxx_destructor)*/);
    CGF.Builder.CreateStore(Fn->arg_begin(), CGF.ReturnValue);
    CGF.FinishFunction();
    Dtor = Fn;
  } else {
    DtorDecl = 0;
  }

  if (Init || DtorDecl) {
    if (!Ctor) {
      FunctionArgList Args;
      ImplicitParamDecl Dst(getContext(), 0, SourceLocation(), 0,
                            getContext().VoidPtrTy);
      Args.push_back(&Dst);

      const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
          getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
      llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
      Ctor = llvm::Constant::getNullValue(FTy->getPointerTo());
    }
    if (!CCtor) {
      FunctionArgList Args;
      ImplicitParamDecl Dst1(getContext(), 0, SourceLocation(), 0,
                             getContext().VoidPtrTy);
      ImplicitParamDecl Dst2(getContext(), 0, SourceLocation(), 0,
                             getContext().VoidPtrTy);
      Args.push_back(&Dst1);
      Args.push_back(&Dst2);
      const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
          getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
      llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
      CCtor = llvm::Constant::getNullValue(FTy->getPointerTo());
    }
    if (!Dtor) {
      FunctionArgList Args;
      ImplicitParamDecl Dst(getContext(), 0, SourceLocation(), 0,
                            getContext().VoidPtrTy);
      Args.push_back(&Dst);

      const CGFunctionInfo &FI = getTypes().arrangeFreeFunctionDeclaration(
          getContext().VoidPtrTy, Args, FunctionType::ExtInfo(), false);
      llvm::FunctionType *FTy = getTypes().GetFunctionType(FI);
      Dtor = llvm::Constant::getNullValue(FTy->getPointerTo());
    }
    llvm::FunctionType *FTy = llvm::FunctionType::get(VoidTy, false);
    InitFunction =
              CreateGlobalInitOrDestructFunction(*this, FTy,
                                                 Twine("__omp_threadprivate_vec_",
                                                       getMangledName(Var)));
  }
}

