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

#include "absl/flags/flag.h"
#include "absl/strings/string_view.h"
#include "base/container/tuple.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "data_manager/data_manager.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/pos_group.h"
#include "dictionary/pos_matcher.h"
#include "dictionary/single_kanji_dictionary.h"
#include "engine/modules.h"
#include "request/conversion_request.h"
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
#include "rewriter/rewriter_interface.h"
#include "rewriter/rewriter_util.h"
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

// Mozkey's built-in English word dictionary.
//
// The table intentionally lives independently from Mozc's system dictionary so
// coverage and ranking can evolve without changing Japanese conversion data.
// Keys are readings used by conversion segments; multiple rows with the same
// key are allowed and are emitted in table order.
struct EnglishWordEntry {
  absl::string_view key;
  absl::string_view value;
};

constexpr EnglishWordEntry kEnglishWordDictionary[] = {
    {"あいこん", "icon"},
    {"あいであ", "idea"},
    {"あうとぷっと", "output"},
    {"あかうんと", "account"},
    {"あくせす", "access"},
    {"あくしょん", "action"},
    {"あどれす", "address"},
    {"あぷり", "app"},
    {"あぷり", "application"},
    {"あぷりけーしょん", "application"},
    {"あっぷでーと", "update"},
    {"あっぷる", "apple"},
    {"あっぷろーど", "upload"},
    {"あるごりずむ", "algorithm"},
    {"いべんと", "event"},
    {"いめーじ", "image"},
    {"いんすたんす", "instance"},
    {"いんすとーる", "install"},
    {"いんたーねっと", "internet"},
    {"いんたーふぇーす", "interface"},
    {"いんでっくす", "index"},
    {"いんぷっと", "input"},
    {"うぃじぇっと", "widget"},
    {"うぃんどう", "window"},
    {"えくすぽーと", "export"},
    {"えくすぷろーらー", "explorer"},
    {"えでぃた", "editor"},
    {"えでぃたー", "editor"},
    {"えらー", "error"},
    {"えんじん", "engine"},
    {"おぶじぇくと", "object"},
    {"おふらいん", "offline"},
    {"おぷしょん", "option"},
    {"おんらいん", "online"},
    {"かれんだー", "calendar"},
    {"きー", "key"},
    {"きーぼーど", "keyboard"},
    {"きゃっしゅ", "cache"},
    {"きゃすと", "cast"},
    {"きゃんでぃでーと", "candidate"},
    {"きゅー", "queue"},
    {"くらいあんと", "client"},
    {"くらうど", "cloud"},
    {"くりっく", "click"},
    {"ぐるーぷ", "group"},
    {"ぐろーばる", "global"},
    {"けーす", "case"},
    {"こーど", "code"},
    {"こぴー", "copy"},
    {"こまんど", "command"},
    {"こみっと", "commit"},
    {"こんそーる", "console"},
    {"こんてきすと", "context"},
    {"こんてな", "container"},
    {"こんばーじょん", "conversion"},
    {"こんぱいる", "compile"},
    {"こんぴゅーた", "computer"},
    {"こんぴゅーたー", "computer"},
    {"さーち", "search"},
    {"さーば", "server"},
    {"さーばー", "server"},
    {"さーびす", "service"},
    {"さいず", "size"},
    {"さいと", "site"},
    {"しすてむ", "system"},
    {"しーん", "scene"},
    {"すくりぷと", "script"},
    {"すたっく", "stack"},
    {"すてーたす", "status"},
    {"すとりーむ", "stream"},
    {"すとーりーぼーど", "storyboard"},
    {"すとれーじ", "storage"},
    {"すぺーす", "space"},
    {"すれっど", "thread"},
    {"せきゅりてぃ", "security"},
    {"せってぃんぐ", "setting"},
    {"せっしょん", "session"},
    {"そけっと", "socket"},
    {"たぐ", "tag"},
    {"たすく", "task"},
    {"たいとる", "title"},
    {"だうんろーど", "download"},
    {"てきすと", "text"},
    {"てすと", "test"},
    {"てーぶる", "table"},
    {"てんぷれーと", "template"},
    {"でーた", "data"},
    {"でーたべーす", "database"},
    {"でばいす", "device"},
    {"でばっぐ", "debug"},
    {"でぃくしょなり", "dictionary"},
    {"でぃれくとり", "directory"},
    {"どきゅめんと", "document"},
    {"どめいん", "domain"},
    {"どらいば", "driver"},
    {"どらいばー", "driver"},
    {"どらっぐ", "drag"},
    {"どろっぷ", "drop"},
    {"とーくん", "token"},
    {"ねっとわーく", "network"},
    {"ねーむ", "name"},
    {"のーど", "node"},
    {"ばいなり", "binary"},
    {"ばっくあっぷ", "backup"},
    {"ばーじょん", "version"},
    {"ぱす", "path"},
    {"ぱすわーど", "password"},
    {"ぱっけーじ", "package"},
    {"ぱらめーた", "parameter"},
    {"ぱらめーたー", "parameter"},
    {"びるど", "build"},
    {"ふぁいる", "file"},
    {"ふぃるた", "filter"},
    {"ふぃるたー", "filter"},
    {"ふぉるだ", "folder"},
    {"ふぉるだー", "folder"},
    {"ふぉーまっと", "format"},
    {"ふらぐ", "flag"},
    {"ふらっしゅ", "flush"},
    {"ふれーむ", "frame"},
    {"ぶらうざ", "browser"},
    {"ぶらうざー", "browser"},
    {"ぶらんち", "branch"},
    {"ぷらぐいん", "plugin"},
    {"ぷれいやー", "player"},
    {"ぷろぐらむ", "program"},
    {"ぷろじぇくと", "project"},
    {"ぷろせす", "process"},
    {"ぷろとこる", "protocol"},
    {"ぷろぱてぃ", "property"},
    {"ぷろぱてぃー", "property"},
    {"ぼたん", "button"},
    {"ほすと", "host"},
    {"ぽーと", "port"},
    {"まうす", "mouse"},
    {"まーじ", "merge"},
    {"めそっど", "method"},
    {"めにゅー", "menu"},
    {"めもり", "memory"},
    {"もじゅーる", "module"},
    {"もーど", "mode"},
    {"ゆーざ", "user"},
    {"ゆーざー", "user"},
    {"らいぶらり", "library"},
    {"りくえすと", "request"},
    {"りすと", "list"},
    {"りすぽんす", "response"},
    {"りぽじとり", "repository"},
    {"りんく", "link"},
    {"るーた", "router"},
    {"るーたー", "router"},
    {"るーる", "rule"},
    {"れいあうと", "layout"},
    {"れこーど", "record"},
    {"れんだら", "renderer"},
    {"れんだらー", "renderer"},
    {"ろぐ", "log"},
    {"ろーかる", "local"},
    {"わーか", "worker"},
    {"わーかー", "worker"},
};

bool HasCandidateValue(const Segment &segment, absl::string_view value) {
  for (const converter::Candidate &candidate : segment.candidates()) {
    if (candidate.value == value) {
      return true;
    }
  }
  return false;
}

class EnglishWordDictionaryRewriter final : public RewriterInterface {
 public:
  int capability(const ConversionRequest &request) const override {
    return RewriterInterface::CONVERSION;
  }

  bool Rewrite(const ConversionRequest &request,
               Segments *segments) const override {
    // use_t13n_conversion is the persisted compatibility switch exposed as
    // "English word dictionary" in Mozkey's Properties dialog.
    if (!request.config().use_t13n_conversion()) {
      return false;
    }

    bool modified = false;
    for (Segment &segment : segments->conversion_segments()) {
      const absl::string_view key = segment.key();
      size_t insert_position = RewriterUtil::CalculateInsertPosition(segment, 3);

      for (const EnglishWordEntry &entry : kEnglishWordDictionary) {
        if (entry.key != key || HasCandidateValue(segment, entry.value)) {
          continue;
        }

        converter::Candidate *candidate =
            segment.insert_candidate(insert_position++);
        candidate->key = std::string(key);
        candidate->content_key = candidate->key;
        candidate->value = std::string(entry.value);
        candidate->content_value = candidate->value;
        candidate->description = "英単語辞書";
        candidate->attributes |=
            (converter::Attribute::NO_LEARNING |
             converter::Attribute::NO_VARIANTS_EXPANSION);
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
  AddRewriter(std::make_unique<EnglishWordDictionaryRewriter>());
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
