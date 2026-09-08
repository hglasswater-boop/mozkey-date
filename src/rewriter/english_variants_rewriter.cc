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

#include "rewriter/english_variants_rewriter.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "base/util.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "protocol/commands.pb.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"
#include "rewriter/rewriter_util.h"

namespace mozc {
namespace {

struct EnglishWordEntry {
  absl::string_view key;
  absl::string_view value;
};

// Mozkey's built-in English word dictionary.  Keep rows grouped by reading;
// when one reading has multiple common spellings they are emitted in this
// order.  The table is intentionally independent from Mozc's system dictionary
// so Mozkey can evolve its practical PC/development vocabulary without
// changing Japanese lexical data.
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
    {"あっぷる", "Apple"},
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
    {"うぃんどうず", "Windows"},
    {"えくすぽーと", "export"},
    {"えくすぷろーらー", "Explorer"},
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

bool HasCandidateValue(const Segment& segment, absl::string_view value) {
  for (const converter::Candidate* candidate : segment.candidates()) {
    if (candidate != nullptr && candidate->value == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Add space-prefixed variants to enable Space-joined English words conversion.
// 'Google Japan' consists of 'ぐーぐる': 'Google', and 'じゃぱん': 'Japan'
// Therefore, when a user types 'ぐーぐるじゃぱん', it will be 'GoogleJapan'
// instead. To avoid this, in non-first segments which follow english word
// segments, add these space-prefixed candidates.
bool EnglishVariantsRewriter::ExpandSpacePrefixedVariants(
    const absl::string_view input, std::vector<std::string>* variants) const {
  DCHECK(variants);

  if (input.empty()) {
    return false;
  }
  if (input.starts_with(' ')) {
    return false;
  }
  std::vector<std::string> space_prefixed_variants;
  space_prefixed_variants.push_back(absl::StrCat(" ", input));
  for (const std::string& word : *variants) {
    if (word.empty()) {
      continue;
    }
    if (!word.starts_with(' ')) {
      space_prefixed_variants.push_back(word);
      space_prefixed_variants.push_back(absl::StrCat(" ", word));
    } else {
      space_prefixed_variants.push_back(word);
    }
  }
  // replace the content of variants
  *variants = std::move(space_prefixed_variants);
  return true;
}

bool EnglishVariantsRewriter::ExpandEnglishVariants(
    const absl::string_view input, std::vector<std::string>* variants) const {
  DCHECK(variants);

  if (input.empty()) {
    return false;
  }

  // multi-word
  if (absl::StrContains(input, " ")) {
    return false;
  }

  std::string lower(input);
  std::string upper(input);
  std::string capitalized(input);
  Util::LowerString(&lower);
  Util::UpperString(&upper);
  Util::CapitalizeString(&capitalized);

  if (lower == upper) {
    // given word is non-ascii.
    return false;
  }

  variants->clear();
  // If |input| is non-standard expression, like "iMac", only
  // expand lowercase.
  if (input != lower && input != upper && input != capitalized) {
    variants->push_back(lower);
    return true;
  }

  if (input != lower) {
    variants->push_back(lower);
  }
  if (input != capitalized) {
    variants->push_back(capitalized);
  }
  if (input != upper) {
    variants->push_back(upper);
  }

  return true;
}

bool EnglishVariantsRewriter::IsT13NCandidate(
    converter::Candidate* candidate) const {
  return (Util::IsEnglishTransliteration(candidate->content_value) &&
          Util::GetScriptType(candidate->content_key) == Util::HIRAGANA);
}

bool EnglishVariantsRewriter::IsEnglishCandidate(
    converter::Candidate* candidate) const {
  return (Util::IsEnglishTransliteration(candidate->content_value) &&
          Util::GetScriptType(candidate->content_key) == Util::ALPHABET);
}

bool EnglishVariantsRewriter::ExpandEnglishVariantsWithSegment(
    bool need_space_prefix, Segment* seg) const {
  CHECK(seg);

  bool modified = false;
  absl::flat_hash_set<std::string> expanded_t13n_candidates;
  absl::flat_hash_set<std::string> original_candidates;

  for (size_t i = 0; i < seg->candidates_size(); ++i) {
    original_candidates.insert(seg->candidate(i).value);
  }

  for (int i = seg->candidates_size() - 1; i >= 0; --i) {
    converter::Candidate* original_candidate = seg->mutable_candidate(i);
    DCHECK(original_candidate);

    // http://b/issue?id=5137299
    // If the entry is coming from user dictionary,
    // expand English variants.
    if (original_candidate->attributes &
            converter::Attribute::NO_VARIANTS_EXPANSION &&
        !(original_candidate->attributes &
          converter::Attribute::USER_DICTIONARY)) {
      continue;
    }

    if (IsT13NCandidate(original_candidate)) {
      if (!(original_candidate->attributes &
            converter::Attribute::NO_VARIANTS_EXPANSION)) {
        modified = true;
        original_candidate->attributes |=
            converter::Attribute::NO_VARIANTS_EXPANSION;
      }

      if (expanded_t13n_candidates.find(original_candidate->value) !=
          expanded_t13n_candidates.end()) {
        continue;
      }

      const bool is_proper_noun =
          (original_candidate->lid == original_candidate->rid &&
           pos_matcher_.IsUniqueNoun(original_candidate->lid));
      if (is_proper_noun && Util::IsUpperAscii(original_candidate->value)) {
        // We do not have to expand upper case proper nouns (ex. NASA).
        // Note:
        // It is very popular that some company or service name is written in
        // lower case, but their formal form is capital case (ex. google)
        // so we suppress expansion only for capital case here.
        continue;
      }

      // Expand T13N candidate variants
      std::vector<std::string> variants;
      bool expanded =
          ExpandEnglishVariants(original_candidate->content_value, &variants);
      if (need_space_prefix) {
        expanded |= ExpandSpacePrefixedVariants(
            original_candidate->content_value, &variants);
      }
      if (expanded) {
        CHECK(!variants.empty());
        for (auto it = variants.rbegin(); it != variants.rend(); ++it) {
          const std::string new_value =
              absl::StrCat(*it, original_candidate->functional_value());
          expanded_t13n_candidates.insert(new_value);
          if (original_candidates.find(new_value) !=
              original_candidates.end()) {
            continue;
          }
          modified = true;

          converter::Candidate* new_candidate = seg->insert_candidate(i + 1);
          DCHECK(new_candidate);
          new_candidate->value = std::move(new_value);
          new_candidate->key = original_candidate->key;
          new_candidate->content_value = std::move(*it);
          new_candidate->content_key = original_candidate->content_key;
          new_candidate->cost = original_candidate->cost;
          new_candidate->wcost = original_candidate->wcost;
          new_candidate->structure_cost = original_candidate->structure_cost;
          new_candidate->lid = original_candidate->lid;
          new_candidate->rid = original_candidate->rid;
          new_candidate->attributes |=
              converter::Attribute::NO_VARIANTS_EXPANSION;
          if (original_candidate->attributes &
              converter::Attribute::PARTIALLY_KEY_CONSUMED) {
            new_candidate->attributes |=
                converter::Attribute::PARTIALLY_KEY_CONSUMED;
            new_candidate->consumed_key_size =
                original_candidate->consumed_key_size;
          }
        }
      }
    } else if (IsEnglishCandidate(original_candidate)) {
      // Fix variants for English candidate
      modified = true;
      original_candidate->attributes |=
          converter::Attribute::NO_VARIANTS_EXPANSION;
    }
  }

  return modified;
}

int EnglishVariantsRewriter::capability(
    const ConversionRequest& request) const {
  if (request.request().mixed_conversion()) {
    return RewriterInterface::ALL;
  }
  return RewriterInterface::CONVERSION;
}

bool EnglishVariantsRewriter::Rewrite(const ConversionRequest& request,
                                      Segments* segments) const {
  const commands::DecoderExperimentParams& params =
      request.request().decoder_experiment_params();
  // 1: enable space insertion
  const bool enable_space_insersion =
      params.english_variation_space_insertion_mode() == 1;
  bool modified = false;
  bool is_previous_candidate_english = false;
  for (Segment& segment : segments->conversion_segments()) {
    // if the top candidate of previous segment is an english word,
    // need_space_prefix = true
    const bool need_space_prefix =
        enable_space_insersion && is_previous_candidate_english;
    modified |= ExpandEnglishVariantsWithSegment(need_space_prefix, &segment);
    is_previous_candidate_english =
        segment.candidates_size() > 0 &&
        Util::IsScriptType(segment.candidate(0).value, Util::ALPHABET);
  }

  return modified;
}

int EnglishWordDictionaryRewriter::capability(
    const ConversionRequest& request) const {
  return RewriterInterface::CONVERSION;
}

bool EnglishWordDictionaryRewriter::Rewrite(const ConversionRequest& request,
                                            Segments* segments) const {
  // For this first version, use_t13n_conversion is the persisted compatibility
  // switch exposed as "English word dictionary" in Mozkey Properties.  The
  // dictionary implementation itself is independent from T13N conversion so a
  // dedicated config field can replace this bridge without changing candidate
  // generation.
  if (!request.config().use_t13n_conversion()) {
    return false;
  }

  bool modified = false;
  for (Segment& segment : segments->conversion_segments()) {
    const absl::string_view key = segment.key();
    size_t insert_position = RewriterUtil::CalculateInsertPosition(segment, 3);

    for (const EnglishWordEntry& entry : kEnglishWordDictionary) {
      if (entry.key != key || HasCandidateValue(segment, entry.value)) {
        continue;
      }

      converter::Candidate* candidate =
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

}  // namespace mozc
