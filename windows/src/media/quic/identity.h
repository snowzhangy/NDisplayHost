// Host TLS identity (WIRE.md §2): self-signed X.509, P-256 ECDSA, persisted.
//
// The key lives in the machine's CNG key storage provider under a fixed name;
// the certificate DER is written to %LOCALAPPDATA%\WVM\. msquic gets the cert
// as a PCCERT_CONTEXT with the provider key attached; the client pins
// SHA-256(DER). Certificate generation is hand-rolled DER — no COM, no
// external crypto dependency.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <ncrypt.h>

namespace wvd {

class HostIdentity {
 public:
  // Load the persisted identity, creating and self-signing a new one on first
  // run. cnName appears in the certificate subject ("WVM Host").
  bool Load(const std::string& cnName, std::string* error);
  void Unload();
  ~HostIdentity() { Unload(); }

  // Owned by this object; valid until Unload()/destruction.
  PCCERT_CONTEXT certContext() const { return cert_; }
  // SHA-256 of the certificate DER (what a paired client pins).
  const std::vector<uint8_t>& certSha256() const { return certHash_; }
  const std::vector<uint8_t>& certDer() const { return der_; }

 private:
  NCRYPT_KEY_HANDLE key_ = 0;
  PCCERT_CONTEXT cert_ = nullptr;
  std::vector<uint8_t> der_;
  std::vector<uint8_t> certHash_;
};

// Where the identity and the pairing store live: %LOCALAPPDATA%\WVM, or
// WVM_HOME when set (isolated tests, service accounts, portable installs).
std::string WvmHomeDir();

// ---- Reusable pieces (also used by auth) -----------------------------------

std::vector<uint8_t> Sha256(const uint8_t* p, size_t n);

// Minimal DER building blocks. Each returns the complete TLV.
std::vector<uint8_t> DerTlv(uint8_t tag, const std::vector<uint8_t>& content);
std::vector<uint8_t> DerSequence(std::vector<std::vector<uint8_t>> members);
std::vector<uint8_t> DerIntegerFromBytes(const std::vector<uint8_t>& magnitude);  // big-endian, normalised
std::vector<uint8_t> DerSmallInt(int64_t v);
std::vector<uint8_t> DerBitString(const std::vector<uint8_t>& bits);  // 0 unused bits
std::vector<uint8_t> DerOid(const char* dotted);
std::vector<uint8_t> DerUtf8String(const std::string& s);
std::vector<uint8_t> DerGeneralizedTime(const SYSTEMTIME& t);  // YYYYMMDDHHMMSSZ
std::vector<uint8_t> DerContextConstructed(uint8_t tagNumber, const std::vector<uint8_t>& content);

// Raw ECDSA r||s (2*partLen) to DER ECDSA-Sig-Value SEQUENCE{INTEGER,INTEGER}.
std::vector<uint8_t> EcdsaRawToDer(const uint8_t* raw, size_t partLen);
// DER ECDSA-Sig-Value to raw r||s (zero-padded); false on malformed.
bool EcdsaDerToRaw(const std::vector<uint8_t>& der, size_t partLen, std::vector<uint8_t>& raw);

}  // namespace wvd
