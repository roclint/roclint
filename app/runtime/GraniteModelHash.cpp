#include "GraniteModelHash.hpp"
#include "GraniteEngine.hpp"
#include <QCryptographicHash>
#include <QFileDevice>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace roclint::runtime {
namespace {
#ifdef Q_OS_WIN
class CngSha256 {
public:
    CngSha256() = default;
    CngSha256(const CngSha256&) = delete;
    CngSha256& operator=(const CngSha256&) = delete;
    ~CngSha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void initialize() {
        check(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        DWORD objectBytes = 0, digestBytes = 0, written = 0;
        check(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &written, 0));
        if (written != sizeof(objectBytes) || !objectBytes || objectBytes > 4 * 1024 * 1024) fail();
        check(BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &written, 0));
        if (written != sizeof(digestBytes) || digestBytes != 32) fail();
        object_.resize(static_cast<qsizetype>(objectBytes));
        check(BCryptCreateHash(algorithm_, &hash_, reinterpret_cast<PUCHAR>(object_.data()),
            objectBytes, nullptr, 0, 0));
    }
    void add(const QByteArray& bytes) {
        check(BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.constData())),
            static_cast<ULONG>(bytes.size()), 0));
    }
    QByteArray finish() {
        QByteArray digest(32, Qt::Uninitialized);
        check(BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(digest.data()), 32, 0));
        return digest;
    }
private:
    [[noreturn]] static void fail() {
        throw GraniteFailure(QStringLiteral("INTEGRITY"), "Windows could not complete SHA-256 verification.");
    }
    static void check(NTSTATUS status) { if (status < 0) fail(); }
    BCRYPT_ALG_HANDLE algorithm_{nullptr};
    BCRYPT_HASH_HANDLE hash_{nullptr};
    QByteArray object_;
};
#endif
}

const char* graniteModelHashBackend() {
#ifdef Q_OS_WIN
    return "windows-cng-sha256";
#else
    return "qt-sha256";
#endif
}

QByteArray graniteModelSha256(QIODevice& input, const std::function<void()>& checkpoint) {
    if (!input.isOpen() || !input.isReadable())
        throw GraniteFailure(QStringLiteral("MODEL_READ"), "The native model is not readable.");
#ifdef Q_OS_WIN
    CngSha256 hash;
    hash.initialize();
#else
    QCryptographicHash hash(QCryptographicHash::Sha256);
#endif
    for (;;) {
        checkpoint();
        const auto bytes = input.read(1024 * 1024);
        if (bytes.isEmpty()) {
            const auto* file = qobject_cast<QFileDevice*>(&input);
            if (!input.atEnd() || (file && file->error() != QFileDevice::NoError))
                throw GraniteFailure(QStringLiteral("MODEL_READ"), "The native model could not be read completely.");
            break;
        }
#ifdef Q_OS_WIN
        hash.add(bytes);
#else
        hash.addData(bytes);
#endif
    }
    checkpoint();
#ifdef Q_OS_WIN
    return hash.finish();
#else
    return hash.result();
#endif
}
}
