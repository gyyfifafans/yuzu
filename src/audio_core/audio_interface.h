// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <vector>
#include "audio_core/audio_types.h"
#include "audio_core/time_stretch.h"
#include "common/common_types.h"
#include "core/memory.h"
#include "audio_core/sink_details.h"

namespace AudioCore {

class Sink;

class AudioInterface {
public:
    AudioInterface();
    virtual ~AudioInterface();

    AudioInterface(const AudioInterface&) = delete;
    AudioInterface(AudioInterface&&) = delete;
    AudioInterface& operator=(const AudioInterface&) = delete;
    AudioInterface& operator=(AudioInterface&&) = delete;

    /// Select the sink to use based on sink id.
    void SetSink(const std::string& sink_id);
    /// Get the current sink
    Sink& GetSink();
    /// Enable/Disable audio stretching.
    void EnableStretching(bool enable);

protected:
    void OutputFrame(const StereoFrame16& frame);

private:
    void FlushResidualStretcherAudio();

    std::unique_ptr<Sink> sink;
    bool perform_time_stretching = false;
    TimeStretcher time_stretcher;
};

} // namespace AudioCore
