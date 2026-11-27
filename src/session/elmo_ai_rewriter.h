// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOZC_SESSION_ELMO_AI_REWRITER_H_
#define MOZC_SESSION_ELMO_AI_REWRITER_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace mozc {
namespace session {

// What to do with the text.
enum class ElmoRewriteMode {
  kPolite = 0,   // Ctrl+T: polite business Japanese (keigo).
  kEnglish = 1,  // Ctrl+E: English translation.
  kPlain = 2,    // Ctrl+R: plain "だ・である" style.
  kEasy = 3,     // Ctrl+Y: easy Japanese (やさしい日本語).
};

enum class ElmoRewriteStatus {
  kPending,
  kDone,
  kFailed,
};

struct ElmoRewritePollResult {
  ElmoRewriteStatus status = ElmoRewriteStatus::kFailed;
  std::string value;   // Rewritten text when kDone.
  std::string reason;  // Short, text-free reason when kFailed.
};

// ELMO_key AI rewrite (trial feature).
//
// Runs a separate llama-server (bundled llama-server.exe) with a separately
// downloaded instruct model on 127.0.0.1 with a random API key, only after
// the first request, and stops it after a period of inactivity.  Requests
// run on a worker thread; the session polls with Poll().  Text never leaves
// the PC.
//
// Model search order (first *.gguf found):
//   1. folder "%LOCALAPPDATA%/ELMO_key/models"
//   2. folder "%ProgramData%/ELMO_key/models"
class ElmoAiRewriter {
 public:
  static ElmoAiRewriter& Get();

  // Starts a rewrite and returns its id (> 0), or 0 when it could not start
  // (e.g. no model file); `reason` is set then.
  uint64_t Start(ElmoRewriteMode mode, std::string_view text,
                 std::string* reason);

  // Returns the state of request `id`.  A finished result is returned once
  // and then forgotten.
  ElmoRewritePollResult Poll(uint64_t id);

  // Forgets request `id` (its result is discarded when it finishes).
  void Cancel(uint64_t id);

  // True when a model file exists in one of the search locations.
  static bool HasModel();

  // Appends a diagnostic line (never typed text) to
  // %USERPROFILE%/AppData/LocalLow/Mozc/elmo_ai_rewrite.log on Windows.
  static void Log(std::string_view line);

  // Exposed for tests.
  static std::string BuildChatRequestJson(ElmoRewriteMode mode,
                                          std::string_view text);
  static bool ExtractMessageContent(std::string_view json, std::string* out);
  // Cleans a raw model answer; returns false when it should not be used.
  static bool CleanAnswer(ElmoRewriteMode mode, std::string_view original,
                          std::string* answer);

 private:
  ElmoAiRewriter() = default;
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_ELMO_AI_REWRITER_H_
