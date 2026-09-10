// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "rewriter/english_word_dictionary_rewriter.h"

#include <cstddef>
#include <string>

#include "absl/strings/string_view.h"
#include "config/config_handler.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "testing/gunit.h"

namespace mozc {
namespace {

Segment* AddInputSegment(absl::string_view key, Segments* segments) {
  Segment* segment = segments->push_back_segment();
  segment->set_key(key);
  converter::Candidate* raw = segment->add_candidate();
  raw->key = std::string(key);
  raw->content_key = raw->key;
  raw->value = std::string(key);
  raw->content_value = raw->value;
  return segment;
}

const converter::Candidate* FindCandidate(const Segment& segment,
                                          absl::string_view value) {
  for (const converter::Candidate* candidate : segment.candidates()) {
    if (candidate != nullptr && candidate->value == value) {
      return candidate;
    }
  }
  return nullptr;
}

ConversionRequest BuildRequest(absl::string_view key, RequestType type,
                               bool dictionary_enabled = true,
                               bool spelling_enabled = true) {
  config::Config config;
  config::ConfigHandler::GetDefaultConfig(&config);
  config.set_use_english_word_dictionary(dictionary_enabled);
  config.set_use_english_spelling_correction(spelling_enabled);
  return ConversionRequestBuilder()
      .SetConfig(config)
      .SetRequestType(type)
      .SetKey(key)
      .Build();
}

}  // namespace

TEST(EnglishWordDictionaryRewriterTest, CompletesCanonicalTechWord) {
  EnglishWordDictionaryRewriter rewriter;
  Segments segments;
  Segment* segment = AddInputSegment("gith", &segments);

  const ConversionRequest request = BuildRequest("gith", SUGGESTION);
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  const converter::Candidate* candidate = FindCandidate(*segment, "GitHub");
  ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(candidate->description, "英単語補完");
  EXPECT_FALSE(candidate->attributes & converter::Attribute::SPELLING_CORRECTION);
}

TEST(EnglishWordDictionaryRewriterTest, CompletesGeneralEnglishWord) {
  EnglishWordDictionaryRewriter rewriter;
  Segments segments;
  Segment* segment = AddInputSegment("prope", &segments);

  const ConversionRequest request = BuildRequest("prope", SUGGESTION);
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  EXPECT_NE(FindCandidate(*segment, "property"), nullptr);
}

TEST(EnglishWordDictionaryRewriterTest, CorrectsTransposedSpelling) {
  EnglishWordDictionaryRewriter rewriter;
  Segments segments;
  Segment* segment = AddInputSegment("recieve", &segments);

  const ConversionRequest request = BuildRequest("recieve", CONVERSION);
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  const converter::Candidate* candidate = FindCandidate(*segment, "receive");
  ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(candidate->description, "英語スペル候補");
  EXPECT_TRUE(candidate->attributes & converter::Attribute::SPELLING_CORRECTION);
  EXPECT_EQ(candidate->prefix, "→ ");
}

TEST(EnglishWordDictionaryRewriterTest, CorrectsSubstitutionSpelling) {
  EnglishWordDictionaryRewriter rewriter;
  Segments segments;
  Segment* segment = AddInputSegment("proparty", &segments);

  const ConversionRequest request = BuildRequest("proparty", CONVERSION);
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  EXPECT_NE(FindCandidate(*segment, "property"), nullptr);
}

TEST(EnglishWordDictionaryRewriterTest, DoesNotRunSpellingScanForSuggestion) {
  EnglishWordDictionaryRewriter rewriter;
  Segments segments;
  Segment* segment = AddInputSegment("recieve", &segments);

  const ConversionRequest request = BuildRequest("recieve", SUGGESTION);
  rewriter.Rewrite(request, &segments);
  EXPECT_EQ(FindCandidate(*segment, "receive"), nullptr);
}

TEST(EnglishWordDictionaryRewriterTest, MasterSwitchDisablesFeature) {
  EnglishWordDictionaryRewriter rewriter;
  Segments segments;
  Segment* segment = AddInputSegment("gith", &segments);

  const ConversionRequest request =
      BuildRequest("gith", SUGGESTION, false, true);
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
  EXPECT_EQ(FindCandidate(*segment, "GitHub"), nullptr);
}

TEST(EnglishWordDictionaryRewriterTest, SpellingSwitchDoesNotDisableCompletion) {
  EnglishWordDictionaryRewriter rewriter;
  Segments segments;
  Segment* segment = AddInputSegment("gith", &segments);

  const ConversionRequest request =
      BuildRequest("gith", CONVERSION, true, false);
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  EXPECT_NE(FindCandidate(*segment, "GitHub"), nullptr);
}

TEST(EnglishWordDictionaryRewriterTest, PreservesUppercaseInputIntent) {
  EnglishWordDictionaryRewriter rewriter;
  Segments segments;
  Segment* segment = AddInputSegment("GITH", &segments);

  const ConversionRequest request = BuildRequest("GITH", SUGGESTION);
  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  EXPECT_NE(FindCandidate(*segment, "GITHUB"), nullptr);
}

}  // namespace mozc
