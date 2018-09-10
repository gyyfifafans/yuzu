// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <unordered_map>
#include <boost/optional.hpp>
#include "common/x64/xbyak_abi.h"
#include "video_core/macro/macro.h"

namespace Tegra {

/**
 * Container for all the different fields the JIT might need to access
 */
/// MAX_* are arbitrarily chosen based on current booting games
constexpr size_t MAX_REGISTERS = 0x10;
constexpr size_t MAX_CODE_SIZE = 0x10000;
struct JitState {
    /// Reference to the
    Engines::Maxwell3D* maxwell3d;
    /// All emulated registers at run time.
    std::array<u32, MAX_REGISTERS> registers;
    /// All runtime parameters
    u32* parameters;
};

class JitMacro : public Xbyak::CodeGenerator, public CachedMacro {
public:
    explicit JitMacro(Engines::Maxwell3D& maxwell3d, const std::vector<u32>& code);

    void Execute(std::vector<u32> parameters) override;

    void Compile_ALU(Macro::Opcode);
    void Compile_AddImmediate(Macro::Opcode);
    void Compile_ExtractInsert(Macro::Opcode);
    void Compile_ExtractShiftLeftImmediate(Macro::Opcode);
    void Compile_ExtractShiftLeftRegister(Macro::Opcode);
    void Compile_Read(Macro::Opcode);
    void Compile_Branch(Macro::Opcode);

private:
    Macro::Opcode GetOpcode() const;
    void Compile();
    bool Compile_NextInstruction();
    void Compile_ProcessResult(Macro::ResultOperation operation, u32 reg);
    void Compile_Send(Xbyak::Reg32 reg);
    Xbyak::Reg32 Compile_FetchParameter();
    Xbyak::Reg32 Compile_GetRegister(u32 index, Xbyak::Reg32 reg);

    BitSet32 PersistentCallerSavedRegs() const;

    /// If the current instruction is a delay slot, store the pc here
    boost::optional<u32> delayed_pc;
    /// Result of the macro compilation
    using CompiledMacro = void(JitState* state);
    CompiledMacro* program = nullptr;
    /// Current program counter. Used during compilation
    u32 pc = 0;
    /// Reference to the code that was compiled
    const std::vector<u32>& code;
    /// Container for any fields the JIT may need to reference
    JitState state;

    std::array<Xbyak::Label, MAX_CODE_SIZE> instruction_labels;
};

class MacroJitX64 final : public MacroEngine {
public:
    explicit MacroJitX64(Engines::Maxwell3D& maxwell3d);
    ~MacroJitX64() override;

protected:
    std::unique_ptr<CachedMacro> Compile(const std::vector<u32>& code) override;

private:
    Engines::Maxwell3D& maxwell3d;
};

} // namespace Tegra
