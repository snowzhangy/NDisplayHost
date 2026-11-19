// WIRE.md §7 crypto + paired device store. See wdauth.h.
#include "wdauth.h"

#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <shlobj.h>

#include "identity.h"  // Sha256, EcdsaDerToRaw
#include "wvd/logging.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")

namespace wvd {

namespace {

// HMAC-SHA256 over one buffer (HKDF input keying material is short).
bool HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen,
                uint8_t out[32]) {
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  BCRYPT_HASH_HANDLE hHash = nullptr;
  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
    return false;
  bool ok = false;
  if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, const_cast<PUCHAR>(key),
                       static_cast<ULONG>(keyLen), 0) >= 0) {
    if (BCryptHashData(hHash, const_cast<PUCHAR>(msg), static_cast<ULONG>(msgLen), 0) >= 0 &&
        BCryptFinishHash(hHash, out, 32, 0) >= 0)
      ok = true;
    BCryptDestroyHash(hHash);
  }
  BCryptCloseAlgorithmProvider(hAlg, 0);
  return ok;
}

}  // namespace

bool HkdfSha256(const uint8_t* ikm, size_t ikmLen, const uint8_t* salt, size_t saltLen,
                const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen) {
  if (outLen == 0 || outLen > 32 * 255) return false;
  uint8_t prk[32];
  const uint8_t zeroSalt[32] = {};
  if (!HmacSha256(salt && saltLen ? salt : zeroSalt, salt && saltLen ? saltLen : 32, ikm, ikmLen,
                  prk))
    return false;

  std::vector<uint8_t> t;  // T(0) empty
  size_t produced = 0;
  uint8_t counter = 1;
  while (produced < outLen) {
    std::vector<uint8_t> msg = t;
    msg.insert(msg.end(), info, info + infoLen);
    msg.push_back(counter);
    uint8_t block[32];
    if (!HmacSha256(prk, 32, msg.data(), msg.size(), block)) return false;
    size_t take = outLen - produced < 32 ? outLen - produced : 32;
    memcpy(out + produced, block, take);
    produced += take;
    t.assign(block, block + 32);
    counter++;
  }
  return true;
}

std::string SasCode(const std::vector<uint8_t>& sasExporter, const std::vector<uint8_t>& certSha256,
                    const std::vector<uint8_t>& devicePublicKeyX963, int digits) {
  // info = "wvd-sas-v1" || cert || pk
  std::vector<uint8_t> info;
  const char* purpose = "wvd-sas-v1";
  info.insert(info.end(), purpose, purpose + 10);
  info.insert(info.end(), certSha256.begin(), certSha256.end());
  info.insert(info.end(), devicePublicKeyX963.begin(), devicePublicKeyX963.end());
  uint8_t okm[8];
  if (!HkdfSha256(sasExporter.data(), sasExporter.size(),
                  reinterpret_cast<const uint8_t*>("wvd"), 3, info.data(), info.size(), okm, 8))
    return {};
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | okm[i];  // big-endian
  v %= 1000000ull;
  char buf[16];
  snprintf(buf, sizeof(buf), "%0*u", digits, static_cast<unsigned>(v % 1000000ull));
  return buf;
}

std::vector<uint8_t> Transcript(AuthPurpose purpose, const std::vector<uint8_t>& exporter,
                                const std::vector<uint8_t>& certSha256,
                                const std::vector<uint8_t>& devicePublicKeyX963,
                                const std::string& deviceId) {
  const char* name = purpose == AuthPurpose::Auth ? "wvd-auth-v1" : "wvd-pair-v1";
  size_t nameLen = strlen(name);
  std::vector<uint8_t> tr;
  tr.reserve(nameLen + 1 + exporter.size() + certSha256.size() + devicePublicKeyX963.size() +
             deviceId.size());
  tr.insert(tr.end(), name, name + nameLen);
  tr.push_back(0x00);
  tr.insert(tr.end(), exporter.begin(), exporter.end());
  tr.insert(tr.end(), certSha256.begin(), certSha256.end());
  tr.insert(tr.end(), devicePublicKeyX963.begin(), devicePublicKeyX963.end());
  tr.insert(tr.end(), deviceId.begin(), deviceId.end());
  return tr;
}

bool VerifyDeviceSignature(const std::vector<uint8_t>& signatureDer,
                           const std::vector<uint8_t>& transcript,
                           const std::vector<uint8_t>& publicKeyX963) {
  if (publicKeyX963.size() != 65 || publicKeyX963[0] != 0x04) return false;
  std::vector<uint8_t> raw;
  if (!EcdsaDerToRaw(signatureDer, 32, raw)) return false;

  bool ok = false;
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
  // BCRYPT_ECCKEY_BLOB header + X + Y, imported under the algorithm handle:
  // there is no key pair to generate for a verification-only key, and a public
  // blob imported under an unfinalised pair verifies as garbage.
  std::vector<uint8_t> blob(8 + 64);
  auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
  hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
  hdr->cbKey = 32;
  memcpy(blob.data() + 8, publicKeyX963.data() + 1, 64);
  BCRYPT_KEY_HANDLE hKey = nullptr;
  if (BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &hKey, blob.data(),
                          static_cast<ULONG>(blob.size()), 0) >= 0) {
    const std::vector<uint8_t> hash = Sha256(transcript.data(), transcript.size());
    const NTSTATUS st = BCryptVerifySignature(hKey, nullptr, const_cast<PUCHAR>(hash.data()),
                                              static_cast<ULONG>(hash.size()), raw.data(),
                                              static_cast<ULONG>(raw.size()), 0);
    ok = st >= 0;
    if (!ok) WVD_LOGW("quic: the device signature did not verify (0x%x)", st);
    BCryptDestroyKey(hKey);
  } else {
    WVD_LOGW("quic: the device public key could not be imported");
  }
  BCryptCloseAlgorithmProvider(hAlg, 0);
  return ok;
}

// ---- Paired device store ----------------------------------------------------

namespace {

std::string StorePath() {
  // Same directory as the host certificate; WVM_HOME redirects both, which is
  // how the regression test keeps its pairings out of the real host's list.
  const std::string dir = WvmHomeDir();
  if (dir.empty()) return {};
  return dir + "\\paired-devices.txt";
}

std::string HexEncode(const std::vector<uint8_t>& v) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(v.size() * 2);
  for (uint8_t b : v) {
    s.push_back(d[b >> 4]);
    s.push_back(d[b & 15]);
  }
  return s;
}

bool HexDecode(const std::string& s, std::vector<uint8_t>& out) {
  if (s.size() % 2) return false;
  out.clear();
  out.reserve(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
  auto val = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = val(s[i]), lo = val(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

}  // namespace

bool PairedDeviceStore::Load(std::string* error) {
  devices_.clear();
  std::string path = StorePath();
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return true;  // no file = no devices, not an error
  std::string text;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
  CloseHandle(h);

  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = eol == std::string::npos ? text.size() : eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    // id \t hexPk \t name \t model  (name/model may be empty)
    size_t p1 = line.find('\t'), p2 = line.find('\t', p1 + 1), p3 = line.find('\t', p2 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) continue;
    PairedDevice d;
    d.deviceId = line.substr(0, p1);
    if (!HexDecode(line.substr(p1 + 1, p2 - p1 - 1), d.publicKeyX963)) continue;
    if (p2 != std::string::npos) {
      d.name = line.substr(p2 + 1, p3 == std::string::npos ? std::string::npos : p3 - p2 - 1);
      if (p3 != std::string::npos) d.model = line.substr(p3 + 1);
    }
    if (d.deviceId.empty() || d.publicKeyX963.size() != 65) continue;
    devices_.push_back(std::move(d));
  }
  (void)error;
  return true;
}

bool PairedDeviceStore::Save(std::string* error) const {
  std::string text = "# WVM paired devices. Deleting a line revokes that headset;\n"
                     "# deleting the file pairs everyone again (new host certificate!).\n";
  for (const auto& d : devices_) {
    text += d.deviceId;
    text += '\t';
    text += HexEncode(d.publicKeyX963);
    text += '\t';
    text += d.name;
    text += '\t';
    text += d.model;
    text += '\n';
  }
  std::string path = StorePath();
  HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = "cannot write paired-devices.txt";
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
            written == text.size();
  CloseHandle(h);
  return ok;
}

const PairedDevice* PairedDeviceStore::find(const std::string& deviceId) const {
  for (const auto& d : devices_)
    if (d.deviceId == deviceId) return &d;
  return nullptr;
}

void PairedDeviceStore::save(const PairedDevice& device) {
  for (auto& d : devices_) {
    if (d.deviceId == device.deviceId) {
      d = device;
      return;
    }
  }
  devices_.push_back(device);
}

bool PairedDeviceStore::remove(const std::string& deviceId) {
  size_t n = devices_.size();
  devices_.erase(std::remove_if(devices_.begin(), devices_.end(),
                                [&](const PairedDevice& d) { return d.deviceId == deviceId; }),
                 devices_.end());
  return devices_.size() != n;
}

}  // namespace wvd
