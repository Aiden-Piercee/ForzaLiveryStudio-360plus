#include "game_export.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "layer.h"
#include "livery_codec.h"
#include "project_codec.h"

#include <QDir>
#include <QFile>
#include <QUuid>

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace fls {
namespace {

constexpr int kHorizon5Sections = 5;
constexpr int kMotorsportSections = 7;
constexpr int kLegacyCounters = 7;
constexpr int kPaintRecordCount = 34;
constexpr int kPaintRecordSize = 40;
constexpr int kPaintBlockSize = 4 + 1 + kPaintRecordCount * kPaintRecordSize + 2;
constexpr int kMotorsportBlockSize = 32768;
constexpr int kCreatorTagSize = 8;
constexpr quint32 kDefaultMotorsportCategory = 11;
constexpr quint32 kDefaultMotorsportPayloadFlags = 2;
constexpr int kMotorsportArtworkOffset = 66;
constexpr int kMotorsportEntrySize = 7;
constexpr int kMotorsportEntryCountOffset = 58;
constexpr int kMotorsportEntriesOffset = 62;

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(bytes) != bytes.size()) {
        throw std::runtime_error(("could not write export file: " + path).toStdString());
    }
}

HeaderMetadata projectHeader(const Project &project) {
    if (project.headerMetadata) {
        return *project.headerMetadata;
    }
    if (!project.sourceHeader.isEmpty()) {
        return parseHeader(project.sourceHeader);
    }

    return defaultDraftHeader(project.name, QString(), static_cast<quint32>(project.carId));
}

QByteArray destinationCreatorTag(const Project &project, const QString &outputFolder) {
    QByteArray tag = creatorTagFromSavePath(outputFolder);
    if (tag.isEmpty()) {
        tag = projectHeader(project).creatorTag.left(kCreatorTagSize);
        tag.append(QByteArray(kCreatorTagSize - tag.size(), '\0'));
    }

    return tag;
}

QByteArray nativeGuidBytes(const QUuid &guid) {
    QByteArray bytes;
    detail::appendLeU32(bytes, guid.data1);
    detail::appendLeU16(bytes, guid.data2);
    detail::appendLeU16(bytes, guid.data3);
    bytes.append(reinterpret_cast<const char *>(guid.data4), 8);

    return bytes;
}

QByteArray nativeHeader(const Project &project, ExportGameVersion game,
                       quint32 shapeCount, const QByteArray &creatorTag,
                       const QString &outputFolder) {
    HeaderMetadata metadata = projectHeader(project);
    const QUuid folderId(QDir(outputFolder).dirName());
    const quint32 sourceVersion = metadata.formatVersion;
    const bool motorsport = game == ExportGameVersion::Motorsport2023;

    metadata.formatVersion = motorsport ? 9 : 4;
    metadata.name = project.name;
    metadata.published = false;
    metadata.description.clear();
    metadata.publishedTail.clear();
    metadata.trailing.clear();
    metadata.typeValue = shapeCount;
    metadata.carId = project.isLivery ? static_cast<quint32>(project.carId) : 0;
    metadata.hasCarId = motorsport || project.isLivery;
    metadata.creatorTag = creatorTag;
    if (metadata.guid.size() != 16 || metadata.formatVersion != sourceVersion) {
        metadata.guid = nativeGuidBytes(QUuid::createUuid());
    }
    if (motorsport && !folderId.isNull()) {
        metadata.guid = nativeGuidBytes(folderId);
    }
    metadata.sectionPrefix = QByteArray(28, '\0');
    metadata.sectionPrefix[0] = '\x01';
    metadata.sectionPrefix.replace(12, 16, metadata.guid);
    metadata.sectionMarker = motorsport ? QByteArray("\x01\x02", 2) : QByteArray("\x01\x01", 2);
    metadata.sectionPadding = QByteArray(motorsport ? 7 : 3, '\0');
    if (motorsport) {
        metadata.sectionPadding[3] = '\x01';
        metadata.category = metadata.category.value_or(kDefaultMotorsportCategory);
    } else {
        metadata.category.reset();
    }

    return buildHeader(metadata);
}

QByteArray legacyPaintBlock(const Project &project) {
    if (!project.liveryPaint.materials.isEmpty()) {
        throw std::runtime_error(
            "FH5/FM23 export cannot map edited paint bindings yet. "
            "Export a project with unchanged native paint or without paint overrides.");
    }
    const QByteArray &source = project.liverySource;
    const int artwork = source.indexOf(QByteArray("gyvl", 4));
    if (artwork >= 4) {
        const quint32 artworkSize = detail::readLeU32(source, artwork - 4);
        const qint64 stats = static_cast<qint64>(artwork) + artworkSize;
        const qint64 paint = stats + 4 + kLegacyCounters * 4;
        if (stats >= artwork && paint + kPaintBlockSize + 4 <= source.size()
            && source.mid(stats, 4) == QByteArray("yrvl", 4)
            && source.mid(paint, 4) == QByteArray("yrvl", 4)
            && source.mid(paint + kPaintBlockSize, 4) == QByteArray("yrvl", 4)) {
            bool validRecords = true;
            for (int index = 0; index < kPaintRecordCount; ++index) {
                const int offset = static_cast<int>(paint) + 5 + index * kPaintRecordSize;
                validRecords = validRecords && detail::bytesAt(source, offset, {1, 1});
            }
            if (validRecords) {
                return source.mid(paint, kPaintBlockSize);
            }
        }
        if (detail::readLeU32(source, 4) == 1 || artwork >= 66) {
            throw std::runtime_error("the native paint block could not be preserved");
        }
    }

    QByteArray paint("yrvl", 4);
    paint.append('\0');
    for (int index = 0; index < kPaintRecordCount; ++index) {
        QByteArray record(kPaintRecordSize, '\0');
        record[0] = '\x01';
        record[1] = '\x01';
        record.replace(27, 4, QByteArray(4, '\xff'));
        paint.append(record);
    }
    paint.append("\x00\x01", 2);

    return paint;
}

void prepareNativeArtworkLayers(scene::Group &group) {
    auto &children = group.children;
    children.erase(std::remove_if(children.begin(), children.end(), [](const auto &child) {
        return !child->visible;
    }), children.end());
    for (const auto &child : children) {
        if (child->kind() == scene::LayerKind::Group) {
            prepareNativeArtworkLayers(static_cast<scene::Group &>(*child));
        } else if (child->kind() == scene::LayerKind::Shape) {
            static_cast<scene::Shape &>(*child).marker.clear();
        }
    }
}

bool containsArtwork(const scene::Layer &layer) {
    if (layer.kind() == scene::LayerKind::Shape) {
        return true;
    }
    if (layer.kind() == scene::LayerKind::Group) {
        const auto &group = static_cast<const scene::Group &>(layer);

        return std::any_of(group.children.begin(), group.children.end(), [](const auto &child) {
            return containsArtwork(*child);
        });
    }

    return false;
}

Project nativeArtworkProject(const Project &project) {
    Project artwork = project;
    std::array<bool, 11> sections{};
    if (artwork.root == nullptr) {
        return artwork;
    }
    prepareNativeArtworkLayers(*artwork.root);
    for (const auto &child : artwork.root->children) {
        if (!containsArtwork(*child)) {
            continue;
        }
        if (child->kind() != scene::LayerKind::Group) {
            throw std::runtime_error("livery artwork must belong to a section");
        }
        const auto &group = static_cast<const scene::Group &>(*child);
        const int slot = group.liverySectionSlot;
        if (!group.isLiverySection || slot < 0 || slot >= static_cast<int>(sections.size())
            || sections[slot]) {
            throw std::runtime_error("livery artwork has invalid or duplicate section metadata");
        }
        sections[slot] = true;
    }

    return artwork;
}

QByteArray nativeLiveryPayload(const Project &project, ExportGameVersion game,
                              const QByteArray &creatorTag,
                              std::array<int, 11> &counts) {
    if (!project.isLivery) {
        throw std::runtime_error("not a livery project");
    }
    const bool motorsport = game == ExportGameVersion::Motorsport2023;
    const QByteArray paint = legacyPaintBlock(project);
    const Project artworkProject = nativeArtworkProject(project);
    const QByteArray artwork = buildLiveryGyvlForSectionCount(
        artworkProject, motorsport ? kMotorsportSections : kHorizon5Sections, &counts);
    QByteArray payload("vlrc", 4);
    QByteArray sourceEntries;
    const QByteArray &source = project.liverySource;
    const int sourceArtwork = source.indexOf(QByteArray("gyvl", 4));
    const bool sourceMotorsport = sourceArtwork >= kMotorsportArtworkOffset
        && detail::readLeU32(source, 4) == 2;
    quint32 entryCount = 0;
    if (motorsport && sourceMotorsport) {
        entryCount = detail::readLeU32(source, kMotorsportEntryCountOffset);
        const quint64 entriesSize = static_cast<quint64>(entryCount) * kMotorsportEntrySize;
        if (entriesSize + kMotorsportArtworkOffset != static_cast<quint64>(sourceArtwork)) {
            throw std::runtime_error("invalid Motorsport livery entry metadata");
        }
        sourceEntries = source.mid(kMotorsportEntriesOffset, static_cast<qsizetype>(entriesSize));
    }

    detail::appendLeU32(payload, motorsport ? 2 : 1);
    detail::appendLeU32(payload, 0);
    detail::appendLeU32(payload, 0);
    detail::appendLeU32(payload, static_cast<quint32>(project.carId));
    detail::appendLeU32(payload, motorsport
        ? (sourceMotorsport ? detail::readLeU32(source, 20) : kDefaultMotorsportPayloadFlags) : 0);
    if (motorsport) {
        payload.append(QByteArray(8, '\xff'));
        payload.append(QByteArray(8, '\0'));
    }
    payload.append('\0');
    payload.append("yrvl", 4);
    detail::appendLeU32(payload, motorsport ? 19 : 17);
    payload.append(creatorTag);
    if (motorsport) {
        payload.append('\0');
        detail::appendLeU32(payload, entryCount);
        payload.append(sourceEntries);
    } else {
        detail::appendLeU32(payload, 1);
        payload.append('\0');
    }
    detail::appendLeU32(payload, static_cast<quint32>(artwork.size()));
    payload.append(artwork);
    payload.append("yrvl", 4);
    for (int index = 0; index < kLegacyCounters; ++index) {
        detail::appendLeU32(payload, static_cast<quint32>(counts[index]));
    }
    payload.append(paint);
    payload.append("yrvl", 4);
    if (motorsport) {
        detail::appendLeU32(payload, 0);
    }

    return payload;
}

void createOutputFolder(const QString &path) {
    if (path.isEmpty() || !QDir().mkpath(path)) {
        throw std::runtime_error(("could not create export folder: " + path).toStdString());
    }
}

void copyThumbnail(const Project &project, const QString &outputFolder,
                   ExportGameVersion game) {
    if (project.sourceFolder.isEmpty()) {
        return;
    }
    const QStringList candidates = {
        exportThumbnailName(game, project.isLivery), QStringLiteral("thumbnail"),
        QStringLiteral("bigThumb.png"), QStringLiteral("bigThumb.webp"),
        QStringLiteral("thumb.png"), QStringLiteral("thumb.webp")};
    for (const QString &name : candidates) {
        QFile file(QDir(project.sourceFolder).filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray thumbnail = file.readAll();
        if (thumbnail.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) {
            writeFile(QDir(outputFolder).filePath(exportThumbnailName(game, project.isLivery)), thumbnail);
            return;
        }
    }
}

quint64 visibleShapeCount(const scene::Layer &layer) {
    if (!layer.visible) {
        return 0;
    }
    if (layer.kind() == scene::LayerKind::Shape) {
        if (static_cast<const scene::Shape &>(layer).raster) {
            throw std::runtime_error("logo layers can only be exported from livery projects");
        }
        return 1;
    }
    quint64 count = 0;
    if (layer.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(layer).children) {
            count += visibleShapeCount(*child);
        }
    }

    return count;
}

} // namespace

QString exportThumbnailName(ExportGameVersion game, bool livery) {
    if (game == ExportGameVersion::Motorsport2023) {
        return QStringLiteral("thumbnail");
    }
    if (game == ExportGameVersion::Horizon5) {
        return livery ? QStringLiteral("bigThumb.png") : QStringLiteral("thumb.png");
    }

    return livery ? QStringLiteral("bigThumb.webp") : QStringLiteral("thumb.webp");
}

QByteArray encodeGameLiveryPayload(const Project &project, ExportGameVersion game) {
    if (game == ExportGameVersion::Horizon6) {
        return encodeCLiveryPayload(project);
    }
    std::array<int, 11> counts{};

    return nativeLiveryPayload(project, game, destinationCreatorTag(project, QString()), counts);
}

void exportGameLivery(const Project &project, const QString &outputFolder, ExportGameVersion game) {
    if (game == ExportGameVersion::Horizon6) {
        exportCLivery(project, outputFolder);
        return;
    }
    std::array<int, 11> counts{};
    const QByteArray creatorTag = destinationCreatorTag(project, outputFolder);
    const QByteArray payload = nativeLiveryPayload(project, game, creatorTag, counts);
    const quint32 shapeCount = static_cast<quint32>(std::accumulate(counts.begin(), counts.end(), 0));
    const QByteArray header = nativeHeader(project, game, shapeCount, creatorTag, outputFolder);
    QByteArray wrapped;

    if (game == ExportGameVersion::Motorsport2023) {
        for (qsizetype offset = 0; offset < payload.size(); offset += kMotorsportBlockSize) {
            wrapped.append(deflateContainer(payload.mid(offset, kMotorsportBlockSize)));
        }
    } else {
        wrapped = deflateContainer(payload);
    }
    createOutputFolder(outputFolder);
    QDir output(outputFolder);
    writeFile(output.filePath(game == ExportGameVersion::Motorsport2023
        ? QStringLiteral("data") : QStringLiteral("C_livery")), wrapped);
    writeFile(output.filePath(QStringLiteral("header")), header);
    copyThumbnail(project, outputFolder, game);
}

void exportGameGroup(const Project &project, const QString &outputFolder,
                    ExportGameVersion game, const SpriteSizeFn &spriteSize) {
    if (game == ExportGameVersion::Horizon6) {
        exportNestedProjectFolder(project, outputFolder, project.name, spriteSize);
        return;
    }
    if (project.isLivery || project.root == nullptr) {
        throw std::runtime_error("not a group project");
    }
    const quint64 count = visibleShapeCount(*project.root);
    if (count > std::numeric_limits<quint32>::max()) {
        throw std::runtime_error("too many group shapes for header metadata");
    }
    Project nativeProject = project;
    nativeProject.sourceDecPrefix.clear();
    const QByteArray payload = buildNestedPayload(nativeProject, spriteSize);
    const QByteArray header = nativeHeader(project, game, static_cast<quint32>(count),
                                           destinationCreatorTag(project, outputFolder), outputFolder);
    const bool motorsport = game == ExportGameVersion::Motorsport2023;
    const QByteArray fileBytes = motorsport ? payload : deflateContainer(payload);

    createOutputFolder(outputFolder);
    QDir output(outputFolder);
    writeFile(output.filePath(motorsport ? QStringLiteral("data") : QStringLiteral("C_group")), fileBytes);
    writeFile(output.filePath(QStringLiteral("header")), header);
    copyThumbnail(project, outputFolder, game);
}

} // namespace fls
