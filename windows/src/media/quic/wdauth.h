// WIRE.md §7 pairing and authentication.
//
// TLS exporter secrets (32 B each, labels EXPORTER-wvd-sas-v1 / -auth-v1 /
// -pair-v1, empty context) plus the certificate hash and the client's P-256
// public key drive:
//   SAS          = HKDF-SHA256(ikm=exporter_sas, salt="wvd",
//                info="wvd-sas-v1"||cert||pk, L=8) -> u64 BE mod 1e6, 6 digits
//   auth_tr      = "wvd-auth-v1"||0x00||exporter_auth||cert||pk||utf8(device_id)
//   pair_tr      = "wvd-pair-v1"||0x00||exporter_pair||cert||pk||utf8(device_id)
//   sig          = ECDSA P-256 / SHA-256, DER
// Reference: mac/WVDKit Security/Pairing.swift.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wvd {

// TLS exporter labels (WIRE.md §7). The client asks for the same three.
inline constexpr const char* kExporterSasLabel = "EXPORTER-wvd-sas-v1";
inline constexpr const char* kExporterAuthLabel = "EXPORTER-wvd-auth-v1";
inline constexpr const char* kExporterPairLabel = "EXPORTER-wvd-pair-v1";

// HKDF-SHA256 (RFC 5869): extract+expand, outLength <= 32*255.
bool HkdfSha256(const uint8_t* ikm, size_t ikmLen, const uint8_t* salt, size_t saltLen,
                const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen);

// 6-digit SAS from the connection's sas exporter.
std::string SasCode(const std::vector<uint8_t>& sasExporter, const std::vector<uint8_t>& certSha256,
                    const std::vector<uint8_t>& devicePublicKeyX963, int digits = 6);

enum class AuthPurpose { Auth, Pair };
std::vector<uint8_t> Transcript(AuthPurpose purpose, const std::vector<uint8_t>& exporter,
                                const std::vector<uint8_t>& certSha256,
                                const std::vector<uint8_t>& devicePublicKeyX963,
                                const std::string& deviceId);

// Verify an ECDSA P-256/SHA-256 DER signature over `transcript` with the
// peer's X9.63 uncompressed public key (0x04||X||Y, 65 bytes).
bool VerifyDeviceSignature(const std::vector<uint8_t>& signatureDer,
                           const std::vector<uint8_t>& transcript,
                           const std::vector<uint8_t>& publicKeyX963);

// Paired devices ({device_id, pk} plus labels), persisted next to the
// certificate. A store entry is enough to authenticate; removal revokes.
struct PairedDevice {
  std::string deviceId;
  std::string name;
  std::string model;
  std::vector<uint8_t> publicKeyX963;
};

class PairedDeviceStore {
 public:
  bool Load(std::string* error);
  bool Save(std::string* error) const;

  const PairedDevice* find(const std::string& deviceId) const;
  void save(const PairedDevice& device);  // insert or replace (in-memory; call Save())
  bool remove(const std::string& deviceId);
  const std::vector<PairedDevice>& all() const { return devices_; }

 private:
  std::vector<PairedDevice> devices_;
};

}  // namespace wvd
