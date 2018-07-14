// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "audio_core/audio_types.h"
#include "audio_core/hle/common.h"
#include "audio_core/hle/hle.h"
#include "audio_core/hle/mixers.h"
#include "audio_core/hle/shared_memory.h"
#include "audio_core/hle/source.h"
#include "audio_core/sink.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core_timing.h"

namespace AudioCore {

static constexpr u64 audio_frame_ticks = 1310252ull; ///< Units: ARM11 cycles

struct DspHle::Impl final {
public:
    ~Impl();

private:
    StereoFrame16 GenerateCurrentFrame();
    bool Tick();

    HLE::Mixers mixers;

    DspHle& parent;
    CoreTiming::EventType* tick_event;
};

StereoFrame16 DspHle::Impl::GenerateCurrentFrame() {
    std::array<QuadFrame32, 3> intermediate_mixes = {};

    // Generate intermediate mixes
    /*for (size_t i = 0; i < HLE::num_sources; i++) {
        write.source_statuses.status[i] =
            sources[i].Tick(read.source_configurations.config[i], read.adpcm_coefficients.coeff[i]);
        for (size_t mix = 0; mix < 3; mix++) {
            sources[i].MixInto(intermediate_mixes[mix], mix);
        }
    }

    // Generate final mix
    write.dsp_status = mixers.Tick(read.dsp_configuration, read.intermediate_mix_samples,
                                   write.intermediate_mix_samples, intermediate_mixes); */

    StereoFrame16 output_frame = mixers.GetOutput();

   /* // Write current output frame to the shared memory region
    for (size_t samplei = 0; samplei < output_frame.size(); samplei++) {
        for (size_t channeli = 0; channeli < output_frame[0].size(); channeli++) {
            write.final_samples.pcm16[samplei][channeli] = s16_le(output_frame[samplei][channeli]);
        }
    } */

    return output_frame;
}

bool DspHle::Impl::Tick() {
    StereoFrame16 current_frame = {};

    // TODO: Check dsp::DSP semaphore (which indicates emulated application has finished writing to
    // shared memory region)
    current_frame = GenerateCurrentFrame();

    parent.OutputFrame(current_frame);

    return true;
}

DspHle::DspHle() : impl(std::make_unique<Impl>(*this)) {}
DspHle::~DspHle() = default;

} // namespace AudioCore
