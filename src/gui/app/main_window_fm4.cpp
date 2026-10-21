#include "main_window.h"

#include "main_window_internal.h"

#include "fm4_codec.h"
#include "fm4_crypto.h"
#include "import_locations.h"

#include <QtGui>

#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>

#include <exception>
#include <optional>
#include <stdexcept>

namespace gui {

using namespace mw_detail;

namespace {

constexpr char kDefaultPeKey[] = "fm4/defaultPe";
constexpr char kProfileIdKey[] = "fm4/profileId";

struct Fm4Credentials {
    QByteArray defaultPe;
    QByteArray xuid;
};

// default.pe and the profile folder name (the 16-hex-digit XUID) come from the
// player's own Forza Motorsport 4 install; both are remembered between sessions.
std::optional<Fm4Credentials> askFm4Credentials(QWidget *parent) {
    QSettings settings;
    QString pePath = settings.value(QLatin1String(kDefaultPeKey)).toString();
    if (pePath.isEmpty() || !QFile::exists(pePath)) {
        pePath = QFileDialog::getOpenFileName(
            parent, QStringLiteral("Locate your FM4 default.pe"), pePath,
            QStringLiteral("default.pe (default.pe);;All files (*)"));
        if (pePath.isEmpty()) {
            return std::nullopt;
        }
    }
    bool accepted = false;
    const QString profileId = QInputDialog::getText(
        parent, QStringLiteral("FM4 profile ID"),
        QStringLiteral("Profile ID (16 hex digits, the name of your profile folder):"),
        QLineEdit::Normal, settings.value(QLatin1String(kProfileIdKey)).toString(), &accepted);
    if (!accepted) {
        return std::nullopt;
    }
    Fm4Credentials credentials;
    credentials.xuid = fls::fm4::xuidFromHex(profileId);
    QFile pe(pePath);
    if (!pe.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open: " + pePath).toStdString());
    }
    credentials.defaultPe = pe.readAll();
    settings.setValue(QLatin1String(kDefaultPeKey), pePath);
    settings.setValue(QLatin1String(kProfileIdKey), profileId.trimmed().toUpper());
    return credentials;
}

} // namespace

void MainWindow::importFM4LiveryDialog() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Forza Motorsport 4 livery"),
        importDialogStartDirectory(this, QStringLiteral("fm4Livery")),
        QStringLiteral("FM4 liveries (*.liv);;All files (*)"));
    if (path.isEmpty() || !confirmDiscardUnsavedChanges()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("fm4Livery"));
    try {
        const std::optional<Fm4Credentials> credentials = askFm4Credentials(this);
        if (!credentials) {
            return;
        }
        bool signatureOk = true;
        QString error;
        const bool imported = loadImportedProject(
            [&]() { return fls::importFM4Livery(path, credentials->defaultPe, credentials->xuid, &signatureOk); },
            QStringLiteral("Imported FM4 livery %1").arg(path), &error);
        if (!imported) {
            QMessageBox::critical(this, QStringLiteral("FM4 import failed"), error);
        } else if (!signatureOk) {
            QMessageBox::warning(this, QStringLiteral("FM4 import"),
                                 QStringLiteral("The file decrypted, but its signature does not match this profile. "
                                                "It may have been edited or come from another profile."));
        }
    } catch (const std::exception &ex) {
        QMessageBox::critical(this, QStringLiteral("FM4 import failed"), QString::fromUtf8(ex.what()));
    }
}

void MainWindow::exportFM4LiveryDialog() {
    if (!state_->hasProject_ || !state_->project_.isLivery) {
        QMessageBox::information(this, QStringLiteral("Export FM4 livery"),
                                 QStringLiteral("Open a livery project before exporting to FM4."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Forza Motorsport 4 livery"),
        importDialogStartDirectory(this, QStringLiteral("fm4Livery")) + QLatin1Char('/')
            + state_->project_.name + QStringLiteral(".liv"),
        QStringLiteral("FM4 liveries (*.liv)"));
    if (path.isEmpty()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("fm4Livery"));
    try {
        const std::optional<Fm4Credentials> credentials = askFm4Credentials(this);
        if (!credentials) {
            return;
        }
        fls::FM4ExportReport report;
        fls::exportFM4Livery(state_->project_, path, credentials->defaultPe, credentials->xuid, &report);
        statusBar()->showMessage(
            QStringLiteral("Exported %1 layer(s) to %2").arg(report.written).arg(path), 8000);
        if (!report.notes.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("FM4 export"), report.notes.join(QLatin1Char('\n')));
        }
    } catch (const std::exception &ex) {
        QMessageBox::critical(this, QStringLiteral("FM4 export failed"), QString::fromUtf8(ex.what()));
    }
}

} // namespace gui
