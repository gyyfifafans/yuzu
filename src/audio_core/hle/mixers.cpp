// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstddef>

#include "audio_core/hle/mixers.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/math_util.h"

namespace AudioCore {
namespace HLE {

void Mixers::Reset() {
    current_frame.fill({});
    state = {};
}

DspStatus Mixers::Tick(DspConfiguration& config, const IntermediateMixSamples& read_samples,
                       IntermediateMixSamples& write_samples,
                       const std::array<QuadFrame32, 3>& input) {

    MixCurrentFrame();

    return GetCurrentStatus();
}

static s16 ClampToS16(s32 value) {
    return static_cast<s16>(MathUtil::Clamp(value, -32768, 32767));
}

static std::array<s16, 2> AddAndClampToS16(const std::array<s16, 2>& a,
                                           const std::array<s16, 2>& b) {
    return {ClampToS16(static_cast<s32>(a[0]) + static_cast<s32>(b[0])),
            ClampToS16(static_cast<s32>(a[1]) + static_cast<s32>(b[1]))};
}

void Mixers::DownmixAndMixIntoCurrentFrame(float gain, const QuadFrame32& samples) {
    // TODO(merry): Limiter. (Currently we're performing final mixing assuming a disabled limiter.)

    switch (state.output_format) {
    case OutputFormat::Mono:
        std::transform(
            current_frame.begin(), current_frame.end(), samples.begin(), current_frame.begin(),
            [gain](const std::array<s16, 2>& accumulator,
                   const std::array<s32, 4>& sample) -> std::array<s16, 2> {
                // Downmix to mono
                s16 mono = ClampToS16(static_cast<s32>(
                    (gain * sample[0] + gain * sample[1] + gain * sample[2] + gain * sample[3]) /
                    2));
                // Mix into current frame
                return AddAndClampToS16(accumulator, {mono, mono});
            });
        return;

    case OutputFormat::Surround:
        // TODO(merry): Implement surround sound.
        // fallthrough

    case OutputFormat::Stereo:
        std::transform(
            current_frame.begin(), current_frame.end(), samples.begin(), current_frame.begin(),
            [gain](const std::array<s16, 2>& accumulator,
                   const std::array<s32, 4>& sample) -> std::array<s16, 2> {
                // Downmix to stereo
                s16 left = ClampToS16(static_cast<s32>(gain * sample[0] + gain * sample[2]));
                s16 right = ClampToS16(static_cast<s32>(gain * sample[1] + gain * sample[3]));
                // Mix into current frame
                return AddAndClampToS16(accumulator, {left, right});
            });
        return;
    }

    UNREACHABLE_MSG("Invalid output_format {}", static_cast<size_t>(state.output_format));
}

void Mixers::AuxReturn(const IntermediateMixSamples& read_samples) {
    // NOTE: read_samples.mix{1,2}.pcm32 annoyingly have their dimensions in reverse order to
    // QuadFrame32.

    if (state.mixer1_enabled) {
        for (size_t sample = 0; sample < samples_per_frame; sample++) {
            for (size_t channel = 0; channel < 4; channel++) {
                state.intermediate_mix_buffer[1][sample][channel] =
                    read_samples.mix1.pcm32[channel][sample];
            }
        }
    }

    if (state.mixer2_enabled) {
        for (size_t sample = 0; sample < samples_per_frame; sample++) {
            for (size_t channel = 0; channel < 4; channel++) {
                state.intermediate_mix_buffer[2][sample][channel] =
                    read_samples.mix2.pcm32[channel][sample];
            }
        }
    }
}

void Mixers::AuxSend(IntermediateMixSamples& write_samples,
                     const std::array<QuadFrame32, 3>& input) {
    // NOTE: read_samples.mix{1,2}.pcm32 annoyingly have their dimensions in reverse order to
    // QuadFrame32.

    state.intermediate_mix_buffer[0] = input[0];

    if (state.mixer1_enabled) {
        for (size_t sample = 0; sample < samples_per_frame; sample++) {
            for (size_t channel = 0; channel < 4; channel++) {
                write_samples.mix1.pcm32[channel][sample] = input[1][sample][channel];
            }
        }
    } else {
        state.intermediate_mix_buffer[1] = input[1];
    }

    if (state.mixer2_enabled) {
        for (size_t sample = 0; sample < samples_per_frame; sample++) {
            for (size_t channel = 0; channel < 4; channel++) {
                write_samples.mix2.pcm32[channel][sample] = input[2][sample][channel];
            }
        }
    } else {
        state.intermediate_mix_buffer[2] = input[2];
    }
}

void Mixers::MixCurrentFrame() {
    current_frame.fill({});

    for (size_t mix = 0; mix < 3; mix++) {
        DownmixAndMixIntoCurrentFrame(state.intermediate_mixer_volume[mix],
                                      state.intermediate_mix_buffer[mix]);
    }

    // TODO(merry): Compressor. (We currently assume a disabled compressor.)
}

DspStatus Mixers::GetCurrentStatus() const {
    DspStatus status;
    status.unknown = 0;
    status.dropped_frames = 0;
    return status;
}

} // namespace HLE
} // namespace AudioCore
