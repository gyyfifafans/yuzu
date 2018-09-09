// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma optimize("", off)

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/macro/macro_interpreter.h"

namespace Tegra {

MacroInterpreter::MacroInterpreter(Engines::Maxwell3D& maxwell3d) : maxwell3d(maxwell3d) {}

std::unique_ptr<CachedMacro> MacroInterpreter::Compile(const std::vector<u32>& code) {
    return std::make_unique<InterpretedMacro>(maxwell3d, code);
}

InterpretedMacro::InterpretedMacro(Engines::Maxwell3D& maxwell3d_, const std::vector<u32>& code_)
    : code(code_), maxwell3d(maxwell3d_) {}

void InterpretedMacro::Execute(std::vector<u32> parameters) {
    Reset();
    registers[1] = parameters[0];
    this->parameters = std::move(parameters);

    // Execute the code until we hit an exit condition.
    bool keep_executing = true;
    while (keep_executing) {
        keep_executing = Step(false);
    }

    // Assert the the macro used all the input parameters
    ASSERT(next_parameter_index == this->parameters.size());
}

void InterpretedMacro::Reset() {
    registers = {};
    pc = 0;
    delayed_pc = boost::none;
    method_address.raw = 0;
    parameters.clear();
    // The next parameter index starts at 1, because $r1 already has the value of the first
    // parameter.
    next_parameter_index = 1;
}

bool InterpretedMacro::Step(bool is_delay_slot) {
    u32 base_address = pc;

    Macro::Opcode opcode = GetOpcode();
    pc += 4;

    // Update the program counter if we were delayed
    if (delayed_pc != boost::none) {
        ASSERT(is_delay_slot);
        pc = *delayed_pc;
        delayed_pc = boost::none;
    }

    switch (opcode.operation) {
    case Macro::Operation::ALU: {
        u32 result = GetALUResult(opcode.alu_operation, GetRegister(opcode.src_a),
                                  GetRegister(opcode.src_b));
        ProcessResult(opcode.result_operation, opcode.dst, result);
        break;
    }
    case Macro::Operation::AddImmediate: {
        ProcessResult(opcode.result_operation, opcode.dst,
                      GetRegister(opcode.src_a) + opcode.immediate);
        break;
    }
    case Macro::Operation::ExtractInsert: {
        u32 dst = GetRegister(opcode.src_a);
        u32 src = GetRegister(opcode.src_b);

        src = (src >> opcode.bf_src_bit) & opcode.GetBitfieldMask();
        dst &= ~(opcode.GetBitfieldMask() << opcode.bf_dst_bit);
        dst |= src << opcode.bf_dst_bit;
        ProcessResult(opcode.result_operation, opcode.dst, dst);
        break;
    }
    case Macro::Operation::ExtractShiftLeftImmediate: {
        u32 dst = GetRegister(opcode.src_a);
        u32 src = GetRegister(opcode.src_b);

        u32 result = ((src >> dst) & opcode.GetBitfieldMask()) << opcode.bf_dst_bit;

        ProcessResult(opcode.result_operation, opcode.dst, result);
        break;
    }
    case Macro::Operation::ExtractShiftLeftRegister: {
        u32 dst = GetRegister(opcode.src_a);
        u32 src = GetRegister(opcode.src_b);

        u32 result = ((src >> opcode.bf_src_bit) & opcode.GetBitfieldMask()) << dst;

        ProcessResult(opcode.result_operation, opcode.dst, result);
        break;
    }
    case Macro::Operation::Read: {
        u32 result = Read(GetRegister(opcode.src_a) + opcode.immediate);
        ProcessResult(opcode.result_operation, opcode.dst, result);
        break;
    }
    case Macro::Operation::Branch: {
        ASSERT_MSG(!is_delay_slot, "Executing a branch in a delay slot is not valid");
        u32 value = GetRegister(opcode.src_a);
        bool taken = EvaluateBranchCondition(opcode.branch_condition, value);
        if (taken) {
            // Ignore the delay slot if the branch has the annul bit.
            if (opcode.branch_annul) {
                pc = base_address + opcode.GetBranchTarget();
                return true;
            }

            delayed_pc = base_address + opcode.GetBranchTarget();
            // Execute one more instruction due to the delay slot.
            return Step(true);
        }
        break;
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented macro operation {}",
                          static_cast<u32>(opcode.operation.Value()));
    }

    if (opcode.is_exit) {
        // Exit has a delay slot, execute the next instruction
        // Note: Executing an exit during a branch delay slot will cause the instruction at the
        // branch target to be executed before exiting.
        Step(true);
        return false;
    }

    return true;
}

Macro::Opcode InterpretedMacro::GetOpcode() const {
    ASSERT((pc % sizeof(u32)) == 0);
    ASSERT(pc < code.size() * sizeof(u32));
    return {code[pc / sizeof(u32)]};
}

u32 InterpretedMacro::GetALUResult(Macro::ALUOperation operation, u32 src_a, u32 src_b) const {
    switch (operation) {
    case Macro::ALUOperation::Add:
        return src_a + src_b;
    // TODO(Subv): Implement AddWithCarry
    case Macro::ALUOperation::Subtract:
        return src_a - src_b;
    // TODO(Subv): Implement SubtractWithBorrow
    case Macro::ALUOperation::Xor:
        return src_a ^ src_b;
    case Macro::ALUOperation::Or:
        return src_a | src_b;
    case Macro::ALUOperation::And:
        return src_a & src_b;
    case Macro::ALUOperation::AndNot:
        return src_a & ~src_b;
    case Macro::ALUOperation::Nand:
        return ~(src_a & src_b);

    default:
        UNIMPLEMENTED_MSG("Unimplemented ALU operation {}", static_cast<u32>(operation));
    }
}

void InterpretedMacro::ProcessResult(Macro::ResultOperation operation, u32 reg, u32 result) {
    switch (operation) {
    case Macro::ResultOperation::IgnoreAndFetch:
        // Fetch parameter and ignore result.
        SetRegister(reg, FetchParameter());
        break;
    case Macro::ResultOperation::Move:
        // Move result.
        SetRegister(reg, result);
        break;
    case Macro::ResultOperation::MoveAndSetMethod:
        // Move result and use as Method Address.
        SetRegister(reg, result);
        SetMethodAddress(result);
        break;
    case Macro::ResultOperation::FetchAndSend:
        // Fetch parameter and send result.
        SetRegister(reg, FetchParameter());
        Send(result);
        break;
    case Macro::ResultOperation::MoveAndSend:
        // Move and send result.
        SetRegister(reg, result);
        Send(result);
        break;
    case Macro::ResultOperation::FetchAndSetMethod:
        // Fetch parameter and use result as Method Address.
        SetRegister(reg, FetchParameter());
        SetMethodAddress(result);
        break;
    case Macro::ResultOperation::MoveAndSetMethodFetchAndSend:
        // Move result and use as Method Address, then fetch and send parameter.
        SetRegister(reg, result);
        SetMethodAddress(result);
        Send(FetchParameter());
        break;
    case Macro::ResultOperation::MoveAndSetMethodSend:
        // Move result and use as Method Address, then send bits 12:17 of result.
        SetRegister(reg, result);
        SetMethodAddress(result);
        Send((result >> 12) & 0b111111);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented result operation {}", static_cast<u32>(operation));
    }
}

u32 InterpretedMacro::FetchParameter() {
    ASSERT(next_parameter_index < parameters.size());
    return parameters[next_parameter_index++];
}

u32 InterpretedMacro::GetRegister(u32 register_id) const {
    // Register 0 is supposed to always return 0.
    if (register_id == 0)
        return 0;

    ASSERT(register_id < registers.size());
    return registers[register_id];
}

void InterpretedMacro::SetRegister(u32 register_id, u32 value) {
    // Register 0 is supposed to always return 0. NOP is implemented as a store to the zero
    // register.
    if (register_id == 0)
        return;

    ASSERT(register_id < registers.size());
    registers[register_id] = value;
}

void InterpretedMacro::SetMethodAddress(u32 address) {
    method_address.raw = address;
}

void InterpretedMacro::Send(u32 value) {
    maxwell3d.WriteReg(method_address.address, value, 0);
    // Increment the method address by the method increment.
    method_address.address.Assign(method_address.address.Value() +
                                  method_address.increment.Value());
}

u32 InterpretedMacro::Read(u32 method) const {
    return maxwell3d.GetRegisterValue(method);
}

bool InterpretedMacro::EvaluateBranchCondition(Macro::BranchCondition cond, u32 value) const {
    switch (cond) {
    case Macro::BranchCondition::Zero:
        return value == 0;
    case Macro::BranchCondition::NotZero:
        return value != 0;
    }
    UNREACHABLE();
}

} // namespace Tegra
