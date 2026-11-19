#include "GraniteModelHash.hpp"
#include "GraniteEngine.hpp"
#include "support/check.hpp"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace roclint::runtime;

class BrokenReader : public QIODevice {
public:
    BrokenReader() { open(QIODevice::ReadOnly); }
    bool isSequential() const override { return true; }
    bool atEnd() const override { return false; }
protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char*, qint64) override { return -1; }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && (std::string_view(argv[1]) == "--file" || std::string_view(argv[1]) == "--compare-file")) {
        QFile file(QString::fromLocal8Bit(argv[2]));
        ROCLINT_CHECK(file.open(QIODevice::ReadOnly));
        const auto bytes = file.size();
        QElapsedTimer timer; timer.start();
        const auto digest = graniteModelSha256(file, [] {});
        std::cout << QJsonDocument(QJsonObject{{"backend", graniteModelHashBackend()},
            {"bytes", bytes}, {"ms", timer.elapsed()}, {"sha256", QString::fromLatin1(digest.toHex())}})
            .toJson(QJsonDocument::Compact).toStdString() << '\n';
        if (std::string_view(argv[1]) == "--compare-file") {
            ROCLINT_CHECK(file.seek(0)); timer.restart();
            QCryptographicHash reference(QCryptographicHash::Sha256);
            while (!file.atEnd()) {
                const auto chunk = file.read(1024 * 1024);
                ROCLINT_CHECK(!chunk.isEmpty() || file.error() == QFileDevice::NoError);
                reference.addData(chunk);
            }
            const auto referenceDigest = reference.result();
            ROCLINT_CHECK(referenceDigest == digest);
            std::cout << QJsonDocument(QJsonObject{{"backend", "qt-sha256-reference"}, {"bytes", bytes},
                {"ms", timer.elapsed()}, {"sha256", QString::fromLatin1(referenceDigest.toHex())}})
                .toJson(QJsonDocument::Compact).toStdString() << '\n';
        }
        return 0;
    }
    for (const auto& bytes : {QByteArray(), QByteArray("abc"), QByteArray(1000000, 'a'),
                              QByteArray(2 * 1024 * 1024 + 13, '\xA5')}) {
        QBuffer input; input.setData(bytes); ROCLINT_CHECK(input.open(QIODevice::ReadOnly));
        const auto digest = graniteModelSha256(input, [] {});
        ROCLINT_CHECK(digest == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        if (bytes == "abc") ROCLINT_CHECK(digest.toHex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        ROCLINT_CHECK(input.pos() == bytes.size());
    }
    // Cancellation cannot return a partial digest as verified evidence.
    QBuffer large; large.setData(QByteArray(4 * 1024 * 1024, 'x')); large.open(QIODevice::ReadOnly);
    int checks = 0; bool cancelled = false;
    try { (void)graniteModelSha256(large, [&] { if (++checks == 3) throw std::runtime_error("cancelled"); }); }
    catch (const std::runtime_error&) { cancelled = true; }
    ROCLINT_CHECK(cancelled && checks == 3 && large.pos() == 2 * 1024 * 1024);
    BrokenReader broken; bool readFailed = false;
    try { (void)graniteModelSha256(broken, [] {}); }
    catch (const GraniteFailure& failure) { readFailed = failure.code == "MODEL_READ"; }
    ROCLINT_CHECK(readFailed);
    QBuffer closed; bool closedFailed = false;
    try { (void)graniteModelSha256(closed, [] {}); }
    catch (const GraniteFailure&) { closedFailed = true; }
    ROCLINT_CHECK(closedFailed);
    std::cout << "Full-stream SHA-256 vectors, cancellation and read failures passed.\n";
}
