// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "video_core/macro/macro.h"
#include "video_core/macro/macro_interpreter.h"

#ifdef ARCHITECTURE_x86_64
#include "video_core/macro/macro_jit_x64.h"
#endif

//#pragma optimize("", off)

namespace Tegra {

void MacroEngine::AddCode(u32 method, u32 data) {
    uploaded_macro_code[method].push_back(data);
}

void MacroEngine::Execute(u32 method, std::vector<u32> parameters) {
    // The requested macro must have been uploaded already.
    auto compiled_macro = macro_cache.find(method);
    if (compiled_macro == macro_cache.end()) {
        // macro hasn't been compiled yet, so compile it and cache it
        // The requested macro must have been uploaded already.
        auto macro_code = uploaded_macro_code.find(method);
        if (macro_code == uploaded_macro_code.end()) {
            LOG_ERROR(HW_GPU, "Macro 0x{0:x} was not uploaded", method);
            return;
        }
        macro_cache[method] = Compile(macro_code->second);
        compiled_macro = macro_cache.find(method);
    }
    compiled_macro->second->Execute(std::move(parameters));
} // namespace Tegra

std::unique_ptr<MacroEngine> GetMacroEngine(Engines::Maxwell3D& maxwell3d) {
#ifdef ARCHITECTURE_x86_64
    // return std::move(std::make_unique<MacroInterpreter>(maxwell3d));
    return std::move(std::make_unique<MacroJitX64>(maxwell3d));
#else
    return std::move(std::make_unique<MacroInterpreter>(maxwell3d));
#endif // ARCHITECTURE_x86_64
}

} // namespace Tegra
