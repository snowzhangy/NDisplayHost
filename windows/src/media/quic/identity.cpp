// Self-signed P-256 host identity: DER certificate builder + CNG persistence.
#include "identity.h"

#include <wincrypt.h>

#include <cstdio>
#include <cstring>

#include <shlobj.h>

#include "wvd/platform.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")

namespace wvd {

namespace {

const wchar_t* kKsp = L"Microsoft Software Key Storage Provider";
const wchar_t* kKeyName = L"WVM Host Identity";
const char* kOidEcPublicKey = "1.2.840.10045.2.1";
const char* kOidCurveP256 = "1.2.840.10045.3.1.7";
const char* kOidEcdsaSha256 = "1.2.840.10045.4.3.2";
const char* kOidBasicConstraints = "2.5.29.19";
const char* kOidKeyUsage = "2.5.29.15";
const char* kOidEkuServerAuth = "2.5.29.37";

void AppendLen(std::vector<uint8_t>& out, size_t len) {
  if (len < 0x80) {
    out.push_back(static_cast<uint8_t>(len));
    return;
  }
  std::vector<uint8_t> bytes;
  while (len) {
    bytes.insert(bytes.begin(), static_cast<uint8_t>(len & 0xFF));
    len >>= 8;
  }
  out.push_back(static_cast<uint8_t>(0x80 | bytes.size()));
  out.insert(out.end(), bytes.begin(), bytes.end());
}

std::string LocalAppDataDir() {
  // WVM_HOME redirects the identity and pairing store. Tests need an isolated
  // one (a regression run must never pair into the real host's list), and it
  // lets the host run from a profile whose LOCALAPPDATA is not the right one.
  char override_[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableA("WVM_HOME", override_, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    std::wstring wide = Utf8ToWide(override_);
    for (size_t i = 1; i < wide.size(); ++i) {
      std::wstring partial = wide.substr(0, i);
      CreateDirectoryW(partial.c_str(), nullptr);
    }
    CreateDirectoryW(wide.c_str(), nullptr);
    return override_;
  }
  PWSTR base = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) return {};
  wchar_t path[MAX_PATH];
  swprintf(path, MAX_PATH, L"%s\\WVM", base);
  CoTaskMemFree(base);
  CreateDirectoryW(path, nullptr);
  int need = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(need > 0 ? need - 1 : 0), '\0');
  if (need > 1) WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), need, nullptr, nullptr);
  return out;
}

bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  size_t off = 0;
  while (off < data.size()) {
    DWORD written = 0;
    DWORD chunk = data.size() - off > 0x40000000 ? 0x40000000 : static_cast<DWORD>(data.size() - off);
    if (!WriteFile(h, data.data() + off, chunk, &written, nullptr) || written != chunk) {
      ok = false;
      break;
    }
    off += written;
  }
  CloseHandle(h);
  return ok;
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out.clear();
  uint8_t buf[65536];
  DWORD got = 0;
  bool ok = true;
  for (;;) {
    if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) break;
    out.insert(out.end(), buf, buf + got);
  }
  CloseHandle(h);
  return ok && !out.empty();
}

}  // namespace

std::string WvmHomeDir() { return LocalAppDataDir(); }

// ---- DER -------------------------------------------------------------------

std::vector<uint8_t> DerTlv(uint8_t tag, const std::vector<uint8_t>& content) {
  std::vector<uint8_t> out;
  out.push_back(tag);
  AppendLen(out, content.size());
  out.insert(out.end(), content.begin(), content.end());
  return out;
}

std::vector<uint8_t> DerSequence(std::vector<std::vector<uint8_t>> members) {
  std::vector<uint8_t> body;
  for (const auto& m : members) body.insert(body.end(), m.begin(), m.end());
  return DerTlv(0x30, body);
}

// Normalise a big-endian magnitude: strip leading zeros, prepend 0x00 when the
// high bit would read as a negative INTEGER.
static std::vector<uint8_t> NormaliseInt(std::vector<uint8_t> mag) {
  size_t first = 0;
  while (first + 1 < mag.size() && mag[first] == 0) first++;
  mag.erase(mag.begin(), mag.begin() + first);
  if (!mag.empty() && (mag[0] & 0x80)) mag.insert(mag.begin(), 0x00);
  if (mag.empty()) mag.push_back(0x00);
  return mag;
}

std::vector<uint8_t> DerIntegerFromBytes(const std::vector<uint8_t>& magnitude) {
  return DerTlv(0x02, NormaliseInt(magnitude));
}

std::vector<uint8_t> DerSmallInt(int64_t v) {
  std::vector<uint8_t> mag;
  uint64_t u = v < 0 ? static_cast<uint64_t>(-(v + 1)) + 1 : static_cast<uint64_t>(v);
  while (u) {
    mag.insert(mag.begin(), static_cast<uint8_t>(u & 0xFF));
    u >>= 8;
  }
  if (mag.empty()) mag.push_back(0);
  return DerIntegerFromBytes(mag);
}

std::vector<uint8_t> DerBitString(const std::vector<uint8_t>& bits) {
  std::vector<uint8_t> body;
  body.push_back(0);  // unused bits
  body.insert(body.end(), bits.begin(), bits.end());
  return DerTlv(0x03, body);
}

std::vector<uint8_t> DerOid(const char* dotted) {
  std::vector<uint8_t> body;
  unsigned long long prev = 0, cur = 0;
  int part = 0;
  auto flushNum = [&]() {
    // Base-128 little-endian groups.
    std::vector<uint8_t> enc;
    enc.push_back(static_cast<uint8_t>(cur & 0x7F));
    cur >>= 7;
    while (cur) {
      enc.insert(enc.begin(), static_cast<uint8_t>((cur & 0x7F) | 0x80));
      cur >>= 7;
    }
    body.insert(body.end(), enc.begin(), enc.end());
  };
  for (const char* p = dotted;; p++) {
    if (*p == '.' || *p == '\0') {
      if (part == 0) {
        prev = cur;
        cur = 0;
      } else if (part == 1) {
        cur += prev * 40;
        flushNum();
        cur = 0;
      } else {
        flushNum();
        cur = 0;
      }
      part++;
      if (*p == '\0') break;
    } else {
      cur = cur * 10 + static_cast<unsigned>(*p - '0');
    }
  }
  return DerTlv(0x06, body);
}

std::vector<uint8_t> DerUtf8String(const std::string& s) {
  return DerTlv(0x0C, std::vector<uint8_t>(s.begin(), s.end()));
}

std::vector<uint8_t> DerGeneralizedTime(const SYSTEMTIME& t) {
  char buf[16];
  sprintf(buf, "%04u%02u%02u%02u%02u%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
          t.wSecond);
  return DerTlv(0x18, std::vector<uint8_t>(buf, buf + 15));
}

std::vector<uint8_t> DerContextConstructed(uint8_t tagNumber, const std::vector<uint8_t>& content) {
  return DerTlv(static_cast<uint8_t>(0xA0 | tagNumber), content);
}

std::vector<uint8_t> EcdsaRawToDer(const uint8_t* raw, size_t partLen) {
  std::vector<uint8_t> r(raw, raw + partLen);
  std::vector<uint8_t> s(raw + partLen, raw + 2 * partLen);
  return DerSequence({DerIntegerFromBytes(r), DerIntegerFromBytes(s)});
}

bool EcdsaDerToRaw(const std::vector<uint8_t>& der, size_t partLen, std::vector<uint8_t>& raw) {
  // SEQUENCE ::= 0x30 len SEQUENCE{ INTEGER r, INTEGER s }
  if (der.size() < 6 || der[0] != 0x30) return false;
  size_t i = 1;
  size_t seqLen = der[i++];
  if (seqLen & 0x80) {
    size_t n = seqLen & 0x7F;
    if (n > 2 || i + n > der.size()) return false;
    seqLen = 0;
    for (size_t k = 0; k < n; k++) seqLen = (seqLen << 8) | der[i++];
  }
  auto readInt = [&](std::vector<uint8_t>& out) -> bool {
    if (i >= der.size() || der[i] != 0x02) return false;
    i++;
    size_t len = der[i++];
    if (len & 0x80) {
      size_t n = len & 0x7F;
      if (n > 2 || i + n > der.size()) return false;
      len = 0;
      for (size_t k = 0; k < n; k++) len = (len << 8) | der[i++];
    }
    if (i + len > der.size()) return false;
    out.assign(der.begin() + i, der.begin() + i + len);
    i += len;
    return true;
  };
  std::vector<uint8_t> r, s;
  if (!readInt(r) || !readInt(s)) return false;
  auto fit = [&](const std::vector<uint8_t>& v, size_t off) -> bool {
    std::vector<uint8_t> m = v;
    while (!m.empty() && m[0] == 0) m.erase(m.begin());
    if (m.size() > partLen) return false;
    raw[off + (partLen - m.size())] = 0;  // ensure zeroed region
    for (size_t k = 0; k < m.size(); k++) raw[off + (partLen - m.size()) + k] = m[k];
    return true;
  };
  raw.assign(2 * partLen, 0);
  return fit(r, 0) && fit(s, partLen);
}

std::vector<uint8_t> Sha256(const uint8_t* p, size_t n) {
  std::vector<uint8_t> out;
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  BCRYPT_HASH_HANDLE hHash = nullptr;
  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return out;
  if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) < 0) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
  }
  out.resize(32);
  if (BCryptHashData(hHash, const_cast<PUCHAR>(p), static_cast<ULONG>(n), 0) < 0 ||
      BCryptFinishHash(hHash, out.data(), 32, 0) < 0)
    out.clear();
  BCryptDestroyHash(hHash);
  BCryptCloseAlgorithmProvider(hAlg, 0);
  return out;
}

// ---- Identity --------------------------------------------------------------

namespace {

// Name ::= SEQUENCE of RDN; single CN.
std::vector<uint8_t> MakeName(const std::string& cn) {
  std::vector<uint8_t> attr =
      DerSequence({DerOid("2.5.4.3"), DerUtf8String(cn)});  // AttributeTypeAndValue
  return DerSequence({DerTlv(0x31, attr)});                 // RDN SET
}

// Build and self-sign the certificate using the CNG key.
std::vector<uint8_t> BuildCertificate(NCRYPT_KEY_HANDLE hKey, const std::string& cn) {
  // Public key: BCRYPT_ECCKEY_BLOB header + X + Y.
  ULONG got = 0;
  std::vector<uint8_t> pub;
  if (NCryptExportKey(hKey, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, nullptr, 0, &got, 0) != ERROR_SUCCESS)
    return {};
  pub.resize(got);
  if (NCryptExportKey(hKey, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, pub.data(), got, &got, 0) != ERROR_SUCCESS)
    return {};
  if (got < 8 + 64) return {};
  std::vector<uint8_t> xy(pub.begin() + 8, pub.end());  // X||Y, 64 bytes
  std::vector<uint8_t> point;
  point.push_back(0x04);
  point.insert(point.end(), xy.begin(), xy.end());

  SYSTEMTIME notBefore{}, notAfter{};
  GetSystemTime(&notBefore);
  notAfter = notBefore;
  notAfter.wYear += 10;

  std::vector<uint8_t> sigAlg = DerSequence({DerOid(kOidEcdsaSha256)});
  std::vector<uint8_t> spki = DerSequence({
      DerSequence({DerOid(kOidEcPublicKey), DerOid(kOidCurveP256)}),
      DerBitString(point),
  });
  // Extensions: basicConstraints (cA=false), keyUsage digitalSignature,
  // extendedKeyUsage serverAuth.
  // Extension extnValue is the DER of the value, wrapped in OCTET STRING.
  std::vector<uint8_t> extBasic = DerSequence({
      DerOid(kOidBasicConstraints), DerTlv(0x04, DerSequence({}))});
  std::vector<uint8_t> extKeyUsage = DerSequence({
      DerOid(kOidKeyUsage), DerTlv(0x04, DerBitString(std::vector<uint8_t>{0x80}))});
  std::vector<uint8_t> extEku = DerSequence({
      DerOid(kOidEkuServerAuth),
      DerTlv(0x04, DerSequence({DerOid("1.3.6.1.5.5.7.3.1")}))});
  std::vector<uint8_t> exts = DerContextConstructed(3, DerSequence({extBasic, extKeyUsage, extEku}));

  // Random serial.
  std::vector<uint8_t> serial(8);
  BCRYPT_ALG_HANDLE hRng = nullptr;
  BCryptOpenAlgorithmProvider(&hRng, BCRYPT_RNG_ALGORITHM, nullptr, 0);
  if (hRng) {
    BCryptGenRandom(hRng, serial.data(), 8, 0);
    BCryptCloseAlgorithmProvider(hRng, 0);
  }
  serial[0] &= 0x7F;  // positive

  std::vector<uint8_t> tbs = DerSequence({
      DerContextConstructed(0, DerSmallInt(2)),  // v3
      DerIntegerFromBytes(serial),
      sigAlg,
      MakeName(cn),
      DerSequence({DerGeneralizedTime(notBefore), DerGeneralizedTime(notAfter)}),
      MakeName(cn),
      spki,
      exts,
  });

  // Sign the TBS.
  std::vector<uint8_t> hash = Sha256(tbs.data(), tbs.size());
  ULONG sigLen = 0;
  std::vector<uint8_t> sig;
  if (NCryptSignHash(hKey, nullptr, hash.data(), static_cast<ULONG>(hash.size()), nullptr, 0,
                     &sigLen, 0) != ERROR_SUCCESS)
    return {};
  sig.resize(sigLen);
  if (NCryptSignHash(hKey, nullptr, hash.data(), static_cast<ULONG>(hash.size()), sig.data(),
                     sigLen, &sigLen, 0) != ERROR_SUCCESS)
    return {};
  sig.resize(sigLen);  // raw r||s for ECDSA P-256 (64 bytes)

  std::vector<uint8_t> der =
      DerSequence({tbs, sigAlg, DerBitString(EcdsaRawToDer(sig.data(), sig.size() / 2))});
  return der;
}

}  // namespace

bool HostIdentity::Load(const std::string& cnName, std::string* error) {
  Unload();
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    Unload();
    return false;
  };

  NCRYPT_PROV_HANDLE hProv = 0;
  SECURITY_STATUS st = NCryptOpenStorageProvider(&hProv, kKsp, 0);
  if (st != ERROR_SUCCESS) return fail("NCryptOpenStorageProvider failed");

  // Load or create the persistent key.
  // User-scope key: the panel may run unelevated, and the identity belongs
  // to the account that streams (its LOCALAPPDATA holds the certificate).
  st = NCryptOpenKey(hProv, &key_, kKeyName, 0, 0);
  bool created = false;
  if (st != ERROR_SUCCESS) {
    st = NCryptCreatePersistedKey(hProv, &key_, NCRYPT_ECDSA_P256_ALGORITHM, kKeyName, 0, 0);
    if (st != ERROR_SUCCESS) {
      NCryptFreeObject(hProv);
      return fail("NCryptCreatePersistedKey failed");
    }
    created = true;
  }

  st = NCryptFinalizeKey(key_, 0);  // no-op for an existing key
  if (st != ERROR_SUCCESS && created) {
    NCryptFreeObject(key_);
    NCryptFreeObject(hProv);
    return fail("NCryptFinalizeKey failed");
  }

  std::string dir = LocalAppDataDir();
  std::string certPath = dir + "\\host-cert.der";
  printf("identity: %s key=%s created=%d\n", certPath.c_str(),
         st == ERROR_SUCCESS ? "loaded" : "new", created ? 1 : 0);

  if (!created && ReadFileBytes(certPath, der_)) {
    // Rebuild a cert context and attach the persisted key.
    cert_ = CertCreateCertificateContext(X509_ASN_ENCODING, der_.data(),
                                         static_cast<DWORD>(der_.size()));
  } else {
    der_ = BuildCertificate(key_, cnName);
    if (der_.empty()) {
      NCryptFreeObject(key_);
      NCryptFreeObject(hProv);
      return fail("certificate build failed");
    }
    cert_ = CertCreateCertificateContext(X509_ASN_ENCODING, der_.data(),
                                         static_cast<DWORD>(der_.size()));
  }
  if (!cert_) {
    NCryptFreeObject(key_);
    NCryptFreeObject(hProv);
    return fail("CertCreateCertificateContext failed");
  }

  // Attach the CNG key so Schannel (and CertGetPrivateKey) can use it:
  // CRYPT_KEY_PROV_INFO with a KSP name and dwProviderType == 0.
  CRYPT_KEY_PROV_INFO kp{};
  kp.pwszContainerName = const_cast<LPWSTR>(kKeyName);
  kp.pwszProvName = const_cast<LPWSTR>(kKsp);
  kp.dwProvType = 0;  // CNG KSP chosen by name
  kp.dwFlags = 0;  // user key set
  kp.cProvParam = 0;
  kp.rgProvParam = nullptr;
  kp.dwKeySpec = 0;
  if (!CertSetCertificateContextProperty(cert_, CERT_KEY_PROV_INFO_PROP_ID, 0, &kp)) {
    CertFreeCertificateContext(cert_);
    cert_ = nullptr;
    NCryptFreeObject(key_);
    NCryptFreeObject(hProv);
    return fail("CertSetCertificateContextProperty failed");
  }

  certHash_ = Sha256(der_.data(), der_.size());
  std::vector<uint8_t> onDisk;
  if (!ReadFileBytes(certPath, onDisk) || onDisk != der_) {
    // Persist whenever the on-disk copy is missing or differs from what we
    // just built; an unwritable certificate is non-fatal but changes the pin
    // on the next start, so say so.
    if (!WriteFileBytes(certPath, der_)) printf("identity: cannot write %s\n", certPath.c_str());
  }
  NCryptFreeObject(hProv);  // the key handle stays valid
  return true;
}

void HostIdentity::Unload() {
  if (cert_) {
    CertFreeCertificateContext(cert_);
    cert_ = nullptr;
  }
  if (key_) {
    NCryptFreeObject(key_);
    key_ = 0;
  }
  der_.clear();
  certHash_.clear();
}

}  // namespace wvd
