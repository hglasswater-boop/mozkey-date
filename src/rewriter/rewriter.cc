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

#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "base/container/tuple.h"
#include "data_manager/data_manager.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/pos_group.h"
#include "dictionary/pos_matcher.h"
#include "dictionary/single_kanji_dictionary.h"
#include "engine/modules.h"
#include "rewriter/a11y_description_rewriter.h"
#include "rewriter/calculator_rewriter.h"
#include "rewriter/collocation_rewriter.h"
#include "rewriter/correction_rewriter.h"
#include "rewriter/dice_rewriter.h"
#include "rewriter/emoji_rewriter.h"
#include "rewriter/emoticon_rewriter.h"
#include "rewriter/english_variants_rewriter.h"
#include "rewriter/environmental_filter_rewriter.h"
#include "rewriter/focus_candidate_rewriter.h"
#include "rewriter/ivs_variants_rewriter.h"
#include "rewriter/language_aware_rewriter.h"
#include "rewriter/number_rewriter.h"
#include "rewriter/remove_redundant_candidate_rewriter.h"
#include "rewriter/single_kanji_rewriter.h"
#include "rewriter/small_letter_rewriter.h"
#include "rewriter/symbol_rewriter.h"
#include "rewriter/t13n_promotion_rewriter.h"
#include "rewriter/transliteration_rewriter.h"
#include "rewriter/unicode_rewriter.h"
#include "rewriter/user_boundary_history_rewriter.h"
#include "rewriter/user_segment_history_rewriter.h"
#include "rewriter/variants_rewriter.h"
#include "rewriter/version_rewriter.h"
#include "rewriter/zenz_feedback_candidate_rewriter.h"
#include "rewriter/zipcode_rewriter.h"

#ifdef __APPLE__
#include <TargetConditionals.h>  // for TARGET_OS_IPHONE
#endif                           // __APPLE__

// CommandRewriter is not tested well on Android or iOS.
// So we temporarily disable it.
// TODO(yukawa, team): Enable CommandRewriter on Android if necessary.
#if !(defined(__ANDROID__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE))
#define MOZC_COMMAND_REWRITER
#endif  // !(__ANDROID__ || TARGET_OS_IPHONE)

// DateRewriter may return the date information that is possibly different from
// the user's environment.
#define MOZC_DATE_REWRITER

// FortuneRewriter changes the result when invoked in another day but it also
// suffers from the inconsistency of locale between server and user's
// environment.
#define MOZC_FORTUNE_REWRITER

// UsageRewriter is not used by non application build.
#ifndef NO_USAGE_REWRITER
#define MOZC_USAGE_REWRITER
#endif  // NO_USAGE_REWRITER

// UserDictionaryRewriter is only for application build because it will
// access to the local files per user.
#define MOZC_USER_DICTIONARY_REWRITER

// HistoryRewriter is used only for application build because it will
// access to the local files at the initialization timing.
#define MOZC_USER_HISTORY_REWRITER

#ifdef MOZC_COMMAND_REWRITER
#include "rewriter/command_rewriter.h"
#endif  // MOZC_COMMAND_REWRITER

#ifdef MOZC_DATE_REWRITER
#include "rewriter/date_rewriter.h"
#endif  // MOZC_DATE_REWRITER

#ifdef MOZC_FORTUNE_REWRITER
#include "rewriter/fortune_rewriter.h"
#endif  // MOZC_FORTUNE_REWRITER

#ifdef MOZC_USAGE_REWRITER
#include "rewriter/usage_rewriter.h"
#endif  // MOZC_USAGE_REWRITER

#ifdef MOZC_USER_DICTIONARY_REWRITER
#include "rewriter/user_dictionary_rewriter.h"
#endif  // MOZC_USER_DICTIONARY_REWRITER

#ifdef MOZC_USER_HISTORY_REWRITER
ABSL_FLAG(bool, use_history_rewriter, true, "Use history rewriter or not.");
#else   // MOZC_USER_HISTORY_REWRITER
ABSL_FLAG(bool, use_history_rewriter, false, "Use history rewriter or not.");
#endif  // MOZC_USER_HISTORY_REWRITER

namespace mozc {
namespace {

bool ParseUnsignedDecimal(const std::string& text, size_t begin, size_t end,
                          int* value) {
  if (begin >= end || end > text.size() || value == nullptr) {
    return false;
  }
  int result = 0;
  for (size_t i = begin; i < end; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return false;
    }
    result = result * 10 + static_cast<int>(c - '0');
  }
  *value = result;
  return true;
}

bool IsLeapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool IsValidDate(int year, int month, int day) {
  if (year <= 0 || month < 1 || month > 12 || day < 1) {
    return false;
  }
  static constexpr int kDaysPerMonth[] = {31, 28, 31, 30, 31, 30,
                                           31, 31, 30, 31, 30, 31};
  int max_day = kDaysPerMonth[month - 1];
  if (month == 2 && IsLeapYear(year)) {
    max_day = 29;
  }
  return day <= max_day;
}

bool ParseCanonicalDate(const std::string& value, int* year, int* month,
                        int* day) {
  if (value.size() < 8 || value.size() > 10) {
    return false;
  }
  const size_t first_separator = value.find_first_of("/-");
  if (first_separator == std::string::npos || first_separator < 1 ||
      first_separator > 4) {
    return false;
  }
  const char separator = value[first_separator];
  const size_t second_separator = value.find(separator, first_separator + 1);
  if (second_separator == std::string::npos ||
      second_separator != first_separator + 3 ||
      value.find(separator, second_separator + 1) != std::string::npos ||
      second_separator + 3 != value.size()) {
    return false;
  }

  int parsed_year = 0;
  int parsed_month = 0;
  int parsed_day = 0;
  if (!ParseUnsignedDecimal(value, 0, first_separator, &parsed_year) ||
      !ParseUnsignedDecimal(value, first_separator + 1, second_separator,
                            &parsed_month) ||
      !ParseUnsignedDecimal(value, second_separator + 1, value.size(),
                            &parsed_day) ||
      !IsValidDate(parsed_year, parsed_month, parsed_day)) {
    return false;
  }
  *year = parsed_year;
  *month = parsed_month;
  *day = parsed_day;
  return true;
}

bool FindCanonicalDate(const Segment& segment, int* year, int* month,
                       int* day) {
  for (size_t i = 0; i < segment.candidates_size(); ++i) {
    if (ParseCanonicalDate(segment.candidate(i).value, year, month, day)) {
      return true;
    }
  }
  return false;
}

int WeekdaySundayFirst(int year, int month, int day) {
  // Tomohiko Sakamoto's Gregorian-calendar algorithm.  0 is Sunday.
  static constexpr int kMonthOffsets[] = {0, 3, 2, 5, 0, 3,
                                           5, 1, 4, 6, 2, 4};
  if (month < 3) {
    --year;
  }
  return (year + year / 4 - year / 100 + year / 400 +
          kMonthOffsets[month - 1] + day) % 7;
}

bool ReplaceAll(std::string* text, const std::string& from,
                const std::string& to) {
  if (text == nullptr || from.empty()) {
    return false;
  }
  bool replaced = false;
  size_t position = 0;
  while ((position = text->find(from, position)) != std::string::npos) {
    text->replace(position, from.size(), to);
    position += to.size();
    replaced = true;
  }
  return replaced;
}

std::string PadDecimal(int value, size_t width) {
  std::string result = std::to_string(value);
  if (result.size() < width) {
    result.insert(0, width - result.size(), '0');
  }
  return result;
}

bool ExpandDateFormatTokens(int year, int month, int day, std::string* value) {
  static constexpr const char* kWeekdays[] = {"日", "月", "火", "水",
                                               "木", "金", "土"};
  const std::string weekday =
      kWeekdays[WeekdaySundayFirst(year, month, day)];

  bool modified = false;
  modified |= ReplaceAll(value, "{YEAR_NOZERO}", std::to_string(year));
  modified |= ReplaceAll(value, "{MONTH_NOZERO}", std::to_string(month));
  modified |= ReplaceAll(value, "{DATE_NOZERO}", std::to_string(day));
  modified |= ReplaceAll(value, "{WEEKDAY_LONG}", weekday + "曜日");
  modified |= ReplaceAll(value, "{WEEKDAY}", weekday);
  modified |= ReplaceAll(value, "{YEAR}", PadDecimal(year, 4));
  modified |= ReplaceAll(value, "{MONTH}", PadDecimal(month, 2));
  modified |= ReplaceAll(value, "{DATE}", PadDecimal(day, 2));
  modified |= ReplaceAll(value, "{{}", "{");
  return modified;
}

bool HasDynamicTimeToken(const std::string& format) {
  return format.find("{HOUR}") != std::string::npos ||
         format.find("{MINUTE}") != std::string::npos;
}

bool CanFilterToConfiguredDateFormats(const config::Config& config) {
  if (config.date_conversion_custom_formats_size() > 0) {
    for (const std::string& format : config.date_conversion_custom_formats()) {
      if (!format.empty() && HasDynamicTimeToken(format)) {
        return false;
      }
    }
    return true;
  }

  if (config.date_conversion_custom_formats_initialized()) {
    // An initialized empty list is intentional. The settings list is the source
    // of truth, so all DateRewriter-generated date-format candidates are
    // removed in this state.
    return true;
  }

  // Keep direct callers that still provide only the v0.1 compatibility field
  // working until their profile is migrated on the next ConfigHandler reload.
  const std::string& legacy_format = config.date_conversion_custom_format();
  return !legacy_format.empty() && !HasDynamicTimeToken(legacy_format);
}

bool MatchesConfiguredDateFormat(const std::string& format, int year, int month,
                                 int day, const std::string& value) {
  if (format.empty()) {
    return false;
  }
  std::string expanded = format;
  ExpandDateFormatTokens(year, month, day, &expanded);
  return expanded == value;
}

bool IsConfiguredDateValue(const config::Config& config, int year, int month,
                           int day, const std::string& value) {
  if (config.date_conversion_custom_formats_size() > 0) {
    for (const std::string& format : config.date_conversion_custom_formats()) {
      if (MatchesConfiguredDateFormat(format, year, month, day, value)) {
        return true;
      }
    }
    return false;
  }

  return MatchesConfiguredDateFormat(config.date_conversion_custom_format(),
                                     year, month, day, value);
}

bool IsDateCandidateDescription(const std::string& description) {
  if (description.find("日付") != std::string::npos) {
    return true;
  }
  return description.rfind("次の", 0) == 0 &&
         description.find("曜日") != std::string::npos;
}

// DateRewriter intentionally keeps the legacy format parser small.  This
// post-processor expands mozkey-date's additional date-format tokens after
// DateRewriter has generated both custom and canonical date candidates.  The
// canonical YYYY/MM/DD candidate supplies the actual target date, so the same
// logic works for today/tomorrow as well as explicit inputs such as 9/8.
//
// Once the date-format settings are initialized, this rewriter also removes
// date candidates that are not represented by the ordered list. This makes the
// settings list authoritative instead of silently appending DateRewriter's
// fixed standard formats behind the user's choices.
class CustomDateFormatTokenRewriter final : public RewriterInterface {
 public:
  int capability(const ConversionRequest& request) const override {
    if (request.request().mixed_conversion()) {
      return RewriterInterface::ALL;
    }
    return RewriterInterface::CONVERSION;
  }

  bool Rewrite(const ConversionRequest& request,
               Segments* segments) const override {
    if (segments == nullptr || !request.config().use_date_conversion()) {
      return false;
    }

    bool modified = false;
    for (size_t segment_index = 0;
         segment_index < segments->conversion_segments_size();
         ++segment_index) {
      Segment* segment = segments->mutable_conversion_segment(segment_index);
      int year = 0;
      int month = 0;
      int day = 0;
      if (!FindCanonicalDate(*segment, &year, &month, &day)) {
        continue;
      }

      for (size_t candidate_index = 0;
           candidate_index < segment->candidates_size(); ++candidate_index) {
        converter::Candidate* candidate =
            segment->mutable_candidate(candidate_index);
        const std::string original_value = candidate->value;
        if (!ExpandDateFormatTokens(year, month, day, &candidate->value)) {
          continue;
        }
        if (candidate->content_value == original_value) {
          candidate->content_value = candidate->value;
        }
        modified = true;
      }

      if (!CanFilterToConfiguredDateFormats(request.config())) {
        continue;
      }

      for (size_t candidate_index = segment->candidates_size();
           candidate_index > 0; --candidate_index) {
        const size_t index = candidate_index - 1;
        const converter::Candidate& candidate = segment->candidate(index);
        if (!IsDateCandidateDescription(candidate.description)) {
          continue;
        }
        if (IsConfiguredDateValue(request.config(), year, month, day,
                                  candidate.value)) {
          continue;
        }
        segment->erase_candidate(static_cast<int>(index));
        modified = true;
      }
    }
    return modified;
  }
};

}  // namespace

Rewriter::Rewriter(const engine::Modules& modules) {
  const DataManager& data_manager = modules.GetDataManager();
  const dictionary::DictionaryInterface& dictionary = modules.GetDictionary();
  const dictionary::PosMatcher& pos_matcher = modules.GetPosMatcher();
  const dictionary::PosGroup& pos_group = modules.GetPosGroup();
  const dictionary::SingleKanjiDictionary& single_kanji_dictionary =
      modules.GetSingleKanjiDictionary();

#ifdef MOZC_USER_DICTIONARY_REWRITER
  AddRewriter(std::make_unique<UserDictionaryRewriter>());
#endif  // MOZC_USER_DICTIONARY_REWRITER

  AddRewriter(make_unique_from_tuples<FocusCandidateRewriter>(
      data_manager.GetCounterSuffixSortedArray(), pos_matcher));
  AddRewriter(std::make_unique<LanguageAwareRewriter>(pos_matcher, dictionary));
  AddRewriter(std::make_unique<TransliterationRewriter>(pos_matcher));
  AddRewriter(std::make_unique<EnglishVariantsRewriter>(pos_matcher));
  AddRewriter(make_unique_from_tuples<NumberRewriter>(
      data_manager.GetCounterSuffixSortedArray(), pos_matcher));
  AddRewriter(apply_from_tuples(CollocationRewriter::Create, pos_matcher,
                                data_manager.GetCollocationData()));
  AddRewriter(std::make_unique<SingleKanjiRewriter>(pos_matcher,
                                                    single_kanji_dictionary));
  AddRewriter(std::make_unique<IvsVariantsRewriter>());
  AddRewriter(make_unique_from_tuples<EmoticonRewriter>(
      data_manager.GetEmoticonRewriterData()));
  AddRewriter(make_unique_from_tuples<EmojiRewriter>(
      data_manager.GetEmojiRewriterData()));
  AddRewriter(std::make_unique<CalculatorRewriter>());
  AddRewriter(make_unique_from_tuples<SymbolRewriter>(
      data_manager.GetSymbolRewriterData()));
  AddRewriter(std::make_unique<UnicodeRewriter>());
  AddRewriter(std::make_unique<VariantsRewriter>(pos_matcher));
  AddRewriter(std::make_unique<ZipcodeRewriter>(pos_matcher));
  AddRewriter(std::make_unique<DiceRewriter>());
  AddRewriter(std::make_unique<SmallLetterRewriter>());

  if (absl::GetFlag(FLAGS_use_history_rewriter)) {
    AddRewriter(std::make_unique<UserBoundaryHistoryRewriter>());
    AddRewriter(std::make_unique<ZenzFeedbackCandidateRewriter>());
    AddRewriter(
        std::make_unique<UserSegmentHistoryRewriter>(pos_matcher, pos_group));
  }

#ifdef MOZC_DATE_REWRITER
  AddRewriter(std::make_unique<DateRewriter>(dictionary));
  AddRewriter(std::make_unique<CustomDateFormatTokenRewriter>());
#endif  // MOZC_DATE_REWRITER

#ifdef MOZC_FORTUNE_REWRITER
  AddRewriter(std::make_unique<FortuneRewriter>());
#endif  // MOZC_FORTUNE_REWRITER

#ifdef MOZC_COMMAND_REWRITER
  AddRewriter(std::make_unique<CommandRewriter>());
#endif  // MOZC_COMMAND_REWRITER

#ifdef MOZC_USAGE_REWRITER
  AddRewriter(make_unique_from_tuples<UsageRewriter>(
      data_manager.GetUsageRewriterData(), dictionary, pos_matcher));
#endif  // MOZC_USAGE_REWRITER

  AddRewriter(std::make_unique<VersionRewriter>(data_manager.GetDataVersion()));
  AddRewriter(make_unique_from_tuples<CorrectionRewriter>(
      modules, data_manager.GetReadingCorrectionData()));
  AddRewriter(std::make_unique<T13nPromotionRewriter>());
  AddRewriter(make_unique_from_tuples<EnvironmentalFilterRewriter>(
      data_manager.GetEmojiRewriterData()));
  AddRewriter(std::make_unique<RemoveRedundantCandidateRewriter>());
  AddRewriter(make_unique_from_tuples<A11yDescriptionRewriter>(
      data_manager.GetA11yDescriptionRewriterData()));
}

}  // namespace mozc
