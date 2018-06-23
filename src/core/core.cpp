// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <memory>
#include <utility>
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/cpu_thread.h"
#include "core/frontend/emu_window.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/controller.h"
#include "core/hle/service/sm/sm.h"
#include "core/hw/hw.h"
#include "core/loader/loader.h"
#include "core/memory_setup.h"
#include "core/settings.h"
#include "video_core/video_core.h"

/// Runs a CPU core while the system is powered on
void CpuThread::Run() {
    while (Core::System::GetInstance().IsPoweredOn()) {
        core->RunLoop(true);
    }
}

namespace Core {

/*static*/ System System::s_instance;

System::~System() = default;

Cpu& System::CurrentCpuCore() {
    // If multicore is enabled, use host thread to figure out the current CPU core
    if (Settings::values.use_multi_core) {
        const auto search = std::find_if(cpu_core_threads.begin(), cpu_core_threads.end(),
                                         [](auto& t) { return t->IsCurrentRunningThread(); });
        // if none of the extra threads are the current core, then it must be the cpu core thats
        // running on the EmuThread
        if (search == cpu_core_threads.end()) {
            return *cpu_cores[0];
        }
        return *cpu_cores[std::distance(cpu_core_threads.begin(), search)];
    }

    // Otherwise, use single-threaded mode active_core variable
    return *cpu_cores[active_core];
}

System::ResultStatus System::RunLoop(bool tight_loop) {
    status = ResultStatus::Success;

    if (GDBStub::IsServerEnabled()) {
        GDBStub::HandlePacket();

        // If the loop is halted and we want to step, use a tiny (1) number of instructions to
        // execute. Otherwise, get out of the loop function.
        if (GDBStub::GetCpuHaltFlag()) {
            if (GDBStub::GetCpuStepFlag()) {
                GDBStub::SetCpuStepFlag(false);
                tight_loop = false;
            } else {
                return ResultStatus::Success;
            }
        }
    }

    for (active_core = 0; active_core < NUM_CPU_CORES; ++active_core) {
        cpu_cores[active_core]->RunLoop(tight_loop);
        if (Settings::values.use_multi_core) {
            // Cores 1-3 are run on other threads in this mode
            break;
        }
    }

    return status;
}

System::ResultStatus System::SingleStep() {
    return RunLoop(false);
}

System::ResultStatus System::Load(EmuWindow* emu_window, const std::string& filepath) {
    app_loader = Loader::GetLoader(filepath);

    if (!app_loader) {
        NGLOG_CRITICAL(Core, "Failed to obtain loader for {}!", filepath);
        return ResultStatus::ErrorGetLoader;
    }
    std::pair<boost::optional<u32>, Loader::ResultStatus> system_mode =
        app_loader->LoadKernelSystemMode();

    if (system_mode.second != Loader::ResultStatus::Success) {
        NGLOG_CRITICAL(Core, "Failed to determine system mode (Error {})!",
                       static_cast<int>(system_mode.second));

        switch (system_mode.second) {
        case Loader::ResultStatus::ErrorEncrypted:
            return ResultStatus::ErrorLoader_ErrorEncrypted;
        case Loader::ResultStatus::ErrorInvalidFormat:
            return ResultStatus::ErrorLoader_ErrorInvalidFormat;
        case Loader::ResultStatus::ErrorUnsupportedArch:
            return ResultStatus::ErrorUnsupportedArch;
        default:
            return ResultStatus::ErrorSystemMode;
        }
    }

    NGLOG_DEBUG(HW_Memory, "initialized OK");

    CoreTiming::Init();

    current_process = Kernel::Process::Create("main");

    cpu_barrier = std::make_shared<CpuBarrier>();
    for (size_t index = 0; index < cpu_cores.size(); ++index) {
        cpu_cores[index] = std::make_shared<Cpu>(cpu_barrier, index);
    }

    gpu_core = std::make_unique<Tegra::GPU>();
    telemetry_session = std::make_unique<Core::TelemetrySession>();
    service_manager = std::make_shared<Service::SM::ServiceManager>();

    HW::Init();
    Kernel::Init(system_mode.first.get());
    Service::Init(service_manager);
    GDBStub::Init();

    if (!VideoCore::Init(emu_window)) {
        NGLOG_CRITICAL(Core, "Failed to initialize video core!");
        return ResultStatus::ErrorVideoCore;
    }

    const Loader::ResultStatus load_result{app_loader->Load(current_process)};
    if (Loader::ResultStatus::Success != load_result) {
        NGLOG_CRITICAL(Core, "Failed to load ROM (Error {})!", static_cast<int>(load_result));
        System::Shutdown();

        switch (load_result) {
        case Loader::ResultStatus::ErrorEncrypted:
            return ResultStatus::ErrorLoader_ErrorEncrypted;
        case Loader::ResultStatus::ErrorInvalidFormat:
            return ResultStatus::ErrorLoader_ErrorInvalidFormat;
        case Loader::ResultStatus::ErrorUnsupportedArch:
            return ResultStatus::ErrorUnsupportedArch;
        default:
            return ResultStatus::ErrorLoader;
        }
    }
    status = ResultStatus::Success;

    NGLOG_DEBUG(Core, "Initialized OK");
    return status;
}

void System::PrepareReschedule() {
    CurrentCpuCore().PrepareReschedule();
}

PerfStats::Results System::GetAndResetPerfStats() {
    return perf_stats.GetAndResetStats(CoreTiming::GetGlobalTimeUs());
}

const std::shared_ptr<Kernel::Scheduler>& System::Scheduler(size_t core_index) {
    ASSERT(core_index < NUM_CPU_CORES);
    return cpu_cores[core_index]->Scheduler();
}

ARM_Interface& System::ArmInterface(size_t core_index) {
    ASSERT(core_index < NUM_CPU_CORES);
    return cpu_cores[core_index]->ArmInterface();
}

Cpu& System::CpuCore(size_t core_index) {
    ASSERT(core_index < NUM_CPU_CORES);
    return *cpu_cores[core_index];
}

System::ResultStatus System::CpuInit(boost::optional<CpuThreads> cpu_threads) {

    if (Settings::values.use_multi_core) {
        cpu_core_threads = std::move(cpu_threads.get());
        // Core 0 is run on the main thread, so start with core 1
        size_t i = 1;
        for (const auto& thread : cpu_core_threads) {
            thread->SetCpu(cpu_cores[i++]);
            thread->Start();
        }
    }
    // Reset counters and set time origin to current frame
    GetAndResetPerfStats();
    perf_stats.BeginSystemFrame();

    return ResultStatus::Success;
}

void System::Shutdown() {
    // Log last frame performance stats
    auto perf_results = GetAndResetPerfStats();
    Telemetry().AddField(Telemetry::FieldType::Performance, "Shutdown_EmulationSpeed",
                         perf_results.emulation_speed * 100.0);
    Telemetry().AddField(Telemetry::FieldType::Performance, "Shutdown_Framerate",
                         perf_results.game_fps);
    Telemetry().AddField(Telemetry::FieldType::Performance, "Shutdown_Frametime",
                         perf_results.frametime * 1000.0);

    // Shutdown emulation session
    VideoCore::Shutdown();
    GDBStub::Shutdown();
    Service::Shutdown();
    Kernel::Shutdown();
    HW::Shutdown();
    service_manager.reset();
    telemetry_session.reset();
    gpu_core.reset();

    // Close all CPU/threading state
    cpu_barrier->NotifyEnd();
    if (Settings::values.use_multi_core) {
        for (auto& thread : cpu_core_threads) {
            thread->Stop();
            thread.reset();
        }
    }
    for (auto& cpu_core : cpu_cores) {
        cpu_core.reset();
    }
    cpu_barrier.reset();

    // Close core timing
    CoreTiming::Shutdown();

    // Close app loader
    app_loader.reset();

    NGLOG_DEBUG(Core, "Shutdown OK");
}

Service::SM::ServiceManager& System::ServiceManager() {
    return *service_manager;
}

const Service::SM::ServiceManager& System::ServiceManager() const {
    return *service_manager;
}

} // namespace Core
