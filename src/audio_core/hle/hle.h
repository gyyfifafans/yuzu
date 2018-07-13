// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <vector>
#include "audio_core/audio_types.h"
#include "audio_core/dsp_interface.h"
#include "common/common_types.h"
#include "core/memory.h"

namespace AudioCore {

class DspHle final : public DspInterface {
public:
    DspHle();
    ~DspHle();

private:
    struct Impl;
    friend struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace AudioCore
