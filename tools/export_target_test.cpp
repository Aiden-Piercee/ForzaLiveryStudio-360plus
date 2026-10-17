#include "binary_io.h"
#include "cgroup_codec.h"
#include "export_asset_dialog.h"
#include "game_export.h"
#include "header_codec.h"
#include "layer.h"
#include "project_codec.h"

#include <QtWidgets>

#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not read export");
    return file.readAll();
}

void testSelection(const QString &directory, bool livery) {
    require(QDir().mkpath(directory), "export destination directory");
    bool selected = false;
    QTimer::singleShot(0, [&selected]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            QApplication::exit(1);
            return;
        }
        for (auto *combo : dialog->findChildren<QComboBox *>()) {
            const int index = combo->findText("Forza Horizon 5");
            if (index >= 0) {
                combo->setCurrentIndex(index);
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                if (buttons != nullptr && buttons->button(QDialogButtonBox::Save)->isEnabled()) {
                    selected = true;
                    buttons->button(QDialogButtonBox::Save)->click();
                    return;
                }
            }
        }
        dialog->reject();
    });
    const auto selection = gui::showExportAssetDialog(nullptr, directory, "Target test", livery, 1069);
    require(selected && selection.has_value(), "could not select and accept FH5 export");
    require(selection->gameVersion == fls::ExportGameVersion::Horizon5, "FH5 selection returned another game");
    require(selection->containerFormat == gui::ExportContainerFormat::PgsFolder, "PGS selection changed");

    fls::Project project;
    project.name = "Target test";
    project.isLivery = livery;
    project.carId = livery ? 1069 : 0;
    project.root = std::make_unique<fls::scene::Group>();
    project.headerMetadata = fls::defaultDraftHeader(project.name, "Roundtrip", project.carId);
    for (int slot = 0; slot < (livery ? 6 : 1); ++slot) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->setVectorShape(101);
        shape->x = slot;
        if (livery) {
            auto section = std::make_unique<fls::scene::Group>();
            section->isLiverySection = true;
            section->liverySectionSlot = slot;
            section->append(std::move(shape));
            project.root->append(std::move(section));
        } else {
            project.root->append(std::move(shape));
        }
    }
    if (livery) {
        fls::exportGameLivery(project, selection->directory, selection->gameVersion);
    } else {
        fls::exportGameGroup(project, selection->directory, selection->gameVersion);
    }
    const auto header = fls::parseHeader(readFile(QDir(selection->directory).filePath("header")));
    require(header.formatVersion == 6, "FH5 selection wrote another header format");
    const QByteArray payload = fls::inflateContainer(readFile(
        QDir(selection->directory).filePath(livery ? "C_livery" : "C_group")));
    require(livery ? fls::detail::readLeU32(payload, 29) == 18 : payload[12] == 3,
        "FH5 selection wrote another payload format");
}

} // namespace

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("FLS tests");
    QCoreApplication::setApplicationName("Export target");
    QTemporaryDir output;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, output.path());
    try {
        require(output.isValid(), "temporary directory");
        testSelection(QDir(output.path()).filePath("group"), false);
        testSelection(QDir(output.path()).filePath("livery"), true);
        qInfo() << "FH5 export target selection passed for groups and liveries";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << error.what();
        return 1;
    }
}
