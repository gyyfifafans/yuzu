// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "audio_core/audio_types.h"
#include "audio_core/sink.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core_timing.h"
#include "audio_hle.h"

namespace AudioCore {

struct AudioHle::Impl final {
public:
    explicit Impl(AudioHle& parent);
    ~Impl();

private:
    StereoFrame16 GenerateCurrentFrame();
    bool Tick();

    AudioHle& parent;
    CoreTiming::EventType* tick_event;
};

AudioHle::Impl::Impl(AudioHle& parent_) : parent(parent_) {
}

StereoFrame16 AudioHle::Impl::GenerateCurrentFrame() {
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

    StereoFrame16 output_frame = StereoFrame16();

   /* // Write current output frame to the shared memory region
    for (size_t samplei = 0; samplei < output_frame.size(); samplei++) {
        for (size_t channeli = 0; channeli < output_frame[0].size(); channeli++) {
            write.final_samples.pcm16[samplei][channeli] = s16_le(output_frame[samplei][channeli]);
        }
    } */

    return output_frame;
}

bool AudioHle::Impl::Tick() {
    StereoFrame16 current_frame = {};

    // TODO: Check dsp::DSP semaphore (which indicates emulated application has finished writing to
    // shared memory region)
    current_frame = GenerateCurrentFrame();

    parent.OutputFrame(current_frame);

    return true;
}

AudioHle::AudioHle() : impl(std::make_unique<Impl>(*this)) {}
AudioHle::~AudioHle() = default;

} // namespace AudioCore
