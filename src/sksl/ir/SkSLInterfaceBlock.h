/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SKSL_INTERFACEBLOCK
#define SKSL_INTERFACEBLOCK

#include "src/sksl/ir/SkSLProgramElement.h"
#include "src/sksl/ir/SkSLSymbolTable.h"
#include "src/sksl/ir/SkSLVarDeclarations.h"

namespace SkSL {

/**
 * An interface block, as in:
 *
 * out sk_PerVertex {
 *   layout(builtin=0) float4 sk_Position;
 *   layout(builtin=1) float sk_PointSize;
 * };
 *
 * At the IR level, this is represented by a single variable of struct type.
 */
struct InterfaceBlock : public ProgramElement {
    static constexpr Kind kProgramElementKind = Kind::kInterfaceBlock;

    InterfaceBlock(int offset, const Variable* var, String typeName, String instanceName,
                   std::vector<std::unique_ptr<Expression>> sizes,
                   std::shared_ptr<SymbolTable> typeOwner)
    : INHERITED(offset, kProgramElementKind)
    , fVariable(var)
    , fTypeName(std::move(typeName))
    , fInstanceName(std::move(instanceName))
    , fSizes(std::move(sizes))
    , fTypeOwner(typeOwner) {}

    std::unique_ptr<ProgramElement> clone() const override {
        std::vector<std::unique_ptr<Expression>> sizesClone;
        sizesClone.reserve(fSizes.size());
        for (const auto& s : fSizes) {
            sizesClone.push_back(s ? s->clone() : nullptr);
        }
        return std::unique_ptr<ProgramElement>(new InterfaceBlock(fOffset, fVariable, fTypeName,
                                                                  fInstanceName,
                                                                  std::move(sizesClone),
                                                                  fTypeOwner));
    }

    String description() const override {
        String result = fVariable->modifiers().description() + fTypeName + " {\n";
        const Type* structType = &fVariable->type();
        while (structType->typeKind() == Type::TypeKind::kArray) {
            structType = &structType->componentType();
        }
        for (const auto& f : structType->fields()) {
            result += f.description() + "\n";
        }
        result += "}";
        if (fInstanceName.size()) {
            result += " " + fInstanceName;
            for (const auto& size : fSizes) {
                result += "[";
                if (size) {
                    result += size->description();
                }
                result += "]";
            }
        }
        return result + ";";
    }

    const Variable* fVariable;
    const String fTypeName;
    const String fInstanceName;
    std::vector<std::unique_ptr<Expression>> fSizes;
    const std::shared_ptr<SymbolTable> fTypeOwner;

    using INHERITED = ProgramElement;
};

}  // namespace SkSL

#endif
