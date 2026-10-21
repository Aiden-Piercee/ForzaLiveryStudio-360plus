#pragma once

#include "core_types.h"
#include "vinyl_decoder.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>

namespace fls {

// Forza Motorsport 4 (Xbox 360) livery design. Plaintext is big-endian, starts
// with "crlv" and holds six flat panels of 30-byte layer records.
inline constexpr int kFM4SectionCount = 6;
extern const LiverySlotDef kFM4LiverySlots[kFM4SectionCount];

struct FM4Layer {
    quint16 shapeId = 0;
    float spin = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float skew = 0.0f;
    quint32 argb = 0xffffffffu;
};

struct FM4Design {
    QByteArray head;
    std::array<QVector<FM4Layer>, kFM4SectionCount> panels;
    std::array<QByteArray, kFM4SectionCount> trailers;
    QByteArray tail;
    quint32 carId = 0;
};

struct FM4ExportReport {
    int written = 0;
    int skippedMasks = 0;
    int skippedRaster = 0;
    int skippedShapeIds = 0;
    QStringList notes;
};

bool isFM4DesignPlain(const QByteArray &plain);
bool isFM4ShapeId(quint16 shapeId);

FM4Design parseFM4Design(const QByteArray &plain);
QByteArray buildFM4Design(const FM4Design &design);
// An empty design for `carId`; `xuid` (8 bytes, optional) is written as the creator.
QByteArray emptyFM4Design(quint32 carId, const QByteArray &xuid = {});

Project importFM4Design(const QByteArray &plain, const QString &name = {});
// Writes the project's section groups back into FM4 plaintext. When the project
// came from an FM4 file its header and panel trailers are reused.
QByteArray exportFM4Design(const Project &project, FM4ExportReport *report = nullptr,
                           const QByteArray &xuid = {});

Project importFM4Livery(const QString &file, const QByteArray &defaultPe, const QByteArray &xuid,
                        bool *signatureOk = nullptr);
void exportFM4Livery(const Project &project, const QString &file, const QByteArray &defaultPe,
                     const QByteArray &xuid, FM4ExportReport *report = nullptr);

} // namespace fls
