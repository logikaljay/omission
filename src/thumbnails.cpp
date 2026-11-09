#include "thumbnails.h"

#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>
#include <QtQuick/qsgtexture_platform.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

// From GL_OES_EGL_image; spelled out because gl2ext.h clashes with Qt's GL.
typedef void (*ImageTargetRenderbufferStorage)(GLenum target, void *image);

#include "capturer.h"

#include <poll.h>

#include <algorithm>
#include <cmath>

Thumbnails *Thumbnails::instance() {
    static Thumbnails store;
    return &store;
}

void Thumbnails::set(const QString &id, const QImage &image) {
    if (image.isNull())
        return;
    Entry &e = m_entries[id];
    e.image = image;
    e.dmabuf.reset();
    e.version = ++m_version;
    emit updated(id);
}

void Thumbnails::setDmabuf(const QString &id, const std::shared_ptr<CaptureBuffer> &buffer) {
    Entry &e = m_entries[id];
    e.image = QImage();
    e.dmabuf = buffer;
    e.version = ++m_version;
    emit updated(id);
}

void Thumbnails::keepOnly(const QSet<QString> &ids) {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (ids.contains(it.key())) {
            ++it;
        } else {
            m_forgotten += it.key();
            it = m_entries.erase(it);
        }
    }
}

QStringList Thumbnails::takeForgotten() {
    QStringList forgotten;
    forgotten.swap(m_forgotten);
    return forgotten;
}

namespace {

// Textures live on the render thread, one per window, for as long as the
// window does (or until the scene graph loses its context).
struct GpuThumb {
    GLuint texture = 0;
    QSize size;
    quint64 version = 0;
    quint64 serial = 0; // bumps whenever the texture object is replaced
};
QHash<QString, GpuThumb> gpuThumbs;
quint64 gpuSerial = 0;
constexpr int maxDimension = 1600;

QSize shrunk(int width, int height) {
    while (width > maxDimension || height > maxDimension) {
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
    }
    return {width, height};
}

int levels(QSize size) {
    return 1 + int(std::floor(std::log2(std::max(size.width(), size.height()))));
}

// Raw GL inside Qt Quick's frame: put back every binding we touch.
struct SavedState {
    GLint readFbo = 0, drawFbo = 0, renderbuffer = 0, texture = 0;
    GLboolean scissor = GL_FALSE;
    explicit SavedState(QOpenGLExtraFunctions *gl) : gl(gl) {
        gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        gl->glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        scissor = gl->glIsEnabled(GL_SCISSOR_TEST);
        gl->glDisable(GL_SCISSOR_TEST);
    }
    ~SavedState() {
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFbo));
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
        gl->glBindRenderbuffer(GL_RENDERBUFFER, GLuint(renderbuffer));
        gl->glBindTexture(GL_TEXTURE_2D, GLuint(texture));
        if (scissor)
            gl->glEnable(GL_SCISSOR_TEST);
    }
    QOpenGLExtraFunctions *gl;
};

// (Re)creates the window's texture at `size`, with a full mip chain.
void ensureTexture(QOpenGLExtraFunctions *gl, GpuThumb &thumb, QSize size) {
    if (thumb.texture && thumb.size == size)
        return;
    if (thumb.texture)
        gl->glDeleteTextures(1, &thumb.texture);
    gl->glGenTextures(1, &thumb.texture);
    gl->glBindTexture(GL_TEXTURE_2D, thumb.texture);
    gl->glTexStorage2D(GL_TEXTURE_2D, levels(size), GL_RGBA8, size.width(), size.height());
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    thumb.size = size;
    thumb.serial = ++gpuSerial;
}

// Imports a dmabuf as an EGLImage-backed renderbuffer and blits it, halved
// with linear filtering (a 2×2 box), into the window's texture. Returns
// false if this driver can't import it.
bool importDmabuf(QOpenGLExtraFunctions *gl, GpuThumb &thumb, const CaptureBuffer &buffer) {
    static auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    static auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    static auto imageStorage = reinterpret_cast<ImageTargetRenderbufferStorage>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    const EGLDisplay display = eglGetCurrentDisplay();
    if (!createImage || !destroyImage || !imageStorage || display == EGL_NO_DISPLAY)
        return false;

    static const EGLint planeAttribs[4][5] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
         EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
         EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
         EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
         EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
    };
    QList<EGLint> attribs{EGL_WIDTH, buffer.width, EGL_HEIGHT, buffer.height,
                          EGL_LINUX_DRM_FOURCC_EXT, EGLint(buffer.format)};
    for (int i = 0; i < buffer.planes; ++i)
        attribs << planeAttribs[i][0] << buffer.fds[i] << planeAttribs[i][1] << EGLint(buffer.offsets[i])
                << planeAttribs[i][2] << EGLint(buffer.strides[i])
                << planeAttribs[i][3] << EGLint(buffer.modifier & 0xffffffff)
                << planeAttribs[i][4] << EGLint(buffer.modifier >> 32);
    attribs << EGL_NONE;
    EGLImageKHR image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.constData());
    if (image == EGL_NO_IMAGE_KHR)
        return false;

    SavedState saved(gl);
    GLuint source = 0, fbos[2] = {};
    gl->glGenRenderbuffers(1, &source);
    gl->glBindRenderbuffer(GL_RENDERBUFFER, source);
    imageStorage(GL_RENDERBUFFER, image);
    gl->glGenFramebuffers(2, fbos);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[0]);
    gl->glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, source);

    const QSize size = shrunk(buffer.width, buffer.height);
    ensureTexture(gl, thumb, size);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[1]);
    gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, thumb.texture, 0);

    const bool complete = gl->glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
                          && gl->glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        gl->glBlitFramebuffer(0, 0, buffer.width, buffer.height, 0, 0, size.width(), size.height(),
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
        gl->glBindTexture(GL_TEXTURE_2D, thumb.texture);
        gl->glGenerateMipmap(GL_TEXTURE_2D);
    }

    gl->glDeleteFramebuffers(2, fbos);
    gl->glDeleteRenderbuffers(1, &source);
    destroyImage(display, image);
    return complete;
}

// Whether every write to the dmabuf has landed (its implicit fences have
// signalled), without waiting.
bool bufferReady(const CaptureBuffer &buffer) {
    pollfd fd{buffer.fds[0], POLLIN, 0};
    return buffer.fds[0] < 0 || (::poll(&fd, 1, 0) == 1 && (fd.revents & POLLIN));
}

void uploadImage(QOpenGLExtraFunctions *gl, GpuThumb &thumb, const QImage &image) {
    SavedState saved(gl);
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    ensureTexture(gl, thumb, rgba.size());
    gl->glBindTexture(GL_TEXTURE_2D, thumb.texture);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rgba.width(), rgba.height(), GL_RGBA, GL_UNSIGNED_BYTE, rgba.constBits());
    gl->glGenerateMipmap(GL_TEXTURE_2D);
}

} // namespace

WindowThumb::WindowThumb(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    connect(Thumbnails::instance(), &Thumbnails::updated, this, [this](const QString &id) {
        if (id == m_id)
            refresh();
    });
    // The scene graph's context went away, and every texture with it.
    connect(this, &QQuickItem::windowChanged, this, [](QQuickWindow *window) {
        if (window)
            connect(window, &QQuickWindow::sceneGraphInvalidated, window, [] { gpuThumbs.clear(); }, Qt::DirectConnection);
    });
}

void WindowThumb::setWindowId(const QString &id) {
    if (id == m_id)
        return;
    m_id = id;
    emit windowIdChanged();
    refresh();
}

void WindowThumb::refresh() {
    const bool ready = Thumbnails::instance()->has(m_id);
    if (ready != m_ready) {
        m_ready = ready;
        emit readyChanged();
    }
    update();
}

QSGNode *WindowThumb::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = static_cast<QSGImageNode *>(old);
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context) {
        delete node;
        return nullptr;
    }
    QOpenGLExtraFunctions *gl = context->extraFunctions();

    Thumbnails *store = Thumbnails::instance();
    for (const QString &id : store->takeForgotten()) {
        auto it = gpuThumbs.find(id);
        if (it != gpuThumbs.end()) {
            gl->glDeleteTextures(1, &it->texture);
            gpuThumbs.erase(it);
        }
    }

    Thumbnails::Entry *entry = store->entry(m_id);
    GpuThumb &thumb = gpuThumbs[m_id];
    // Importing a buffer the compositor is still rendering into would make
    // this frame wait for it on the GPU; keep the old picture and look
    // again next frame instead.
    if (entry && entry->dmabuf && thumb.texture && !bufferReady(*entry->dmabuf)) {
        QMetaObject::invokeMethod(this, &QQuickItem::update, Qt::QueuedConnection);
    } else if (entry && entry->version != thumb.version) {
        bool ok = true;
        if (entry->dmabuf) {
            static const bool debug = qEnvironmentVariableIsSet("OMISSION_DEBUG");
            QElapsedTimer timer;
            timer.start();
            ok = importDmabuf(gl, thumb, *entry->dmabuf);
            if (debug)
                qInfo("omission: imported %s in %.2f ms", qPrintable(m_id), timer.nsecsElapsed() / 1e6);
            if (!ok)
                QMetaObject::invokeMethod(store, [store, id = m_id] { emit store->importFailed(id); }, Qt::QueuedConnection);
            // The texture holds the picture now; let the buffer go.
            entry->dmabuf.reset();
        } else if (!entry->image.isNull()) {
            uploadImage(gl, thumb, entry->image);
        }
        if (ok)
            thumb.version = entry->version;
    }

    if (!thumb.texture || width() <= 0 || height() <= 0) {
        delete node;
        return nullptr;
    }
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        node->setMipmapFiltering(QSGTexture::Linear);
        m_shownTexture = 0;
    }
    if (m_shownTexture != thumb.serial) {
        m_shownTexture = thumb.serial;
        QSGTexture *texture = QNativeInterface::QSGOpenGLTexture::fromNative(thumb.texture, window(), thumb.size,
                                                                            QQuickWindow::TextureHasMipmaps);
        texture->setMipmapFiltering(QSGTexture::Linear);
        node->setTexture(texture);
    }
    // Same texture object, new contents: the node must redraw.
    node->markDirty(QSGNode::DirtyMaterial);
    node->setRect(boundingRect());
    return node;
}
