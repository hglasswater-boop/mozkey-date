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
    """  if (has_explicit_date_input) {\n    int raw_candidate_index = -1;\n""",
    """  if (has_explicit_date_input) {\n    results.erase(\n        std::remove_if(results.begin(), results.end(),\n                       [&](const DateCandidate& item) {\n                         return item.candidate == raw_input;\n                       }),\n        results.end());\n    int raw_candidate_index = -1;\n""",
    "raw candidate dedupe",
)

source_path.write_text(source, encoding="utf-8")


test_path = Path("src/rewriter/date_rewriter_test.cc")
test = test_path.read_text(encoding="utf-8")

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
