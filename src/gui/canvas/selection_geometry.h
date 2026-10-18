#pragma once

#include <QImage>
#include <QPainterPath>

namespace gui {

struct ShapeGeometry;

QPainterPath vectorSelectionBody(const ShapeGeometry &geometry);
QPainterPath rasterSelectionBody(const QImage &image);

} // namespace gui
