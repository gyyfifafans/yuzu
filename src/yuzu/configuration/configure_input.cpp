// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <memory>

#include <QSignalBlocker>
#include <QTimer>

#include "configuration/configure_touchscreen_advanced.h"
#include "core/core.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applet_ae.h"
#include "core/hle/service/am/applet_oe.h"
#include "core/hle/service/hid/controllers/npad.h"
#include "core/hle/service/sm/sm.h"
#include "ui_configure_input.h"
#include "ui_configure_input_player.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/configuration/configure_mouse_advanced.h"

void OnDockedModeChanged(bool last_state, bool new_state) {
    if (last_state == new_state) {
        return;
    }

    Core::System& system{Core::System::GetInstance()};
    if (!system.IsPoweredOn()) {
        return;
    }
    Service::SM::ServiceManager& sm = system.ServiceManager();

    // Message queue is shared between these services, we just need to signal an operation
    // change to one and it will handle both automatically
    auto applet_oe = sm.GetService<Service::AM::AppletOE>("appletOE");
    auto applet_ae = sm.GetService<Service::AM::AppletAE>("appletAE");
    bool has_signalled = false;

    if (applet_oe != nullptr) {
        applet_oe->GetMessageQueue()->OperationModeChanged();
        has_signalled = true;
    }

    if (applet_ae != nullptr && !has_signalled) {
        applet_ae->GetMessageQueue()->OperationModeChanged();
    }
}

namespace {
template <typename Dialog, typename... Args>
void CallConfigureDialog(ConfigureInput& parent, Args&&... args) {
    parent.ApplyConfiguration();
    Dialog dialog(&parent, std::forward<Args>(args)...);

    const auto res = dialog.exec();
    if (res == QDialog::Accepted) {
        dialog.ApplyConfiguration();
    }
}
} // Anonymous namespace

ConfigureInput::ConfigureInput(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureInput>()) {
    ui->setupUi(this);

    player_controller = {
        new ConfigureInputPlayer(this, 0, ui->consoleInputSettings),
        new ConfigureInputPlayer(this, 1, ui->consoleInputSettings),
        new ConfigureInputPlayer(this, 2, ui->consoleInputSettings),
        new ConfigureInputPlayer(this, 3, ui->consoleInputSettings),
        new ConfigureInputPlayer(this, 4, ui->consoleInputSettings),
        new ConfigureInputPlayer(this, 5, ui->consoleInputSettings),
        new ConfigureInputPlayer(this, 6, ui->consoleInputSettings),
        new ConfigureInputPlayer(this, 7, ui->consoleInputSettings),
    };

    player_tabs = {
        ui->tabPlayer1, ui->tabPlayer2, ui->tabPlayer3, ui->tabPlayer4,
        ui->tabPlayer5, ui->tabPlayer6, ui->tabPlayer7, ui->tabPlayer8,
    };

    player_connected = {
        ui->checkboxPlayer1Connected, ui->checkboxPlayer2Connected, ui->checkboxPlayer3Connected,
        ui->checkboxPlayer4Connected, ui->checkboxPlayer5Connected, ui->checkboxPlayer6Connected,
        ui->checkboxPlayer7Connected, ui->checkboxPlayer8Connected,
    };

    for (int i = 0; i < player_tabs.size(); ++i) {
        player_tabs[i]->setLayout(new QHBoxLayout(player_tabs[i]));
        player_tabs[i]->layout()->addWidget(player_controller[i]);
        connect(player_controller[i], &ConfigureInputPlayer::Connected,
                [&, i](bool is_connected) { player_connected[i]->setChecked(is_connected); });
        connect(player_connected[i], &QCheckBox::stateChanged,
                [&, i](int state) { player_controller[i]->ConnectPlayer(state == Qt::Checked); });
    }
    // Only the first player can choose handheld mode so connect the signal just to player 1
    connect(player_controller[0], &ConfigureInputPlayer::HandheldStateChanged,
            [&](bool is_handheld) { UpdateDockedState(is_handheld); });

    RetranslateUI();
    UpdateDockedState(Settings::values.players[0].type ==
                      Settings::ControllerType::HandheldJoyconAttached);
    LoadConfiguration();
    UpdateUIEnabled();

    // for (auto* enabled : players_controller) {
    //    connect(enabled, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
    //            &ConfigureInput::UpdateUIEnabled);
    //}
    // connect(ui->use_docked_mode, &QCheckBox::stateChanged, this,
    // &ConfigureInput::UpdateUIEnabled); connect(ui->handheld_connected, &QCheckBox::stateChanged,
    // this,
    //        &ConfigureInput::UpdateUIEnabled);
    // connect(ui->mouse_enabled, &QCheckBox::stateChanged, this, &ConfigureInput::UpdateUIEnabled);
    // connect(ui->keyboard_enabled, &QCheckBox::stateChanged, this,
    // &ConfigureInput::UpdateUIEnabled); connect(ui->debug_enabled, &QCheckBox::stateChanged, this,
    // &ConfigureInput::UpdateUIEnabled); connect(ui->touchscreen_enabled, &QCheckBox::stateChanged,
    // this,
    //        &ConfigureInput::UpdateUIEnabled);

    // for (std::size_t i = 0; i < players_configure.size(); ++i) {
    //    connect(players_configure[i], &QPushButton::clicked, this,
    //            [this, i] { CallConfigureDialog<ConfigureInputPlayer>(*this, i, false); });
    //}

    // connect(ui->handheld_configure, &QPushButton::clicked, this,
    //        [this] { CallConfigureDialog<ConfigureInputPlayer>(*this, 8, false); });

    // connect(ui->debug_configure, &QPushButton::clicked, this,
    //        [this] { CallConfigureDialog<ConfigureInputPlayer>(*this, 9, true); });

    // connect(ui->mouse_advanced, &QPushButton::clicked, this,
    //        [this] { CallConfigureDialog<ConfigureMouseAdvanced>(*this); });

    // connect(ui->touchscreen_advanced, &QPushButton::clicked, this,
    //        [this] { CallConfigureDialog<ConfigureTouchscreenAdvanced>(*this); });

    connect(ui->buttonClearAll, &QPushButton::clicked, [this] { ClearAll(); });
    connect(ui->buttonRestoreDefaults, &QPushButton::clicked, [this] { RestoreDefaults(); });
}

ConfigureInput::~ConfigureInput() = default;

QList<QWidget*> ConfigureInput::GetSubTabs() const {
    return {
        ui->tabPlayer1, ui->tabPlayer2, ui->tabPlayer3, ui->tabPlayer4,  ui->tabPlayer5,
        ui->tabPlayer6, ui->tabPlayer7, ui->tabPlayer8, ui->tabAdvanced,
    };
}

void ConfigureInput::ApplyConfiguration() {
    for (auto controller : player_controller) {
        controller->ApplyConfiguration();
    }

    const bool pre_docked_mode = Settings::values.use_docked_mode;
    Settings::values.use_docked_mode = ui->radioDocked->isChecked();
    OnDockedModeChanged(pre_docked_mode, Settings::values.use_docked_mode);
    // Settings::values
    //    .players[Service::HID::Controller_NPad::NPadIdToIndex(Service::HID::NPAD_HANDHELD)]
    //    .connected = ui->handheld_connected->isChecked();
    // Settings::values.debug_pad_enabled = ui->debug_enabled->isChecked();
    // Settings::values.mouse_enabled = ui->mouse_enabled->isChecked();
    // Settings::values.keyboard_enabled = ui->keyboard_enabled->isChecked();
    // Settings::values.touchscreen.enabled = ui->touchscreen_enabled->isChecked();
}

void ConfigureInput::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureInput::RetranslateUI() {
    ui->retranslateUi(this);
    // RetranslateControllerComboBoxes();
}

void ConfigureInput::UpdateUIEnabled() {
    // bool hit_disabled = false;
    // for (auto* player : player_controller) {
    //    player->setDisabled(hit_disabled);
    //    if (hit_disabled) {
    //        player->setCurrentIndex(0);
    //    }
    //    if (!hit_disabled && player->currentIndex() == 0) {
    //        hit_disabled = true;
    //    }
    //}

    // for (std::size_t i = 0; i < players_controller.size(); ++i) {
    //    players_configure[i]->setEnabled(players_controller[i]->currentIndex() != 0);
    //}

    // ui->handheld_connected->setChecked(ui->handheld_connected->isChecked() &&
    //                                   !ui->use_docked_mode->isChecked());
    // ui->handheld_connected->setEnabled(!ui->use_docked_mode->isChecked());
    // ui->handheld_configure->setEnabled(ui->handheld_connected->isChecked() &&
    //                                   !ui->use_docked_mode->isChecked());
    // ui->mouse_advanced->setEnabled(ui->mouse_enabled->isChecked());
    // ui->debug_configure->setEnabled(ui->debug_enabled->isChecked());
    // ui->touchscreen_advanced->setEnabled(ui->touchscreen_enabled->isChecked());
}

void ConfigureInput::LoadConfiguration() {
    std::stable_partition(
        Settings::values.players.begin(),
        Settings::values.players.begin() +
            Service::HID::Controller_NPad::NPadIdToIndex(Service::HID::NPAD_HANDHELD),
        [](const auto& player) { return player.connected; });

    LoadPlayerControllerIndices();

    // ui->use_docked_mode->setChecked(Settings::values.use_docked_mode);
    // ui->handheld_connected->setChecked(
    //    Settings::values
    //        .players[Service::HID::Controller_NPad::NPadIdToIndex(Service::HID::NPAD_HANDHELD)]
    //        .connected);
    // ui->debug_enabled->setChecked(Settings::values.debug_pad_enabled);
    // ui->mouse_enabled->setChecked(Settings::values.mouse_enabled);
    // ui->keyboard_enabled->setChecked(Settings::values.keyboard_enabled);
    // ui->touchscreen_enabled->setChecked(Settings::values.touchscreen.enabled);

    UpdateUIEnabled();
}

void ConfigureInput::LoadPlayerControllerIndices() {
    for (std::size_t i = 0; i < player_connected.size(); ++i) {
        const auto connected = Settings::values.players[i].connected;
        player_connected[i]->setChecked(connected);
    }
}

void ConfigureInput::ClearAll() {
    // We don't have a good way to know what tab is active, but we can find out by getting the
    // parent of the consoleInputSettings
    auto player_tab = static_cast<ConfigureInputPlayer*>(ui->consoleInputSettings->parent());
    player_tab->ClearAll();
}

void ConfigureInput::RestoreDefaults() {
    // players_controller[0]->setCurrentIndex(2);

    // for (std::size_t i = 1; i < players_controller.size(); ++i) {
    //    players_controller[i]->setCurrentIndex(0);
    //}

    // ui->use_docked_mode->setCheckState(Qt::Unchecked);
    // ui->handheld_connected->setCheckState(Qt::Unchecked);
    // ui->mouse_enabled->setCheckState(Qt::Unchecked);
    // ui->keyboard_enabled->setCheckState(Qt::Unchecked);
    // ui->debug_enabled->setCheckState(Qt::Unchecked);
    // ui->touchscreen_enabled->setCheckState(Qt::Checked);

    // We don't have a good way to know what tab is active, but we can find out by getting the
    // parent of the consoleInputSettings
    auto player_tab = static_cast<ConfigureInputPlayer*>(ui->consoleInputSettings->parent());
    player_tab->RestoreDefaults();

    ui->radioDocked->setChecked(true);
    ui->radioUndocked->setChecked(false);
    UpdateUIEnabled();
}

void ConfigureInput::UpdateDockedState(bool is_handheld) {
    // If the controller type is handheld only, disallow changing docked mode
    ui->radioDocked->setEnabled(!is_handheld);
    ui->radioUndocked->setEnabled(!is_handheld);

    ui->radioDocked->setChecked(Settings::values.use_docked_mode);
    ui->radioUndocked->setChecked(!Settings::values.use_docked_mode);

    // If its handheld only, force docked mode off (since you can't play handheld in a dock)
    if (is_handheld) {
        ui->radioUndocked->setChecked(true);
    }
}
