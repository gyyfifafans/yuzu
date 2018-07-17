// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstddef>
#include "audio_core/audio_interface.h"
#include "audio_core/sink.h"
#include "audio_core/sink_details.h"
#include "common/assert.h"

namespace AudioCore {

AudioInterface::AudioInterface() = default;

AudioInterface::~AudioInterface() {
    if (perform_time_stretching) {
        FlushResidualStretcherAudio();
    }
}

void AudioInterface::SetSink(const std::string& sink_id) {
    const SinkDetails& sink_details = GetSinkDetails(sink_id);
    sink = sink_details.factory();
    time_stretcher.SetOutputSampleRate(sink->GetNativeSampleRate());
}

Sink& AudioInterface::GetSink() {
    ASSERT(sink);
    return *sink.get();
}

void AudioInterface::EnableStretching(bool enable) {
    if (perform_time_stretching == enable)
        return;

    if (!enable) {
        FlushResidualStretcherAudio();
    }
    perform_time_stretching = enable;
}

void AudioInterface::OutputFrame(const StereoFrame16& frame) {
    if (!sink)
        return;

    if (perform_time_stretching) {
        time_stretcher.AddSamples(&frame[0][0], frame.size());
        std::vector<s16> stretched_samples = time_stretcher.Process(sink->SamplesInQueue());
        sink->EnqueueSamples(stretched_samples.data(), stretched_samples.size() / 2);
    } else {
        constexpr size_t maximum_sample_latency = 2048; // about 64 miliseconds
        if (sink->SamplesInQueue() > maximum_sample_latency) {
            // This can occur if we're running too fast and samples are starting to back up.
            // Just drop the samples.
            return;
        }

        sink->EnqueueSamples(&frame[0][0], frame.size());
    }
}

void AudioInterface::FlushResidualStretcherAudio() {
    if (!sink)
        return;

    time_stretcher.Flush();
    while (true) {
        std::vector<s16> residual_audio = time_stretcher.Process(sink->SamplesInQueue());
        if (residual_audio.empty())
            break;
        sink->EnqueueSamples(residual_audio.data(), residual_audio.size() / 2);
    }
}

} // namespace AudioCore
