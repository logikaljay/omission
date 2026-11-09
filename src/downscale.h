#pragma once

#include <QImage>

// Halves a 32-bit XRGB/ARGB image until it fits maxDimension, averaging each
// 2×2 block. It is several times faster than a smooth QImage::scaled and,
// for the exact halvings a HiDPI capture needs, just as clean. The result
// is always a copy, so the source may be shared memory that is reused.
QImage downscale(const uchar *pixels, int width, int height, int stride, bool alpha, int maxDimension);
