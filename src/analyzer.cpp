#include <stdio.h>
#include <iostream>
#include <stack>
#include <unordered_map>
#include <sstream>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/StmtVisitor.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include "analyzer.h"
#include "InitializerChecker.h"

using namespace llvm;
using namespace clang;

std::set<const clang::ObjCContainerDecl *> MethodUtility::enumerateContainers(const clang::ObjCMessageExpr *expr) {
    std::set<const ObjCContainerDecl *> set;
    Selector selector = expr->getSelector();
    
    if (expr->getInstanceReceiver()) {
        const Expr *receiver = expr->getInstanceReceiver();
        const Type *receiverType = receiver->getType().getTypePtr();

        if (receiverType->isObjCObjectPointerType()) {
            const ObjCObjectPointerType *objectPointerType = receiverType->getAsObjCInterfacePointerType();
            if (objectPointerType) {
                const ObjCInterfaceDecl *interface = objectPointerType->getInterfaceDecl();
    
                while (interface) {
                    if (interface->getInstanceMethod(selector)) {
                        set.insert(interface);
                        interface = nullptr; // break
                    } else {
                        for (auto protocol : interface->protocols()) {
                            if (protocol->getInstanceMethod(selector)) {
                                set.insert(protocol);
                                interface = nullptr; //break
                            }
                        }
                    }
                    
                    if (interface) {
                        interface = interface->getSuperClass();
                    }
                }
            }
            
            if (receiverType->isObjCQualifiedIdType()) {
                const ObjCObjectPointerType *pointerType = receiverType->getAsObjCQualifiedIdType();
                
                auto protocols = pointerType->getNumProtocols();
                for (unsigned index = 0; index < protocols; index++) {
                    auto protocol = pointerType->getProtocol(index);
                    
                    if (protocol->getInstanceMethod(selector)) {
                        set.insert(protocol);
                    }
                }
            }
        }
    }
    
    if (expr->isClassMessage()) {
        const ObjCInterfaceDecl *interface = expr->getMethodDecl()->getClassInterface();
        if (interface) {
            set.insert(interface);
        }
    }
    
    return set;
}

QualType NullabilityCheckContext::getReturnType() const {
    if (BlockExpr) {
        const Type *type = BlockExpr->getType().getTypePtr();
        const BlockPointerType *blockType = llvm::dyn_cast<BlockPointerType>(type);
        const FunctionProtoType *funcType = llvm::dyn_cast<FunctionProtoType>(blockType->getPointeeType().getTypePtr());
        
        return funcType->getReturnType();
    } else {
        return MethodDecl.getReturnType();
    }
}

class NullCheckVisitor : public RecursiveASTVisitor<NullCheckVisitor> {
public:
    NullCheckVisitor(ASTContext &context, bool debug, Filter &filter) : _ASTContext(context), _Debug(debug), _Filter(filter) {}

    bool VisitDecl(Decl *decl) {
        ObjCMethodDecl *methodDecl = llvm::dyn_cast<ObjCMethodDecl>(decl);
        if (methodDecl) {
            if (methodDecl->hasBody()) {
                auto map = std::shared_ptr<VariableNullabilityMapping>(new VariableNullabilityMapping);
                
                std::shared_ptr<VariableNullabilityEnvironment> varEnv(new VariableNullabilityEnvironment(_ASTContext, map));
                ExpressionNullabilityCalculator nullabilityCalculator(_ASTContext, varEnv);
                VariableNullabilityPropagation propagation(nullabilityCalculator, varEnv);
                
                propagation.propagate(methodDecl);

                if (_Debug) {
                    for (auto it : *map) {
                        const VarDecl *decl = it.first;
                        NullabilityKind kind = it.second.getNullability();
                        
                        std::string x = "";
                        switch (kind) {
                            case NullabilityKind::Unspecified:
                                x = "unspecified";
                                break;
                            case NullabilityKind::NonNull:
                                x = "nonnull";
                                break;
                            case NullabilityKind::Nullable:
                                x = "nullable";
                                break;
                        }
                        
                        DiagnosticsEngine &engine = _ASTContext.getDiagnostics();
                        unsigned id = engine.getCustomDiagID(DiagnosticsEngine::Remark, "Variable nullability: %0");
                        engine.Report(decl->getLocation(), id) << x;
                    }
                }
                
                NullabilityCheckContext checkContext(*(methodDecl->getClassInterface()), *methodDecl);

                MethodBodyChecker checker(_ASTContext, checkContext, nullabilityCalculator, varEnv, _Filter);
                checker.TraverseStmt(methodDecl->getBody());
            }
        }
        
        return true;
    }

private:
    ASTContext &_ASTContext;
    bool _Debug;
    Filter &_Filter;
};

class InitializerCheckerVisitor : public RecursiveASTVisitor<InitializerCheckerVisitor> {
    ASTContext &_ASTContext;
    bool _Debug;
    Filter &_Filter;
    
public:
    InitializerCheckerVisitor(ASTContext &astContext, bool debug, Filter &filter) : _ASTContext(astContext), _Debug(debug), _Filter(filter) {}
    
    bool TraverseObjCImplementationDecl(ObjCImplementationDecl *decl) {
        InitializerChecker checker(_ASTContext, decl);
        
        std::set<std::string> subject{ decl->getNameAsString() };
        if (_Filter.testClassName(subject)) {
            for (auto methodDecl : decl->methods()) {
                auto uninitializedVars = checker.check(methodDecl);
                
                if (!uninitializedVars.empty()) {
                    std::stringstream names;
                    bool first = true;
                    for (auto info : uninitializedVars) {
                        if (first) {
                            first = false;
                        } else {
                            names << ", ";
                        }
                        names << info->getIvarDecl()->getNameAsString();
                    }
                    
                    DiagnosticsEngine &engine = _ASTContext.getDiagnostics();
                    unsigned id = engine.getCustomDiagID(DiagnosticsEngine::Warning, "Nonnull ivar should be initialized: %0");
                    engine.Report(methodDecl->getLocation(), id) << names.str();
                }
            }
        }
        
        return true;
    }
};

class NullCheckConsumer : public ASTConsumer {
public:
    explicit NullCheckConsumer(bool debug, Filter &filter) : ASTConsumer(), _Debug(debug), _Filter(filter) {
    }
    
    virtual void HandleTranslationUnit(clang::ASTContext &Context) {
        NullCheckVisitor visitor(Context, _Debug, _Filter);
        visitor.TraverseDecl(Context.getTranslationUnitDecl());
        
        InitializerCheckerVisitor initializerCheckerVisitor(Context, _Debug, _Filter);
        initializerCheckerVisitor.TraverseDecl(Context.getTranslationUnitDecl());
    }
    
private:
    bool _Debug;
    Filter &_Filter;
};

std::unique_ptr<clang::ASTConsumer> NullCheckAction::CreateASTConsumer(CompilerInstance &Compiler, StringRef InFile) {
    return std::unique_ptr<ASTConsumer>(new NullCheckConsumer(Debug, _Filter));
}



