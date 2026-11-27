// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOZC_GUI_CONFIG_DIALOG_ELMO_MODEL_DOWNLOADER_H_
#define MOZC_GUI_CONFIG_DIALOG_ELMO_MODEL_DOWNLOADER_H_

#include <cstdint>
#include <string>

namespace mozc {
namespace gui {

// Downloads the AI rewrite model for ELMO_key into
// %ProgramData%\ELMO_key\models with Windows BITS.  ELMO_key's own programs
// are blocked from the internet by the installer's firewall rules; BITS (a
// Windows service) performs the transfer only when the user asks for it.
// The finished file is checked by size and SHA-256 before it is used.
class ElmoModelDownloader {
 public:
  enum class State {
    kNotStarted,   // No download job.
    kRunning,      // Connecting or transferring.
    kVerifying,    // Transferred; checking size and SHA-256.
    kReady,        // The verified model is in place.
    kError,        // Failed; see error().
  };

  // True when a model file exists in %ProgramData%\ELMO_key\models.
  static bool ModelInstalled();
  // The folder the model goes to.
  static std::wstring ModelDirectory();

  // Starts (or resumes) the download.  Returns false with error() set when
  // it cannot start.
  bool Start();
  // Updates and returns the state.  Call periodically (e.g. every 500 ms).
  // Also picks up a job started in an earlier settings window.
  State Poll();
  // Cancels a running download and removes partial files.
  void Cancel();

  uint64_t bytes_done() const { return bytes_done_; }
  uint64_t bytes_total() const { return bytes_total_; }
  const std::wstring& error() const { return error_; }

 private:
  State state_ = State::kNotStarted;
  uint64_t bytes_done_ = 0;
  uint64_t bytes_total_ = 0;
  std::wstring error_;
};

}  // namespace gui
}  // namespace mozc

#endif  // MOZC_GUI_CONFIG_DIALOG_ELMO_MODEL_DOWNLOADER_H_
