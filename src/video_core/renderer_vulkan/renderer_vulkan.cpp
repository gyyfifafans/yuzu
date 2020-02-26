// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <optional>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/telemetry.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "core/settings.h"
#include "core/telemetry_session.h"
#include "video_core/gpu.h"
#include "video_core/renderer_vulkan/declarations.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_device.h"
#include "video_core/renderer_vulkan/vk_memory_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_resource_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Vulkan {

namespace {

VkBool32 DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity_,
                       VkDebugUtilsMessageTypeFlagsEXT type,
                       const VkDebugUtilsMessengerCallbackDataEXT* data,
                       [[maybe_unused]] void* user_data) {
    const vk::DebugUtilsMessageSeverityFlagBitsEXT severity{severity_};
    const char* message{data->pMessage};

    if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
        LOG_CRITICAL(Render_Vulkan, "{}", message);
    } else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
        LOG_WARNING(Render_Vulkan, "{}", message);
    } else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) {
        LOG_INFO(Render_Vulkan, "{}", message);
    } else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose) {
        LOG_DEBUG(Render_Vulkan, "{}", message);
    }
    return VK_FALSE;
}

Common::DynamicLibrary OpenVulkanLibrary() {
    Common::DynamicLibrary dl;
#ifdef __APPLE__
    // Check if a path to a specific Vulkan library has been specified.
    char* libvulkan_env = getenv("LIBVULKAN_PATH");
    if (!libvulkan_env || !dl.Open(libvulkan_env)) {
        // Use the libvulkan.dylib from the application bundle.
        std::string filename = File::GetBundleDirectory() + "/Contents/Frameworks/libvulkan.dylib";
        dl.Open(filename.c_str());
    }
#else
    std::string filename = Common::DynamicLibrary::GetVersionedFilename("vulkan", 1);
    if (!dl.Open(filename.c_str())) {
        // Android devices may not have libvulkan.so.1, only libvulkan.so.
        filename = Common::DynamicLibrary::GetVersionedFilename("vulkan");
        dl.Open(filename.c_str());
    }
#endif
    return dl;
}

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const VKDevice& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == vk::DriverIdKHR::eNvidiaProprietary) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == vk::DriverIdKHR::eIntelProprietaryWindows) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }

    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(std::vector<std::string> available_extensions) {
    std::sort(std::begin(available_extensions), std::end(available_extensions));

    static constexpr std::size_t AverageExtensionSize = 64;
    std::string separated_extensions;
    separated_extensions.reserve(available_extensions.size() * AverageExtensionSize);

    const auto end = std::end(available_extensions);
    for (auto extension = std::begin(available_extensions); extension != end; ++extension) {
        if (const bool is_last = extension + 1 == end; is_last) {
            separated_extensions += *extension;
        } else {
            separated_extensions += fmt::format("{},", *extension);
        }
    }
    return separated_extensions;
}

bool SelectInstanceExtensions(const vk::DispatchLoaderDynamic& dldi,
                              std::vector<const char*>* extension_list,
                              const Core::Frontend::WindowSystemType& wstype,
                              bool enable_debug_report) {
    auto available_extension_list = vk::enumerateInstanceExtensionProperties(nullptr, dldi);

    if (available_extension_list.size() == 0) {
        LOG_ERROR(Render_Vulkan, "Vulkan: No extensions supported by instance.");
        return false;
    }

    for (const auto& extension_properties : available_extension_list)
        LOG_INFO(Render_Vulkan, "Available extension: {}", extension_properties.extensionName);

    auto AddExtension = [&](const char* name, bool required) {
        if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
                         [&](const VkExtensionProperties& properties) {
                             return !strcmp(name, properties.extensionName);
                         }) != available_extension_list.end()) {
            LOG_INFO(Render_Vulkan, "Enabling extension: {}", name);
            extension_list->push_back(name);
            return true;
        }
        if (required) {
            LOG_ERROR(Render_Vulkan, "Vulkan: Missing required extension {}.", name);
        }
        return false;
    };

#if defined(VK_USE_PLATFORM_WIN32_KHR)
    if (wstype == Core::Frontend::WindowSystemType::Windows &&
        !AddExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true)) {
        return false;
    }
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    if (wstype == Core::Frontend::WindowSystemType::X11 &&
        !AddExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME, true)) {
        return false;
    }
#endif

    if (!AddExtension(VK_KHR_SURFACE_EXTENSION_NAME, true)) {
        return false;
    }

    // VK_EXT_debug_report
    if (enable_debug_report && !AddExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false)) {
        LOG_WARNING(Render_Vulkan,
                    "Vulkan: Debug report requested, but extension is not available.");
    }

    AddExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, false);
    AddExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false);

    return true;
}

vk::Instance CreateVulkanInstance(const vk::DispatchLoaderDynamic& dldi,
                                  const Core::Frontend::WindowSystemType& wstype,
                                  bool enable_debug) {
    std::vector<const char*> enabled_extensions;
    if (!SelectInstanceExtensions(dldi, &enabled_extensions, wstype, enable_debug))
        return nullptr;

    if (vk::enumerateInstanceVersion(dldi) < VK_MAKE_VERSION(1, 1, 0)) {
        LOG_ERROR(Render_Vulkan, "Vulkan 1.1 is not susupported! Try updating your drivers");
        return nullptr;
    }

    vk::ApplicationInfo app_info = {};
    app_info.pNext = nullptr;
    app_info.pApplicationName = "yuzu Emulator";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "yuzu Emulator";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_MAKE_VERSION(1, 1, 0);

    vk::InstanceCreateInfo instance_create_info = {};
    instance_create_info.pNext = nullptr;
    instance_create_info.flags = vk::InstanceCreateFlags{};
    instance_create_info.pApplicationInfo = &app_info;
    instance_create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
    instance_create_info.ppEnabledExtensionNames = enabled_extensions.data();
    instance_create_info.enabledLayerCount = 0;
    instance_create_info.ppEnabledLayerNames = nullptr;

    // Enable debug layer on debug builds
    if (enable_debug) {
        static const char* layer_names[] = {"VK_LAYER_LUNARG_standard_validation"};
        instance_create_info.enabledLayerCount = 1;
        instance_create_info.ppEnabledLayerNames = layer_names;
    }

    vk::Instance instance;
    vk::Result res = vk::createInstance(&instance_create_info, nullptr, &instance, dldi);
    if (res != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "vkCreateInstance failed: {}", vk::to_string(res));
        return nullptr;
    }

    return instance;
}

} // Anonymous namespace

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& window, Core::System& system)
    : RendererBase(window), system{system} {}

RendererVulkan::~RendererVulkan() {
    ShutDown();
}

void RendererVulkan::SwapBuffers(const Tegra::FramebufferConfig* framebuffer) {
    const auto& layout = render_window.GetFramebufferLayout();
    if (framebuffer && layout.width > 0 && layout.height > 0 && render_window.IsShown()) {
        const VAddr framebuffer_addr = framebuffer->address + framebuffer->offset;
        const bool use_accelerated =
            rasterizer->AccelerateDisplay(*framebuffer, framebuffer_addr, framebuffer->stride);
        const bool is_srgb = use_accelerated && screen_info.is_srgb;
        if (swapchain->HasFramebufferChanged(layout) || swapchain->GetSrgbState() != is_srgb) {
            swapchain->Create(layout.width, layout.height, is_srgb);
            blit_screen->Recreate();
        }

        scheduler->WaitWorker();

        swapchain->AcquireNextImage();
        const auto [fence, render_semaphore] = blit_screen->Draw(*framebuffer, use_accelerated);

        scheduler->Flush(false, render_semaphore);

        if (swapchain->Present(render_semaphore, fence)) {
            blit_screen->Recreate();
        }

        render_window.SwapBuffers();
        rasterizer->TickFrame();
    }

    render_window.PollEvents();
}

std::optional<Core::Frontend::BackendInfo> RendererVulkan::MakeBackendInfo(
    Core::Frontend::WindowSystemType window_type) {
    Core::Frontend::BackendInfo info;
    info.name = "Vulkan";
    info.api_type = Core::Frontend::APIType::Vulkan;
    info.dl = OpenVulkanLibrary();
    if (!info.dl.IsOpen()) {
        return {};
    }

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    if (!info.dl.GetSymbol("vkGetInstanceProcAddr", &vkGetInstanceProcAddr)) {
        return {};
    }

    vk::DispatchLoaderDynamic dldi(vkGetInstanceProcAddr);
    auto instance = CreateVulkanInstance(dldi, window_type, Settings::values.renderer_debug);
    if (!instance) {
        return {};
    }
    dldi.init(instance);

    for (const auto physical_device : instance.enumeratePhysicalDevices(dldi)) {
        info.adapters.push_back(physical_device.getProperties(dldi).deviceName);
    }
    return info;
}

bool RendererVulkan::Init() {
    auto backend_info = render_window.GetBackendInfo(Core::Frontend::APIType::Vulkan);
    if (!backend_info) {
        return false;
    }

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    if (!backend_info->dl.GetSymbol("vkGetInstanceProcAddr", &vkGetInstanceProcAddr)) {
        return false;
    }

    vk::DispatchLoaderDynamic dldi(vkGetInstanceProcAddr);

    // TODO turn on validation layers
    if (!instance) {
        instance = CreateVulkanInstance(dldi, render_window.GetWindowInfo().type,
                                        Settings::values.renderer_debug);
    }
    if (!instance) {
        return false;
    }
    dldi.init(instance);

    std::optional<vk::DebugUtilsMessengerEXT> callback;
    if (Settings::values.renderer_debug && dldi.vkCreateDebugUtilsMessengerEXT) {
        callback = CreateDebugCallback(dldi);
        if (!callback) {
            return false;
        }
    }

    const HWND hWnd = static_cast<HWND>(render_window.GetWindowInfo().render_surface);
    const HINSTANCE hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtr(hWnd, -6));

    const vk::Win32SurfaceCreateInfoKHR win32_ci({}, hInstance, hWnd);
    const vk::Result result = instance.createWin32SurfaceKHR(&win32_ci, nullptr, &surface, dldi);
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Ooops");
        return false;
    }

    if (!PickDevices(dldi)) {
        if (callback) {
            instance.destroy(*callback, nullptr, dldi);
        }
        return false;
    }
    debug_callback = UniqueDebugUtilsMessengerEXT(
        *callback, vk::ObjectDestroy<vk::Instance, vk::DispatchLoaderDynamic>(
                       instance, nullptr, device->GetDispatchLoader()));

    Report();

    memory_manager = std::make_unique<VKMemoryManager>(*device);

    resource_manager = std::make_unique<VKResourceManager>(*device);

    const auto& framebuffer = render_window.GetFramebufferLayout();
    swapchain = std::make_unique<VKSwapchain>(surface, *device);
    swapchain->Create(framebuffer.width, framebuffer.height, false);

    scheduler = std::make_unique<VKScheduler>(*device, *resource_manager);

    rasterizer = std::make_unique<RasterizerVulkan>(system, render_window, screen_info, *device,
                                                    *resource_manager, *memory_manager, *scheduler);

    blit_screen = std::make_unique<VKBlitScreen>(system, render_window, *rasterizer, *device,
                                                 *resource_manager, *memory_manager, *swapchain,
                                                 *scheduler, screen_info);

    return true;
}

void RendererVulkan::ShutDown() {
    if (!device) {
        return;
    }
    const auto dev = device->GetLogical();
    const auto& dld = device->GetDispatchLoader();
    if (dev && dld.vkDeviceWaitIdle) {
        dev.waitIdle(dld);
    }

    rasterizer.reset();
    blit_screen.reset();
    scheduler.reset();
    swapchain.reset();
    memory_manager.reset();
    resource_manager.reset();
    device.reset();
}

std::optional<vk::DebugUtilsMessengerEXT> RendererVulkan::CreateDebugCallback(
    const vk::DispatchLoaderDynamic& dldi) {
    const vk::DebugUtilsMessengerCreateInfoEXT callback_ci(
        {},
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
        vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
            vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
            vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        &DebugCallback, nullptr);
    vk::DebugUtilsMessengerEXT callback;
    if (instance.createDebugUtilsMessengerEXT(&callback_ci, nullptr, &callback, dldi) !=
        vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Failed to create debug callback");
        return {};
    }
    return callback;
}

bool RendererVulkan::PickDevices(const vk::DispatchLoaderDynamic& dldi) {
    const auto devices = instance.enumeratePhysicalDevices(dldi);

    // TODO(Rodrigo): Choose device from config file
    const s32 device_index = Settings::values.vulkan_device;
    if (device_index < 0 || device_index >= static_cast<s32>(devices.size())) {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        return false;
    }
    const vk::PhysicalDevice physical_device = devices[device_index];

    if (!VKDevice::IsSuitable(dldi, physical_device, surface)) {
        return false;
    }

    device = std::make_unique<VKDevice>(dldi, physical_device, surface);
    return device->Create(dldi, instance);
}

void RendererVulkan::Report() const {
    const std::string vendor_name{device->GetVendorName()};
    const std::string model_name{device->GetModelName()};
    const std::string driver_version = GetDriverVersion(*device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device->GetApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device->GetAvailableExtensions());

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);

    auto& telemetry_session = system.TelemetrySession();
    constexpr auto field = Telemetry::FieldType::UserSystem;
    telemetry_session.AddField(field, "GPU_Vendor", vendor_name);
    telemetry_session.AddField(field, "GPU_Model", model_name);
    telemetry_session.AddField(field, "GPU_Vulkan_Driver", driver_name);
    telemetry_session.AddField(field, "GPU_Vulkan_Version", api_version);
    telemetry_session.AddField(field, "GPU_Vulkan_Extensions", extensions);
}

} // namespace Vulkan