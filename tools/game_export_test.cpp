#include "binary_io.h"
#include "cgroup_codec.h"
#include "fm_codec.h"
#include "game_export.h"
#include "header_codec.h"
#include "layer.h"
#include "livery_codec.h"
#include "project_codec.h"
#include "vinyl_decoder.h"
#include "vinyl_flattener.h"
#include "wgs_save_reader.h"

#include <QtCore>

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace {

void require(bool condition, const QString &message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("read failed: %1").arg(path));
    return file.readAll();
}

std::unique_ptr<fls::scene::Shape> makeShape(int index) {
    auto shape = std::make_unique<fls::scene::Shape>();
    shape->id = QString::number(index);
    shape->setVectorShape(101);
    shape->x = index * 3.0;
    shape->y = index * -2.0;
    shape->scaleX = 0.25;
    shape->scaleY = 0.5;
    shape->rotation = index * 0.25;
    shape->color = {20, 40, 80, 255};
    return shape;
}

fls::Project makeLivery(int sectionCount, bool nested = false) {
    fls::Project project;
    project.isLivery = true;
    project.name = QStringLiteral("Native export");
    project.carId = 1069;
    project.root = std::make_unique<fls::scene::Group>();
    project.headerMetadata = fls::defaultDraftHeader(project.name, QStringLiteral("Exporter"), project.carId);
    for (int slot = 0; slot < sectionCount; ++slot) {
        auto section = std::make_unique<fls::scene::Group>();
        section->isLiverySection = true;
        section->liverySectionSlot = slot;
        section->name = QString::number(slot);
        if (nested) {
            auto group = std::make_unique<fls::scene::Group>();
            group->x = 12.0;
            group->y = -8.0;
            auto first = makeShape(slot * 2);
            first->mask = true;
            group->append(std::move(first));
            group->append(makeShape(slot * 2 + 1));
            section->append(std::move(group));
        } else {
            section->append(makeShape(slot));
        }
        project.root->append(std::move(section));
    }
    return project;
}

int shapeCount(const fls::scene::Layer &layer) {
    if (layer.kind() == fls::scene::LayerKind::Shape) {
        return 1;
    }
    int count = 0;
    if (layer.kind() == fls::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fls::scene::Group &>(layer).children) {
            count += shapeCount(*child);
        }
    }
    return count;
}

void collectShapes(const fls::scene::Layer &layer, QVector<const fls::scene::Shape *> &shapes) {
    if (layer.kind() == fls::scene::LayerKind::Shape) {
        shapes.append(static_cast<const fls::scene::Shape *>(&layer));
    } else if (layer.kind() == fls::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fls::scene::Group &>(layer).children) {
            collectShapes(*child, shapes);
        }
    }
}

void compareArtwork(const fls::Project &source, const fls::Project &imported, const QString &name,
                    bool centered = false) {
    QVector<const fls::scene::Shape *> before;
    QVector<const fls::scene::Shape *> after;
    collectShapes(*source.root, before);
    collectShapes(*imported.root, after);
    if (before.size() != after.size()) {
        for (int index = 0; index < std::min(before.size(), after.size()); ++index) {
            const auto expected = before[index]->worldMatrix();
            const auto actual = after[index]->worldMatrix();
            if (before[index]->shapeId != after[index]->shapeId
                || before[index]->color != after[index]->color
                || std::abs(expected.m[0][0] - actual.m[0][0]) > 0.03) {
                qInfo() << "First divergence" << index << before[index]->shapeId << after[index]->shapeId
                    << before[index]->x << before[index]->y << before[index]->rotation
                    << before[index]->scaleX << before[index]->scaleY << before[index]->skew;
                break;
            }
        }
        for (int index = 0; index < before.size(); ++index) {
            const auto *shape = before[index];
            if (std::abs(shape->scaleX) >= 200 || std::abs(shape->scaleY) >= 5000
                || std::abs(shape->x) >= 50000 || std::abs(shape->y) >= 50000
                || std::abs(shape->scaleX) <= 1e-6 || std::abs(shape->scaleY) <= 1e-6) {
                qInfo() << "Shape outside decoder range" << index << shape->shapeId
                    << shape->x << shape->y << shape->scaleX << shape->scaleY;
            }
        }
    }
    require(before.size() == after.size(), QStringLiteral("%1 artwork count %2/%3")
        .arg(name).arg(before.size()).arg(after.size()));
    for (int index = 0; index < before.size(); ++index) {
        require(before[index]->shapeId == after[index]->shapeId
            && before[index]->rasterId == after[index]->rasterId,
            QStringLiteral("%1 shape %2 identifier %3/%4 vs %5/%6")
                .arg(name).arg(index).arg(before[index]->shapeId).arg(before[index]->rasterId)
                .arg(after[index]->shapeId).arg(after[index]->rasterId));
        auto expected = before[index]->worldMatrix();
        auto actual = after[index]->worldMatrix();
        if (centered) {
            for (int row = 0; row < 2; ++row) {
                expected.m[row][2] -= before.front()->worldMatrix().m[row][2];
                actual.m[row][2] -= after.front()->worldMatrix().m[row][2];
            }
        }
        require(before[index]->color == after[index]->color, name + " shape color");
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                require(std::abs(expected.m[row][column] - actual.m[row][column]) < 0.03,
                    QStringLiteral("%1 shape %2 transform %3/%4: %5 vs %6")
                        .arg(name).arg(index).arg(row).arg(column)
                        .arg(expected.m[row][column]).arg(actual.m[row][column]));
            }
        }
    }
}

QByteArray paintBlock(const QByteArray &raw) {
    const int artwork = raw.indexOf(QByteArray("gyvl", 4));
    const int stats = artwork + static_cast<int>(fls::detail::readLeU32(raw, artwork - 4));
    const bool tagged = raw.mid(stats, 4) == QByteArray("yrvl", 4);
    const int paint = stats + (tagged ? 36 : 28);
    const int end = tagged ? raw.indexOf(QByteArray("yrvl", 4), paint) : raw.size();
    return raw.mid(paint, end - paint);
}

void testEditedLivery(const fls::Project &source, const QString &output, fls::ExportGameVersion game) {
    auto edited = source;
    QVector<const fls::scene::Shape *> shapes;
    collectShapes(*edited.root, shapes);
    if (shapes.isEmpty()) {
        return;
    }
    auto *shape = const_cast<fls::scene::Shape *>(shapes.front());
    shape->x += 0.25;
    shape->y -= 1.5;
    shape->rotation += 12.0;
    shape->scaleX *= 1.05;
    shape->skew += 0.03;
    shape->color[0] ^= 8;
    fls::exportGameLivery(edited, output, game);
    const auto imported = game == fls::ExportGameVersion::Motorsport2023
        ? fls::importFM2023Asset(output) : fls::importCLivery(output);
    compareArtwork(edited, imported, source.name + " edited");
}

void testLiveries(const QString &root) {
    const std::array<fls::ExportGameVersion, 7> games = {
        fls::ExportGameVersion::Horizon6, fls::ExportGameVersion::Horizon5,
        fls::ExportGameVersion::Motorsport2023, fls::ExportGameVersion::Horizon2,
        fls::ExportGameVersion::Horizon3, fls::ExportGameVersion::Motorsport5,
        fls::ExportGameVersion::Motorsport6};
    const std::array<quint32, 7> versions = {7, 4, 9, 0, 1, 0, 0};
    const std::array<int, 7> envelopes = {51, 50, 66, 49, 50, 45, 49};
    for (int gameIndex = 0; gameIndex < static_cast<int>(games.size()); ++gameIndex) {
        const auto game = games[gameIndex];
        const int sections = gameIndex == 0 ? 11 : 6;
        for (int nested = 0; nested < 2; ++nested) {
            auto project = makeLivery(sections, nested != 0);
            const QString folder = QDir(root).filePath(QStringLiteral("livery-%1-%2").arg(gameIndex).arg(nested));
            fls::exportGameLivery(project, folder, game);
            const auto header = fls::parseHeader(readFile(QDir(folder).filePath(QStringLiteral("header"))));
            require(header.formatVersion == versions[gameIndex], "header version");
            require(header.carId == 1069 && header.typeValue == static_cast<quint32>(sections * (nested + 1)), "header counts/car");
            require(header.creatorName == QStringLiteral("Exporter"), "creator");
            const auto imported = gameIndex == 2 ? fls::importFM2023Asset(folder) : fls::importCLivery(folder);
            require(shapeCount(*imported.root) == sections * (nested + 1), QStringLiteral("livery shape count %1/%2").arg(gameIndex).arg(nested));
            compareArtwork(project, imported, QStringLiteral("generated %1/%2").arg(gameIndex).arg(nested));
            const auto raw = fls::encodeGameLiveryPayload(project, game);
            require(fls::detail::readLeU32(raw, 8) == 0, "unlocked payload");
            require(raw.indexOf(QByteArray("gyvl", 4)) == envelopes[gameIndex], "root envelope");
            if (gameIndex != 0) {
                auto reexport = imported;
                const auto rebuilt = fls::encodeGameLiveryPayload(reexport, game);
                require(paintBlock(raw) == paintBlock(rebuilt), "paint preservation");
            }
        }
    }
}

void testGroups(const QString &root) {
    fls::Project project;
    project.name = QStringLiteral("Native group");
    project.root = std::make_unique<fls::scene::Group>();
    auto group = std::make_unique<fls::scene::Group>();
    auto first = makeShape(0);
    first->mask = true;
    group->append(std::move(first));
    group->append(makeShape(1));
    project.root->append(std::move(group));
    project.root->append(makeShape(2));
    for (const auto game : {fls::ExportGameVersion::Horizon5, fls::ExportGameVersion::Motorsport2023,
            fls::ExportGameVersion::Horizon2, fls::ExportGameVersion::Horizon3,
            fls::ExportGameVersion::Motorsport5, fls::ExportGameVersion::Motorsport6}) {
        const QString folder = QDir(root).filePath(QStringLiteral("group-%1").arg(static_cast<int>(game)));
        fls::exportGameGroup(project, folder, game);
        const auto imported = game == fls::ExportGameVersion::Motorsport2023
            ? fls::importFM2023Asset(folder) : fls::importCGroupNested(folder);
        require(shapeCount(*imported.root) == 3, "group shape count");
        compareArtwork(project, imported, "generated group", true);
        const auto header = fls::parseHeader(readFile(QDir(folder).filePath(QStringLiteral("header"))));
        require(header.typeValue == 3 && header.carId == 0, "group header counts");
        require(!QFile::exists(QDir(folder).filePath(QStringLiteral("container.1"))), "no WGS descriptor");
    }
}

void testBoundsAndBlocks(const QString &root) {
    for (const auto game : {fls::ExportGameVersion::Horizon5, fls::ExportGameVersion::Motorsport2023,
            fls::ExportGameVersion::Horizon2, fls::ExportGameVersion::Horizon3,
            fls::ExportGameVersion::Motorsport5, fls::ExportGameVersion::Motorsport6}) {
        const int sections = 6;
        for (int populated = -1; populated < sections; ++populated) {
            auto sparse = makeLivery(sections);
            for (int slot = 0; slot < sections; ++slot) {
                if (slot != populated) {
                    static_cast<fls::scene::Group &>(*sparse.root->children[slot]).children.clear();
                }
            }
            const QString folder = QDir(root).filePath(QStringLiteral("sparse-%1-%2").arg(sections).arg(populated));
            fls::exportGameLivery(sparse, folder, game);
            const auto imported = game == fls::ExportGameVersion::Motorsport2023
                ? fls::importFM2023Asset(folder) : fls::importCLivery(folder);
            require(shapeCount(*imported.root) == (populated < 0 ? 0 : 1), "sparse section framing");
        }
    }
    auto unsupported = makeLivery(11);
    const auto stripped = fls::parseInflatedLiveryPayload(
        fls::encodeGameLiveryPayload(unsupported, fls::ExportGameVersion::Horizon5));
    require(stripped.sectionCounts.size() == 6
        && std::accumulate(stripped.sectionCounts.begin(), stripped.sectionCounts.end(), 0) == 6,
        "window artwork discarded and spoiler retained");
    auto painted = makeLivery(6);
    painted.liveryPaint.setDefaultCarColorBgra({20, 40, 80, 255});
    require(paintBlock(fls::encodeGameLiveryPayload(painted, fls::ExportGameVersion::Horizon5))
        == paintBlock(fls::encodeGameLiveryPayload(makeLivery(6), fls::ExportGameVersion::Horizon5)),
        "unmatched paint replaced with native fallback");
    auto large = makeLivery(6);
    auto &top = static_cast<fls::scene::Group &>(*large.root->children[2]);
    for (int index = 1; index < 1500; ++index) {
        top.append(makeShape(index));
    }
    const QString folder = QDir(root).filePath(QStringLiteral("blocks"));
    fls::exportGameLivery(large, folder, fls::ExportGameVersion::Motorsport2023);
    const QByteArray data = readFile(QDir(folder).filePath(QStringLiteral("data")));
    QByteArray inflated;
    int blocks = 0;
    for (int offset = 0; offset < data.size(); ++blocks) {
        const int compressed = static_cast<int>(fls::detail::readLeU32(data, offset));
        require(fls::detail::readLeU32(data, offset + 4) <= 32768, "FM block limit");
        inflated.append(fls::inflateFirstContainer(data.mid(offset, compressed + 8)));
        offset += compressed + 8;
    }
    require(blocks > 1 && inflated == fls::encodeGameLiveryPayload(large, fls::ExportGameVersion::Motorsport2023), "block concatenation");
    require(shapeCount(*fls::importFM2023Asset(folder).root) == 1505, "multi-block import");
}

int skippedShapes(const fls::VinylGroup &group) {
    int count = group.skippedChildren;
    for (const auto &item : group.items) {
        if (!item.isShape()) {
            count += skippedShapes(*std::get<fls::VinylGroupPtr>(item.value));
        }
    }
    return count;
}

void testEarlySamples(const QString &sampleRoot, const QString &outputRoot) {
    const std::array<std::pair<QString, fls::ExportGameVersion>, 4> games = {{
        {"PTG Stuzib85 - Forza Horizon 2", fls::ExportGameVersion::Horizon2},
        {"PTG Stuzib85 - Forza Horizon 3", fls::ExportGameVersion::Horizon3},
        {"PTG Stuzib85 - Forza Motorsport 5", fls::ExportGameVersion::Motorsport5},
        {"PTG Stuzib85 - Forza Motorsport 6", fls::ExportGameVersion::Motorsport6}}};
    int headers = 0;
    int roundTrips = 0;
    int locked = 0;
    for (const auto &[name, game] : games) {
        for (const auto &folder : QDir(QDir(sampleRoot).filePath(name)).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = folder.absoluteFilePath();
            const QByteArray header = readFile(QDir(path).filePath("header"));
            require(fls::buildHeader(fls::parseHeader(header)) == header,
                name + "/" + folder.fileName() + " header round trip");
            ++headers;
            const bool livery = QFile::exists(QDir(path).filePath("C_livery"));
            const auto raw = fls::inflateContainer(readFile(QDir(path).filePath(livery ? "C_livery" : "C_group")));
            if ((livery && fls::detail::readLeU32(raw, 8) == 1)
                || (!livery && static_cast<quint8>(raw.at(29)) == 33)) {
                ++locked;
                continue;
            }
            qInfo().noquote() << name << folder.fileName();
            const auto project = livery ? fls::importCLivery(path) : fls::importCGroupNested(path);
            require(project.headerMetadata.has_value(), "native metadata imported");
            if (livery) {
                const auto payload = fls::parseInflatedLiveryPayload(raw);
                require(payload.sectionCounts.size() == 6, "six early sections");
                const int expected = std::accumulate(payload.sectionCounts.begin(), payload.sectionCounts.end(), 0);
                int skipped = 0;
                for (const auto &section : fls::decodeLiverySections(payload)) {
                    skipped += skippedShapes(section.subtree);
                }
                if (shapeCount(*project.root) + skipped != expected) {
                    qInfo() << "Declared sections" << payload.sectionCounts;
                    for (const auto &section : project.root->children) {
                        qInfo() << section->name << shapeCount(*section);
                    }
                }
                require(shapeCount(*project.root) + skipped == expected,
                    QStringLiteral("%1/%2 complete native artwork %3/%4")
                        .arg(name).arg(folder.fileName()).arg(shapeCount(*project.root)).arg(expected));
            }
            const QString output = QDir(outputRoot).filePath(QStringLiteral("early-%1-%2").arg(static_cast<int>(game)).arg(folder.fileName()));
            if (livery) {
                fls::exportGameLivery(project, output, game);
            } else {
                fls::exportGameGroup(project, output, game);
            }
            const auto imported = livery ? fls::importCLivery(output) : fls::importCGroupNested(output);
            compareArtwork(project, imported, name + "/" + folder.fileName(), !livery);
            if (livery) {
                require(paintBlock(raw) == paintBlock(imported.liverySource), "native paint preserved");
                testEditedLivery(project, output + "-edited", game);
            }
            ++roundTrips;
        }
    }
    qInfo() << "Early sample headers:" << headers << "round trips:" << roundTrips << "locked:" << locked;
}

void testSamples(const QString &sampleRoot, const QString &outputRoot) {
    int headerCount = 0;
    int exportCount = 0;
    int locked = 0;
    for (const QFileInfo &folder : QDir(QDir(sampleRoot).filePath(QStringLiteral("fh5"))).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        fls::WgsAsset asset;
        try {
            asset = fls::readWgsContainer(folder.absoluteFilePath());
        } catch (const std::exception &) {
            continue;
        }
        if (!asset.headerPath.isEmpty()) {
            const QByteArray header = readFile(asset.headerPath);
            require(fls::buildHeader(fls::parseHeader(header)) == header, QStringLiteral("FH5 header round trip: %1").arg(folder.fileName()));
            ++headerCount;
        }
        const QByteArray wrapped = readFile(asset.payloadPath);
        const QByteArray raw = fls::inflateContainer(wrapped);
        if ((asset.kind == fls::WgsAssetKind::Livery && fls::detail::readLeU32(raw, 8) == 1)
            || (asset.kind == fls::WgsAssetKind::Group && static_cast<quint8>(raw.at(29)) == 33)) {
            ++locked;
            continue;
        }
        const QByteArray header = asset.headerPath.isEmpty() ? QByteArray() : readFile(asset.headerPath);
        const auto project = asset.kind == fls::WgsAssetKind::Livery
            ? fls::importCLiveryData(wrapped, header, folder.fileName())
            : fls::importCGroupNestedData(wrapped, header, folder.fileName());
        const QString output = QDir(outputRoot).filePath(folder.fileName());
        if (project.isLivery) {
            fls::exportGameLivery(project, output, fls::ExportGameVersion::Horizon5);
        } else {
            fls::exportGameGroup(project, output, fls::ExportGameVersion::Horizon5);
        }
        const auto imported = project.isLivery ? fls::importCLivery(output) : fls::importCGroupNested(output);
        require(shapeCount(*project.root) == shapeCount(*imported.root), QStringLiteral("FH5 sample geometry count: %1").arg(folder.fileName()));
        compareArtwork(project, imported, folder.fileName());
        if (project.isLivery) {
            require(paintBlock(raw) == paintBlock(imported.liverySource), "FH5 native paint preservation");
            testEditedLivery(project, output + QStringLiteral("-edited"), fls::ExportGameVersion::Horizon5);
        }
        ++exportCount;
    }
    for (const QFileInfo &folder : QDir(QDir(sampleRoot).filePath(QStringLiteral("fm23"))).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QByteArray header = readFile(QDir(folder.absoluteFilePath()).filePath(QStringLiteral("header")));
        require(fls::buildHeader(fls::parseHeader(header)) == header, QStringLiteral("FM23 header round trip: %1").arg(folder.fileName()));
        ++headerCount;
        const auto payload = fls::readFM2023LiveryPayload(folder.absoluteFilePath());
        if (fls::detail::readLeU32(payload.raw, 8) == 1) {
            ++locked;
            continue;
        }
        const auto project = fls::importFM2023Asset(folder.absoluteFilePath());
        const QString output = QDir(outputRoot).filePath(folder.fileName());
        fls::exportGameLivery(project, output, fls::ExportGameVersion::Motorsport2023);
        const auto imported = fls::importFM2023Asset(output);
        require(shapeCount(*project.root) == shapeCount(*imported.root), QStringLiteral("FM23 sample geometry count: %1").arg(folder.fileName()));
        compareArtwork(project, imported, folder.fileName());
        require(paintBlock(project.liverySource) == paintBlock(imported.liverySource),
            QStringLiteral("FM23 native paint preservation %1: version %2 sizes %3/%4")
                .arg(folder.fileName()).arg(fls::detail::readLeU32(project.liverySource, 45))
                .arg(paintBlock(project.liverySource).size()).arg(paintBlock(imported.liverySource).size()));
        testEditedLivery(project, output + QStringLiteral("-edited"), fls::ExportGameVersion::Motorsport2023);
        ++exportCount;
    }
    qInfo() << "Sample headers:" << headerCount << "export/import:" << exportCount << "locked skipped:" << locked;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() == 3 && app.arguments()[1] == "--inspect") {
            const auto payload = fls::readLiveryPayload(app.arguments()[2]);
            qInfo() << "Artwork body bytes" << payload.body.size();
            for (const auto &section : fls::decodeLiverySections(payload)) {
                const auto layers = fls::flattenGroup(section.subtree);
                const int expected = section.slot < payload.sectionCounts.size() ? payload.sectionCounts[section.slot] : 0;
                qInfo() << section.name << "offset" << section.absPos << "shapes" << layers.size()
                    << "declared" << expected << "skipped" << skippedShapes(section.subtree)
                    << "errors" << fls::validateTree(section.subtree);
                if (!layers.isEmpty()) {
                    const auto &last = layers.back();
                    qInfo() << "Last record" << last.absPos << "remaining bytes" << payload.body.size() - last.absPos
                        << "last identifier" << last.shapeId;
                }
                if (qEnvironmentVariableIsSet("FLS_DUMP_RECORDS")) {
                    for (const auto &layer : layers) {
                        qInfo() << "RECORD" << section.slot << layer.absPos << layer.shapeId;
                    }
                }
            }
            return 0;
        }
        const QString root = QDir::current().filePath(QStringLiteral("tests"));
        require(QDir().mkpath(root), "temporary parent");
        QTemporaryDir temporary(QDir(root).filePath(QStringLiteral("export-XXXXXX")));
        require(temporary.isValid(), "temporary directory");
        if (qEnvironmentVariableIsSet("FLS_KEEP_TEST_OUTPUT")) {
            temporary.setAutoRemove(false);
            qInfo() << "Retained output:" << temporary.path();
        }
        qInfo() << "Testing generated liveries";
        testLiveries(temporary.path());
        qInfo() << "Testing generated groups";
        testGroups(temporary.path());
        qInfo() << "Testing sparse sections, bounds, and blocks";
        testBoundsAndBlocks(temporary.path());
        if (app.arguments().size() == 3 && app.arguments()[1] == QStringLiteral("--samples")) {
            qInfo() << "Testing native samples";
            testEarlySamples(app.arguments()[2], temporary.path());
            testSamples(app.arguments()[2], temporary.path());
        }
        qInfo() << "Game export tests passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << "Game export test failed:" << error.what();
        return 1;
    }
}
