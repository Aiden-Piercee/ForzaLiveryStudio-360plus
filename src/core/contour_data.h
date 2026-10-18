#pragma once

#include <QColor>
#include <QPointF>
#include <QVector>

#include <optional>

namespace fls::scene {

enum class ContourPointKind { Hard, Soft };
enum class ContourPathKind { Pen, Lining };

inline constexpr double kDefaultLiningWidth = 8.0;
inline const QColor kDefaultLiningFillColor(Qt::black);
inline const QColor kDefaultPenFillColor(Qt::white);

struct ContourPoint {
    QPointF position;
    ContourPointKind kind = ContourPointKind::Soft;
    QPointF incoming;
    QPointF outgoing;
    bool explicitHandles = false;
    double strokeWidth = 0.0;
    bool operator==(const ContourPoint &) const = default;
};

struct ContourData {
    QVector<ContourPoint> points;
    QVector<QVector<ContourPoint>> cutouts;
    std::optional<QColor> fillColor;
    bool closed = false;
    bool cutoutClosed = true;
    bool fillMask = false;
    ContourPathKind pathKind = ContourPathKind::Pen;
    double strokeWidth = kDefaultLiningWidth;
    bool operator==(const ContourData &) const = default;
};

inline QColor contourFillColor(const ContourData &data) {
    if (data.fillColor.has_value() && data.fillColor->isValid()) {
        return *data.fillColor;
    }

    return data.pathKind == ContourPathKind::Lining ? kDefaultLiningFillColor : kDefaultPenFillColor;
}

} // namespace fls::scene
