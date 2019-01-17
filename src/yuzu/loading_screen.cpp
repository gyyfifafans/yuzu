// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QBuffer>
#include <QByteArray>
#include <QIODevice>
#include <QImage>
#include <QLabel>
#include <QMovie>
#include <QPalette>
#include <QPixmap>
#include <QVBoxLayout>
#include "common/logging/log.h"
#include "core/loader/loader.h"
#include "yuzu/loading_screen.h"

LoadingScreen::LoadingScreen(QWidget* parent) : QWidget(parent) {
    banner = new QLabel(this);
    logo = new QLabel(this);
    QPalette pal;
    pal.setColor(QPalette::Background, Qt::black);
    setAutoFillBackground(true);
    setPalette(pal);
}

void LoadingScreen::Prepare(Loader::AppLoader& loader) {
    if (layout()) {
        delete layout();
    }
    if (animation) {
        delete animation;
    }
    std::vector<u8> buffer;
    loader.ReadBanner(buffer);
    backing_mem =
        std::make_unique<QByteArray>(reinterpret_cast<char*>(buffer.data()), buffer.size());
    backing_buf = std::make_unique<QBuffer>(backing_mem.get());
    backing_buf->open(QIODevice::ReadOnly);
    animation = new QMovie(backing_buf.get(), QByteArray(), this);
    // todo check validity
    banner->setMovie(animation);
    animation->start();
    buffer.clear();

    loader.ReadLogo(buffer);
    QPixmap map;
    map.loadFromData(buffer.data(), buffer.size());
    logo->setPixmap(map);

    QBoxLayout* layout = new QVBoxLayout(this);
    resize(1000, 500);
    layout->addWidget(banner);
    layout->addWidget(loading);
    layout->addWidget(logo);
    layout->setMargin(0);

    setLayout(layout);
}

void LoadingScreen::OnLoadProgress(std::size_t value, std::size_t total) {
    LOG_INFO(Frontend, "Progress {} / {}", value, total);
}
