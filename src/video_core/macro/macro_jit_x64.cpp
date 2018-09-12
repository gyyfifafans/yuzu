// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "common/x64/cpu_detect.h"
#include "common/x64/xbyak_abi.h"
#include "common/x64/xbyak_util.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/macro/macro_jit_x64.h"

#pragma optimize("", off)

using namespace Common::X64;
using namespace Xbyak::util;
using Xbyak::Label;
using Xbyak::Reg32;
using Xbyak::Reg64;
using Xbyak::Xmm;

namespace Tegra {

typedef void (JitMacro::*JitFunction)(Macro::Opcode opcode);

const JitFunction instr_table[8] = {
    &JitMacro::Compile_ALU,
    &JitMacro::Compile_AddImmediate,
    &JitMacro::Compile_ExtractInsert,
    &JitMacro::Compile_ExtractShiftLeftImmediate,
    &JitMacro::Compile_ExtractShiftLeftRegister,
    &JitMacro::Compile_Read,
    nullptr, // Unused
    &JitMacro::Compile_Branch,
};

// The following is used to alias some commonly used registers. Generally, RAX-RDX and XMM0-XMM3 can
// be used as scratch registers within a compiler function. The other registers have designated
// purposes, as documented below:

/// Pointer to the input parameters array
static const Reg64 PARAMETERS = r9;
/// Pointer to register array
static const Reg64 REGISTERS = r10;
/// Pointer to the current JitMacro state
static const Reg64 STATE = r11;
/// Index of the next parameter that the macro will use
static const Reg64 NEXT_PARAMETER = r12;
/// Value of the result calculated by ProcessResult
static const Reg32 RESULT = r13d;
/// Value of the method address
static const Reg32 METHOD_ADDRESS = r14d;

static const BitSet32 PERSISTENT_REGISTERS = BuildRegSet({
    PARAMETERS,
    NEXT_PARAMETER,
    REGISTERS,
    RESULT,
    METHOD_ADDRESS,
    STATE,
});

JitMacro::JitMacro(Engines::Maxwell3D& maxwell3d, const std::vector<u32>& code_)
    : Xbyak::CodeGenerator(MAX_CODE_SIZE), state{&maxwell3d, {0}, nullptr}, code(code_) {
    Compile();
}

void JitMacro::Execute(std::vector<u32> parameters) {
    state.parameters = parameters.data();
    program(&state);
}

Macro::Opcode JitMacro::GetOpcode() const {
    ASSERT((pc % sizeof(u32)) == 0);
    ASSERT(pc < code.size() * sizeof(u32));
    return {code[pc / sizeof(u32)]};
}

void JitMacro::Compile() {
    program = (CompiledMacro*)getCurr();

    ABI_PushRegistersAndAdjustStack(*this, ABI_ALL_CALLEE_SAVED, 8);

    mov(STATE, ABI_PARAM1);
    mov(PARAMETERS, ABI_PARAM1);
    add(PARAMETERS, static_cast<Xbyak::uint32>(offsetof(JitState, parameters)));
    xor_(NEXT_PARAMETER, NEXT_PARAMETER);
    mov(REGISTERS, ABI_PARAM1);
    add(REGISTERS, static_cast<Xbyak::uint32>(offsetof(JitState, registers)));
    xor_(RESULT, RESULT);
    xor_(METHOD_ADDRESS, METHOD_ADDRESS);

    instruction_labels.fill(Xbyak::Label());

    bool keep_executing = true;
    while (keep_executing) {
        keep_executing = Compile_NextInstruction();
    }

    ABI_PopRegistersAndAdjustStack(*this, ABI_ALL_CALLEE_SAVED, 8);
    ret();
    ready();
}

bool JitMacro::Compile_NextInstruction() {
    Macro::Opcode opcode = GetOpcode();
    L(instruction_labels[pc / 4]);

    pc += 4;

    auto instr_func = instr_table[static_cast<u32>(opcode.operation.Value())];

    if (instr_func) {
        ((*this).*instr_func)(opcode);
    } else {
        // Unhandled instruction
        LOG_CRITICAL(HW_GPU, "Unhandled macro jit instruction: 0x{:02x} (0x{:04x})",
                     static_cast<u32>(opcode.operation.Value()), opcode.raw);
    }

    if (opcode.is_exit) {
        // Exit has a delay slot, execute the next instruction
        // Note: Executing an exit during a branch delay slot will cause the instruction at the
        // branch target to be executed before exiting.
        Compile_NextInstruction();
        return false;
    }

    return true;
}

void JitMacro::Compile_ALU(Macro::Opcode opcode) {
    auto src_a = Compile_GetRegister(opcode.src_a, eax);
    auto src_b = Compile_GetRegister(opcode.src_b, ebx);
    switch (opcode.alu_operation) {
    case Macro::ALUOperation::Add:
        add(src_a, src_b);
        break;
    // TODO(Subv): Implement AddWithCarry
    case Macro::ALUOperation::Subtract:
        sub(src_a, src_b);
        break;
    // TODO(Subv): Implement SubtractWithBorrow
    case Macro::ALUOperation::Xor:
        xor_(src_a, src_b);
        break;
    case Macro::ALUOperation::Or:
        or_(src_a, src_b);
        break;
    case Macro::ALUOperation::And:
        and_(src_a, src_b);
        break;
    case Macro::ALUOperation::AndNot:
        not_(src_b);
        and_(src_a, src_b);
        break;
    case Macro::ALUOperation::Nand:
        and_(src_a, src_b);
        not_(src_a);
        break;
    default:
        break;
    }
    mov(RESULT, src_a);
    Compile_ProcessResult(opcode.result_operation, opcode.dst);
}

void JitMacro::Compile_AddImmediate(Macro::Opcode opcode) {
    auto result = Compile_GetRegister(opcode.src_a, RESULT);
    add(result, opcode.immediate);
    Compile_ProcessResult(opcode.result_operation, opcode.dst);
}

void JitMacro::Compile_ExtractInsert(Macro::Opcode opcode) {
    auto dst = Compile_GetRegister(opcode.src_a, RESULT);
    auto src = Compile_GetRegister(opcode.src_b, eax);
    // src = (src >> opcode.bf_src_bit) & opcode.GetBitfieldMask();
    // src = src << opcode.bf_dst_bit
    shr(src, opcode.bf_src_bit);
    and_(src, opcode.GetBitfieldMask());
    shl(src, opcode.bf_dst_bit);
    // dst &= ~(opcode.GetBitfieldMask() << opcode.bf_dst_bit);
    // dst |= src;
    const u32 shift = ~(opcode.GetBitfieldMask() << opcode.bf_dst_bit);
    and_(dst, shift);
    or_(dst, src);
    Compile_ProcessResult(opcode.result_operation, opcode.dst);
}

void JitMacro::Compile_ExtractShiftLeftImmediate(Macro::Opcode opcode) {
    auto dst = Compile_GetRegister(opcode.src_a, ecx);
    auto src = Compile_GetRegister(opcode.src_b, RESULT);
    // result = ((src >> dst) & opcode.GetBitfieldMask()) << opcode.bf_dst_bit
    shl(src, cl);
    and_(src, opcode.GetBitfieldMask());
    sal(src, opcode.bf_dst_bit);
    Compile_ProcessResult(opcode.result_operation, opcode.dst);
}

void JitMacro::Compile_ExtractShiftLeftRegister(Macro::Opcode opcode) {
    auto dst = Compile_GetRegister(opcode.src_a, ecx);
    auto src = Compile_GetRegister(opcode.src_b, RESULT);
    // result = ((src >> opcode.bf_src_bit) & opcode.GetBitfieldMask()) << dst;
    shl(src, opcode.bf_src_bit);
    and_(src, opcode.GetBitfieldMask());
    shr(src, cl);
    Compile_ProcessResult(opcode.result_operation, opcode.dst);
}

void JitMacro::Compile_Read(Macro::Opcode opcode) {
    // TODO: avoid ABI overhead by putting a pointer to the engine's registers in STATE
    // ABI_PushRegistersAndAdjustStack(*this, PersistentCallerSavedRegs(), 0);
    // mov(ABI_PARAM1, STATE);
    // add(ABI_PARAM1, static_cast<Xbyak::uint32>(offsetof(JitState, maxwell3d)));
    // mov(ABI_PARAM2, METHOD_ADDRESS);
    // CallFarFunction(*this, Read);
    // ABI_PopRegistersAndAdjustStack(*this, PersistentCallerSavedRegs(), 0);
    // mov(REGISTERS, dword[ABI_RETURN]);

    // Load into rax the value of the register that we want to read
    Compile_GetRegister(opcode.src_a, eax);
    add(eax, opcode.immediate);
    mov(eax, eax);
    // Load the value of that register into result
    mov(rbx, qword[STATE]);
    add(rbx, static_cast<Xbyak::uint32>(offsetof(JitState, maxwell3d->regs.reg_array)));
    mov(RESULT, dword[rbx + rax * 4]);
    Compile_ProcessResult(opcode.result_operation, opcode.dst);
}

void JitMacro::Compile_Branch(Macro::Opcode opcode) {
    Xbyak::Label taken, end;
    auto value = Compile_GetRegister(opcode.src_b, eax);
    cmp(value, 0);
    switch (opcode.branch_condition) {
    case Macro::BranchCondition::Zero:
        je(taken);
        break;
    case Macro::BranchCondition::NotZero:
        jne(taken);
        break;
    }
    // Branch was not taken
    jmp(end);

    // Branch was taken
    L(taken);
    // Ignore the delay slot if the branch has the annul bit.
    s32 jump_address = pc + opcode.GetBranchTarget();
    if (!opcode.branch_annul) {
        // Execute one more instruction due to the delay slot.
        Compile_NextInstruction();
    }
    jmp(instruction_labels[jump_address / 4], T_NEAR);
    L(end);
}

BitSet32 JitMacro::PersistentCallerSavedRegs() const {
    return PERSISTENT_REGISTERS & ABI_ALL_CALLER_SAVED;
}

/// Moves the next parameter into edi and increments NEXT_PARAMETER
Reg32 JitMacro::Compile_FetchParameter() {
    mov(edi, dword[PARAMETERS + NEXT_PARAMETER * sizeof(u32)]);
    inc(NEXT_PARAMETER);
    return edi;
}

/// Copies the value of the register to the passed in register and returns it
Reg32 JitMacro::Compile_GetRegister(u32 index, Reg32 reg) {
    mov(reg, dword[REGISTERS + index * sizeof(u32)]);
    return reg;
}

void JitMacro::Compile_ProcessResult(Macro::ResultOperation operation, u32 reg) {
    auto SetRegister = [&](Reg32 result) { mov(dword[REGISTERS + reg * 4], result); };
    auto SetMethodAddress = [&] { mov(METHOD_ADDRESS, RESULT); };
    switch (operation) {
    case Macro::ResultOperation::IgnoreAndFetch:
        // Fetch parameter and ignore result.
        SetRegister(Compile_FetchParameter());
        break;
    case Macro::ResultOperation::Move:
        // Move result.
        SetRegister(RESULT);
        break;
    case Macro::ResultOperation::MoveAndSetMethod:
        // Move result and use as Method Address.
        SetRegister(RESULT);
        SetMethodAddress();
        break;
    case Macro::ResultOperation::FetchAndSend:
        // Fetch parameter and send result.
        SetRegister(Compile_FetchParameter());
        Compile_Send(RESULT);
        break;
    case Macro::ResultOperation::MoveAndSend:
        // Move and send result.
        SetRegister(RESULT);
        Compile_Send(RESULT);
        break;
    case Macro::ResultOperation::FetchAndSetMethod:
        // Fetch parameter and use result as Method Address.
        SetRegister(Compile_FetchParameter());
        SetMethodAddress();
        break;
    case Macro::ResultOperation::MoveAndSetMethodFetchAndSend:
        // Move result and use as Method Address, then fetch and send parameter.
        SetRegister(RESULT);
        SetMethodAddress();
        Compile_Send(Compile_FetchParameter());
        break;
    case Macro::ResultOperation::MoveAndSetMethodSend:
        // Move result and use as Method Address, then send bits 12:17 of result.
        SetRegister(RESULT);
        SetMethodAddress();
        sar(RESULT, 12);
        and_(RESULT, 0b111111);
        Compile_Send(RESULT);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented result operation {}", static_cast<u32>(operation));
    }
}

static void Send(Engines::Maxwell3D* maxwell3d, u32 method_address, u32 value) {
    maxwell3d->WriteReg(method_address, value, 0);
}

static u32 Read(Engines::Maxwell3D* maxwell3d, u32 method) {
    maxwell3d->regs.reg_array[method];
    return maxwell3d->GetRegisterValue(method);
}

void JitMacro::Compile_Send(Xbyak::Reg32 reg) {
    ABI_PushRegistersAndAdjustStack(*this, PersistentCallerSavedRegs(), 8);
    // The first member of the struct is the maxwell3d pointer, so we dont move the param
    mov(ABI_PARAM1, qword[STATE]);
    // add(ABI_PARAM1, static_cast<Xbyak::uint32>(offsetof(JitState, maxwell3d)));
    mov(ABI_PARAM2, METHOD_ADDRESS);
    // the address field is only the 12 highest bits so shift out the others
    shr(ABI_PARAM2, 20);
    mov(ABI_PARAM3, reg);
    CallFarFunction(*this, Send);
    ABI_PopRegistersAndAdjustStack(*this, PersistentCallerSavedRegs(), 8);
    // Increment method_address address bits by the increment amount
    // method_address (u32): xx xx xx xx xx xx yy yy yy 00 00 00 00 00 00 00
    // x = address bits
    // y = increment bits
    mov(eax, METHOD_ADDRESS);
    shl(eax, 6);
    and_(eax, 0b111111 << 20);
    add(METHOD_ADDRESS, eax);
}

MacroJitX64::MacroJitX64(Engines::Maxwell3D& maxwell3d_) : maxwell3d(maxwell3d_) {}
MacroJitX64::~MacroJitX64() {}

std::unique_ptr<CachedMacro> MacroJitX64::Compile(const std::vector<u32>& code) {
    return std::make_unique<JitMacro>(maxwell3d, code);
}

} // namespace Tegra
