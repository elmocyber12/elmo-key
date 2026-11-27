// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rewriter/elmo_school_rewriter.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "base/util.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"

namespace mozc {
namespace {

#include "rewriter/elmo_grade_kanji_data.inc"

constexpr char32_t kIterationMark = 0x3005;  // 々

bool IsKanji(char32_t c) {
  return c != kIterationMark && Util::GetScriptType(c) == Util::KANJI;
}

bool ContainsKanji(std::string_view text) {
  for (const char32_t c : Util::Utf8ToUtf32(text)) {
    if (IsKanji(c)) return true;
  }
  return false;
}

}  // namespace

ElmoSchoolRewriter::ElmoSchoolRewriter() {
  for (int grade = 1; grade <= 6; ++grade) {
    for (const char32_t c : Util::Utf8ToUtf32(kElmoGradeKanji[grade - 1])) {
      grade_of_.emplace(c, grade);
    }
  }
}

int ElmoSchoolRewriter::GradeOf(char32_t c) const {
  const auto it = grade_of_.find(c);
  return it == grade_of_.end() ? 0 : it->second;
}

bool ElmoSchoolRewriter::AllKanjiAllowed(std::string_view text,
                                         int max_grade) const {
  for (const char32_t c : Util::Utf8ToUtf32(text)) {
    if (!IsKanji(c)) continue;
    const int grade = GradeOf(c);
    if (grade == 0 || grade > max_grade) return false;
  }
  return true;
}

int ElmoSchoolRewriter::capability(const ConversionRequest& request) const {
  return RewriterInterface::ALL;
}

bool ElmoSchoolRewriter::RewriteCandidate(
    int max_grade, uint32_t furigana_style,
    converter::Candidate* candidate) const {
  bool changed = false;
  if (max_grade >= 1 && max_grade <= 6 && !candidate->key.empty() &&
      !AllKanjiAllowed(candidate->value, max_grade)) {
    // Show the whole word in hiragana (its reading).
    candidate->value = candidate->key;
    candidate->content_value = candidate->content_key;
    changed = true;
  }
  if (furigana_style == 1 || furigana_style == 2) {
    const std::string& content_value = candidate->content_value;
    if (!candidate->content_key.empty() && content_value.size() <= candidate->value.size() &&
        candidate->value.starts_with(content_value) &&
        ContainsKanji(content_value) &&
        content_value != candidate->content_key) {
      const std::string_view open = furigana_style == 1 ? "（" : "(";
      const std::string_view close = furigana_style == 1 ? "）" : ")";
      const std::string functional =
          candidate->value.substr(content_value.size());
      std::string new_content =
          absl::StrCat(content_value, open, candidate->content_key, close);
      candidate->value = absl::StrCat(new_content, functional);
      candidate->content_value = std::move(new_content);
      changed = true;
    }
  }
  if (changed) {
    candidate->attributes |= converter::Attribute::NO_LEARNING;
  }
  return changed;
}

bool ElmoSchoolRewriter::Rewrite(const ConversionRequest& request,
                                 Segments* segments) const {
  const config::Config& config = request.config();
  const int max_grade = static_cast<int>(config.elmo_kanji_grade());
  const uint32_t furigana_style = config.elmo_furigana_style();
  if ((max_grade < 1 || max_grade > 6) &&
      furigana_style != 1 && furigana_style != 2) {
    return false;
  }

  bool modified = false;
  for (Segment& segment : segments->conversion_segments()) {
    bool segment_changed = false;
    for (size_t i = 0; i < segment.candidates_size(); ++i) {
      segment_changed |= RewriteCandidate(max_grade, furigana_style,
                                          segment.mutable_candidate(i));
    }
    if (!segment_changed) continue;
    modified = true;
    // Several candidates can collapse into the same hiragana surface; keep
    // the first (best ranked) one.
    absl::flat_hash_set<std::string> seen;
    for (size_t i = 0; i < segment.candidates_size();) {
      if (!seen.insert(segment.candidate(i).value).second) {
        segment.erase_candidate(i);
      } else {
        ++i;
      }
    }
  }
  return modified;
}

}  // namespace mozc
