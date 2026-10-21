#include "fm4_codec.h"

#include "fm4_crypto.h"
#include "layer.h"
#include "matrix_math.h"
#include "scene_codec.h"
#include "shape_registry.h"

#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace fls {
namespace {

constexpr int kHeadSize = 0x40;
constexpr int kLayerSize = 30;
constexpr int kTrailerSize = 15;
constexpr int kPanelHeaderSize = 8;
constexpr int kLengthFieldPos = 0x29;
constexpr int kCarIdPos = 0x10;
constexpr int kCreatorPos = 33;
constexpr int kCountsBack = 12;
constexpr quint32 kMaxLayersPerPanel = 100000;

// Plaintext of an FM4 design with no layers; the creator field (offset 33) is zeroed.
constexpr unsigned char kEmptyDesign[] = {
    0x63, 0x72, 0x6c, 0x76, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x84, 0x01, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x76, 0x72, 0x79, 0x00, 0x00, 0x00,
    0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8e, 0x6c, 0x76, 0x79,
    0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x43, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00,
    0x42, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x10, 0x26, 0x4f, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x10, 0x26,
    0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0xff, 0x10, 0x26, 0x4f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x01, 0xff, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x10, 0x26, 0x4f, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

quint32 beU32(const QByteArray &b, int pos) {
    return (static_cast<quint32>(static_cast<quint8>(b[pos])) << 24)
        | (static_cast<quint32>(static_cast<quint8>(b[pos + 1])) << 16)
        | (static_cast<quint32>(static_cast<quint8>(b[pos + 2])) << 8)
        | static_cast<quint32>(static_cast<quint8>(b[pos + 3]));
}

quint16 beU16(const QByteArray &b, int pos) {
    return static_cast<quint16>((static_cast<quint8>(b[pos]) << 8) | static_cast<quint8>(b[pos + 1]));
}

float beFloat(const QByteArray &b, int pos) {
    const quint32 v = beU32(b, pos);
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

void putBeU32(QByteArray &b, int pos, quint32 v) {
    b[pos] = static_cast<char>(v >> 24);
    b[pos + 1] = static_cast<char>(v >> 16);
    b[pos + 2] = static_cast<char>(v >> 8);
    b[pos + 3] = static_cast<char>(v);
}

void putLeU32(QByteArray &b, int pos, quint32 v) {
    b[pos] = static_cast<char>(v);
    b[pos + 1] = static_cast<char>(v >> 8);
    b[pos + 2] = static_cast<char>(v >> 16);
    b[pos + 3] = static_cast<char>(v >> 24);
}

void appendBeU32(QByteArray &b, quint32 v) {
    const int pos = b.size();
    b.resize(pos + 4);
    putBeU32(b, pos, v);
}

void appendBeU16(QByteArray &b, quint16 v) {
    b.append(static_cast<char>(v >> 8));
    b.append(static_cast<char>(v));
}

void appendBeFloat(QByteArray &b, float f) {
    quint32 v;
    std::memcpy(&v, &f, sizeof v);
    appendBeU32(b, v);
}

// Fork colours are BGRA; FM4 stores A, R, G, B.
std::array<quint8, 4> bgraFromArgb(quint32 argb) {
    return {static_cast<quint8>(argb), static_cast<quint8>(argb >> 8),
            static_cast<quint8>(argb >> 16), static_cast<quint8>(argb >> 24)};
}

quint32 argbFromBgra(const std::array<quint8, 4> &c) {
    return (static_cast<quint32>(c[3]) << 24) | (static_cast<quint32>(c[2]) << 16)
        | (static_cast<quint32>(c[1]) << 8) | static_cast<quint32>(c[0]);
}

void collectShapes(const scene::Layer &node, QVector<const scene::Shape *> &out) {
    if (node.kind() == scene::LayerKind::Shape) {
        out.push_back(static_cast<const scene::Shape *>(&node));
    } else if (node.kind() == scene::LayerKind::Group && node.visible) {
        for (const auto &child : static_cast<const scene::Group &>(node).children) {
            collectShapes(*child, out);
        }
    }
}

} // namespace

const LiverySlotDef kFM4LiverySlots[kFM4SectionCount] = {
    {"Front", 0.0}, {"Back", 0.0}, {"Top", 0.0},
    {"Left", 0.0},  {"Right", 0.0}, {"Spoiler", 0.0},
};

bool isFM4DesignPlain(const QByteArray &plain) {
    return plain.size() >= kHeadSize && plain.left(4) == QByteArray("crlv");
}

// FM4 vinyls are a_01..t_40 (101..2040) and u_01..u_80 (2101..2240); the id -> file table lives
// in the player's gamedb, so only the overall range is checked here. Horizon-only ids above it
// (e.g. 3001) have no FM4 equivalent.
bool isFM4ShapeId(quint16 shapeId) {
    const int index = shapeId % 100;
    return shapeId >= 101 && shapeId <= 2240 && index >= 1;
}

FM4Design parseFM4Design(const QByteArray &plain) {
    if (!isFM4DesignPlain(plain)) {
        throw std::runtime_error("not an FM4 livery design (missing crlv header)");
    }
    // Only crlv version 3 (FM4) has been checked against real data. Other Xbox 360 titles
    // (FM2, FM3, FH1, FH2) may use a different version or layout; refuse instead of misparsing.
    const quint32 version = beU32(plain, 4);
    if (version != 3) {
        throw std::runtime_error(QStringLiteral("unsupported crlv version %1: only FM4 (version 3) is verified. "
                                                "FM2, FM3 and Horizon 360 saves need sample files first")
                                     .arg(version).toStdString());
    }
    FM4Design design;
    design.head = plain.left(kHeadSize);
    design.carId = beU32(plain, kCarIdPos);
    int pos = kHeadSize;
    for (int panel = 0; panel < kFM4SectionCount; ++panel) {
        if (pos + kPanelHeaderSize > plain.size()) {
            throw std::runtime_error("FM4 livery is truncated (panel header)");
        }
        const quint32 count = beU32(plain, pos);
        pos += kPanelHeaderSize;
        if (count > kMaxLayersPerPanel) {
            throw std::runtime_error("FM4 livery has an implausible layer count");
        }
        if (count > 0) {
            ++pos; // padding byte before the first record
            if (pos + static_cast<qint64>(count) * kLayerSize > plain.size()) {
                throw std::runtime_error("FM4 livery is truncated (layers)");
            }
            design.panels[panel].reserve(static_cast<qsizetype>(count));
            for (quint32 i = 0; i < count; ++i, pos += kLayerSize) {
                FM4Layer layer;
                layer.shapeId = beU16(plain, pos);
                layer.spin = beFloat(plain, pos + 2);
                layer.x = beFloat(plain, pos + 6);
                layer.y = beFloat(plain, pos + 10);
                layer.scaleX = beFloat(plain, pos + 14);
                layer.scaleY = beFloat(plain, pos + 18);
                layer.skew = beFloat(plain, pos + 22);
                layer.argb = beU32(plain, pos + 26);
                design.panels[panel].push_back(layer);
            }
        }
        if (pos + kTrailerSize > plain.size()) {
            throw std::runtime_error("FM4 livery is truncated (panel trailer)");
        }
        design.trailers[panel] = plain.mid(pos, kTrailerSize);
        pos += kTrailerSize;
    }
    design.tail = plain.mid(pos);
    if (design.tail.size() < kCountsBack) {
        throw std::runtime_error("FM4 livery is truncated (tail)");
    }
    return design;
}

QByteArray buildFM4Design(const FM4Design &design) {
    QByteArray out = design.head;
    if (out.size() != kHeadSize) {
        throw std::runtime_error("FM4 design header must be 64 bytes");
    }
    qsizetype total = 0;
    int nonEmpty = 0;
    for (const auto &panel : design.panels) {
        total += panel.size();
        nonEmpty += panel.isEmpty() ? 0 : 1;
    }
    putBeU32(out, kLengthFieldPos, static_cast<quint32>(142 + kLayerSize * total + nonEmpty));
    for (int p = 0; p < kFM4SectionCount; ++p) {
        const auto &layers = design.panels[p];
        appendBeU32(out, static_cast<quint32>(layers.size()));
        appendBeU32(out, layers.isEmpty() ? 0u : 1u);
        if (!layers.isEmpty()) {
            out.append('\0');
            for (const FM4Layer &l : layers) {
                appendBeU16(out, l.shapeId);
                appendBeFloat(out, l.spin);
                appendBeFloat(out, l.x);
                appendBeFloat(out, l.y);
                appendBeFloat(out, l.scaleX);
                appendBeFloat(out, l.scaleY);
                appendBeFloat(out, l.skew);
                appendBeU32(out, l.argb);
            }
        }
        out.append(design.trailers[p].leftJustified(kTrailerSize, '\0', true));
    }
    // Per-panel layer counts: six little-endian u32 ending 12 bytes into the tail.
    const int panelsEnd = out.size();
    out.append(design.tail);
    for (int p = 0; p < kFM4SectionCount; ++p) {
        putLeU32(out, panelsEnd - kCountsBack + 4 * p, static_cast<quint32>(design.panels[p].size()));
    }
    return out;
}

QByteArray emptyFM4Design(quint32 carId, const QByteArray &xuid) {
    QByteArray raw(reinterpret_cast<const char *>(kEmptyDesign), static_cast<qsizetype>(sizeof kEmptyDesign));
    if (xuid.size() == 8) {
        raw.replace(kCreatorPos, 8, xuid);
    }
    FM4Design design = parseFM4Design(raw);
    putBeU32(design.head, kCarIdPos, carId);
    return buildFM4Design(design);
}

Project importFM4Design(const QByteArray &plain, const QString &name) {
    const FM4Design design = parseFM4Design(plain);

    Project project;
    if (!name.isEmpty()) {
        project.name = name;
    }
    project.isLivery = true;
    project.carId = static_cast<int>(design.carId);
    project.liverySource = plain;
    scene::ensureProjectSceneRoot(project);

    int shapeIndex = 0;
    for (int panel = 0; panel < kFM4SectionCount; ++panel) {
        auto section = std::make_unique<scene::Group>();
        section->id = QStringLiteral("group-%1").arg(panel + 1, 4, 10, QLatin1Char('0'));
        section->name = QString::fromLatin1(kFM4LiverySlots[panel].name);
        section->isLiverySection = true;
        section->liverySectionSlot = panel;
        for (const FM4Layer &l : design.panels[panel]) {
            ++shapeIndex;
            const quint16 shapeId = detail::canonicalShapeId(l.shapeId);
            auto s = std::make_unique<scene::Shape>();
            s->id = QStringLiteral("layer-%1").arg(shapeIndex, 4, 10, QLatin1Char('0'));
            s->name = QStringLiteral("%1 %2")
                .arg(shapeIndex, 4, 10, QLatin1Char('0'))
                .arg(detail::shapeName(shapeId));
            s->transform.x = l.x;
            s->transform.y = l.y;
            s->transform.scaleX = l.scaleX;
            s->transform.scaleY = l.scaleY;
            s->transform.rotation = l.spin;
            s->transform.skew = l.skew;
            s->hasSourceTransform = true;
            s->sourceTransform = s->transform;
            s->setVectorShape(shapeId);
            s->color = bgraFromArgb(l.argb);
            s->opacity = static_cast<double>(s->color[3]) / 255.0;
            s->sourceShape = shapeIndex;
            section->append(std::move(s));
        }
        for (const auto &child : section->children) {
            section->sourceChildren.push_back(child->id);
        }
        project.root->append(std::move(section));
    }
    return project;
}

QByteArray exportFM4Design(const Project &project, FM4ExportReport *report, const QByteArray &xuid) {
    if (!project.isLivery || project.root == nullptr) {
        throw std::runtime_error("only livery projects can be exported as an FM4 livery");
    }
    FM4Design design;
    if (isFM4DesignPlain(project.liverySource)) {
        design = parseFM4Design(project.liverySource);
    } else {
        design = parseFM4Design(emptyFM4Design(static_cast<quint32>(project.carId), xuid));
    }
    putBeU32(design.head, kCarIdPos, static_cast<quint32>(project.carId));
    for (auto &panel : design.panels) {
        panel.clear();
    }

    FM4ExportReport local;
    FM4ExportReport &rep = report != nullptr ? *report : local;
    rep = FM4ExportReport{};

    for (const auto &child : project.root->children) {
        if (child->kind() != scene::LayerKind::Group) {
            continue;
        }
        const auto &section = static_cast<const scene::Group &>(*child);
        const int slot = section.liverySectionSlot;
        if (!section.isLiverySection || slot < 0) {
            continue;
        }
        if (slot >= kFM4SectionCount) {
            if (!section.children.empty()) {
                rep.notes << QStringLiteral("Section \"%1\" does not exist in FM4 and was dropped.").arg(section.name);
            }
            continue;
        }
        QVector<const scene::Shape *> shapes;
        collectShapes(section, shapes);
        for (const scene::Shape *shape : shapes) {
            if (!shape->visible) {
                continue;
            }
            if (shape->raster) {
                ++rep.skippedRaster;
                continue;
            }
            if (shape->mask) {
                ++rep.skippedMasks;
                continue;
            }
            if (!isFM4ShapeId(shape->shapeId)) {
                ++rep.skippedShapeIds;
                continue;
            }
            const scene::Transform2D t = decomposeTransform2D(shape->worldMatrix());
            FM4Layer layer;
            layer.shapeId = shape->shapeId;
            layer.spin = static_cast<float>(t.rotation);
            layer.x = static_cast<float>(t.x);
            layer.y = static_cast<float>(t.y);
            layer.scaleX = static_cast<float>(t.scaleX);
            layer.scaleY = static_cast<float>(t.scaleY);
            layer.skew = static_cast<float>(t.skew);
            layer.argb = argbFromBgra(shape->color);
            design.panels[slot].push_back(layer);
            ++rep.written;
        }
    }
    if (rep.skippedMasks > 0) {
        rep.notes << QStringLiteral("%1 masked layer(s) skipped: FM4 has no masks.").arg(rep.skippedMasks);
    }
    if (rep.skippedRaster > 0) {
        rep.notes << QStringLiteral("%1 logo/raster layer(s) skipped: FM4 liveries only hold vinyl shapes.").arg(rep.skippedRaster);
    }
    if (rep.skippedShapeIds > 0) {
        rep.notes << QStringLiteral("%1 layer(s) skipped: shape not in the FM4 vinyl set.").arg(rep.skippedShapeIds);
    }
    return buildFM4Design(design);
}

Project importFM4Livery(const QString &file, const QByteArray &defaultPe, const QByteArray &xuid,
                        bool *signatureOk) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open: " + file).toStdString());
    }
    const fm4::DecryptedLiv liv = fm4::decryptLiv(f.readAll(), defaultPe, xuid);
    if (liv.kind != fm4::ContainerKind::Design) {
        throw std::runtime_error("this is an FM4 vinyl group (lvyg), not a livery design; group import is not supported yet");
    }
    if (signatureOk != nullptr) {
        *signatureOk = liv.signatureOk;
    }
    Project project = importFM4Design(liv.plain, QFileInfo(file).completeBaseName());
    project.sourceFolder = QFileInfo(file).absolutePath();
    return project;
}

void exportFM4Livery(const Project &project, const QString &file, const QByteArray &defaultPe,
                     const QByteArray &xuid, FM4ExportReport *report) {
    const QByteArray plain = exportFM4Design(project, report, xuid);
    const QByteArray encrypted = fm4::encryptLiv(plain, defaultPe, xuid, fm4::ContainerKind::Design);
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(encrypted) != encrypted.size()) {
        throw std::runtime_error(("could not write: " + file).toStdString());
    }
}

} // namespace fls
