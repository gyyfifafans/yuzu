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

namespace AudioCore {

class Sink;

class DspInterface {
public:
    DspInterface();
    virtual ~DspInterface();

    DspInterface(const DspInterface&) = delete;
    DspInterface(DspInterface&&) = delete;
    DspInterface& operator=(const DspInterface&) = delete;
    DspInterface& operator=(DspInterface&&) = delete;

    /// Returns a reference to the array backing DSP memory
    virtual std::array<u8, Memory::DSP_RAM_SIZE>& GetDspMemory() = 0;

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
