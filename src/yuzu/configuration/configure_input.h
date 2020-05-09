﻿// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>

#include <QDialog>
#include <QKeyEvent>

#include "yuzu/configuration/configure_input_player.h"

#include "ui_configure_input.h"

class QString;
class QTimer;
class QCheckBox;

namespace Ui {
class ConfigureInput;
}

void OnDockedModeChanged(bool last_state, bool new_state);

class ConfigureInput : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureInput(QWidget* parent = nullptr);
    ~ConfigureInput() override;

    /// Save all button configurations to settings file
    void ApplyConfiguration();

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void ClearAll();

    void UpdateUIEnabled();

    void UpdateDockedState(bool is_handheld);

    /// Load configuration settings.
    void LoadConfiguration();
    void LoadPlayerControllerIndices();

    /// Restore all buttons to their default values.
    void RestoreDefaults();

    std::unique_ptr<Ui::ConfigureInput> ui;

    std::array<ConfigureInputPlayer*, 8> player_controller;
    std::array<QWidget*, 8> player_tabs;
    std::array<QCheckBox*, 8> player_connected;
};
