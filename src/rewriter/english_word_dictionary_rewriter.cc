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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"

namespace mozc {
namespace {

constexpr size_t kMaxEnglishWordLength = 40;
constexpr size_t kMaxPrefixCandidates = 8;
constexpr size_t kMaxSpellingCandidates = 5;
constexpr size_t kPrefixScanLimit = 512;

struct EnglishWordData {
  absl::string_view word;
  uint8_t tier;
};

// Generated from ESDB/SCOWL. The generated words are ASCII lower-case and
// sorted lexicographically so prefix lookup can use lower_bound.
#include "rewriter/english_word_dictionary_data.inc"

struct ManualWordEntry {
  absl::string_view key;
  absl::string_view value;
  uint8_t tier;
};

// Modern product/development vocabulary and canonical capitalization that is
// useful for PC input but may lag behind a general spelling dictionary.
constexpr ManualWordEntry kManualWords[] = {
    {"android", "Android", 20},
    {"api", "API", 20},
    {"azure", "Azure", 20},
    {"bazel", "Bazel", 20},
    {"chatgpt", "ChatGPT", 20},
    {"chromium", "Chromium", 20},
    {"docker", "Docker", 20},
    {"github", "GitHub", 20},
    {"gitlab", "GitLab", 20},
    {"gmail", "Gmail", 20},
    {"google", "Google", 20},
    {"javascript", "JavaScript", 20},
    {"json", "JSON", 20},
    {"kubernetes", "Kubernetes", 20},
    {"linux", "Linux", 20},
    {"macos", "macOS", 20},
    {"markdown", "Markdown", 20},
    {"mysql", "MySQL", 20},
    {"nodejs", "Node.js", 20},
    {"npm", "npm", 20},
    {"nvidia", "NVIDIA", 20},
    {"openai", "OpenAI", 20},
    {"postgresql", "PostgreSQL", 20},
    {"powershell", "PowerShell", 20},
    {"python", "Python", 20},
    {"react", "React", 20},
    {"rust", "Rust", 20},
    {"sqlite", "SQLite", 20},
    {"typescript", "TypeScript", 20},
    {"unicode", "Unicode", 20},
    {"windows", "Windows", 20},
    {"yaml", "YAML", 20},
};

struct RankedWord {
  absl::string_view key;
  absl::string_view value;
  uint8_t tier;
  int distance;
};

bool IsAsciiWordInput(absl::string_view input) {
  if (input.empty() || input.size() > kMaxEnglishWordLength) {
    return false;
  }
  bool has_letter = false;
  for (const unsigned char c : input) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      has_letter = true;
      continue;
    }
    if (c == '\'' || c == '-') {
      continue;
    }
    return false;
  }
  return has_letter;
}

std::string LowerAscii(absl::string_view input) {
  std::string result(input);
  for (char& c : result) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return result;
}

std::string UpperAscii(absl::string_view input) {
  std::string result(input);
  for (char& c : result) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return result;
}

bool IsAllUpperAscii(absl::string_view input) {
  bool has_letter = false;
  for (const char c : input) {
    if (c >= 'a' && c <= 'z') {
      return false;
    }
    if (c >= 'A' && c <= 'Z') {
      has_letter = true;
    }
  }
  return has_letter;
}

bool IsTitleAscii(absl::string_view input) {
  if (input.empty() || input.front() < 'A' || input.front() > 'Z') {
    return false;
  }
  for (size_t i = 1; i < input.size(); ++i) {
    const char c = input[i];
    if (c >= 'A' && c <= 'Z') {
      return false;
    }
  }
  return true;
}

std::string ApplyInputCase(absl::string_view input,
                           absl::string_view canonical) {
  if (IsAllUpperAscii(input)) {
    return UpperAscii(canonical);
  }
  std::string result(canonical);
  if (IsTitleAscii(input) && !result.empty() && result.front() >= 'a' &&
      result.front() <= 'z') {
    result.front() = static_cast<char>(result.front() - 'a' + 'A');
  }
  return result;
}

absl::string_view CanonicalValue(absl::string_view key) {
  for (const ManualWordEntry& entry : kManualWords) {
    if (entry.key == key) {
      return entry.value;
    }
  }
  return key;
}

bool HasCandidateValue(const Segment& segment, absl::string_view value) {
  for (const converter::Candidate* candidate : segment.candidates()) {
    if (candidate != nullptr && candidate->value == value) {
      return true;
    }
  }
  return false;
}

bool HasExactGeneratedWord(absl::string_view key) {
  const auto begin = std::begin(kEnglishWordDictionary);
  const auto end = std::end(kEnglishWordDictionary);
  const auto it = std::lower_bound(
      begin, end, key,
      [](const EnglishWordData& entry, absl::string_view value) {
        return entry.word < value;
      });
  return it != end && it->word == key;
}

bool HasExactWord(absl::string_view key) {
  for (const ManualWordEntry& entry : kManualWords) {
    if (entry.key == key) {
      return true;
    }
  }
  return HasExactGeneratedWord(key);
}

void AddRankedPrefixWords(absl::string_view prefix,
                          std::vector<RankedWord>* result) {
  for (const ManualWordEntry& entry : kManualWords) {
    if (entry.key != prefix && absl::StartsWith(entry.key, prefix)) {
      result->push_back({entry.key, entry.value, entry.tier, 0});
    }
  }

  const auto begin = std::begin(kEnglishWordDictionary);
  const auto end = std::end(kEnglishWordDictionary);
  auto it = std::lower_bound(
      begin, end, prefix,
      [](const EnglishWordData& entry, absl::string_view value) {
        return entry.word < value;
      });

  size_t scanned = 0;
  for (; it != end && absl::StartsWith(it->word, prefix) &&
         scanned < kPrefixScanLimit;
       ++it, ++scanned) {
    if (it->word == prefix) {
      continue;
    }
    result->push_back(
        {it->word, CanonicalValue(it->word), it->tier, 0});
  }
}

int BoundedDamerauLevenshtein(absl::string_view source,
                              absl::string_view target,
                              int max_distance) {
  const int source_size = static_cast<int>(source.size());
  const int target_size = static_cast<int>(target.size());
  if (std::abs(source_size - target_size) > max_distance) {
    return max_distance + 1;
  }
  if (source_size > static_cast<int>(kMaxEnglishWordLength) ||
      target_size > static_cast<int>(kMaxEnglishWordLength)) {
    return max_distance + 1;
  }

  std::array<int, kMaxEnglishWordLength + 1> previous_previous{};
  std::array<int, kMaxEnglishWordLength + 1> previous{};
  std::array<int, kMaxEnglishWordLength + 1> current{};

  for (int j = 0; j <= target_size; ++j) {
    previous[j] = j;
  }

  for (int i = 1; i <= source_size; ++i) {
    current[0] = i;
    int row_min = current[0];
    for (int j = 1; j <= target_size; ++j) {
      const int substitution_cost =
          source[i - 1] == target[j - 1] ? 0 : 1;
      current[j] = std::min(
          {previous[j] + 1, current[j - 1] + 1,
           previous[j - 1] + substitution_cost});

      if (i > 1 && j > 1 && source[i - 1] == target[j - 2] &&
          source[i - 2] == target[j - 1]) {
        current[j] = std::min(current[j], previous_previous[j - 2] + 1);
      }
      row_min = std::min(row_min, current[j]);
    }

    if (row_min > max_distance) {
      return max_distance + 1;
    }
    previous_previous.swap(previous);
    previous.swap(current);
  }

  return previous[target_size];
}

void AddSpellingMatches(absl::string_view input,
                        std::vector<RankedWord>* result) {
  const int max_distance = input.size() <= 4 ? 1 : 2;

  for (const ManualWordEntry& entry : kManualWords) {
    const int distance =
        BoundedDamerauLevenshtein(input, entry.key, max_distance);
    if (distance > 0 && distance <= max_distance) {
      result->push_back({entry.key, entry.value, entry.tier, distance});
    }
  }

  for (const EnglishWordData& entry : kEnglishWordDictionary) {
    const int distance =
        BoundedDamerauLevenshtein(input, entry.word, max_distance);
    if (distance > 0 && distance <= max_distance) {
      result->push_back(
          {entry.word, CanonicalValue(entry.word), entry.tier, distance});
    }
  }
}

size_t InsertCandidate(Segment* segment, size_t position,
                       absl::string_view input,
                       const RankedWord& ranked_word,
                       absl::string_view description,
                       bool spelling_correction) {
  const std::string value = ApplyInputCase(input, ranked_word.value);
  if (HasCandidateValue(*segment, value)) {
    return position;
  }

  position = std::min(position, segment->candidates_size());
  converter::Candidate* candidate = segment->insert_candidate(position);
  candidate->key = std::string(input);
  candidate->content_key = candidate->key;
  candidate->value = value;
  candidate->content_value = candidate->value;
  candidate->description = std::string(description);
  candidate->attributes |= converter::Attribute::NO_VARIANTS_EXPANSION;
  if (spelling_correction) {
    candidate->attributes |= converter::Attribute::SPELLING_CORRECTION;
    candidate->prefix = "→ ";
  }
  return position + 1;
}

bool AddPrefixCandidates(absl::string_view raw_input,
                         absl::string_view lower_input,
                         Segment* segment) {
  if (lower_input.size() < 2) {
    return false;
  }

  std::vector<RankedWord> matches;
  AddRankedPrefixWords(lower_input, &matches);
  std::sort(matches.begin(), matches.end(),
            [lower_input](const RankedWord& lhs, const RankedWord& rhs) {
              if (lhs.tier != rhs.tier) {
                return lhs.tier < rhs.tier;
              }
              const size_t lhs_remaining = lhs.key.size() - lower_input.size();
              const size_t rhs_remaining = rhs.key.size() - lower_input.size();
              if (lhs_remaining != rhs_remaining) {
                return lhs_remaining < rhs_remaining;
              }
              return lhs.key < rhs.key;
            });

  size_t insert_position = std::min<size_t>(1, segment->candidates_size());
  size_t inserted = 0;
  for (const RankedWord& word : matches) {
    const size_t old_position = insert_position;
    insert_position = InsertCandidate(segment, insert_position, raw_input, word,
                                      "英単語補完", false);
    if (insert_position != old_position && ++inserted >= kMaxPrefixCandidates) {
      break;
    }
  }
  return inserted > 0;
}

bool AddSpellingCandidates(absl::string_view raw_input,
                           absl::string_view lower_input,
                           Segment* segment) {
  if (lower_input.size() < 3 || HasExactWord(lower_input)) {
    return false;
  }

  std::vector<RankedWord> matches;
  AddSpellingMatches(lower_input, &matches);
  std::sort(matches.begin(), matches.end(),
            [lower_input](const RankedWord& lhs, const RankedWord& rhs) {
              if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
              }
              if (lhs.tier != rhs.tier) {
                return lhs.tier < rhs.tier;
              }
              const int lhs_length_delta = std::abs(
                  static_cast<int>(lhs.key.size()) -
                  static_cast<int>(lower_input.size()));
              const int rhs_length_delta = std::abs(
                  static_cast<int>(rhs.key.size()) -
                  static_cast<int>(lower_input.size()));
              if (lhs_length_delta != rhs_length_delta) {
                return lhs_length_delta < rhs_length_delta;
              }
              return lhs.key < rhs.key;
            });

  size_t insert_position = std::min<size_t>(1, segment->candidates_size());
  size_t inserted = 0;
  for (const RankedWord& word : matches) {
    const size_t old_position = insert_position;
    insert_position = InsertCandidate(segment, insert_position, raw_input, word,
                                      "英語スペル候補", true);
    if (insert_position != old_position &&
        ++inserted >= kMaxSpellingCandidates) {
      break;
    }
  }
  return inserted > 0;
}

}  // namespace

int EnglishWordDictionaryRewriter::capability(
    const ConversionRequest&) const {
  return RewriterInterface::ALL;
}

bool EnglishWordDictionaryRewriter::Rewrite(const ConversionRequest& request,
                                            Segments* segments) const {
  if (!request.config().use_english_word_dictionary()) {
    return false;
  }

  bool modified = false;
  for (Segment& segment : segments->conversion_segments()) {
    const absl::string_view raw_input = segment.key();
    if (!IsAsciiWordInput(raw_input)) {
      continue;
    }
    const std::string lower_input = LowerAscii(raw_input);

    modified |= AddPrefixCandidates(raw_input, lower_input, &segment);

    if (request.request_type() == ConversionRequest::CONVERSION &&
        request.config().use_english_spelling_correction()) {
      modified |= AddSpellingCandidates(raw_input, lower_input, &segment);
    }
  }
  return modified;
}

}  // namespace mozc
