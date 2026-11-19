#pragma once
#include <QByteArray>
#include <QIODevice>
#include <functional>

namespace roclint::runtime {
// Hashes every remaining byte. No timestamp/digest cache or skipped verification.
// The checkpoint runs between bounded reads and before returning the digest.
QByteArray graniteModelSha256(QIODevice& input, const std::function<void()>& checkpoint);
const char* graniteModelHashBackend();
}
