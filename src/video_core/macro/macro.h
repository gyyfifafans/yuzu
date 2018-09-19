// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include "common/bit_field.h"
#include "common/common_types.h"

namespace Tegra {
namespace Engines {
class Maxwell3D;
}

namespace Macro {

static constexpr size_t NumMacroRegisters = 8;
enum class Operation : u32 {
    ALU = 0,
    AddImmediate = 1,
    ExtractInsert = 2,
    ExtractShiftLeftImmediate = 3,
    ExtractShiftLeftRegister = 4,
    Read = 5,
    Unused = 6, // This operation doesn't seem to be a valid encoding.
    Branch = 7,
};

enum class ALUOperation : u32 {
    Add = 0,
    AddWithCarry = 1,
    Subtract = 2,
    SubtractWithBorrow = 3,
    // Operations 4-7 don't seem to be valid encodings.
    Xor = 8,
    Or = 9,
    And = 10,
    AndNot = 11,
    Nand = 12
};

enum class ResultOperation : u32 {
    IgnoreAndFetch = 0,
    Move = 1,
    MoveAndSetMethod = 2,
    FetchAndSend = 3,
    MoveAndSend = 4,
    FetchAndSetMethod = 5,
    MoveAndSetMethodFetchAndSend = 6,
    MoveAndSetMethodSend = 7
};

enum class BranchCondition : u32 {
    Zero = 0,
    NotZero = 1,
};

union Opcode {
    u32 raw;
    BitField<0, 3, Operation> operation;
    BitField<4, 3, ResultOperation> result_operation;
    BitField<4, 1, BranchCondition> branch_condition;
    // If set on a branch, then the branch doesn't have a delay slot.
    BitField<5, 1, u32> branch_annul;
    BitField<7, 1, u32> is_exit;
    BitField<8, 3, u32> dst;
    BitField<11, 3, u32> src_a;
    BitField<14, 3, u32> src_b;
    // The signed immediate overlaps the second source operand and the alu operation.
    BitField<14, 18, s32> immediate;

    BitField<17, 5, ALUOperation> alu_operation;

    // Bitfield instructions data
    BitField<17, 5, u32> bf_src_bit;
    BitField<22, 5, u32> bf_size;
    BitField<27, 5, u32> bf_dst_bit;

    u32 GetBitfieldMask() const {
        return (1 << bf_size) - 1;
    }

    s32 GetBranchTarget() const {
        return static_cast<s32>(immediate * sizeof(u32));
    }
};

union MethodAddress {
    u32 raw;
    BitField<0, 12, u32> address;
    BitField<12, 6, u32> increment;
};
} // namespace Macro

class CachedMacro {
public:
    virtual ~CachedMacro() = default;
    /**
     * Executes the macro code with the specified input parameters.
     * @param code The macro byte code to execute
     * @param parameters The parameters of the macro
     */
    virtual void Execute(std::vector<u32> parameters) = 0;
};

class MacroEngine {
public:
    virtual ~MacroEngine() = default;

    /// Store the uploaded macro code to compile them when they're called.
    void AddCode(const u32 method, u32 data);

    /// Compiles the macro if its not in the cache, and executes the compiled macro
    void Execute(const u32 method, std::vector<u32> parameters);

protected:
    virtual std::unique_ptr<CachedMacro> Compile(const std::vector<u32>& code) = 0;

private:
    std::unordered_map<u32, std::unique_ptr<CachedMacro>> macro_cache;
    std::unordered_map<u32, std::vector<u32>> uploaded_macro_code;
};

std::unique_ptr<MacroEngine> GetMacroEngine(Engines::Maxwell3D& maxwell3d);

} // namespace Tegra
