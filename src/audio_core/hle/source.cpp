// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include "audio_core/codec.h"
#include "audio_core/hle/common.h"
#include "audio_core/hle/source.h"
#include "audio_core/interpolate.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/memory.h"

namespace AudioCore {
namespace HLE {

void Source::MixInto(QuadFrame32& dest, size_t intermediate_mix_id) const {

    const std::array<float, 4>& gains = state.gain.at(intermediate_mix_id);
    for (size_t samplei = 0; samplei < samples_per_frame; samplei++) {
        // Conversion from stereo (current_frame) to quadraphonic (dest) occurs here.
        dest[samplei][0] += static_cast<s32>(gains[0] * current_frame[samplei][0]);
        dest[samplei][1] += static_cast<s32>(gains[1] * current_frame[samplei][1]);
        dest[samplei][2] += static_cast<s32>(gains[2] * current_frame[samplei][0]);
        dest[samplei][3] += static_cast<s32>(gains[3] * current_frame[samplei][1]);
    }
}

void Source::Reset() {
    current_frame.fill({});
    state = {};
}


} // namespace HLE
} // namespace AudioCore
