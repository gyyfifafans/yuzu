// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <vector>
#include <queue>
#include "audio_core/audio_types.h"
#include "audio_core/codec.h"
#include "audio_core/hle/common.h"
#include "audio_core/hle/filter.h"
#include "audio_core/interpolate.h"
#include "common/common_types.h"
#include "shared_memory.h"

namespace AudioCore {
namespace HLE {

/**
 * This module performs:
 * - Buffer management
 * - Decoding of buffers
 * - Buffer resampling and interpolation
 * - Per-source filtering (SimpleFilter, BiquadFilter)
 * - Per-source gain
 * - Other per-source processing
 */
class Source final {
public:
    explicit Source(size_t source_id_) : source_id(source_id_) {
        Reset();
    }

    /// Resets internal state.
    void Reset();

    /**
     * This is called once every audio frame. This performs per-source processing every frame.
     * @param config The new configuration we've got for this Source from the application.
     * @param adpcm_coeffs ADPCM coefficients to use if config tells us to use them (may contain
     * invalid values otherwise).
     * @return The current status of this Source. This is given back to the emulated application via
     * SharedMemory.
     */
    SourceStatus::Status Tick(SourceConfiguration::Configuration& config,
                              const s16_le (&adpcm_coeffs)[16]);

    /**
     * Mix this source's output into dest, using the gains for the `intermediate_mix_id`-th
     * intermediate mixer.
     * @param dest The QuadFrame32 to mix into.
     * @param intermediate_mix_id The id of the intermediate mix whose gains we are using.
     */
    void MixInto(QuadFrame32& dest, size_t intermediate_mix_id) const;

private:
    const size_t source_id;
    StereoFrame16 current_frame;

    using Format = SourceConfiguration::Configuration::Format;
    using InterpolationMode = SourceConfiguration::Configuration::InterpolationMode;
    using MonoOrStereo = SourceConfiguration::Configuration::MonoOrStereo;

    /// Internal representation of a buffer for our buffer queue
    struct Buffer {
        u32 length;
        u16 buffer_id;
        MonoOrStereo mono_or_stereo;
        Format format;
    };

    struct BufferOrder {
        bool operator()(const Buffer& a, const Buffer& b) const {
            // Lower buffer_id comes first.
            return a.buffer_id > b.buffer_id;
        }
    };

    struct {

        // TODO: STATES HERE

        // Mixing

        std::array<std::array<float, 4>, 3> gain = {};

        // Buffer queue

        std::priority_queue<Buffer, std::vector<Buffer>, BufferOrder> input_queue;
        MonoOrStereo mono_or_stereo = MonoOrStereo::Mono;
        Format format = Format::ADPCM;
    } state;

    // Internal functions

    /// INTERNAL: Generate the current audio output for this frame based on our internal state.
    void GenerateFrame();
};

} // namespace HLE
} // namespace AudioCore
