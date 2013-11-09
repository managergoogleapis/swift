//===--- TypeCheckType.cpp - Type Validation ------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements validation for Swift types, emitting semantic errors as
// appropriate and checking default initializer values.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "GenericTypeResolver.h"

#include "swift/AST/ASTWalker.h"
#include "swift/AST/ExprHandle.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeLoc.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

GenericTypeResolver::~GenericTypeResolver() { }

Type TypeChecker::getArraySliceType(SourceLoc loc, Type elementType) {
  if (!Context.getSliceDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 0);
    return Type();
  }

  return ArraySliceType::get(elementType, Context);
}

Type TypeChecker::getOptionalType(SourceLoc loc, Type elementType) {
  if (!Context.getOptionalDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 1);
    return Type();
  }

  return OptionalType::get(elementType, Context);
}

Type TypeChecker::resolveTypeInContext(TypeDecl *typeDecl,
                                       DeclContext *fromDC,
                                       bool isSpecialized,
                                       GenericTypeResolver *resolver) {
  PartialGenericTypeToArchetypeResolver defaultResolver(*this);
  if (!resolver)
    resolver = &defaultResolver;

  // If we found a generic parameter, map to the archetype if there is one.
  if (auto genericParam = dyn_cast<GenericTypeParamDecl>(typeDecl)) {
    return resolver->resolveGenericTypeParamType(
             genericParam->getDeclaredType()->castTo<GenericTypeParamType>());
  }

  // If we're referring to a generic type and no generic arguments have been
  // provided, and we are in the context of that generic type or one of its
  // extensions, imply the generic arguments
  if (auto nominal = dyn_cast<NominalTypeDecl>(typeDecl)) {
    if (nominal->getGenericParams() && !isSpecialized) {
      for (DeclContext *dc = fromDC; dc; dc = dc->getParent()) {
        switch (dc->getContextKind()) {
        case DeclContextKind::Module:
        case DeclContextKind::SourceFile:
        case DeclContextKind::TopLevelCodeDecl:
          break;

        case DeclContextKind::NominalTypeDecl:
          // If this is our nominal type, return its type within its context.
          if (cast<NominalTypeDecl>(dc) == nominal)
            return resolver->resolveTypeOfContext(nominal);
          continue;
            
        case DeclContextKind::ExtensionDecl:
          // If this is an extension of our nominal type, return the type
          // within the context of its extension.
          if (cast<ExtensionDecl>(dc)->getExtendedType()->getAnyNominal()
                == nominal)
            return resolver->resolveTypeOfContext(dc);
          continue;

        case DeclContextKind::AbstractClosureExpr:
        case DeclContextKind::AbstractFunctionDecl:
          continue;
        }

        break;
      }
    }
  }

  // If the type declaration itself is in a non-type context, no type
  // substitution is needed.
  DeclContext *ownerDC = typeDecl->getDeclContext();
  if (!ownerDC->isTypeContext()) {
    return typeDecl->getDeclaredType();
  }

  // Find the nearest enclosing type context around the context from which
  // we started our search.
  while (!fromDC->isTypeContext()) {
    fromDC = fromDC->getParent();
    assert(!fromDC->isModuleContext());
  }

  // If we found an associated type in an inherited protocol, the base
  // for our reference to this associated type is our own 'Self'.
  if (isa<AssociatedTypeDecl>(typeDecl)) {
    // If we found an associated type from within its protocol, resolve it
    // as a dependent member relative to Self if Self is still dependent.
    if (auto proto = dyn_cast<ProtocolDecl>(fromDC)) {
      auto selfTy
        = proto->getSelf()->getDeclaredType()->castTo<GenericTypeParamType>();
      auto baseTy = resolver->resolveGenericTypeParamType(selfTy);

      if (baseTy->isDependentType()) {
        return resolver->resolveDependentMemberType(baseTy, fromDC,
                                                    SourceRange(),
                                                    typeDecl->getName(),
                                                    SourceLoc());
      }
    }

    if (typeDecl->getDeclContext() != fromDC) {
      if (auto fromProto = dyn_cast<ProtocolDecl>(fromDC)) {
        return substMemberTypeWithBase(fromDC->getParentModule(),
                                       typeDecl->getDeclaredType(), typeDecl,
                                       fromProto->getSelf()->getArchetype());
      }
    }
  }

  // Walk up through the type scopes to find the context where the type
  // declaration was found. When we find it, substitute the appropriate base
  // type.
  Type ownerType = resolver->resolveTypeOfContext(ownerDC);
  auto ownerNominal = ownerType->getAnyNominal();
  assert(ownerNominal && "Owner must be a nominal type");
  for (; !fromDC->isModuleContext(); fromDC = fromDC->getParent()) {
    // Skip non-type contexts.
    if (!fromDC->isTypeContext())
      continue;

    // Search the type of this context and its supertypes.
    for (auto fromType = resolver->resolveTypeOfContext(fromDC);
         fromType;
         fromType = getSuperClassOf(fromType)) {
      // If the nominal type declaration of the context type we're looking at
      // matches the owner's nominal type declaration, this is how we found
      // the member type declaration. Substitute the type we're coming from as
      // the base of the member type to produce the projected type result.
      if (fromType->getAnyNominal() == ownerNominal) {
        return substMemberTypeWithBase(fromDC->getParentModule(),
                                       typeDecl->getDeclaredType(), typeDecl,
                                       fromType);
      }
    }
  }

  llvm_unreachable("Shouldn't have found this type");
}

/// Apply generic arguments to the given type.
Type TypeChecker::applyGenericArguments(Type type,
                                        SourceLoc loc,
                                        DeclContext *dc,
                                        MutableArrayRef<TypeLoc> genericArgs,
                                        GenericTypeResolver *resolver) {
  // Make sure we always have a resolver to use.
  PartialGenericTypeToArchetypeResolver defaultResolver(*this);
  if (!resolver)
    resolver = &defaultResolver;

  auto unbound = type->getAs<UnboundGenericType>();
  if (!unbound) {
    // FIXME: Highlight generic arguments and introduce a Fix-It to remove
    // them.
    diagnose(loc, diag::not_a_generic_type, type);

    // Just return the type; this provides better recovery anyway.
    return type;
  }

  // Make sure we have the right number of generic arguments.
  // FIXME: If we have fewer arguments than we need, that might be okay, if
  // we're allowed to deduce the remaining arguments from context.
  auto genericParams = unbound->getDecl()->getGenericParams();
  if (genericParams->size() != genericArgs.size()) {
    // FIXME: Highlight <...>.
    diagnose(loc, diag::type_parameter_count_mismatch,
             unbound->getDecl()->getName(),
             genericParams->size(), genericArgs.size(),
             genericArgs.size() < genericParams->size());
    diagnose(unbound->getDecl(), diag::generic_type_declared_here,
             unbound->getDecl()->getName());
    return nullptr;
  }

  // Validate the generic arguments and capture just the types.
  SmallVector<Type, 4> genericArgTypes;
  for (auto &genericArg : genericArgs) {
    // Validate the generic argument.
    if (validateType(genericArg, dc, /*allowUnboundGenerics=*/false,
                     resolver))
      return nullptr;

    genericArgTypes.push_back(genericArg.getType());
  }

  // Form the bound generic type
  BoundGenericType *BGT = BoundGenericType::get(unbound->getDecl(),
                                                unbound->getParent(),
                                                genericArgTypes);
  // Check protocol conformance.
  // FIXME: Should be able to check when there are type variables?
  if (!BGT->isDependentType() && !BGT->hasTypeVariable()) {
    // FIXME: Record that we're checking substitutions, so we can't end up
    // with infinite recursion.
    TypeSubstitutionMap Substitutions;
    ConformanceMap Conformance;
    unsigned Index = 0;
    for (Type Arg : BGT->getGenericArgs()) {
      auto GP = genericParams->getParams()[Index++];
      auto Archetype = GP.getAsTypeParam()->getArchetype();
      Substitutions[Archetype] = Arg;
    }

    if (checkSubstitutions(Substitutions, Conformance, dc, loc))
      return nullptr;
  }

  return BGT;
}

static Type applyGenericTypeReprArgs(TypeChecker &TC, Type type, SourceLoc loc,
                                     DeclContext *dc,
                                     MutableArrayRef<TypeRepr *> genericArgs,
                                     GenericTypeResolver *resolver) {
  SmallVector<TypeLoc, 8> args;
  for (auto tyR : genericArgs)
    args.push_back(tyR);
  Type ty = TC.applyGenericArguments(type, loc, dc, args, resolver);
  if (!ty)
    return ErrorType::get(TC.Context);
  return ty;
}


/// \brief Diagnose a use of an unbound generic type.
static void diagnoseUnboundGenericType(TypeChecker &tc, Type ty,SourceLoc loc) {
  tc.diagnose(loc, diag::generic_type_requires_arguments, ty);
  auto unbound = ty->castTo<UnboundGenericType>();
  tc.diagnose(unbound->getDecl()->getLoc(), diag::generic_type_declared_here,
              unbound->getDecl()->getName());
}

/// \brief Returns a valid type or ErrorType in case of an error.
static Type resolveTypeDecl(TypeChecker &TC, TypeDecl *typeDecl, SourceLoc loc,
                            DeclContext *dc,
                            MutableArrayRef<TypeRepr *> genericArgs,
                            bool allowUnboundGenerics,
                            GenericTypeResolver *resolver) {
  TC.validateDecl(typeDecl);

  Type type;
  if (dc) {
    // Resolve the type declaration to a specific type. How this occurs
    // depends on the current context and where the type was found.
    type = TC.resolveTypeInContext(typeDecl, dc, !genericArgs.empty(),
                                   resolver);
  } else {
    type = typeDecl->getDeclaredType();
  }

  if (type->is<UnboundGenericType>() &&
      genericArgs.empty() && !allowUnboundGenerics) {
    diagnoseUnboundGenericType(TC, type, loc);
    return ErrorType::get(TC.Context);
  }

  // If we found a generic parameter, try to resolve it.
  if (auto genericParam = type->getAs<GenericTypeParamType>()) {
    type = resolver->resolveGenericTypeParamType(genericParam);
  }

  if (!genericArgs.empty()) {
    // Apply the generic arguments to the type.
    type = applyGenericTypeReprArgs(TC, type, loc, dc, genericArgs, resolver);
  }

  assert(type);
  return type;
}

static llvm::PointerUnion<Type, Module *>
resolveIdentTypeComponent(TypeChecker &TC, DeclContext *DC,
                          MutableArrayRef<IdentTypeRepr::Component> components,
                          bool allowUnboundGenerics,
                          bool diagnoseErrors,
                          GenericTypeResolver *resolver) {
  auto &comp = components.back();
  if (!comp.isBound()) {
    auto parentComps = components.slice(0, components.size()-1);
    if (parentComps.empty()) {
      // Resolve the first component, which is the only one that requires
      // unqualified name lookup.
      UnqualifiedLookup Globals(comp.getIdentifier(), DC, &TC, comp.getIdLoc(),
                                /*TypeLookup*/true);

      // Process the names we found.
      llvm::PointerUnion<Type, Module *> current;
      bool isAmbiguous = false;
      for (const auto &result : Globals.Results) {
        // If we found a module, record it.
        if (result.Kind == UnqualifiedLookupResult::ModuleName) {
          // If we already found a name of some sort, it's ambiguous.
          if (!current.isNull()) {
            isAmbiguous = true;
            break;
          }

          // Save this result.
          current = result.getNamedModule();
          comp.setValue(result.getNamedModule());
          continue;
        }

        // Ignore non-type declarations.
        auto typeDecl = dyn_cast<TypeDecl>(result.getValueDecl());
        if (!typeDecl)
          continue;

        Type type = resolveTypeDecl(TC, typeDecl, comp.getIdLoc(),
                                    DC, comp.getGenericArgs(),
                                    allowUnboundGenerics,
                                    resolver);
        if (type->is<ErrorType>()) {
          comp.setValue(type);
          return type;
        }

        // If this is the first result we found, record it.
        if (current.isNull()) {
          current = type;
          comp.setValue(type);
          continue;
        }

        // Otherwise, check for an ambiguity.
        if (current.is<Module *>() || !current.get<Type>()->isEqual(type)) {
          isAmbiguous = true;
          break;
        }

        // We have a found multiple type aliases that refer to the sam thing.
        // Ignore the duplicate.
      }

      // If we found nothing, complain and fail.
      if (current.isNull()) {
        if (diagnoseErrors)
          TC.diagnose(comp.getIdLoc(), components.size() == 1 ?
                      diag::use_undeclared_type : diag::unknown_name_in_type,
                      comp.getIdentifier())
            .highlight(SourceRange(comp.getIdLoc(),
                                   components.back().getIdLoc()));
        Type ty = ErrorType::get(TC.Context);
        comp.setValue(ty);
        return ty;
      }

      // Complain about any ambiguities we detected.
      // FIXME: We could recover by looking at later components.
      if (isAmbiguous) {
        if (diagnoseErrors) {
          TC.diagnose(comp.getIdLoc(), diag::ambiguous_type_base,
                      comp.getIdentifier())
            .highlight(SourceRange(comp.getIdLoc(),
                                   components.back().getIdLoc()));
          for (auto Result : Globals.Results) {
            if (Globals.Results[0].hasValueDecl())
              TC.diagnose(Result.getValueDecl(), diag::found_candidate);
            else
              TC.diagnose(comp.getIdLoc(), diag::found_candidate);
          }
        }
        Type ty = ErrorType::get(TC.Context);
        comp.setValue(ty);
        return ty;
      }

    } else {
      llvm::PointerUnion<Type, Module *>
        parent = resolveIdentTypeComponent(TC, DC, parentComps,
                                           allowUnboundGenerics,
                                           diagnoseErrors,
                                           resolver);
      // If the last resolved component is a type, perform member type lookup.
      if (parent.is<Type>()) {
        // FIXME: Want the end of the back range.
        SourceRange parentRange(parentComps.front().getIdLoc(),
                                parentComps.back().getIdLoc());

        auto parentTy = parent.get<Type>();
        if (parentTy->is<ErrorType>())
          return parent.get<Type>();

        // If the parent is a dependent type, the member is a dependent member.
        if (parentTy->isDependentType()) {
          // Try to resolve the dependent member type to a specific associated
          // type.
          Type memberType = resolver->resolveDependentMemberType(
                                        parentTy, DC,
                                        parentRange,
                                        comp.getIdentifier(),
                                        comp.getIdLoc());
          assert(memberType && "Received null dependent member type");

          if (!comp.getGenericArgs().empty() && !memberType->is<ErrorType>()) {
            // FIXME: Highlight generic arguments and introduce a Fix-It to
            // remove them.
            if (diagnoseErrors)
              TC.diagnose(comp.getIdLoc(), diag::not_a_generic_type, memberType);

            // Drop the arguments.
          }

          comp.setValue(memberType);
          return memberType;
        }

        // Look for member types with the given name.
        auto memberTypes = TC.lookupMemberType(parentTy, comp.getIdentifier(),
                                               DC);

        // If we didn't find anything, complain.
        // FIXME: Typo correction!
        if (!memberTypes) {
          if (diagnoseErrors)
            TC.diagnose(comp.getIdLoc(), diag::invalid_member_type,
                        comp.getIdentifier(), parent.get<Type>())
              .highlight(parentRange);
          Type ty = ErrorType::get(TC.Context);
          comp.setValue(ty);
          return ty;
        }

        // Name lookup was ambiguous. Complain.
        // FIXME: Could try to apply generic arguments first, and see whether
        // that resolves things. But do we really want that to succeed?
        if (memberTypes.size() > 1) {
          if (diagnoseErrors)
            TC.diagnoseAmbiguousMemberType(parent.get<Type>(),
                                           parentRange,
                                           comp.getIdentifier(),
                                           comp.getIdLoc(),
                                           memberTypes);
          Type ty = ErrorType::get(TC.Context);
          comp.setValue(ty);
          return ty;
        }

        auto memberType = memberTypes.back().second;

        // If there are generic arguments, apply them now.
        if (!comp.getGenericArgs().empty())
          memberType = applyGenericTypeReprArgs(TC, memberType, comp.getIdLoc(),
                                                DC, comp.getGenericArgs(),
                                                resolver);

        comp.setValue(memberType);
        return memberType;
      }

      // Lookup into a module.
      auto module = parent.get<Module *>();
      LookupTypeResult foundModuleTypes =
        TC.lookupMemberType(ModuleType::get(module), comp.getIdentifier(), DC);

      // If we didn't find a type, complain.
      if (!foundModuleTypes) {
        // FIXME: Fully-qualified module name?
        if (diagnoseErrors)
          TC.diagnose(comp.getIdLoc(), diag::no_module_type, comp.getIdentifier(),
                      module->Name);
        Type ty = ErrorType::get(TC.Context);
        comp.setValue(ty);
        return ty;
      }

      // If lookup was ambiguous, complain.
      if (foundModuleTypes.isAmbiguous()) {
        if (diagnoseErrors) {
          TC.diagnose(comp.getIdLoc(), diag::ambiguous_module_type,
                      comp.getIdentifier(), module->Name);
          for (auto foundType : foundModuleTypes) {
            // Only consider type declarations.
            auto typeDecl = foundType.first;
            if (!typeDecl)
              continue;

            TC.diagnose(typeDecl, diag::found_candidate_type,
                        typeDecl->getDeclaredType());
          }
        }
        Type ty = ErrorType::get(TC.Context);
        comp.setValue(ty);
        return ty;
      }
      Type foundType = foundModuleTypes[0].second;

      // If there are generic arguments, apply them now.
      if (!comp.getGenericArgs().empty()) {
        foundType = applyGenericTypeReprArgs(TC, foundType, comp.getIdLoc(),
                                             DC, comp.getGenericArgs(),
                                             resolver);
      }

      comp.setValue(foundType);
    }
  }

  assert(comp.isBound());
  if (Type ty = comp.getBoundType())
    return ty;
  if (Module *mod = comp.getBoundModule())
    return mod;

  ValueDecl *VD = comp.getBoundDecl();
  auto typeDecl = dyn_cast<TypeDecl>(VD);
  if (!typeDecl) {
    if (diagnoseErrors) {
      TC.diagnose(comp.getIdLoc(), diag::use_non_type_value, VD->getName());
      TC.diagnose(VD, diag::use_non_type_value_prev, VD->getName());
    }
    Type ty = ErrorType::get(TC.Context);
    comp.setValue(ty);
    return ty;
  }

  Type type = resolveTypeDecl(TC, typeDecl, comp.getIdLoc(), nullptr,
                              comp.getGenericArgs(), allowUnboundGenerics,
                              resolver);
  comp.setValue(type);
  return type;
}

/// \brief Returns a valid type or ErrorType in case of an error.
Type TypeChecker::resolveIdentifierType(DeclContext *DC,
                                        IdentTypeRepr *IdType,
                                        bool allowUnboundGenerics,
                                        bool diagnoseErrors,
                                        GenericTypeResolver *resolver) {
  assert(resolver && "Missing generic type resolver");

  llvm::PointerUnion<Type, Module *>
    result = resolveIdentTypeComponent(*this, DC, IdType->Components,
                                       allowUnboundGenerics,
                                       diagnoseErrors,
                                       resolver);
  if (auto mod = result.dyn_cast<Module*>()) {
    if (diagnoseErrors)
      diagnose(IdType->Components.back().getIdLoc(),
               diag::use_module_as_type, mod->Name);
    Type ty = ErrorType::get(Context);
    IdType->Components.back().setValue(ty);
    return ty;
  }

  return result.get<Type>();
}

bool TypeChecker::validateType(TypeLoc &Loc, bool isSILType, DeclContext *DC,
                               bool allowUnboundGenerics,
                               GenericTypeResolver *resolver) {
  // FIXME: Verify that these aren't circular and infinite size.
  
  // If we've already validated this type, don't do so again.
  if (Loc.wasValidated())
    return Loc.isError();

  if (Loc.getType().isNull()) {
    Loc.setType(resolveType(Loc.getTypeRepr(), isSILType, DC,
                            allowUnboundGenerics, resolver),
                true);
    return Loc.isError();
  }

  Loc.setType(Loc.getType(), true);
  return Loc.isError();
}

namespace {
  class TypeResolver {
    TypeChecker &TC;
    ASTContext &Context;
    DeclContext *DC;
    bool AllowUnboundGenerics;
    GenericTypeResolver *Resolver;

  public:
    TypeResolver(TypeChecker &tc, DeclContext *DC, bool allowUnboundGenerics,
                 GenericTypeResolver *resolver)
      : TC(tc), Context(tc.Context), DC(DC),
        AllowUnboundGenerics(allowUnboundGenerics), Resolver(resolver) {
      assert(resolver);
    }

    Type resolveType(TypeRepr *repr, bool isSILType);

  private:
    Type resolveAttributedType(AttributedTypeRepr *repr, bool isSILType);
    Type resolveAttributedType(TypeAttributes &attrs, TypeRepr *repr,
                               bool isSILType);
    Type resolveFunctionType(FunctionTypeRepr *repr,
                             bool isSILType,
                             FunctionType::ExtInfo extInfo
                               = FunctionType::ExtInfo());
    Type resolveArrayType(ArrayTypeRepr *repr, bool isSILType);
    Type resolveOptionalType(OptionalTypeRepr *repr, bool isSILType);
    Type resolveTupleType(TupleTypeRepr *repr, bool isSILType);
    Type resolveProtocolCompositionType(ProtocolCompositionTypeRepr *repr,
                                        bool isSILType);
    Type resolveMetaTypeType(MetaTypeTypeRepr *repr, bool isSILType);
  };
}

Type TypeChecker::resolveType(TypeRepr *TyR, bool isSILType, DeclContext *DC,
                              bool allowUnboundGenerics,
                              GenericTypeResolver *resolver) {
  PrettyStackTraceTypeRepr stackTrace(Context, "resolving", TyR);

  // Make sure we always have a resolver to use.
  PartialGenericTypeToArchetypeResolver defaultResolver(*this);
  if (!resolver)
    resolver = &defaultResolver;

  TypeResolver typeResolver(*this, DC, allowUnboundGenerics, resolver);
  return typeResolver.resolveType(TyR, isSILType);
}

Type TypeResolver::resolveType(TypeRepr *repr, bool isSILType) {
  assert(repr && "Cannot validate null TypeReprs!");
  switch (repr->getKind()) {
  case TypeReprKind::Error:
    return ErrorType::get(Context);

  case TypeReprKind::Attributed:
    return resolveAttributedType(cast<AttributedTypeRepr>(repr), isSILType);

  case TypeReprKind::Ident:
    return TC.resolveIdentifierType(DC, cast<IdentTypeRepr>(repr),
                                    AllowUnboundGenerics,
                                    /*diagnoseErrors*/ true,
                                    Resolver);

  case TypeReprKind::Function:
    return resolveFunctionType(cast<FunctionTypeRepr>(repr), isSILType);

  case TypeReprKind::Array:
    return resolveArrayType(cast<ArrayTypeRepr>(repr), isSILType);

  case TypeReprKind::Optional:
    return resolveOptionalType(cast<OptionalTypeRepr>(repr), isSILType);

  case TypeReprKind::Tuple:
    return resolveTupleType(cast<TupleTypeRepr>(repr), isSILType);

  case TypeReprKind::Named:
    llvm_unreachable("NamedTypeRepr only shows up as an element of Tuple");

  case TypeReprKind::ProtocolComposition:
    return resolveProtocolCompositionType(
                                    cast<ProtocolCompositionTypeRepr>(repr),
                                          isSILType);

  case TypeReprKind::MetaType:
    return resolveMetaTypeType(cast<MetaTypeTypeRepr>(repr), isSILType);
  }
  llvm_unreachable("all cases should be handled");
}

Type TypeResolver::resolveAttributedType(AttributedTypeRepr *repr,
                                         bool isSILType) {
  // Copy the attributes, since we're about to start hacking on them.
  TypeAttributes attrs = repr->getAttrs();
  assert(!attrs.empty());

  return resolveAttributedType(attrs, repr->getTypeRepr(), isSILType);
}

Type TypeResolver::resolveAttributedType(TypeAttributes &attrs,
                                         TypeRepr *repr,
                                         bool isSILType) {
  // The type we're working with, in case we want to build it differently
  // based on the attributes we see.
  Type ty;

  // Pass down the variable function type attributes to the
  // function-type creator.
  static const TypeAttrKind FunctionAttrs[] = {
    TAK_auto_closure, TAK_objc_block, TAK_cc, TAK_thin, TAK_noreturn
  };

  bool hasFunctionAttr = false;
  for (auto i : FunctionAttrs)
    if (attrs.has(i)) {
      hasFunctionAttr = true;
      break;
    }

  // Function attributes require a syntactic function type.
  FunctionTypeRepr *fnRepr = dyn_cast<FunctionTypeRepr>(repr);
  if (hasFunctionAttr && fnRepr) {
    // auto_closures must take () syntactically.
    if (attrs.has(TAK_auto_closure)) {
      auto input = fnRepr->getArgsTypeRepr();
      auto inputTuple = dyn_cast<TupleTypeRepr>(input);
      if (inputTuple == 0 || !inputTuple->getElements().empty()) {
        TC.diagnose(attrs.getLoc(TAK_auto_closure),
                    diag::autoclosure_function_input_nonunit)
          .highlight(input->getSourceRange());
      }
    }

    // Resolve the function type directly with these attributes.
    FunctionType::ExtInfo extInfo(attrs.hasCC()
                                    ? attrs.getAbstractCC()
                                    : AbstractCC::Freestanding,
                                  attrs.has(TAK_thin),
                                  attrs.has(TAK_noreturn),
                                  attrs.has(TAK_auto_closure),
                                  attrs.has(TAK_objc_block));
    ty = resolveFunctionType(fnRepr, isSILType, extInfo);

    for (auto i : FunctionAttrs)
      attrs.clearAttribute(i);
    attrs.cc = Nothing;

  } else if (hasFunctionAttr) {
    for (auto i : FunctionAttrs) {
      if (attrs.has(i)) {
        TC.diagnose(attrs.getLoc(i), diag::attribute_requires_function_type);
        attrs.clearAttribute(i);
      }
    }
  } 

  // If we didn't build the type differently above, build it normally now.
  if (!ty) ty = resolveType(repr, isSILType);

  if (ty->is<ErrorType>())
    return ty;

  // In SIL, handle @sil_self, which extracts the Self type of a protocol.
  if (attrs.has(TAK_sil_self)) {
    if (auto protoTy = ty->getAs<ProtocolType>()) {
      ty = protoTy->getDecl()->getSelf()->getArchetype();
    } else {
      TC.diagnose(attrs.getLoc(TAK_sil_self), diag::sil_self_non_protocol, ty)
        .highlight(repr->getSourceRange());
    }
    attrs.clearAttribute(TAK_sil_self);
  }

  if (attrs.has(TAK_inout)) {
    LValueType::Qual quals;
    ty = LValueType::get(ty, quals, Context);
    attrs.clearAttribute(TAK_inout);
  }

  // In SIL translation units *only*, permit @weak and @unowned to
  // apply directly to types.
  if (attrs.hasOwnership() && ty->hasReferenceSemantics()) {
    if (auto SF = DC->getParentSourceFile()) {
      if (SF->Kind == SourceFile::SIL) {
        ty = ReferenceStorageType::get(ty, attrs.getOwnership(), Context);
        attrs.clearOwnership();
      }
    }
  }

  // Diagnose @local_storage in nested positions.
  if (attrs.has(TAK_local_storage)) {
    assert(DC->getParentSourceFile()->Kind == SourceFile::SIL);
    TC.diagnose(attrs.getLoc(TAK_local_storage),diag::sil_local_storage_nested);
    attrs.clearAttribute(TAK_local_storage);
  }

  for (unsigned i = 0; i != TypeAttrKind::TAK_Count; ++i)
    if (attrs.has((TypeAttrKind)i))
      TC.diagnose(attrs.getLoc((TypeAttrKind)i),
                  diag::attribute_does_not_apply_to_type);

  return ty;
}

Type TypeResolver::resolveFunctionType(FunctionTypeRepr *repr,
                                       bool isSILType,
                                       FunctionType::ExtInfo extInfo) {
  Type inputTy = resolveType(repr->getArgsTypeRepr(), isSILType);
  if (inputTy->is<ErrorType>())
    return inputTy;
  Type outputTy = resolveType(repr->getResultTypeRepr(), isSILType);
  if (outputTy->is<ErrorType>())
    return outputTy;
  return FunctionType::get(inputTy, outputTy, extInfo, Context);
}

Type TypeResolver::resolveArrayType(ArrayTypeRepr *repr, bool isSILType) {
  // FIXME: diagnose non-materializability of element type!
  Type baseTy = resolveType(repr->getBase(), isSILType);
  if (baseTy->is<ErrorType>())
    return baseTy;
  
  if (ExprHandle *sizeEx = repr->getSize()) {
    // FIXME: We don't support fixed-length arrays yet.
    // FIXME: We need to check Size! (It also has to be convertible to int).
    TC.diagnose(repr->getBrackets().Start, diag::unsupported_fixed_length_array)
      .highlight(sizeEx->getExpr()->getSourceRange());
    return ErrorType::get(Context);
  }

  auto sliceTy = TC.getArraySliceType(repr->getBrackets().Start, baseTy);
  if (!sliceTy)
    return ErrorType::get(Context);

  return sliceTy;
}

Type TypeResolver::resolveOptionalType(OptionalTypeRepr *repr, bool isSILType) {
  // The T in T? is a generic type argument and therefore always an AST type.
  // FIXME: diagnose non-materializability of element type!
  Type baseTy = resolveType(repr->getBase(), false);
  if (baseTy->is<ErrorType>())
    return baseTy;

  auto optionalTy = TC.getOptionalType(repr->getQuestionLoc(), baseTy);
  if (!optionalTy)
    return ErrorType::get(Context);

  return optionalTy;
}

Type TypeResolver::resolveTupleType(TupleTypeRepr *repr, bool isSILType) {
  SmallVector<TupleTypeElt, 8> elements;
  elements.reserve(repr->getElements().size());
  for (auto tyR : repr->getElements()) {
    if (NamedTypeRepr *namedTyR = dyn_cast<NamedTypeRepr>(tyR)) {
      Type ty = resolveType(namedTyR->getTypeRepr(), isSILType);
      if (ty->is<ErrorType>())
        return ty;
      elements.push_back(TupleTypeElt(ty, namedTyR->getName()));
    } else {
      Type ty = resolveType(tyR, isSILType);
      if (ty->is<ErrorType>())
        return ty;
      elements.push_back(TupleTypeElt(ty));
    }
  }

  if (repr->hasEllipsis()) {
    Type baseTy = elements.back().getType();
    Type fullTy = TC.getArraySliceType(repr->getEllipsisLoc(), baseTy);
    Identifier name = elements.back().getName();
    // FIXME: Where are we rejecting default arguments for variadic
    // parameters?
    elements.back() = TupleTypeElt(fullTy, name, DefaultArgumentKind::None,
                                   true);
  }

  return TupleType::get(elements, Context);
}

Type TypeResolver::resolveProtocolCompositionType(
                                         ProtocolCompositionTypeRepr *repr,
                                                  bool isSILType) {
  SmallVector<Type, 4> ProtocolTypes;
  for (auto tyR : repr->getProtocols()) {
    Type ty = TC.resolveType(tyR, isSILType, DC,
                             /*allowUnboundGenerics=*/false, Resolver);
    if (ty->is<ErrorType>())
      return ty;
    if (!ty->isExistentialType()) {
      TC.diagnose(tyR->getStartLoc(), diag::protocol_composition_not_protocol,
                  ty);
      continue;
    }

    // The special DynamicLookup protocol can't be part of a protocol
    // composition.
    if (auto protoTy = ty->getAs<ProtocolType>()){
      if (protoTy->getDecl()->isSpecificProtocol(
              KnownProtocolKind::DynamicLookup)) {
        TC.diagnose(tyR->getStartLoc(),
                    diag::protocol_composition_dynamic_lookup);
        continue;
      }
    }

    ProtocolTypes.push_back(ty);
  }
  return ProtocolCompositionType::get(Context, ProtocolTypes);
}

Type TypeResolver::resolveMetaTypeType(MetaTypeTypeRepr *repr, bool isSILType) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  Type ty = resolveType(repr->getBase(), false);
  if (ty->is<ErrorType>())
    return ty;
  return MetaTypeType::get(ty, Context);
}

Type TypeChecker::transformType(Type type,
                                const std::function<Type(Type)> &fn) {
  return type.transform(Context, fn);
}

Type TypeChecker::substType(Module *module, Type type,
                            TypeSubstitutionMap &Substitutions,
                            bool IgnoreMissing) {
  return type.subst(module, Substitutions, IgnoreMissing, this);
}

Type TypeChecker::substMemberTypeWithBase(Module *module, Type T,
                                          ValueDecl *Member, Type BaseTy) {
  if (!BaseTy)
    return T;

  return BaseTy->getTypeOfMember(module, Member, this, T);
}

Type TypeChecker::getSuperClassOf(Type type) {
  return type->getSuperclass(this);
}

Type TypeChecker::resolveMemberType(DeclContext *dc, Type type,
                                    Identifier name) {
  LookupTypeResult memberTypes = lookupMemberType(type, name, dc);
  if (!memberTypes)
    return Type();

  // FIXME: Detect ambiguities here?
  return memberTypes.back().second;
}

static void lookupStdlibTypes(TypeChecker &TC,
                              Module *Stdlib,
                              ArrayRef<Identifier> TypeNames,
                              llvm::DenseSet<CanType> &Types) {
  SmallVector<ValueDecl *, 4> Results;
  for (Identifier Id : TypeNames) {
    Stdlib->lookupValue({}, Id, NLKind::UnqualifiedLookup, Results);
    for (auto *VD : Results) {
      if (auto *TD = dyn_cast<TypeDecl>(VD)) {
        TC.validateDecl(TD);
        Types.insert(TD->getDeclaredType()->getCanonicalType());
      }
    }
    Results.clear();
  }
}

bool isClassOrObjCProtocol(TypeChecker &TC, Type T) {
  if (T->is<ClassType>())
    return true;

  SmallVector<ProtocolDecl *, 4> Protocols;
  if (T->isExistentialType(Protocols)) {
    if (Protocols.empty()) {
      // protocol<> is not @objc.
      return false;
    }
    // Check that all protocols are @objc.
    for (auto PD : Protocols) {
      if (!PD->getAttrs().isObjC())
        return false;
    }
    return true;
  }

  return false;
}

static bool isParamRepresentableInObjC(TypeChecker &TC,
                                       const DeclContext *DC,
                                       const Pattern *P) {
  auto *TP = dyn_cast<TypedPattern>(P);
  if (!TP)
    return false;
  if (!TC.isRepresentableInObjC(DC, TP->getType()))
    return false;
  auto *SubPattern = TP->getSubPattern();
  return isa<NamedPattern>(SubPattern) || isa<AnyPattern>(SubPattern);
}

static bool isParamPatternRepresentableInObjC(TypeChecker &TC,
                                              const DeclContext *DC,
                                              const Pattern *P) {
  if (auto *TP = dyn_cast<TuplePattern>(P)) {
    for (auto TupleElt : TP->getFields()) {
      if (!isParamRepresentableInObjC(TC, DC, TupleElt.getPattern()))
        return false;
    }
    return true;
  }
  auto *PP = cast<ParenPattern>(P);
  return isParamRepresentableInObjC(TC, DC, PP->getSubPattern());
}

bool TypeChecker::isRepresentableInObjC(const AbstractFunctionDecl *AFD) {
  if (auto *FD = dyn_cast<FuncDecl>(AFD)) {
    if (!FD->getGetterOrSetterDecl()) {
      unsigned ExpectedParamPatterns = 1;
      if (FD->getImplicitSelfDecl())
        ExpectedParamPatterns++;
      if (FD->getArgParamPatterns().size() != ExpectedParamPatterns)
        return false;
    }
  }

  if (!isParamPatternRepresentableInObjC(*this, AFD,
                                         AFD->getArgParamPatterns()[1]))
    return false;

  if (auto FD = dyn_cast<FuncDecl>(AFD)) {
    Type Result = FD->getResultType();
    if (!Result->isVoid() && !isRepresentableInObjC(FD, Result))
      return false;
  }

  return true;
}

bool TypeChecker::isRepresentableInObjC(const VarDecl *VD) {
  return isRepresentableInObjC(VD->getDeclContext(), VD->getType());
}

bool TypeChecker::isTriviallyRepresentableInObjC(const DeclContext *DC,
                                                 Type T) {
  if (isClassOrObjCProtocol(*this, T))
    return true;

  if (auto MTT = T->getAs<MetaTypeType>()) {
    if (isClassOrObjCProtocol(*this, MTT->getInstanceType()))
      return true;
  }

  if (auto NTD = T->getAnyNominal()) {
    // If the type was imported from Clang, it is representable in Objective-C.
    if (NTD->hasClangNode())
      return true;
  }

  fillObjCRepresentableTypeCache(DC);
  if (ObjCMappedTypes.count(T->getCanonicalType()))
    return true;

  // An UnsafePointer<T> is representable in Objective-C if T is a trivially
  // mapped type, or T is a representable UnsafePointer<U> type.
  while (true) {
    if (auto BGT = T->getAs<BoundGenericType>()) {
      if (BGT->getDecl() == getUnsafePointerDecl(DC)) {
        T = BGT->getGenericArgs()[0];
        continue;
      }
    }

    if (ObjCMappedTypes.count(T->getCanonicalType()))
      return true;
    break;
  }

  return false;
}

bool TypeChecker::isRepresentableInObjC(const DeclContext *DC, Type T) {
  if (isTriviallyRepresentableInObjC(DC, T))
    return true;

  if (auto FT = T->getAs<FunctionType>()) {
    if (FT->isThin())
      return false;
    Type Input = FT->getInput();
    if (auto InputTuple = Input->getAs<TupleType>()) {
      for (auto &Elt : InputTuple->getFields()) {
        if (!isRepresentableInObjC(DC, Elt.getType()))
          return false;
      }
    } else if (!isRepresentableInObjC(DC, Input)) {
      return false;
    }

    Type Result = FT->getResult();
    if (!Result->isVoid() && !isRepresentableInObjC(DC, Result))
      return false;

    return true;
  }

  fillObjCRepresentableTypeCache(DC);
  if (ObjCRepresentableTypes.count(T->getCanonicalType()))
    return true;

  return false;
}

void TypeChecker::fillObjCRepresentableTypeCache(const DeclContext *DC) {
  if (!ObjCMappedTypes.empty())
    return;

  SmallVector<Identifier, 16> StdlibTypeNames;

  StdlibTypeNames.push_back(Context.getIdentifier("COpaquePointer"));
#define MAP_BUILTIN_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME) \
  StdlibTypeNames.push_back(Context.getIdentifier(#SWIFT_TYPE_NAME));
#include "swift/ClangImporter/BuiltinMappedTypes.def"

  Module *Stdlib = getStdlibModule(DC);
  lookupStdlibTypes(*this, Stdlib, StdlibTypeNames, ObjCMappedTypes);

  StdlibTypeNames.clear();
#define BRIDGE_TYPE(BRIDGED_MODULE, BRIDGED_TYPE,                          \
                    NATIVE_MODULE, NATIVE_TYPE)                            \
  if (Context.getIdentifier(#NATIVE_MODULE) == Context.StdlibModuleName) \
    StdlibTypeNames.push_back(Context.getIdentifier(#NATIVE_TYPE));
#include "swift/SIL/BridgedTypes.def"

  lookupStdlibTypes(*this, Stdlib, StdlibTypeNames, ObjCRepresentableTypes);

  if (auto *DynamicLookup =
         Context.getProtocol(KnownProtocolKind::DynamicLookup)) {
    validateDecl(DynamicLookup);
    CanType DynamicLookupType =
        DynamicLookup->getDeclaredType()->getCanonicalType();
    ObjCMappedTypes.insert(DynamicLookupType);
    ObjCMappedTypes.insert(
        MetaTypeType::get(DynamicLookupType, Context)->getCanonicalType());
  }
}

