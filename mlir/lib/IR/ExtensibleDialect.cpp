//===- ExtensibleDialect.cpp - Extensible dialect ---------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Dialects that can register new operations/types/attributes at runtime.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/ExtensibleDialect.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/IsDynamicInterfaces.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// Dynamic type
//===----------------------------------------------------------------------===//

std::unique_ptr<DynamicTypeDefinition>
DynamicTypeDefinition::get(Dialect *dialect, llvm::StringRef name,
                           VerifierFn &&verifier) {
  auto *typeDef = new DynamicTypeDefinition(dialect, name);

  auto parser = [](DialectAsmParser &parser,
                   std::vector<Attribute> &parsedParams) {
    // No parameters
    if (parser.parseOptionalLess() || !parser.parseOptionalGreater())
      return success();

    Attribute attr;
    if (parser.parseAttribute(attr))
      return failure();
    parsedParams.push_back(attr);

    while (parser.parseOptionalGreater()) {
      Attribute attr;
      if (parser.parseComma() || parser.parseAttribute(attr))
        return failure();
      parsedParams.push_back(attr);
    }

    return success();
  };

  auto printer = [](DialectAsmPrinter &printer, ArrayRef<Attribute> params) {
    if (params.empty())
      return;

    printer << "<";
    llvm::interleaveComma(params, printer.getStream());
    printer << ">";
  };

  typeDef->verifier = std::move(verifier);
  typeDef->parser = std::move(parser);
  typeDef->printer = std::move(printer);

  return std::unique_ptr<DynamicTypeDefinition>(typeDef);
}

std::unique_ptr<DynamicTypeDefinition>
DynamicTypeDefinition::get(Dialect *dialect, llvm::StringRef name,
                           VerifierFn &&verifier, ParserFn &&parser,
                           PrinterFn &&printer) {
  return std::unique_ptr<DynamicTypeDefinition>(
      new DynamicTypeDefinition(dialect, name, std::move(verifier),
                                std::move(parser), std::move(printer)));
}

DynamicTypeDefinition::DynamicTypeDefinition(Dialect *dialect,
                                             llvm::StringRef nameRef,
                                             VerifierFn &&verifier,
                                             ParserFn &&parser,
                                             PrinterFn &&printer)
    : name(nameRef), dialect(dialect), verifier(std::move(verifier)),
      parser(std::move(parser)), printer(std::move(printer)),
      typeID(dialect->getContext()->allocateTypeID()),
      ctx(dialect->getContext()) {
  assert(!nameRef.contains('.') &&
         "name should not be prefixed by the dialect name");
}

DynamicTypeDefinition::DynamicTypeDefinition(Dialect *dialect,
                                             llvm::StringRef nameRef)
    : name(nameRef), dialect(dialect),
      typeID(dialect->getContext()->allocateTypeID()),
      ctx(dialect->getContext()) {
  assert(!nameRef.contains('.') &&
         "name should not be prefixed by the dialect name");
}

void DynamicTypeDefinition::registerInTypeUniquer() {
  detail::TypeUniquer::registerType<DynamicType>(&getContext(), getTypeID());
}

namespace mlir {
namespace detail {
/// Storage of DynamicType.
/// Contains a pointer to the type definition and type parameters.
struct DynamicTypeStorage : public TypeStorage {

  using KeyTy = std::pair<DynamicTypeDefinition *, ArrayRef<Attribute>>;

  explicit DynamicTypeStorage(DynamicTypeDefinition *typeDef,
                              ArrayRef<Attribute> params)
      : typeDef(typeDef), params(params) {}

  bool operator==(const KeyTy &key) const {
    return typeDef == key.first && params == key.second;
  }

  static llvm::hash_code hashKey(const KeyTy &key) {
    return llvm::hash_value(key);
  }

  static DynamicTypeStorage *construct(TypeStorageAllocator &alloc,
                                       const KeyTy &key) {
    return new (alloc.allocate<DynamicTypeStorage>())
        DynamicTypeStorage(key.first, alloc.copyInto(key.second));
  }

  /// Definition of the type.
  DynamicTypeDefinition *typeDef;

  /// The type parameters.
  ArrayRef<Attribute> params;
};
} // namespace detail
} // namespace mlir

DynamicType DynamicType::get(DynamicTypeDefinition *typeDef,
                             ArrayRef<Attribute> params) {
  auto &ctx = typeDef->getContext();
  return detail::TypeUniquer::getWithTypeID<DynamicType>(
      &ctx, typeDef->getTypeID(), typeDef, params);
}

DynamicType
DynamicType::getChecked(function_ref<InFlightDiagnostic()> emitError,
                        DynamicTypeDefinition *typeDef,
                        ArrayRef<Attribute> params) {
  if (failed(typeDef->verify(emitError, params)))
    return {};
  return get(typeDef, params);
}

DynamicTypeDefinition *DynamicType::getTypeDef() { return getImpl()->typeDef; }

ArrayRef<Attribute> DynamicType::getParams() { return getImpl()->params; }

bool DynamicType::classof(Type type) {
  return type.isa<IsDynamicTypeInterface>();
}

ParseResult DynamicType::parse(DialectAsmParser &parser,
                               DynamicTypeDefinition *typeDef,
                               DynamicType &parsedType) {
  std::vector<Attribute> params;
  if (failed(typeDef->parser(parser, params)))
    return failure();
  auto emitError = [&]() {
    return parser.emitError(parser.getCurrentLocation());
  };
  parsedType = DynamicType::getChecked(emitError, typeDef, params);
  return success();
}

void DynamicType::print(DialectAsmPrinter &printer) {
  printer << getTypeDef()->getName();
  getTypeDef()->printer(printer, getParams());
}

//===----------------------------------------------------------------------===//
// Dynamic operation
//===----------------------------------------------------------------------===//

DynamicOpDefinition::DynamicOpDefinition(
    StringRef name, Dialect *dialect,
    AbstractOperation::VerifyInvariantsFn &&verifyFn,
    AbstractOperation::ParseAssemblyFn &&parseFn,
    AbstractOperation::PrintAssemblyFn &&printFn)
    : typeID(dialect->getContext()->allocateTypeID()),
      name((dialect->getNamespace() + "." + name).str()), dialect(dialect),
      verifyFn(std::move(verifyFn)), parseFn(std::move(parseFn)),
      printFn(std::move(printFn)) {}

std::unique_ptr<DynamicOpDefinition>
DynamicOpDefinition::get(StringRef name, Dialect *dialect,
                         AbstractOperation::VerifyInvariantsFn &&verifyFn) {
  auto parseFn = [](OpAsmParser &parser, OperationState &result) {
    parser.emitError(parser.getCurrentLocation(),
                     "dynamic operation do not define any parser function");
    return failure();
  };

  auto printFn = [](Operation *op, OpAsmPrinter &printer) {
    printer.printGenericOp(op);
  };

  return std::unique_ptr<DynamicOpDefinition>(
      new DynamicOpDefinition(name, dialect, std::move(verifyFn),
                              std::move(parseFn), std::move(printFn)));
}

std::unique_ptr<DynamicOpDefinition>
DynamicOpDefinition::get(StringRef name, Dialect *dialect,
                         AbstractOperation::VerifyInvariantsFn &&verifyFn,
                         AbstractOperation::ParseAssemblyFn &&parseFn,
                         AbstractOperation::PrintAssemblyFn &&printFn) {
  assert(!name.contains('.') &&
         "name should not be prefixed by the dialect name");
  return std::unique_ptr<DynamicOpDefinition>(
      new DynamicOpDefinition(name, dialect, std::move(verifyFn),
                              std::move(parseFn), std::move(printFn)));
}

//===----------------------------------------------------------------------===//
// Extensible dialect
//===----------------------------------------------------------------------===//

/// Interface that can only be implemented by extensible dialects.
/// The interface is used to check if a dialect is extensible or not.
class IsExtensibleDialect : public DialectInterface::Base<IsExtensibleDialect> {
public:
  IsExtensibleDialect(Dialect *dialect) : Base(dialect) {}
};

ExtensibleDialect::ExtensibleDialect(StringRef name, MLIRContext *ctx,
                                     TypeID typeID)
    : Dialect(name, ctx, typeID) {
  addInterfaces<IsExtensibleDialect>();
}

void ExtensibleDialect::addDynamicType(
    std::unique_ptr<DynamicTypeDefinition> &&type) {
  auto *typePtr = type.get();
  auto typeID = type->getTypeID();
  auto name = type->getName();
  auto *dialect = type->getDialect();

  assert(dialect == this &&
         "trying to register a dynamic type in the wrong dialect");

  // If a type with the same name is already defined, fail.
  auto registered = dynTypes.try_emplace(typeID, std::move(type)).second;
  assert(registered && "generated TypeID was not unique");

  registered = nameToDynTypes.insert({name, typePtr}).second;
  assert(registered &&
         "Trying to create a new dynamic type with an existing name");

  auto interfaceMap =
      detail::InterfaceMap::get<IsDynamicTypeInterface::Trait<DynamicType>>();
  auto abstractType = AbstractType(*dialect, std::move(interfaceMap), typeID);

  /// Add the type to the dialect and the type uniquer.
  addType(typeID, std::move(abstractType));
  typePtr->registerInTypeUniquer();
}

void ExtensibleDialect::addDynamicOp(
    std::unique_ptr<DynamicOpDefinition> &&type) {
  assert(type->dialect == this &&
         "trying to register a dynamic op in the wrong dialect");
  auto foldHook = [](mlir::Operation *op,
                     llvm::ArrayRef<mlir::Attribute> operands,
                     llvm::SmallVectorImpl<mlir::OpFoldResult> &results) {
    return failure();
  };

  auto getCanonicalizationPatterns = [](OwningRewritePatternList &,
                                        MLIRContext *) {};

  auto hasTraitFn = [](TypeID traitId) { return false; };

  AbstractOperation::insert(
      type->name, *type->dialect, type->typeID, std::move(type->parseFn),
      std::move(type->printFn), std::move(type->verifyFn), std::move(foldHook),
      std::move(getCanonicalizationPatterns), detail::InterfaceMap::get<>(),
      std::move(hasTraitFn));
}

bool ExtensibleDialect::classof(Dialect *dialect) {
  return dialect->getRegisteredInterface<IsExtensibleDialect>();
}

OptionalParseResult ExtensibleDialect::parseOptionalDynamicType(
    StringRef typeName, DialectAsmParser &parser, Type &resultType) const {
  auto typeDef = lookupTypeDefinition(typeName);
  if (succeeded(typeDef)) {
    DynamicType dynType;
    if (DynamicType::parse(parser, *typeDef, dynType))
      return failure();
    resultType = dynType;
    return {success()};
  }

  return {};
}

LogicalResult
ExtensibleDialect::printIfDynamicType(Type type, DialectAsmPrinter &printer) {
  if (auto dynType = type.dyn_cast<DynamicType>()) {
    dynType.print(printer);
    return success();
  }
  return failure();
}
