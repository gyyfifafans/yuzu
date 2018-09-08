// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/macro/macro.h"
#include "video_core/macro/macro_interpreter.h"

namespace Tegra {

std::unique_ptr<MacroEngine> GetMacroEngine(Engines::Maxwell3D& maxwell3d) {
    return std::move(std::make_unique<MacroInterpreter>(maxwell3d));
}

} // namespace Tegra