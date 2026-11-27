// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rewriter/elmo_school_rewriter.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <string_view>
#include <vector>

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

bool IsKanjiRunChar(char32_t c) {
  return c == kIterationMark || IsKanji(c);
}

char32_t ToHiragana(char32_t c) {
  return (c >= 0x30A1 && c <= 0x30F6) ? c - 0x60 : c;
}

// One run of the surface: kanji (with its reading) or other characters.
struct Run {
  bool kanji = false;
  std::u32string text;
  std::u32string reading;  // Only for kanji runs.
};

// Aligns the kanji runs of `value` with the reading `key`, using the non-kanji
// runs as anchors (学校で勉強を / がっこうでべんきょうを ->
// [学校=がっこう][で][勉強=べんきょう][を]).  Returns false when the surface
// cannot be explained by the reading.
bool AlignRuns(const std::u32string& value, const std::u32string& key,
               std::vector<Run>* runs) {
  runs->clear();
  for (const char32_t c : value) {
    const bool k = IsKanjiRunChar(c);
    if (runs->empty() || runs->back().kanji != k) runs->push_back({k, {}, {}});
    runs->back().text.push_back(c);
  }
  std::u32string hira_key;
  for (const char32_t c : key) hira_key.push_back(ToHiragana(c));
  // Depth-first search over reading lengths for kanji runs.
  const std::function<bool(size_t, size_t)> match = [&](size_t r,
                                                        size_t pos) -> bool {
    if (r == runs->size()) return pos == hira_key.size();
    Run& run = (*runs)[r];
    if (!run.kanji) {
      if (pos + run.text.size() > hira_key.size()) return false;
      for (size_t i = 0; i < run.text.size(); ++i) {
        if (ToHiragana(run.text[i]) != hira_key[pos + i]) return false;
      }
      return match(r + 1, pos + run.text.size());
    }
    // Each kanji reads as 1..4 kana.
    const size_t min_len = run.text.size();
    const size_t max_len = run.text.size() * 4;
    for (size_t len = min_len; len <= max_len && pos + len <= hira_key.size();
         ++len) {
      run.reading = hira_key.substr(pos, len);
      if (match(r + 1, pos + len)) return true;
    }
    run.reading.clear();
    return false;
  };
  return match(0, 0);
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
  const bool grade_on = max_grade >= 1 && max_grade <= 6;
  const bool furigana_on = furigana_style == 1 || furigana_style == 2;
  if ((!grade_on && !furigana_on) || candidate->key.empty()) return false;
  const std::string_view open = furigana_style == 1 ? "（" : "(";
  const std::string_view close = furigana_style == 1 ? "）" : ")";

  // Rewrites one word (key/value split into content and functional parts).
  auto rewrite_word = [&](std::string_view key, std::string_view value,
                          std::string_view content_key,
                          std::string_view content_value) -> std::string {
    // Preferred: align each kanji run with its reading, so that taught words
    // keep their kanji and furigana goes right after each kanji run.
    std::vector<Run> runs;
    if (ContainsKanji(value) &&
        AlignRuns(Util::Utf8ToUtf32(value), Util::Utf8ToUtf32(key), &runs)) {
      std::string out;
      for (const Run& run : runs) {
        const std::string text = Util::Utf32ToUtf8(run.text);
        if (!run.kanji) {
          out += text;
          continue;
        }
        const std::string reading = Util::Utf32ToUtf8(run.reading);
        if (grade_on && !AllKanjiAllowed(text, max_grade)) {
          out += reading;
        } else if (furigana_on) {
          absl::StrAppend(&out, text, open, reading, close);
        } else {
          out += text;
        }
      }
      return out;
    }
    // Fallback: the whole word.
    if (grade_on && !AllKanjiAllowed(value, max_grade)) {
      return std::string(key);  // The whole word in hiragana.
    }
    if (furigana_on && !content_key.empty() &&
        content_value.size() <= value.size() &&
        value.substr(0, content_value.size()) == content_value &&
        ContainsKanji(content_value) && content_value != content_key) {
      return absl::StrCat(content_value, open, content_key, close,
                          value.substr(content_value.size()));
    }
    return std::string(value);
  };

  // A candidate can hold several words (e.g. 学校で|勉強を); handle each word
  // separately so that taught words keep their kanji.
  std::string new_value;
  if (candidate->inner_segment_boundary.empty()) {
    new_value = rewrite_word(candidate->key, candidate->value,
                             candidate->content_key, candidate->content_value);
  } else {
    for (const auto& word : candidate->inner_segments()) {
      new_value += rewrite_word(word.GetKey(), word.GetValue(),
                                word.GetContentKey(), word.GetContentValue());
    }
  }
  if (new_value == candidate->value) return false;

  if (candidate->inner_segment_boundary.empty() &&
      new_value.size() >= candidate->value.size() - candidate->content_value.size() &&
      new_value.ends_with(candidate->functional_value())) {
    // Keep the content/functional split for a single word.
    candidate->content_value = new_value.substr(
        0, new_value.size() - candidate->functional_value().size());
  } else {
    candidate->content_value = new_value;
    candidate->content_key = candidate->key;
  }
  candidate->value = std::move(new_value);
  candidate->inner_segment_boundary.clear();
  candidate->proper_noun_spans.clear();
  candidate->attributes |= converter::Attribute::NO_LEARNING;
  return true;
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
