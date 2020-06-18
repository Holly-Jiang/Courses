////////////////////////////////////////////////////////////////////////////////
/// Copyright (c) 2018, University of Science and Techonolgy of China
/// All rights reserved.
///
/// @file ModuleCheck.cpp
/// @brief Main entry of this anlysis tool, which parses arguments and dispatches
/// to corresponding FrontendAction instances
///
/// @version 0.1.0
/// @author Yuxiang Zhang (Leader), <a href="linkURL">zyx504@mail.ustc.edu.cn</a> 
/// @author Shengliang Deng, <a href="linkURL">dengsl@mail.ustc.edu.cn</a> 
/// @author Yu Zhang (Mentor), <a href="linkURL">yuzhang@ustc.edu.cn</a> 
////////////////////////////////////////////////////////////////////////////////

#include "ModuleCheck.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/TaintManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymbolManager.h"
#include "llvm/ADT/SetVector.h"

using namespace clang;
using namespace clang::ento;

REGISTER_SET_WITH_PROGRAMSTATE(ParamDeclSet, const MemRegion *)

ModuleChecker::ModuleChecker()
{
	ReadUncheckedBugType.reset(
		new BugType(this, "reduntant parameter check", "NASAC 2018 Coding Specification Check"));
	PassUncheckedParamBugType.reset(
		new BugType(this, "pass unchecked parameter", "NASAC 2018 Coding Specification Check"));
}

void ModuleChecker::reportReadUncheckedBug(
	ExplodedNode *ErrNode, CheckerContext &Ctx) const
{
	std::unique_ptr<BugReport> R = llvm::make_unique<BugReport>(
		*ReadUncheckedBugType, ReadUncheckedBugType->getName(), ErrNode);
	Ctx.emitReport(std::move(R));
}

void ModuleChecker::reportPassUncheckedParamBug(
	ExplodedNode *ErrNode, CheckerContext &Ctx) const
{
	std::unique_ptr<BugReport> R = llvm::make_unique<BugReport>(
		*PassUncheckedParamBugType, PassUncheckedParamBugType->getName(), ErrNode);
	Ctx.emitReport(std::move(R));
}

void ModuleChecker::checkBeginFunction(CheckerContext &Ctx) const
{
	ProgramStateRef State = Ctx.getState();
	StoreManager &SM = Ctx.getStoreManager();
	const LocationContext *LC = Ctx.getLocationContext();

	const FunctionDecl *FD = dyn_cast<FunctionDecl>(LC->getDecl());
	if (!FD)
	{
		return;
	}
	
	NeedCheckParam = false;

	for (int I = 0, N = FD->param_size(); I != N; I++)
	{
		const ParmVarDecl *PD = FD->getParamDecl(I);
		Loc PDLVal = SM.getLValueVar(PD, LC);
		if (NeedCheckParam) {
			SVal PDRVal = SM.getBinding(State->getStore(), PDLVal);
			if (SymbolRef PDRValSym = PDRVal.getAsSymbol())
			{
				State = State->addTaint(PDRValSym);
			}
		}
		else {
			if (const MemRegion *Region = PDLVal.getAsRegion()) {
				State = State->add<ParamDeclSet>(Region);
			}
		}

	}

	if (State != Ctx.getState())
	{
		Ctx.addTransition(State);
	}
}

static std::set<std::string> ReturnErrorFuncs = {
	"scanf"
};

inline bool NeedHandleError(const FunctionDecl *FD)
{
	return ReturnErrorFuncs.find(FD->getName().str()) != ReturnErrorFuncs.end();
}

void ModuleChecker::checkPostStmt(
	const CallExpr *CE, CheckerContext &Ctx) const
{

	ProgramStateRef State = Ctx.getState();

	SVal RetVal = Ctx.getSVal(CE);
	if (RetVal.isUnknownOrUndef())
	{
		return;
	}

	const SymbolRef RetSymbol = RetVal.getAsSymbol();
	if (!RetSymbol)
	{
		return;
	}

	// 对于需要错误处理的函数，将输出参数标记为污染
	const FunctionDecl *FD = CE->getDirectCallee();
	if (FD && NeedHandleError(FD))
	{
		for (int I = 0, N = CE->getNumArgs(); I != N; I++)
		{
			auto Arg = CE->getArg(I);
			SVal Val = Ctx.getSVal(Arg);
			if (auto Optional = Val.getAs<Loc>()) {
				auto Loc = Optional.getValue();
				SymbolRef Sym = Ctx.getStoreManager().getBinding(State->getStore(), Loc).getAsSymbol();
				if (Sym) {
					State = State->addTaint(Sym);
				}
			}
		}
		State = State->addTaint(RetSymbol);
	}

	if (State != Ctx.getState())
	{
		Ctx.addTransition(State);
	}
}

inline bool IsFunctionInSameModule(const FunctionDecl *FD, ASTContext *Ctx)
{
	auto &SM = Ctx->getSourceManager();
	return SM.getMainFileID() == SM.getFileID(FD->getBeginLoc());
}

BinaryOperator *FindParentOrExpr(Expr *S, CheckerContext &Ctx)
{
	ParentMap &PM = Ctx.getLocationContext()->getParentMap();
	Stmt *P = PM.getParent(S);
	Stmt *C = S;

	while (P)
	{
		if (P->getStmtClass() == Stmt::BinaryOperatorClass) {
			auto Bop = cast<BinaryOperator>(P);
			if (Bop->getOpcodeStr() == "||") {
				return Bop;
			}
		}
		C = P;
		P = PM.getParent(C);
	}

	return nullptr;
}

void ModuleChecker::checkPreStmt(
	const CallExpr *CE, CheckerContext &Ctx) const
{
	ProgramStateRef State = Ctx.getState();
	const LocationContext *LocCtx = Ctx.getLocationContext();

	const FunctionDecl *FD = CE->getDirectCallee();
	if (!FD)
	{
		return;
	}

	//if (FD->getName().contains("assert")) {
	//	if ()
	//	TryToReportReduntantParamCheck(OrExpr->getLHS(), Ctx);
	//	return;
	//}

	if (!IsFunctionInSameModule(FD, &Ctx.getASTContext()))
	{
		return;
	}

	for (int I = 0, N = CE->getNumArgs(); I != N; I++)
	{
		auto Arg = CE->getArg(I);
		SVal Val = Ctx.getSVal(Arg);
		if (Val.isUnknownOrUndef())
		{
			continue;
		}

		SymbolRef Symbol = Val.getAsSymbol();
		if (Symbol && State->isTainted(Symbol))
		{
			ExplodedNode *ErrNode = Ctx.generateNonFatalErrorNode();
			reportPassUncheckedParamBug(ErrNode, Ctx);
			break;
		}
	}

	if (State != Ctx.getState()) {
		Ctx.addTransition(State);
	}
}

void CollectDependentRegionValueSymbols(
	const SymbolRef SE, std::set<SymbolRef> &Set, CheckerContext &Ctx);

bool IsIfStmtCondition(Stmt *S, CheckerContext &Ctx)
{
	ParentMap &PM = Ctx.getLocationContext()->getParentMap();
	Stmt *P = PM.getParent(S);
	Stmt *C = S;

	while (P)
	{
		if (P->getStmtClass() == Stmt::IfStmtClass) {
			return C == cast<IfStmt>(P)->getCond();
		}
		C = P;
		P = PM.getParent(C);
	}

	return false;
}

void ModuleChecker::TryToRemoveTaintDueToCheck(const Stmt *Condition, CheckerContext &Ctx) const {
	ProgramStateRef State = Ctx.getState();

	SVal Val = Ctx.getSVal(Condition);
	if (Val.isUnknownOrUndef()) {
		return;
	}

	if (const SymbolRef Symbol = Val.getAsSymbol())
	{
		std::set<SymbolRef> Set;
		CollectDependentRegionValueSymbols(Symbol, Set, Ctx);
		if (!Set.empty())
		{
			for (auto I = Set.begin(), E = Set.end(); I != E; I++)
			{
				const SymbolRef DependentSymbol = *I;
				if (State->isTainted(DependentSymbol))
				{
					State = State->remove<TaintMap>(DependentSymbol);
				}
			}
		}
	}

	if (State != Ctx.getState())
	{
		Ctx.addTransition(State);
	}
}

void ModuleChecker::TryToReportReduntantParamCheck(const Stmt *Condition, CheckerContext &Ctx) const {
	ProgramStateRef State = Ctx.getState();

	SVal Val = Ctx.getSVal(Condition);
	if (Val.isUnknownOrUndef()) {
		return;
	}

	if (const SymbolRef Symbol = Val.getAsSymbol())
	{
		std::set<SymbolRef> Set;
		CollectDependentRegionValueSymbols(Symbol, Set, Ctx);
		if (!Set.empty())
		{
			for (auto I = Set.begin(), E = Set.end(); I != E; I++)
			{
				if (auto SRV = dyn_cast<SymbolRegionValue>(*I)) {
					if (State->contains<ParamDeclSet>(SRV->getRegion())) {
						ExplodedNode *ErrNode = Ctx.generateNonFatalErrorNode();
						reportReadUncheckedBug(ErrNode, Ctx);
					}
				}
				const SymbolRef DependentSymbol = *I;
				if (State->isTainted(DependentSymbol))
				{
					State = State->remove<TaintMap>(DependentSymbol);
				}
			}
		}
	}

	if (State != Ctx.getState())
	{
		Ctx.addTransition(State);
	}
}

void ModuleChecker::checkBranchCondition(
	const Stmt *Condition, CheckerContext &Ctx) const
{
	if (!IsIfStmtCondition(const_cast<Stmt *>(Condition), Ctx)) {
		return;
	}



	if (NeedCheckParam) {
		TryToRemoveTaintDueToCheck(Condition, Ctx);
	}
	else {
		TryToReportReduntantParamCheck(Condition, Ctx);
	}
	
}

void CollectDependentRegionValueSymbols(
	const SymbolRef SE, std::set<SymbolRef> &Set, CheckerContext &Ctx)
{
	for (auto I = SE->symbol_begin(), N = SE->symbol_end(); I != N; ++I)
	{
		const SymExpr *SubSE = *I;
		switch (SubSE->getKind())
		{
		case SymExpr::SymbolCastKind:
		{
			auto SC = cast<SymbolCast>(SubSE);
			CollectDependentRegionValueSymbols(SC->getOperand(), Set, Ctx);
			break;
		}
		case SymExpr::SymbolRegionValueKind:
		{
			auto SR = cast<SymbolRegionValue>(SubSE);
			Set.insert(SR);
			break;
		}
		case SymExpr::SymbolDerivedKind:
		{
			auto SD = cast<SymbolDerived>(SubSE);
			Set.insert(SD);
			break;
		}
		case SymExpr::SymbolConjuredKind:
		{
			auto SC = cast<SymbolConjured>(SubSE);
			Set.insert(SC);
			break;
		}
		case SymExpr::SymbolExtentKind:
		{
			auto SE = cast<SymbolExtent>(SubSE);
			Set.insert(SE);
			break;
		}
		case SymExpr::SymbolMetadataKind:
		{
			auto SM = cast<SymbolMetadata>(SubSE);
			Set.insert(SE);
			break;
		}
		}
	}
}