// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QBuffer>
#include <QByteArray>
#include <QHBoxLayout>
#include <QIODevice>
#include <QImage>
#include <QLabel>
#include <QMovie>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProgressBar>
#include <QStyleOption>
#include <QWindow>
#include "common/logging/log.h"
#include "core/loader/loader.h"
#include "ui_loading_screen.h"
#include "yuzu/loading_screen.h"

LoadingScreen::LoadingScreen(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::LoadingScreen>()) {
    ui->setupUi(this);
    // Progress bar is hidden until we have a use for it.
    ui->progress_bar->hide();
}

void LoadingScreen::Prepare(Loader::AppLoader& loader) {
    std::vector<u8> buffer;
    if (loader.ReadBanner(buffer) == Loader::ResultStatus::Success) {
        backing_mem =
            std::make_unique<QByteArray>(reinterpret_cast<char*>(buffer.data()), buffer.size());
        backing_buf = std::make_unique<QBuffer>(backing_mem.get());
        backing_buf->open(QIODevice::ReadOnly);
        animation = std::make_unique<QMovie>(backing_buf.get(), QByteArray("GIF"));
        animation->start();
        ui->banner->setMovie(animation.get());
        buffer.clear();
    }
    if (loader.ReadLogo(buffer) == Loader::ResultStatus::Success) {
        QPixmap map;
        map.loadFromData(buffer.data(), buffer.size());
        ui->logo->setPixmap(map);
    }
}

void LoadingScreen::OnLoadProgress(std::size_t value, std::size_t total) {
    if (total != previous_total) {
        ui->progress_bar->setMaximum(total);
        previous_total = total;
    }
    ui->progress_bar->setValue(value);
}

void LoadingScreen::paintEvent(QPaintEvent* event) {
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
    QWidget::paintEvent(event);
}

void LoadingScreen::Clear() {
    animation.reset();
    backing_buf.reset();
    backing_mem.reset();
}
