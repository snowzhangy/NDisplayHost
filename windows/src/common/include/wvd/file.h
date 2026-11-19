// Small state-file helpers (crash-safe save/restore, §4.2).
// Writes go to a temp file + MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH).
#pragma once

#include <cstdint>
#include <vector>

#include "wvd/platform.h"

namespace wvd {

// Ensures the directory exists (recursive create).
bool EnsureDirectory(const std::wstring& path);

bool WriteFileAtomic(const std::wstring& path, const void* data, size_t len);
bool ReadFileWhole(const std::wstring& path, std::vector<uint8_t>& out);

inline bool WriteTextAtomic(const std::wstring& path, const std::string& s) {
  return WriteFileAtomic(path, s.data(), s.size());
}

// %ProgramData%\WVD. A privileged process creates it admin-only and refuses a
// pre-existing one that is a junction or not owned by SYSTEM/Administrators;
// then DataDir()/DataPath() return an empty string and callers fail closed.
std::wstring DataDir();
std::wstring DataPath(const wchar_t* leaf);

}  // namespace wvd
