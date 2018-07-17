// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "audio_core/audio_interface.h"
#include "audio_core/sink_details.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hle/service/hid/hid.h"
#include "core/settings.h"
#include "video_core/video_core.h"

namespace Settings {

Values values = {};

void Apply() {

    GDBStub::SetServerPort(values.gdbstub_port);
    GDBStub::ToggleServer(values.use_gdbstub);

    VideoCore::g_toggle_framelimit_enabled = values.toggle_framelimit;

    if (VideoCore::g_emu_window) {
        auto layout = VideoCore::g_emu_window->GetFramebufferLayout();
        VideoCore::g_emu_window->UpdateCurrentFramebufferLayout(layout.width, layout.height);
    }

    if (Core::System::GetInstance().IsPoweredOn()) {
        Core::AudioCore().SetSink(values.sink_id);
        Core::AudioCore().EnableStretching(values.enable_audio_stretching);
    }

    Service::HID::ReloadInputDevices();
}

} // namespace Settings
