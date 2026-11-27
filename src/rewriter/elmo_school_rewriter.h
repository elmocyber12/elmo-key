// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOZC_REWRITER_ELMO_SCHOOL_REWRITER_H_
#define MOZC_REWRITER_ELMO_SCHOOL_REWRITER_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"

namespace mozc {

// ELMO_key school features.
//
// - Grade-level kanji mode (config.elmo_kanji_grade = 1..6): a candidate that
//   contains a kanji not taught up to the selected elementary school grade
//   (MEXT grade-level kanji table) is shown in hiragana (its reading).
// - Furigana output (config.elmo_furigana_style = 1..2): the reading is
//   appended after the content word, e.g. "漢字（かんじ）".
//
// Rewritten candidates are marked NO_LEARNING so that the modified surface is
// never learned into the normal history.
class ElmoSchoolRewriter : public RewriterInterface {
 public:
  ElmoSchoolRewriter();

  int capability(const ConversionRequest& request) const override;
  bool Rewrite(const ConversionRequest& request,
               Segments* segments) const override;

  // Returns the grade (1..6) in which `c` is taught, or 0 when `c` is not in
  // the table.
  int GradeOf(char32_t c) const;

  // Returns true when every kanji in `text` is taught by `max_grade`.
  bool AllKanjiAllowed(std::string_view text, int max_grade) const;

  // Exposed for tests: applies both features to one candidate.  Returns true
  // when the candidate was changed.
  bool RewriteCandidate(int max_grade, uint32_t furigana_style,
                        converter::Candidate* candidate) const;

 private:
  absl::flat_hash_map<char32_t, int> grade_of_;
};

}  // namespace mozc

#endif  // MOZC_REWRITER_ELMO_SCHOOL_REWRITER_H_
