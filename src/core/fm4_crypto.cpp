#include "fm4_crypto.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

#include <array>
#include <cstdint>
#include <stdexcept>

namespace fls::fm4 {
namespace {

struct KindOffsets {
    int aesOffset;
    int aesIndex;
    int hmacOffset;
    int hmacIndex;
};

constexpr KindOffsets kDesignOffsets = {0xEC80, 5, 0xEC60, 1};
constexpr KindOffsets kGroupOffsets = {0x1D0C4, 2, 0x1D0A4, 7};

constexpr int kHeaderPos = 0x20;
constexpr int kHeaderSize = 20;
constexpr int kIvPos = 0x24;
constexpr int kCipherPos = 0x34;
constexpr quint8 kPadByte = 0xbb;

quint32 readBeU32(const QByteArray &b, int pos) {
    return (static_cast<quint32>(static_cast<quint8>(b[pos])) << 24)
        | (static_cast<quint32>(static_cast<quint8>(b[pos + 1])) << 16)
        | (static_cast<quint32>(static_cast<quint8>(b[pos + 2])) << 8)
        | static_cast<quint32>(static_cast<quint8>(b[pos + 3]));
}

void appendBeU32(QByteArray &b, quint32 v) {
    b.append(static_cast<char>(v >> 24));
    b.append(static_cast<char>(v >> 16));
    b.append(static_cast<char>(v >> 8));
    b.append(static_cast<char>(v));
}

struct AesTables {
    std::array<std::uint8_t, 256> sbox{};
    std::array<std::uint8_t, 256> inv{};

    AesTables() {
        std::uint8_t p = 1;
        std::uint8_t q = 1;
        do {
            p = static_cast<std::uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
            q = static_cast<std::uint8_t>(q ^ (q << 1));
            q = static_cast<std::uint8_t>(q ^ (q << 2));
            q = static_cast<std::uint8_t>(q ^ (q << 4));
            if (q & 0x80) {
                q ^= 0x09;
            }
            const auto rotl = [](std::uint8_t v, int n) {
                return static_cast<std::uint8_t>((v << n) | (v >> (8 - n)));
            };
            sbox[p] = static_cast<std::uint8_t>(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) {
            inv[sbox[i]] = static_cast<std::uint8_t>(i);
        }
    }
};

const AesTables &tables() {
    static const AesTables t;
    return t;
}

std::uint8_t xtime(std::uint8_t v) {
    return static_cast<std::uint8_t>((v << 1) ^ ((v & 0x80) ? 0x1b : 0));
}

std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t r = 0;
    while (b) {
        if (b & 1) {
            r ^= a;
        }
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

using Block = std::array<std::uint8_t, 16>;
using RoundKeys = std::array<Block, 11>;

RoundKeys expandKey(const QByteArray &key) {
    if (key.size() != 16) {
        throw std::runtime_error("AES-128 key must be 16 bytes");
    }
    const auto &sbox = tables().sbox;
    std::array<std::uint8_t, 176> w{};
    for (int i = 0; i < 16; ++i) {
        w[i] = static_cast<std::uint8_t>(key[i]);
    }
    std::uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        std::uint8_t t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if (i % 16 == 0) {
            const std::uint8_t first = t[0];
            t[0] = static_cast<std::uint8_t>(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j) {
            w[i + j] = static_cast<std::uint8_t>(w[i - 16 + j] ^ t[j]);
        }
    }
    RoundKeys keys;
    for (int r = 0; r < 11; ++r) {
        for (int i = 0; i < 16; ++i) {
            keys[r][i] = w[r * 16 + i];
        }
    }
    return keys;
}

void addRoundKey(Block &s, const Block &k) {
    for (int i = 0; i < 16; ++i) {
        s[i] ^= k[i];
    }
}

// State bytes are column-major: s[row + 4 * col].
void shiftRows(Block &s, bool inverse) {
    Block t = s;
    for (int r = 1; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const int from = inverse ? (c + 4 - r) % 4 : (c + r) % 4;
            s[r + 4 * c] = t[r + 4 * from];
        }
    }
}

void mixColumns(Block &s, bool inverse) {
    for (int c = 0; c < 4; ++c) {
        std::uint8_t *col = &s[4 * c];
        const std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        if (!inverse) {
            col[0] = static_cast<std::uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
            col[1] = static_cast<std::uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
            col[2] = static_cast<std::uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
            col[3] = static_cast<std::uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
        } else {
            col[0] = static_cast<std::uint8_t>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
            col[1] = static_cast<std::uint8_t>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
            col[2] = static_cast<std::uint8_t>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
            col[3] = static_cast<std::uint8_t>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
        }
    }
}

void encryptBlock(Block &s, const RoundKeys &keys) {
    const auto &sbox = tables().sbox;
    addRoundKey(s, keys[0]);
    for (int round = 1; round <= 10; ++round) {
        for (auto &b : s) {
            b = sbox[b];
        }
        shiftRows(s, false);
        if (round != 10) {
            mixColumns(s, false);
        }
        addRoundKey(s, keys[round]);
    }
}

void decryptBlock(Block &s, const RoundKeys &keys) {
    const auto &inv = tables().inv;
    addRoundKey(s, keys[10]);
    for (int round = 9; round >= 0; --round) {
        shiftRows(s, true);
        for (auto &b : s) {
            b = inv[b];
        }
        addRoundKey(s, keys[round]);
        if (round != 0) {
            mixColumns(s, true);
        }
    }
}

void checkCbcArgs(const QByteArray &iv, const QByteArray &data) {
    if (iv.size() != 16) {
        throw std::runtime_error("AES-CBC IV must be 16 bytes");
    }
    if (data.size() % 16 != 0) {
        throw std::runtime_error("AES-CBC data must be a multiple of 16 bytes");
    }
}

QByteArray xorKey(const QByteArray &base, const QByteArray &xuid, int index) {
    QByteArray out(base.size(), '\0');
    for (int j = 0; j < base.size(); ++j) {
        out[j] = static_cast<char>(static_cast<quint8>(base[j]) ^ static_cast<quint8>(xuid[(j + index) & 7]));
    }
    return out;
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data) {
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

QByteArray buildHeader(quint32 length) {
    QByteArray hdr;
    appendBeU32(hdr, length);
    for (int i = 0; i < 4; ++i) {
        appendBeU32(hdr, 0xbbbbbbbbu);
    }
    return hdr;
}

quint32 headerLength(const QByteArray &hdr) {
    quint32 v = 0;
    for (int i = 0; i < 5; ++i) {
        v ^= readBeU32(hdr, i * 4);
    }
    return v;
}

} // namespace

QByteArray aes128CbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &data) {
    checkCbcArgs(iv, data);
    const RoundKeys keys = expandKey(key);
    QByteArray out(data.size(), '\0');
    Block prev;
    for (int i = 0; i < 16; ++i) {
        prev[i] = static_cast<std::uint8_t>(iv[i]);
    }
    for (int pos = 0; pos < data.size(); pos += 16) {
        Block cipher;
        for (int i = 0; i < 16; ++i) {
            cipher[i] = static_cast<std::uint8_t>(data[pos + i]);
        }
        Block plain = cipher;
        decryptBlock(plain, keys);
        for (int i = 0; i < 16; ++i) {
            out[pos + i] = static_cast<char>(plain[i] ^ prev[i]);
        }
        prev = cipher;
    }
    return out;
}

QByteArray aes128CbcEncrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &data) {
    checkCbcArgs(iv, data);
    const RoundKeys keys = expandKey(key);
    QByteArray out(data.size(), '\0');
    Block prev;
    for (int i = 0; i < 16; ++i) {
        prev[i] = static_cast<std::uint8_t>(iv[i]);
    }
    for (int pos = 0; pos < data.size(); pos += 16) {
        Block block;
        for (int i = 0; i < 16; ++i) {
            block[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(data[pos + i]) ^ prev[i]);
        }
        encryptBlock(block, keys);
        for (int i = 0; i < 16; ++i) {
            out[pos + i] = static_cast<char>(block[i]);
        }
        prev = block;
    }
    return out;
}

QByteArray xuidFromHex(const QString &hex) {
    const QString trimmed = hex.trimmed();
    const QByteArray bytes = QByteArray::fromHex(trimmed.toLatin1());
    if (trimmed.size() != 16 || bytes.size() != 8) {
        throw std::runtime_error("profile ID must be 16 hex characters");
    }
    return bytes;
}

KeyPair deriveKeys(const QByteArray &defaultPe, const QByteArray &xuid, ContainerKind kind) {
    const KindOffsets &o = kind == ContainerKind::Design ? kDesignOffsets : kGroupOffsets;
    if (xuid.size() != 8) {
        throw std::runtime_error("profile ID must be 8 bytes");
    }
    if (defaultPe.size() < o.aesOffset + 16 || defaultPe.size() < o.hmacOffset + 32) {
        throw std::runtime_error("default.pe is too small to hold the livery keys");
    }
    return {xorKey(defaultPe.mid(o.aesOffset, 16), xuid, o.aesIndex),
            xorKey(defaultPe.mid(o.hmacOffset, 32), xuid, o.hmacIndex)};
}

DecryptedLiv decryptLiv(const QByteArray &raw, const QByteArray &defaultPe, const QByteArray &xuid) {
    if (raw.size() < kCipherPos + 16) {
        throw std::runtime_error("file is too small to be an FM4 livery");
    }
    const QByteArray header = raw.mid(kHeaderPos, kHeaderSize);
    const quint32 length = headerLength(header);
    const QByteArray iv = raw.mid(kIvPos, 16);
    QByteArray cipher = raw.mid(kCipherPos);
    cipher.chop(cipher.size() % 16);

    for (const ContainerKind kind : {ContainerKind::Design, ContainerKind::Group}) {
        const KeyPair keys = deriveKeys(defaultPe, xuid, kind);
        QByteArray plain = aes128CbcDecrypt(keys.aes, iv, cipher);
        plain.truncate(static_cast<qsizetype>(length));
        const QByteArray magic = kind == ContainerKind::Design ? QByteArray("crlv") : QByteArray("lvyg");
        if (plain.left(4) == magic) {
            DecryptedLiv out;
            out.kind = kind;
            out.plain = plain;
            out.signatureOk = hmacSha256(keys.hmac, header + raw.mid(kCipherPos)) == raw.left(0x20);
            return out;
        }
    }
    throw std::runtime_error("cannot decrypt: wrong profile ID, or not a livery file");
}

QByteArray encryptLiv(const QByteArray &plain, const QByteArray &defaultPe, const QByteArray &xuid,
                      ContainerKind kind) {
    const KeyPair keys = deriveKeys(defaultPe, xuid, kind);
    QByteArray data = plain;
    data.append(QByteArray((16 - plain.size() % 16) % 16, static_cast<char>(kPadByte)));
    const QByteArray header = buildHeader(static_cast<quint32>(plain.size()));
    const QByteArray cipher = aes128CbcEncrypt(keys.aes, header.mid(4, 16), data);
    return hmacSha256(keys.hmac, header + cipher) + header + cipher;
}

} // namespace fls::fm4
