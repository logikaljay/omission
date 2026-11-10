#include "capturer.h"
#include "downscale.h"

#include <QAbstractEventDispatcher>
#include <QMetaObject>
#include <QSocketNotifier>
#include <QThreadPool>
#include <QTimer>

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client.h"
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
#include "linux-dmabuf-v1-client.h"

#include <gbm.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

// OMISSION_DEBUG=1 traces the capture protocol on stderr.
static const bool debug = qEnvironmentVariableIsSet("OMISSION_DEBUG");
#define TRACE(...) do { if (debug) qInfo(__VA_ARGS__); } while (0)

static std::mutex gbmMutex; // libgbm makes no promises about threads

// Buffers handed to the render thread can outlive the capturer's Wayland
// connection and GPU device at shutdown. Once those are gone, a buffer only
// gives back its memory.
static std::mutex teardownMutex;
static bool connectionAlive = true;

CaptureBuffer::~CaptureBuffer() {
    std::lock_guard teardown(teardownMutex);
    if (buffer && connectionAlive)
        wl_buffer_destroy(buffer);
    if (data)
        munmap(data, size);
    for (int i = 0; i < planes; ++i)
        if (fds[i] >= 0)
            ::close(fds[i]);
    if (bo && connectionAlive) {
        std::lock_guard lock(gbmMutex);
        gbm_bo_destroy(bo);
    }
}

struct Capturer::Toplevel {
    Capturer *owner = nullptr;
    ext_foreign_toplevel_handle_v1 *handle = nullptr;
    QString id;
    bool announced = false;

    ext_image_capture_source_v1 *source = nullptr;
    ext_image_copy_capture_session_v1 *session = nullptr;
    bool constraintsKnown = false;
    int width = 0, height = 0;
    uint32_t format = UINT32_MAX;
    // Constraints arrive as a batch closed by "done", in no fixed order, so
    // they collect here and only take effect on done.
    int pendingWidth = 0, pendingHeight = 0;
    uint32_t pendingFormat = UINT32_MAX;
    // The dmabuf format on offer (DRM fourcc, 0 if none) and its modifiers.
    uint32_t dmabufFormat = 0, pendingDmabufFormat = 0;
    QList<uint64_t> modifiers, pendingModifiers;

    std::shared_ptr<CaptureBuffer> buffer;
    ext_image_copy_capture_frame_v1 *frame = nullptr;
    bool processing = false; // a downscale is running on the pool
    bool wanted = false;     // capture once more
    bool live = false;       // keep capturing on damage
    quint64 serial = 0;
    qint64 lastCapture = -1000;
    QTimer *retry = nullptr;
};

Capturer::Capturer(int maxDimension) : m_maxDimension(maxDimension) {
    m_thread.setObjectName("omission-capture");
    moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, this, &Capturer::start);
    m_thread.start();
}

Capturer::~Capturer() {
    QThreadPool::globalInstance()->waitForDone();
    QMetaObject::invokeMethod(this, [this] {
        for (auto *t : std::as_const(m_toplevels))
            forget(t);
        m_toplevels.clear();
        delete m_notifier;
        std::lock_guard teardown(teardownMutex);
        connectionAlive = false;
        if (m_gbm)
            gbm_device_destroy(m_gbm);
        if (m_gbmFd >= 0)
            ::close(m_gbmFd);
        if (m_display)
            wl_display_disconnect(m_display);
        m_display = nullptr;
        m_thread.quit();
    }, Qt::QueuedConnection);
    m_thread.wait(2000);
}

void Capturer::start() {
    m_clock.start();
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        emit failed(tr("Can't connect to the Wayland display, so windows can't be previewed."));
        return;
    }
    static const wl_registry_listener registryListener = {onGlobal, onGlobalRemove};
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &registryListener, this);
    wl_display_roundtrip(m_display);
    if (!m_shm || !m_list || !m_sources || !m_copy) {
        emit failed(tr("This compositor doesn't offer ext-image-copy-capture, so windows can't be previewed."));
        return;
    }
    static const ext_foreign_toplevel_list_v1_listener listListener = {onToplevel, onListFinished};
    ext_foreign_toplevel_list_v1_add_listener(m_list, &listListener, this);
    wl_display_roundtrip(m_display);

    m_notifier = new QSocketNotifier(wl_display_get_fd(m_display), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Capturer::dispatch);
    // Qt's event loop drains queued calls before sleeping, so flush there
    // too: requests made from a queued slot then reach the compositor at once.
    connect(QAbstractEventDispatcher::instance(&m_thread), &QAbstractEventDispatcher::aboutToBlock, this, &Capturer::flush);
}

void Capturer::dispatch() {
    TRACE("omission: wayland readable");
    if (!m_display)
        return;
    if (wl_display_prepare_read(m_display) == 0) {
        if (wl_display_read_events(m_display) < 0) {
            emit failed(tr("Lost the Wayland connection used for previews."));
            m_notifier->setEnabled(false);
            return;
        }
    }
    wl_display_dispatch_pending(m_display);
    flush();
}

void Capturer::flush() {
    if (m_display)
        wl_display_flush(m_display);
}

void Capturer::onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    auto *self = static_cast<Capturer *>(data);
    if (!std::strcmp(interface, wl_shm_interface.name))
        self->m_shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (!std::strcmp(interface, ext_foreign_toplevel_list_v1_interface.name))
        self->m_list = static_cast<ext_foreign_toplevel_list_v1 *>(wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1));
    else if (!std::strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
        self->m_sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1 *>(
            wl_registry_bind(registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    else if (!std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name))
        self->m_copy = static_cast<ext_image_copy_capture_manager_v1 *>(wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
    else if (!std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && version >= 3)
        self->m_dmabuf = static_cast<zwp_linux_dmabuf_v1 *>(wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3));
}

void Capturer::onGlobalRemove(void *, wl_registry *, uint32_t) {}

void Capturer::onToplevel(void *data, ext_foreign_toplevel_list_v1 *, ext_foreign_toplevel_handle_v1 *handle) {
    auto *self = static_cast<Capturer *>(data);
    auto *t = new Toplevel;
    t->owner = self;
    t->handle = handle;
    self->m_toplevels.insert(t);
    static const ext_foreign_toplevel_handle_v1_listener listener = {onClosed, onDone, onTitle, onAppId, onIdentifier};
    ext_foreign_toplevel_handle_v1_add_listener(handle, &listener, t);
}

void Capturer::onListFinished(void *, ext_foreign_toplevel_list_v1 *) {}

void Capturer::onClosed(void *data, ext_foreign_toplevel_handle_v1 *) {
    auto *t = static_cast<Toplevel *>(data);
    Capturer *self = t->owner;
    self->m_byId.remove(t->id);
    self->m_toplevels.remove(t);
    self->forget(t);
}

void Capturer::onDone(void *data, ext_foreign_toplevel_handle_v1 *) {
    auto *t = static_cast<Toplevel *>(data);
    if (t->announced)
        return;
    t->announced = true;
    Capturer *self = t->owner;
    self->m_byId.insert(t->id, t);
    TRACE("omission: toplevel %s", qPrintable(t->id));
    // The overview asked for this window before the compositor told us
    // about it (it had only just opened).
    if (self->m_wanted.remove(t->id)) {
        t->live = self->m_live;
        self->want(t);
    }
}

void Capturer::onTitle(void *, ext_foreign_toplevel_handle_v1 *, const char *) {}
void Capturer::onAppId(void *, ext_foreign_toplevel_handle_v1 *, const char *) {}

void Capturer::onIdentifier(void *data, ext_foreign_toplevel_handle_v1 *, const char *identifier) {
    static_cast<Toplevel *>(data)->id = QString::fromUtf8(identifier);
}

void Capturer::disableDmabuf() {
    if (!m_gbm)
        return;
    TRACE("omission: dmabuf import failed, switching to shared memory");
    m_gbm = nullptr; // the device is kept open; buffers may still refer to it
    for (auto *t : std::as_const(m_toplevels))
        t->buffer.reset();
}

void Capturer::capture(const QStringList &ids, bool live) {
    static const bool disabled = qEnvironmentVariableIsSet("OMISSION_NO_CAPTURE");
    if (disabled)
        return;
    TRACE("omission: %lld capture %lld windows (live %d), %lld known", m_clock.elapsed(), qsizetype(ids.size()), live, qsizetype(m_byId.size()));
    m_live = live;
    for (const auto &id : ids) {
        Toplevel *t = m_byId.value(id);
        if (!t) {
            m_wanted.insert(id);
            continue;
        }
        t->live = live;
        want(t);
    }
}

void Capturer::rest() {
    m_live = false;
    m_wanted.clear();
    for (auto *t : std::as_const(m_toplevels)) {
        t->live = false;
        t->wanted = false;
        if (t->retry)
            t->retry->stop();
        // A pending live frame waits for damage that may never come; drop
        // it and the memory behind it.
        if (t->frame) {
            ext_image_copy_capture_frame_v1_destroy(t->frame);
            t->frame = nullptr;
        }
        // Sessions stay: Hyprland answers every capture request whether or
        // not the window changed, and a kept session saves the round trip
        // for its constraints (~40 ms) on the next opening.
        t->buffer.reset();
    }
}

void Capturer::want(Toplevel *t) {
    t->wanted = true;
    // A live frame already waiting for damage will deliver on its own; a
    // one-off request should not have to wait for damage, so replace it.
    if (t->frame && !t->live) {
        ext_image_copy_capture_frame_v1_destroy(t->frame);
        t->frame = nullptr;
    }
    ensureSession(t);
    startFrame(t);
}

void Capturer::ensureSession(Toplevel *t) {
    if (t->session || !m_sources || !m_copy)
        return;
    t->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(m_sources, t->handle);
    t->session = ext_image_copy_capture_manager_v1_create_session(m_copy, t->source, 0);
    static const ext_image_copy_capture_session_v1_listener listener = {
        onBufferSize, onShmFormat, onDmabufDevice, onDmabufFormat, onSessionDone, onStopped};
    ext_image_copy_capture_session_v1_add_listener(t->session, &listener, t);
}

void Capturer::onBufferSize(void *data, ext_image_copy_capture_session_v1 *, uint32_t width, uint32_t height) {
    auto *t = static_cast<Toplevel *>(data);
    t->pendingWidth = int(width);
    t->pendingHeight = int(height);
}

void Capturer::onShmFormat(void *data, ext_image_copy_capture_session_v1 *, uint32_t format) {
    auto *t = static_cast<Toplevel *>(data);
    // Only the two 32-bit formats QImage reads without conversion; prefer
    // opaque XRGB, which is what nearly every window offers.
    if (format == WL_SHM_FORMAT_XRGB8888 || (format == WL_SHM_FORMAT_ARGB8888 && t->pendingFormat != WL_SHM_FORMAT_XRGB8888))
        t->pendingFormat = format;
}

void Capturer::onDmabufDevice(void *data, ext_image_copy_capture_session_v1 *, struct wl_array *device) {
    auto *t = static_cast<Toplevel *>(data);
    if (device->size >= sizeof(dev_t)) {
        dev_t dev;
        std::memcpy(&dev, device->data, sizeof(dev));
        t->owner->openGbm(dev);
    }
}

void Capturer::onDmabufFormat(void *data, ext_image_copy_capture_session_v1 *, uint32_t format, struct wl_array *modifiers) {
    auto *t = static_cast<Toplevel *>(data);
    if (format != GBM_FORMAT_XRGB8888 && format != GBM_FORMAT_ARGB8888)
        return;
    if (format == GBM_FORMAT_XRGB8888 || t->pendingDmabufFormat != GBM_FORMAT_XRGB8888) {
        t->pendingDmabufFormat = format;
        t->pendingModifiers.clear();
        const auto *mods = static_cast<const uint64_t *>(modifiers->data);
        for (size_t i = 0; i < modifiers->size / sizeof(uint64_t); ++i)
            if (mods[i] != 0x00ffffffffffffffULL) // DRM_FORMAT_MOD_INVALID
                t->pendingModifiers += mods[i];
    }
}

// The render node the compositor named, for allocating dmabufs. Opened once.
void Capturer::openGbm(dev_t device) {
    if (m_gbm || m_gbmTried)
        return;
    m_gbmTried = true;
    if (qEnvironmentVariableIsSet("OMISSION_SHM"))
        return;
    DIR *dir = opendir("/dev/dri");
    if (!dir)
        return;
    while (dirent *entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "renderD", 7) != 0)
            continue;
        const QByteArray path = QByteArray("/dev/dri/") + entry->d_name;
        struct stat st;
        if (stat(path.constData(), &st) != 0 || st.st_rdev != device)
            continue;
        m_gbmFd = ::open(path.constData(), O_RDWR | O_CLOEXEC);
        if (m_gbmFd >= 0)
            m_gbm = gbm_create_device(m_gbmFd);
        break;
    }
    closedir(dir);
    TRACE("omission: dmabuf capture %s", m_gbm ? "on" : "unavailable, using shared memory");
}

void Capturer::onSessionDone(void *data, ext_image_copy_capture_session_v1 *) {
    auto *t = static_cast<Toplevel *>(data);
    t->width = t->pendingWidth;
    t->height = t->pendingHeight;
    t->format = t->pendingFormat;
    t->dmabufFormat = t->pendingDmabufFormat;
    t->modifiers = t->pendingModifiers;
    t->pendingFormat = UINT32_MAX;
    t->pendingDmabufFormat = 0;
    t->pendingModifiers.clear();
    TRACE("omission: session %s %dx%d format %u", qPrintable(t->id), t->width, t->height, t->format);
    if (t->format == UINT32_MAX)
        TRACE("omission: %s offers no 32-bit shared-memory format", qPrintable(t->id));
    t->constraintsKnown = true;
    // A resized window needs a new buffer, but a frame may still be copying
    // into the old one; startFrame swaps it once that frame is done.
    t->owner->startFrame(t);
}

void Capturer::onStopped(void *data, ext_image_copy_capture_session_v1 *) {
    auto *t = static_cast<Toplevel *>(data);
    t->owner->dropSession(t);
}

void Capturer::startFrame(Toplevel *t) {
    if (!t->wanted || t->frame || t->processing || !t->session || !t->constraintsKnown)
        return;
    if ((t->format == UINT32_MAX && !(m_gbm && t->dmabufFormat)) || t->width <= 0 || t->height <= 0)
        return;

    // Live windows that change constantly (video) still only refresh at
    // about 30 Hz, which keeps the compositor's copies cheap.
    const qint64 since = m_clock.elapsed() - t->lastCapture;
    if (t->live && since < 33) {
        if (!t->retry) {
            t->retry = new QTimer(this);
            t->retry->setSingleShot(true);
            connect(t->retry, &QTimer::timeout, this, [this, t] { startFrame(t); });
        }
        t->retry->start(int(33 - since));
        return;
    }

    // The window was resized: the buffer no longer fits.
    if (t->buffer && (t->buffer->width != t->width || t->buffer->height != t->height))
        t->buffer.reset();
    if (!t->buffer)
        t->buffer = (m_gbm && m_dmabuf && t->dmabufFormat) ? allocateDmabuf(t) : nullptr;
    if (!t->buffer && t->format != UINT32_MAX)
        t->buffer = allocateShm(t);
    if (!t->buffer)
        return;

    static const ext_image_copy_capture_frame_v1_listener listener = {
        onTransform, onDamage, onPresentationTime, onReady, onFailed};
    t->frame = ext_image_copy_capture_session_v1_create_frame(t->session);
    ext_image_copy_capture_frame_v1_add_listener(t->frame, &listener, t);
    ext_image_copy_capture_frame_v1_attach_buffer(t->frame, t->buffer->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(t->frame, 0, 0, t->width, t->height);
    ext_image_copy_capture_frame_v1_capture(t->frame);
    TRACE("omission: %lld frame requested %s", m_clock.elapsed(), qPrintable(t->id));
    t->wanted = false;
}

std::shared_ptr<CaptureBuffer> Capturer::allocateShm(Toplevel *t) {
    auto buffer = std::make_shared<CaptureBuffer>();
    buffer->width = t->width;
    buffer->height = t->height;
    buffer->stride = t->width * 4;
    buffer->format = t->format;
    buffer->alpha = t->format == WL_SHM_FORMAT_ARGB8888;
    buffer->size = size_t(buffer->stride) * size_t(buffer->height);
    buffer->planes = 1;
    buffer->fds[0] = memfd_create("omission-capture", MFD_CLOEXEC);
    if (buffer->fds[0] < 0 || ftruncate(buffer->fds[0], off_t(buffer->size)) != 0)
        return nullptr;
    void *map = mmap(nullptr, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fds[0], 0);
    if (map == MAP_FAILED)
        return nullptr;
    buffer->data = static_cast<uchar *>(map);
    wl_shm_pool *pool = wl_shm_create_pool(m_shm, buffer->fds[0], int32_t(buffer->size));
    buffer->buffer = wl_shm_pool_create_buffer(pool, 0, buffer->width, buffer->height, buffer->stride, buffer->format);
    wl_shm_pool_destroy(pool);
    return buffer;
}

std::shared_ptr<CaptureBuffer> Capturer::allocateDmabuf(Toplevel *t) {
    auto buffer = std::make_shared<CaptureBuffer>();
    buffer->width = t->width;
    buffer->height = t->height;
    buffer->format = t->dmabufFormat;
    buffer->alpha = t->dmabufFormat == GBM_FORMAT_ARGB8888;
    buffer->dmabuf = true;
    // The GPU's own tiled layout when offered: it's the fastest for Hyprland
    // to render into and for us to sample from.
    QList<uint64_t> modifiers;
    for (uint64_t m : std::as_const(t->modifiers))
        if (m != 0 || qEnvironmentVariableIsSet("OMISSION_LINEAR"))
            modifiers += m;
    if (modifiers.isEmpty() && t->modifiers.contains(uint64_t(0)))
        modifiers += 0;
    if (modifiers.isEmpty())
        return nullptr;
    {
        std::lock_guard lock(gbmMutex);
        buffer->bo = gbm_bo_create_with_modifiers2(m_gbm, uint32_t(t->width), uint32_t(t->height), t->dmabufFormat,
                                                   modifiers.constData(), unsigned(modifiers.size()), GBM_BO_USE_RENDERING);
        if (!buffer->bo)
            return nullptr;
        buffer->planes = std::min(gbm_bo_get_plane_count(buffer->bo), 4);
        buffer->modifier = gbm_bo_get_modifier(buffer->bo);
        for (int i = 0; i < buffer->planes; ++i) {
            buffer->fds[i] = gbm_bo_get_fd_for_plane(buffer->bo, i);
            buffer->strides[i] = gbm_bo_get_stride_for_plane(buffer->bo, i);
            buffer->offsets[i] = gbm_bo_get_offset(buffer->bo, i);
        }
    }
    for (int i = 0; i < buffer->planes; ++i)
        if (buffer->fds[i] < 0)
            return nullptr;

    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(m_dmabuf);
    for (int i = 0; i < buffer->planes; ++i)
        zwp_linux_buffer_params_v1_add(params, buffer->fds[i], uint32_t(i), buffer->offsets[i], buffer->strides[i],
                                       uint32_t(buffer->modifier >> 32), uint32_t(buffer->modifier & 0xffffffff));
    buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params, t->width, t->height, t->dmabufFormat, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    return buffer;
}

void Capturer::onTransform(void *, ext_image_copy_capture_frame_v1 *, uint32_t) {}
void Capturer::onDamage(void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t, int32_t) {}
void Capturer::onPresentationTime(void *, ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t, uint32_t) {}

void Capturer::onReady(void *data, ext_image_copy_capture_frame_v1 *frame) {
    auto *t = static_cast<Toplevel *>(data);
    TRACE("omission: %lld ready %s", t->owner->m_clock.elapsed(), qPrintable(t->id));
    ext_image_copy_capture_frame_v1_destroy(frame);
    t->frame = nullptr;
    t->owner->finishFrame(t);
}

void Capturer::onFailed(void *data, ext_image_copy_capture_frame_v1 *frame, uint32_t reason) {
    auto *t = static_cast<Toplevel *>(data);
    TRACE("omission: failed %s reason %u", qPrintable(t->id), reason);
    ext_image_copy_capture_frame_v1_destroy(frame);
    t->frame = nullptr;
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
        t->owner->dropSession(t);
        return;
    }
    // Buffer constraints changed; the session sends new ones and a done
    // event, which starts the next frame.
    t->wanted = true;
    t->buffer.reset();
}

void Capturer::finishFrame(Toplevel *t) {
    t->lastCapture = m_clock.elapsed();
    if (!t->buffer) {
        startFrame(t);
        return;
    }
    if (t->buffer->dmabuf) {
        // Nothing to do on the CPU: the buffer itself goes to the render
        // thread, which imports and shrinks it on the GPU. The next capture
        // gets a fresh buffer.
        TRACE("omission: %lld handing over %s", m_clock.elapsed(), qPrintable(t->id));
        emit dmabufFrame(t->id, t->buffer);
        t->buffer.reset();
        if (t->live)
            t->wanted = true;
        startFrame(t);
        return;
    }
    t->processing = true;
    const quint64 serial = t->serial = ++m_serial;
    const QString id = t->id;
    const std::shared_ptr<CaptureBuffer> buffer = t->buffer;
    const int maxDimension = m_maxDimension;
    // Downscaling a 5-megapixel window takes a few milliseconds; spread the
    // windows over the pool so a whole overview lands at once.
    QThreadPool::globalInstance()->start([this, buffer, id, serial, maxDimension] {
        QImage image = downscale(buffer->data, buffer->width, buffer->height, buffer->stride, buffer->alpha, maxDimension);
        TRACE("omission: %lld scaled %s", m_clock.elapsed(), qPrintable(id));
        emit frame(id, image);
        QMetaObject::invokeMethod(this, [this, serial, id] { processed(serial, id); }, Qt::QueuedConnection);
    });
}

void Capturer::processed(quint64 serial, const QString &id) {
    Toplevel *t = m_byId.value(id);
    if (!t || t->serial != serial)
        return;
    t->processing = false;
    if (t->live)
        t->wanted = true;
    else if (!t->wanted) {
        t->buffer.reset();
        return;
    }
    startFrame(t);
}

void Capturer::dropSession(Toplevel *t) {
    if (t->retry)
        t->retry->stop();
    if (t->frame)
        ext_image_copy_capture_frame_v1_destroy(t->frame);
    t->frame = nullptr;
    if (t->session)
        ext_image_copy_capture_session_v1_destroy(t->session);
    t->session = nullptr;
    if (t->source)
        ext_image_capture_source_v1_destroy(t->source);
    t->source = nullptr;
    t->constraintsKnown = false;
    t->buffer.reset();
}

void Capturer::forget(Toplevel *t) {
    dropSession(t);
    delete t->retry;
    if (t->handle)
        ext_foreign_toplevel_handle_v1_destroy(t->handle);
    delete t;
}
