/* This file is part of the KDE project
   SPDX-FileCopyrightText: 2018-2021 Jan Grulich <jgrulich@redhat.com>
   SPDX-FileCopyrightText: 2018 Oleg Chernovskiy <kanedias@xaker.ru>

   SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "config-krfb.h"

// system
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <vector>

// Qt
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QScreen>
#include <QSocketNotifier>
#include <qpa/qplatformnativeinterface.h>

#include <KConfigGroup>
#include <KSharedConfig>

struct wl_display;
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/registry.h>

// pipewire
#include <climits>

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include "krfb_fb_pipewire_debug.h"
#include "pw_framebuffer.h"
#include "screencasting.h"
#include "xdp_dbus_remotedesktop_interface.h"
#include "xdp_dbus_screencast_interface.h"
#include <PipeWireSourceStream>

static const int BYTES_PER_PIXEL = 4;
static const uint MIN_SUPPORTED_XDP_KDE_SC_VERSION = 1;

Q_DECLARE_METATYPE(PWFrameBuffer::Stream);
Q_DECLARE_METATYPE(PWFrameBuffer::Streams);

const QDBusArgument &operator>>(const QDBusArgument &arg, PWFrameBuffer::Stream &stream)
{
    arg.beginStructure();
    arg >> stream.nodeId;

    arg.beginMap();
    while (!arg.atEnd()) {
        QString key;
        QVariant map;
        arg.beginMapEntry();
        arg >> key >> map;
        arg.endMapEntry();
        stream.map.insert(key, map);
    }
    arg.endMap();
    arg.endStructure();

    return arg;
}

/**
 * @brief The PWFrameBuffer::Private class - private counterpart of PWFramebuffer class. This is the entity where
 *        whole logic resides, for more info search for "d-pointer pattern" information.
 */
class PWFrameBuffer::Private
{
public:
    Private(PWFrameBuffer *q);
    ~Private();

private:
    friend class PWFrameBuffer;

    void initDbus();

    // dbus handling
    void handleSessionCreated(quint32 code, const QVariantMap &results);
    void handleDevicesSelected(quint32 code, const QVariantMap &results);
    void handleSourcesSelected(quint32 code, const QVariantMap &results);
    void handleRemoteDesktopStarted(quint32 code, const QVariantMap &results);
    void setVideoSize(const QSize &size);

    // pw handling
    void handleFrame(const PipeWireFrame &frame);

    // link to public interface
    PWFrameBuffer *q;

    // requests a session from XDG Desktop Portal
    // auto-generated and compiled from xdp_dbus_interface.xml file
    QScopedPointer<OrgFreedesktopPortalScreenCastInterface> dbusXdpScreenCastService;
    QScopedPointer<OrgFreedesktopPortalRemoteDesktopInterface> dbusXdpRemoteDesktopService;

    // XDP screencast session handle
    QDBusObjectPath sessionPath;

    // screen geometry holder
    QSize videoSize;

    // sanity indicator
    bool isValid = true;
    std::unique_ptr<PipeWireSourceStream> stream;
    std::optional<PipeWireCursor> cursor;

    // EGL resources for DMA-BUF readback (cached across frames)
    struct EglResources {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLContext context = EGL_NO_CONTEXT;
        GLuint texture = 0;
        GLuint fbo = 0;
    };
    EglResources egl;

    bool setupEgl();
    bool downloadFrame(const PipeWireFrame &frame, QImage &qimage);
};

PWFrameBuffer::Private::Private(PWFrameBuffer *q)
    : q(q)
    , stream(new PipeWireSourceStream(q))
{
    QObject::connect(stream.get(), &PipeWireSourceStream::frameReceived, q, [this](const PipeWireFrame &frame) {
        handleFrame(frame);
    });
}

/**
 * @brief PWFrameBuffer::Private::initDbus - initialize D-Bus connectivity with XDG Desktop Portal.
 *        Based on XDG_CURRENT_DESKTOP environment variable it will give us implementation that we need,
 *        in case of KDE it is xdg-desktop-portal-kde binary.
 */
void PWFrameBuffer::Private::initDbus()
{
    qInfo() << "Initializing D-Bus connectivity with XDG Desktop Portal";
    dbusXdpScreenCastService.reset(new OrgFreedesktopPortalScreenCastInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                                               QStringLiteral("/org/freedesktop/portal/desktop"),
                                                                               QDBusConnection::sessionBus()));
    dbusXdpRemoteDesktopService.reset(new OrgFreedesktopPortalRemoteDesktopInterface(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                                                     QStringLiteral("/org/freedesktop/portal/desktop"),
                                                                                     QDBusConnection::sessionBus()));
    auto version = dbusXdpScreenCastService->version();
    if (version < MIN_SUPPORTED_XDP_KDE_SC_VERSION) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Unsupported XDG Portal screencast interface version:" << version;
        isValid = false;
        return;
    }

    // create session
    auto sessionParameters = QVariantMap{
        {QStringLiteral("session_handle_token"), QStringLiteral("krfb%1").arg(QRandomGenerator::global()->generate())},
        {QStringLiteral("handle_token"), QStringLiteral("krfb%1").arg(QRandomGenerator::global()->generate())}};
    auto sessionReply = dbusXdpRemoteDesktopService->CreateSession(sessionParameters);
    sessionReply.waitForFinished();
    if (!sessionReply.isValid()) {
        qWarning("Couldn't initialize XDP-KDE screencast session");
        isValid = false;
        return;
    }

    qInfo() << "DBus session created: " << sessionReply.value().path();
    QDBusConnection::sessionBus().connect(QString(),
                                          sessionReply.value().path(),
                                          QStringLiteral("org.freedesktop.portal.Request"),
                                          QStringLiteral("Response"),
                                          this->q,
                                          SLOT(handleXdpSessionCreated(uint, QVariantMap)));
}

void PWFrameBuffer::handleXdpSessionCreated(quint32 code, const QVariantMap &results)
{
    d->handleSessionCreated(code, results);
}

/**
 * @brief PWFrameBuffer::Private::handleSessionCreated - handle creation of ScreenCast session.
 *        XDG Portal answers with session path if it was able to successfully create the screencast.
 *
 * @param code return code for dbus call. Zero is success, non-zero means error
 * @param results map with results of call.
 */
void PWFrameBuffer::Private::handleSessionCreated(quint32 code, const QVariantMap &results)
{
    if (code != 0) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Failed to create session: " << code;
        isValid = false;
        return;
    }

    sessionPath = QDBusObjectPath(results.value(QStringLiteral("session_handle")).toString());

    // select sources for the session
    auto selectionOptions = QVariantMap{
        // We have to specify it's an uint, otherwise xdg-desktop-portal will not forward it to backend implementation
        {QStringLiteral("types"), QVariant::fromValue<uint>(7)}, // request all (KeyBoard, Pointer, TouchScreen)
        {QStringLiteral("handle_token"), QStringLiteral("krfb%1").arg(QRandomGenerator::global()->generate())},
        {QStringLiteral("persist_mode"), QVariant::fromValue<uint>(2)}, // Persist permission until explicitly revoked by user
    };

    KConfigGroup stateConfig = KSharedConfig::openStateConfig()->group(QStringLiteral("XdgPortal"));
    const QString restoreToken = stateConfig.readEntry(QStringLiteral("RestoreToken"), QString());
    if (!restoreToken.isEmpty()) {
        selectionOptions[QStringLiteral("restore_token")] = restoreToken;
    }

    auto selectorReply = dbusXdpRemoteDesktopService->SelectDevices(sessionPath, selectionOptions);
    selectorReply.waitForFinished();
    if (!selectorReply.isValid()) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Couldn't select devices for the remote-desktop session";
        isValid = false;
        return;
    }
    QDBusConnection::sessionBus().connect(QString(),
                                          selectorReply.value().path(),
                                          QStringLiteral("org.freedesktop.portal.Request"),
                                          QStringLiteral("Response"),
                                          this->q,
                                          SLOT(handleXdpDevicesSelected(uint, QVariantMap)));
}

void PWFrameBuffer::handleXdpDevicesSelected(quint32 code, const QVariantMap &results)
{
    d->handleDevicesSelected(code, results);
}

/**
 * @brief PWFrameBuffer::Private::handleDevicesCreated - handle selection of devices we want to use for remote desktop
 *
 * @param code return code for dbus call. Zero is success, non-zero means error
 * @param results map with results of call.
 */
void PWFrameBuffer::Private::handleDevicesSelected(quint32 code, const QVariantMap &results)
{
    Q_UNUSED(results)
    if (code != 0) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Failed to select devices: " << code;
        isValid = false;
        return;
    }

    // select sources for the session
    auto selectionOptions = QVariantMap{
        {QStringLiteral("types"), QVariant::fromValue<uint>(1)}, // only MONITOR is supported
        {QStringLiteral("multiple"), false},
        {QStringLiteral("handle_token"), QStringLiteral("krfb%1").arg(QRandomGenerator::global()->generate())}};
    auto selectorReply = dbusXdpScreenCastService->SelectSources(sessionPath, selectionOptions);
    selectorReply.waitForFinished();
    if (!selectorReply.isValid()) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Couldn't select sources for the screen-casting session";
        isValid = false;
        return;
    }
    QDBusConnection::sessionBus().connect(QString(),
                                          selectorReply.value().path(),
                                          QStringLiteral("org.freedesktop.portal.Request"),
                                          QStringLiteral("Response"),
                                          this->q,
                                          SLOT(handleXdpSourcesSelected(uint, QVariantMap)));
}

void PWFrameBuffer::handleXdpSourcesSelected(quint32 code, const QVariantMap &results)
{
    d->handleSourcesSelected(code, results);
}

/**
 * @brief PWFrameBuffer::Private::handleSourcesSelected - handle Screencast sources selection.
 *        XDG Portal shows a dialog at this point which allows you to select monitor from the list.
 *        This function is called after you make a selection.
 *
 * @param code return code for dbus call. Zero is success, non-zero means error
 * @param results map with results of call.
 */
void PWFrameBuffer::Private::handleSourcesSelected(quint32 code, const QVariantMap &)
{
    if (code != 0) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Failed to select sources: " << code;
        isValid = false;
        return;
    }

    // start session
    auto startParameters = QVariantMap{
        {QStringLiteral("handle_token"), QStringLiteral("krfb%1").arg(QRandomGenerator::global()->generate())}};
    auto startReply = dbusXdpRemoteDesktopService->Start(sessionPath, QString(), startParameters);
    startReply.waitForFinished();
    QDBusConnection::sessionBus().connect(QString(),
                                          startReply.value().path(),
                                          QStringLiteral("org.freedesktop.portal.Request"),
                                          QStringLiteral("Response"),
                                          this->q,
                                          SLOT(handleXdpRemoteDesktopStarted(uint, QVariantMap)));
}

void PWFrameBuffer::handleXdpRemoteDesktopStarted(quint32 code, const QVariantMap &results)
{
    d->handleRemoteDesktopStarted(code, results);
}

/**
 * @brief PWFrameBuffer::Private::handleScreencastStarted - handle Screencast start.
 *        At this point there shall be ready pipewire stream to consume.
 *
 * @param code return code for dbus call. Zero is success, non-zero means error
 * @param results map with results of call.
 */
void PWFrameBuffer::Private::handleRemoteDesktopStarted(quint32 code, const QVariantMap &results)
{
    if (code != 0) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Failed to start screencast: " << code;
        isValid = false;
        return;
    }

    if (results.value(QStringLiteral("devices")).toUInt() == 0) {
        qCWarning(KRFB_FB_PIPEWIRE) << "No devices were granted" << results;
        isValid = false;
        return;
    }

    // there should be only one stream
    const Streams streams = qdbus_cast<Streams>(results.value(QStringLiteral("streams")));
    if (streams.isEmpty()) {
        // maybe we should check deeper with qdbus_cast but this suffices for now
        qCWarning(KRFB_FB_PIPEWIRE) << "Failed to get screencast streams";
        isValid = false;
        return;
    }

    auto streamReply = dbusXdpScreenCastService->OpenPipeWireRemote(sessionPath, QVariantMap());
    streamReply.waitForFinished();
    if (!streamReply.isValid()) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Couldn't open pipewire remote for the screen-casting session";
        isValid = false;
        return;
    }

    QDBusUnixFileDescriptor pipewireFd = streamReply.value();
    if (!pipewireFd.isValid()) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Couldn't get pipewire connection file descriptor";
        isValid = false;
        return;
    }

    if (!stream->createStream(streams.first().nodeId, pipewireFd.takeFileDescriptor())) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Couldn't create the pipewire stream";
        isValid = false;
        return;
    }

    // save restore token
    KConfigGroup stateConfig = KSharedConfig::openStateConfig()->group(QStringLiteral("XdgPortal"));
    stateConfig.writeEntry(QStringLiteral("RestoreToken"), results[QStringLiteral("restore_token")].toString());
}

void PWFrameBuffer::Private::handleFrame(const PipeWireFrame &frame)
{
    cursor = frame.cursor;

#if KPIPEWIRE60
    if (!frame.dmabuf && !frame.image) {
#else
    if (!frame.dmabuf && !frame.dataFrame) {
#endif
        qCDebug(KRFB_FB_PIPEWIRE) << "Got empty buffer. The buffer possibly carried only "
                                     "information about the mouse cursor.";
        return;
    }

#if KPIPEWIRE60
    if (frame.image) {
        memcpy(q->fb, frame.image->constBits(), frame.image->sizeInBytes());
        setVideoSize(frame.image->size());
    }
#else
    if (frame.dataFrame) {
        // FIXME: Assuming stride == width * 4, not sure to which extent this holds
        setVideoSize(frame.dataFrame->size);
        memcpy(q->fb, frame.dataFrame->data, frame.dataFrame->size.height() * frame.dataFrame->stride);
    }
#endif
    else if (frame.dmabuf) {
        // FIXME: Assuming stride == width * 4, not sure to which extent this holds
        const QSize size = {frame.dmabuf->width, frame.dmabuf->height};
        setVideoSize(size);
        QImage src(reinterpret_cast<uchar *>(q->fb), size.width(), size.height(), QImage::Format_RGB32);
        if (!downloadFrame(frame, src)) {
            stream->renegotiateModifierFailed(frame.format, frame.dmabuf->modifier);
            qCDebug(KRFB_FB_PIPEWIRE) << "Failed to download frame.";
            return;
        }
    } else {
        qCDebug(KRFB_FB_PIPEWIRE) << "Unknown kind of frame";
    }

    if (auto damage = frame.damage) {
        for (const auto &rect : *damage) {
            q->tiles.append(rect);
        }
    } else {
        q->tiles.append(QRect(0, 0, videoSize.width(), videoSize.height()));
    }
}

void PWFrameBuffer::Private::setVideoSize(const QSize &size)
{
    if (q->fb && videoSize == size) {
        return;
    }

    free(q->fb);
    q->fb = static_cast<char *>(malloc(size.width() * size.height() * BYTES_PER_PIXEL));
    if (!q->fb) {
        qCWarning(KRFB_FB_PIPEWIRE) << "Failed to allocate buffer";
        isValid = false;
        return;
    }
    videoSize = size;

    Q_EMIT q->frameBufferChanged();
}

PWFrameBuffer::Private::~Private()
{
    if (egl.display != EGL_NO_DISPLAY) {
        if (egl.texture) {
            glDeleteTextures(1, &egl.texture);
        }
        if (egl.fbo) {
            glDeleteFramebuffers(1, &egl.fbo);
        }
        if (egl.context != EGL_NO_CONTEXT) {
            eglDestroyContext(egl.display, egl.context);
        }
        eglTerminate(egl.display);
    }
}

bool PWFrameBuffer::Private::setupEgl()
{
    if (egl.display != EGL_NO_DISPLAY) {
        return true;
    }

    // Get the Wayland display from QPA
    auto *native = QGuiApplication::platformNativeInterface();
    auto *wl_display = native ? static_cast<struct wl_display *>(native->nativeResourceForIntegration(QByteArrayLiteral("display"))) : nullptr;

    EGLDisplay disp = EGL_NO_DISPLAY;
    if (wl_display) {
        disp = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display, nullptr);
    }
    if (disp == EGL_NO_DISPLAY) {
        disp = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (disp == EGL_NO_DISPLAY) {
        qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: no Wayland EGL display, trying default";
        disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (disp == EGL_NO_DISPLAY) {
        qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: failed to get EGL display";
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(disp, &major, &minor)) {
        qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: eglInitialize failed";
        return false;
    }

    bool is_gles = false;
    if (!eglBindAPI(EGL_OPENGL_API)) {
        qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: OpenGL API unavailable, trying OpenGL ES";
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: failed to bind any EGL API";
            return false;
        }
        is_gles = true;
    }

    EGLConfig config;
    EGLint count;
    if (is_gles) {
        const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_NONE,
        };
        if (!eglChooseConfig(disp, attribs, &config, 1, &count)) {
            qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: no ES3 config";
            return false;
        }
    } else {
        const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_NONE,
        };
        if (!eglChooseConfig(disp, attribs, &config, 1, &count)) {
            qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: no desktop GL config, trying ES3";
            const EGLint es_attribs[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                EGL_NONE,
            };
            if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(disp, es_attribs, &config, 1, &count)) {
                return false;
            }
            is_gles = true;
        }
    }

    if (is_gles) {
        const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        egl.context = eglCreateContext(disp, config, EGL_NO_CONTEXT, ctx_attribs);
    } else {
        const EGLint ctx_attribs[] = {EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE};
        egl.context = eglCreateContext(disp, config, EGL_NO_CONTEXT, ctx_attribs);
    }

    if (egl.context == EGL_NO_CONTEXT) {
        qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: context creation failed";
        return false;
    }

    if (!eglMakeCurrent(disp, EGL_NO_SURFACE, EGL_NO_SURFACE, egl.context)) {
        qCWarning(KRFB_FB_PIPEWIRE) << "setupEgl: eglMakeCurrent failed";
        return false;
    }

    egl.display = disp;
    qCDebug(KRFB_FB_PIPEWIRE) << "setupEgl: EGL initialized (GLES=" << is_gles << ")";
    return true;
}

bool PWFrameBuffer::Private::downloadFrame(const PipeWireFrame &frame, QImage &qimage)
{
    if (!setupEgl()) {
        return false;
    }

    const auto &dmabuf = *frame.dmabuf;
    if (dmabuf.planes.isEmpty()) {
        qCWarning(KRFB_FB_PIPEWIRE) << "downloadFrame: no planes";
        return false;
    }

    // Build EGL attrs for DMA-BUF import
    // eglCreateImageKHR uses EGLint-based attributes
    std::vector<EGLint> attribs;
    attribs.reserve(4 + dmabuf.planes.size() * 3 + 1);
    attribs.push_back(EGL_WIDTH);
    attribs.push_back(dmabuf.width);
    attribs.push_back(EGL_HEIGHT);
    attribs.push_back(dmabuf.height);
    attribs.push_back(EGL_LINUX_DRM_FOURCC_EXT);
    attribs.push_back(static_cast<EGLint>(dmabuf.format));

    for (int i = 0; i < dmabuf.planes.size(); i++) {
        const auto &plane = dmabuf.planes[i];
        // dup the FD: eglCreateImage takes ownership, PipeWireFrame owns the original
        int dup_fd = fcntl(plane.fd, F_DUPFD_CLOEXEC, 3);
        if (dup_fd < 0) {
            qCWarning(KRFB_FB_PIPEWIRE) << "downloadFrame: failed to dup plane" << i << "fd";
            return false;
        }
        attribs.push_back(EGL_DMA_BUF_PLANE0_FD_EXT + i * 3);
        attribs.push_back(dup_fd);
        attribs.push_back(EGL_DMA_BUF_PLANE0_OFFSET_EXT + i * 3);
        attribs.push_back(static_cast<EGLint>(plane.offset));
        attribs.push_back(EGL_DMA_BUF_PLANE0_PITCH_EXT + i * 3);
        attribs.push_back(static_cast<EGLint>(plane.stride));
    }
    attribs.push_back(EGL_NONE);

    EGLImageKHR image = eglCreateImageKHR(egl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        qCWarning(KRFB_FB_PIPEWIRE) << "downloadFrame: eglCreateImageKHR failed";
        return false;
    }

    auto releaseImage = qScopeGuard([&]() {
        eglDestroyImageKHR(egl.display, image);
    });

    // Reuse texture + FBO across frames
    if (!egl.texture) {
        glGenTextures(1, &egl.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!egl.fbo) {
        glGenFramebuffers(1, &egl.fbo);
    }

    glBindTexture(GL_TEXTURE_2D, egl.texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glBindFramebuffer(GL_FRAMEBUFFER, egl.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, egl.texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        qCWarning(KRFB_FB_PIPEWIRE) << "downloadFrame: framebuffer incomplete";
        return false;
    }

    // Read with GL_BGRA → byte order B,G,R,A matches QImage::Format_RGB32 on LE
    glReadPixels(0, 0, dmabuf.width, dmabuf.height, GL_BGRA, GL_UNSIGNED_BYTE, qimage.bits());

    // glReadPixels reads from bottom-left; QImage expects top-left → flip rows
    int stride = qimage.bytesPerLine();
    int h = dmabuf.height;
    auto tmpRow = std::make_unique<uchar[]>(stride);
    for (int y = 0; y < h / 2; y++) {
        uchar *top = qimage.scanLine(y);
        uchar *bot = qimage.scanLine(h - 1 - y);
        memcpy(tmpRow.get(), top, stride);
        memcpy(top, bot, stride);
        memcpy(bot, tmpRow.get(), stride);
    }

    return true;
}

PWFrameBuffer::PWFrameBuffer(QObject *parent)
    : FrameBuffer(parent)
    , d(new Private(this))
{
}

PWFrameBuffer::~PWFrameBuffer()
{
    free(fb);
    fb = nullptr;
}

void PWFrameBuffer::initDBus()
{
    d->initDbus();
}

void PWFrameBuffer::startVirtualMonitor(const QString &name, const QSize &resolution, qreal dpr)
{
    d->videoSize = resolution * dpr;
    using namespace KWayland::Client;
    auto connection = ConnectionThread::fromApplication(this);
    if (!connection) {
        qWarning() << "Failed getting Wayland connection from QPA";
        QCoreApplication::exit(1);
        return;
    }

    auto registry = new Registry(this);
    connect(registry, &KWayland::Client::Registry::interfaceAnnounced, this, [this, registry, name, dpr, resolution](const QByteArray &interfaceName, quint32 wlname, quint32 version) {
        if (interfaceName != "zkde_screencast_unstable_v1") {
            return;
        }

        auto screencasting = new Screencasting(registry, wlname, version, this);
        auto r = screencasting->createVirtualMonitorStream(name, resolution, dpr, Screencasting::Metadata);
        connect(r, &ScreencastingStream::created, this, [this](quint32 nodeId) {
            d->stream->createStream(nodeId, 0);
        });
    });
    registry->create(connection);
    registry->setup();
}

int PWFrameBuffer::depth()
{
    return 32;
}

int PWFrameBuffer::height()
{
    if (!d->videoSize.isValid()) {
        return 0;
    }
    return d->videoSize.height();
}

int PWFrameBuffer::width()
{
    if (!d->videoSize.isValid()) {
        return 0;
    }
    return d->videoSize.width();
}

int PWFrameBuffer::paddedWidth()
{
    return width() * 4;
}

void PWFrameBuffer::getServerFormat(rfbPixelFormat &format)
{
    format.bitsPerPixel = 32;
    format.depth = 32;
    format.trueColour = true;
    format.bigEndian = false;
}

void PWFrameBuffer::startMonitor()
{
}

void PWFrameBuffer::stopMonitor()
{
}

QVariant PWFrameBuffer::customProperty(const QString &property) const
{
    if (property == QLatin1String("stream_node_id")) {
        return QVariant::fromValue<uint>(d->stream->nodeId());
    }
    if (property == QLatin1String("session_handle")) {
        return QVariant::fromValue<QDBusObjectPath>(d->sessionPath);
    }

    return FrameBuffer::customProperty(property);
}

bool PWFrameBuffer::isValid() const
{
    return d->isValid;
}

QPoint PWFrameBuffer::cursorPosition()
{
    const auto cursor = d->cursor;
    if (cursor) {
        return cursor->position;
    } else {
        return {};
    }
}
