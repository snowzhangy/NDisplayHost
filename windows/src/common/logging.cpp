#include "wvd/logging.h"

#include <cstdio>
#include <mutex>

#include "wvd/platform.h"

namespace wvd::log {

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
const char* g_ident = "wvd";
Level g_level = Level::Info;

const char* LevelTag(Level l) {
  switch (l) {
    case Level::Error: return "E";
    case Level::Warn: return "W";
    case Level::Info: return "I";
    default: return "V";
  }
}

}  // namespace

void Init(const char* ident, Level level) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_ident = ident ? ident : "wvd";
  g_level = level;
}

void SetLevel(Level level) { g_level = level; }
Level GetLevel() { return g_level; }

void SetFile(const wchar_t* path) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
  if (path && path[0]) {
    // Binary append: the lines are already UTF-8. (A ccs=UTF-8 stream is a
    // wide-text stream and the CRT rejects narrow fputs on it.)
    _wfopen_s(&g_file, path, L"ab");
  }
}

void WriteV(Level lvl, const char* fmt, va_list ap) {
  if (static_cast<int>(lvl) > static_cast<int>(g_level)) return;
  char body[2048];
  vsnprintf(body, sizeof(body), fmt, ap);

  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[2400];
  _snprintf_s(line, sizeof(line), _TRUNCATE,
              "%04d-%02d-%02d %02d:%02d:%02d.%03d %s %s: %s\n", st.wYear, st.wMonth,
              st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
              LevelTag(lvl), g_ident, body);

  std::lock_guard<std::mutex> lk(g_mutex);
  fputs(line, stderr);
  if (g_file) {
    fputs(line, g_file);
    fflush(g_file);
  }
  OutputDebugStringA(line);
}

void Write(Level lvl, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  WriteV(lvl, fmt, ap);
  va_end(ap);
}

}  // namespace wvd::log
