// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <memory>
#include <utility>
#include <QColorDialog>
#include <QGridLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>
#include "common/assert.h"
#include "common/param_package.h"
#include "input_common/main.h"
#include "ui_configure_input_player.h"
#include "yuzu/configuration/config.h"
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/uisettings.h"

const std::array<std::string, ConfigureInputPlayer::ANALOG_SUB_BUTTONS_NUM>
    ConfigureInputPlayer::analog_sub_buttons{{
        "up",
        "down",
        "left",
        "right",
    }};

namespace {

/// Maps the controller type combobox index to Controller Type enum
constexpr Settings::ControllerType GetControllerTypeFromIndex(int index) {
    switch (index) {
    case 0:
    default:
        return Settings::ControllerType::ProController;
    case 1:
        return Settings::ControllerType::DualJoyconDetached;
    case 2:
        return Settings::ControllerType::RightJoycon;
    case 3:
        return Settings::ControllerType::LeftJoycon;
    case 4:
        return Settings::ControllerType::HandheldJoyconAttached;
    }
}

/// Maps the Controller Type enum to controller type combobox index
constexpr int GetIndexFromControllerType(Settings::ControllerType type) {
    switch (type) {
    case Settings::ControllerType::ProController:
    default:
        return 0;
    case Settings::ControllerType::DualJoyconDetached:
        return 1;
    case Settings::ControllerType::RightJoycon:
        return 2;
    case Settings::ControllerType::LeftJoycon:
        return 3;
    case Settings::ControllerType::HandheldJoyconAttached:
        return 4;
    }
}

void LayerGridElements(QGridLayout* grid, QWidget* item, QWidget* onTopOf) {
    const int index1 = grid->indexOf(item);
    const int index2 = grid->indexOf(onTopOf);
    int row, column, rowSpan, columnSpan;
    grid->getItemPosition(index2, &row, &column, &rowSpan, &columnSpan);
    grid->takeAt(index1);
    grid->addWidget(item, row, column, rowSpan, columnSpan);
}

QString GetKeyName(int key_code) {
    switch (key_code) {
    case Qt::Key_Shift:
        return QObject::tr("Shift");
    case Qt::Key_Control:
        return QObject::tr("Ctrl");
    case Qt::Key_Alt:
        return QObject::tr("Alt");
    case Qt::Key_Meta:
        return {};
    default:
        return QKeySequence(key_code).toString();
    }
}

void SetAnalogParam(const Common::ParamPackage& input_param, Common::ParamPackage& analog_param,
                    const std::string& button_name) {
    // The poller returned a complete axis, so set all the buttons
    if (input_param.Has("axis_x") && input_param.Has("axis_y")) {
        analog_param = input_param;
        return;
    }
    // Check if the current configuration has either no engine or an axis binding.
    // Clears out the old binding and adds one with analog_from_button.
    if (!analog_param.Has("engine") || analog_param.Has("axis_x") || analog_param.Has("axis_y")) {
        analog_param = {
            {"engine", "analog_from_button"},
        };
    }
    analog_param.Set(button_name, input_param.Serialize());
}

QString ButtonToText(const Common::ParamPackage& param) {
    if (!param.Has("engine")) {
        return QObject::tr("[not set]");
    }

    if (param.Get("engine", "") == "keyboard") {
        return GetKeyName(param.Get("code", 0));
    }

    if (param.Get("engine", "") == "sdl") {
        if (param.Has("hat")) {
            const QString hat_str = QString::fromStdString(param.Get("hat", ""));
            const QString direction_str = QString::fromStdString(param.Get("direction", ""));

            return QObject::tr("Hat %1 %2").arg(hat_str, direction_str);
        }

        if (param.Has("axis")) {
            const QString axis_str = QString::fromStdString(param.Get("axis", ""));
            const QString direction_str = QString::fromStdString(param.Get("direction", ""));

            return QObject::tr("Axis %1%2").arg(axis_str, direction_str);
        }

        if (param.Has("button")) {
            const QString button_str = QString::fromStdString(param.Get("button", ""));

            return QObject::tr("Button %1").arg(button_str);
        }

        return {};
    }

    return QObject::tr("[unknown]");
}

QString AnalogToText(const Common::ParamPackage& param, const std::string& dir) {
    if (!param.Has("engine")) {
        return QObject::tr("[not set]");
    }

    if (param.Get("engine", "") == "analog_from_button") {
        return ButtonToText(Common::ParamPackage{param.Get(dir, "")});
    }

    if (param.Get("engine", "") == "sdl") {
        if (dir == "modifier") {
            return QObject::tr("[unused]");
        }

        if (dir == "left" || dir == "right") {
            const QString axis_x_str = QString::fromStdString(param.Get("axis_x", ""));

            return QObject::tr("Axis %1").arg(axis_x_str);
        }

        if (dir == "up" || dir == "down") {
            const QString axis_y_str = QString::fromStdString(param.Get("axis_y", ""));

            return QObject::tr("Axis %1").arg(axis_y_str);
        }

        return {};
    }

    return QObject::tr("[unknown]");
}
} // namespace

ConfigureInputPlayer::ConfigureInputPlayer(QWidget* parent, std::size_t player_index,
                                           QWidget* bottom_row, bool debug)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureInputPlayer>()), player_index(player_index),
      debug(debug), timeout_timer(std::make_unique<QTimer>()),
      poll_timer(std::make_unique<QTimer>()), bottom_row(bottom_row) {
    ui->setupUi(this);

    setFocusPolicy(Qt::ClickFocus);

    button_map = {
        ui->buttonA,         ui->buttonB,         ui->buttonX,         ui->buttonY,
        ui->buttonLStick,    ui->buttonRStick,    ui->buttonL,         ui->buttonR,
        ui->buttonZL,        ui->buttonZR,        ui->buttonPlus,      ui->buttonMinus,
        ui->buttonDpadLeft,  ui->buttonDpadUp,    ui->buttonDpadRight, ui->buttonDpadDown,
        ui->buttonSL,        ui->buttonSR,        ui->buttonHome,      ui->buttonScreenshot,
        ui->buttonLStickMod, ui->buttonRStickMod,
    };

    analog_map_buttons = {{
        {
            ui->buttonLStickUp,
            ui->buttonLStickDown,
            ui->buttonLStickLeft,
            ui->buttonLStickRight,
        },
        {
            ui->buttonRStickUp,
            ui->buttonRStickDown,
            ui->buttonRStickLeft,
            ui->buttonRStickRight,
        },
    }};

    analog_map_modifier = {ui->buttonLStickMod, ui->buttonRStickMod};
    analog_map_deadzone = {ui->sliderLStickDeadzone, ui->sliderRStickDeadzone};
    analog_map_deadzone_label = {ui->labelLStickDeadzone, ui->labelRStickDeadzone};

    const auto ConfigureButtonClick = [&](QPushButton* button, Common::ParamPackage* param,
                                          int default_val) {
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::clicked, [=] {
            HandleClick(button,
                        [=](Common::ParamPackage params) {
                            // Workaround for ZL & ZR for analog triggers like on XBOX
                            // controllers. Analog triggers (from controllers like the XBOX
                            // controller) would not work due to a different range of their
                            // signals (from 0 to 255 on analog triggers instead of -32768 to
                            // 32768 on analog joysticks). The SDL driver misinterprets analog
                            // triggers as analog joysticks.
                            // TODO: reinterpret the signal range for analog triggers to map the
                            // values correctly. This is required for the correct emulation of
                            // the analog triggers of the GameCube controller.
                            if (params.Get("engine", "") == "analog_from_button") {
                                params.Set("direction", "+");
                                params.Set("threshold", "0.5");
                            }
                            (*param) = std::move(params);
                        },
                        InputCommon::Polling::DeviceType::Button);
        });
        connect(button, &QPushButton::customContextMenuRequested, [=](const QPoint& menu_location) {
            QMenu context_menu;
            context_menu.addAction(tr("Clear"), [&] {
                param->Clear();
                button->setText(tr("[not set]"));
            });
            context_menu.addAction(tr("Restore Default"), [&] {
                (*param) = Common::ParamPackage{InputCommon::GenerateKeyboardParam(default_val)};
                button->setText(ButtonToText(*param));
            });
            context_menu.exec(button->mapToGlobal(menu_location));
        });
    };

    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; button_id++) {
        auto* const button = button_map[button_id];
        if (button == nullptr) {
            continue;
        }
        ConfigureButtonClick(button_map[button_id], &buttons_param[button_id],
                             Config::default_buttons[button_id]);
    }
    // Handle clicks for the modifier buttons as well
    ConfigureButtonClick(ui->buttonLStickMod, &lstick_mod, Config::default_lstick_mod);
    ConfigureButtonClick(ui->buttonRStickMod, &rstick_mod, Config::default_rstick_mod);

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; sub_button_id++) {
            auto* const analog_button = analog_map_buttons[analog_id][sub_button_id];
            if (analog_button == nullptr) {
                continue;
            }

            analog_button->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(analog_button, &QPushButton::clicked, [=]() {
                HandleClick(analog_map_buttons[analog_id][sub_button_id],
                            [=](const Common::ParamPackage& params) {
                                SetAnalogParam(params, analogs_param[analog_id],
                                               analog_sub_buttons[sub_button_id]);
                            },
                            InputCommon::Polling::DeviceType::AnalogPreferred);
            });
            connect(analog_button, &QPushButton::customContextMenuRequested,
                    [=](const QPoint& menu_location) {
                        QMenu context_menu;
                        context_menu.addAction(tr("Clear"), [&] {
                            analogs_param[analog_id].Erase(analog_sub_buttons[sub_button_id]);
                            analog_map_buttons[analog_id][sub_button_id]->setText(tr("[not set]"));
                        });
                        context_menu.addAction(tr("Restore Default"), [&] {
                            Common::ParamPackage params{InputCommon::GenerateKeyboardParam(
                                Config::default_analogs[analog_id][sub_button_id])};
                            SetAnalogParam(params, analogs_param[analog_id],
                                           analog_sub_buttons[sub_button_id]);
                            analog_map_buttons[analog_id][sub_button_id]->setText(AnalogToText(
                                analogs_param[analog_id], analog_sub_buttons[sub_button_id]));
                        });
                        context_menu.exec(analog_map_buttons[analog_id][sub_button_id]->mapToGlobal(
                            menu_location));
                    });
        }
        connect(analog_map_modifier[analog_id], &QPushButton::clicked, [=]() {
            HandleClick(analog_map_modifier[analog_id],
                        [=](const Common::ParamPackage& params) {
                            SetAnalogParam(params, analogs_param[analog_id], "modifier");
                        },
                        InputCommon::Polling::DeviceType::AnalogPreferred);
        });
    }

    // Player Connected checkbox
    connect(ui->groupConnectedController, &QGroupBox::toggled,
            [&](bool checked) { emit Connected(checked); });

    // set up controller type. Only player 1 can choose handheld mode
    ui->comboControllerType->clear();
    QStringList controller_types = {
        QStringLiteral("Pro Controller"),
        QStringLiteral("Dual Joycons"),
        QStringLiteral("Right Joycon"),
        QStringLiteral("Left Joycon"),
    };
    if (player_index == 0) {
        controller_types.append(QStringLiteral("Handheld"));
        connect(ui->comboControllerType, qOverload<int>(&QComboBox::currentIndexChanged),
                [&](int index) {
                    emit HandheldStateChanged(GetControllerTypeFromIndex(index) ==
                                              Settings::ControllerType::HandheldJoyconAttached);
                });
    }
    ui->comboControllerType->addItems(controller_types);

    UpdateControllerIcon();
    UpdateControllerAvailableButtons();
    connect(ui->comboControllerType, qOverload<int>(&QComboBox::currentIndexChanged), [&](int) {
        UpdateControllerIcon();
        UpdateControllerAvailableButtons();
    });

    // TODO refresh input devices somehow?
    input_devices = InputCommon::GetInputDevices();
    ui->comboDevices->clear();
    for (auto device : input_devices) {
        ui->comboDevices->addItem(QString::fromStdString(device.Get("display", "Unknown")), {});
    }
    connect(ui->comboDevices, qOverload<int>(&QComboBox::currentIndexChanged),
            [&] { UpdateMappingWithDefaults(); });

    timeout_timer->setSingleShot(true);
    connect(timeout_timer.get(), &QTimer::timeout, [this] { SetPollingResult({}, true); });

    connect(poll_timer.get(), &QTimer::timeout, [this] {
        Common::ParamPackage params;
        for (auto& poller : device_pollers) {
            params = poller->GetNextInput();
            if (params.Has("engine")) {
                SetPollingResult(params, false);
                return;
            }
        }
    });

    controller_color_buttons = {
        ui->left_body_button,
        ui->left_buttons_button,
        ui->right_body_button,
        ui->right_buttons_button,
    };

    for (std::size_t i = 0; i < controller_color_buttons.size(); ++i) {
        connect(controller_color_buttons[i], &QPushButton::clicked, this,
                [this, i] { OnControllerButtonClick(static_cast<int>(i)); });
    }

    LoadConfiguration();

    // TODO(wwylele): enable this when we actually emulate it
    ui->buttonHome->setEnabled(false);
}

ConfigureInputPlayer::~ConfigureInputPlayer() = default;

void ConfigureInputPlayer::ApplyConfiguration() {
    auto& player = Settings::values.players[player_index];
    auto& buttons = debug ? Settings::values.debug_pad_buttons : player.buttons;
    auto& analogs = debug ? Settings::values.debug_pad_analogs : player.analogs;

    std::transform(buttons_param.begin(), buttons_param.end(), buttons.begin(),
                   [](const Common::ParamPackage& param) { return param.Serialize(); });
    std::transform(analogs_param.begin(), analogs_param.end(), analogs.begin(),
                   [](const Common::ParamPackage& param) { return param.Serialize(); });

    if (debug)
        return;

    std::array<u32, 4> colors{};
    std::transform(controller_colors.begin(), controller_colors.end(), colors.begin(),
                   [](QColor color) { return color.rgb(); });

    player.body_color_left = colors[0];
    player.button_color_left = colors[1];
    player.body_color_right = colors[2];
    player.button_color_right = colors[3];
    player.type = static_cast<Settings::ControllerType>(
        std::max(ui->comboControllerType->currentIndex() - 1, 0));
    player.connected = ui->groupConnectedController->isChecked();
}

void ConfigureInputPlayer::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureInputPlayer::RetranslateUI() {
    ui->retranslateUi(this);
    UpdateButtonLabels();
}

void ConfigureInputPlayer::OnControllerButtonClick(int i) {
    const QColor new_bg_color = QColorDialog::getColor(controller_colors[i]);
    if (!new_bg_color.isValid())
        return;
    controller_colors[i] = new_bg_color;
    controller_color_buttons[i]->setStyleSheet(
        QStringLiteral("background-color: %1; min-width: 55px;").arg(controller_colors[i].name()));
}

void ConfigureInputPlayer::LoadConfiguration() {
    auto& player = Settings::values.players[player_index];
    if (debug) {
        std::transform(Settings::values.debug_pad_buttons.begin(),
                       Settings::values.debug_pad_buttons.end(), buttons_param.begin(),
                       [](const std::string& str) { return Common::ParamPackage(str); });
        std::transform(Settings::values.debug_pad_analogs.begin(),
                       Settings::values.debug_pad_analogs.end(), analogs_param.begin(),
                       [](const std::string& str) { return Common::ParamPackage(str); });
    } else {
        std::transform(player.buttons.begin(), Settings::values.players[player_index].buttons.end(),
                       buttons_param.begin(),
                       [](const std::string& str) { return Common::ParamPackage(str); });
        std::transform(player.analogs.begin(), player.analogs.end(), analogs_param.begin(),
                       [](const std::string& str) { return Common::ParamPackage(str); });
    }

    UpdateButtonLabels();

    if (debug)
        return;

    std::array<u32, 4> colors = {
        player.body_color_left,
        player.button_color_left,
        player.body_color_right,
        player.button_color_right,
    };

    std::transform(colors.begin(), colors.end(), controller_colors.begin(),
                   [](u32 rgb) { return QColor::fromRgb(rgb); });

    for (std::size_t i = 0; i < colors.size(); ++i) {
        controller_color_buttons[i]->setStyleSheet(
            QStringLiteral("background-color: %1; min-width: 55px;")
                .arg(controller_colors[i].name()));
    }
    ui->groupConnectedController->setChecked(player.connected);
}

void ConfigureInputPlayer::RestoreDefaults() {
    // Reset button
    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; button_id++) {
        buttons_param[button_id] = Common::ParamPackage{
            InputCommon::GenerateKeyboardParam(Config::default_buttons[button_id])};
    }
    // Reset analog
    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; sub_button_id++) {
            Common::ParamPackage params{InputCommon::GenerateKeyboardParam(
                Config::default_analogs[analog_id][sub_button_id])};
            SetAnalogParam(params, analogs_param[analog_id], analog_sub_buttons[sub_button_id]);
        }
    }
    // TODO: Modifiers are not a native button or native analog, so reset them here
    // ui->buttonLStickMod
    UpdateButtonLabels();
    ui->comboControllerType->setCurrentIndex(0);
    ui->comboDevices->setCurrentIndex(0);
}

void ConfigureInputPlayer::ClearAll() {
    for (int button_id = 0; button_id < Settings::NativeButton::NumButtons; button_id++) {
        const auto* const button = button_map[button_id];
        if (button == nullptr || !button->isEnabled()) {
            continue;
        }

        buttons_param[button_id].Clear();
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; sub_button_id++) {
            const auto* const analog_button = analog_map_buttons[analog_id][sub_button_id];
            if (analog_button == nullptr || !analog_button->isEnabled()) {
                continue;
            }

            analogs_param[analog_id].Clear();
        }
    }

    UpdateButtonLabels();
}

void ConfigureInputPlayer::UpdateButtonLabels() {
    for (int button = 0; button < Settings::NativeButton::NumButtons; button++) {
        button_map[button]->setText(ButtonToText(buttons_param[button]));
    }

    for (int analog_id = 0; analog_id < Settings::NativeAnalog::NumAnalogs; analog_id++) {
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; sub_button_id++) {
            auto* const analog_button = analog_map_buttons[analog_id][sub_button_id];

            if (analog_button == nullptr) {
                continue;
            }

            analog_button->setText(
                AnalogToText(analogs_param[analog_id], analog_sub_buttons[sub_button_id]));
        }

        auto& param = analogs_param[analog_id];
        auto* const analog_deadzone_slider = analog_map_deadzone[analog_id];
        auto* const analog_deadzone_label = analog_map_deadzone_label[analog_id];
        const bool is_controller = param.Has("engine") && param.Get("engine", "") == "sdl";
        if (is_controller) {
            if (!param.Has("deadzone")) {
                param.Set("deadzone", 0.1f);
            }

            analog_deadzone_slider->setValue(static_cast<int>(param.Get("deadzone", 0.1f) * 100));
        }
        analog_deadzone_slider->setVisible(is_controller);
        analog_deadzone_label->setVisible(is_controller);
    }
}

void ConfigureInputPlayer::UpdateMappingWithDefaults() {
    if (ui->comboDevices->currentIndex() < 2) {
        return;
    }
    const auto& device = input_devices[ui->comboDevices->currentIndex()];
    auto button_mapping = InputCommon::GetButtonMappingForDevice(device);
    auto analog_mapping = InputCommon::GetAnalogMappingForDevice(device);
    for (int i = 0; i < buttons_param.size(); ++i) {
        buttons_param[i] = button_mapping[static_cast<Settings::NativeButton::Values>(i)];
    }
    for (int i = 0; i < analogs_param.size(); ++i) {
        analogs_param[i] = button_mapping[static_cast<Settings::NativeButton::Values>(i)];
    }
    UpdateButtonLabels();
}

void ConfigureInputPlayer::HandleClick(
    QPushButton* button, std::function<void(const Common::ParamPackage&)> new_input_setter,
    InputCommon::Polling::DeviceType type) {
    button->setText(tr("[waiting]"));
    button->setFocus();

    // The first two input devices are always Any and Keyboard. If the user filtered to a
    // controller, then they don't want keyboard input
    want_keyboard_keys = ui->comboDevices->currentIndex() < 2;

    input_setter = new_input_setter;

    device_pollers = InputCommon::Polling::GetPollers(type);

    for (auto& poller : device_pollers) {
        poller->Start();
    }

    grabKeyboard();
    grabMouse();
    timeout_timer->start(5000); // Cancel after 5 seconds
    poll_timer->start(200);     // Check for new inputs every 200ms
}

void ConfigureInputPlayer::SetPollingResult(const Common::ParamPackage& params, bool abort) {
    releaseKeyboard();
    releaseMouse();
    timeout_timer->stop();
    poll_timer->stop();
    for (auto& poller : device_pollers) {
        poller->Stop();
    }

    if (!abort) {
        (*input_setter)(params);
    }

    UpdateButtonLabels();
    input_setter = std::nullopt;
}

void ConfigureInputPlayer::keyPressEvent(QKeyEvent* event) {
    if (!input_setter || !event) {
        return;
    }

    if (event->key() != Qt::Key_Escape) {
        if (want_keyboard_keys) {
            SetPollingResult(Common::ParamPackage{InputCommon::GenerateKeyboardParam(event->key())},
                             false);
        } else {
            // Escape key wasn't pressed and we don't want any keyboard keys, so don't stop polling
            return;
        }
    }
    SetPollingResult({}, true);
}

void ConfigureInputPlayer::UpdateControllerIcon() {
    // We aren't using Qt's built in theme support here since we aren't drawing an icon (and its
    // "nonstandard" to use an image through the icon support)
    QString stylesheet{};
    switch (GetControllerTypeFromIndex(ui->comboControllerType->currentIndex())) {
    case Settings::ControllerType::ProController:
        stylesheet = QStringLiteral("image: url(:/controller/pro_controller%0)");
        break;
    case Settings::ControllerType::DualJoyconDetached:
        stylesheet = QStringLiteral("image: url(:/controller/dual_joycon%0)");
        break;
    case Settings::ControllerType::HandheldJoyconAttached:
        stylesheet = QStringLiteral("image: url(:/controller/handheld%0)");
        break;
    case Settings::ControllerType::LeftJoycon:
        stylesheet = QStringLiteral("image: url(:/controller/single_joycon_left%0)");
        break;
    case Settings::ControllerType::RightJoycon:
        stylesheet = QStringLiteral("image: url(:/controller/single_joycon_right%0)");
        break;
    default:
        break;
    }
    const QString theme = QIcon::themeName().contains(QStringLiteral("dark"))
                              ? QStringLiteral("_dark")
                              : QStringLiteral("");
    ui->controllerFrame->setStyleSheet(stylesheet.arg(theme));
}

void ConfigureInputPlayer::UpdateControllerAvailableButtons() {
    auto layout = GetControllerTypeFromIndex(ui->comboControllerType->currentIndex());
    if (debug)
        layout = Settings::ControllerType::DualJoyconDetached;

    // List of all the widgets that will be hidden by any of the following layouts that need
    // "unhidden" after the controller type changes
    const std::vector<QWidget*> layout_show = {
        ui->buttonMiscButtonsLeftJoycon,
        ui->buttonMiscButtonsRightJoycon,
        ui->buttonShoulderButtonsSLSR,
        ui->buttonShoulderButtonsRight,
        ui->buttonMiscButtonsPlusHome,
        ui->RStick,
        ui->faceButtons,
        ui->buttonShoulderButtonsLeft,
        ui->buttonMiscButtonsMinusScreenshot,
        ui->LStick,
        ui->Dpad,
        ui->buttonLStickModGroup,
        ui->groupRStickPressed,
        ui->buttonMiscButtonsHomeGroup,
        ui->buttonMiscButtonsScreenshotGroup,
    };

    for (auto* widget : layout_show) {
        widget->show();
    }

    std::vector<QWidget*> layout_hidden;
    switch (layout) {
    case Settings::ControllerType::ProController:
        layout_hidden = {
            ui->buttonMiscButtonsLeftJoycon,
            ui->buttonMiscButtonsRightJoycon,
            ui->buttonShoulderButtonsSLSR,
        };
        break;
    case Settings::ControllerType::DualJoyconDetached:
    case Settings::ControllerType::HandheldJoyconAttached:
        layout_hidden = {
            ui->buttonShoulderButtonsSLSR,
        };
        break;
    case Settings::ControllerType::LeftJoycon:
        layout_hidden = {
            ui->buttonMiscButtonsRightJoycon,
            ui->buttonShoulderButtonsRight,
            ui->buttonMiscButtonsPlusHome,
            ui->RStick,
            ui->faceButtons,
        };
        break;
    case Settings::ControllerType::RightJoycon:
        layout_hidden = {
            ui->buttonMiscButtonsLeftJoycon,
            ui->buttonShoulderButtonsLeft,
            ui->buttonMiscButtonsMinusScreenshot,
            ui->LStick,
            ui->Dpad,
        };
        break;
    }

    if (debug) {
        const std::vector<QWidget*> debug_hidden = {
            ui->buttonShoulderButtonsSLSR,
            ui->buttonLStickModGroup,
            ui->groupRStickPressed,
            ui->buttonMiscButtonsHomeGroup,
            ui->buttonMiscButtonsScreenshotGroup,
        };
        layout_hidden.insert(layout_hidden.end(), debug_hidden.begin(), debug_hidden.end());
    }

    for (auto* widget : layout_hidden) {
        widget->hide();
    }
}

void ConfigureInputPlayer::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    ui->main->addWidget(bottom_row);
}

void ConfigureInputPlayer::ConnectPlayer(bool connected) {
    ui->groupConnectedController->setChecked(connected);
}
