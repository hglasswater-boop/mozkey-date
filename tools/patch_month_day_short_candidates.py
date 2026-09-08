from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one marker, found {count}")
    return text.replace(old, new, 1)


source_path = Path("src/rewriter/date_rewriter.cc")
source = source_path.read_text(encoding="utf-8")

source = replace_once(
    source,
    """bool AppendFullDateCandidates(\n    uint32_t year, uint32_t month, uint32_t day,\n    absl::Span<const std::string> extra_date_formats,\n    std::vector<DateCandidate>* results) {\n""",
    """bool AppendFullDateCandidates(\n    uint32_t year, uint32_t month, uint32_t day,\n    absl::Span<const std::string> extra_date_formats,\n    bool include_month_day_candidates, std::vector<DateCandidate>* results) {\n""",
    "AppendFullDateCandidates signature",
)

source = replace_once(
    source,
    """  for (const absl::string_view date_format : extra_date_formats) {\n    AppendDateCandidateIfMissing(absl::FormatTime(date_format, at, tz),\n                                 results);\n  }\n  for (std::string candidate :\n       DateRewriter::ConvertDateWithYear(year, month, day)) {\n""",
    """  for (const absl::string_view date_format : extra_date_formats) {\n    AppendDateCandidateIfMissing(absl::FormatTime(date_format, at, tz),\n                                 results);\n  }\n  if (include_month_day_candidates) {\n    AppendDateCandidateIfMissing(absl::StrFormat(\"%d月%d日\", month, day),\n                                 results);\n    AppendDateCandidateIfMissing(absl::StrFormat(\"%02d/%02d\", month, day),\n                                 results);\n  }\n  for (std::string candidate :\n       DateRewriter::ConvertDateWithYear(year, month, day)) {\n""",
    "short month/day candidates",
)

source = replace_once(
    source,
    """  return AppendFullDateCandidates(year, month, day, extra_date_formats,\n                                  results);\n""",
    """  return AppendFullDateCandidates(year, month, day, extra_date_formats,\n                                  false, results);\n""",
    "eight-digit call",
)

source = replace_once(
    source,
    """    if (!AppendFullDateCandidates(parsed.year, parsed.month, parsed.day,\n                                  extra_date_formats, &results)) {\n""",
    """    if (!AppendFullDateCandidates(parsed.year, parsed.month, parsed.day,\n                                  extra_date_formats, !parsed.has_year,\n                                  &results)) {\n""",
    "separated-date call",
)

source = replace_once(
    source,
    """std::optional<ParsedDateExpression> GetSeparatedDateExpression(\n    const composer::ComposerData& composer, const Segments& segments) {\n""",
    """std::optional<RewriterInterface::ResizeSegmentsRequest>\nGetSeparatedDateResizeRequest(const ConversionRequest& request,\n                              const Segments& segments) {\n  if (segments.conversion_segments_size() <= 1) {\n    return std::nullopt;\n  }\n\n  std::string combined_key;\n  size_t raw_key_len = 0;\n  for (const Segment& segment : segments.conversion_segments()) {\n    combined_key.append(segment.key());\n    raw_key_len += segment.key_len();\n  }\n\n  const size_t key_len = Util::CharsLen(combined_key);\n  if (key_len == 0 || key_len > std::numeric_limits<uint8_t>::max()) {\n    return std::nullopt;\n  }\n\n  std::optional<ParsedDateExpression> parsed =\n      ParseSeparatedDateExpression(combined_key);\n  if (!parsed) {\n    parsed = ParseSeparatedDateExpression(\n        request.composer().GetRawSubString(0, raw_key_len));\n  }\n  if (!parsed) {\n    return std::nullopt;\n  }\n\n  if (!parsed->has_year) {\n    const absl::TimeZone tz = Clock::GetTimeZone();\n    const uint32_t current_year = static_cast<uint32_t>(\n        absl::ToCivilDay(Clock::GetAbslTime(), tz).year());\n    if (!IsValidDate(current_year, parsed->month, parsed->day)) {\n      return std::nullopt;\n    }\n  }\n\n  return RewriterInterface::ResizeSegmentsRequest{\n      .segment_index = 0,\n      .segment_sizes = {static_cast<uint8_t>(key_len), 0, 0, 0, 0, 0, 0, 0},\n  };\n}\n\nstd::optional<ParsedDateExpression> GetSeparatedDateExpression(\n    const composer::ComposerData& composer, const Segments& segments) {\n""",
    "separated-date resize helper",
)

source = replace_once(
    source,
    """  for (size_t segment_index = 0;\n       segment_index < segments.conversion_segments_size(); ++segment_index) {\n""",
    """  if (std::optional<RewriterInterface::ResizeSegmentsRequest>\n          resize_request = GetSeparatedDateResizeRequest(request, segments);\n      resize_request.has_value()) {\n    return resize_request;\n  }\n\n  for (size_t segment_index = 0;\n       segment_index < segments.conversion_segments_size(); ++segment_index) {\n""",
    "resize request integration",
)

source = replace_once(
    source,
    """  if (has_explicit_date_input) {\n    int raw_candidate_index = -1;\n""",
    """  if (has_explicit_date_input) {\n    results.erase(\n        std::remove_if(results.begin(), results.end(),\n                       [&](const DateCandidate& item) {\n                         return item.candidate == raw_input;\n                       }),\n        results.end());\n    int raw_candidate_index = -1;\n""",
    "raw candidate dedupe",
)

source_path.write_text(source, encoding="utf-8")


test_path = Path("src/rewriter/date_rewriter_test.cc")
test = test_path.read_text(encoding="utf-8")

test = replace_once(
    test,
    """TEST_F(DateRewriterTest, AtokStyleSeparatedDateAndCustomFormat) {\n""",
    """TEST_F(DateRewriterTest, AtokStyleSeparatedDateResize) {\n  ClockMock mock_clock(ParseTimeOrDie(\"2026-09-08T12:00:00Z\"));\n  Clock::SetClockForUnitTest(&mock_clock);\n\n  Segments segments;\n  AppendSegment(\"9\", \"9\", &segments);\n  AppendSegment(\"・\", \"・\", &segments);\n  AppendSegment(\"8\", \"8\", &segments);\n\n  auto table = std::make_shared<composer::Table>();\n  const commands::Request command_request;\n  const config::Config config;\n  composer::Composer composer(table, command_request, config);\n  composer.InsertCharacter(\"9/8\");\n  const ConversionRequest request =\n      ConversionRequestBuilder().SetComposer(composer).Build();\n\n  DateRewriter rewriter;\n  const auto resize_request =\n      rewriter.CheckResizeSegmentsRequest(request, segments);\n  ASSERT_TRUE(resize_request.has_value());\n  EXPECT_EQ(resize_request->segment_index, 0);\n  EXPECT_EQ(resize_request->segment_sizes[0], 3);\n\n  Clock::SetClockForUnitTest(nullptr);\n}\n\nTEST_F(DateRewriterTest, AtokStyleSeparatedDateAndCustomFormat) {\n""",
    "separated-date resize test",
)

test = replace_once(
    test,
    """                  ValueAndDescAre(\"9/8\", \"\"),\n                  ValueAndDescAre(\"2026.09.08\", \"日付\"),\n                  ValueAndDescAre(\"2026/09/08\", \"日付\"),\n""",
    """                  ValueAndDescAre(\"9/8\", \"\"),\n                  ValueAndDescAre(\"2026.09.08\", \"日付\"),\n                  ValueAndDescAre(\"9月8日\", \"日付\"),\n                  ValueAndDescAre(\"09/08\", \"日付\"),\n                  ValueAndDescAre(\"2026/09/08\", \"日付\"),\n""",
    "9/8 expected candidates",
)

test = replace_once(
    test,
    """  InitSegment(\"９／８\", \"９／８\", &segments);\n  EXPECT_TRUE(rewriter.Rewrite(request, &segments));\n  EXPECT_EQ(segments.segment(0).candidate(1).value, \"2026.09.08\");\n  EXPECT_EQ(segments.segment(0).candidate(2).value, \"2026/09/08\");\n""",
    """  for (const absl::string_view input :\n       {\"9-8\", \"9.8\", \"09/08\", \"９／８\"}) {\n    InitSegment(input, input, &segments);\n    EXPECT_TRUE(rewriter.Rewrite(request, &segments)) << input;\n    EXPECT_EQ(segments.segment(0).candidate(1).value, \"2026.09.08\")\n        << input;\n    EXPECT_EQ(segments.segment(0).candidate(2).value, \"9月8日\") << input;\n    EXPECT_EQ(segments.segment(0).candidate(3).value, \"09/08\") << input;\n  }\n""",
    "separator variants",
)

test_path.write_text(test, encoding="utf-8")
