// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <QDialog>

#include "common/param_package.h"
#include "core/settings.h"
#include "ui_configure_input.h"
#include "yuzu/uisettings.h"

class QCheckBox;
class QKeyEvent;
class QLabel;
class QPushButton;
class QString;
class QTimer;

namespace InputCommon::Polling {
class DevicePoller;
enum class DeviceType;
} // namespace InputCommon::Polling

namespace Ui {
class ConfigureInputPlayer;
}

class ConfigureInputPlayer : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureInputPlayer(QWidget* parent, std::size_t player_index, QWidget* bottom_row,
                                  bool debug = false);
    ~ConfigureInputPlayer() override;

    /// Save all button configurations to settings file
    void ApplyConfiguration();

    /// Clear all input configuration
    void ClearAll();

    /// Restore all buttons to their default values.
    void RestoreDefaults();

    /// Set the connection state checkbox (used to sync state)
    void ConnectPlayer(bool connected);

signals:
    /// Emitted when this controller is connected by the user
    void Connected(bool connected);
    /// Emitted when the first player controller selects Handheld mode (undocked with dual joycons
    /// attached)
    void HandheldStateChanged(bool is_handheld);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void OnControllerButtonClick(int i);

    /// Load configuration settings.
    void LoadConfiguration();

    /// Update UI to reflect current configuration.
    void UpdateButtonLabels();

    /// Called when the button was pressed.
    void HandleClick(QPushButton* button,
                     std::function<void(const Common::ParamPackage&)> new_input_setter,
                     InputCommon::Polling::DeviceType type);

    /// Finish polling and configure input using the input_setter
    void SetPollingResult(const Common::ParamPackage& params, bool abort);

    /// Handle key press events.
    void keyPressEvent(QKeyEvent* event) override;

    /// Update the current controller icon
    void UpdateControllerIcon();

    /// Hides and disables controller settings based on the current controller type
    void UpdateControllerAvailableButtons();

    /// Gets the default controller mapping for this device and auto configures the input to match
    void UpdateMappingWithDefaults();

    void NewProfile();
    void DeleteProfile();
    void RenameProfile();
    bool IsProfileNameDuplicate(const QString& name) const;
    void WarnProposedProfileNameIsDuplicate();

    std::unique_ptr<Ui::ConfigureInputPlayer> ui;

    std::size_t player_index;
    bool debug;

    std::unique_ptr<QTimer> timeout_timer;
    std::unique_ptr<QTimer> poll_timer;

    static constexpr int PLAYER_COUNT = 8;
    std::array<QCheckBox*, PLAYER_COUNT> player_connected_checkbox;

    /// This will be the the setting function when an input is awaiting configuration.
    std::optional<std::function<void(const Common::ParamPackage&)>> input_setter;

    std::array<Common::ParamPackage, Settings::NativeButton::NumButtons> buttons_param;
    std::array<Common::ParamPackage, Settings::NativeAnalog::NumAnalogs> analogs_param;

    static constexpr int ANALOG_SUB_BUTTONS_NUM = 4;
    // Adds room for two extra push buttons LStick Modifier and RStick Modifier
    static constexpr int BUTTON_MAP_COUNT = Settings::NativeButton::NumButtons + 2;

    /// Each button input is represented by a QPushButton.
    std::array<QPushButton*, BUTTON_MAP_COUNT> button_map;
    /// Extra buttons for the modifiers
    Common::ParamPackage lstick_mod;
    Common::ParamPackage rstick_mod;

    /// A group of four QPushButtons represent one analog input. The buttons each represent up,
    /// down, left, right, respectively.
    std::array<std::array<QPushButton*, ANALOG_SUB_BUTTONS_NUM>, Settings::NativeAnalog::NumAnalogs>
        analog_map_buttons;

    /// Analog inputs are also represented each with a single button, used to configure with an
    /// actual analog stick
    std::array<QPushButton*, Settings::NativeAnalog::NumAnalogs> analog_map_modifier;
    std::array<QSlider*, Settings::NativeAnalog::NumAnalogs> analog_map_deadzone;
    std::array<QLabel*, Settings::NativeAnalog::NumAnalogs> analog_map_deadzone_label;

    static const std::array<std::string, ANALOG_SUB_BUTTONS_NUM> analog_sub_buttons;

    std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> device_pollers;

    /// A flag to indicate if keyboard keys are okay when configuring an input. If this is false,
    /// keyboard events are ignored.
    bool want_keyboard_keys = false;

    std::array<QPushButton*, 4> controller_color_buttons;
    std::array<QColor, 4> controller_colors;

    /// List of physical devices users can map with. If a SDL backed device is selected, then you
    /// can usue this device to get a default mapping
    std::vector<Common::ParamPackage> input_devices;

    /// Bottom row is where console wide settings are held, and its "owned" by the parent
    /// ConfigureInput widget. On show, add this widget to the main layout. This will change the
    /// parent of the widget to this widget (but thats fine)
    QWidget* bottom_row;
};
