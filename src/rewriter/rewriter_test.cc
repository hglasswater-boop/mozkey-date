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

#include "rewriter/rewriter.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "absl/log/check.h"
#include "base/clock_mock.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "data_manager/testing/mock_data_manager.h"
#include "engine/modules.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc {
namespace {

size_t CommandCandidatesSize(const Segment& segment) {
  size_t result = 0;
  for (int i = 0; i < segment.candidates_size(); ++i) {
    if (segment.candidate(i).attributes &
        converter::Attribute::COMMAND_CANDIDATE) {
      result++;
    }
  }
  return result;
}

bool HasCandidateValue(const Segment& segment, const std::string& value) {
  for (size_t i = 0; i < segment.candidates_size(); ++i) {
    if (segment.candidate(i).value == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

class RewriterTest : public testing::TestWithTempUserProfile {
 protected:
  void SetUp() override {
    modules_ =
        engine::Modules::Create(std::make_unique<testing::MockDataManager>())
            .value();
    rewriter_ = std::make_unique<Rewriter>(*modules_);
  }

  const RewriterInterface* GetRewriter() const { return rewriter_.get(); }

  std::unique_ptr<const engine::Modules> modules_;
  std::unique_ptr<Rewriter> rewriter_;
};

// Command rewriter should be disabled on Android build. b/5851240
TEST_F(RewriterTest, CommandRewriterAvailability) {
  Segments segments;
  const ConversionRequest request;
  Segment* seg = segments.push_back_segment();

  {
    converter::Candidate* candidate = seg->add_candidate();
    seg->set_key("こまんど");
    candidate->value = "コマンド";
    EXPECT_TRUE(GetRewriter()->Rewrite(request, &segments));
#ifdef __ANDROID__
    EXPECT_EQ(CommandCandidatesSize(*seg), 0);
#else  // __ANDROID__
    EXPECT_EQ(CommandCandidatesSize(*seg), 2);
#endif  // __ANDROID__
    seg->clear_candidates();
  }

  {
    converter::Candidate* candidate = seg->add_candidate();
    seg->set_key("さじぇすと");
    candidate->value = "サジェスト";
    EXPECT_TRUE(GetRewriter()->Rewrite(request, &segments));
#ifdef __ANDROID__
    EXPECT_EQ(CommandCandidatesSize(*seg), 0);
#else  // __ANDROID__
    EXPECT_EQ(CommandCandidatesSize(*seg), 1);
#endif  // __ANDROID__
    seg->clear_candidates();
  }
}

TEST_F(RewriterTest, EmoticonsAboveSymbols) {
  constexpr char kKey[] = "かおもじ";
  constexpr char kEmoticon[] = "^^;";
  constexpr char kSymbol[] = "☹";  // A platform-dependent symbol

  const ConversionRequest request;
  Segments segments;
  Segment* seg = segments.push_back_segment();
  converter::Candidate* candidate = seg->add_candidate();
  seg->set_key(kKey);
  candidate->value = kKey;
  EXPECT_EQ(seg->candidates_size(), 1);
  EXPECT_TRUE(GetRewriter()->Rewrite(request, &segments));
  EXPECT_LT(1, seg->candidates_size());

  int emoticon_index = -1;
  int symbol_index = -1;
  for (size_t i = 0; i < seg->candidates_size(); ++i) {
    if (seg->candidate(i).value == kEmoticon) {
      emoticon_index = i;
    } else if (seg->candidate(i).value == kSymbol) {
      symbol_index = i;
    }
  }
  EXPECT_NE(emoticon_index, -1);
  EXPECT_NE(symbol_index, -1);
  EXPECT_LT(emoticon_index, symbol_index);
}

TEST_F(RewriterTest, DateFormatWeekdayAndZeroSuppressTokens) {
  const ConversionRequest request;
  Segments segments;
  Segment* seg = segments.push_back_segment();
  seg->set_key("dummy-date-format-token-test");

  // DateRewriter normally supplies this canonical candidate. The custom
  // token post-processor derives the target date from it.
  seg->add_candidate()->value = "2026/09/08";
  seg->add_candidate()->value =
      "{YEAR_NOZERO}/{MONTH_NOZERO}/{DATE_NOZERO}({WEEKDAY})";
  seg->add_candidate()->value =
      "{YEAR_NOZERO}年{MONTH_NOZERO}月{DATE_NOZERO}日({WEEKDAY_LONG})";

  EXPECT_TRUE(GetRewriter()->Rewrite(request, &segments));
  EXPECT_TRUE(HasCandidateValue(*seg, "2026/9/8(火)"));
  EXPECT_TRUE(HasCandidateValue(*seg, "2026年9月8日(火曜日)"));
}

TEST_F(RewriterTest, DateFormatListFiltersUnconfiguredDateCandidates) {
  config::Config config;
  config.set_use_date_conversion(true);
  config.set_date_conversion_custom_formats_initialized(true);
  config.add_date_conversion_custom_formats(
      "{YEAR}/{MONTH_NOZERO}/{DATE_NOZERO}({WEEKDAY})");
  const ConversionRequest request =
      ConversionRequestBuilder().SetConfig(config).Build();

  Segments segments;
  Segment* seg = segments.push_back_segment();
  seg->set_key("dummy-date-format-filter-test");

  converter::Candidate* canonical = seg->add_candidate();
  canonical->value = "2026/09/08";
  canonical->description = "今日の日付";

  converter::Candidate* standard = seg->add_candidate();
  standard->value = "2026-09-08";
  standard->description = "今日の日付";

  converter::Candidate* configured = seg->add_candidate();
  configured->value =
      "{YEAR}/{MONTH_NOZERO}/{DATE_NOZERO}({WEEKDAY})";
  configured->description = "今日の日付";

  converter::Candidate* ordinary = seg->add_candidate();
  ordinary->value = "keep-me";

  EXPECT_TRUE(GetRewriter()->Rewrite(request, &segments));
  EXPECT_TRUE(HasCandidateValue(*seg, "2026/9/8(火)"));
  EXPECT_FALSE(HasCandidateValue(*seg, "2026/09/08"));
  EXPECT_FALSE(HasCandidateValue(*seg, "2026-09-08"));
  EXPECT_TRUE(HasCandidateValue(*seg, "keep-me"));
}

TEST_F(RewriterTest, EmptyInitializedDateFormatListRemovesDateCandidates) {
  config::Config config;
  config.set_use_date_conversion(true);
  config.set_date_conversion_custom_formats_initialized(true);
  const ConversionRequest request =
      ConversionRequestBuilder().SetConfig(config).Build();

  Segments segments;
  Segment* seg = segments.push_back_segment();
  seg->set_key("dummy-empty-date-format-filter-test");

  converter::Candidate* canonical = seg->add_candidate();
  canonical->value = "2026/09/08";
  canonical->description = "今日の日付";

  converter::Candidate* standard = seg->add_candidate();
  standard->value = "2026年9月8日";
  standard->description = "今日の日付";

  converter::Candidate* ordinary = seg->add_candidate();
  ordinary->value = "keep-me";

  EXPECT_TRUE(GetRewriter()->Rewrite(request, &segments));
  EXPECT_FALSE(HasCandidateValue(*seg, "2026/09/08"));
  EXPECT_FALSE(HasCandidateValue(*seg, "2026年9月8日"));
  EXPECT_TRUE(HasCandidateValue(*seg, "keep-me"));
}

TEST_F(RewriterTest, WeekdayDatesSurviveConfiguredFormatFiltering) {
  struct TestCase {
    const char* now;
    const char* key;
    const char* value;
    std::vector<std::string> expected;
  };
  const TestCase cases[] = {
      {"2026-09-25T12:00:00Z", "きんよう", "金曜",
       {"2026/9/25(金)", "2026年9月25日(金曜日)",
        "2026/10/2(金)", "2026年10月2日(金曜日)",
        "2026/9/18(金)", "2026年9月18日(金曜日)"}},
      {"2026-09-25T12:00:00Z", "きんようび", "金曜日",
       {"2026/9/25(金)", "2026年9月25日(金曜日)",
        "2026/10/2(金)", "2026年10月2日(金曜日)",
        "2026/9/18(金)", "2026年9月18日(金曜日)"}},
      {"2026-12-31T12:00:00Z", "げつよう", "月曜",
       {"2026/12/28(月)", "2026年12月28日(月曜日)",
        "2027/1/4(月)", "2027年1月4日(月曜日)",
        "2026/12/21(月)", "2026年12月21日(月曜日)"}},
      {"2026-12-31T12:00:00Z", "げつようび", "月曜日",
       {"2026/12/28(月)", "2026年12月28日(月曜日)",
        "2027/1/4(月)", "2027年1月4日(月曜日)",
        "2026/12/21(月)", "2026年12月21日(月曜日)"}},
      {"2026-09-27T12:00:00Z", "にちよう", "日曜",
       {"2026/9/27(日)", "2026年9月27日(日曜日)",
        "2026/10/4(日)", "2026年10月4日(日曜日)",
        "2026/9/20(日)", "2026年9月20日(日曜日)"}},
      {"2026-09-27T12:00:00Z", "にちようび", "日曜日",
       {"2026/9/27(日)", "2026年9月27日(日曜日)",
        "2026/10/4(日)", "2026年10月4日(日曜日)",
        "2026/9/20(日)", "2026年9月20日(日曜日)"}},
  };
  config::Config config;
  config.set_date_conversion_custom_formats_initialized(true);
  config.add_date_conversion_custom_formats(
      "{YEAR}/{MONTH_NOZERO}/{DATE_NOZERO}({WEEKDAY})");
  config.add_date_conversion_custom_formats(
      "{YEAR}年{MONTH_NOZERO}月{DATE_NOZERO}日({WEEKDAY_LONG})");
  const ConversionRequest request =
      ConversionRequestBuilder().SetConfig(config).Build();

  for (const TestCase& test : cases) {
    SCOPED_TRACE(test.key);
    const ScopedClockMock clock(ParseTimeOrDie(test.now));
    Segments segments;
    Segment* segment = segments.add_segment();
    segment->set_key(test.key);
    converter::Candidate* ordinary = segment->add_candidate();
    ordinary->key = ordinary->content_key = test.key;
    ordinary->value = ordinary->content_value = test.value;

    ASSERT_TRUE(GetRewriter()->Rewrite(request, &segments));
    std::vector<std::string> values;
    std::vector<std::string> descriptions;
    for (size_t i = 0; i < segment->candidates_size(); ++i) {
      const converter::Candidate& candidate = segment->candidate(i);
      if (candidate.description.find("日付") == std::string::npos) {
        continue;
      }
      values.push_back(candidate.value);
      descriptions.push_back(candidate.description);
      EXPECT_EQ(candidate.content_value, candidate.value);
    }
    EXPECT_EQ(values, test.expected);
    EXPECT_EQ(descriptions, (std::vector<std::string>{
        "今週の日付", "今週の日付", "来週の日付", "来週の日付",
        "先週の日付", "先週の日付"}));
    EXPECT_TRUE(HasCandidateValue(*segment, test.value));
  }
}

}  // namespace mozc
