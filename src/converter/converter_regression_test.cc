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

#include <memory>
#include <string>

#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "base/clock_mock.h"
#include "composer/composer.h"
#include "config/config_handler.h"
#include "converter/converter_interface.h"
#include "converter/segments.h"
#include "engine/engine.h"
#include "engine/engine_factory.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc {
namespace {

ConversionRequest ConvReq(absl::string_view key) {
  composer::Composer composer;
  composer.SetPreeditTextForTestOnly(key);
  return ConversionRequestBuilder().SetComposer(composer).Build();
}

class ConverterRegressionTest : public testing::TestWithTempUserProfile {};

TEST_F(ConverterRegressionTest, QueryOfDeathTest) {
  std::unique_ptr<Engine> engine = EngineFactory::Create().value();
  std::shared_ptr<const ConverterInterface> converter = engine->GetConverter();

  CHECK(converter);
  {
    Segments segments;
    EXPECT_TRUE(
        converter->StartConversion(ConvReq("りゅきゅけmぽ"), &segments));
  }
  {
    Segments segments;
    EXPECT_TRUE(converter->StartConversion(ConvReq("5.1,||t:1"), &segments));
  }
  {
    Segments segments;
    // Converter returns false, but not crash.
    EXPECT_FALSE(converter->StartConversion(ConvReq(""), &segments));
  }
  {
    Segments segments;
    const ConversionRequest conv_request;
    // Converter returns false, but not crash.
    EXPECT_FALSE(converter->StartConversion(conv_request, &segments));
  }
}

TEST_F(ConverterRegressionTest, Regression3323108) {
  std::unique_ptr<Engine> engine = EngineFactory::Create().value();
  std::shared_ptr<const ConverterInterface> converter = engine->GetConverter();
  Segments segments;

  EXPECT_TRUE(
      converter->StartConversion(ConvReq("ここではきものをぬぐ"), &segments));
  EXPECT_EQ(segments.conversion_segments_size(), 3);
  const ConversionRequest default_request;
  EXPECT_TRUE(converter->ResizeSegment(&segments, default_request, 1, 2));
  EXPECT_EQ(segments.conversion_segments_size(), 2);
  EXPECT_EQ(segments.conversion_segment(1).key(), "きものをぬぐ");
}

TEST_F(ConverterRegressionTest, WeekdayDatesWithProductDefaultFormats) {
  const ScopedClockMock clock(ParseTimeOrDie("2026-09-27T12:00:00Z"));
  std::unique_ptr<Engine> engine = EngineFactory::Create().value();
  std::shared_ptr<const ConverterInterface> converter = engine->GetConverter();
  config::Config config;
  config::ConfigHandler::GetDefaultConfig(&config);
  ASSERT_TRUE(config.date_conversion_custom_formats_initialized());
  ASSERT_GT(config.date_conversion_custom_formats_size(), 0);

  struct TestCase {
    const char* key;
    const char* dates[3];
  };
  const TestCase cases[] = {
      {"げつよう", {"2026/09/21", "2026/09/28", "2026/09/14"}},
      {"げつようび", {"2026/09/21", "2026/09/28", "2026/09/14"}},
      {"かよう", {"2026/09/22", "2026/09/29", "2026/09/15"}},
      {"かようび", {"2026/09/22", "2026/09/29", "2026/09/15"}},
      {"すいよう", {"2026/09/23", "2026/09/30", "2026/09/16"}},
      {"すいようび", {"2026/09/23", "2026/09/30", "2026/09/16"}},
      {"もくよう", {"2026/09/24", "2026/10/01", "2026/09/17"}},
      {"もくようび", {"2026/09/24", "2026/10/01", "2026/09/17"}},
      {"きんよう", {"2026/09/25", "2026/10/02", "2026/09/18"}},
      {"きんようび", {"2026/09/25", "2026/10/02", "2026/09/18"}},
      {"どよう", {"2026/09/26", "2026/10/03", "2026/09/19"}},
      {"どようび", {"2026/09/26", "2026/10/03", "2026/09/19"}},
      {"にちよう", {"2026/09/27", "2026/10/04", "2026/09/20"}},
      {"にちようび", {"2026/09/27", "2026/10/04", "2026/09/20"}},
  };
  const char* descriptions[] = {"今週の日付", "来週の日付", "先週の日付"};
  for (const TestCase& test : cases) {
    SCOPED_TRACE(test.key);
    composer::Composer composer;
    composer.SetPreeditTextForTestOnly(test.key);
    const ConversionRequest request = ConversionRequestBuilder()
                                          .SetComposer(composer)
                                          .SetConfig(config)
                                          .Build();
    Segments segments;
    ASSERT_TRUE(converter->StartConversion(request, &segments));
    ASSERT_EQ(segments.conversion_segments_size(), 1);
    const Segment& segment = segments.conversion_segment(0);
    for (size_t week = 0; week < 3; ++week) {
      bool found = false;
      for (size_t i = 0; i < segment.candidates_size(); ++i) {
        const converter::Candidate& candidate = segment.candidate(i);
        if (candidate.value == test.dates[week] &&
            candidate.description == descriptions[week]) {
          found = true;
          break;
        }
      }
      EXPECT_TRUE(found) << test.dates[week] << " " << descriptions[week];
    }
  }
}

}  // namespace
}  // namespace mozc
