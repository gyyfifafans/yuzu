// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <SDL.h>
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/param_package.h"
#include "common/threadsafe_queue.h"
#include "core/frontend/input.h"
#include "input_common/sdl/sdl_impl.h"

namespace InputCommon::SDL {

namespace {
std::string GetGUID(SDL_Joystick* joystick) {
    const SDL_JoystickGUID guid = SDL_JoystickGetGUID(joystick);
    char guid_str[33];
    SDL_JoystickGetGUIDString(guid, guid_str, sizeof(guid_str));
    return guid_str;
}

/// Creates a ParamPackage from an SDL_Event that can directly be used to create a ButtonDevice
Common::ParamPackage SDLEventToButtonParamPackage(SDLState& state, const SDL_Event& event);
} // namespace

static int SDLEventWatcher(void* user_data, SDL_Event* event) {
    auto* const sdl_state = static_cast<SDLState*>(user_data);

    // Don't handle the event if we are configuring
    if (sdl_state->polling) {
        sdl_state->event_queue.Push(*event);
    } else {
        sdl_state->HandleGameControllerEvent(*event);
    }

    return 0;
}

class SDLJoystick {
public:
    SDLJoystick(std::string guid_, int port_, SDL_Joystick* joystick,
                SDL_GameController* gamecontroller)
        : guid{std::move(guid_)}, port{port_}, sdl_joystick{joystick, &SDL_JoystickClose},
          sdl_controller{gamecontroller, &SDL_GameControllerClose} {}

    void SetButton(int button, bool value) {
        std::lock_guard lock{mutex};
        state.buttons.insert_or_assign(button, value);
    }

    bool GetButton(int button) const {
        std::lock_guard lock{mutex};
        return state.buttons.at(button);
    }

    void SetAxis(int axis, Sint16 value) {
        std::lock_guard lock{mutex};
        state.axes.insert_or_assign(axis, value);
    }

    float GetAxis(int axis) const {
        std::lock_guard lock{mutex};
        return state.axes.at(axis) / 32767.0f;
    }

    std::tuple<float, float> GetAnalog(int axis_x, int axis_y) const {
        float x = GetAxis(axis_x);
        float y = GetAxis(axis_y);
        y = -y; // 3DS uses an y-axis inverse from SDL

        // Make sure the coordinates are in the unit circle,
        // otherwise normalize it.
        float r = x * x + y * y;
        if (r > 1.0f) {
            r = std::sqrt(r);
            x /= r;
            y /= r;
        }

        return std::make_tuple(x, y);
    }

    void SetHat(int hat, Uint8 direction) {
        std::lock_guard lock{mutex};
        state.hats.insert_or_assign(hat, direction);
    }

    bool GetHatDirection(int hat, Uint8 direction) const {
        std::lock_guard lock{mutex};
        return (state.hats.at(hat) & direction) != 0;
    }
    /**
     * The guid of the joystick
     */
    const std::string& GetGUID() const {
        return guid;
    }

    /**
     * The number of joystick from the same type that were connected before this joystick
     */
    int GetPort() const {
        return port;
    }

    SDL_Joystick* GetSDLJoystick() const {
        return sdl_joystick.get();
    }

    void SetSDLJoystick(SDL_Joystick* joystick, SDL_GameController* controller) {
        sdl_controller.reset(controller);
        sdl_joystick.reset(joystick);
    }

    SDL_GameController* GetSDLGameController() const {
        return sdl_controller.get();
    }

private:
    struct State {
        std::unordered_map<int, bool> buttons;
        std::unordered_map<int, Sint16> axes;
        std::unordered_map<int, Uint8> hats;
    } state;
    std::string guid;
    int port;
    std::unique_ptr<SDL_Joystick, decltype(&SDL_JoystickClose)> sdl_joystick;
    std::unique_ptr<SDL_GameController, decltype(&SDL_GameControllerClose)> sdl_controller;
    mutable std::mutex mutex;
};

std::shared_ptr<SDLJoystick> SDLState::GetSDLJoystickByGUID(const std::string& guid, int port) {
    std::lock_guard lock{joystick_map_mutex};
    const auto it = joystick_map.find(guid);
    if (it != joystick_map.end()) {
        while (it->second.size() <= static_cast<std::size_t>(port)) {
            auto joystick = std::make_shared<SDLJoystick>(guid, static_cast<int>(it->second.size()),
                                                          nullptr, nullptr);
            it->second.emplace_back(std::move(joystick));
        }
        return it->second[port];
    }
    auto joystick = std::make_shared<SDLJoystick>(guid, 0, nullptr, nullptr);
    return joystick_map[guid].emplace_back(std::move(joystick));
}

std::shared_ptr<SDLJoystick> SDLState::GetSDLJoystickBySDLID(SDL_JoystickID sdl_id) {
    auto sdl_joystick = SDL_JoystickFromInstanceID(sdl_id);
    auto sdl_controller = SDL_GameControllerFromInstanceID(sdl_id);
    const std::string guid = GetGUID(sdl_joystick);

    std::lock_guard lock{joystick_map_mutex};
    const auto map_it = joystick_map.find(guid);
    if (map_it != joystick_map.end()) {
        const auto vec_it =
            std::find_if(map_it->second.begin(), map_it->second.end(),
                         [&sdl_joystick](const std::shared_ptr<SDLJoystick>& joystick) {
                             return sdl_joystick == joystick->GetSDLJoystick();
                         });
        if (vec_it != map_it->second.end()) {
            // This is the common case: There is already an existing SDL_Joystick maped to a
            // SDLJoystick. return the SDLJoystick
            return *vec_it;
        }

        // Search for a SDLJoystick without a mapped SDL_Joystick...
        const auto nullptr_it = std::find_if(map_it->second.begin(), map_it->second.end(),
                                             [](const std::shared_ptr<SDLJoystick>& joystick) {
                                                 return !joystick->GetSDLJoystick();
                                             });
        if (nullptr_it != map_it->second.end()) {
            // ... and map it
            (*nullptr_it)->SetSDLJoystick(sdl_joystick, sdl_controller);
            return *nullptr_it;
        }

        // There is no SDLJoystick without a mapped SDL_Joystick
        // Create a new SDLJoystick
        const int port = static_cast<int>(map_it->second.size());
        auto joystick = std::make_shared<SDLJoystick>(guid, port, sdl_joystick, sdl_controller);
        return map_it->second.emplace_back(std::move(joystick));
    }

    auto joystick = std::make_shared<SDLJoystick>(guid, 0, sdl_joystick, sdl_controller);
    return joystick_map[guid].emplace_back(std::move(joystick));
}

void SDLState::InitJoystick(int joystick_index) {
    SDL_Joystick* sdl_joystick = SDL_JoystickOpen(joystick_index);
    SDL_GameController* sdl_gamecontroller = nullptr;
    if (SDL_IsGameController(joystick_index)) {
        sdl_gamecontroller = SDL_GameControllerOpen(joystick_index);
    }
    if (!sdl_joystick) {
        LOG_ERROR(Input, "failed to open joystick {}", joystick_index);
        return;
    }
    const std::string guid = GetGUID(sdl_joystick);

    std::lock_guard lock{joystick_map_mutex};
    if (joystick_map.find(guid) == joystick_map.end()) {
        auto joystick = std::make_shared<SDLJoystick>(guid, 0, sdl_joystick, sdl_gamecontroller);
        joystick_map[guid].emplace_back(std::move(joystick));
        return;
    }
    auto& joystick_guid_list = joystick_map[guid];
    const auto it = std::find_if(
        joystick_guid_list.begin(), joystick_guid_list.end(),
        [](const std::shared_ptr<SDLJoystick>& joystick) { return !joystick->GetSDLJoystick(); });
    if (it != joystick_guid_list.end()) {
        (*it)->SetSDLJoystick(sdl_joystick, sdl_gamecontroller);
        return;
    }
    const int port = static_cast<int>(joystick_guid_list.size());
    auto joystick = std::make_shared<SDLJoystick>(guid, port, sdl_joystick, sdl_gamecontroller);
    joystick_guid_list.emplace_back(std::move(joystick));
}

void SDLState::CloseJoystick(SDL_Joystick* sdl_joystick) {
    const std::string guid = GetGUID(sdl_joystick);

    std::shared_ptr<SDLJoystick> joystick;
    {
        std::lock_guard lock{joystick_map_mutex};
        // This call to guid is safe since the joystick is guaranteed to be in the map
        const auto& joystick_guid_list = joystick_map[guid];
        const auto joystick_it =
            std::find_if(joystick_guid_list.begin(), joystick_guid_list.end(),
                         [&sdl_joystick](const std::shared_ptr<SDLJoystick>& joystick) {
                             return joystick->GetSDLJoystick() == sdl_joystick;
                         });
        joystick = *joystick_it;
    }

    // Destruct SDL_Joystick outside the lock guard because SDL can internally call the
    // event callback which locks the mutex again.
    joystick->SetSDLJoystick(nullptr, nullptr);
}

void SDLState::HandleGameControllerEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_JOYBUTTONUP: {
        if (auto joystick = GetSDLJoystickBySDLID(event.jbutton.which)) {
            joystick->SetButton(event.jbutton.button, false);
        }
        break;
    }
    case SDL_JOYBUTTONDOWN: {
        if (auto joystick = GetSDLJoystickBySDLID(event.jbutton.which)) {
            joystick->SetButton(event.jbutton.button, true);
        }
        break;
    }
    case SDL_JOYHATMOTION: {
        if (auto joystick = GetSDLJoystickBySDLID(event.jhat.which)) {
            joystick->SetHat(event.jhat.hat, event.jhat.value);
        }
        break;
    }
    case SDL_JOYAXISMOTION: {
        if (auto joystick = GetSDLJoystickBySDLID(event.jaxis.which)) {
            joystick->SetAxis(event.jaxis.axis, event.jaxis.value);
        }
        break;
    }
    case SDL_JOYDEVICEREMOVED:
        LOG_DEBUG(Input, "Controller removed with Instance_ID {}", event.jdevice.which);
        CloseJoystick(SDL_JoystickFromInstanceID(event.jdevice.which));
        break;
    case SDL_JOYDEVICEADDED:
        LOG_DEBUG(Input, "Controller connected with device index {}", event.jdevice.which);
        InitJoystick(event.jdevice.which);
        break;
    }
}

void SDLState::CloseJoysticks() {
    std::lock_guard lock{joystick_map_mutex};
    joystick_map.clear();
}

class SDLButton final : public Input::ButtonDevice {
public:
    explicit SDLButton(std::shared_ptr<SDLJoystick> joystick_, int button_)
        : joystick(std::move(joystick_)), button(button_) {}

    bool GetStatus() const override {
        return joystick->GetButton(button);
    }

private:
    std::shared_ptr<SDLJoystick> joystick;
    int button;
};

class SDLDirectionButton final : public Input::ButtonDevice {
public:
    explicit SDLDirectionButton(std::shared_ptr<SDLJoystick> joystick_, int hat_, Uint8 direction_)
        : joystick(std::move(joystick_)), hat(hat_), direction(direction_) {}

    bool GetStatus() const override {
        return joystick->GetHatDirection(hat, direction);
    }

private:
    std::shared_ptr<SDLJoystick> joystick;
    int hat;
    Uint8 direction;
};

class SDLAxisButton final : public Input::ButtonDevice {
public:
    explicit SDLAxisButton(std::shared_ptr<SDLJoystick> joystick_, int axis_, float threshold_,
                           bool trigger_if_greater_)
        : joystick(std::move(joystick_)), axis(axis_), threshold(threshold_),
          trigger_if_greater(trigger_if_greater_) {}

    bool GetStatus() const override {
        const float axis_value = joystick->GetAxis(axis);
        if (trigger_if_greater) {
            return axis_value > threshold;
        }
        return axis_value < threshold;
    }

private:
    std::shared_ptr<SDLJoystick> joystick;
    int axis;
    float threshold;
    bool trigger_if_greater;
};

class SDLAnalog final : public Input::AnalogDevice {
public:
    SDLAnalog(std::shared_ptr<SDLJoystick> joystick_, int axis_x_, int axis_y_, float deadzone_)
        : joystick(std::move(joystick_)), axis_x(axis_x_), axis_y(axis_y_), deadzone(deadzone_) {}

    std::tuple<float, float> GetStatus() const override {
        const auto [x, y] = joystick->GetAnalog(axis_x, axis_y);
        const float r = std::sqrt((x * x) + (y * y));
        if (r > deadzone) {
            return std::make_tuple(x / r * (r - deadzone) / (1 - deadzone),
                                   y / r * (r - deadzone) / (1 - deadzone));
        }
        return std::make_tuple<float, float>(0.0f, 0.0f);
    }

    bool GetAnalogDirectionStatus(Input::AnalogDirection direction) const override {
        const auto [x, y] = GetStatus();
        const float directional_deadzone = 0.4f;
        switch (direction) {
        case Input::AnalogDirection::RIGHT:
            return x > directional_deadzone;
        case Input::AnalogDirection::LEFT:
            return x < -directional_deadzone;
        case Input::AnalogDirection::UP:
            return y > directional_deadzone;
        case Input::AnalogDirection::DOWN:
            return y < -directional_deadzone;
        }
        return false;
    }

private:
    std::shared_ptr<SDLJoystick> joystick;
    const int axis_x;
    const int axis_y;
    const float deadzone;
};

/// A button device factory that creates button devices from SDL joystick
class SDLButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    explicit SDLButtonFactory(SDLState& state_) : state(state_) {}

    /**
     * Creates a button device from a joystick button
     * @param params contains parameters for creating the device:
     *     - "guid": the guid of the joystick to bind
     *     - "port": the nth joystick of the same type to bind
     *     - "button"(optional): the index of the button to bind
     *     - "hat"(optional): the index of the hat to bind as direction buttons
     *     - "axis"(optional): the index of the axis to bind
     *     - "direction"(only used for hat): the direction name of the hat to bind. Can be "up",
     *         "down", "left" or "right"
     *     - "threshold"(only used for axis): a float value in (-1.0, 1.0) which the button is
     *         triggered if the axis value crosses
     *     - "direction"(only used for axis): "+" means the button is triggered when the axis
     * value is greater than the threshold; "-" means the button is triggered when the axis
     * value is smaller than the threshold
     */
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        const std::string guid = params.Get("guid", "0");
        const int port = params.Get("port", 0);

        auto joystick = state.GetSDLJoystickByGUID(guid, port);

        if (params.Has("hat")) {
            const int hat = params.Get("hat", 0);
            const std::string direction_name = params.Get("direction", "");
            Uint8 direction;
            if (direction_name == "up") {
                direction = SDL_HAT_UP;
            } else if (direction_name == "down") {
                direction = SDL_HAT_DOWN;
            } else if (direction_name == "left") {
                direction = SDL_HAT_LEFT;
            } else if (direction_name == "right") {
                direction = SDL_HAT_RIGHT;
            } else {
                direction = 0;
            }
            // This is necessary so accessing GetHat with hat won't crash
            joystick->SetHat(hat, SDL_HAT_CENTERED);
            return std::make_unique<SDLDirectionButton>(joystick, hat, direction);
        }

        if (params.Has("axis")) {
            const int axis = params.Get("axis", 0);
            const float threshold = params.Get("threshold", 0.5f);
            const std::string direction_name = params.Get("direction", "");
            bool trigger_if_greater;
            if (direction_name == "+") {
                trigger_if_greater = true;
            } else if (direction_name == "-") {
                trigger_if_greater = false;
            } else {
                trigger_if_greater = true;
                LOG_ERROR(Input, "Unknown direction {}", direction_name);
            }
            // This is necessary so accessing GetAxis with axis won't crash
            joystick->SetAxis(axis, 0);
            return std::make_unique<SDLAxisButton>(joystick, axis, threshold, trigger_if_greater);
        }

        const int button = params.Get("button", 0);
        // This is necessary so accessing GetButton with button won't crash
        joystick->SetButton(button, false);
        return std::make_unique<SDLButton>(joystick, button);
    }

private:
    SDLState& state;
};

/// An analog device factory that creates analog devices from SDL joystick
class SDLAnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    explicit SDLAnalogFactory(SDLState& state_) : state(state_) {}
    /**
     * Creates analog device from joystick axes
     * @param params contains parameters for creating the device:
     *     - "guid": the guid of the joystick to bind
     *     - "port": the nth joystick of the same type
     *     - "axis_x": the index of the axis to be bind as x-axis
     *     - "axis_y": the index of the axis to be bind as y-axis
     */
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        const std::string guid = params.Get("guid", "0");
        const int port = params.Get("port", 0);
        const int axis_x = params.Get("axis_x", 0);
        const int axis_y = params.Get("axis_y", 1);
        const float deadzone = std::clamp(params.Get("deadzone", 0.0f), 0.0f, .99f);

        auto joystick = state.GetSDLJoystickByGUID(guid, port);

        // This is necessary so accessing GetAxis with axis_x and axis_y won't crash
        joystick->SetAxis(axis_x, 0);
        joystick->SetAxis(axis_y, 0);
        return std::make_unique<SDLAnalog>(joystick, axis_x, axis_y, deadzone);
    }

private:
    SDLState& state;
};

SDLState::SDLState() {
    using namespace Input;
    analog_factory = std::make_shared<SDLAnalogFactory>(*this);
    button_factory = std::make_shared<SDLButtonFactory>(*this);
    RegisterFactory<AnalogDevice>("sdl", analog_factory);
    RegisterFactory<ButtonDevice>("sdl", button_factory);

    // If the frontend is going to manage the event loop, then we dont start one here
    start_thread = !SDL_WasInit(SDL_INIT_JOYSTICK);
    if (start_thread && SDL_Init(SDL_INIT_JOYSTICK) < 0) {
        LOG_CRITICAL(Input, "SDL_Init(SDL_INIT_JOYSTICK) failed with: {}", SDL_GetError());
        return;
    }
    has_gamecontroller = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1") == SDL_FALSE) {
        LOG_ERROR(Input, "Failed to set hint for background events with: {}", SDL_GetError());
    }

    SDL_AddEventWatch(&SDLEventWatcher, this);

    initialized = true;
    if (start_thread) {
        poll_thread = std::thread([this] {
            using namespace std::chrono_literals;
            while (initialized) {
                SDL_PumpEvents();
                std::this_thread::sleep_for(10ms);
            }
        });
    }
    // Because the events for joystick connection happens before we have our event watcher added, we
    // can just open all the joysticks right here
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        InitJoystick(i);
    }
}

SDLState::~SDLState() {
    using namespace Input;
    UnregisterFactory<ButtonDevice>("sdl");
    UnregisterFactory<AnalogDevice>("sdl");

    CloseJoysticks();
    SDL_DelEventWatch(&SDLEventWatcher, this);

    initialized = false;
    if (start_thread) {
        poll_thread.join();
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    }
}

std::vector<Common::ParamPackage> SDLState::GetInputDevices() {
    std::scoped_lock lock(joystick_map_mutex);
    std::vector<Common::ParamPackage> devices = {};
    for (const auto & [key, value] : joystick_map) {
        for (const auto& joystick : value) {
            auto controller = joystick->GetSDLGameController();
            auto joy = joystick->GetSDLJoystick();
            if (controller) {
                std::string name = SDL_GameControllerName(controller);
                devices.emplace_back(Common::ParamPackage{
                    {"class", "sdl"},
                    {"display", name},
                    {"guid", joystick->GetGUID()},
                    {"port", std::to_string(joystick->GetPort())},
                });
            } else if (joy) {
                std::string name = SDL_JoystickName(joy);
                devices.emplace_back(Common::ParamPackage{
                    {"class", "sdl"},
                    {"display", name},
                    {"guid", joystick->GetGUID()},
                    {"port", std::to_string(joystick->GetPort())},
                });
            }
        }
    }
    return devices;
}

namespace {
Common::ParamPackage BuildAnalogParamPackageForButton(int port, std::string guid, u8 axis,
                                                      float value = 0.1) {
    Common::ParamPackage params({{"engine", "sdl"}});
    params.Set("port", port);
    params.Set("guid", guid);
    params.Set("axis", axis);
    if (value > 0) {
        params.Set("direction", "+");
        params.Set("threshold", "0.5");
    } else {
        params.Set("direction", "-");
        params.Set("threshold", "-0.5");
    }
    return params;
}

Common::ParamPackage BuildButtonParamPackageForButton(int port, std::string guid, u8 button) {
    Common::ParamPackage params({{"engine", "sdl"}});
    params.Set("port", port);
    params.Set("guid", guid);
    params.Set("button", button);
    return params;
}

Common::ParamPackage BuildHatParamPackageForButton(int port, std::string guid, u8 hat, u8 value) {
    Common::ParamPackage params({{"engine", "sdl"}});

    params.Set("port", port);
    params.Set("guid", guid);
    params.Set("hat", hat);
    switch (value) {
    case SDL_HAT_UP:
        params.Set("direction", "up");
        break;
    case SDL_HAT_DOWN:
        params.Set("direction", "down");
        break;
    case SDL_HAT_LEFT:
        params.Set("direction", "left");
        break;
    case SDL_HAT_RIGHT:
        params.Set("direction", "right");
        break;
    default:
        return {};
    }
    return params;
}

Common::ParamPackage SDLEventToButtonParamPackage(SDLState& state, const SDL_Event& event) {
    Common::ParamPackage params{};

    switch (event.type) {
    case SDL_JOYAXISMOTION: {
        const auto joystick = state.GetSDLJoystickBySDLID(event.jaxis.which);
        params = BuildAnalogParamPackageForButton(joystick->GetPort(), joystick->GetGUID(),
                                                  event.jaxis.axis, event.jaxis.value);
        break;
    }
    case SDL_JOYBUTTONUP: {
        const auto joystick = state.GetSDLJoystickBySDLID(event.jbutton.which);
        params = BuildButtonParamPackageForButton(joystick->GetPort(), joystick->GetGUID(),
                                                  event.jbutton.button);
        break;
    }
    case SDL_JOYHATMOTION: {
        const auto joystick = state.GetSDLJoystickBySDLID(event.jhat.which);
        params = BuildHatParamPackageForButton(joystick->GetPort(), joystick->GetGUID(),
                                               event.jhat.hat, event.jhat.value);
        break;
    }
    }
    return params;
}

Common::ParamPackage BuildParamPackageForBinding(int port, const std::string& guid,
                                                 const SDL_GameControllerButtonBind& binding) {
    Common::ParamPackage out{};
    switch (binding.bindType) {
    case SDL_CONTROLLER_BINDTYPE_AXIS:
        out = BuildAnalogParamPackageForButton(port, guid, binding.value.axis);
        break;
    case SDL_CONTROLLER_BINDTYPE_BUTTON:
        out = BuildButtonParamPackageForButton(port, guid, binding.value.button);
        break;
    case SDL_CONTROLLER_BINDTYPE_HAT:
        out = BuildHatParamPackageForButton(port, guid, binding.value.hat.hat,
                                            binding.value.hat.hat_mask);
        break;
    default:
        break;
    }
    return out;
};

Common::ParamPackage BuildParamPackageForAnalog(int port, const std::string& guid, int axis_x,
                                                int axis_y) {
    Common::ParamPackage params{};
    params.Set("engine", "sdl");
    params.Set("port", port);
    params.Set("guid", guid);
    params.Set("axis_x", axis_x);
    params.Set("axis_y", axis_y);
    return params;
}
} // namespace

ButtonMapping SDLState::GetButtonMappingForDevice(const Common::ParamPackage& params) {
    // This list is missing L/R since those are not considered buttons in SDL GameController.
    // We will add those afterwards
    // This list also excludes Screenshoot since theres not really a mapping for that
    std::unordered_map<Settings::NativeButton::Values, SDL_GameControllerButton>
        switch_to_sdl_button = {
            {Settings::NativeButton::A, SDL_CONTROLLER_BUTTON_B},
            {Settings::NativeButton::B, SDL_CONTROLLER_BUTTON_A},
            {Settings::NativeButton::X, SDL_CONTROLLER_BUTTON_Y},
            {Settings::NativeButton::Y, SDL_CONTROLLER_BUTTON_X},
            {Settings::NativeButton::LStick, SDL_CONTROLLER_BUTTON_LEFTSTICK},
            {Settings::NativeButton::RStick, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
            {Settings::NativeButton::L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
            {Settings::NativeButton::R, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
            {Settings::NativeButton::Plus, SDL_CONTROLLER_BUTTON_START},
            {Settings::NativeButton::Minus, SDL_CONTROLLER_BUTTON_BACK},
            {Settings::NativeButton::DLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
            {Settings::NativeButton::DUp, SDL_CONTROLLER_BUTTON_DPAD_UP},
            {Settings::NativeButton::DRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
            {Settings::NativeButton::DDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
            {Settings::NativeButton::SL, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
            {Settings::NativeButton::SR, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
            {Settings::NativeButton::Home, SDL_CONTROLLER_BUTTON_GUIDE},
        };
    if (!params.Has("guid") || !params.Has("port")) {
        return {};
    }
    const auto joystick = GetSDLJoystickByGUID(params.Get("guid", ""), params.Get("port", 0));
    auto controller = joystick->GetSDLGameController();
    if (!controller) {
        return {};
    }

    ButtonMapping mapping{};
    for (const auto & [switch_button, sdl_button] : switch_to_sdl_button) {
        const auto& binding = SDL_GameControllerGetBindForButton(controller, sdl_button);
        mapping[switch_button] =
            BuildParamPackageForBinding(joystick->GetPort(), joystick->GetGUID(), binding);
    }

    // Add the missing bindings for L/R
    std::unordered_map<Settings::NativeButton::Values, SDL_GameControllerAxis> switch_to_sdl_axis =
        {
            {Settings::NativeButton::ZL, SDL_CONTROLLER_AXIS_TRIGGERLEFT},
            {Settings::NativeButton::ZR, SDL_CONTROLLER_AXIS_TRIGGERRIGHT},
        };
    for (const auto & [switch_button, sdl_axis] : switch_to_sdl_axis) {
        const auto& binding = SDL_GameControllerGetBindForAxis(controller, sdl_axis);
        mapping[switch_button] =
            BuildParamPackageForBinding(joystick->GetPort(), joystick->GetGUID(), binding);
    }

    return mapping;
}

AnalogMapping SDLState::GetAnalogMappingForDevice(const Common::ParamPackage& params) {
    if (!params.Has("guid") || !params.Has("port")) {
        return {};
    }
    const auto joystick = GetSDLJoystickByGUID(params.Get("guid", ""), params.Get("port", 0));
    auto controller = joystick->GetSDLGameController();
    if (!controller) {
        return {};
    }

    AnalogMapping mapping = {};
    const auto& binding_left_x =
        SDL_GameControllerGetBindForAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    const auto& binding_left_y =
        SDL_GameControllerGetBindForAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    mapping[Settings::NativeAnalog::LStick] =
        BuildParamPackageForAnalog(joystick->GetPort(), joystick->GetGUID(),
                                   binding_left_x.value.axis, binding_left_y.value.axis);
    const auto& binding_right_x =
        SDL_GameControllerGetBindForAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
    const auto& binding_right_y =
        SDL_GameControllerGetBindForAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
    mapping[Settings::NativeAnalog::RStick] =
        BuildParamPackageForAnalog(joystick->GetPort(), joystick->GetGUID(),
                                   binding_right_x.value.axis, binding_right_y.value.axis);
    return mapping;
}

namespace Polling {
class SDLPoller : public InputCommon::Polling::DevicePoller {
public:
    explicit SDLPoller(SDLState& state_) : state(state_) {}

    void Start(std::string device_id) override {
        state.event_queue.Clear();
        state.polling = true;
    }

    void Stop() override {
        state.polling = false;
    }

protected:
    SDLState& state;
};

class SDLButtonPoller final : public SDLPoller {
public:
    explicit SDLButtonPoller(SDLState& state_) : SDLPoller(state_) {}

    Common::ParamPackage GetNextInput() override {
        SDL_Event event;
        while (state.event_queue.Pop(event)) {
            const auto package = FromEvent(event);
            if (package) {
                return *package;
            }
        }
        return {};
    }
    std::optional<Common::ParamPackage> FromEvent(const SDL_Event& event) {
        switch (event.type) {
        case SDL_JOYAXISMOTION:
            if (std::abs(event.jaxis.value / 32767.0) < 0.5) {
                break;
            }
        case SDL_JOYBUTTONUP:
        case SDL_JOYHATMOTION:
            return {SDLEventToButtonParamPackage(state, event)};
        }
        return {};
    }
};

/**
 * Attempts to match the press to a controller joy axis (left/right stick) and if a match
 * isn't found, checks if the event matches anything from SDLButtonPoller and uses that
 * instead
 */
class SDLAnalogPreferredPoller final : public SDLPoller {
public:
    explicit SDLAnalogPreferredPoller(SDLState& state_)
        : SDLPoller(state_), button_poller(state_) {}

    void Start(std::string device_id) override {
        SDLPoller::Start(device_id);
        // Load the game controller
        // Reset stored axes
        analog_x_axis = -1;
        analog_y_axis = -1;
    }

    Common::ParamPackage GetNextInput() override {
        SDL_Event event;
        while (state.event_queue.Pop(event)) {
            // Filter out axis events that are below a threshold
            if (event.type == SDL_JOYAXISMOTION && std::abs(event.jaxis.value / 32767.0) < 0.5) {
                continue;
            }
            // Simplify controller config by testing if game controller support is enabled.
            // If so, attempt to search the mapping for the analog stick that matches this
            // input
            if (event.type == SDL_JOYAXISMOTION) {
                std::string axis = fmt::format("a{}", event.jaxis.axis);
                const auto joystick = state.GetSDLJoystickBySDLID(event.jaxis.which);
                const auto controller = joystick->GetSDLGameController();
                if (controller) {
                    std::string mapping = SDL_GameControllerMapping(controller);
                    // TODO refactor this to be reusable or something
                    // Mapping formats are a string of id,name,key:value pairs... We are
                    // looking for the axis that was just pressed. Axis are prefixed with
                    // `a` and buttons are prefixed with `b` so we search the string for the
                    // pair that matches the axis that was pressed
                    std::stringstream ss(mapping);
                    std::string item;
                    int index = -1;
                    while (std::getline(ss, item, ',')) {
                        ++index;
                        // Skip over the id and name
                        if (index < 2) {
                            continue;
                        }
                        // split the key:value and check if the value is the axis that was
                        // pressed
                        const auto pos = item.find_first_of(':');
                        if (pos == std::string::npos) {
                            continue;
                        }
                        std::string key = item.substr(0, pos);
                        std::string val = item.substr(pos + 1);
                        if (val == axis) {
                            // we found the name of the axis we were looking for, now we can
                            // map this back to a joystick
                            if (key == "leftx" || key == "lefty") {
                                analog_x_axis = SDL_GameControllerGetBindForAxis(
                                                    controller, SDL_CONTROLLER_AXIS_LEFTX)
                                                    .value.axis;
                                analog_y_axis = SDL_GameControllerGetBindForAxis(
                                                    controller, SDL_CONTROLLER_AXIS_LEFTY)
                                                    .value.axis;
                                LOG_ERROR(Frontend, "analog_x_axis {} analog_y_axis {}",
                                          analog_x_axis, analog_y_axis);
                                break;
                            } else if (key == "rightx" || key == "righty") {
                                analog_x_axis = SDL_GameControllerGetBindForAxis(
                                                    controller, SDL_CONTROLLER_AXIS_RIGHTX)
                                                    .value.axis;
                                analog_y_axis = SDL_GameControllerGetBindForAxis(
                                                    controller, SDL_CONTROLLER_AXIS_RIGHTY)
                                                    .value.axis;
                                LOG_ERROR(Frontend, "analog_x_axis {} analog_y_axis {}",
                                          analog_x_axis, analog_y_axis);
                                break;
                            }
                        }
                    }
                    if (analog_x_axis != -1 && analog_y_axis != -1) {
                        break;
                    }
                } else {
                    // theres no automatic mapping available for this controller so just use
                    // the old method of binding the axis to the button?
                }
            }

            // If the press wasn't accepted as a joy axis, check for a button press
            auto button_press = button_poller.FromEvent(event);
            if (button_press) {
                return *button_press;
            }
        }

        if (analog_x_axis != -1 && analog_y_axis != -1) {
            const auto joystick = state.GetSDLJoystickBySDLID(event.jaxis.which);
            auto params = BuildParamPackageForAnalog(joystick->GetPort(), joystick->GetGUID(),
                                                     analog_x_axis, analog_y_axis);
            analog_x_axis = -1;
            analog_y_axis = -1;
            return params;
        }
        return {};
    }

private:
    int analog_x_axis = -1;
    int analog_y_axis = -1;
    SDLButtonPoller button_poller;
};
} // namespace Polling

SDLState::Pollers SDLState::GetPollers(InputCommon::Polling::DeviceType type) {
    Pollers pollers;

    switch (type) {
    case InputCommon::Polling::DeviceType::AnalogPreferred:
        pollers.emplace_back(std::make_unique<Polling::SDLAnalogPreferredPoller>(*this));
        break;
    case InputCommon::Polling::DeviceType::Button:
        pollers.emplace_back(std::make_unique<Polling::SDLButtonPoller>(*this));
        break;
    }

    return pollers;
}

} // namespace InputCommon::SDL
