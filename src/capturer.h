#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThread>

#include <memory>

#include <sys/types.h>

struct wl_display;
struct wl_registry;
struct wl_shm;
struct wl_buffer;
struct ext_foreign_toplevel_list_v1;
struct ext_foreign_toplevel_handle_v1;
struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_image_capture_source_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_frame_v1;
struct zwp_linux_dmabuf_v1;
struct gbm_device;
class QSocketNotifier;
class QTimer;
struct gbm_bo;

// The memory one capture lands in: a memfd for wl_shm, or a dmabuf
// (possibly tiled, up to four planes) that the render thread imports on the
// GPU. Shared, because whoever reads it may outlive the window it came from.
struct CaptureBuffer {
    int width = 0, height = 0;
    bool alpha = false;
    bool dmabuf = false;

    // wl_shm
    uchar *data = nullptr;
    size_t size = 0;
    int stride = 0;
    uint32_t format = 0; // wl_shm format, or DRM fourcc for a dmabuf

    // dmabuf
    gbm_bo *bo = nullptr;
    int planes = 0;
    int fds[4] = {-1, -1, -1, -1};
    uint32_t strides[4] = {};
    uint32_t offsets[4] = {};
    uint64_t modifier = 0;

    wl_buffer *buffer = nullptr;

    CaptureBuffer() = default;
    CaptureBuffer(const CaptureBuffer &) = delete;
    CaptureBuffer &operator=(const CaptureBuffer &) = delete;
    ~CaptureBuffer();
};

Q_DECLARE_METATYPE(std::shared_ptr<CaptureBuffer>)

// Captures window contents through ext-image-copy-capture on a Wayland
// connection of its own, in a thread of its own, so the overview's frames
// never wait on the compositor. Windows are keyed by their
// ext-foreign-toplevel identifier, which Hyprland reports as "stableId".
//
// Every public slot is safe to call from the GUI thread; calls are queued.
class Capturer : public QObject {
    Q_OBJECT

public:
    explicit Capturer(int maxDimension = 1600);
    ~Capturer() override;

public slots:
    // Capture these windows now. While live, each keeps recapturing whenever
    // it changes (the protocol waits for damage), at most every 33 ms.
    void capture(const QStringList &ids, bool live);
    // Stop recapturing and give the shared memory back.
    void rest();
    // The render thread couldn't import a dmabuf; fall back to shared memory.
    void disableDmabuf();

signals:
    void frame(const QString &id, const QImage &image);
    void dmabufFrame(const QString &id, const std::shared_ptr<CaptureBuffer> &buffer);
    void failed(const QString &reason);

private:
    struct Toplevel;

    void start();
    void dispatch();
    void flush();
    void want(Toplevel *t);
    void ensureSession(Toplevel *t);
    void startFrame(Toplevel *t);
    void finishFrame(Toplevel *t);
    void dropSession(Toplevel *t);
    void forget(Toplevel *t);
    void processed(quint64 serial, const QString &id);
    void openGbm(dev_t device);
    std::shared_ptr<CaptureBuffer> allocateShm(Toplevel *t);
    std::shared_ptr<CaptureBuffer> allocateDmabuf(Toplevel *t);

    // Wayland listeners, which reach the instance through user data.
    static void onGlobal(void *, wl_registry *, uint32_t, const char *, uint32_t);
    static void onGlobalRemove(void *, wl_registry *, uint32_t);
    static void onToplevel(void *, ext_foreign_toplevel_list_v1 *, ext_foreign_toplevel_handle_v1 *);
    static void onListFinished(void *, ext_foreign_toplevel_list_v1 *);
    static void onClosed(void *, ext_foreign_toplevel_handle_v1 *);
    static void onDone(void *, ext_foreign_toplevel_handle_v1 *);
    static void onTitle(void *, ext_foreign_toplevel_handle_v1 *, const char *);
    static void onAppId(void *, ext_foreign_toplevel_handle_v1 *, const char *);
    static void onIdentifier(void *, ext_foreign_toplevel_handle_v1 *, const char *);
    static void onBufferSize(void *, ext_image_copy_capture_session_v1 *, uint32_t, uint32_t);
    static void onShmFormat(void *, ext_image_copy_capture_session_v1 *, uint32_t);
    static void onDmabufDevice(void *, ext_image_copy_capture_session_v1 *, struct wl_array *);
    static void onDmabufFormat(void *, ext_image_copy_capture_session_v1 *, uint32_t, struct wl_array *);
    static void onSessionDone(void *, ext_image_copy_capture_session_v1 *);
    static void onStopped(void *, ext_image_copy_capture_session_v1 *);
    static void onTransform(void *, ext_image_copy_capture_frame_v1 *, uint32_t);
    static void onDamage(void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t, int32_t);
    static void onPresentationTime(void *, ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t, uint32_t);
    static void onReady(void *, ext_image_copy_capture_frame_v1 *);
    static void onFailed(void *, ext_image_copy_capture_frame_v1 *, uint32_t);

    QThread m_thread;
    int m_maxDimension;
    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    wl_shm *m_shm = nullptr;
    ext_foreign_toplevel_list_v1 *m_list = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1 *m_sources = nullptr;
    ext_image_copy_capture_manager_v1 *m_copy = nullptr;
    zwp_linux_dmabuf_v1 *m_dmabuf = nullptr;
    gbm_device *m_gbm = nullptr;
    int m_gbmFd = -1;
    bool m_gbmTried = false;
    QSocketNotifier *m_notifier = nullptr;

    QHash<QString, Toplevel *> m_byId;
    QSet<Toplevel *> m_toplevels;
    QSet<QString> m_wanted; // requested before their toplevel was announced
    bool m_live = false;
    QElapsedTimer m_clock;
    quint64 m_serial = 0;
};
