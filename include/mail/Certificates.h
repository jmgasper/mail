#pragma once
#include <string>

namespace mail {
struct CertificateDetails { std::string pem,subject,issuer,validFrom,validUntil,sha256; bool selfSigned=false; };
// True when an endpoint stores certificate text rather than a file path.
bool isPemText(const std::string&);
// Rebuilds PEM certificates whose line breaks were flattened, for example by
// pasting into a single-line text field. Other PEM blocks (keys) are dropped.
std::string normalizePem(const std::string&);
// Accepts PEM or DER bytes; the result's pem holds the first certificate as
// normalized PEM. Throws std::invalid_argument for anything else.
CertificateDetails describeCertificate(const std::string& bytes);
}
