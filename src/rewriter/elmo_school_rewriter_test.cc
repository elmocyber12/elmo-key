// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rewriter/elmo_school_rewriter.h"

#include "converter/attribute.h"
#include "converter/candidate.h"
#include "testing/gunit.h"

namespace mozc {
namespace {

converter::Candidate MakeCandidate(const char* key, const char* value,
                                   const char* content_key,
                                   const char* content_value) {
  converter::Candidate c;
  c.key = key;
  c.value = value;
  c.content_key = content_key;
  c.content_value = content_value;
  return c;
}

TEST(ElmoSchoolRewriterTest, GradeTable) {
  ElmoSchoolRewriter rewriter;
  EXPECT_EQ(rewriter.GradeOf(U'一'), 1);
  EXPECT_EQ(rewriter.GradeOf(U'強'), 2);
  EXPECT_EQ(rewriter.GradeOf(U'勉'), 3);
  EXPECT_EQ(rewriter.GradeOf(U'械'), 4);
  EXPECT_EQ(rewriter.GradeOf(U'資'), 5);
  EXPECT_EQ(rewriter.GradeOf(U'議'), 4);
  EXPECT_EQ(rewriter.GradeOf(U'憲'), 6);
  EXPECT_EQ(rewriter.GradeOf(U'鬱'), 0);
}

TEST(ElmoSchoolRewriterTest, GradeModeTurnsUntaughtWordsIntoHiragana) {
  ElmoSchoolRewriter rewriter;
  // 勉 is grade 3: kept in grade 3, hiragana in grade 2.
  converter::Candidate c = MakeCandidate("べんきょう", "勉強", "べんきょう", "勉強");
  EXPECT_FALSE(rewriter.RewriteCandidate(3, 0, &c));
  EXPECT_EQ(c.value, "勉強");
  EXPECT_TRUE(rewriter.RewriteCandidate(2, 0, &c));
  EXPECT_EQ(c.value, "べんきょう");
  EXPECT_EQ(c.content_value, "べんきょう");
  EXPECT_EQ(c.attributes & converter::Attribute::NO_LEARNING,
            converter::Attribute::NO_LEARNING);

  // Okurigana and non-kanji characters are kept as they are.
  converter::Candidate verb = MakeCandidate("かきます", "書きます", "かき", "書き");
  EXPECT_FALSE(rewriter.RewriteCandidate(2, 0, &verb));  // 書 is grade 2.
  EXPECT_TRUE(rewriter.RewriteCandidate(1, 0, &verb));
  EXPECT_EQ(verb.value, "かきます");

  converter::Candidate kata = MakeCandidate("てすと", "テスト", "てすと", "テスト");
  EXPECT_FALSE(rewriter.RewriteCandidate(1, 0, &kata));
}

TEST(ElmoSchoolRewriterTest, Furigana) {
  ElmoSchoolRewriter rewriter;
  converter::Candidate c = MakeCandidate("かんじ", "漢字", "かんじ", "漢字");
  EXPECT_TRUE(rewriter.RewriteCandidate(0, 1, &c));
  EXPECT_EQ(c.value, "漢字（かんじ）");
  EXPECT_EQ(c.content_value, "漢字（かんじ）");

  converter::Candidate verb = MakeCandidate("かきます", "書きます", "かき", "書き");
  EXPECT_TRUE(rewriter.RewriteCandidate(0, 2, &verb));
  EXPECT_EQ(verb.value, "書き(かき)ます");

  // No kanji, no furigana.
  converter::Candidate hira = MakeCandidate("ひらがな", "ひらがな", "ひらがな", "ひらがな");
  EXPECT_FALSE(rewriter.RewriteCandidate(0, 1, &hira));
}

TEST(ElmoSchoolRewriterTest, GradeModeThenFurigana) {
  ElmoSchoolRewriter rewriter;
  // Untaught word becomes hiragana, so no furigana is added.
  converter::Candidate c = MakeCandidate("ゆううつ", "憂鬱", "ゆううつ", "憂鬱");
  EXPECT_TRUE(rewriter.RewriteCandidate(6, 1, &c));
  EXPECT_EQ(c.value, "ゆううつ");
  // Taught word keeps kanji and gets furigana.
  converter::Candidate d = MakeCandidate("がっこう", "学校", "がっこう", "学校");
  EXPECT_TRUE(rewriter.RewriteCandidate(1, 1, &d));
  EXPECT_EQ(d.value, "学校（がっこう）");
}

}  // namespace
}  // namespace mozc
