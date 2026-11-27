// Copyright 2026 ELMO_key contributors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "session/elmo_ai_rewriter.h"

#include <string>

#include "testing/gunit.h"

namespace mozc {
namespace session {
namespace {

TEST(ElmoAiRewriterTest, BuildChatRequestJsonEscapesText) {
  const std::string json = ElmoAiRewriter::BuildChatRequestJson(
      ElmoRewriteMode::kPolite, "資料\"直して\"\n");
  EXPECT_NE(json.find("\"messages\":["), std::string::npos);
  EXPECT_NE(json.find("資料\\\"直して\\\"\\n"), std::string::npos);
  EXPECT_NE(json.find("\"temperature\":0.2"), std::string::npos);
  // Few-shot examples are included.
  EXPECT_NE(json.find("\"role\":\"assistant\""), std::string::npos);
}

TEST(ElmoAiRewriterTest, ExtractMessageContent) {
  std::string out;
  EXPECT_TRUE(ElmoAiRewriter::ExtractMessageContent(
      R"({"choices":[{"index":0,"message":{"role":"assistant","content":"承知いたしました。\n\"OK\""}}]})",
      &out));
  EXPECT_EQ(out, "承知いたしました。\n\"OK\"");
  EXPECT_FALSE(ElmoAiRewriter::ExtractMessageContent("{\"error\":1}", &out));
}

TEST(ElmoAiRewriterTest, CleanAnswer) {
  std::string a = "  「その件につきましては、部長にご確認ください。」</s> ";
  EXPECT_TRUE(ElmoAiRewriter::CleanAnswer(ElmoRewriteMode::kPolite,
                                          "その件は部長に聞いといて", &a));
  EXPECT_EQ(a, "その件につきましては、部長にご確認ください。");

  // A reply instead of a rewrite is rejected...
  std::string reply = "かしこまりました。修正いたします。";
  EXPECT_FALSE(ElmoAiRewriter::CleanAnswer(
      ElmoRewriteMode::kPolite, "この資料、間違ってるから直して", &reply));
  // ...unless the original itself is an acknowledgement.
  std::string ack = "承知いたしました。そのように進めます。";
  EXPECT_TRUE(ElmoAiRewriter::CleanAnswer(ElmoRewriteMode::kPolite,
                                          "了解、じゃあそれで進めとく", &ack));

  std::string english = "会議の時間が変わりました。";
  EXPECT_FALSE(ElmoAiRewriter::CleanAnswer(ElmoRewriteMode::kEnglish,
                                           "会議の時間が変わった", &english));
  std::string ok_english = "The meeting time has changed.";
  EXPECT_TRUE(ElmoAiRewriter::CleanAnswer(ElmoRewriteMode::kEnglish,
                                          "会議の時間が変わった", &ok_english));

  // An unmatched closing bracket echoed from the prompt is removed.
  std::string echoed = "The meeting time has been changed to 3:00 PM.」";
  EXPECT_TRUE(ElmoAiRewriter::CleanAnswer(
      ElmoRewriteMode::kEnglish, "会議の時間が3時に変わりました", &echoed));
  EXPECT_EQ(echoed, "The meeting time has been changed to 3:00 PM.");

  std::string empty = "  </s>";
  EXPECT_FALSE(
      ElmoAiRewriter::CleanAnswer(ElmoRewriteMode::kPlain, "テスト", &empty));
}

}  // namespace
}  // namespace session
}  // namespace mozc
