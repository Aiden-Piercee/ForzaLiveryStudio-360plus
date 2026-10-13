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

void compareArtwork(const fls::Project &source, const fls::Project &imported, const QString &name) {
    QVector<const fls::scene::Shape *> before;
    QVector<const fls::scene::Shape *> after;
    collectShapes(*source.root, before);
    collectShapes(*imported.root, after);
    require(before.size() == after.size(), name + " artwork count");
    for (int index = 0; index < before.size(); ++index) {
        require(before[index]->shapeId == after[index]->shapeId
            && before[index]->rasterId == after[index]->rasterId,
            QStringLiteral("%1 shape %2 identifier %3/%4 vs %5/%6")
                .arg(name).arg(index).arg(before[index]->shapeId).arg(before[index]->rasterId)
                .arg(after[index]->shapeId).arg(after[index]->rasterId));
        const auto expected = before[index]->worldMatrix();
        const auto actual = after[index]->worldMatrix();
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
    return raw.mid(stats + 32, 1367);
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
    const auto imported = game == fls::ExportGameVersion::Horizon5
        ? fls::importCLivery(output) : fls::importFM2023Asset(output);
    compareArtwork(edited, imported, source.name + " edited");
}

void testLiveries(const QString &root) {
    const std::array<fls::ExportGameVersion, 3> games = {
        fls::ExportGameVersion::Horizon6, fls::ExportGameVersion::Horizon5,
        fls::ExportGameVersion::Motorsport2023};
    for (int gameIndex = 0; gameIndex < 3; ++gameIndex) {
        const auto game = games[gameIndex];
        const int sections = gameIndex == 0 ? 11 : gameIndex == 1 ? 5 : 7;
        for (int nested = 0; nested < 2; ++nested) {
            auto project = makeLivery(sections, nested != 0);
            const QString folder = QDir(root).filePath(QStringLiteral("livery-%1-%2").arg(gameIndex).arg(nested));
            fls::exportGameLivery(project, folder, game);
            const auto header = fls::parseHeader(readFile(QDir(folder).filePath(QStringLiteral("header"))));
            require(header.formatVersion == (gameIndex == 0 ? 7u : gameIndex == 1 ? 4u : 9u), "header version");
            require(header.carId == 1069 && header.typeValue == static_cast<quint32>(sections * (nested + 1)), "header counts/car");
            require(header.creatorName == QStringLiteral("Exporter"), "creator");
            const auto imported = gameIndex == 2 ? fls::importFM2023Asset(folder) : fls::importCLivery(folder);
            require(shapeCount(*imported.root) == sections * (nested + 1), QStringLiteral("livery shape count %1/%2").arg(gameIndex).arg(nested));
            const auto raw = fls::encodeGameLiveryPayload(project, game);
            require(fls::detail::readLeU32(raw, 8) == 0, "unlocked payload");
            require(raw.indexOf(QByteArray("gyvl", 4)) == (gameIndex == 0 ? 51 : gameIndex == 1 ? 50 : 66), "root envelope");
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
    for (const auto game : {fls::ExportGameVersion::Horizon5, fls::ExportGameVersion::Motorsport2023}) {
        const QString folder = QDir(root).filePath(QStringLiteral("group-%1").arg(static_cast<int>(game)));
        fls::exportGameGroup(project, folder, game);
        const auto imported = game == fls::ExportGameVersion::Motorsport2023
            ? fls::importFM2023Asset(folder) : fls::importCGroupNested(folder);
        require(shapeCount(*imported.root) == 3, "group shape count");
        const auto header = fls::parseHeader(readFile(QDir(folder).filePath(QStringLiteral("header"))));
        require(header.typeValue == 3 && header.carId == 0, "group header counts");
        require(!QFile::exists(QDir(folder).filePath(QStringLiteral("container.1"))), "no WGS descriptor");
    }
}

void testBoundsAndBlocks(const QString &root) {
    for (const auto game : {fls::ExportGameVersion::Horizon5, fls::ExportGameVersion::Motorsport2023}) {
        const int sections = game == fls::ExportGameVersion::Horizon5 ? 5 : 7;
        for (int populated = -1; populated < sections; ++populated) {
            auto sparse = makeLivery(sections);
            for (int slot = 0; slot < sections; ++slot) {
                if (slot != populated) {
                    static_cast<fls::scene::Group &>(*sparse.root->children[slot]).children.clear();
                }
            }
            const QString folder = QDir(root).filePath(QStringLiteral("sparse-%1-%2").arg(sections).arg(populated));
            fls::exportGameLivery(sparse, folder, game);
            const auto imported = game == fls::ExportGameVersion::Horizon5
                ? fls::importCLivery(folder) : fls::importFM2023Asset(folder);
            require(shapeCount(*imported.root) == (populated < 0 ? 0 : 1), "sparse section framing");
        }
    }
    auto unsupported = makeLivery(7);
    bool rejected = false;
    try {
        fls::encodeGameLiveryPayload(unsupported, fls::ExportGameVersion::Horizon5);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "unsupported panels rejected");
    auto painted = makeLivery(5);
    painted.liveryPaint.setDefaultCarColorBgra({20, 40, 80, 255});
    rejected = false;
    try {
        fls::encodeGameLiveryPayload(painted, fls::ExportGameVersion::Horizon5);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "unverified paint mapping rejected");
    auto large = makeLivery(7);
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
    require(shapeCount(*fls::importFM2023Asset(folder).root) == 1506, "multi-block import");
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
        require(paintBlock(project.liverySource) == paintBlock(imported.liverySource), "FM23 native paint preservation");
        testEditedLivery(project, output + QStringLiteral("-edited"), fls::ExportGameVersion::Motorsport2023);
        ++exportCount;
    }
    qInfo() << "Sample headers:" << headerCount << "export/import:" << exportCount << "locked skipped:" << locked;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    try {
        const QString root = QDir::current().filePath(QStringLiteral("tests"));
        require(QDir().mkpath(root), "temporary parent");
        QTemporaryDir temporary(QDir(root).filePath(QStringLiteral("export-XXXXXX")));
        require(temporary.isValid(), "temporary directory");
        qInfo() << "Testing generated liveries";
        testLiveries(temporary.path());
        qInfo() << "Testing generated groups";
        testGroups(temporary.path());
        qInfo() << "Testing sparse sections, bounds, and blocks";
        testBoundsAndBlocks(temporary.path());
        if (app.arguments().size() == 3 && app.arguments()[1] == QStringLiteral("--samples")) {
            qInfo() << "Testing native samples";
            testSamples(app.arguments()[2], temporary.path());
        }
        qInfo() << "Game export tests passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << "Game export test failed:" << error.what();
        return 1;
    }
}
