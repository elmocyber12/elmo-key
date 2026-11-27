// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gui/config_dialog/elmo_model_downloader.h"

#include <cstdint>
#include <string>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <bcrypt.h>
#include <bits.h>
#include <wrl/client.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
// clang-format on

#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#endif  // _WIN32

namespace mozc {
namespace gui {

#ifdef _WIN32
namespace {

using Microsoft::WRL::ComPtr;

// The model is pinned to a revision and checked by size and SHA-256.
constexpr wchar_t kModelUrl[] =
    L"https://huggingface.co/mmnga/sarashina2.2-3b-instruct-v0.1-gguf/"
    L"resolve/31d771319b/sarashina2.2-3b-instruct-v0.1-Q4_K_M.gguf";
constexpr wchar_t kModelFileName[] =
    L"sarashina2.2-3b-instruct-v0.1-Q4_K_M.gguf";
constexpr uint64_t kModelSize = 2066390112ULL;
constexpr char kModelSha256[] =
    "d96f4d98eb528df26e8bc09ab81a1d165be4fce67616739e65980bed9038f0f2";
constexpr wchar_t kJobName[] = L"ELMO_key AI model";

std::wstring PartPath() {
  return ElmoModelDownloader::ModelDirectory() + L"\\" + kModelFileName +
         L".part";
}
std::wstring FinalPath() {
  return ElmoModelDownloader::ModelDirectory() + L"\\" + kModelFileName;
}

bool EnsureDirectory(const std::wstring& dir) {
  // %ProgramData%\ELMO_key\models
  const size_t last = dir.find_last_of(L'\\');
  if (last != std::wstring::npos) {
    ::CreateDirectoryW(dir.substr(0, last).c_str(), nullptr);
  }
  ::CreateDirectoryW(dir.c_str(), nullptr);
  const DWORD attr = ::GetFileAttributesW(dir.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

ComPtr<IBackgroundCopyManager> Manager() {
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // Qt may already have.
  ComPtr<IBackgroundCopyManager> manager;
  if (FAILED(::CoCreateInstance(__uuidof(BackgroundCopyManager), nullptr,
                                CLSCTX_LOCAL_SERVER,
                                IID_PPV_ARGS(&manager)))) {
    return nullptr;
  }
  return manager;
}

ComPtr<IBackgroundCopyJob> FindJob(IBackgroundCopyManager* manager) {
  ComPtr<IEnumBackgroundCopyJobs> jobs;
  if (FAILED(manager->EnumJobs(0, &jobs))) return nullptr;
  ComPtr<IBackgroundCopyJob> job;
  while (jobs->Next(1, job.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
    LPWSTR name = nullptr;
    if (SUCCEEDED(job->GetDisplayName(&name)) && name != nullptr) {
      const bool match = wcscmp(name, kJobName) == 0;
      ::CoTaskMemFree(name);
      if (match) {
        BG_JOB_STATE state;
        if (SUCCEEDED(job->GetState(&state)) &&
            state != BG_JOB_STATE_ACKNOWLEDGED &&
            state != BG_JOB_STATE_CANCELLED) {
          return job;
        }
      }
    }
  }
  return nullptr;
}

bool VerifyFile(const std::wstring& path, std::wstring* error) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    *error = L"ダウンロードしたファイルが見つかりません。";
    return false;
  }
  const uint64_t size =
      (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  if (size != kModelSize) {
    *error = L"ダウンロードしたファイルの大きさが正しくありません。";
    return false;
  }
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
          &alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
      !BCRYPT_SUCCESS(
          ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
    if (alg != nullptr) ::BCryptCloseAlgorithmProvider(alg, 0);
    *error = L"ファイルの確認に失敗しました。";
    return false;
  }
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  bool ok = file != INVALID_HANDLE_VALUE;
  std::vector<unsigned char> buf(4 * 1024 * 1024);
  while (ok) {
    DWORD read = 0;
    if (!::ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &read,
                    nullptr)) {
      ok = false;
      break;
    }
    if (read == 0) break;
    ok = BCRYPT_SUCCESS(::BCryptHashData(hash, buf.data(), read, 0));
  }
  if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
  unsigned char digest[32] = {};
  ok = ok && BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest, 32, 0));
  ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(alg, 0);
  if (!ok) {
    *error = L"ファイルの確認に失敗しました。";
    return false;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  for (const unsigned char c : digest) {
    hex += kHex[c >> 4];
    hex += kHex[c & 0xF];
  }
  if (hex != kModelSha256) {
    *error = L"ダウンロードしたファイルが本物と一致しません。";
    return false;
  }
  return true;
}

// Verification runs on a worker thread so the settings window stays usable.
struct Verification {
  std::atomic<int> state{0};  // 0 = running, 1 = ok, 2 = failed
  std::wstring error;
};
std::shared_ptr<Verification>& CurrentVerification() {
  static auto* v = new std::shared_ptr<Verification>();
  return *v;
}

}  // namespace

std::wstring ElmoModelDownloader::ModelDirectory() {
  wchar_t buf[MAX_PATH];
  const DWORD n = ::GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
  const std::wstring base =
      (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L"C:\\ProgramData";
  return base + L"\\ELMO_key\\models";
}

bool ElmoModelDownloader::ModelInstalled() {
  WIN32_FIND_DATAW data;
  HANDLE find =
      ::FindFirstFileW((ModelDirectory() + L"\\*.gguf").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return false;
  ::FindClose(find);
  return true;
}

bool ElmoModelDownloader::Start() {
  error_.clear();
  const std::wstring dir = ModelDirectory();
  if (!EnsureDirectory(dir)) {
    error_ = L"保存先のフォルダーを作れませんでした：" + dir;
    state_ = State::kError;
    return false;
  }
  ULARGE_INTEGER free_bytes = {};
  if (::GetDiskFreeSpaceExW(dir.c_str(), &free_bytes, nullptr, nullptr) &&
      free_bytes.QuadPart < kModelSize + 200ULL * 1024 * 1024) {
    error_ = L"空き容量が足りません（約 2.1GB 必要です）。";
    state_ = State::kError;
    return false;
  }
  ComPtr<IBackgroundCopyManager> manager = Manager();
  if (!manager) {
    error_ = L"Windows のダウンロードの仕組み（BITS）を使えませんでした。";
    state_ = State::kError;
    return false;
  }
  ComPtr<IBackgroundCopyJob> job = FindJob(manager.Get());
  if (!job) {
    GUID id;
    if (FAILED(manager->CreateJob(kJobName, BG_JOB_TYPE_DOWNLOAD, &id,
                                  &job)) ||
        FAILED(job->AddFile(kModelUrl, PartPath().c_str()))) {
      error_ = L"ダウンロードを始められませんでした。";
      state_ = State::kError;
      return false;
    }
    job->SetPriority(BG_JOB_PRIORITY_FOREGROUND);
    job->SetDescription(L"ELMO_key: AI rewrite model");
  }
  job->Resume();
  state_ = State::kRunning;
  return true;
}

ElmoModelDownloader::State ElmoModelDownloader::Poll() {
  // Verification in progress or finished.
  if (auto& v = CurrentVerification(); v) {
    const int s = v->state.load();
    if (s == 0) return state_ = State::kVerifying;
    if (s == 1) {
      v.reset();
      ::DeleteFileW(FinalPath().c_str());
      if (!::MoveFileExW(PartPath().c_str(), FinalPath().c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
        error_ = L"モデルを置けませんでした。";
        return state_ = State::kError;
      }
      return state_ = State::kReady;
    }
    error_ = v->error;
    v.reset();
    ::DeleteFileW(PartPath().c_str());
    return state_ = State::kError;
  }
  if (ModelInstalled()) return state_ = State::kReady;

  ComPtr<IBackgroundCopyManager> manager = Manager();
  ComPtr<IBackgroundCopyJob> job =
      manager ? FindJob(manager.Get()) : nullptr;
  if (!job) {
    // A previous window may have finished the transfer already.
    if (::GetFileAttributesW(PartPath().c_str()) != INVALID_FILE_ATTRIBUTES) {
      auto v = std::make_shared<Verification>();
      CurrentVerification() = v;
      std::thread([v, path = PartPath()] {
        std::wstring err;
        const bool ok = VerifyFile(path, &err);
        v->error = err;
        v->state = ok ? 1 : 2;
      }).detach();
      return state_ = State::kVerifying;
    }
    return state_ = (state_ == State::kError ? State::kError
                                              : State::kNotStarted);
  }
  BG_JOB_PROGRESS progress = {};
  if (SUCCEEDED(job->GetProgress(&progress))) {
    bytes_done_ = progress.BytesTransferred;
    bytes_total_ = progress.BytesTotal == BG_SIZE_UNKNOWN
                       ? kModelSize
                       : progress.BytesTotal;
  }
  BG_JOB_STATE job_state;
  if (FAILED(job->GetState(&job_state))) return state_;
  switch (job_state) {
    case BG_JOB_STATE_SUSPENDED:
      job->Resume();
      return state_ = State::kRunning;
    case BG_JOB_STATE_QUEUED:
    case BG_JOB_STATE_CONNECTING:
    case BG_JOB_STATE_TRANSFERRING:
    case BG_JOB_STATE_TRANSIENT_ERROR:  // BITS retries by itself.
      return state_ = State::kRunning;
    case BG_JOB_STATE_ERROR: {
      error_ = L"ダウンロードに失敗しました。インターネットへの接続や、"
               L"会社のネットワークの制限を確かめてください。";
      ComPtr<IBackgroundCopyError> err;
      if (SUCCEEDED(job->GetError(&err))) {
        LPWSTR text = nullptr;
        if (SUCCEEDED(err->GetErrorDescription(
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), &text)) &&
            text != nullptr) {
          error_ += L"\n（";
          error_ += text;
          error_ += L"）";
          ::CoTaskMemFree(text);
        }
      }
      job->Cancel();
      return state_ = State::kError;
    }
    case BG_JOB_STATE_TRANSFERRED: {
      job->Complete();  // Moves the file to PartPath().
      auto v = std::make_shared<Verification>();
      CurrentVerification() = v;
      std::thread([v, path = PartPath()] {
        std::wstring err;
        const bool ok = VerifyFile(path, &err);
        v->error = err;
        v->state = ok ? 1 : 2;
      }).detach();
      return state_ = State::kVerifying;
    }
    default:
      return state_;
  }
}

void ElmoModelDownloader::Cancel() {
  ComPtr<IBackgroundCopyManager> manager = Manager();
  if (manager) {
    if (ComPtr<IBackgroundCopyJob> job = FindJob(manager.Get())) {
      job->Cancel();
    }
  }
  ::DeleteFileW(PartPath().c_str());
  state_ = State::kNotStarted;
}

#else  // _WIN32

std::wstring ElmoModelDownloader::ModelDirectory() { return L""; }
bool ElmoModelDownloader::ModelInstalled() { return false; }
bool ElmoModelDownloader::Start() {
  error_ = L"unsupported";
  state_ = State::kError;
  return false;
}
ElmoModelDownloader::State ElmoModelDownloader::Poll() { return state_; }
void ElmoModelDownloader::Cancel() {}

#endif  // _WIN32

}  // namespace gui
}  // namespace mozc
