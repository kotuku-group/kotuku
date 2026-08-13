// Allocation-site ownership proof for contextual table designation.
//
// Phase 0 of the opt-in table context plan.  A table may only be permanently designated contextual when the author
// owns it, which means the designation target traces back to a table literal allocated in the function performing the
// designation.  This is allocation-site reachability rather than escape analysis: prior ordinary use of an owned
// allocation - mutation, aliasing, storage or being passed to a call - never removes ownership, because the runtime
// context gate reads the table flag live at every call.
//
// The classification never survives parsing.

#pragma once

#include <cstdint>

#include "static_type_descriptor.h"

class ParserContext;
struct BlockStmt;
struct ExprNode;

// Ownership of a designation target with respect to the function currently being analysed.
enum class TableOwnership : uint8_t {
   Unknown,   // Provenance could not be resolved; designation is rejected conservatively.
   Owned,     // Traces to a table literal allocated in the current function.
   Foreign    // Resolved to an allocation belonging to another author.
};

// Why a target was classified as foreign, used to produce a precise diagnostic.
enum class ForeignTableSource : uint8_t {
   None,
   Parameter,        // A function parameter; the caller owns the table.
   Global,           // A global; ownership is shared with the whole program.
   Upvalue,          // Resolved outside the analysed function.
   Import,           // An imported module value.
   CallResult,       // An unresolved call result.
   MergedUnknown,    // A control-flow merge with a value of unknown provenance.
   NotATable         // The target cannot be a table allocation at all.
};

struct TableOwnershipProof {
   TableOwnership ownership = TableOwnership::Unknown;
   ForeignTableSource foreign_source = ForeignTableSource::None;

   [[nodiscard]] bool owned() const noexcept { return this->ownership IS TableOwnership::Owned; }
};

// Classify a designation target expression against the function that encloses it.  CurrentFunctionDepth is the
// function nesting depth of the designating statement, matching StaticBindingDescriptor::function_depth.
//
// EnclosingBody is the body of the function that performs the designation, or the module block at depth zero.  It is
// scanned for assignments to a reassigned target so that a merge of owned literals can be proven.  Passing nullptr
// classifies a reassigned binding as unknown rather than tracing its assignments.
[[nodiscard]] TableOwnershipProof classify_table_ownership(
   const ParserContext &, const ExprNode &Target, uint16_t CurrentFunctionDepth,
   const BlockStmt *EnclosingBody = nullptr);

// Human-readable reason for a rejected designation target, suitable for a parser diagnostic.
[[nodiscard]] const char * foreign_table_source_reason(ForeignTableSource) noexcept;
