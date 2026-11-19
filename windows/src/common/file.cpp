#include "wvd/file.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include "wvd/logging.h"

#pragma comment(lib, "advapi32.lib")

namespace wvd {

namespace {

bool ProcessIsPrivileged() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  TOKEN_ELEVATION el{};
  DWORD len = 0;
  bool elevated = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &len) &&
                  el.TokenIsElevated;
  CloseHandle(tok);
  return elevated;  // SYSTEM tokens report elevated too
}

bool OwnerIsTrusted(const std::wstring& path) {
  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                            &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS)
    return false;
  const bool ok = owner && (IsWellKnownSid(owner, WinLocalSystemSid) ||
                            IsWellKnownSid(owner, WinBuiltinAdministratorsSid) ||
                            IsWellKnownSid(owner, WinLocalServiceSid));
  LocalFree(sd);
  return ok;
}

// %ProgramData%\WVD is written by the SYSTEM owner service. ProgramData lets
// ordinary users create folders, so a user could pre-create WVD (or make it a
// junction) and have the service write or read files wherever they like. When
// running privileged we create it with an admin-only DACL and refuse a
// directory that is a reparse point or owned by someone untrusted.
bool EnsureSecureDataDirectory(const std::wstring& dir) {
  const bool privileged = ProcessIsPrivileged();
  DWORD a = GetFileAttributesW(dir.c_str());
  if (a == INVALID_FILE_ATTRIBUTES) {
    // SY/BA full control, Users read+execute, not inheritable from the parent.
    const wchar_t* sddl = privileged
                              ? L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)"
                              : nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    if (sddl && ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                                     &sd, nullptr))
      sa.lpSecurityDescriptor = sd;
    const BOOL created = CreateDirectoryW(dir.c_str(), sa.lpSecurityDescriptor ? &sa : nullptr);
    if (sd) LocalFree(sd);
    if (!created && GetLastError() != ERROR_ALREADY_EXISTS) {
      WVD_LOGE("create %ls failed gle=%lu", dir.c_str(), GetLastError());
      return false;
    }
    a = GetFileAttributesW(dir.c_str());
  }
  if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
  if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
    WVD_LOGE("%ls is a junction/symlink — refusing to use it", dir.c_str());
    return false;
  }
  if (privileged && !OwnerIsTrusted(dir)) {
    WVD_LOGE("%ls is not owned by SYSTEM/Administrators — refusing to use it from a "
             "privileged process (delete it and restart)", dir.c_str());
    return false;
  }
  return true;
}

}  // namespace

bool EnsureDirectory(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return true;
  // SHCreateDirectoryEx creates intermediate directories.
  return SHCreateDirectoryExW(nullptr, path.c_str(), nullptr) == ERROR_SUCCESS;
}

bool WriteFileAtomic(const std::wstring& path, const void* data, size_t len) {
  if (path.empty()) return false;
  size_t slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) EnsureDirectory(path.substr(0, slash));
  std::wstring tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    WVD_LOGE("create %ls failed gle=%lu", path.c_str(), GetLastError());
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(h, data, DWORD(len), &written, nullptr) && written == len;
  if (ok) FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    WVD_LOGE("move %ls failed gle=%lu", path.c_str(), GetLastError());
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

bool ReadFileWhole(const std::wstring& path, std::vector<uint8_t>& out) {
  out.clear();
  if (path.empty()) return false;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 64ll * 1024 * 1024) {
    CloseHandle(h);
    return false;
  }
  out.resize(size_t(sz.QuadPart));
  DWORD got = 0;
  bool ok = sz.QuadPart == 0 ||
            (ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) &&
             got == DWORD(out.size()));
  CloseHandle(h);
  if (!ok) out.clear();
  return ok;
}

std::wstring DataDir() {
  PWSTR pd = nullptr;
  std::wstring base = L"C:\\ProgramData";
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) {
    base = pd;
    CoTaskMemFree(pd);
  }
  std::wstring dir = base + L"\\WVD";
  // Empty on refusal: every caller then fails closed (no read, no write).
  return EnsureSecureDataDirectory(dir) ? dir : std::wstring();
}

std::wstring DataPath(const wchar_t* leaf) {
  std::wstring dir = DataDir();
  return dir.empty() ? dir : dir + L"\\" + leaf;
}

}  // namespace wvd
