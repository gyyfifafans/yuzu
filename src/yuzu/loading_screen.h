// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <QWidget>

namespace Loader {
class AppLoader;
}

class QLabel;
class QMovie;
class QByteArray;
class QBuffer;
class QProgressBar;

class LoadingWidget : public QWidget {
    Q_OBJECT
public:
    explicit LoadingWidget(QWidget* parent = nullptr);

private:
    QProgressBar* progress_bar;
};

class LoadingScreen : public QWidget {
    Q_OBJECT

public:
    explicit LoadingScreen(QWidget* parent = nullptr);
    void Prepare(Loader::AppLoader& loader);

public slots:
    void OnLoadProgress(std::size_t value, std::size_t total);

private:
    QLabel* banner = nullptr;
    QLabel* logo = nullptr;
    LoadingWidget* loading = nullptr;
    QMovie* animation = nullptr;
    std::unique_ptr<QBuffer> backing_buf;
    std::unique_ptr<QByteArray> backing_mem;
};
