#pragma once

#include <QByteArray>
#include <QString>

namespace fls::fm4 {

// Forza Motorsport 4 (Xbox 360) livery containers. The base keys live in the
// player's own default.pe and are combined with the 8-byte profile XUID at
// runtime; nothing from the game is bundled.
enum class ContainerKind { Design, Group };

struct KeyPair {
    QByteArray aes;
    QByteArray hmac;
};

struct DecryptedLiv {
    ContainerKind kind = ContainerKind::Design;
    bool signatureOk = false;
    QByteArray plain;
};

QByteArray xuidFromHex(const QString &hex);
KeyPair deriveKeys(const QByteArray &defaultPe, const QByteArray &xuid, ContainerKind kind);

DecryptedLiv decryptLiv(const QByteArray &raw, const QByteArray &defaultPe, const QByteArray &xuid);
QByteArray encryptLiv(const QByteArray &plain, const QByteArray &defaultPe, const QByteArray &xuid,
                      ContainerKind kind = ContainerKind::Design);

// AES-128-CBC without padding; `data` must be a multiple of 16 bytes.
QByteArray aes128CbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &data);
QByteArray aes128CbcEncrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &data);

} // namespace fls::fm4
