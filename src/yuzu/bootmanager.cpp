// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <glad/glad.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMessageBox>
#ifndef __APPLE__
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLWindow>
#endif
#include <QPainter>
#include <QScreen>
#include <QStringList>
#include <QWindow>

#ifndef WIN32
#include <qpa/qplatformnativeinterface.h>
#endif

#include <fmt/format.h>

#include "common/assert.h"
#include "common/microprofile.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/scope_acquire_context.h"
#include "core/settings.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/motion_emu.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"
#include "yuzu/bootmanager.h"
#include "yuzu/main.h"

EmuThread::EmuThread(GRenderWindow& window)
    : shared_context{window.CreateSharedContext()},
      context{(Settings::values.use_asynchronous_gpu_emulation && shared_context) ? *shared_context
                                                                                  : window} {}

EmuThread::~EmuThread() = default;

static GMainWindow* GetMainWindow() {
    for (QWidget* w : qApp->topLevelWidgets()) {
        if (GMainWindow* main = qobject_cast<GMainWindow*>(w)) {
            return main;
        }
    }
    return nullptr;
}

void EmuThread::run() {
    MicroProfileOnThreadCreate("EmuThread");

    Core::Frontend::ScopeAcquireContext acquire_context{context};

    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);

    Core::System::GetInstance().Renderer().Rasterizer().LoadDiskResources(
        stop_run, [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
            emit LoadProgress(stage, value, total);
        });

    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);

    // Holds whether the cpu was running during the last iteration,
    // so that the DebugModeLeft signal can be emitted before the
    // next execution step
    bool was_active = false;
    while (!stop_run) {
        if (running) {
            if (!was_active)
                emit DebugModeLeft();

            Core::System::ResultStatus result = Core::System::GetInstance().RunLoop();
            if (result != Core::System::ResultStatus::Success) {
                this->SetRunning(false);
                emit ErrorThrown(result, Core::System::GetInstance().GetStatusDetails());
            }

            was_active = running || exec_step;
            if (!was_active && !stop_run)
                emit DebugModeEntered();
        } else if (exec_step) {
            if (!was_active)
                emit DebugModeLeft();

            exec_step = false;
            Core::System::GetInstance().SingleStep();
            emit DebugModeEntered();
            yieldCurrentThread();

            was_active = false;
        } else {
            std::unique_lock lock{running_mutex};
            running_cv.wait(lock, [this] { return IsRunning() || exec_step || stop_run; });
        }
    }

    // Shutdown the core emulation
    Core::System::GetInstance().Shutdown();

#if MICROPROFILE_ENABLED
    MicroProfileOnThreadExit();
#endif
}

#ifndef __APPLE__
class OpenGLContext : public Core::Frontend::GraphicsContext {
public:
    explicit OpenGLContext(QOpenGLContext* shared_context)
        : context(new QOpenGLContext(shared_context->parent())),
          surface(new QOffscreenSurface(nullptr)) {

        // disable vsync for any shared contexts
        auto format = shared_context->format();
        format.setSwapInterval(0);

        context->setShareContext(shared_context);
        context->setFormat(format);
        context->create();
        surface->setParent(shared_context->parent());
        surface->setFormat(format);
        surface->create();
    }

    void MakeCurrent() override {
        context->makeCurrent(surface);
    }

    void DoneCurrent() override {
        context->doneCurrent();
    }

private:
    QOpenGLContext* context;
    QOffscreenSurface* surface;
};
#endif

class RenderWidget : public QWidget {
public:
    RenderWidget(GRenderWindow* parent) : QWidget(parent), parent(parent) {
        setAttribute(Qt::WA_NativeWindow);
        SetFillBackground(true);
    }

    virtual ~RenderWidget() = default;

    void resizeEvent(QResizeEvent* ev) override {
        parent->resize(ev->size());
        parent->OnFramebufferSizeChanged();
    }

    void keyPressEvent(QKeyEvent* event) override {
        InputCommon::GetKeyboard()->PressKey(event->key());
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        InputCommon::GetKeyboard()->ReleaseKey(event->key());
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->source() == Qt::MouseEventSynthesizedBySystem)
            return; // touch input is handled in TouchBeginEvent

        const auto pos{event->pos()};
        if (event->button() == Qt::LeftButton) {
            const auto [x, y] = parent->ScaleTouch(pos);
            parent->TouchPressed(x, y);
        } else if (event->button() == Qt::RightButton) {
            InputCommon::GetMotionEmu()->BeginTilt(pos.x(), pos.y());
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->source() == Qt::MouseEventSynthesizedBySystem)
            return; // touch input is handled in TouchUpdateEvent

        const auto pos{event->pos()};
        const auto [x, y] = parent->ScaleTouch(pos);
        parent->TouchMoved(x, y);
        InputCommon::GetMotionEmu()->Tilt(pos.x(), pos.y());
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->source() == Qt::MouseEventSynthesizedBySystem)
            return; // touch input is handled in TouchEndEvent

        if (event->button() == Qt::LeftButton)
            parent->TouchReleased();
        else if (event->button() == Qt::RightButton)
            InputCommon::GetMotionEmu()->EndTilt();
    }

    std::pair<unsigned, unsigned> GetSize() const {
        return std::make_pair(width(), height());
    }

    void SetFillBackground(bool fill) {
        setAutoFillBackground(fill);
        setAttribute(Qt::WA_OpaquePaintEvent, !fill);
        setAttribute(Qt::WA_NoSystemBackground, !fill);
        setAttribute(Qt::WA_PaintOnScreen, !fill);
    }

    QPaintEngine* paintEngine() const override {
        return autoFillBackground() ? QWidget::paintEngine() : nullptr;
    }

    virtual void Present() {}
    
private:
    GRenderWindow* parent;
};

#ifndef __APPLE__
class OpenGLWidget final : public RenderWidget {
public:
    OpenGLWidget(GRenderWindow* parent, QOpenGLContext* shared_context)
        : RenderWidget{parent},
          context(new QOpenGLContext(shared_context->parent())) {

        // disable vsync for any shared contexts
        auto format = shared_context->format();
        format.setSwapInterval(Settings::values.use_vsync ? 1 : 0);
        this->setFormat(format);

        context->setShareContext(shared_context);
        context->setScreen(this->screen());
        context->setFormat(format);
        context->create();

        setSurfaceType(QWindow::OpenGLSurface);

        // TODO: One of these flags might be interesting: WA_OpaquePaintEvent, WA_NoBackground,
        // WA_DontShowOnScreen, WA_DeleteOnClose
    }

    ~OpenGLWidget() override {
        context->doneCurrent();
    }

    void Present() override {
        if (!isExposed()) {
            return;
        }

        context->makeCurrent(this);
        Core::System::GetInstance().Renderer().TryPresent(100);
        context->swapBuffers(this);
        auto f = context->versionFunctions<QOpenGLFunctions_4_3_Core>();
        f->glFinish();
        QWindow::requestUpdate();
    }

private:
    QOpenGLContext* context{};
};
#endif

static Core::Frontend::WindowSystemType GetWindowSystemType() {
    // Determine WSI type based on Qt platform.
    QString platform_name = QGuiApplication::platformName();
    if (platform_name == QStringLiteral("windows"))
        return Core::Frontend::WindowSystemType::Windows;
    else if (platform_name == QStringLiteral("cocoa"))
        return Core::Frontend::WindowSystemType::MacOS;
    else if (platform_name == QStringLiteral("xcb"))
        return Core::Frontend::WindowSystemType::X11;
    else if (platform_name == QStringLiteral("wayland"))
        return Core::Frontend::WindowSystemType::Wayland;

    LOG_CRITICAL(Frontend, "Unknown Qt platform!");
    return Core::Frontend::WindowSystemType::Windows;
}

static Core::Frontend::EmuWindow::WindowSystemInfo GetWindowSystemInfo(QWindow* window) {
    Core::Frontend::EmuWindow::WindowSystemInfo wsi;
    wsi.type = GetWindowSystemType();

    // Our Win32 Qt external doesn't have the private API.
#if defined(WIN32) || defined(__APPLE__)
    wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    wsi.display_connection = pni->nativeResourceForWindow("display", window);
    if (wsi.type == Core::Frontend::WindowSystemType::Wayland)
        wsi.render_surface = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
    else
        wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#endif
    wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;

    return wsi;
}

GRenderWindow::GRenderWindow(QWidget* parent_, EmuThread* emu_thread)
    : QWidget(parent_), emu_thread(emu_thread) {
    setWindowTitle(QStringLiteral("yuzu %1 | %2-%3")
                       .arg(QString::fromUtf8(Common::g_build_name),
                            QString::fromUtf8(Common::g_scm_branch),
                            QString::fromUtf8(Common::g_scm_desc)));
    setAttribute(Qt::WA_AcceptTouchEvents);
    auto layout = new QHBoxLayout(this);
    layout->setMargin(0);
    setLayout(layout);
    InputCommon::Init();

    GMainWindow* parent = GetMainWindow();
    connect(this, &GRenderWindow::FirstFrameDisplayed, parent, &GMainWindow::OnLoadComplete);
}

GRenderWindow::~GRenderWindow() {
    InputCommon::Shutdown();
}

void GRenderWindow::MakeCurrent() {
#ifndef __APPLE__
    if (core_context) {
        core_context->MakeCurrent();
    }
#endif
}

void GRenderWindow::DoneCurrent() {
#ifndef __APPLE__
    if (core_context) {
        core_context->DoneCurrent();
    }
#endif
}

void GRenderWindow::PollEvents() {
    if (!first_frame) {
        first_frame = true;
        emit FirstFrameDisplayed();
    }
}

bool GRenderWindow::IsShown() const {
    return !isMinimized();
}

// On Qt 5.0+, this correctly gets the size of the framebuffer (pixels).
//
// Older versions get the window size (density independent pixels),
// and hence, do not support DPI scaling ("retina" displays).
// The result will be a viewport that is smaller than the extent of the window.
void GRenderWindow::OnFramebufferSizeChanged() {
    // Screen changes potentially incur a change in screen DPI, hence we should update the
    // framebuffer size
    const qreal pixel_ratio = windowPixelRatio();
    const u32 width = this->width() * pixel_ratio;
    const u32 height = this->height() * pixel_ratio;
    UpdateCurrentFramebufferLayout(width, height);
}

void GRenderWindow::BackupGeometry() {
    geometry = QWidget::saveGeometry();
}

void GRenderWindow::RestoreGeometry() {
    // We don't want to back up the geometry here (obviously)
    QWidget::restoreGeometry(geometry);
}

void GRenderWindow::restoreGeometry(const QByteArray& geometry) {
    // Make sure users of this class don't need to deal with backing up the geometry themselves
    QWidget::restoreGeometry(geometry);
    BackupGeometry();
}

QByteArray GRenderWindow::saveGeometry() {
    // If we are a top-level widget, store the current geometry
    // otherwise, store the last backup
    if (parent() == nullptr) {
        return QWidget::saveGeometry();
    }

    return geometry;
}

qreal GRenderWindow::windowPixelRatio() const {
    return devicePixelRatio();
}

std::pair<u32, u32> GRenderWindow::ScaleTouch(const QPointF pos) const {
    const qreal pixel_ratio = windowPixelRatio();
    return {static_cast<u32>(std::max(std::round(pos.x() * pixel_ratio), qreal{0.0})),
            static_cast<u32>(std::max(std::round(pos.y() * pixel_ratio), qreal{0.0}))};
}

void GRenderWindow::closeEvent(QCloseEvent* event) {
    emit Closed();
    QWidget::closeEvent(event);
}

void GRenderWindow::keyPressEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->PressKey(event->key());
}

void GRenderWindow::keyReleaseEvent(QKeyEvent* event) {
    InputCommon::GetKeyboard()->ReleaseKey(event->key());
}

void GRenderWindow::mousePressEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchBeginEvent

    auto pos = event->pos();
    if (event->button() == Qt::LeftButton) {
        const auto [x, y] = ScaleTouch(pos);
        this->TouchPressed(x, y);
    } else if (event->button() == Qt::RightButton) {
        InputCommon::GetMotionEmu()->BeginTilt(pos.x(), pos.y());
    }
}

void GRenderWindow::mouseMoveEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchUpdateEvent

    auto pos = event->pos();
    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
    InputCommon::GetMotionEmu()->Tilt(pos.x(), pos.y());
}

void GRenderWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventSynthesizedBySystem)
        return; // touch input is handled in TouchEndEvent

    if (event->button() == Qt::LeftButton)
        this->TouchReleased();
    else if (event->button() == Qt::RightButton)
        InputCommon::GetMotionEmu()->EndTilt();
}

void GRenderWindow::TouchBeginEvent(const QTouchEvent* event) {
    // TouchBegin always has exactly one touch point, so take the .first()
    const auto [x, y] = ScaleTouch(event->touchPoints().first().pos());
    this->TouchPressed(x, y);
}

void GRenderWindow::TouchUpdateEvent(const QTouchEvent* event) {
    QPointF pos;
    int active_points = 0;

    // average all active touch points
    for (const auto tp : event->touchPoints()) {
        if (tp.state() & (Qt::TouchPointPressed | Qt::TouchPointMoved | Qt::TouchPointStationary)) {
            active_points++;
            pos += tp.pos();
        }
    }

    pos /= active_points;

    const auto [x, y] = ScaleTouch(pos);
    this->TouchMoved(x, y);
}

void GRenderWindow::TouchEndEvent() {
    this->TouchReleased();
}

bool GRenderWindow::event(QEvent* event) {
    if (event->type() == QEvent::TouchBegin) {
        TouchBeginEvent(static_cast<QTouchEvent*>(event));
        return true;
    } else if (event->type() == QEvent::TouchUpdate) {
        TouchUpdateEvent(static_cast<QTouchEvent*>(event));
        return true;
    } else if (event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
        TouchEndEvent();
        return true;
    }

    return QWidget::event(event);
}

void GRenderWindow::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    InputCommon::GetKeyboard()->ReleaseAllKeys();
}

void GRenderWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    OnFramebufferSizeChanged();
}

std::unique_ptr<Core::Frontend::GraphicsContext> GRenderWindow::CreateSharedContext() const {
#ifndef __APPLE__
    if (Settings::values.renderer_backend == Settings::RendererBackend::OpenGL) {
        return std::make_unique<OpenGLContext>(QOpenGLContext::globalShareContext());
    }
#endif
    return {};
}

bool GRenderWindow::ReloadRenderTarget() {
#ifndef __APPLE__
    core_context.reset();
#endif
    delete child;
    delete layout();
    first_frame = false;

    child = new RenderWidget(this);

    // Update the Window System information with the new render target
    window_info = GetWindowSystemInfo(child->windowHandle());

    QBoxLayout* layout = new QHBoxLayout(this);
    layout->setMargin(0);
    setLayout(layout);

    switch (Settings::values.renderer_backend) {
    case Settings::RendererBackend::OpenGL:
        if (!InitializeOpenGL()) {
            return false;
        }
        break;
    case Settings::RendererBackend::Vulkan:
        if (!InitializeVulkan()) {
            return false;
        }
        break;
    }

    // Reset minimum required size to avoid resizing issues on the main window after restarting.
    setMinimumSize(1, 1);

    // Show causes the window to actually be created and the gl context as well, but we don't want
    // the widget to be shown yet, so immediately hide it.
    show();
    hide();

    resize(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);
    child->resize(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);

    OnFramebufferSizeChanged();

    BackupGeometry();

    if (Settings::values.renderer_backend == Settings::RendererBackend::OpenGL) {
        if (!LoadOpenGL()) {
            return false;
        }
    }

    return true;
}

void GRenderWindow::ReleaseRenderTarget() {
    if (child_widget) {
        layout()->removeWidget(child_widget);
        delete child_widget;
        child_widget = nullptr;
    }
}

void GRenderWindow::CaptureScreenshot(u32 res_scale, const QString& screenshot_path) {
    auto& renderer = Core::System::GetInstance().Renderer();

    if (res_scale == 0) {
        res_scale = VideoCore::GetResolutionScaleFactor(renderer);
    }

    const Layout::FramebufferLayout layout{Layout::FrameLayoutFromResolutionScale(res_scale)};
    screenshot_image = QImage(QSize(layout.width, layout.height), QImage::Format_RGB32);
    renderer.RequestScreenshot(
        screenshot_image.bits(),
        [=] {
            const std::string std_screenshot_path = screenshot_path.toStdString();
            if (screenshot_image.mirrored(false, true).save(screenshot_path)) {
                LOG_INFO(Frontend, "Screenshot saved to \"{}\"", std_screenshot_path);
            } else {
                LOG_ERROR(Frontend, "Failed to save screenshot to \"{}\"", std_screenshot_path);
            }
        },
        layout);
}

bool GRenderWindow::InitializeOpenGL() {
#ifndef __APPLE__
    // TODO: One of these flags might be interesting: WA_OpaquePaintEvent, WA_NoBackground,
    // WA_DontShowOnScreen, WA_DeleteOnClose
    QSurfaceFormat fmt;
    fmt.setVersion(4, 3);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    fmt.setOption(QSurfaceFormat::FormatOption::DeprecatedFunctions);
    // TODO: expose a setting for buffer value (ie default/single/double/triple)
    fmt.setSwapBehavior(QSurfaceFormat::DefaultSwapBehavior);
    fmt.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    layout()->addWidget(child_widget);

    core_context = CreateSharedContext();
    return true;
#else
    return false;
#endif
}

bool GRenderWindow::InitializeVulkan() {
#ifdef HAS_VULKAN
    child->windowHandle()->setSurfaceType(QSurface::SurfaceType::VulkanSurface);
    layout()->addWidget(child);
    return true;
#else
    QMessageBox::critical(this, tr("Vulkan not available!"),
                          tr("yuzu has not been compiled with Vulkan support."));
    return false;
#endif
}

bool GRenderWindow::LoadOpenGL() {
    Core::Frontend::ScopeAcquireContext acquire_context{*this};
    if (!gladLoadGL()) {
        QMessageBox::critical(this, tr("Error while initializing OpenGL 4.3!"),
                              tr("Your GPU may not support OpenGL 4.3, or you do not have the "
                                 "latest graphics driver."));
        return false;
    }

    QStringList unsupported_gl_extensions = GetUnsupportedGLExtensions();
    if (!unsupported_gl_extensions.empty()) {
        QMessageBox::critical(
            this, tr("Error while initializing OpenGL!"),
            tr("Your GPU may not support one or more required OpenGL extensions. Please ensure you "
               "have the latest graphics driver.<br><br>Unsupported extensions:<br>") +
                unsupported_gl_extensions.join(QStringLiteral("<br>")));
        return false;
    }
    return true;
}

QStringList GRenderWindow::GetUnsupportedGLExtensions() const {
    QStringList unsupported_ext;

    if (!GLAD_GL_ARB_buffer_storage)
        unsupported_ext.append(QStringLiteral("ARB_buffer_storage"));
    if (!GLAD_GL_ARB_direct_state_access)
        unsupported_ext.append(QStringLiteral("ARB_direct_state_access"));
    if (!GLAD_GL_ARB_vertex_type_10f_11f_11f_rev)
        unsupported_ext.append(QStringLiteral("ARB_vertex_type_10f_11f_11f_rev"));
    if (!GLAD_GL_ARB_texture_mirror_clamp_to_edge)
        unsupported_ext.append(QStringLiteral("ARB_texture_mirror_clamp_to_edge"));
    if (!GLAD_GL_ARB_multi_bind)
        unsupported_ext.append(QStringLiteral("ARB_multi_bind"));
    if (!GLAD_GL_ARB_clip_control)
        unsupported_ext.append(QStringLiteral("ARB_clip_control"));

    // Extensions required to support some texture formats.
    if (!GLAD_GL_EXT_texture_compression_s3tc)
        unsupported_ext.append(QStringLiteral("EXT_texture_compression_s3tc"));
    if (!GLAD_GL_ARB_texture_compression_rgtc)
        unsupported_ext.append(QStringLiteral("ARB_texture_compression_rgtc"));
    if (!GLAD_GL_ARB_depth_buffer_float)
        unsupported_ext.append(QStringLiteral("ARB_depth_buffer_float"));

    for (const QString& ext : unsupported_ext)
        LOG_CRITICAL(Frontend, "Unsupported GL extension: {}", ext.toStdString());

    return unsupported_ext;
}

void GRenderWindow::OnEmulationStarting(EmuThread* emu_thread) {
    this->emu_thread = emu_thread;
    child->SetFillBackground(false);
}

void GRenderWindow::OnEmulationStopping() {
    emu_thread = nullptr;
    child->SetFillBackground(true);
}

void GRenderWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

    // windowHandle() is not initialized until the Window is shown, so we connect it here.
    connect(windowHandle(), &QWindow::screenChanged, this, &GRenderWindow::OnFramebufferSizeChanged,
            Qt::UniqueConnection);
}
