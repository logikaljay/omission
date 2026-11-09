#include "downscale.h"

QImage downscale(const uchar *pixels, int width, int height, int stride, bool alpha, int maxDimension) {
    const QImage::Format format = alpha ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
    if (!pixels || width <= 0 || height <= 0)
        return {};

    QImage current;
    const uchar *src = pixels;
    int srcStride = stride;
    while ((width > maxDimension || height > maxDimension) && width >= 2 && height >= 2) {
        const int w = width / 2, h = height / 2;
        QImage half(w, h, format);
        for (int y = 0; y < h; ++y) {
            const auto *a = reinterpret_cast<const quint32 *>(src + size_t(2 * y) * srcStride);
            const auto *b = reinterpret_cast<const quint32 *>(src + size_t(2 * y + 1) * srcStride);
            auto *out = reinterpret_cast<quint32 *>(half.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const quint32 p0 = a[2 * x], p1 = a[2 * x + 1], p2 = b[2 * x], p3 = b[2 * x + 1];
                const quint32 rb = (((p0 & 0x00ff00ff) + (p1 & 0x00ff00ff) + (p2 & 0x00ff00ff) + (p3 & 0x00ff00ff) + 0x00020002) >> 2) & 0x00ff00ff;
                const quint32 ag = ((((p0 >> 8) & 0x00ff00ff) + ((p1 >> 8) & 0x00ff00ff) + ((p2 >> 8) & 0x00ff00ff) + ((p3 >> 8) & 0x00ff00ff) + 0x00020002) >> 2) & 0x00ff00ff;
                out[x] = rb | (ag << 8);
            }
        }
        current = half;
        src = current.constBits();
        srcStride = int(current.bytesPerLine());
        width = w;
        height = h;
    }
    if (current.isNull()) // Already small enough: copy out of shared memory.
        current = QImage(pixels, width, height, stride, format).copy();
    // XRGB leaves the padding byte undefined, but Format_RGB32 promises 0xff.
    if (!alpha)
        for (int y = 0; y < current.height(); ++y) {
            auto *line = reinterpret_cast<quint32 *>(current.scanLine(y));
            for (int x = 0; x < current.width(); ++x)
                line[x] |= 0xff000000;
        }
    return current;
}
