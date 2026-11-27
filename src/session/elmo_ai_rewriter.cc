// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "session/elmo_ai_rewriter.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <userenv.h>
// clang-format on

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#endif  // _WIN32

namespace mozc {
namespace session {
namespace {

// ---------------------------------------------------------------------------
// Prompts.  Few-shot examples keep the small model from answering the text
// instead of rewriting it (see docs/keigo-model-test.md).

struct Shot {
  const char* input;
  const char* output;
};

struct ModeSpec {
  const char* system;
  const char* instruction;
  Shot shots[2];
};

constexpr ModeSpec kPoliteSpec = {
    "あなたは日本語の文章を整えるアシスタントです。"
    "ユーザーが送る文を、社内外のビジネスメールで使える丁寧な日本語（敬語）に"
    "書き直してください。意味は変えず、情報（日付・時刻・数字など）を足さず、書き直した文だけを"
    "出力してください。",
    "次の文を丁寧な敬語に書き直してください。文への返事はせず、"
    "書き直した文だけを出力してください。",
    {{"会議の時間、3時に変わったから",
      "会議の開始時刻が15時に変更となりましたので、お知らせいたします。"},
     {"それ、あとで送っといて",
      "そちらの件、後ほどお送りいただけますでしょうか。"}},
};

constexpr ModeSpec kEnglishSpec = {
    "You are a professional Japanese-to-English translator for business "
    "email. Translate the user's Japanese text into natural, polite business "
    "English. Do not reply to the text, do not add information, and output "
    "only the English translation.",
    "次の日本語を英語に翻訳してください。返事はせず、英訳だけを出力してください。",
    {{"会議の開始時刻が15時に変更となりました。",
      "The meeting start time has been changed to 3:00 p.m."},
     {"それ、あとで送っといて", "Could you send that to me later?"}},
};

constexpr ModeSpec kPlainSpec = {
    "あなたは日本語の文章を整えるアシスタントです。"
    "ユーザーが送る文を、報告書や議事録で使う「だ・である」調の文に書き直して"
    "ください。意味は変えず、情報（日付・時刻・数字など）を足さず、書き直した文だけを出力してください。",
    "次の文を「だ・である」調に書き直してください。文への返事はせず、"
    "書き直した文だけを出力してください。",
    {{"会議の開始時刻が15時に変更となりました。",
      "会議の開始時刻は15時に変更となった。"},
     {"来週までに資料を準備します。", "来週までに資料を準備する。"}},
};

constexpr ModeSpec kEasySpec = {
    "あなたは「やさしい日本語」に書き直すアシスタントです。"
    "ユーザーが送る文を、子どもや日本語に慣れていない人にも分かる、短くて"
    "やさしい日本語に書き直してください。難しい言葉はやさしい言葉に言い換え、"
    "一文を短くし、「です・ます」で書いてください。意味は変えず、"
    "元の文にない日付・時刻・数字・情報を足さず、"
    "書き直した文だけを出力してください。",
    "次の文をやさしい日本語に書き直してください。文への返事はせず、"
    "書き直した文だけを出力してください。",
    {{"明日は荒天が予想されるため、休校とします。",
      "あしたは 天気が とても 悪く なります。学校は 休みです。"},
     {"提出期限は今月末日です。",
      "今月の 最後の 日までに 出して ください。"}},
};

const ModeSpec& SpecFor(ElmoRewriteMode mode) {
  switch (mode) {
    case ElmoRewriteMode::kPolite:
      return kPoliteSpec;
    case ElmoRewriteMode::kEnglish:
      return kEnglishSpec;
    case ElmoRewriteMode::kPlain:
      return kPlainSpec;
    case ElmoRewriteMode::kEasy:
      return kEasySpec;
  }
  return kPoliteSpec;
}

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0xF];
        } else {
          out += ch;
        }
    }
  }
  return out;
}

void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parses a JSON string literal starting at s[pos] == '"'.
bool ParseJsonString(std::string_view s, size_t pos, std::string* out) {
  if (pos >= s.size() || s[pos] != '"') return false;
  out->clear();
  for (size_t i = pos + 1; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') return true;
    if (c != '\\') {
      out->push_back(c);
      continue;
    }
    if (++i >= s.size()) return false;
    switch (s[i]) {
      case '"':
        out->push_back('"');
        break;
      case '\\':
        out->push_back('\\');
        break;
      case '/':
        out->push_back('/');
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case 'u': {
        auto read4 = [&](size_t at, uint32_t* v) {
          if (at + 4 > s.size()) return false;
          uint32_t r = 0;
          for (size_t k = 0; k < 4; ++k) {
            const int h = HexValue(s[at + k]);
            if (h < 0) return false;
            r = (r << 4) | static_cast<uint32_t>(h);
          }
          *v = r;
          return true;
        };
        uint32_t cp = 0;
        if (!read4(i + 1, &cp)) return false;
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() &&
            s[i + 1] == '\\' && s[i + 2] == 'u') {
          uint32_t low = 0;
          if (read4(i + 3, &low) && low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            i += 6;
          }
        }
        AppendUtf8(cp, out);
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

std::string Trim(std::string_view s) {
  auto is_space = [](std::string_view rest) -> size_t {
    if (rest.empty()) return 0;
    const unsigned char c = static_cast<unsigned char>(rest[0]);
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return 1;
    if (rest.substr(0, 3) == "\xE3\x80\x80") return 3;  // U+3000
    return 0;
  };
  while (size_t n = is_space(s)) s.remove_prefix(n);
  while (!s.empty()) {
    const unsigned char c = static_cast<unsigned char>(s.back());
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      s.remove_suffix(1);
    } else if (s.size() >= 3 && s.substr(s.size() - 3) == "\xE3\x80\x80") {
      s.remove_suffix(3);
    } else {
      break;
    }
  }
  return std::string(s);
}

bool Contains(std::string_view s, std::string_view part) {
  return s.find(part) != std::string_view::npos;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.substr(0, prefix.size()) == prefix;
}

}  // namespace

std::string ElmoAiRewriter::BuildChatRequestJson(ElmoRewriteMode mode,
                                                 std::string_view text) {
  const ModeSpec& spec = SpecFor(mode);
  auto wrap = [&](std::string_view t) {
    std::string s = spec.instruction;
    s += "\n";
    s += mode == ElmoRewriteMode::kEnglish ? "日本語：「" : "文：「";
    s += t;
    s += "」";
    return s;
  };
  std::string json = "{\"messages\":[";
  json += "{\"role\":\"system\",\"content\":\"" + JsonEscape(spec.system) +
          "\"}";
  for (const Shot& shot : spec.shots) {
    json += ",{\"role\":\"user\",\"content\":\"" + JsonEscape(wrap(shot.input)) +
            "\"}";
    json += ",{\"role\":\"assistant\",\"content\":\"" +
            JsonEscape(shot.output) + "\"}";
  }
  json += ",{\"role\":\"user\",\"content\":\"" + JsonEscape(wrap(text)) + "\"}";
  // Output length: rewritten Japanese is usually 1-3x the input; English
  // tokens are shorter but more numerous.
  const size_t max_tokens =
      std::min<size_t>(1024, 64 + text.size() * (mode == ElmoRewriteMode::kEnglish ? 2 : 1));
  json += "],\"temperature\":0.2,\"max_tokens\":" + std::to_string(max_tokens) +
          ",\"stream\":false}";
  return json;
}

bool ElmoAiRewriter::ExtractMessageContent(std::string_view json,
                                           std::string* out) {
  const size_t message = json.find("\"message\"");
  if (message == std::string_view::npos) return false;
  size_t key = json.find("\"content\"", message);
  if (key == std::string_view::npos) return false;
  size_t pos = json.find(':', key + 9);
  if (pos == std::string_view::npos) return false;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
                               json[pos] == '\r' || json[pos] == '\t')) {
    ++pos;
  }
  return ParseJsonString(json, pos, out);
}

bool ElmoAiRewriter::CleanAnswer(ElmoRewriteMode mode,
                                 std::string_view original,
                                 std::string* answer) {
  std::string s = *answer;
  for (const std::string_view junk : {"</s>", "<|im_end|>", "<|eot_id|>"}) {
    size_t at;
    while ((at = s.find(junk)) != std::string::npos) s.erase(at, junk.size());
  }
  s = Trim(s);
  // Strip one pair of wrapping quotes the model may copy from the prompt.
  const std::pair<std::string_view, std::string_view> quotes[] = {
      {"「", "」"}, {"\"", "\""}, {"“", "”"}};
  for (const auto& [open, close] : quotes) {
    if (s.size() > open.size() + close.size() && StartsWith(s, open) &&
        s.substr(s.size() - close.size()) == close) {
      s = Trim(std::string_view(s).substr(
          open.size(), s.size() - open.size() - close.size()));
      break;
    }
  }
  // The prompt wraps the text in 「」; drop a bracket the model echoed
  // without its pair (e.g. "The meeting ... 3:00 PM.」").
  if (!Contains(original, "」") && !Contains(s, "「")) {
    while (s.size() >= 3 && s.substr(s.size() - 3) == "」") {
      s = Trim(std::string_view(s).substr(0, s.size() - 3));
    }
  }
  if (!Contains(original, "「") && !Contains(s, "」")) {
    while (StartsWith(s, "「")) s = Trim(std::string_view(s).substr(3));
  }
  if (s.empty()) return false;
  if (s.size() > original.size() * 6 + 200) return false;

  if (mode == ElmoRewriteMode::kEnglish) {
    const bool has_latin = std::any_of(s.begin(), s.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
    if (!has_latin) return false;
  } else {
    // Reject an answer to the text ("かしこまりました…") unless the
    // original text itself is an acknowledgement or thanks.
    const bool original_is_ack =
        Contains(original, "了解") || Contains(original, "承知") ||
        Contains(original, "わかった") || Contains(original, "分かった") ||
        Contains(original, "ありがと") || Contains(original, "かしこま") ||
        Contains(original, "OK") || Contains(original, "おk");
    if (!original_is_ack) {
      for (const std::string_view reply :
           {"かしこまりました", "承知いたしました", "承知しました",
            "了解いたしました", "了解しました", "ありがとうございます"}) {
        if (StartsWith(s, reply)) return false;
      }
    }
  }
  // Reject invented numbers (dates, times, amounts): every digit in the
  // answer must also appear in the original.  The model may convert kanji
  // numerals to digits, so kanji numerals in the original allow any digit.
  {
    auto digits_of = [](std::string_view text) {
      std::string d;
      for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= '0' && c <= '9') {
          d.push_back(static_cast<char>(c));
        } else if (c == 0xEF && i + 2 < text.size() &&
                   static_cast<unsigned char>(text[i + 1]) == 0xBC &&
                   static_cast<unsigned char>(text[i + 2]) >= 0x90 &&
                   static_cast<unsigned char>(text[i + 2]) <= 0x99) {
          d.push_back(static_cast<char>(
              '0' + (static_cast<unsigned char>(text[i + 2]) - 0x90)));
          i += 2;
        }
      }
      return d;
    };
    const std::string original_digits = digits_of(original);
    bool original_has_kanji_numeral = false;
    for (const std::string_view numeral :
         {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十", "百",
          "千", "万", "〇", "零"}) {
      if (Contains(original, numeral)) original_has_kanji_numeral = true;
    }
    // English often rewrites times (15時 -> 3:00 p.m.), so only Japanese
    // rewrites are checked.
    if (mode != ElmoRewriteMode::kEnglish && !original_has_kanji_numeral) {
      for (const char d : digits_of(s)) {
        if (original_digits.find(d) == std::string::npos) return false;
      }
    }
  }
  *answer = std::move(s);
  return true;
}

#ifdef _WIN32
namespace {

constexpr auto kIdleShutdown = std::chrono::minutes(10);
constexpr DWORD kReadyTimeoutMsec = 120 * 1000;
constexpr DWORD kRequestTimeoutMsec = 60 * 1000;

std::wstring GetEnvW(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = ::GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
  if (n == 0 || n >= ARRAYSIZE(buf)) return L"";
  return std::wstring(buf, n);
}

// The user's profile folder from the process token.  mozc_server can be
// started by an application whose environment differs (e.g. an app
// container where %LOCALAPPDATA% points into a package folder), so the
// environment variables alone are not reliable.
std::wstring UserProfileFromToken() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return L"";
  }
  wchar_t buf[MAX_PATH];
  DWORD size = MAX_PATH;
  const bool ok = ::GetUserProfileDirectoryW(token, buf, &size) != FALSE;
  ::CloseHandle(token);
  return ok ? std::wstring(buf) : L"";
}

std::string WideToUtf8Simple(const std::wstring& w) {
  if (w.empty()) return "";
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
  std::string out(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
  return out;
}

// Diagnostic log for the trial feature (no typed text is written):
// %USERPROFILE%\AppData\LocalLow\Mozc\elmo_ai_rewrite.log
void DiagLog(const std::string& line) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  std::wstring profile = UserProfileFromToken();
  if (profile.empty()) profile = GetEnvW(L"USERPROFILE");
  if (profile.empty()) return;
  const std::wstring path = profile + L"\\AppData\\LocalLow\\Mozc\\elmo_ai_rewrite.log";
  HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER size = {};
  if (::GetFileSizeEx(file, &size) && size.QuadPart > 512 * 1024) {
    ::CloseHandle(file);
    return;  // Keep the log small.
  }
  SYSTEMTIME t;
  ::GetLocalTime(&t);
  char head[64];
  std::snprintf(head, sizeof(head), "%02d:%02d:%02d pid=%lu ", t.wHour,
                t.wMinute, t.wSecond, ::GetCurrentProcessId());
  const std::string text = std::string(head) + line + "\r\n";
  DWORD written = 0;
  ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
              nullptr);
  ::CloseHandle(file);
}

std::wstring FindModelPath() {
  std::vector<std::wstring> bases;
  // ProgramData first.  When mozc_server is started by a packaged app (e.g.
  // the Windows 11 Notepad), the user's AppData folders created by the user
  // are not visible to it (seen on a real PC as ERROR_PATH_NOT_FOUND for both
  // AppData\Local\ELMO_key and AppData\LocalLow\ELMO_key), while ProgramData
  // is.  AppData locations remain as fallbacks for older setups.
  bases.push_back(GetEnvW(L"ProgramData"));
  bases.push_back(L"C:\\ProgramData");
  const std::wstring profile = UserProfileFromToken();
  if (!profile.empty()) {
    bases.push_back(profile + L"\\AppData\\LocalLow");
    bases.push_back(profile + L"\\AppData\\Local");
  }
  bases.push_back(GetEnvW(L"LOCALAPPDATA"));
  for (const std::wstring& base : bases) {
    if (base.empty()) continue;
    const std::wstring dir = base + L"\\ELMO_key\\models\\";
    WIN32_FIND_DATAW data;
    HANDLE find = ::FindFirstFileW((dir + L"*.gguf").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
      DiagLog("model search miss dir=" + WideToUtf8Simple(dir) +
              " error=" + std::to_string(::GetLastError()));
      continue;
    }
    std::wstring found;
    do {
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        found = dir + data.cFileName;
        break;
      }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    if (!found.empty()) return found;
  }
  return L"";
}

std::wstring ModuleDirectory() {
  wchar_t path[MAX_PATH];
  HMODULE module = nullptr;
  ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module);
  const DWORD n = ::GetModuleFileNameW(module, path, MAX_PATH);
  std::wstring p(path, n);
  const size_t slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

bool EnsureWinsock() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA data;
    ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  return ok;
}

int FindFreeLoopbackPort() {
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return 0;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int port = 0;
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    int len = sizeof(addr);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
      port = ntohs(addr.sin_port);
    }
  }
  ::closesocket(s);
  return port;
}

std::string RandomHex(size_t bytes) {
  std::string out;
  std::vector<unsigned char> buf(bytes);
  // RtlGenRandom via SystemFunction036.
  using RtlGenRandomFn = BOOLEAN(WINAPI*)(PVOID, ULONG);
  static RtlGenRandomFn fn = reinterpret_cast<RtlGenRandomFn>(::GetProcAddress(
      ::LoadLibraryW(L"advapi32.dll"), "SystemFunction036"));
  if (fn == nullptr || !fn(buf.data(), static_cast<ULONG>(buf.size()))) {
    const uint64_t seed = ::GetTickCount64() ^ ::GetCurrentProcessId();
    for (size_t i = 0; i < bytes; ++i) {
      buf[i] = static_cast<unsigned char>((seed >> ((i % 8) * 8)) + i * 131);
    }
  }
  static constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char c : buf) {
    out += kHex[c >> 4];
    out += kHex[c & 0xF];
  }
  return out;
}

// Minimal HTTP/1.1 client for 127.0.0.1 (Connection: close).
bool HttpRequest(int port, std::string_view method, std::string_view path,
                 std::string_view api_key, std::string_view body,
                 DWORD timeout_msec, int* status, std::string* response_body) {
  if (!EnsureWinsock()) return false;
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_msec),
               sizeof(timeout_msec));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_msec),
               sizeof(timeout_msec));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<u_short>(port));
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::closesocket(s);
    return false;
  }
  std::string req;
  req += std::string(method) + " " + std::string(path) + " HTTP/1.1\r\n";
  req += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
  req += "Authorization: Bearer " + std::string(api_key) + "\r\n";
  req += "Connection: close\r\n";
  if (!body.empty()) {
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "\r\n";
  req += body;
  size_t sent = 0;
  while (sent < req.size()) {
    const int n = ::send(s, req.data() + sent,
                         static_cast<int>(req.size() - sent), 0);
    if (n <= 0) {
      ::closesocket(s);
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  std::string raw;
  char buf[8192];
  for (;;) {
    const int n = ::recv(s, buf, sizeof(buf), 0);
    if (n <= 0) break;
    raw.append(buf, static_cast<size_t>(n));
    if (raw.size() > 16 * 1024 * 1024) break;
  }
  ::closesocket(s);
  const size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos || raw.size() < 12) return false;
  *status = std::atoi(raw.c_str() + 9);
  std::string headers = raw.substr(0, header_end);
  std::string payload = raw.substr(header_end + 4);
  std::transform(headers.begin(), headers.end(), headers.begin(),
                 [](char c) { return static_cast<char>(::tolower(c)); });
  if (headers.find("transfer-encoding: chunked") != std::string::npos) {
    std::string decoded;
    size_t pos = 0;
    while (pos < payload.size()) {
      const size_t line_end = payload.find("\r\n", pos);
      if (line_end == std::string::npos) break;
      const size_t size = std::strtoul(payload.c_str() + pos, nullptr, 16);
      if (size == 0) break;
      decoded.append(payload, line_end + 2, size);
      pos = line_end + 2 + size + 2;
    }
    payload = std::move(decoded);
  }
  *response_body = std::move(payload);
  return true;
}

struct RequestState {
  ElmoRewriteStatus status = ElmoRewriteStatus::kPending;
  std::string value;
  std::string reason;
};

class Engine {
 public:
  static Engine& Get() {
    static Engine* engine = new Engine();  // Intentionally leaked.
    return *engine;
  }

  uint64_t Start(ElmoRewriteMode mode, std::string text, std::string* reason) {
    // Reserve a slot atomically: at most 2 rewrites run at once even when
    // several sessions start one at the same moment.
    int current = active_.load();
    do {
      if (current >= 2) {
        DiagLog("start: busy");
        *reason = "busy";
        return 0;
      }
    } while (!active_.compare_exchange_weak(current, current + 1));
    const std::wstring model = FindModelPath();
    if (model.empty()) {
      --active_;
      DiagLog("start: no model found");
      *reason = "no_model";
      return 0;
    }
    DiagLog("start: model=" + WideToUtf8Simple(model) +
            " chars=" + std::to_string(text.size()));
    const uint64_t id = ++next_id_;
    {
      std::lock_guard<std::mutex> lock(requests_mutex_);
      requests_[id] = RequestState();
    }
    std::thread([this, id, mode, text = std::move(text), model] {
      RequestState result = Run(mode, text, model);
      --active_;
      std::lock_guard<std::mutex> lock(requests_mutex_);
      auto it = requests_.find(id);
      if (it != requests_.end()) it->second = std::move(result);
    }).detach();
    return id;
  }

  ElmoRewritePollResult Poll(uint64_t id) {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    ElmoRewritePollResult r;
    auto it = requests_.find(id);
    if (it == requests_.end()) {
      r.status = ElmoRewriteStatus::kFailed;
      r.reason = "unknown_request";
      return r;
    }
    r.status = it->second.status;
    r.value = it->second.value;
    r.reason = it->second.reason;
    if (r.status != ElmoRewriteStatus::kPending) requests_.erase(it);
    return r;
  }

  void Cancel(uint64_t id) {
    std::lock_guard<std::mutex> lock(requests_mutex_);
    auto it = requests_.find(id);
    if (it == requests_.end()) return;
    // A running worker finds no slot when it finishes and drops its result.
    requests_.erase(it);
  }

 private:
  Engine() = default;

  RequestState Run(ElmoRewriteMode mode, const std::string& text,
                   const std::wstring& model) {
    RequestState result;
    int port = 0;
    std::string key;
    if (!EnsureServer(model, &port, &key)) {
      DiagLog("run: server_start_failed");
      result.status = ElmoRewriteStatus::kFailed;
      result.reason = "server_start_failed";
      return result;
    }
    const std::string body = ElmoAiRewriter::BuildChatRequestJson(mode, text);
    int status = 0;
    std::string response;
    const bool ok = HttpRequest(port, "POST", "/v1/chat/completions", key,
                                body, kRequestTimeoutMsec, &status, &response);
    Touch();
    std::string answer;
    if (!ok || status != 200 ||
        !ElmoAiRewriter::ExtractMessageContent(response, &answer) ||
        !ElmoAiRewriter::CleanAnswer(mode, text, &answer)) {
      result.status = ElmoRewriteStatus::kFailed;
      result.reason = ok ? (status == 200 ? "unusable_answer" : "http_error")
                         : "request_failed";
      DiagLog("run: failed reason=" + result.reason +
              " http=" + std::to_string(status));
      return result;
    }
    DiagLog("run: done");
    result.status = ElmoRewriteStatus::kDone;
    result.value = std::move(answer);
    return result;
  }

  void Touch() {
    std::lock_guard<std::mutex> lock(server_mutex_);
    last_used_ = std::chrono::steady_clock::now();
  }

  bool IsRunningLocked() const {
    return process_ != nullptr &&
           ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
  }

  void StopLocked() {
    if (job_ != nullptr) {
      ::TerminateJobObject(job_, 0);
      ::CloseHandle(job_);
      job_ = nullptr;
    } else if (process_ != nullptr) {
      ::TerminateProcess(process_, 0);
    }
    if (process_ != nullptr) {
      ::CloseHandle(process_);
      process_ = nullptr;
    }
    port_ = 0;
    key_.clear();
  }

  bool EnsureServer(const std::wstring& model, int* port, std::string* key) {
    std::lock_guard<std::mutex> lock(server_mutex_);
    last_used_ = std::chrono::steady_clock::now();
    if (IsRunningLocked() && model == model_) {
      *port = port_;
      *key = key_;
      return true;
    }
    StopLocked();
    if (!EnsureWinsock()) return false;

    const std::wstring exe = ModuleDirectory() + L"\\llama-server.exe";
    const int new_port = FindFreeLoopbackPort();
    if (new_port == 0) return false;
    const std::string new_key = RandomHex(24);
    std::wstring cmd = L"\"" + exe + L"\" -m \"" + model +
                       L"\" --host 127.0.0.1 --port " +
                       std::to_wstring(new_port) +
                       L" -c 2048 -t 4 --parallel 1 --api-key " +
                       std::wstring(new_key.begin(), new_key.end());
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
      info.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                sizeof(info));
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    const std::wstring dir = ModuleDirectory();
    if (!::CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                          dir.c_str(), &startup, &pi)) {
      DiagLog("server: CreateProcess failed error=" +
              std::to_string(::GetLastError()));
      if (job != nullptr) ::CloseHandle(job);
      return false;
    }
    DiagLog("server: launched pid=" + std::to_string(pi.dwProcessId));
    if (job != nullptr && !::AssignProcessToJobObject(job, pi.hProcess)) {
      ::CloseHandle(job);
      job = nullptr;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    job_ = job;
    port_ = new_port;
    key_ = new_key;
    model_ = model;

    // Wait until the model is loaded.
    const ULONGLONG deadline = ::GetTickCount64() + kReadyTimeoutMsec;
    bool ready = false;
    while (::GetTickCount64() < deadline) {
      if (::WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) break;
      int status = 0;
      std::string body;
      if (HttpRequest(port_, "GET", "/health", key_, "", 2000, &status,
                      &body) &&
          status == 200) {
        ready = true;
        break;
      }
      ::Sleep(300);
    }
    if (!ready) {
      DiagLog("server: not ready (exited or timeout)");
      StopLocked();
      return false;
    }
    DiagLog("server: ready");
    StartIdleWatcherLocked();
    *port = port_;
    *key = key_;
    return true;
  }

  void StartIdleWatcherLocked() {
    if (watcher_started_) return;
    watcher_started_ = true;
    std::thread([this] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::lock_guard<std::mutex> lock(server_mutex_);
        if (process_ != nullptr && active_.load() == 0 &&
            std::chrono::steady_clock::now() - last_used_ > kIdleShutdown) {
          StopLocked();
        }
      }
    }).detach();
  }

  std::atomic<uint64_t> next_id_{0};
  std::atomic<int> active_{0};
  std::mutex requests_mutex_;
  std::unordered_map<uint64_t, RequestState> requests_;

  std::mutex server_mutex_;
  HANDLE process_ = nullptr;
  HANDLE job_ = nullptr;
  int port_ = 0;
  std::string key_;
  std::wstring model_;
  std::chrono::steady_clock::time_point last_used_;
  bool watcher_started_ = false;
};

}  // namespace

ElmoAiRewriter& ElmoAiRewriter::Get() {
  static ElmoAiRewriter* rewriter = new ElmoAiRewriter();
  return *rewriter;
}

uint64_t ElmoAiRewriter::Start(ElmoRewriteMode mode, std::string_view text,
                               std::string* reason) {
  if (text.empty()) {
    *reason = "empty_text";
    return 0;
  }
  return Engine::Get().Start(mode, std::string(text), reason);
}

ElmoRewritePollResult ElmoAiRewriter::Poll(uint64_t id) {
  return Engine::Get().Poll(id);
}

void ElmoAiRewriter::Cancel(uint64_t id) { Engine::Get().Cancel(id); }

bool ElmoAiRewriter::HasModel() { return !FindModelPath().empty(); }

void ElmoAiRewriter::Log(std::string_view line) { DiagLog(std::string(line)); }

#else  // _WIN32

void ElmoAiRewriter::Log(std::string_view) {}

ElmoAiRewriter& ElmoAiRewriter::Get() {
  static ElmoAiRewriter* rewriter = new ElmoAiRewriter();
  return *rewriter;
}

uint64_t ElmoAiRewriter::Start(ElmoRewriteMode, std::string_view,
                               std::string* reason) {
  *reason = "unsupported_platform";
  return 0;
}

ElmoRewritePollResult ElmoAiRewriter::Poll(uint64_t) {
  ElmoRewritePollResult r;
  r.status = ElmoRewriteStatus::kFailed;
  r.reason = "unsupported_platform";
  return r;
}

void ElmoAiRewriter::Cancel(uint64_t) {}

bool ElmoAiRewriter::HasModel() { return false; }

#endif  // _WIN32

}  // namespace session
}  // namespace mozc
