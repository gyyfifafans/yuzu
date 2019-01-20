// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <unordered_map>
#include <QBuffer>
#include <QByteArray>
#include <QHBoxLayout>
#include <QIODevice>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProgressBar>
#include <QStyleOption>
#include <QTime>
#include <QtConcurrent/QtConcurrentRun>
#include "common/logging/log.h"
#include "core/loader/loader.h"
#include "ui_loading_screen.h"
#include "video_core/rasterizer_interface.h"
#include "yuzu/loading_screen.h"

// Mingw seems to not have QMovie at all. If QMovie is missing then use a single frame instead of an
// showing the full animation
#if !YUZU_QT_MOVIE_MISSING
#include <QMovie>
#endif

/// Static maps defining the differences in text and styling for each stage
static std::unordered_map<VideoCore::LoadCallbackStage, const char*> stage_translations = {
    {VideoCore::LoadCallbackStage::Prepare, QT_TRANSLATE_NOOP("LoadingScreen", "Loading...")},
    {VideoCore::LoadCallbackStage::Raw,
     QT_TRANSLATE_NOOP("LoadingScreen", "Preparing Shaders %1 / %2")},
    {VideoCore::LoadCallbackStage::Binary,
     QT_TRANSLATE_NOOP("LoadingScreen", "Loading Shaders %1 / %2")},
    {VideoCore::LoadCallbackStage::Complete, QT_TRANSLATE_NOOP("LoadingScreen", "Launching...")},
};

static std::unordered_map<VideoCore::LoadCallbackStage, const char*> progressbar_style = {
    {VideoCore::LoadCallbackStage::Prepare,
     R"(
QProgressBar {
background-color: black;
border: 2px solid black;
border-radius: 4px;
padding: 2px;
}
QProgressBar::chunk {
background-color: white;
})"},
    {VideoCore::LoadCallbackStage::Raw,
     R"(
QProgressBar {
background-color: black;
border: 2px solid white;
border-radius: 4px;
padding: 2px;
}
QProgressBar::chunk {
background-color: #0ab9e6;
})"},
    {VideoCore::LoadCallbackStage::Binary,
     R"(
QProgressBar {
background-color: black;
border: 2px solid white;
border-radius: 4px;
padding: 2px;
}
QProgressBar::chunk {
background-color: #ff3c28;
})"},
    {VideoCore::LoadCallbackStage::Complete,
     R"(
QProgressBar {
background-color: black;
border: 2px solid white;
border-radius: 4px;
padding: 2px;
}
QProgressBar::chunk {
background-color: #ff3c28;
})"},
};

LoadingScreen::LoadingScreen(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::LoadingScreen>()),
      previous_stage(VideoCore::LoadCallbackStage::Complete) {
    ui->setupUi(this);

    connect(this, &LoadingScreen::LoadProgress, this, &LoadingScreen::OnLoadProgress,
            Qt::QueuedConnection);
    qRegisterMetaType<VideoCore::LoadCallbackStage>();
}

LoadingScreen::~LoadingScreen() = default;

void LoadingScreen::Prepare(Loader::AppLoader& loader) {
    std::vector<u8> buffer;
    if (loader.ReadBanner(buffer) == Loader::ResultStatus::Success) {
#ifdef YUZU_QT_MOVIE_MISSING
        QPixmap map;
        map.loadFromData(buffer.data(), buffer.size());
        ui->banner->setPixmap(map);
#else
        backing_mem =
            std::make_unique<QByteArray>(reinterpret_cast<char*>(buffer.data()), buffer.size());
        backing_buf = std::make_unique<QBuffer>(backing_mem.get());
        backing_buf->open(QIODevice::ReadOnly);
        animation = std::make_unique<QMovie>(backing_buf.get(), QByteArray());
        animation->start();
        ui->banner->setMovie(animation.get());
#endif
        buffer.clear();
    }
    if (loader.ReadLogo(buffer) == Loader::ResultStatus::Success) {
        QPixmap map;
        map.loadFromData(buffer.data(), buffer.size());
        ui->logo->setPixmap(map);
    }

    OnLoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 100);
    // testing fake shader loading
    QtConcurrent::run([this] {
        QThread::msleep(500);

        // test fast shader loading
        for (int i = 0; i < 1500; i++) {
            emit LoadProgress(VideoCore::LoadCallbackStage::Raw, i, 1500);
            QThread::msleep(1);
        }
        int total = 300;
        for (int i = 0; i < 270; i++) {
            emit LoadProgress(VideoCore::LoadCallbackStage::Binary, i, total);
            QThread::msleep(1);
        }
        // test stage slow down when it reaches shaders that aren't compiled
        for (int i = 270; i < 300; i++) {
            emit LoadProgress(VideoCore::LoadCallbackStage::Binary, i, total);
            QThread::msleep(rand() % 500 + 50);
        }
        emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 100, 100);
    });
}

void LoadingScreen::OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value,
                                   std::size_t total) {
    using namespace std::chrono;
    auto now = high_resolution_clock::now();
    // reset the timer if the stage changes
    if (stage != previous_stage) {
        ui->progress_bar->setStyleSheet(progressbar_style[stage]);
        previous_stage = stage;
        // reset back to fast shader compiling since the stage changed
        slow_shader_compile_start = false;
    }
    // update the max of the progress bar if the number of shaders change
    if (total != previous_total) {
        ui->progress_bar->setMaximum(total);
        previous_total = total;
    }

    QString estimate;
    // If theres a drastic slowdown in the rate, then display an estimate
    if (now - previous_time > milliseconds{20}) {
        if (!slow_shader_compile_start) {
            slow_shader_start = high_resolution_clock::now();
            slow_shader_compile_start = true;
            slow_shader_first_value = value;
        }
        // only calculate an estimate time after a second has passed since stage change
        auto diff = duration_cast<milliseconds>(now - slow_shader_start);
        if (diff > seconds{1}) {
            auto eta_mseconds =
                static_cast<long>(static_cast<double>(total - slow_shader_first_value) /
                                  (value - slow_shader_first_value) * diff.count());
            estimate =
                tr("Estimated Time %1")
                    .arg(QTime(0, 0, 0, 0)
                             .addMSecs(std::max<long>(eta_mseconds - diff.count() + 1000, 1000))
                             .toString("mm:ss"));
        }
    }

    // update labels and progress bar
    ui->stage->setText(tr(stage_translations[stage]).arg(value).arg(total));
    ui->value->setText(estimate);
    ui->progress_bar->setValue(value);
    previous_time = now;
}

void LoadingScreen::paintEvent(QPaintEvent* event) {
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
    QWidget::paintEvent(event);
}

void LoadingScreen::Clear() {
#ifndef YUZU_QT_MOVIE_MISSING
    animation.reset();
    backing_buf.reset();
    backing_mem.reset();
#endif
}
