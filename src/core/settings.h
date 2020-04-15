// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "input_common/settings.h"

namespace Settings {

enum class NANDTotalSize : u64 {
    S29_1GB = 0x747C00000ULL,
};

enum class NANDUserSize : u64 {
    S26GB = 0x680000000ULL,
};

enum class NANDSystemSize : u64 {
    S2_5GB = 0xA0000000,
};

enum class SDMCSize : u64 {
    S1GB = 0x40000000,
    S2GB = 0x80000000,
    S4GB = 0x100000000ULL,
    S8GB = 0x200000000ULL,
    S16GB = 0x400000000ULL,
    S32GB = 0x800000000ULL,
    S64GB = 0x1000000000ULL,
    S128GB = 0x2000000000ULL,
    S256GB = 0x4000000000ULL,
    S1TB = 0x10000000000ULL,
};

enum class RendererBackend {
    OpenGL = 0,
    Vulkan = 1,
};

enum class GPUAccuracy : u32 {
    Normal = 0,
    High = 1,
    Extreme = 2,
};

struct Values {
    // System
    bool use_docked_mode;
    std::optional<u32> rng_seed;
    // Measured in seconds since epoch
    std::optional<std::chrono::seconds> custom_rtc;
    // Set on game boot, reset on stop. Seconds difference between current time and `custom_rtc`
    std::chrono::seconds custom_rtc_differential;

    s32 current_user;
    s32 language_index;
    s32 region_index;
    s32 sound_index;

    // Controls
    std::array<PlayerInput, 10> players;

    bool mouse_enabled;
    std::string mouse_device;
    MouseButtonsRaw mouse_buttons;

    bool keyboard_enabled;
    KeyboardKeysRaw keyboard_keys;
    KeyboardModsRaw keyboard_mods;

    bool debug_pad_enabled;
    ButtonsRaw debug_pad_buttons;
    AnalogsRaw debug_pad_analogs;

    std::string motion_device;
    TouchscreenInput touchscreen;
    std::atomic_bool is_device_reload_pending{true};
    std::string udp_input_address;
    u16 udp_input_port;
    u8 udp_pad_index;

    // Core
    bool use_multi_core;

    // Data Storage
    bool use_virtual_sd;
    bool gamecard_inserted;
    bool gamecard_current_game;
    std::string gamecard_path;
    NANDTotalSize nand_total_size;
    NANDSystemSize nand_system_size;
    NANDUserSize nand_user_size;
    SDMCSize sdmc_size;

    // Renderer
    RendererBackend renderer_backend;
    bool renderer_debug;
    int vulkan_device;

    float resolution_factor;
    int aspect_ratio;
    int max_anisotropy;
    bool use_frame_limit;
    u16 frame_limit;
    bool use_disk_shader_cache;
    GPUAccuracy gpu_accuracy;
    bool use_asynchronous_gpu_emulation;
    bool use_vsync;
    bool force_30fps_mode;
    bool use_fast_gpu_time;

    float bg_red;
    float bg_green;
    float bg_blue;

    std::string log_filter;

    bool use_dev_keys;

    // Audio
    std::string sink_id;
    bool enable_audio_stretching;
    std::string audio_device_id;
    float volume;

    // Debugging
    bool record_frame_times;
    bool use_gdbstub;
    u16 gdbstub_port;
    std::string program_args;
    bool dump_exefs;
    bool dump_nso;
    bool reporting_services;
    bool quest_flag;
    bool disable_cpu_opt;

    // BCAT
    std::string bcat_backend;
    bool bcat_boxcat_local;

    // WebService
    bool enable_telemetry;
    std::string web_api_url;
    std::string yuzu_username;
    std::string yuzu_token;

    // Add-Ons
    std::map<u64, std::vector<std::string>> disabled_addons;
} extern values;

bool IsGPULevelExtreme();
bool IsGPULevelHigh();

void Apply();
void LogSettings();

} // namespace Settings
