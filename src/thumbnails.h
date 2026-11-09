#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QQuickItem>
#include <QSet>

#include <memory>

struct CaptureBuffer;

// The latest picture of every window, kept between openings so the
// overview has something to show in its very first frame.
//
// A picture arrives either as a QImage (shared-memory capture) or as a
// dmabuf straight from the compositor. Dmabufs never touch the CPU: the
// render thread imports them, shrinks them into a mipmapped texture and
// lets the buffer go. Those textures are cached by window, on the render
// thread, for as long as the window exists.
class Thumbnails : public QObject {
    Q_OBJECT

public:
    struct Entry {
        QImage image;
        std::shared_ptr<CaptureBuffer> dmabuf;
        quint64 version = 0;
    };

    static Thumbnails *instance();

    bool has(const QString &id) const { return m_entries.contains(id); }
    // Read and changed by the render thread only while the GUI thread is
    // blocked in the scene graph's sync phase.
    Entry *entry(const QString &id) { auto it = m_entries.find(id); return it == m_entries.end() ? nullptr : &it.value(); }

    void set(const QString &id, const QImage &image);
    void setDmabuf(const QString &id, const std::shared_ptr<CaptureBuffer> &buffer);
    // Drop pictures of windows that no longer exist.
    void keepOnly(const QSet<QString> &ids);
    // Windows dropped since the render thread last looked, so it can free
    // their textures.
    QStringList takeForgotten();

signals:
    void updated(const QString &id);
    // The render thread couldn't import a dmabuf from this window.
    void importFailed(const QString &id);

private:
    QHash<QString, Entry> m_entries;
    QStringList m_forgotten;
    quint64 m_version = 0;
};

// Draws one window's picture as a mipmapped texture, uploaded or imported
// once per new capture and shared by every item showing that window.
class WindowThumb : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString windowId READ windowId WRITE setWindowId NOTIFY windowIdChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit WindowThumb(QQuickItem *parent = nullptr);

    QString windowId() const { return m_id; }
    void setWindowId(const QString &id);
    bool ready() const { return m_ready; }

signals:
    void windowIdChanged();
    void readyChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    void refresh();

    QString m_id;
    bool m_ready = false;
    quint64 m_shownTexture = 0;
};
