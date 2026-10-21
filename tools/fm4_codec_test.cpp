#include "fm4_codec.h"
#include "fm4_crypto.h"
#include "layer.h"

#include <QtCore>

#include <cmath>
#include <exception>

#ifndef FLS_FM4_FIXTURE_DIR
#error "FLS_FM4_FIXTURE_DIR must point at tools/fixtures/fm4"
#endif

namespace {

// Same deterministic stand-in for default.pe that generated the fixtures.
QByteArray fakeDefaultPe() {
    QByteArray pe(0x1E000, '\0');
    for (int i = 0; i < pe.size(); ++i) {
        pe[i] = static_cast<char>((i * 131 + 7) & 0xff);
    }
    return pe;
}

QByteArray readFixture(const char *name) {
    QFile f(QDir(QStringLiteral(FLS_FM4_FIXTURE_DIR)).filePath(QString::fromLatin1(name)));
    if (!f.open(QIODevice::ReadOnly)) {
        qCritical() << "missing fixture" << f.fileName();
        return {};
    }
    return f.readAll();
}

bool check(bool ok, const char *what) {
    if (!ok) {
        qCritical() << "FAILED:" << what;
    }
    return ok;
}

bool testAesVector() {
    // FIPS-197 appendix C.1
    const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray plain = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const QByteArray expect = QByteArray::fromHex("69c4e0d86a7b0430d8cdb78070b4c55a");
    const QByteArray iv(16, '\0');
    const QByteArray enc = fls::fm4::aes128CbcEncrypt(key, iv, plain);
    return check(enc == expect, "AES-128 encrypt vector")
        && check(fls::fm4::aes128CbcDecrypt(key, iv, enc) == plain, "AES-128 decrypt vector");
}

bool testCryptoFixture() {
    const QByteArray pe = fakeDefaultPe();
    const QByteArray xuid = fls::fm4::xuidFromHex(QStringLiteral("0123456789ABCDEF"));
    const QByteArray enc = readFixture("sample.liv");
    const QByteArray plain = readFixture("sample.plain");
    if (enc.isEmpty() || plain.isEmpty()) {
        return false;
    }
    const fls::fm4::DecryptedLiv dec = fls::fm4::decryptLiv(enc, pe, xuid);
    if (!check(dec.kind == fls::fm4::ContainerKind::Design, "design key picked")
        || !check(dec.signatureOk, "signature verifies")
        || !check(dec.plain == plain, "plaintext matches the reference implementation")) {
        return false;
    }
    // The C++ encryptor must be byte-identical to the reference one.
    if (!check(fls::fm4::encryptLiv(plain, pe, xuid) == enc, "encryption matches the reference implementation")) {
        return false;
    }
    try {
        fls::fm4::decryptLiv(enc, pe, fls::fm4::xuidFromHex(QStringLiteral("FEDCBA9876543210")));
        return check(false, "wrong profile ID must be rejected");
    } catch (const std::exception &) {
    }
    return true;
}

bool testDesignRoundTrip() {
    const QByteArray plain = readFixture("sample.plain");
    const fls::FM4Design design = fls::parseFM4Design(plain);
    if (!check(design.carId == 1234, "car id")
        || !check(design.panels[0].size() == 1 && design.panels[3].size() == 2
                  && design.panels[5].size() == 1 && design.panels[1].isEmpty(),
                  "panel layer counts")
        || !check(design.panels[3][1].shapeId == 2201 && std::abs(design.panels[3][1].spin - 45.0f) < 1e-6f,
                  "layer fields")) {
        return false;
    }
    return check(fls::buildFM4Design(design) == plain, "parse/build is byte-identical");
}

bool testProjectRoundTrip() {
    const QByteArray plain = readFixture("sample.plain");
    fls::Project project = fls::importFM4Design(plain, QStringLiteral("sample"));
    if (!check(project.isLivery && project.carId == 1234, "project metadata")
        || !check(project.root && project.root->children.size() == fls::kFM4SectionCount, "six sections")) {
        return false;
    }
    const auto &left = static_cast<const fls::scene::Group &>(*project.root->children[3]);
    const auto &shape = static_cast<const fls::scene::Shape &>(*left.children[0]);
    // 0x80FFFFFF is A=0x80 R=G=B=0xFF -> BGRA {ff,ff,ff,80}
    if (!check(left.liverySectionSlot == 3 && left.children.size() == 2 && shape.shapeId == 2040
               && shape.color[3] == 0x80 && shape.color[0] == 0xff,
               "imported layer")) {
        return false;
    }
    fls::FM4ExportReport report;
    const QByteArray out = fls::exportFM4Design(project, &report);
    return check(report.written == 4 && report.notes.isEmpty(), "export report")
        && check(out == plain, "project import/export is lossless");
}

bool testExportFromScratch() {
    fls::Project project = fls::importFM4Design(fls::emptyFM4Design(77));
    auto &front = static_cast<fls::scene::Group &>(*project.root->children[0]);
    auto s = std::make_unique<fls::scene::Shape>();
    s->setVectorShape(450);
    s->transform.x = 3;
    s->color = {1, 2, 3, 255};
    front.append(std::move(s));
    auto bad = std::make_unique<fls::scene::Shape>();
    bad->setVectorShape(3001);
    front.append(std::move(bad));
    auto mask = std::make_unique<fls::scene::Shape>();
    mask->setVectorShape(101);
    mask->mask = true;
    front.append(std::move(mask));

    fls::FM4ExportReport report;
    const QByteArray out = fls::exportFM4Design(project, &report);
    const fls::FM4Design design = fls::parseFM4Design(out);
    return check(report.written == 1 && report.skippedShapeIds == 1 && report.skippedMasks == 1, "skips reported")
        && check(design.carId == 77 && design.panels[0].size() == 1, "re-parsed export")
        && check(design.panels[0][0].argb == 0xff030201u, "BGRA -> ARGB")
        && check(fls::isFM4ShapeId(2240) && fls::isFM4ShapeId(101) && !fls::isFM4ShapeId(100)
                 && !fls::isFM4ShapeId(3001), "shape id set");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    try {
        return testAesVector() && testCryptoFixture() && testDesignRoundTrip()
            && testProjectRoundTrip() && testExportFromScratch() ? 0 : 1;
    } catch (const std::exception &error) {
        qCritical() << "exception:" << error.what();
        return 1;
    }
}
