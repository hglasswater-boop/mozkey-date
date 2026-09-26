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

// Session class of Mozc server.

#include "session/session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "base/clock.h"
#include "base/strings/unicode.h"
#include "base/util.h"
#include "composer/composer.h"
#include "composer/key_event_util.h"
#include "composer/table.h"
#include "engine/engine_converter_interface.h"
#include "engine/engine_interface.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "session/ime_context.h"
#include "session/key_event_transformer.h"
#include "session/keymap.h"
#include "session/vertical_writing_key_transform.h"
#include "session/zenz_client_context.h"
#include "session/zenz_client_factory.h"
#include "session/zenz_context_assembler.h"
#include "session/zenz_prompt_builder.h"
#include "session/zenz_text_privacy_analyzer.h"
#include "transliteration/transliteration.h"

#ifdef __APPLE__
#include <TargetConditionals.h>  // for TARGET_OS_IPHONE
#endif                           // __APPLE__

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mozc {
namespace session {
namespace {

using ::mozc::engine::ConversionPreferences;
using ::mozc::engine::EngineConverterInterface;

#if defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)
std::wstring Utf8ToWideForDebug(absl::string_view s) {
  if (s.empty()) {
    return std::wstring();
  }

  const int input_size = static_cast<int>(s.size());
  const int wide_size =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, nullptr, 0);
  if (wide_size <= 0) {
    return L"<invalid utf8>";
  }

  std::wstring w(wide_size, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, w.data(), wide_size);
  return w;
}

void MozcLeftContextDebugOutput(absl::string_view message) {
  std::wstring w = Utf8ToWideForDebug(message);
  w.push_back(L'\n');
  ::OutputDebugStringW(w.c_str());
}
#endif  // defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)

#if defined(_WIN32)
std::wstring Utf8ToWideForZenzDebug(absl::string_view s) {
  if (s.empty()) {
    return std::wstring();
  }

  const int input_size = static_cast<int>(s.size());
  const int wide_size =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, nullptr, 0);
  if (wide_size <= 0) {
    return L"<invalid utf8>";
  }

  std::wstring w(wide_size, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), input_size, w.data(), wide_size);
  return w;
}

void ZenzDebugOutput(absl::string_view message) {
  std::wstring w = Utf8ToWideForZenzDebug(message);
  w.push_back(L'\n');
  ::OutputDebugStringW(w.c_str());
}
#else
void ZenzDebugOutput(absl::string_view) {}
#endif

std::string ZenzRedactedTextStats(absl::string_view label,
                                  absl::string_view text) {
  return absl::StrCat(
      label, "_bytes=", text.size(),
      " ", label, "_chars=", Util::CharsLen(text));
}

std::string ZenzBool(bool value) {
  return value ? "true" : "false";
}

std::string ZenzSafeDebugReason(absl::string_view debug) {
  if (debug.empty()) {
    return "";
  }

  // The scorer is a local helper process, but the response still crosses a
  // process boundary.  Never propagate arbitrary scorer-provided debug text to
  // DebugView or client-visible Output fields.  Keep only short symbolic ASCII
  // reason strings.  Anything else is collapsed to a generic marker.
  constexpr size_t kMaxSafeDebugReasonBytes = 80;

  if (debug.size() > kMaxSafeDebugReasonBytes) {
    return "external_debug";
  }

  for (const unsigned char c : debug) {
    const bool safe =
        ('a' <= c && c <= 'z') ||
        ('A' <= c && c <= 'Z') ||
        ('0' <= c && c <= '9') ||
        c == '_' ||
        c == '-' ||
        c == '.';

    if (!safe) {
      return "external_debug";
    }
  }

  return std::string(debug);
}

// Maximum size of multiple undo stack.
const size_t kMultipleUndoMaxSize = 10;

// Avoid teaching one-character particles or accidental raw commits as strong
// user segment history when the user cancels a conversion and immediately
// commits the restored hiragana preedit.
constexpr size_t kMinRerankedPreeditCommitCharsAfterConvertCancel = 2;

constexpr uint32_t kDefaultZenzConversionTimeoutMsec = 1000;
constexpr uint32_t kDefaultZenzConversionPollMsec = 24;
constexpr uint32_t kZenzSuggestionDebounceMsec = 250;
constexpr uint32_t kMinimumZenzConversionKeyLength = 2;
constexpr int32_t kZenzSuggestionCandidateId = 0x7fffffff;
constexpr uint32_t kMaxZenzConversionRightContextLength = 128;
constexpr uint32_t kMaxZenzConversionTimeoutMsec = 1000;

// This is not the model inference timeout.  It is the maximum time the session
// keeps polling the async worker after the request has been submitted.  Cold
// start may include scorer process launch, pipe creation, llama-server startup,
// ready probing, and then the actual completion request.  Since this path is
// async, a longer poll window does not block the IME thread.
constexpr uint32_t kZenzConversionAsyncWaitMsec = 3000;

bool IsAsciiIdentityChar(const unsigned char c) {
  return (('0' <= c) && (c <= '9')) || (('A' <= c) && (c <= 'Z')) ||
         (('a' <= c) && (c <= 'z')) || c == '_' || c == '-' || c == '.' ||
         c == '+' || c == '#';
}

bool HasAsciiLetterOrDigit(absl::string_view value) {
  for (const unsigned char c : value) {
    if ((('0' <= c) && (c <= '9')) || (('A' <= c) && (c <= 'Z')) ||
        (('a' <= c) && (c <= 'z'))) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> ExtractAsciiIdentitySurfaces(
    absl::string_view value) {
  std::vector<std::string> surfaces;
  size_t start = absl::string_view::npos;

  auto flush = [&](size_t end) {
    if (start == absl::string_view::npos || end <= start) {
      start = absl::string_view::npos;
      return;
    }
    const absl::string_view surface = value.substr(start, end - start);
    if (HasAsciiLetterOrDigit(surface)) {
      surfaces.emplace_back(surface);
    }
    start = absl::string_view::npos;
  };

  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (IsAsciiIdentityChar(c)) {
      if (start == absl::string_view::npos) {
        start = i;
      }
    } else {
      flush(i);
    }
  }
  flush(value.size());

  return surfaces;
}

std::string FindPreeditKeyForValue(const commands::Preedit& preedit,
                                   absl::string_view value) {
  for (int i = 0; i < preedit.segment_size(); ++i) {
    const commands::Preedit::Segment& segment = preedit.segment(i);
    if (segment.value() == value) {
      return segment.key();
    }
  }
  return std::string();
}

ProtectedConversionSpan* FindProtectedSurface(
    std::vector<ProtectedConversionSpan>* protected_spans,
    absl::string_view value) {
  if (protected_spans == nullptr) {
    return nullptr;
  }
  for (ProtectedConversionSpan& span : *protected_spans) {
    if (span.value == value) {
      return &span;
    }
  }
  return nullptr;
}

size_t CountSurfaceOccurrences(absl::string_view value,
                               absl::string_view surface) {
  if (surface.empty()) {
    return 0;
  }

  size_t count = 0;
  size_t pos = 0;
  while ((pos = value.find(surface, pos)) != absl::string_view::npos) {
    ++count;
    pos += surface.size();
  }
  return count;
}

struct ZenzReverseLearningProjection {
  std::vector<std::pair<std::string, std::string>> changed_segments;
  std::vector<ZenzProjectedLearningSegment> projected_segments;
};

ZenzReverseLearningProjection BuildZenzReverseLearningSegmentsFromPreedit(
    const commands::Preedit& preedit,
    absl::string_view full_key,
    absl::string_view full_value) {
  constexpr int kMaxReverseLearningPairs = 4;

  const int segment_size = preedit.segment_size();
  if (segment_size <= 0 || full_key.empty() || full_value.empty()) {
    return {};
  }

  std::string concatenated_key;
  std::vector<std::string> keys;
  std::vector<std::string> mozc_values;
  keys.reserve(segment_size);
  mozc_values.reserve(segment_size);

  for (int i = 0; i < segment_size; ++i) {
    const commands::Preedit::Segment& segment = preedit.segment(i);
    if (segment.key().empty() || segment.value().empty()) {
      return {};
    }
    keys.push_back(segment.key());
    mozc_values.push_back(segment.value());
    concatenated_key.append(segment.key());
  }

  // The snapshot must describe the same full Zenz request.  Otherwise it may
  // belong to an older conversion generation and must not be used.
  if (concatenated_key != full_key) {
    return {};
  }

  struct Anchor {
    int index;
    size_t begin;
    size_t end;
  };

  std::vector<Anchor> anchors;
  anchors.reserve(segment_size);
  for (int i = 0; i < segment_size; ++i) {
    const std::string& value = mozc_values[i];
    if (CountSurfaceOccurrences(full_value, value) != 1) {
      continue;
    }
    const size_t begin = full_value.find(value);
    if (begin == absl::string_view::npos) {
      return {};
    }
    anchors.push_back(Anchor{i, begin, begin + value.size()});
  }

  size_t previous_anchor_end = 0;
  for (const Anchor& anchor : anchors) {
    if (anchor.begin < previous_anchor_end) {
      return {};
    }
    previous_anchor_end = anchor.end;
  }

  std::vector<std::string> projected_values(segment_size);
  int left_index = -1;
  size_t left_value_end = 0;

  auto assign_gap = [&](int first_index, int last_index,
                        absl::string_view value) -> bool {
    const int gap_size = last_index - first_index + 1;
    if (gap_size <= 0) {
      return value.empty();
    }

    if (gap_size == 1) {
      if (value.empty()) {
        return false;
      }
      projected_values[first_index] = std::string(value);
      return true;
    }

    std::string original_gap_value;
    for (int i = first_index; i <= last_index; ++i) {
      original_gap_value.append(mozc_values[i]);
    }
    if (original_gap_value != value) {
      return false;
    }
    for (int i = first_index; i <= last_index; ++i) {
      projected_values[i] = mozc_values[i];
    }
    return true;
  };

  for (const Anchor& anchor : anchors) {
    if (!assign_gap(left_index + 1, anchor.index - 1,
                    full_value.substr(left_value_end,
                                      anchor.begin - left_value_end))) {
      return {};
    }
    projected_values[anchor.index] = mozc_values[anchor.index];
    left_index = anchor.index;
    left_value_end = anchor.end;
  }

  if (!assign_gap(left_index + 1, segment_size - 1,
                  full_value.substr(left_value_end))) {
    return {};
  }

  std::string reconstructed_value;
  for (const std::string& value : projected_values) {
    if (value.empty()) {
      return {};
    }
    reconstructed_value.append(value);
  }
  if (reconstructed_value != full_value) {
    return {};
  }

  ZenzReverseLearningProjection result;
  result.projected_segments.reserve(segment_size);
  for (int i = 0; i < segment_size; ++i) {
    result.projected_segments.push_back(
        {keys[i], projected_values[i],
         projected_values[i] != mozc_values[i]});
  }

  for (int i = 0; i < segment_size; ++i) {
    // Full-sequence learning already covers the whole accepted result.  The
    // reverse path records only segments that Zenz actually changed relative to
    // the visible Mozc conversion result.
    if (projected_values[i] == mozc_values[i]) {
      continue;
    }
    if (keys[i] == full_key && projected_values[i] == full_value) {
      continue;
    }
    result.changed_segments.push_back({keys[i], projected_values[i]});
    if (result.changed_segments.size() > kMaxReverseLearningPairs) {
      return {};
    }
  }

  // If the correction rewrites too many independent segments, it is likely a
  // style-level rewrite rather than a stable local conversion preference.
  if (result.changed_segments.empty()) {
    result.projected_segments.clear();
  }
  return result;
}


std::string InferProtectedKeyForEmbeddedSurface(absl::string_view segment_key,
                                                absl::string_view segment_value,
                                                absl::string_view surface) {
  if (segment_key.empty() || segment_value.empty() || surface.empty()) {
    return std::string();
  }

  if (segment_value == surface) {
    return std::string(segment_key);
  }

  // Keep this inference deliberately narrow.  It is safe to infer the protected
  // reading when the protected surface starts the segment and the remaining
  // value suffix appears literally at the end of the segment key, e.g.
  //   key:   もずきーを
  //   value: Mozkeyを
  //   span:  もずきー -> Mozkey
  // For converted suffixes such as "Mozkeyを使用しています" vs
  // "もずきーをしようしています", this returns empty and leaves the span
  // reject-only instead of guessing a boundary.
  if (!absl::StartsWith(segment_value, surface)) {
    return std::string();
  }

  const absl::string_view suffix = segment_value.substr(surface.size());
  if (suffix.empty() || suffix.size() >= segment_key.size()) {
    return std::string();
  }
  if (!absl::EndsWith(segment_key, suffix)) {
    return std::string();
  }

  return std::string(segment_key.substr(0, segment_key.size() - suffix.size()));
}

std::string FindPreeditKeyForProtectedSurface(
    const commands::Preedit& preedit, absl::string_view surface,
    absl::string_view candidate_key) {
  if (surface.empty()) {
    return std::string();
  }

  for (int i = 0; i < preedit.segment_size(); ++i) {
    const commands::Preedit::Segment& segment = preedit.segment(i);
    if (segment.value().empty()) {
      continue;
    }

    const std::string segment_key =
        segment.has_key() ? segment.key() : std::string();
    const std::string protected_key = InferProtectedKeyForEmbeddedSurface(
        segment_key, segment.value(), surface);
    if (!protected_key.empty()) {
      return protected_key;
    }

    // Some conversion preedit segments can be larger than the protected
    // word, e.g. "じしょごのてんてきです" -> "辞書語の点滴です".
    // In that case the visible suffix is converted, so literal suffix matching
    // cannot infer the boundary.  If the user-dictionary key and value both
    // start the same preedit segment, use the candidate key as the protected
    // reading.
    if (!candidate_key.empty() && absl::StartsWith(segment.value(), surface) &&
        absl::StartsWith(segment_key, candidate_key)) {
      return std::string(candidate_key);
    }
  }
  return std::string();
}

void AddOrUpdateProtectedSpan(
    absl::string_view key, absl::string_view value,
    ProtectedConversionSpan::Tier tier, bool repairable,
    absl::string_view mozc_value,
    std::vector<ProtectedConversionSpan>* protected_spans) {
  if (protected_spans == nullptr || value.empty() ||
      !absl::StrContains(mozc_value, value)) {
    return;
  }

  size_t required_occurrences = CountSurfaceOccurrences(mozc_value, value);
  if (required_occurrences == 0) {
    required_occurrences = 1;
  }

  if (ProtectedConversionSpan* existing =
          FindProtectedSurface(protected_spans, value)) {
    if (existing->required_occurrences < required_occurrences) {
      existing->required_occurrences = required_occurrences;
    }
    if (existing->key.empty() && !key.empty()) {
      existing->key = std::string(key);
    }
    if (tier == ProtectedConversionSpan::Tier::kIdentityCritical) {
      existing->tier = tier;
    }
    if (!existing->repairable && repairable && !key.empty()) {
      existing->repairable = true;
    }
    return;
  }

  ProtectedConversionSpan span;
  span.key = !key.empty() ? std::string(key) : std::string();
  span.value = std::string(value);
  span.tier = tier;
  span.repairable = repairable && !span.key.empty();
  span.required_occurrences = required_occurrences;
  protected_spans->push_back(std::move(span));
}

void AddPreeditIdentityProtectedSpans(
    const commands::Preedit& preedit, absl::string_view mozc_value,
    std::vector<ProtectedConversionSpan>* protected_spans) {
  for (int i = 0; i < preedit.segment_size(); ++i) {
    const commands::Preedit::Segment& segment = preedit.segment(i);
    if (segment.value().empty()) {
      continue;
    }

    const std::string segment_key =
        segment.has_key() ? segment.key() : std::string();
    const std::vector<std::string> identity_surfaces =
        ExtractAsciiIdentitySurfaces(segment.value());
    for (const std::string& surface : identity_surfaces) {
      const std::string surface_key = InferProtectedKeyForEmbeddedSurface(
          segment_key, segment.value(), surface);
      AddOrUpdateProtectedSpan(
          surface_key, surface, ProtectedConversionSpan::Tier::kIdentityCritical,
          !surface_key.empty(), mozc_value, protected_spans);
    }
  }
}


bool IsSafeDirectUserDictionaryProtectedEntry(absl::string_view key,
                                              absl::string_view value) {
  if (key.empty() || value.empty()) {
    return false;
  }

  // Very short entries are too ambiguous for direct global matching. They can
  // still be protected through candidate provenance when the converter exposes
  // the selected user-dictionary candidate.
  if (Util::CharsLen(key) < 2 || Util::CharsLen(value) < 2) {
    return false;
  }

  return true;
}

std::vector<absl::string_view> GetUtf8SuffixesForUserDictionaryLookup(
    absl::string_view key) {
  std::vector<absl::string_view> suffixes;
  if (key.empty()) {
    return suffixes;
  }

  suffixes.reserve(Util::CharsLen(key));
  for (size_t i = 0; i < key.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(key[i]);
    if (i != 0 && (c & 0xC0) == 0x80) {
      continue;
    }
    suffixes.push_back(key.substr(i));
  }
  return suffixes;
}

void AddDirectUserDictionaryEntryProtectedSpans(
    const engine::EngineConverterInterface& converter,
    absl::string_view conversion_key, absl::string_view mozc_value,
    std::vector<ProtectedConversionSpan>* protected_spans) {
  if (protected_spans == nullptr || conversion_key.empty() ||
      mozc_value.empty()) {
    return;
  }

  for (const absl::string_view suffix :
       GetUtf8SuffixesForUserDictionaryLookup(conversion_key)) {
    std::vector<UserDictionaryLookupResult> entries;
    converter.LookupUserDictionaryPrefixEntries(suffix, &entries);
    for (const UserDictionaryLookupResult& entry : entries) {
      const absl::string_view entry_key(entry.key);
      const absl::string_view entry_value(entry.value);
      if (!IsSafeDirectUserDictionaryProtectedEntry(entry_key, entry_value)) {
        continue;
      }
      if (!absl::StartsWith(suffix, entry_key) ||
          !absl::StrContains(mozc_value, entry_value)) {
        continue;
      }

      AddOrUpdateProtectedSpan(
          entry_key, entry_value, ProtectedConversionSpan::Tier::kUserPreferred,
          false, mozc_value, protected_spans);
    }
  }
}

bool CandidateWordHasAttribute(const commands::CandidateWord& candidate,
                               commands::CandidateAttribute attribute) {
  for (int i = 0; i < candidate.attributes_size(); ++i) {
    if (candidate.attributes(i) == attribute) {
      return true;
    }
  }
  return false;
}

std::vector<ProtectedConversionSpan> BuildZenzProtectedConversionSpans(
    const engine::EngineConverterInterface& converter,
    const commands::Output& output, absl::string_view conversion_key,
    absl::string_view mozc_value) {
  std::vector<ProtectedConversionSpan> protected_spans;

  if (output.has_all_candidate_words()) {
    const commands::CandidateList& candidate_list = output.all_candidate_words();
    const uint32_t focused_index = candidate_list.has_focused_index()
                                     ? candidate_list.focused_index()
                                     : 0;

    for (int i = 0; i < candidate_list.candidates_size(); ++i) {
      const commands::CandidateWord& candidate = candidate_list.candidates(i);
      const bool focused_candidate = candidate.index() == focused_index;

      if (!CandidateWordHasAttribute(candidate, commands::USER_DICTIONARY)) {
        continue;
      }

      if (candidate.value().empty()) {
        continue;
      }

      std::string candidate_key =
          candidate.has_key() ? candidate.key() : std::string();
      if (candidate_key.empty() && output.has_preedit()) {
        candidate_key =
            FindPreeditKeyForValue(output.preedit(), candidate.value());
      }

      std::string preedit_protected_key;
      if (output.has_preedit()) {
        preedit_protected_key = FindPreeditKeyForProtectedSurface(
            output.preedit(), candidate.value(), candidate_key);
      }

      // For non-focused candidates, protect only when the same surface is
      // actually visible in the current preedit.  This recovers embedded
      // user-dictionary words such as "じしょごの" -> "辞書語の" without pinning
      // unrelated user-dictionary candidates that merely exist in the candidate
      // list.
      if (!focused_candidate && preedit_protected_key.empty()) {
        continue;
      }

      const std::string protected_key = !preedit_protected_key.empty()
                                            ? preedit_protected_key
                                            : candidate_key;

      const std::vector<std::string> identity_surfaces =
          ExtractAsciiIdentitySurfaces(candidate.value());
      if (!identity_surfaces.empty()) {
        for (const std::string& surface : identity_surfaces) {
          const std::string surface_key = InferProtectedKeyForEmbeddedSurface(
              protected_key, candidate.value(), surface);
          AddOrUpdateProtectedSpan(
              surface_key, surface, ProtectedConversionSpan::Tier::kIdentityCritical,
              !surface_key.empty(), mozc_value, &protected_spans);
        }
        continue;
      }

      AddOrUpdateProtectedSpan(
          protected_key, candidate.value(),
          ProtectedConversionSpan::Tier::kUserPreferred, false, mozc_value,
          &protected_spans);
    }
  }

  if (output.has_preedit()) {
    AddPreeditIdentityProtectedSpans(output.preedit(), mozc_value,
                                     &protected_spans);
  }

  AddDirectUserDictionaryEntryProtectedSpans(converter, conversion_key,
                                             mozc_value,
                                             &protected_spans);

  return protected_spans;
}



uint32_t GetZenzConversionTimeoutMsec(const config::Config& config) {
  const uint32_t configured_timeout =
      config.has_zenz_conversion_timeout_msec()
          ? config.zenz_conversion_timeout_msec()
          : kDefaultZenzConversionTimeoutMsec;
  return std::max<uint32_t>(
      1, std::min(configured_timeout, kMaxZenzConversionTimeoutMsec));
}

uint32_t GetZenzConversionLeftContextLength(
    const config::Config& config) {
  return config.use_zenz_context() ? config.zenz_context_left_length() : 0;
}

uint32_t GetZenzConversionRightContextLength(
    const config::Config& config) {
  if (!config.use_zenz_context() || !config.use_zenz_right_context()) {
    return 0;
  }

  const uint32_t length =
      config.zenz_context_right_length();
  return std::min<uint32_t>(length,
                            kMaxZenzConversionRightContextLength);
}

bool UseZenzFeedbackLearning(const config::Config& config) {
  return config.use_zenz_feedback_learning();
}

ZenzFeedbackAutoBlockPolicy GetZenzFeedbackAutoBlockPolicy(
    const config::Config& config) {
  ZenzFeedbackAutoBlockPolicy policy;
  policy.enabled = config.use_zenz_auto_block_rejected_correction();
  policy.reject_threshold =
      static_cast<int>(config.zenz_auto_block_reject_threshold());
  return policy;
}

bool IsExplicitZenzHardRejectReason(absl::string_view reason) {
  return reason == "hard_reject" ||
         reason == "user_hard_reject" ||
         reason == "manual_hard_reject" ||
         reason == "explicit_hard_reject";
}

bool StartsWithString(absl::string_view text, absl::string_view prefix) {
  return text.size() >= prefix.size() &&
         text.substr(0, prefix.size()) == prefix;
}

constexpr size_t kMaxZenzConversionKeyChars = 64;
constexpr size_t kMaxZenzConversionValueChars = 128;

struct ZenzTextPrivacyDecision {
  bool allow = false;
  const char* reason = "unspecified";
};

bool IsJapaneseScriptSignal(char32_t c) {
  // Hiragana
  if (0x3040 <= c && c <= 0x309F) {
    return true;
  }

  // Katakana
  if (0x30A0 <= c && c <= 0x30FF) {
    return true;
  }

  // Halfwidth Katakana
  if (0xFF66 <= c && c <= 0xFF9F) {
    return true;
  }

  // CJK Unified Ideographs
  if (0x4E00 <= c && c <= 0x9FFF) {
    return true;
  }

  // CJK Unified Ideographs Extension A
  if (0x3400 <= c && c <= 0x4DBF) {
    return true;
  }

  // CJK Compatibility Ideographs
  if (0xF900 <= c && c <= 0xFAFF) {
    return true;
  }

  // CJK extensions outside BMP.
  if ((0x20000 <= c && c <= 0x2A6DF) ||
      (0x2A700 <= c && c <= 0x2B73F) ||
      (0x2B740 <= c && c <= 0x2B81F) ||
      (0x2B820 <= c && c <= 0x2CEAF) ||
      (0x30000 <= c && c <= 0x3134F)) {
    return true;
  }

  return false;
}

bool ContainsJapaneseScriptSignal(
    absl::string_view text) {
  for (ConstChar32Iterator iter(text);
       !iter.Done();
       iter.Next()) {
    if (IsJapaneseScriptSignal(
            iter.Get())) {
      return true;
    }
  }

  return false;
}
ZenzTextPrivacyDecision EvaluateZenzConversionKeyPrivacy(
    absl::string_view key) {
  if (key.empty()) {
    return {false, "empty_key"};
  }

  if (!Util::IsValidUtf8(key)) {
    return {false, "invalid_utf8"};
  }

  for (const unsigned char c : key) {
    if (c < 0x20 ||
        c == 0x7f) {
      return {false, "control_char"};
    }
  }

  if (Util::CharsLen(key) >
      kMaxZenzConversionKeyChars) {
    return {false, "key_too_long"};
  }

  // Preserve the existing live-key requirement exactly.
  if (!ContainsJapaneseScriptSignal(
          key)) {
    return {false, "no_japanese_signal"};
  }

  const ZenzTextPrivacyAnalysis privacy =
      ZenzTextPrivacyAnalyzer().Analyze(
          key,
          ZenzTextPrivacyPolicy::kLiveText);

  if (privacy.sensitive()) {
    return {
        false,
        privacy.reason(),
    };
  }

  return {true, "allow"};
}
ZenzTextPrivacyDecision EvaluateZenzConversionValuePrivacy(
    absl::string_view value) {
  if (value.empty()) {
    return {false, "empty_value"};
  }

  if (!Util::IsValidUtf8(value)) {
    return {false, "invalid_utf8"};
  }

  for (const unsigned char c : value) {
    if (c < 0x20 ||
        c == 0x7f) {
      return {false, "control_char"};
    }
  }

  if (Util::CharsLen(value) >
      kMaxZenzConversionValueChars) {
    return {false, "value_too_long"};
  }

  // Value intentionally does not require a Japanese-script signal.
  // Example: key=ぎっとはぶ, value=GitHub remains valid.
  const ZenzTextPrivacyAnalysis privacy =
      ZenzTextPrivacyAnalyzer().Analyze(
          value,
          ZenzTextPrivacyPolicy::kLiveText);

  if (privacy.sensitive()) {
    return {
        false,
        privacy.reason(),
    };
  }

  return {true, "allow"};
}
void AddPreeditSegment(absl::string_view key,
                       absl::string_view value,
                       commands::Preedit::Segment::Annotation annotation,
                       commands::Preedit* preedit) {
  commands::Preedit::Segment* segment = preedit->add_segment();
  segment->set_annotation(annotation);
  segment->set_key(std::string(key));
  segment->set_value(std::string(value));
  segment->set_value_length(Util::CharsLen(value));
}
bool IsPendingZenzFeedbackDiscardKey(const commands::KeyEvent& key) {
  if (!key.has_special_key()) {
    return false;
  }

  switch (key.special_key()) {
    case commands::KeyEvent::BACKSPACE:
    case commands::KeyEvent::ESCAPE:
      return true;
    default:
      return false;
  }
}

bool IsPendingDirectCommitLearningDiscardKey(
    const commands::KeyEvent& key) {
  if (!key.has_special_key()) {
    return false;
  }

  switch (key.special_key()) {
    case commands::KeyEvent::BACKSPACE:
    case commands::KeyEvent::ESCAPE:
      return true;
    default:
      return false;
  }
}


// Set input mode if the current input mode is not the given mode.
void SwitchInputMode(const transliteration::TransliterationType mode,
                     composer::Composer* composer) {
  if (composer->GetInputMode() != mode) {
    composer->SetInputMode(mode);
  }
  composer->SetNewInput();
}

// Set input mode to the |composer| if the input mode of |composer| is not
// the given |mode|.
void ApplyCompositionMode(const commands::CompositionMode mode,
                          composer::Composer* composer) {
  switch (mode) {
    case commands::HIRAGANA:
      SwitchInputMode(transliteration::HIRAGANA, composer);
      break;
    case commands::FULL_KATAKANA:
      SwitchInputMode(transliteration::FULL_KATAKANA, composer);
      break;
    case commands::HALF_KATAKANA:
      SwitchInputMode(transliteration::HALF_KATAKANA, composer);
      break;
    case commands::FULL_ASCII:
      SwitchInputMode(transliteration::FULL_ASCII, composer);
      break;
    case commands::HALF_ASCII:
      SwitchInputMode(transliteration::HALF_ASCII, composer);
      break;
    default:
      LOG(DFATAL) << "ime on with invalid mode";
  }
}

// Return true if the specified key event consists of any modifier key only.
bool IsPureModifierKeyEvent(const commands::KeyEvent& key) {
  if (key.has_key_code()) {
    return false;
  }
  if (key.has_special_key()) {
    return false;
  }
  if (key.modifier_keys_size() == 0) {
    return false;
  }
  return true;
}

bool IsPureSpaceKey(const commands::KeyEvent& key) {
  if (key.has_key_code()) {
    return false;
  }
  if (key.modifier_keys_size() > 0) {
    return false;
  }
  if (!key.has_special_key()) {
    return false;
  }
  if (key.special_key() != commands::KeyEvent::SPACE) {
    return false;
  }
  return true;
}

// Set session state to the given state and also update related status.
void SetSessionState(const ImeContext::State state, ImeContext* context) {
  const ImeContext::State prev_state = context->state();
  context->set_state(state);
  switch (state) {
    case ImeContext::DIRECT:
    case ImeContext::PRECOMPOSITION:
      context->mutable_composer()->Reset();
      break;
    case ImeContext::CONVERSION:
      context->mutable_composer()->ResetInputMode();
      break;
    case ImeContext::COMPOSITION:
      if (prev_state == ImeContext::PRECOMPOSITION) {
        // Notify the start of composition to the converter so that internal
        // state can be refreshed by the client context (especially by
        // preceding text).
        context->mutable_converter()->OnStartComposition(
            context->client_context());
      }
      break;
    default:
      // Do nothing.
      break;
  }
}

void SetStateToPredompositionAndCancel(ImeContext* context) {
  SetSessionState(ImeContext::PRECOMPOSITION, context);
  // mutable_converter's internal state should be updated by calling Cancel().
  // Internal state contains:
  // - candidate list
  // - result text to commit
  if (!context->mutable_converter()->CheckState(
          EngineConverterInterface::COMPOSITION)) {
    context->mutable_converter()->Cancel();
  }
}

commands::CompositionMode ToCompositionMode(
    mozc::transliteration::TransliterationType type) {
  commands::CompositionMode mode = commands::HIRAGANA;
  switch (type) {
    case transliteration::HIRAGANA:
      mode = commands::HIRAGANA;
      break;
    case transliteration::FULL_KATAKANA:
      mode = commands::FULL_KATAKANA;
      break;
    case transliteration::HALF_KATAKANA:
      mode = commands::HALF_KATAKANA;
      break;
    case transliteration::FULL_ASCII:
      mode = commands::FULL_ASCII;
      break;
    case transliteration::HALF_ASCII:
      mode = commands::HALF_ASCII;
      break;
    default:
      LOG(ERROR) << "Unknown input mode: " << type;
      // use HIRAGANA as a default.
  }
  return mode;
}

ImeContext::State GetEffectiveStateForTestSendKey(const commands::KeyEvent& key,
                                                  ImeContext::State state) {
  if (!key.has_activated()) {
    return state;
  }
  if (state == ImeContext::DIRECT && key.activated()) {
    // Indirect IME On found.
    return ImeContext::PRECOMPOSITION;
  }
  if (state != ImeContext::DIRECT && !key.activated()) {
    // Indirect IME Off found.
    return ImeContext::DIRECT;
  }
  return state;
}

void MergeCommandResult(const commands::Result& step_result,
                        commands::Result* accumulated_result) {
  if (!accumulated_result->has_key() && !accumulated_result->has_value()) {
    *accumulated_result = step_result;
    return;
  }

  if (step_result.has_key()) {
    accumulated_result->set_key(
        absl::StrCat(accumulated_result->key(), step_result.key()));
  }
  if (step_result.has_value()) {
    accumulated_result->set_value(
        absl::StrCat(accumulated_result->value(), step_result.value()));
  }
}

void AccumulateCommandOutput(const commands::Output& step_output,
                             bool* consumed,
                             bool* has_accumulated_result,
                             commands::Result* accumulated_result,
                             commands::Output* final_output) {
  *consumed = *consumed || step_output.consumed();
  *final_output = step_output;

  if (step_output.has_result()) {
    MergeCommandResult(step_output.result(), accumulated_result);
    *has_accumulated_result = true;
  }

  final_output->set_consumed(*consumed);
  if (*has_accumulated_result) {
    *final_output->mutable_result() = *accumulated_result;
  }
}

}  // namespace

Session::Session(const EngineInterface& engine)
    : context_(CreateContext(engine)) {}

std::unique_ptr<ImeContext> Session::CreateContext(
    const EngineInterface& engine) const {
  auto context = std::make_unique<ImeContext>(engine.CreateEngineConverter());
  context->set_create_time(Clock::GetAbslTime());

#ifdef _WIN32
  // On Windows session is started with direct mode.
  // FIXME(toshiyuki): Ditto for Mac after verifying on Mac.
  context->set_state(ImeContext::DIRECT);
#else   // _WIN32
  context->set_state(ImeContext::PRECOMPOSITION);
#endif  // _WIN32

  // TODO(team): Remove #if based behavior change for cascading window.
  // Tests for session layer (session_handler_scenario_test, etc) can be
  // unstable.
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__linux__) || \
    defined(__wasm__)
  context->mutable_converter()->set_use_cascading_window(false);
#endif  // TARGET_OS_IPHONE || __linux__ || __wasm__

  return context;
}

void Session::PushUndoContext() {
  UndoEntry entry;
  entry.context = std::make_unique<ImeContext>(*context_);
  undo_contexts_.push_back(std::move(entry));

  // If the stack size exceeds the limitation, purge the oldest entries.
  while (undo_contexts_.size() > kMultipleUndoMaxSize) {
    undo_contexts_.pop_front();
  }
  DCHECK_LE(undo_contexts_.size(), kMultipleUndoMaxSize);
}

void Session::PushDirectCommitUndoContext() {
  PushUndoContext();
  DCHECK(!undo_contexts_.empty());
  undo_contexts_.back().revert_converter_on_undo = false;
}

void Session::PopUndoContext() {
  if (!HasUndoContext()) {
    return;
  }

  UndoEntry entry = std::move(undo_contexts_.back());
  undo_contexts_.pop_back();
  context_ = std::move(entry.context);

  // Invalidate every callback issued before Undo.
  ClearZenzConversionState();
}

bool Session::ShouldRevertConverterOnUndo() const {
  return HasUndoContext() &&
         undo_contexts_.back().revert_converter_on_undo;
}

void Session::ClearUndoContext() { undo_contexts_.clear(); }

bool Session::HasUndoContext() const { return !undo_contexts_.empty(); }

bool Session::IsCancelKeyForCompositionOrConversion(
    const commands::KeyEvent& key) const {
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();

  keymap::CompositionState::Commands composition_command;
  if (keymap->GetCommandComposition(key, &composition_command) &&
      composition_command == keymap::CompositionState::CANCEL) {
    return true;
  }

  keymap::ConversionState::Commands conversion_command;
  if (keymap->GetCommandConversion(key, &conversion_command) &&
      conversion_command == keymap::ConversionState::CANCEL) {
    return true;
  }

  return false;
}

void Session::MaybeSetUndoStatus(commands::Command* command) const {
  if (HasUndoContext()) {
    command->mutable_output()->mutable_status()->set_undo_available(true);
  }
}

void Session::EnsureIMEIsOn() {
  if (context_->state() == ImeContext::DIRECT) {
    SetSessionState(ImeContext::PRECOMPOSITION, context_.get());
  }
}

bool Session::SendCommand(commands::Command* command) {
  UpdateTime();
  UpdatePreferences(command);
  if (!command->input().has_command()) {
    return false;
  }
  TransformInput(command->mutable_input());

  const commands::SessionCommand& session_command = command->input().command();
  HandlePendingDirectCommitLearningForSessionCommand(session_command.type());
  HandlePendingZenzFeedbackForSessionCommand(session_command.type());

  bool result = false;
  if (session_command.type() ==
      commands::SessionCommand::SWITCH_COMPOSITION_MODE) {
    if (!session_command.has_composition_mode()) {
      return false;
    }
    switch (session_command.composition_mode()) {
      case commands::DIRECT:
        // TODO(komatsu): Implement here.
        break;
      case commands::HIRAGANA:
        result = CompositionModeHiragana(command);
        break;
      case commands::FULL_KATAKANA:
        result = CompositionModeFullKatakana(command);
        break;
      case commands::HALF_ASCII:
        result = CompositionModeHalfASCII(command);
        break;
      case commands::FULL_ASCII:
        result = CompositionModeFullASCII(command);
        break;
      case commands::HALF_KATAKANA:
        result = CompositionModeHalfKatakana(command);
        break;
      default:
        LOG(ERROR) << "Unknown mode: " << session_command.composition_mode();
        break;
    }
    MaybeSetUndoStatus(command);
    return result;
  }

  DCHECK_EQ(false, result);
  switch (command->input().command().type()) {
    case commands::SessionCommand::NONE:
      result = DoNothing(command);
      break;
    case commands::SessionCommand::REVERT:
      result = Revert(command);
      break;
    case commands::SessionCommand::SUBMIT:
      result = Commit(command);
      break;
    case commands::SessionCommand::SELECT_CANDIDATE:
      result = SelectCandidate(command);
      break;
    case commands::SessionCommand::SUBMIT_CANDIDATE:
      result = CommitCandidate(command);
      break;
    case commands::SessionCommand::HIGHLIGHT_CANDIDATE:
      result = HighlightCandidate(command);
      break;
    case commands::SessionCommand::GET_STATUS:
      result = GetStatus(command);
      break;
    case commands::SessionCommand::CONVERT_REVERSE:
      result = ConvertReverse(command);
      break;
    case commands::SessionCommand::UNDO:
      result = Undo(command);
      break;
    case commands::SessionCommand::RESET_CONTEXT:
      result = ResetContext(command);
      break;
    case commands::SessionCommand::MOVE_CURSOR:
      result = MoveCursorTo(command);
      break;
    case commands::SessionCommand::SWITCH_INPUT_FIELD_TYPE:
      result = SwitchInputFieldType(command);
      break;
    case commands::SessionCommand::UNDO_OR_REWIND:
      result = UndoOrRewind(command);
      break;
    case commands::SessionCommand::COMMIT_RAW_TEXT:
      result = CommitRawText(command);
      break;
    case commands::SessionCommand::CONVERT_PREV_PAGE:
      result = ConvertPrevPage(command);
      break;
    case commands::SessionCommand::CONVERT_NEXT_PAGE:
      result = ConvertNextPage(command);
      break;
    case commands::SessionCommand::TURN_ON_IME:
      result = MakeSureIMEOn(command);
      break;
    case commands::SessionCommand::TURN_OFF_IME:
      result = MakeSureIMEOff(command);
      break;
    case commands::SessionCommand::DELETE_CANDIDATE_FROM_HISTORY:
      result = DeleteCandidateFromHistory(command);
      break;
    case commands::SessionCommand::STOP_KEY_TOGGLING:
      result = StopKeyToggling(command);
      break;
    case commands::SessionCommand::UPDATE_COMPOSITION:
      result = UpdateComposition(command);
      break;

    case commands::SessionCommand::APPLY_ZENZ_CONVERSION:
      result = ApplyZenzConversion(command);
      break;

    case commands::SessionCommand::APPLY_ZENZ_SUGGESTION:
      result = ApplyZenzSuggestion(command);
      break;

    case commands::SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE:
      // This command is a client-side callback command.  It should normally be
      // handled by the TSF client from Output::Callback.  If it reaches the
      // server through SendCommand unexpectedly, consume it without changing the
      // current composition.
      result = DoNothing(command);
      break;

    case commands::SessionCommand::REQUEST_NWP: {
      ConversionPreferences conversion_preferences =
          context_->converter().conversion_preferences();
      conversion_preferences.request_suggestion =
          command->input().request_suggestion();
      // Resets converter's state (e.g. previous segments).
      // NWP will be generated from surrounding text given by the client.
      context_->mutable_converter()->Reset();
      result = context_->mutable_converter()->SuggestWithPreferences(
          context_->composer(), command->input().context(),
          conversion_preferences);
      if (result) {
        Output(command);
      }
      break;
    }
    default:
      LOG(WARNING) << "Unknown command" << *command;
      result = DoNothing(command);
      break;
  }
  if (context_->state() != ImeContext::CONVERSION) {
    ClearZenzConversionState();
  }

  MaybeSetUndoStatus(command);
  return result;
}

bool Session::TestSendKey(commands::Command* command) {
  UpdateTime();
  UpdatePreferences(command);
  TransformInput(command->mutable_input());

  if (context_->state() == ImeContext::NONE) {
    // This must be an error.
    LOG(ERROR) << "Invalid state: NONE";
    return false;
  }

  const commands::KeyEvent& key = command->input().key();

  // To support indirect IME on/off by using KeyEvent::activated, use effective
  // state instead of directly using context_->state().
  const ImeContext::State state =
      GetEffectiveStateForTestSendKey(key, context_->state());

  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();

  // Direct input
  if (state == ImeContext::DIRECT) {
    keymap::DirectInputState::Commands key_command;
    if (!keymap->GetCommandDirect(key, &key_command) ||
        key_command == keymap::DirectInputState::NONE) {
      return EchoBack(command);
    }
    return DoNothing(command);
  }

  // Precomposition
  if (state == ImeContext::PRECOMPOSITION) {
    keymap::PrecompositionState::Commands key_command;
    const bool is_suggestion =
        context_->converter().CheckState(EngineConverterInterface::SUGGESTION);
    const bool result =
        is_suggestion ? keymap->GetCommandZeroQuerySuggestion(key, &key_command)
                      : keymap->GetCommandPrecomposition(key, &key_command);
    if (!result || key_command == keymap::PrecompositionState::NONE) {
      if (HasUndoContext() && IsCancelKeyForCompositionOrConversion(key)) {
        return Revert(command);
      }

      if (pending_direct_commit_learning_.pending &&
          IsCancelKeyForCompositionOrConversion(key)) {
        // A direct-commit punctuation/symbol may have already sent text to the
        // application without creating Mozc's undo context.  In that case a
        // cancel-like key such as Ctrl+Z should still cancel Mozkey's pending
        // learning, while the key itself must be echoed back so that the
        // application can decide how to undo the visible text.
        DiscardPendingDirectCommitLearning(
            "precomposition_cancel_key_after_direct_commit_learning");
        return EchoBackAndClearUndoContext(command);
      }

      // Clear undo context just in case. b/5529702.
      // Note that the undo context will not be cleared in
      // EchoBackAndClearUndoContext if the key event consists of modifier keys
      // only.
      return EchoBackAndClearUndoContext(command);
    }
    // If the input_style is DIRECT_INPUT, KeyEvent is not consumed
    // and done echo back.  It works only when key_string is equal to
    // key_code.  We should fix this limitation when the as_is flag is
    // used for rather than numpad characters.
    if (key_command == keymap::PrecompositionState::INSERT_CHARACTER &&
        key.input_style() == commands::KeyEvent::DIRECT_INPUT) {
      return EchoBack(command);
    }

    // TODO(komatsu): This is a hack to work around the problem with
    // the inconsistency between TestSendKey and SendKey.
    switch (key_command) {
      case keymap::PrecompositionState::INSERT_SPACE:
      case keymap::PrecompositionState::RECONVERT_SELECTION_OR_INSERT_SPACE:
        if (!IsFullWidthInsertSpace(command->input()) && IsPureSpaceKey(key)) {
          return EchoBackAndClearUndoContext(command);
        }
        return DoNothing(command);
      case keymap::PrecompositionState::INSERT_ALTERNATE_SPACE:
        if (IsFullWidthInsertSpace(command->input()) && IsPureSpaceKey(key)) {
          return EchoBackAndClearUndoContext(command);
        }
        return DoNothing(command);
      case keymap::PrecompositionState::INSERT_HALF_SPACE:
        if (IsPureSpaceKey(key)) {
          return EchoBackAndClearUndoContext(command);
        }
        return DoNothing(command);
      case keymap::PrecompositionState::INSERT_FULL_SPACE:
        return DoNothing(command);
      default:
        // Do nothing.
        break;
    }

    if (key_command == keymap::PrecompositionState::REVERT) {
      return Revert(command);
    }

    // If undo context is empty, echoes back the key event so that it can be
    // handled by the application. b/5553298
    if (key_command == keymap::PrecompositionState::UNDO && !HasUndoContext()) {
      return EchoBack(command);
    }

    return DoNothing(command);
  }

  // Do nothing.
  return DoNothing(command);
}

bool Session::SendKey(commands::Command* command) {
  UpdateTime();
  UpdatePreferences(command);
  TransformInput(command->mutable_input());
  // To support indirect IME on/off by using KeyEvent::activated, use effective
  // state instead of directly using context_->state().
  HandleIndirectImeOnOff(command);

  bool result = false;
  switch (context_->state()) {
    case ImeContext::DIRECT:
      result = SendKeyDirectInputState(command);
      break;

    case ImeContext::PRECOMPOSITION:
      result = SendKeyPrecompositionState(command);
      break;

    case ImeContext::COMPOSITION:
      result = SendKeyCompositionState(command);
      break;

    case ImeContext::CONVERSION:
      result = SendKeyConversionState(command);
      break;

    case ImeContext::NONE:
      result = false;
      break;
  }

  if (context_->state() != ImeContext::CONVERSION) {
    ClearZenzConversionState();
  }

  MaybeSetUndoStatus(command);
  return result;
}

bool Session::UpdateCompositionInternal(commands::Command* command) {
  command->mutable_output()->set_consumed(true);

  context_->mutable_composer()->Reset();
  // Use the top entry for now.
  context_->mutable_composer()->SetCompositionsForHandwriting(
      command->input().command().composition_events());
  ClearUndoContext();
  SetSessionState(ImeContext::COMPOSITION, context_.get());

  if (Suggest(command->input())) {
    Output(command);
    return true;
  }

  OutputComposition(command);
  return true;
}

bool Session::UpdateComposition(commands::Command* command) {
  bool result = false;
  switch (context_->state()) {
    case ImeContext::DIRECT:
      result = EchoBackAndClearUndoContext(command);
      break;

    case ImeContext::PRECOMPOSITION:
      [[fallthrough]];
    case ImeContext::COMPOSITION:
      result = UpdateCompositionInternal(command);
      break;

    case ImeContext::CONVERSION:
      result = false;
      break;

    case ImeContext::NONE:
      result = false;
      break;
  }
  return result;
}

bool Session::ExecuteCommandSequence(
    const keymap::CommandSequence& command_sequence,
    commands::Command* command) {
  return ExecuteCommandSequenceWithInitialOutput(command_sequence, nullptr,
                                                 command);
}

bool Session::ExecuteCommandSequenceWithInitialOutput(
    const keymap::CommandSequence& command_sequence,
    const commands::Output* initial_output,
    commands::Command* command) {
  bool executed = false;
  bool consumed = false;
  bool has_accumulated_result = false;
  commands::Result accumulated_result;
  commands::Output final_output;

  if (initial_output != nullptr) {
    AccumulateCommandOutput(*initial_output,
                            &consumed,
                            &has_accumulated_result,
                            &accumulated_result,
                            &final_output);
    executed = true;
  }

  for (const std::string& command_name : command_sequence) {
    if (command_name.empty()) {
      continue;
    }

    command->mutable_output()->Clear();

    if (!ExecuteCommandName(command_name, command)) {
      if (!executed) {
        return DoNothing(command);
      }

      final_output.set_consumed(consumed);
      if (has_accumulated_result) {
        *final_output.mutable_result() = accumulated_result;
      }
      *command->mutable_output() = final_output;
      return true;
    }

    executed = true;

    const commands::Output step_output = command->output();
    AccumulateCommandOutput(step_output,
                            &consumed,
                            &has_accumulated_result,
                            &accumulated_result,
                            &final_output);
  }

  if (!executed) {
    return DoNothing(command);
  }

  final_output.set_consumed(consumed);
  if (has_accumulated_result) {
    *final_output.mutable_result() = accumulated_result;
  }
  *command->mutable_output() = final_output;
  return true;
}

bool Session::ExecuteCommandName(const std::string& command_name,
                                 commands::Command* command) {
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();

  switch (context_->state()) {
    case ImeContext::DIRECT: {
      keymap::DirectInputState::Commands key_command;
      if (!keymap->ResolveDirectCommandName(command_name, &key_command)) {
        return false;
      }
      return ExecuteDirectInputCommand(key_command, command);
    }

    case ImeContext::PRECOMPOSITION: {
      keymap::PrecompositionState::Commands key_command;
      if (!keymap->ResolvePrecompositionCommandName(command_name,
                                                    &key_command)) {
        return false;
      }
      return ExecutePrecompositionCommand(key_command, command);
    }

    case ImeContext::COMPOSITION: {
      keymap::CompositionState::Commands key_command;
      if (!keymap->ResolveCompositionCommandName(command_name, &key_command)) {
        return false;
      }
      return ExecuteCompositionCommand(key_command, command);
    }

    case ImeContext::CONVERSION: {
      keymap::ConversionState::Commands key_command;
      if (!keymap->ResolveConversionCommandName(command_name, &key_command)) {
        return false;
      }
      return ExecuteConversionCommand(key_command, command);
    }

    case ImeContext::NONE:
      return false;
  }

  return false;
}

bool Session::ExecuteDirectInputCommand(
    keymap::DirectInputState::Commands key_command,
    commands::Command* command) {
  switch (key_command) {
    case keymap::DirectInputState::IME_ON:
      return IMEOn(command);
    case keymap::DirectInputState::COMPOSITION_MODE_HIRAGANA:
      return CompositionModeHiragana(command);
    case keymap::DirectInputState::COMPOSITION_MODE_FULL_KATAKANA:
      return CompositionModeFullKatakana(command);
    case keymap::DirectInputState::COMPOSITION_MODE_HALF_KATAKANA:
      return CompositionModeHalfKatakana(command);
    case keymap::DirectInputState::COMPOSITION_MODE_FULL_ALPHANUMERIC:
      return CompositionModeFullASCII(command);
    case keymap::DirectInputState::COMPOSITION_MODE_HALF_ALPHANUMERIC:
      return CompositionModeHalfASCII(command);
    case keymap::DirectInputState::NONE:
      return EchoBackAndClearUndoContext(command);
    case keymap::DirectInputState::RECONVERT:
      return RequestConvertReverse(command);
  }

  return false;
}

bool Session::ExecutePrecompositionCommand(
    keymap::PrecompositionState::Commands key_command,
    commands::Command* command) {
  switch (key_command) {
    case keymap::PrecompositionState::INSERT_CHARACTER:
      return InsertCharacter(command);
    case keymap::PrecompositionState::INSERT_SPACE:
      return InsertSpace(command);
    case keymap::PrecompositionState::INSERT_ALTERNATE_SPACE:
      return InsertSpaceToggled(command);
    case keymap::PrecompositionState::INSERT_HALF_SPACE:
      return InsertSpaceHalfWidth(command);
    case keymap::PrecompositionState::INSERT_FULL_SPACE:
      return InsertSpaceFullWidth(command);
    case keymap::PrecompositionState::TOGGLE_ALPHANUMERIC_MODE:
      return ToggleAlphanumericMode(command);
    case keymap::PrecompositionState::REVERT:
      return Revert(command);
    case keymap::PrecompositionState::UNDO:
      return RequestUndo(command);
    case keymap::PrecompositionState::IME_OFF:
      return IMEOff(command);
    case keymap::PrecompositionState::IME_ON:
      return DoNothing(command);

    case keymap::PrecompositionState::COMPOSITION_MODE_HIRAGANA:
      return CompositionModeHiragana(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_FULL_KATAKANA:
      return CompositionModeFullKatakana(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_HALF_KATAKANA:
      return CompositionModeHalfKatakana(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_FULL_ALPHANUMERIC:
      return CompositionModeFullASCII(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_HALF_ALPHANUMERIC:
      return CompositionModeHalfASCII(command);
    case keymap::PrecompositionState::COMPOSITION_MODE_SWITCH_KANA_TYPE:
      return CompositionModeSwitchKanaType(command);

    case keymap::PrecompositionState::LAUNCH_CONFIG_DIALOG:
      return LaunchConfigDialog(command);
    case keymap::PrecompositionState::LAUNCH_DICTIONARY_TOOL:
      return LaunchDictionaryTool(command);
    case keymap::PrecompositionState::LAUNCH_WORD_REGISTER_DIALOG:
      return LaunchWordRegisterDialog(command);

    case keymap::PrecompositionState::CANCEL:
      return EditCancel(command);
    case keymap::PrecompositionState::CANCEL_AND_IME_OFF:
      return EditCancelAndIMEOff(command);
    case keymap::PrecompositionState::COMMIT_FIRST_SUGGESTION:
      return CommitFirstSuggestion(command);
    case keymap::PrecompositionState::PREDICT_AND_CONVERT:
      return PredictAndConvert(command);

    case keymap::PrecompositionState::NONE:
      if (HasUndoContext() &&
          IsCancelKeyForCompositionOrConversion(command->input().key())) {
        return Revert(command);
      }
      return EchoBackAndClearUndoContext(command);
    case keymap::PrecompositionState::RECONVERT:
      return RequestConvertReverse(command);
    case keymap::PrecompositionState::RECONVERT_SELECTION_OR_INSERT_SPACE:
      return RequestReconvertSelectionOrInsertSpace(command);

    case keymap::PrecompositionState::IME_ACTION:
      return ImeAction(command);
  }

  return false;
}

bool Session::ExecuteCompositionCommand(
    keymap::CompositionState::Commands key_command,
    commands::Command* command) {
  switch (key_command) {
    case keymap::CompositionState::INSERT_CHARACTER:
      return InsertCharacter(command);

    case keymap::CompositionState::COMMIT:
      return Commit(command);

    case keymap::CompositionState::COMMIT_FIRST_SUGGESTION:
      return CommitFirstSuggestion(command);

    case keymap::CompositionState::CONVERT:
      return Convert(command);

    case keymap::CompositionState::CONVERT_WITHOUT_HISTORY:
      return ConvertWithoutHistory(command);

    case keymap::CompositionState::PREDICT_AND_CONVERT:
      return PredictAndConvert(command);

    case keymap::CompositionState::DEL:
      return Delete(command);

    case keymap::CompositionState::BACKSPACE:
      return Backspace(command);

    case keymap::CompositionState::INSERT_SPACE:
      return InsertSpace(command);

    case keymap::CompositionState::INSERT_ALTERNATE_SPACE:
      return InsertSpaceToggled(command);

    case keymap::CompositionState::INSERT_HALF_SPACE:
      return InsertSpaceHalfWidth(command);

    case keymap::CompositionState::INSERT_FULL_SPACE:
      return InsertSpaceFullWidth(command);

    case keymap::CompositionState::MOVE_CURSOR_LEFT:
      return MoveCursorLeft(command);

    case keymap::CompositionState::MOVE_CURSOR_RIGHT:
      return MoveCursorRight(command);

    case keymap::CompositionState::MOVE_CURSOR_TO_BEGINNING:
      return MoveCursorToBeginning(command);

    case keymap::CompositionState::MOVE_MOVE_CURSOR_TO_END:
      return MoveCursorToEnd(command);

    case keymap::CompositionState::CANCEL:
      return EditCancel(command);

    case keymap::CompositionState::CANCEL_AND_IME_OFF:
      return EditCancelAndIMEOff(command);

    case keymap::CompositionState::UNDO:
      return RequestUndo(command);

    case keymap::CompositionState::IME_OFF:
      return IMEOff(command);

    case keymap::CompositionState::IME_ON:
      return DoNothing(command);

    case keymap::CompositionState::CONVERT_TO_HIRAGANA:
      return ConvertToHiragana(command);

    case keymap::CompositionState::CONVERT_TO_FULL_KATAKANA:
      return ConvertToFullKatakana(command);

    case keymap::CompositionState::CONVERT_TO_HALF_KATAKANA:
      return ConvertToHalfKatakana(command);

    case keymap::CompositionState::CONVERT_TO_HALF_WIDTH:
      return ConvertToHalfWidth(command);

    case keymap::CompositionState::CONVERT_TO_FULL_ALPHANUMERIC:
      return ConvertToFullASCII(command);

    case keymap::CompositionState::CONVERT_TO_HALF_ALPHANUMERIC:
      return ConvertToHalfASCII(command);

    case keymap::CompositionState::SWITCH_KANA_TYPE:
      return SwitchKanaType(command);

    case keymap::CompositionState::DISPLAY_AS_HIRAGANA:
      return DisplayAsHiragana(command);

    case keymap::CompositionState::DISPLAY_AS_FULL_KATAKANA:
      return DisplayAsFullKatakana(command);

    case keymap::CompositionState::DISPLAY_AS_HALF_KATAKANA:
      return DisplayAsHalfKatakana(command);

    case keymap::CompositionState::TRANSLATE_HALF_WIDTH:
      return TranslateHalfWidth(command);

    case keymap::CompositionState::TRANSLATE_FULL_ASCII:
      return TranslateFullASCII(command);

    case keymap::CompositionState::TRANSLATE_HALF_ASCII:
      return TranslateHalfASCII(command);

    case keymap::CompositionState::TOGGLE_ALPHANUMERIC_MODE:
      return ToggleAlphanumericMode(command);

    case keymap::CompositionState::COMPOSITION_MODE_HIRAGANA:
      return CompositionModeHiragana(command);

    case keymap::CompositionState::COMPOSITION_MODE_FULL_KATAKANA:
      return CompositionModeFullKatakana(command);

    case keymap::CompositionState::COMPOSITION_MODE_HALF_KATAKANA:
      return CompositionModeHalfKatakana(command);

    case keymap::CompositionState::COMPOSITION_MODE_FULL_ALPHANUMERIC:
      return CompositionModeFullASCII(command);

    case keymap::CompositionState::COMPOSITION_MODE_HALF_ALPHANUMERIC:
      return CompositionModeHalfASCII(command);

    case keymap::CompositionState::NONE:
      return DoNothing(command);
  }

  return false;
}

bool Session::ExecuteConversionCommand(
    keymap::ConversionState::Commands key_command,
    commands::Command* command) {
  switch (key_command) {
    case keymap::ConversionState::INSERT_CHARACTER:
      return InsertCharacter(command);

    case keymap::ConversionState::INSERT_SPACE:
      return InsertSpace(command);

    case keymap::ConversionState::INSERT_ALTERNATE_SPACE:
      return InsertSpaceToggled(command);

    case keymap::ConversionState::INSERT_HALF_SPACE:
      return InsertSpaceHalfWidth(command);

    case keymap::ConversionState::INSERT_FULL_SPACE:
      return InsertSpaceFullWidth(command);

    case keymap::ConversionState::COMMIT:
      return Commit(command);

    case keymap::ConversionState::COMMIT_SEGMENT:
      return CommitSegment(command);

    case keymap::ConversionState::CONVERT_NEXT:
      return ConvertNext(command);

    case keymap::ConversionState::CONVERT_PREV:
      return ConvertPrev(command);

    case keymap::ConversionState::CONVERT_NEXT_PAGE:
      return ConvertNextPage(command);

    case keymap::ConversionState::CONVERT_PREV_PAGE:
      return ConvertPrevPage(command);

    case keymap::ConversionState::PREDICT_AND_CONVERT:
      return PredictAndConvert(command);

    case keymap::ConversionState::SEGMENT_FOCUS_LEFT:
      return SegmentFocusLeft(command);

    case keymap::ConversionState::SEGMENT_FOCUS_RIGHT:
      return SegmentFocusRight(command);

    case keymap::ConversionState::SEGMENT_FOCUS_FIRST:
      return SegmentFocusLeftEdge(command);

    case keymap::ConversionState::SEGMENT_FOCUS_LAST:
      return SegmentFocusLast(command);

    case keymap::ConversionState::SEGMENT_WIDTH_EXPAND:
      return SegmentWidthExpand(command);

    case keymap::ConversionState::SEGMENT_WIDTH_SHRINK:
      return SegmentWidthShrink(command);

    case keymap::ConversionState::CANCEL:
      return ConvertCancel(command);

    case keymap::ConversionState::CANCEL_AND_IME_OFF:
      return EditCancelAndIMEOff(command);

    case keymap::ConversionState::UNDO:
      return RequestUndo(command);

    case keymap::ConversionState::IME_OFF:
      return IMEOff(command);

    case keymap::ConversionState::IME_ON:
      return DoNothing(command);

    case keymap::ConversionState::CONVERT_TO_HIRAGANA:
      return ConvertToHiragana(command);

    case keymap::ConversionState::CONVERT_TO_FULL_KATAKANA:
      return ConvertToFullKatakana(command);

    case keymap::ConversionState::CONVERT_TO_HALF_KATAKANA:
      return ConvertToHalfKatakana(command);

    case keymap::ConversionState::CONVERT_TO_HALF_WIDTH:
      return ConvertToHalfWidth(command);

    case keymap::ConversionState::CONVERT_TO_FULL_ALPHANUMERIC:
      return ConvertToFullASCII(command);

    case keymap::ConversionState::CONVERT_TO_HALF_ALPHANUMERIC:
      return ConvertToHalfASCII(command);

    case keymap::ConversionState::SWITCH_KANA_TYPE:
      return SwitchKanaType(command);

    case keymap::ConversionState::DISPLAY_AS_HIRAGANA:
      return DisplayAsHiragana(command);

    case keymap::ConversionState::DISPLAY_AS_FULL_KATAKANA:
      return DisplayAsFullKatakana(command);

    case keymap::ConversionState::DISPLAY_AS_HALF_KATAKANA:
      return DisplayAsHalfKatakana(command);

    case keymap::ConversionState::TRANSLATE_HALF_WIDTH:
      return TranslateHalfWidth(command);

    case keymap::ConversionState::TRANSLATE_FULL_ASCII:
      return TranslateFullASCII(command);

    case keymap::ConversionState::TRANSLATE_HALF_ASCII:
      return TranslateHalfASCII(command);

    case keymap::ConversionState::TOGGLE_ALPHANUMERIC_MODE:
      return ToggleAlphanumericMode(command);

    case keymap::ConversionState::COMPOSITION_MODE_HIRAGANA:
      return CompositionModeHiragana(command);

    case keymap::ConversionState::COMPOSITION_MODE_FULL_KATAKANA:
      return CompositionModeFullKatakana(command);

    case keymap::ConversionState::COMPOSITION_MODE_HALF_KATAKANA:
      return CompositionModeHalfKatakana(command);

    case keymap::ConversionState::COMPOSITION_MODE_FULL_ALPHANUMERIC:
      return CompositionModeFullASCII(command);

    case keymap::ConversionState::COMPOSITION_MODE_HALF_ALPHANUMERIC:
      return CompositionModeHalfASCII(command);

    case keymap::ConversionState::REPORT_BUG:
      return ReportBug(command);

    case keymap::ConversionState::DELETE_SELECTED_CANDIDATE:
      return DeleteCandidateFromHistory(command);

    case keymap::ConversionState::NONE:
      return DoNothing(command);
  }

  return false;
}

bool Session::SendKeyDirectInputState(commands::Command* command) {
  keymap::CommandSequence command_sequence;
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();
  if (!keymap->GetCommandSequenceDirect(command->input().key(),
                                        &command_sequence)) {
    return EchoBackAndClearUndoContext(command);
  }

  return ExecuteCommandSequence(command_sequence, command);
}

bool Session::SendKeyPrecompositionState(commands::Command* command) {
  keymap::CommandSequence command_sequence;
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();
  const bool result =
      context_->converter().CheckState(EngineConverterInterface::SUGGESTION)
          ? keymap->GetCommandSequenceZeroQuerySuggestion(
                command->input().key(), &command_sequence)
          : keymap->GetCommandSequencePrecomposition(command->input().key(),
                                                     &command_sequence);

  if (!result) {
    if (HasUndoContext() &&
        IsCancelKeyForCompositionOrConversion(command->input().key())) {
      return Revert(command);
    }

    if (pending_direct_commit_learning_.pending &&
        IsCancelKeyForCompositionOrConversion(command->input().key())) {
      // A direct-commit punctuation/symbol may have already sent text to the
      // application without creating Mozc's undo context.  In that case a
      // cancel-like key such as Ctrl+Z should still cancel Mozkey's pending
      // learning, while the key itself must be echoed back so that the
      // application can decide how to undo the visible text.
      DiscardPendingDirectCommitLearning(
          "precomposition_cancel_key_after_direct_commit_learning");
      return EchoBackAndClearUndoContext(command);
    }
    return EchoBackAndClearUndoContext(command);
  }

  // Update the client context (if any) for later use. Note that the client
  // context is updated only here. In other words, we will stop updating the
  // client context once a conversion starts (mainly for performance reasons).
  if (command->has_input() && command->input().has_context()) {
    *context_->mutable_client_context() = command->input().context();

#if defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)
    const commands::Context& client_context = command->input().context();
    if (client_context.has_preceding_text()) {
      MozcLeftContextDebugOutput(absl::StrCat(
          "[mozc-left-context] session preceding_text=[",
          client_context.preceding_text(), "]"));
    } else {
      MozcLeftContextDebugOutput(
          "[mozc-left-context] session context has no preceding_text");
    }
#endif  // defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)

  } else {
    context_->mutable_client_context()->Clear();

#if defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)
    MozcLeftContextDebugOutput("[mozc-left-context] session no context");
#endif  // defined(_WIN32) && defined(MOZC_LEFT_CONTEXT_DEBUG)
  }

  return ExecuteCommandSequence(command_sequence, command);
}

bool Session::SendKeyCompositionState(commands::Command* command) {
  keymap::CommandSequence command_sequence;
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();
  const bool result =
      context_->converter().CheckState(EngineConverterInterface::SUGGESTION)
          ? keymap->GetCommandSequenceSuggestion(command->input().key(),
                                                 &command_sequence)
          : keymap->GetCommandSequenceComposition(command->input().key(),
                                                  &command_sequence);

  if (!result) {
    return DoNothing(command);
  }

  return ExecuteCommandSequence(command_sequence, command);
}

bool Session::SendKeyConversionState(commands::Command* command) {
  keymap::CommandSequence command_sequence;
  const keymap::KeyMapManager* keymap = &context_->GetKeyMapManager();
  const bool result =
      context_->converter().CheckState(EngineConverterInterface::PREDICTION)
          ? keymap->GetCommandSequencePrediction(command->input().key(),
                                                 &command_sequence)
          : keymap->GetCommandSequenceConversion(command->input().key(),
                                                  &command_sequence);

  if (!result || command_sequence.empty()) {
    return DoNothing(command);
  }

  keymap::ConversionState::Commands key_command;
  if (!keymap->ResolveConversionCommandName(command_sequence.front(),
                                            &key_command)) {
    return DoNothing(command);
  }

  const commands::KeyEvent& input_key = command->input().key();
  if (pending_zenz_conversion_.pending) {
    // Any new user action supersedes the asynchronous result.  The ordinary
    // Mozc conversion remains active and supplies the fallback candidates.
    CancelPendingZenzConversion();
  }

  if (HasVisibleZenzConversion()) {
    if (key_command == keymap::ConversionState::COMMIT) {
      if (!CommitZenzConversionResult(command)) {
        return false;
      }
      if (command_sequence.size() == 1) {
        return true;
      }
      const commands::Output zenz_commit_output = command->output();
      keymap::CommandSequence remaining_sequence(
          command_sequence.begin() + 1, command_sequence.end());
      return ExecuteCommandSequenceWithInitialOutput(
          remaining_sequence, &zenz_commit_output, command);
    }

    if (key_command == keymap::ConversionState::CONVERT_NEXT &&
        IsPureSpaceKey(input_key)) {
      // Space exposes the original Mozc candidate list after rejecting the
      // temporary Zenz result.
      return RevertZenzConversionToMozc(command);
    }

    // Text input is handled by InsertCharacter(), which can commit the visible
    // Zenz value when punctuation is directly committed. Other conversion
    // commands return to the normal Mozc converter before they run.
    if (key_command != keymap::ConversionState::INSERT_CHARACTER) {
      SetPendingZenzFeedbackRejected("conversion_command_after_zenz");
      ClearZenzConversionState();
    }
  }

  return ExecuteCommandSequence(command_sequence, command);
}

void Session::UpdatePreferences(commands::Command* command) {
  DCHECK(command);
  const config::Config& config = command->input().config();
  if (command->input().has_capability()) {
    *context_->mutable_client_capability() = command->input().capability();
  }

  // Update config values modified temporarily.
  // TODO(team): Stop using config for temporary modification.
  if (config.has_selection_shortcut()) {
    context_->mutable_converter()->set_selection_shortcut(
        config.selection_shortcut());
  }

#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__linux__) || \
    defined(__wasm__)
  context_->mutable_converter()->set_use_cascading_window(false);
#else   // TARGET_OS_IPHONE || __linux__ || __wasm__
  if (config.has_use_cascading_window()) {
    context_->mutable_converter()->set_use_cascading_window(
        config.use_cascading_window());
  }
#endif  // TARGET_OS_IPHONE || __linux__ || __wasm__
}

bool Session::IMEOn(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  ClearUndoContext();

  SetSessionState(ImeContext::PRECOMPOSITION, context_.get());
  if (command->input().has_key() && command->input().key().has_mode()) {
    ApplyCompositionMode(command->input().key().mode(),
                         context_->mutable_composer());
  }
  OutputMode(command);
  return true;
}

bool Session::IMEOff(commands::Command* command) {
  ConfirmPendingDirectCommitLearning("ime_off_after_direct_commit_learning");

  command->mutable_output()->set_consumed(true);
  ClearUndoContext();

  Commit(command);
  ConfirmPendingZenzFeedback();

  // Reset the context.
  context_->mutable_converter()->Reset();

  SetSessionState(ImeContext::DIRECT, context_.get());
  OutputMode(command);
  return true;
}

bool Session::MakeSureIMEOn(mozc::commands::Command* command) {
  if (command->input().has_command() &&
      command->input().command().has_composition_mode() &&
      (command->input().command().composition_mode() == commands::DIRECT)) {
    // This is invalid and unsupported usage.
    return false;
  }

  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::DIRECT) {
    ClearUndoContext();
    SetSessionState(ImeContext::PRECOMPOSITION, context_.get());
  }
  if (command->input().has_command() &&
      command->input().command().has_composition_mode()) {
    ApplyCompositionMode(command->input().command().composition_mode(),
                         context_->mutable_composer());
  }
  OutputMode(command);
  return true;
}

bool Session::MakeSureIMEOff(mozc::commands::Command* command) {
  ConfirmPendingDirectCommitLearning(
      "make_sure_ime_off_after_direct_commit_learning");

  if (command->input().has_command() &&
      command->input().command().has_composition_mode() &&
      (command->input().command().composition_mode() == commands::DIRECT)) {
    // This is invalid and unsupported usage.
    return false;
  }

  command->mutable_output()->set_consumed(true);
  if (context_->state() != ImeContext::DIRECT) {
    ClearUndoContext();
    Commit(command);
    // Reset the context.
    context_->mutable_converter()->Reset();
    SetSessionState(ImeContext::DIRECT, context_.get());
  }
  ConfirmPendingZenzFeedback();
  if (command->input().has_command() &&
      command->input().command().has_composition_mode()) {
    ApplyCompositionMode(command->input().command().composition_mode(),
                         context_->mutable_composer());
  }
  OutputMode(command);
  return true;
}

bool Session::EchoBack(commands::Command* command) {
  command->mutable_output()->set_consumed(false);
  context_->mutable_converter()->Reset();
  OutputKey(command);
  return true;
}

bool Session::EchoBackAndClearUndoContext(commands::Command* command) {
  command->mutable_output()->set_consumed(false);

  // Don't clear undo context when KeyEvent has a modifier key only.
  // TODO(hsumita): A modifier key may be assigned to another functions.
  //                ex) InsertSpace
  //                We need to check it outside of this function.
  const commands::KeyEvent& key_event = command->input().key();

  if (IsPendingDirectCommitLearningDiscardKey(key_event)) {
    DiscardPendingDirectCommitLearning(
        "echo_back_discard_key_after_direct_commit");
  }

  if (IsPendingZenzFeedbackDiscardKey(key_event)) {
    DiscardPendingZenzFeedback("echo_back_discard_key");
  }

  if (!IsPureModifierKeyEvent(key_event)) {
    ClearUndoContext();
  }

  return EchoBack(command);
}

bool Session::DoNothing(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  // Quick hack for zero query suggestion.
  // Caveats: Resetting converter causes b/8703702 on Windows.
  // Basically we should not *do* something in DoNothing.
  // TODO(komatsu): Fix this.
  if (context_->GetRequest().zero_query_suggestion() &&
      context_->converter().IsActive() &&
      (context_->state() == ImeContext::PRECOMPOSITION)) {
    context_->mutable_converter()->Reset();
    Output(command);
  }
  if (context_->state() & (ImeContext::COMPOSITION | ImeContext::CONVERSION)) {
    Output(command);
  }
  return true;
}

bool Session::Revert(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "revert_after_direct_commit_learning");
  DiscardPendingZenzFeedback("revert_after_pending_feedback");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (context_->state() == ImeContext::PRECOMPOSITION) {
    context_->mutable_converter()->Revert();
    return EchoBackAndClearUndoContext(command);
  }

  if (!(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::CONVERSION))) {
    return DoNothing(command);
  }

  command->mutable_output()->set_consumed(true);
  ClearUndoContext();

  SetStateToPredompositionAndCancel(context_.get());
  ClearZenzConversionState();
  Output(command);
  return true;
}

bool Session::ResetContext(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "reset_context_after_direct_commit_learning");
  DiscardPendingZenzFeedback("reset_context_after_pending_feedback");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (context_->state() == ImeContext::PRECOMPOSITION) {
    context_->mutable_converter()->Reset();
    return EchoBackAndClearUndoContext(command);
  }

  command->mutable_output()->set_consumed(true);
  ClearUndoContext();

  context_->mutable_converter()->Reset();

  SetStateToPredompositionAndCancel(context_.get());
  ClearZenzConversionState();
  Output(command);
  return true;
}

void Session::SetTable(std::shared_ptr<const composer::Table> table) {
  if (!table) {
    return;
  }
  ClearUndoContext();
  context_->mutable_composer()->SetTable(std::move(table));
}

void Session::SetConfig(std::shared_ptr<const config::Config> config) {
  DCHECK(config);
  ClearUndoContext();
  context_->SetConfig(std::move(config));
}

void Session::SetRequest(std::shared_ptr<const commands::Request> request) {
  DCHECK(request);
  ClearUndoContext();
  context_->SetRequest(std::move(request));
}

void Session::SetKeyMapManager(
    std::shared_ptr<const mozc::keymap::KeyMapManager> key_map_manager) {
  DCHECK(key_map_manager);
  context_->SetKeyMapManager(key_map_manager);
}

bool Session::GetStatus(commands::Command* command) {
  OutputMode(command);
  return true;
}

bool Session::RequestConvertReverse(commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION &&
      context_->state() != ImeContext::DIRECT) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  Output(command);

  // Fill callback message.
  commands::SessionCommand* session_command =
      command->mutable_output()->mutable_callback()->mutable_session_command();
  session_command->set_type(commands::SessionCommand::CONVERT_REVERSE);
  return true;
}

bool Session::RequestReconvertSelectionOrInsertSpace(
    commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION) {
    return DoNothing(command);
  }

  // Build the normal InsertSpace output first.  The TSF client will apply this
  // output only when the application has no selected text.  When selected text
  // exists, the callback is handled before the fallback output is applied.
  if (!InsertSpace(command)) {
    return false;
  }

  commands::SessionCommand* session_command =
      command->mutable_output()->mutable_callback()->mutable_session_command();
  session_command->set_type(
      commands::SessionCommand::RECONVERT_SELECTION_OR_INSERT_SPACE);
  return true;
}

bool Session::ConvertReverse(commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION &&
      context_->state() != ImeContext::DIRECT) {
    return DoNothing(command);
  }

  const std::string& composition = command->input().command().text();

  // Validate before requesting reverse conversion
  if (!Util::IsValidUtf8(composition)) {
    DLOG(INFO) << "Input is not valid text as utf8";
    return DoNothing(command);
  }
  for (ConstChar32Iterator iter(composition); !iter.Done(); iter.Next()) {
    if (!Util::IsAcceptableCharacterAsCandidate(iter.Get())) {
      DLOG(INFO)
          << "Input contains characters not suitable for reverse conversion";
      return DoNothing(command);
    }
  }

  std::string reading;
  if (!context_->mutable_converter()->GetReadingText(composition, &reading)) {
    LOG(ERROR) << "Failed to get reading text";
    return DoNothing(command);
  }

  composer::Composer* composer = context_->mutable_composer();
  composer->Reset();
  ClearUndoContext();
  std::vector<std::string> reading_characters;
  composer->InsertCharacterPreedit(reading);
  composer->set_source_text(composition);
  // start conversion here.
  if (!context_->mutable_converter()->Convert(*composer)) {
    LOG(ERROR) << "Failed to start conversion for reverse conversion";
    return false;
  }

  command->mutable_output()->set_consumed(true);

  SetSessionState(ImeContext::CONVERSION, context_.get());
  context_->mutable_converter()->SetCandidateListVisible(true);
  Output(command);
  return true;
}

bool Session::RequestUndo(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "undo_after_direct_commit_learning");
  DiscardPendingZenzFeedback("undo_after_pending_feedback");

  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::CONVERSION |
         ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }

  // If undo context is empty, echoes back the key event so that it can be
  // handled by the application. b/5553298
  if (context_->state() == ImeContext::PRECOMPOSITION && !HasUndoContext()) {
    return EchoBack(command);
  }

  command->mutable_output()->set_consumed(true);
  Output(command);

  // Fill callback message.
  commands::SessionCommand* session_command =
      command->mutable_output()->mutable_callback()->mutable_session_command();
  session_command->set_type(commands::SessionCommand::UNDO);
  return true;
}

bool Session::Undo(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "undo_command_after_direct_commit_learning");
  DiscardPendingZenzFeedback("undo_command_after_pending_feedback");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::CONVERSION |
         ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  // Check the undo context
  if (!HasUndoContext()) {
    return DoNothing(command);
  }

  // Roll back converter learning only for commit paths that actually used the
  // converter. Pending-display Submit commits a visible string directly and
  // therefore has no converter learning to revert.
  if (ShouldRevertConverterOnUndo()) {
    context_->mutable_converter()->Revert();
  }

  size_t result_size = 0;
  int32_t cursor_offset = 0;
  if (context_->output().has_result()) {
    // Check the client's capability
    if (!(context_->client_capability().text_deletion() &
          commands::Capability::DELETE_PRECEDING_TEXT)) {
      return DoNothing(command);
    }
    result_size = Util::CharsLen(context_->output().result().value());
    cursor_offset = context_->output().result().cursor_offset();
  }

  PopUndoContext();

  if (result_size > 0) {
    commands::DeletionRange* range =
        command->mutable_output()->mutable_deletion_range();
    range->set_offset(-(static_cast<int32_t>(result_size) + cursor_offset));
    range->set_length(result_size);
  }



  Output(command);
  return true;
}

bool Session::SelectCandidateInternal(commands::Command* command) {
  // If the current state is not conversion, composition or
  // precomposition, the candidate window should not be shown.  (On
  // composition or precomposition, the window is able to be shown as
  // a suggestion window).
  if (!(context_->state() & (ImeContext::CONVERSION | ImeContext::COMPOSITION |
                             ImeContext::PRECOMPOSITION))) {
    return false;
  }
  if (!command->input().has_command() || !command->input().command().has_id()) {
    LOG(WARNING) << "input.command or input.command.id did not exist.";
    return false;
  }
  if (command->input().command().id() == kZenzSuggestionCandidateId &&
      !zenz_suggestion_visible_key_.empty() &&
      zenz_suggestion_visible_key_ ==
          context_->composer().GetQueryForConversion()) {
    command->mutable_output()->set_consumed(true);
    zenz_suggestion_selected_ = true;
    return true;
  }
  if (!context_->converter().IsActive()) {
    LOG(WARNING) << "converter is not active. (no candidates)";
    return false;
  }

  command->mutable_output()->set_consumed(true);

  zenz_suggestion_selected_ = false;
  context_->mutable_converter()->CandidateMoveToId(
      command->input().command().id(), context_->composer());
  SetSessionState(ImeContext::CONVERSION, context_.get());

  return true;
}

bool Session::SelectCandidate(commands::Command* command) {
  if (!SelectCandidateInternal(command)) {
    return DoNothing(command);
  }
  Output(command);
  return true;
}

bool Session::CommitCandidate(commands::Command* command) {
  if (!(context_->state() & (ImeContext::COMPOSITION | ImeContext::CONVERSION |
                             ImeContext::PRECOMPOSITION))) {
    return false;
  }
  const commands::Input& input = command->input();
  if (!input.has_command() || !input.command().has_id()) {
    LOG(WARNING) << "input.command or input.command.id did not exist.";
    return false;
  }
  if (input.command().id() == kZenzSuggestionCandidateId) {
    return CommitZenzSuggestion(command);
  }
  if (!context_->converter().IsActive()) {
    LOG(WARNING) << "converter is not active. (no candidates)";
    return false;
  }
  command->mutable_output()->set_consumed(true);

  PushUndoContext();

  if (context_->state() & ImeContext::CONVERSION) {
    // There is a focused candidate so just select a candidate based on
    // input message and commit first segment.
    context_->mutable_converter()->CandidateMoveToId(input.command().id(),
                                                     context_->composer());
    CommitHeadToFocusedSegmentsInternal(command->input().context());
  } else {
    // No candidate is focused.
    size_t consumed_key_size = 0;
    if (context_->mutable_converter()->CommitSuggestionById(
            input.command().id(), context_->composer(),
            command->input().context(), &consumed_key_size)) {
      if (consumed_key_size < context_->composer().GetLength()) {
        // partial suggestion was committed.
        context_->mutable_composer()->DeleteRange(0, consumed_key_size);
        // Don't clear the undo context, which we've just updated.
        MoveCursorToEndInternal(command, false);
        // Copy the previous output for Undo.
        *context_->mutable_output() = command->output();
        return true;
      }
    }
  }

  if (!context_->converter().IsActive()) {
    // If the converter is not active (ie. the segment size was one.),
    // the state should be switched to precomposition.
    SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

    // Get suggestion if zero_query_suggestion is set.
    // zero_query_suggestion is usually set where the client is a
    // mobile.
    if (context_->GetRequest().zero_query_suggestion()) {
      Suggest(command->input());
    }
  }
  Output(command);
  // Copy the previous output for Undo.
  *context_->mutable_output() = command->output();
  return true;
}

bool Session::HighlightCandidate(commands::Command* command) {
  if (!SelectCandidateInternal(command)) {
    return false;
  }
  context_->mutable_converter()->SetCandidateListVisible(true);
  Output(command);
  return true;
}

bool Session::MaybeSelectCandidate(commands::Command* command) {
  if (context_->state() != ImeContext::CONVERSION) {
    return false;
  }
  // When using special romaji table (== The key event is from a virtual
  // keyboard), don't consume it as a shortcut selection operation.
  if (context_->GetRequest().special_romanji_table() !=
      commands::Request::DEFAULT_TABLE) {
    return false;
  }

  // Note that SHORTCUT_ASDFGHJKL should be handled even when the CapsLock is
  // enabled. This is why we need to normalize the key event here.
  // See b/5655743.
  commands::KeyEvent normalized_keyevent;
  KeyEventUtil::NormalizeModifiers(command->input().key(),
                                   &normalized_keyevent);

  // Check if the input character is in the shortcut.
  // TODO(komatsu): Support non ASCII characters such as Unicode and
  // special keys.
  const char shortcut = static_cast<char>(normalized_keyevent.key_code());
  return context_->mutable_converter()->CandidateMoveToShortcut(shortcut);
}




namespace {
bool ShouldSuppressShiftedAsciiAutoSuggestion(
    const config::Config& config,
    const composer::Composer& composer);
}  // namespace







std::string Session::BuildZenzFeedbackContextClass(
    absl::string_view left_context) const {
  const ZenzContextSanitizationResult result =
      zenz_context_sanitizer_.SanitizeForZenz(
          left_context, GetZenzConversionLeftContextLength(
                            context_->GetConfig()));

  // Do not persist raw context or reversible context snippets.  Feedback uses
  // only a coarse non-reversible class.
  return result.context_class.empty() ? "empty" : result.context_class;
}

void Session::RecordZenzConversionAccepted(
    absl::string_view key,
    absl::string_view left_context,
    absl::string_view value) {
  if (!UseZenzFeedbackLearning(context_->GetConfig())) {
    return;
  }

  if (key.empty() || value.empty()) {
    LOG(ERROR) << "[zenz-feedback] skip accepted empty key/value";
    return;
  }

  const ZenzTextPrivacyDecision key_privacy =
      EvaluateZenzConversionKeyPrivacy(key);
  if (!key_privacy.allow) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] skip accepted key_privacy reason=",
        key_privacy.reason,
        " ",
        ZenzRedactedTextStats("key", key)));
    return;
  }

  const ZenzTextPrivacyDecision value_privacy =
      EvaluateZenzConversionValuePrivacy(value);
  if (!value_privacy.allow) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] skip accepted value_privacy reason=",
        value_privacy.reason,
        " ",
        ZenzRedactedTextStats("value", value)));
    return;
  }

  const std::string context_class =
      BuildZenzFeedbackContextClass(left_context);

  ZenzDebugOutput(absl::StrCat(
      "[zenz-feedback] accepted ",
      ZenzRedactedTextStats("key", key),
      " ", ZenzRedactedTextStats("value", value),
      " context_class=", context_class));

  // ZenzFeedbackStore owns only the full request/response pair.  More general
  // learning from an accepted correction is handled separately through Mozc
  // history below.
  zenz_feedback_store_.RecordAccepted(key, context_class, value);

  ZenzDebugOutput("[zenz-feedback] RecordAccepted returned");
}

bool Session::MaybeLearnZenzCandidateToMozcHistory(
    absl::string_view key,
    absl::string_view value) {
  if (!UseZenzFeedbackLearning(context_->GetConfig())) {
    return false;
  }

  // Mozc history learning is intentionally separate from ZenzFeedbackStore.
  // The feedback TSV remains full-sequence scoped, while Mozc history can learn
  // the accepted external conversion result using converter/history semantics.
  if (key.empty() || value.empty()) {
    return false;
  }

  if (context_->composer().GetInputFieldType() ==
      commands::Context::PASSWORD) {
    return false;
  }

  const ZenzTextPrivacyDecision key_privacy =
      EvaluateZenzConversionKeyPrivacy(key);
  if (!key_privacy.allow) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] skip mozc history key_privacy reason=",
        key_privacy.reason,
        " ",
        ZenzRedactedTextStats("key", key)));
    return false;
  }

  const ZenzTextPrivacyDecision value_privacy =
      EvaluateZenzConversionValuePrivacy(value);
  if (!value_privacy.allow) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] skip mozc history value_privacy reason=",
        value_privacy.reason,
        " ",
        ZenzRedactedTextStats("value", value)));
    return false;
  }

  return context_->mutable_converter()->LearnExternalConversionResult(
      key, value, context_->client_context());
}

int Session::MaybeLearnZenzReverseSegmentsToMozcHistory(
    const std::vector<std::pair<std::string, std::string>>& segments) {
  int learned_count = 0;
  for (const auto& [key, value] : segments) {
    if (MaybeLearnZenzCandidateToMozcHistory(key, value)) {
      ++learned_count;
    }
  }
  return learned_count;
}

int Session::MaybeLearnZenzProjectedSegmentsToMozcHistory(
    const std::vector<ZenzProjectedLearningSegment>& segments) {
  if (!UseZenzFeedbackLearning(context_->GetConfig())) {
    return 0;
  }
  if (segments.size() < 2) {
    return 0;
  }
  if (context_->composer().GetInputFieldType() ==
      commands::Context::PASSWORD) {
    return 0;
  }

  std::vector<ExternalConversionSegment> external_segments;
  external_segments.reserve(segments.size());
  for (const ZenzProjectedLearningSegment& segment : segments) {
    const std::string& key = segment.key;
    const std::string& value = segment.value;
    if (key.empty() || value.empty()) {
      return 0;
    }

    const ZenzTextPrivacyDecision key_privacy =
        EvaluateZenzConversionKeyPrivacy(key);
    if (!key_privacy.allow) {
      ZenzDebugOutput(absl::StrCat(
          "[zenz-feedback] skip projected mozc history key_privacy reason=",
          key_privacy.reason,
          " ",
          ZenzRedactedTextStats("key", key)));
      return 0;
    }

    const ZenzTextPrivacyDecision value_privacy =
        EvaluateZenzConversionValuePrivacy(value);
    if (!value_privacy.allow) {
      ZenzDebugOutput(absl::StrCat(
          "[zenz-feedback] skip projected mozc history value_privacy reason=",
          value_privacy.reason,
          " ",
          ZenzRedactedTextStats("value", value)));
      return 0;
    }

    external_segments.push_back({key, value, segment.is_reranked});
  }

  if (!context_->mutable_converter()->LearnExternalConversionSegments(
          external_segments, context_->client_context())) {
    return 0;
  }
  return static_cast<int>(external_segments.size());
}

bool Session::HasVisibleZenzConversion() const {
  if (zenz_conversion_visible_generation_ == 0 ||
      zenz_conversion_key_.empty() || zenz_conversion_value_.empty() ||
      zenz_conversion_mozc_value_.empty() ||
      context_->state() != ImeContext::CONVERSION) {
    return false;
  }
  return zenz_conversion_key_ ==
         context_->composer().GetQueryForConversion();
}

void Session::SetPendingZenzFeedbackAccepted(
    absl::string_view key,
    absl::string_view context_class,
    absl::string_view value) {
  if (!UseZenzFeedbackLearning(context_->GetConfig())) {
    return;
  }

  if (key.empty() || value.empty()) {
    return;
  }

  const ZenzTextPrivacyDecision key_privacy =
      EvaluateZenzConversionKeyPrivacy(key);
  if (!key_privacy.allow) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] skip pending accepted key_privacy reason=",
        key_privacy.reason,
        " ",
        ZenzRedactedTextStats("key", key)));
    return;
  }

  const ZenzTextPrivacyDecision value_privacy =
      EvaluateZenzConversionValuePrivacy(value);
  if (!value_privacy.allow) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] skip pending accepted value_privacy reason=",
        value_privacy.reason,
        " ",
        ZenzRedactedTextStats("value", value)));
    return;
  }

  pending_zenz_feedback_.pending = true;
  pending_zenz_feedback_.action = PendingZenzFeedback::Action::kAccepted;
  pending_zenz_feedback_.key = std::string(key);
  pending_zenz_feedback_.context_class =
      context_class.empty() ? "empty" : std::string(context_class);
  pending_zenz_feedback_.value = std::string(value);
  pending_zenz_feedback_.reason.clear();
  pending_zenz_feedback_.has_final_committed_value = false;
  pending_zenz_feedback_.final_committed_value.clear();
  const ZenzReverseLearningProjection reverse_learning_projection =
      BuildZenzReverseLearningSegmentsFromPreedit(
          zenz_conversion_mozc_preedit_output_, key, value);
  pending_zenz_feedback_.reverse_learning_segments =
      reverse_learning_projection.changed_segments;
  pending_zenz_feedback_.reverse_projected_learning_segments =
      reverse_learning_projection.projected_segments;

  ZenzDebugOutput(absl::StrCat(
      "[zenz-feedback] pending accepted ",
      ZenzRedactedTextStats("key", key),
      " ", ZenzRedactedTextStats("value", value),
      " context_class=", pending_zenz_feedback_.context_class));
}

void Session::SetPendingZenzFeedbackRejected(absl::string_view reason) {
  if (!UseZenzFeedbackLearning(context_->GetConfig())) {
    return;
  }

  if (!HasVisibleZenzConversion()) {
    return;
  }

  const ZenzTextPrivacyDecision key_privacy =
      EvaluateZenzConversionKeyPrivacy(zenz_conversion_key_);
  if (!key_privacy.allow) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] skip pending rejected key_privacy reason=",
        key_privacy.reason,
        " ",
        ZenzRedactedTextStats("key", zenz_conversion_key_)));
    return;
  }

  const ZenzTextPrivacyDecision value_privacy =
      EvaluateZenzConversionValuePrivacy(zenz_conversion_value_);
  if (!value_privacy.allow) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] skip pending rejected value_privacy reason=",
        value_privacy.reason,
        " ",
        ZenzRedactedTextStats("value", zenz_conversion_value_)));
    return;
  }

  pending_zenz_feedback_.pending = true;
  pending_zenz_feedback_.action = PendingZenzFeedback::Action::kRejected;
  pending_zenz_feedback_.key = zenz_conversion_key_;
  pending_zenz_feedback_.context_class =
      zenz_conversion_context_class_.empty() ? "empty" : zenz_conversion_context_class_;
  pending_zenz_feedback_.value = zenz_conversion_value_;
  pending_zenz_feedback_.reason = std::string(reason);
  pending_zenz_feedback_.has_final_committed_value = false;
  pending_zenz_feedback_.final_committed_value.clear();
  pending_zenz_feedback_.reverse_learning_segments.clear();
  pending_zenz_feedback_.reverse_projected_learning_segments.clear();

  ZenzDebugOutput(absl::StrCat(
      "[zenz-feedback] pending rejected ",
      ZenzRedactedTextStats("key", pending_zenz_feedback_.key),
      " ", ZenzRedactedTextStats("value", pending_zenz_feedback_.value),
      " context_class=", pending_zenz_feedback_.context_class,
      " reason=", pending_zenz_feedback_.reason));
}

void Session::ObservePendingZenzFeedbackCommittedResult(
    const commands::Command& command,
    absl::string_view reason) {
  if (!pending_zenz_feedback_.pending ||
      pending_zenz_feedback_.action != PendingZenzFeedback::Action::kRejected) {
    return;
  }

  // Only a fully committed conversion/direct commit should resolve pending
  // rejected feedback. Partial segment commits stay in CONVERSION and must not
  // be interpreted as the user's final full-sequence decision.
  if (context_->state() != ImeContext::PRECOMPOSITION) {
    return;
  }

  if (!command.output().has_result() ||
      !command.output().result().has_value()) {
    return;
  }

  pending_zenz_feedback_.has_final_committed_value = true;
  pending_zenz_feedback_.final_committed_value =
      command.output().result().value();

  ZenzDebugOutput(absl::StrCat(
      "[zenz-feedback] observed final committed value reason=", reason,
      " ", ZenzRedactedTextStats("key", pending_zenz_feedback_.key),
      " ", ZenzRedactedTextStats("zenz_value", pending_zenz_feedback_.value),
      " ", ZenzRedactedTextStats("final_value",
                                  pending_zenz_feedback_.final_committed_value),
      " context_class=", pending_zenz_feedback_.context_class,
      " pending_reason=", pending_zenz_feedback_.reason));
}

void Session::ConfirmPendingZenzFeedback() {
  if (!pending_zenz_feedback_.pending) {
    return;
  }

  if (!UseZenzFeedbackLearning(context_->GetConfig())) {
    pending_zenz_feedback_ = PendingZenzFeedback();
    return;
  }

  if (pending_zenz_feedback_.action ==
      PendingZenzFeedback::Action::kAccepted) {
    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] confirm pending accepted ",
        ZenzRedactedTextStats("key", pending_zenz_feedback_.key),
        " ", ZenzRedactedTextStats("value", pending_zenz_feedback_.value),
        " context_class=", pending_zenz_feedback_.context_class));

    // Accepted feedback stored in the TSV remains full-sequence scoped.
    // Any broader generalization is delegated to Mozc history learning below.
    zenz_feedback_store_.RecordAccepted(
        pending_zenz_feedback_.key,
        pending_zenz_feedback_.context_class,
        pending_zenz_feedback_.value);

    const bool learned_to_mozc_history =
        MaybeLearnZenzCandidateToMozcHistory(
            pending_zenz_feedback_.key,
            pending_zenz_feedback_.value);

    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] mozc history learning ",
        ZenzBool(learned_to_mozc_history),
        " ", ZenzRedactedTextStats("key", pending_zenz_feedback_.key),
        " ", ZenzRedactedTextStats("value", pending_zenz_feedback_.value),
        " context_class=", pending_zenz_feedback_.context_class));

    const int projected_segment_learning_count =
        MaybeLearnZenzProjectedSegmentsToMozcHistory(
            pending_zenz_feedback_.reverse_projected_learning_segments);

    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] projected segment mozc history learning count=",
        projected_segment_learning_count,
        " context_class=", pending_zenz_feedback_.context_class));

    int reverse_segment_learning_count = 0;
    if (projected_segment_learning_count == 0) {
      reverse_segment_learning_count =
          MaybeLearnZenzReverseSegmentsToMozcHistory(
              pending_zenz_feedback_.reverse_learning_segments);
    }

    ZenzDebugOutput(absl::StrCat(
        "[zenz-feedback] reverse segment mozc history learning count=",
        reverse_segment_learning_count,
        " context_class=", pending_zenz_feedback_.context_class));
  } else if (pending_zenz_feedback_.action ==
             PendingZenzFeedback::Action::kRejected) {
    if (!IsExplicitZenzHardRejectReason(pending_zenz_feedback_.reason) &&
        !pending_zenz_feedback_.has_final_committed_value) {
      ZenzDebugOutput(absl::StrCat(
          "[zenz-feedback] neutralize pending rejected without final commit ",
          ZenzRedactedTextStats("key", pending_zenz_feedback_.key),
          " ", ZenzRedactedTextStats("value", pending_zenz_feedback_.value),
          " context_class=", pending_zenz_feedback_.context_class,
          " reason=", pending_zenz_feedback_.reason));
    } else if (!IsExplicitZenzHardRejectReason(pending_zenz_feedback_.reason) &&
               pending_zenz_feedback_.final_committed_value ==
                   pending_zenz_feedback_.value) {
      ZenzDebugOutput(absl::StrCat(
          "[zenz-feedback] neutralize pending rejected same final value ",
          ZenzRedactedTextStats("key", pending_zenz_feedback_.key),
          " ", ZenzRedactedTextStats("value", pending_zenz_feedback_.value),
          " context_class=", pending_zenz_feedback_.context_class,
          " reason=", pending_zenz_feedback_.reason));
    } else {
      ZenzDebugOutput(absl::StrCat(
          "[zenz-feedback] confirm pending rejected ",
          ZenzRedactedTextStats("key", pending_zenz_feedback_.key),
          " ", ZenzRedactedTextStats("value", pending_zenz_feedback_.value),
          " context_class=", pending_zenz_feedback_.context_class,
          " reason=", pending_zenz_feedback_.reason));

      // Rejected feedback is full-sequence scoped too.  A final mismatch after
      // Space revert should not create segment-local negative evidence.
      zenz_feedback_store_.RecordRejected(
          pending_zenz_feedback_.key,
          pending_zenz_feedback_.context_class,
          pending_zenz_feedback_.value,
          pending_zenz_feedback_.reason);
    }
  }

  pending_zenz_feedback_ = PendingZenzFeedback();
}

void Session::DiscardPendingZenzFeedback(absl::string_view reason) {
  if (!pending_zenz_feedback_.pending) {
    return;
  }

  ZenzDebugOutput(absl::StrCat(
      "[zenz-feedback] discard pending feedback reason=", reason,
      " ", ZenzRedactedTextStats("key", pending_zenz_feedback_.key),
      " ", ZenzRedactedTextStats("value", pending_zenz_feedback_.value),
      " context_class=", pending_zenz_feedback_.context_class));

  pending_zenz_feedback_ = PendingZenzFeedback();
}

bool Session::SetPendingDirectCommitLearning(
    absl::string_view key,
    absl::string_view value,
    absl::string_view reason) {
  if (key.empty() || value.empty()) {
    return false;
  }

  if (context_->composer().GetInputFieldType() ==
      commands::Context::PASSWORD) {
    return false;
  }

  pending_direct_commit_learning_.pending = true;
  pending_direct_commit_learning_.key = std::string(key);
  pending_direct_commit_learning_.value = std::string(value);
  pending_direct_commit_learning_.reason = std::string(reason);

  // Keep a snapshot of the converter state immediately after the normal
  // conversion commit.  The current converter will be reset later by
  // CommitStringDirectly(), so delayed cancellation must use this snapshot to
  // revert the original rich Mozc learning.
  pending_direct_commit_learning_.revert_context =
      std::make_unique<ImeContext>(*context_);

  ZenzDebugOutput(absl::StrCat(
      "[direct-commit-learning] pending delayed revert reason=", reason,
      " ", ZenzRedactedTextStats("key",
                                 pending_direct_commit_learning_.key),
      " ", ZenzRedactedTextStats("value",
                                 pending_direct_commit_learning_.value)));

  return true;
}

bool Session::SetPendingDirectCommitLearningFromCommittedResult(
    const commands::Command& command,
    absl::string_view reason) {
  if (!command.output().has_result()) {
    return false;
  }

  const commands::Result& result = command.output().result();
  return SetPendingDirectCommitLearning(result.key(), result.value(), reason);
}

void Session::ConfirmPendingDirectCommitLearning(absl::string_view reason) {
  if (!pending_direct_commit_learning_.pending) {
    return;
  }

  ZenzDebugOutput(absl::StrCat(
      "[direct-commit-learning] confirm reason=", reason,
      " original_reason=", pending_direct_commit_learning_.reason,
      " ", ZenzRedactedTextStats("key",
                                 pending_direct_commit_learning_.key),
      " ", ZenzRedactedTextStats("value",
                                 pending_direct_commit_learning_.value)));

  pending_direct_commit_learning_ = PendingDirectCommitLearning();
}

void Session::DiscardPendingDirectCommitLearning(absl::string_view reason) {
  if (!pending_direct_commit_learning_.pending) {
    return;
  }

  if (pending_direct_commit_learning_.revert_context != nullptr) {
    pending_direct_commit_learning_.revert_context
        ->mutable_converter()
        ->Revert();
  }

  ZenzDebugOutput(absl::StrCat(
      "[direct-commit-learning] discard and revert reason=", reason,
      " original_reason=", pending_direct_commit_learning_.reason,
      " ", ZenzRedactedTextStats("key",
                                 pending_direct_commit_learning_.key),
      " ", ZenzRedactedTextStats("value",
                                 pending_direct_commit_learning_.value)));

  pending_direct_commit_learning_ = PendingDirectCommitLearning();
}

void Session::HandlePendingDirectCommitLearningForKeyEvent(
    const commands::KeyEvent& key) {
  if (!pending_direct_commit_learning_.pending) {
    return;
  }

  if (IsPendingDirectCommitLearningDiscardKey(key)) {
    DiscardPendingDirectCommitLearning("discard_key_after_direct_commit");
    return;
  }

  if (IsPureModifierKeyEvent(key)) {
    return;
  }

  ConfirmPendingDirectCommitLearning("next_real_key_after_direct_commit");
}

void Session::HandlePendingDirectCommitLearningForSessionCommand(
    commands::SessionCommand::CommandType type) {
  if (!pending_direct_commit_learning_.pending) {
    return;
  }

  switch (type) {
    case commands::SessionCommand::REVERT:
    case commands::SessionCommand::RESET_CONTEXT:
    case commands::SessionCommand::UNDO:
      DiscardPendingDirectCommitLearning(
          "session_command_discard_after_direct_commit");
      break;
    default:
      break;
  }
}

void Session::HandlePendingZenzFeedbackForKeyEvent(
    const commands::KeyEvent& key) {
  if (!pending_zenz_feedback_.pending) {
    return;
  }

  // This function is called only from InsertCharacter(), i.e. after the keymap
  // has already classified the event as text insertion.  Do not re-classify the
  // event here by key_string/key_code; some real romaji input events may have no
  // useful key_string.
  //
  // While still in conversion, keys such as Space, Enter, candidate movement,
  // and candidate shortcut selection are still part of deciding the current
  // conversion result. They must not confirm pending zenz feedback.
  if (context_->state() == ImeContext::CONVERSION) {
    return;
  }

  ZenzDebugOutput(absl::StrCat(
      "[zenz-feedback] confirm pending by InsertCharacter"
      " state=", static_cast<int>(context_->state()),
      " has_key_string=", ZenzBool(key.has_key_string()),
      " key_string_bytes=",
      key.has_key_string() ? key.key_string().size() : 0,
      " has_key_code=", ZenzBool(key.has_key_code()),
      " key_code=", key.has_key_code() ? key.key_code() : 0));

  ConfirmPendingZenzFeedback();
}

void Session::HandlePendingZenzFeedbackForSessionCommand(
    commands::SessionCommand::CommandType type) {
  switch (type) {
    case commands::SessionCommand::REVERT:
    case commands::SessionCommand::RESET_CONTEXT:
    case commands::SessionCommand::UNDO:
      DiscardPendingZenzFeedback("session_command_discard");
      break;
    default:
      break;
  }
}

void Session::CancelPendingZenzConversion() {
  ++zenz_conversion_generation_;
  pending_zenz_conversion_ = PendingZenzConversion();

  if (zenz_conversion_service_ != nullptr) {
    zenz_conversion_service_->CancelPending();
  }
}

void Session::ClearZenzConversionState() {
  ++zenz_conversion_generation_;
  pending_zenz_conversion_ = PendingZenzConversion();

  if (zenz_conversion_service_ != nullptr) {
    zenz_conversion_service_->CancelPending();
  }

  zenz_conversion_visible_generation_ = 0;
  zenz_conversion_key_.clear();
  zenz_conversion_display_key_.clear();
  zenz_conversion_value_.clear();
  zenz_conversion_mozc_value_.clear();
  zenz_conversion_context_class_.clear();
  zenz_conversion_left_context_.clear();
  zenz_conversion_mozc_preedit_output_.Clear();
}

bool Session::MaybeApplyZenzFeedbackConversion(
    commands::Command* command) {
  const config::Config& config = context_->GetConfig();
  if (!UseZenzFeedbackLearning(config) || !config.use_zenz_conversion() ||
      context_->state() != ImeContext::CONVERSION ||
      context_->composer().GetInputFieldType() == commands::Context::PASSWORD ||
      !command->output().has_preedit() ||
      command->output().preedit().segment_size() <= 1) {
    return false;
  }
  const std::string key = context_->composer().GetQueryForConversion();
  const commands::Preedit mozc_preedit = command->output().preedit();
  std::string mozc_value;
  for (const commands::Preedit::Segment& segment : mozc_preedit.segment()) {
    mozc_value.append(segment.value());
  }
  if (key.empty() || mozc_value.empty() ||
      Util::CharsLen(key) < kMinimumZenzConversionKeyLength) {
    return false;
  }
  const ZenzTextPrivacyDecision key_privacy =
      EvaluateZenzConversionKeyPrivacy(key);
  const ZenzTextPrivacyDecision value_privacy =
      EvaluateZenzConversionValuePrivacy(mozc_value);
  if (!key_privacy.allow || !value_privacy.allow) {
    return false;
  }
  const std::vector<ProtectedConversionSpan> protected_spans =
      BuildZenzProtectedConversionSpans(context_->converter(),
                                        command->output(), key, mozc_value);
  const ZenzClientContextView client_context =
      GetZenzClientContextView(context_->client_context());
  ZenzContextAssemblyInput context_input;
  context_input.preceding_text = client_context.preceding_text;
  context_input.left_max_chars = GetZenzConversionLeftContextLength(config);
  const ZenzContextAssemblyResult assembled_context =
      zenz_context_assembler_.Assemble(context_input);
  const std::string context_class =
      assembled_context.left.context_class.empty()
          ? std::string("empty")
          : assembled_context.left.context_class;
  const std::vector<ZenzFeedbackCandidate> candidates =
      zenz_feedback_store_.GetAcceptedCandidates(
          key, context_class, GetZenzFeedbackAutoBlockPolicy(config));
  for (const ZenzFeedbackCandidate& candidate : candidates) {
    if (!EvaluateZenzConversionValuePrivacy(candidate.value).allow) {
      continue;
    }
    ZenzValidationInput validation_input;
    validation_input.key = key;
    validation_input.mozc_value = mozc_value;
    validation_input.zenz_value = candidate.value;
    validation_input.left_context = assembled_context.left.prompt_context;
    validation_input.min_key_length = kMinimumZenzConversionKeyLength;
    validation_input.allow_synthetic_candidate =
        config.allow_zenz_synthetic_candidate();
    if (!zenz_output_validator_.Validate(validation_input).accept) {
      continue;
    }
    ZenzAdoptionInput adoption_input;
    adoption_input.key = key;
    adoption_input.mozc_value = mozc_value;
    adoption_input.zenz_value = candidate.value;
    adoption_input.protected_spans = protected_spans;
    const ZenzAdoptionResult adoption =
        zenz_adoption_policy_.Decide(adoption_input);
    if (adoption.action == ZenzAdoptionResult::Action::kReject) {
      continue;
    }
    ++zenz_conversion_generation_;
    pending_zenz_conversion_ = PendingZenzConversion();
    zenz_conversion_visible_generation_ = zenz_conversion_generation_;
    zenz_conversion_key_ = key;
    zenz_conversion_display_key_ = key;
    zenz_conversion_value_ = adoption.value;
    zenz_conversion_mozc_value_ = mozc_value;
    zenz_conversion_context_class_ = context_class;
    zenz_conversion_left_context_ = assembled_context.left.prompt_context;
    zenz_conversion_mozc_preedit_output_ = mozc_preedit;
    return OutputZenzConversion(adoption.value, command);
  }
  return false;
}

bool Session::MaybeScheduleZenzConversion(commands::Command* command) {
  const config::Config& config = context_->GetConfig();
  if (!config.use_zenz_conversion() ||
      context_->state() != ImeContext::CONVERSION ||
      context_->composer().GetInputFieldType() == commands::Context::PASSWORD ||
      !command->output().has_preedit()) {
    return false;
  }
  const std::string key = context_->composer().GetQueryForConversion();
  const commands::Preedit mozc_preedit = command->output().preedit();
  std::string mozc_value;
  for (const commands::Preedit::Segment& segment : mozc_preedit.segment()) {
    mozc_value.append(segment.value());
  }
  if (key.empty() || mozc_value.empty() ||
      Util::CharsLen(key) < kMinimumZenzConversionKeyLength) {
    return false;
  }
  const ZenzTextPrivacyDecision key_privacy =
      EvaluateZenzConversionKeyPrivacy(key);
  const ZenzTextPrivacyDecision value_privacy =
      EvaluateZenzConversionValuePrivacy(mozc_value);
  if (!key_privacy.allow || !value_privacy.allow) {
    return false;
  }
  const std::vector<ProtectedConversionSpan> protected_spans =
      BuildZenzProtectedConversionSpans(context_->converter(),
                                        command->output(), key, mozc_value);
  const ZenzClientContextView client_context =
      GetZenzClientContextView(context_->client_context());
  ZenzContextAssemblyInput context_input;
  context_input.preceding_text = client_context.preceding_text;
  context_input.following_text = client_context.following_text;
  context_input.left_max_chars = GetZenzConversionLeftContextLength(config);
  context_input.right_max_chars = GetZenzConversionRightContextLength(config);
  const ZenzContextAssemblyResult assembled_context =
      zenz_context_assembler_.Assemble(context_input);
  ZenzPromptOptions prompt_options;
  prompt_options.left_context = assembled_context.left.prompt_context;
  prompt_options.right_context = assembled_context.right.prompt_context;
  prompt_options.profile = config.zenz_profile();
  prompt_options.topic = config.zenz_topic();
  prompt_options.style = config.zenz_style();
  prompt_options.settings = config.zenz_settings();
  ZenzProtectedPromptInput protected_prompt_input;
  protected_prompt_input.key = key;
  protected_prompt_input.protected_spans = protected_spans;
  const ZenzProtectedPromptResult protected_prompt =
      zenz_adoption_policy_.ProtectPromptKey(protected_prompt_input);
  ZenzPromptBuilder prompt_builder;
  ++zenz_conversion_generation_;
  pending_zenz_conversion_ = PendingZenzConversion();
  pending_zenz_conversion_.generation = zenz_conversion_generation_;
  pending_zenz_conversion_.key = key;
  pending_zenz_conversion_.left_context =
      assembled_context.left.prompt_context;
  pending_zenz_conversion_.right_context =
      assembled_context.right.prompt_context;
  pending_zenz_conversion_.context_class =
      assembled_context.left.context_class;
  pending_zenz_conversion_.mozc_value = mozc_value;
  pending_zenz_conversion_.symbol_style_source = key;
  pending_zenz_conversion_.prompt =
      prompt_builder.Build(protected_prompt.key, prompt_options);
  pending_zenz_conversion_.mozc_preedit_output = mozc_preedit;
  pending_zenz_conversion_.protected_spans = protected_prompt.protected_spans;
  pending_zenz_conversion_.issued_at = Clock::GetAbslTime();
  pending_zenz_conversion_.pending = true;
  pending_zenz_conversion_.submitted = false;
  pending_zenz_conversion_.poll_count = 0;
  ZenzDebugOutput(absl::StrCat(
      "[zenz] normal conversion request ", ZenzRedactedTextStats("key", key),
      " ", ZenzRedactedTextStats("mozc_value", mozc_value),
      " context_class=", pending_zenz_conversion_.context_class,
      " context_allowed=", ZenzBool(assembled_context.left.allowed_for_prompt),
      " right_context_allowed=", ZenzBool(assembled_context.right.allowed_for_prompt)));
  command->mutable_output()->set_zenz_conversion_pending(true);
  return AdvancePendingZenzConversion(
      command, /*refresh_output_on_submit=*/false);
}

void Session::MaybeScheduleZenzSuggestion() {
  const config::Config& config = context_->GetConfig();
  const std::string key = context_->composer().GetQueryForConversion();
  if (!config.use_zenz_conversion() ||
      !(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::PRECOMPOSITION)) ||
      key.empty() ||
      context_->composer().GetInputFieldType() == commands::Context::PASSWORD ||
      Util::CharsLen(key) < kMinimumZenzConversionKeyLength) {
    return;
  }

  commands::Output mozc_output;
  context_->converter().FillOutput(context_->composer(), &mozc_output);
  std::string mozc_value;
  if (mozc_output.has_all_candidate_words() &&
      mozc_output.all_candidate_words().candidates_size() > 0) {
    const commands::CandidateList& candidates =
        mozc_output.all_candidate_words();
    const int focused_index = candidates.has_focused_index()
                                  ? candidates.focused_index()
                                  : 0;
    for (const commands::CandidateWord& candidate : candidates.candidates()) {
      if (candidate.index() == focused_index) {
        mozc_value = candidate.value();
        break;
      }
    }
  }
  if (mozc_value.empty() && mozc_output.has_candidate_window() &&
      mozc_output.candidate_window().candidate_size() > 0) {
    mozc_value = mozc_output.candidate_window().candidate(0).value();
  }
  if (mozc_value.empty()) {
    mozc_value = context_->composer().GetStringForSubmission();
  }
  if (mozc_value.empty()) {
    return;
  }

  if (!EvaluateZenzConversionKeyPrivacy(key).allow ||
      !EvaluateZenzConversionValuePrivacy(mozc_value).allow) {
    return;
  }

  const std::vector<ProtectedConversionSpan> protected_spans =
      BuildZenzProtectedConversionSpans(context_->converter(), mozc_output, key,
                                        mozc_value);
  const ZenzClientContextView client_context =
      GetZenzClientContextView(context_->client_context());
  ZenzContextAssemblyInput context_input;
  context_input.preceding_text = client_context.preceding_text;
  context_input.following_text = client_context.following_text;
  context_input.left_max_chars = GetZenzConversionLeftContextLength(config);
  context_input.right_max_chars = GetZenzConversionRightContextLength(config);
  const ZenzContextAssemblyResult assembled_context =
      zenz_context_assembler_.Assemble(context_input);

  ZenzPromptOptions prompt_options;
  prompt_options.left_context = assembled_context.left.prompt_context;
  prompt_options.right_context = assembled_context.right.prompt_context;
  prompt_options.profile = config.zenz_profile();
  prompt_options.topic = config.zenz_topic();
  prompt_options.style = config.zenz_style();
  prompt_options.settings = config.zenz_settings();
  ZenzProtectedPromptInput protected_prompt_input;
  protected_prompt_input.key = key;
  protected_prompt_input.protected_spans = protected_spans;
  const ZenzProtectedPromptResult protected_prompt =
      zenz_adoption_policy_.ProtectPromptKey(protected_prompt_input);
  ZenzPromptBuilder prompt_builder;

  pending_zenz_suggestion_ = PendingZenzSuggestion();
  pending_zenz_suggestion_.generation = zenz_suggestion_generation_;
  pending_zenz_suggestion_.key = key;
  pending_zenz_suggestion_.mozc_value = mozc_value;
  pending_zenz_suggestion_.left_context =
      assembled_context.left.prompt_context;
  pending_zenz_suggestion_.context_class =
      assembled_context.left.context_class;
  pending_zenz_suggestion_.prompt =
      prompt_builder.Build(protected_prompt.key, prompt_options);
  pending_zenz_suggestion_.protected_spans = protected_prompt.protected_spans;
  pending_zenz_suggestion_.issued_at = Clock::GetAbslTime();
  pending_zenz_suggestion_.pending = true;
}

void Session::AttachZenzSuggestionPollCallback(
    commands::Command* command) const {
  commands::Output::Callback* callback =
      command->mutable_output()->mutable_callback();
  commands::SessionCommand* session_command =
      callback->mutable_session_command();
  session_command->set_type(commands::SessionCommand::APPLY_ZENZ_SUGGESTION);
  session_command->set_zenz_conversion_generation(
      pending_zenz_suggestion_.generation);
  session_command->set_zenz_conversion_key(pending_zenz_suggestion_.key);
  callback->set_delay_millisec(pending_zenz_suggestion_.submitted
                                   ? kDefaultZenzConversionPollMsec
                                   : kZenzSuggestionDebounceMsec);
}

bool Session::IsCurrentZenzSuggestionCallback(
    const commands::Command& command) const {
  if (!pending_zenz_suggestion_.pending ||
      !(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::PRECOMPOSITION)) ||
      context_->composer().GetQueryForConversion() !=
          pending_zenz_suggestion_.key ||
      !command.input().has_command()) {
    return false;
  }
  const commands::SessionCommand& session_command = command.input().command();
  return session_command.has_zenz_conversion_generation() &&
         session_command.zenz_conversion_generation() ==
             pending_zenz_suggestion_.generation &&
         session_command.has_zenz_conversion_key() &&
         session_command.zenz_conversion_key() == pending_zenz_suggestion_.key;
}

bool Session::ApplyZenzSuggestion(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (!IsCurrentZenzSuggestionCallback(*command)) {
    return DoNothing(command);
  }

  const config::Config& config = context_->GetConfig();
  const uint32_t timeout_msec = GetZenzConversionTimeoutMsec(config);
  if (!pending_zenz_suggestion_.submitted) {
    pending_zenz_suggestion_.submitted = true;
    pending_zenz_suggestion_.issued_at = Clock::GetAbslTime();
    ZenzConversionRequest request;
    request.generation = pending_zenz_suggestion_.generation;
    request.key = pending_zenz_suggestion_.key;
    request.prompt = pending_zenz_suggestion_.prompt;
    request.left_context = pending_zenz_suggestion_.left_context;
    request.mozc_value = pending_zenz_suggestion_.mozc_value;
    request.pipe_name = config.zenz_pipe_name();
    request.timeout_msec = timeout_msec;
    request.max_output_chars = 256;
    request.issued_at = pending_zenz_suggestion_.issued_at;
    ZenzPromptBuilder prompt_builder;
    request.reading_katakana =
        prompt_builder.HiraganaToKatakana(pending_zenz_suggestion_.key);
    EnsureZenzConversionService()->Submit(std::move(request));
    Output(command);
    return true;
  }

  std::optional<ZenzConversionResponse> response =
      zenz_conversion_service_->TakeResult(pending_zenz_suggestion_.generation);
  if (!response.has_value()) {
    ++pending_zenz_suggestion_.poll_count;
    const uint32_t max_poll_count =
        std::max<uint32_t>(
            1, timeout_msec / kDefaultZenzConversionPollMsec + 2);
    if (Clock::GetAbslTime() - pending_zenz_suggestion_.issued_at >=
            absl::Milliseconds(timeout_msec) ||
        pending_zenz_suggestion_.poll_count >= max_poll_count) {
      pending_zenz_suggestion_ = PendingZenzSuggestion();
      Output(command);
      return true;
    }
    Output(command);
    return true;
  }

  std::string value = response->value;
  if (!response->ok || response->timeout ||
      response->generation != pending_zenz_suggestion_.generation ||
      (!response->key.empty() &&
       response->key != pending_zenz_suggestion_.key)) {
    pending_zenz_suggestion_ = PendingZenzSuggestion();
    Output(command);
    return true;
  }
  if (!pending_zenz_suggestion_.left_context.empty() &&
      StartsWithString(value, pending_zenz_suggestion_.left_context)) {
    value.erase(0, pending_zenz_suggestion_.left_context.size());
  }
  value = zenz_adoption_policy_.RestorePlaceholders(
      value, pending_zenz_suggestion_.protected_spans);
  value = ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
      pending_zenz_suggestion_.key, pending_zenz_suggestion_.mozc_value, value);
  ZenzValidationInput validation_input;
  validation_input.key = pending_zenz_suggestion_.key;
  validation_input.mozc_value = pending_zenz_suggestion_.mozc_value;
  validation_input.zenz_value = value;
  validation_input.left_context = pending_zenz_suggestion_.left_context;
  validation_input.min_key_length = kMinimumZenzConversionKeyLength;
  validation_input.allow_synthetic_candidate =
      config.allow_zenz_synthetic_candidate();
  if (!zenz_output_validator_.Validate(validation_input).accept) {
    pending_zenz_suggestion_ = PendingZenzSuggestion();
    Output(command);
    return true;
  }
  ZenzAdoptionInput adoption_input;
  adoption_input.key = pending_zenz_suggestion_.key;
  adoption_input.mozc_value = pending_zenz_suggestion_.mozc_value;
  adoption_input.zenz_value = value;
  adoption_input.protected_spans = pending_zenz_suggestion_.protected_spans;
  const ZenzAdoptionResult adoption =
      zenz_adoption_policy_.Decide(adoption_input);
  const std::string key = pending_zenz_suggestion_.key;
  const std::string context_class = pending_zenz_suggestion_.context_class;
  pending_zenz_suggestion_ = PendingZenzSuggestion();
  if (adoption.action != ZenzAdoptionResult::Action::kReject &&
      EvaluateZenzConversionValuePrivacy(adoption.value).allow) {
    zenz_suggestion_visible_key_ = key;
    zenz_suggestion_visible_value_ = adoption.value;
    zenz_suggestion_visible_context_class_ = context_class;
    zenz_suggestion_selected_ = false;
  }
  Output(command);
  return true;
}

void Session::AttachZenzConversionPollCallback(
    commands::Command* command) const {
  commands::Output::Callback* callback =
      command->mutable_output()->mutable_callback();
  commands::SessionCommand* session_command =
      callback->mutable_session_command();

  session_command->set_type(
      commands::SessionCommand::APPLY_ZENZ_CONVERSION);
  session_command->set_zenz_conversion_generation(
      pending_zenz_conversion_.generation);
  session_command->set_zenz_conversion_key(pending_zenz_conversion_.key);

  callback->set_delay_millisec(kDefaultZenzConversionPollMsec);
}

bool Session::IsCurrentZenzConversionCallback(
    const commands::Command& command) const {
  if (!pending_zenz_conversion_.pending ||
      context_->state() != ImeContext::CONVERSION ||
      context_->composer().GetQueryForConversion() !=
          pending_zenz_conversion_.key ||
      !command.input().has_command()) {
    return false;
  }
  const commands::SessionCommand& session_command = command.input().command();
  return session_command.has_zenz_conversion_generation() &&
         session_command.zenz_conversion_generation() ==
             pending_zenz_conversion_.generation &&
         session_command.has_zenz_conversion_key() &&
         session_command.zenz_conversion_key() ==
             pending_zenz_conversion_.key;
}

bool Session::OutputConversionWithZenzPending(
    commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  Output(command);
  command->mutable_output()->set_zenz_conversion_pending(true);
  return true;
}

bool Session::OutputConversionAfterZenzStop(
    commands::Command* command,
    absl::string_view debug) {
  command->mutable_output()->set_consumed(true);
  Output(command);
  command->mutable_output()->set_zenz_conversion_pending(false);
  if (!debug.empty()) {
    command->mutable_output()->set_zenz_conversion_debug(std::string(debug));
  }
  return true;
}

ZenzConversionService* Session::EnsureZenzConversionService() {
  if (zenz_conversion_service_ == nullptr) {
    zenz_conversion_service_ =
        std::make_unique<ZenzConversionService>(CreateZenzClient());
  }
  return zenz_conversion_service_.get();
}

bool Session::ApplyZenzConversion(commands::Command* command) {
  ZenzDebugOutput("[zenz] ApplyZenzConversion called");
  command->mutable_output()->set_consumed(true);

  if (!IsCurrentZenzConversionCallback(*command)) {
    ZenzDebugOutput("[zenz] stale zenz callback");
    return DoNothing(command);
  }

  return AdvancePendingZenzConversion(
      command, /*refresh_output_on_submit=*/true);
}

bool Session::AdvancePendingZenzConversion(
    commands::Command* command,
    const bool refresh_output_on_submit) {
  command->mutable_output()->set_consumed(true);
  const config::Config& config = context_->GetConfig();
  const absl::Time now = Clock::GetAbslTime();
  const uint32_t timeout_msec = GetZenzConversionTimeoutMsec(config);

  if (!pending_zenz_conversion_.submitted) {
    pending_zenz_conversion_.issued_at = now;
    pending_zenz_conversion_.submitted = true;
    pending_zenz_conversion_.poll_count = 0;
    ZenzConversionRequest request;
    request.generation = pending_zenz_conversion_.generation;
    request.key = pending_zenz_conversion_.key;
    request.prompt = pending_zenz_conversion_.prompt;
    request.left_context = pending_zenz_conversion_.left_context;
    request.mozc_value = pending_zenz_conversion_.mozc_value;
    request.pipe_name = config.zenz_pipe_name();
    request.timeout_msec = timeout_msec;
    request.max_output_chars = 256;
    request.issued_at = pending_zenz_conversion_.issued_at;
    ZenzPromptBuilder prompt_builder;
    request.reading_katakana =
        prompt_builder.HiraganaToKatakana(pending_zenz_conversion_.key);
    EnsureZenzConversionService()->Submit(std::move(request));

    bool result = true;
    if (refresh_output_on_submit) {
      result = OutputConversionWithZenzPending(command);
    } else {
      command->mutable_output()->set_zenz_conversion_pending(true);
    }
    AttachZenzConversionPollCallback(command);
    return result;
  }

  if (zenz_conversion_service_ == nullptr) {
    CancelPendingZenzConversion();
    return OutputConversionAfterZenzStop(
        command, "zenz_conversion_service_missing");
  }
  std::optional<ZenzConversionResponse> response =
      zenz_conversion_service_->TakeResult(pending_zenz_conversion_.generation);
  if (response.has_value()) {
    return ApplyZenzConversionResult(*response, command);
  }

  ++pending_zenz_conversion_.poll_count;
  const uint32_t async_wait_msec =
      std::max<uint32_t>(timeout_msec, kZenzConversionAsyncWaitMsec);
  const uint32_t max_poll_count = std::max<uint32_t>(
      1, async_wait_msec / kDefaultZenzConversionPollMsec + 2);
  const bool timed_out =
      now - pending_zenz_conversion_.issued_at >=
      absl::Milliseconds(async_wait_msec);
  if (timed_out || pending_zenz_conversion_.poll_count >= max_poll_count) {
    CancelPendingZenzConversion();
    return OutputConversionAfterZenzStop(
        command, timed_out ? "zenz_conversion_timeout"
                           : "zenz_conversion_poll_exhausted");
  }

  const bool result = OutputConversionWithZenzPending(command);
  AttachZenzConversionPollCallback(command);
  return result;
}

bool Session::ApplyZenzConversionResult(
    const ZenzConversionResponse& response,
    commands::Command* command) {
  const config::Config& config = context_->GetConfig();
  if (!response.ok || response.timeout ||
      response.generation != pending_zenz_conversion_.generation ||
      (!response.key.empty() && response.key != pending_zenz_conversion_.key)) {
    const std::string debug = response.debug.empty()
                                  ? "zenz_conversion_failed"
                                  : ZenzSafeDebugReason(response.debug);
    CancelPendingZenzConversion();
    return OutputConversionAfterZenzStop(command, debug);
  }

  std::string zenz_value = response.value;
  if (!pending_zenz_conversion_.left_context.empty() &&
      StartsWithString(zenz_value, pending_zenz_conversion_.left_context)) {
    zenz_value.erase(0, pending_zenz_conversion_.left_context.size());
  }
  zenz_value = zenz_adoption_policy_.RestorePlaceholders(
      zenz_value, pending_zenz_conversion_.protected_spans);
  const absl::string_view symbol_source =
      pending_zenz_conversion_.symbol_style_source.empty()
          ? absl::string_view(pending_zenz_conversion_.key)
          : absl::string_view(pending_zenz_conversion_.symbol_style_source);
  zenz_value = ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
      symbol_source, pending_zenz_conversion_.mozc_value, zenz_value);
  const std::string display_key =
      ZenzOutputValidator::RestoreUserVisibleSymbolStyle(
          symbol_source, pending_zenz_conversion_.mozc_value,
          pending_zenz_conversion_.key);

  const std::string context_class =
      pending_zenz_conversion_.context_class.empty()
          ? BuildZenzFeedbackContextClass(
                pending_zenz_conversion_.left_context)
          : pending_zenz_conversion_.context_class;
  ZenzValidationInput validation_input;
  validation_input.key = pending_zenz_conversion_.key;
  validation_input.mozc_value = pending_zenz_conversion_.mozc_value;
  validation_input.zenz_value = zenz_value;
  validation_input.left_context = pending_zenz_conversion_.left_context;
  validation_input.min_key_length = kMinimumZenzConversionKeyLength;
  validation_input.allow_synthetic_candidate =
      config.allow_zenz_synthetic_candidate();
  const ZenzValidationResult validation =
      zenz_output_validator_.Validate(validation_input);
  if (!validation.accept) {
    CancelPendingZenzConversion();
    return OutputConversionAfterZenzStop(command, validation.reason);
  }

  const ZenzTextPrivacyDecision key_privacy =
      EvaluateZenzConversionKeyPrivacy(pending_zenz_conversion_.key);
  const ZenzTextPrivacyDecision value_privacy =
      EvaluateZenzConversionValuePrivacy(zenz_value);
  if (!key_privacy.allow || !value_privacy.allow) {
    CancelPendingZenzConversion();
    return OutputConversionAfterZenzStop(
        command, !key_privacy.allow
                     ? absl::StrCat("key_privacy_", key_privacy.reason)
                     : absl::StrCat("value_privacy_", value_privacy.reason));
  }

  ZenzAdoptionInput adoption_input;
  adoption_input.key = pending_zenz_conversion_.key;
  adoption_input.mozc_value = pending_zenz_conversion_.mozc_value;
  adoption_input.zenz_value = zenz_value;
  adoption_input.protected_spans = pending_zenz_conversion_.protected_spans;
  const ZenzAdoptionResult adoption =
      zenz_adoption_policy_.Decide(adoption_input);
  if (adoption.action == ZenzAdoptionResult::Action::kReject) {
    CancelPendingZenzConversion();
    return OutputConversionAfterZenzStop(command, adoption.reason);
  }
  zenz_value = adoption.value;
  if (!EvaluateZenzConversionValuePrivacy(zenz_value).allow) {
    CancelPendingZenzConversion();
    return OutputConversionAfterZenzStop(command, "adopted_value_privacy");
  }

  if (UseZenzFeedbackLearning(config)) {
    const ZenzFeedbackDecision decision = zenz_feedback_store_.Decide(
        pending_zenz_conversion_.key, context_class, zenz_value,
        GetZenzFeedbackAutoBlockPolicy(config));
    if (decision.action == ZenzFeedbackAction::kReject) {
      CancelPendingZenzConversion();
      return OutputConversionAfterZenzStop(command, decision.reason);
    }
  }

  zenz_conversion_visible_generation_ = pending_zenz_conversion_.generation;
  zenz_conversion_key_ = pending_zenz_conversion_.key;
  zenz_conversion_display_key_ = display_key;
  zenz_conversion_value_ = zenz_value;
  zenz_conversion_mozc_value_ = pending_zenz_conversion_.mozc_value;
  zenz_conversion_context_class_ = context_class;
  zenz_conversion_left_context_ = pending_zenz_conversion_.left_context;
  zenz_conversion_mozc_preedit_output_ =
      pending_zenz_conversion_.mozc_preedit_output;
  pending_zenz_conversion_.pending = false;
  return OutputZenzConversion(zenz_value, command);
}

bool Session::OutputZenzConversion(
    absl::string_view value,
    commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (!command->output().has_preedit()) {
    // Rebuild the original Mozc conversion output so its candidate window stays
    // available underneath the temporary Zenz preedit.
    Output(command);
  }
  commands::Output* output = command->mutable_output();
  output->set_zenz_conversion_pending(false);
  output->set_zenz_conversion_applied(true);
  commands::Preedit* preedit = output->mutable_preedit();
  preedit->Clear();
  const absl::string_view display_key =
      zenz_conversion_display_key_.empty() ? zenz_conversion_key_
                                           : zenz_conversion_display_key_;
  AddPreeditSegment(display_key, value,
                    commands::Preedit::Segment::HIGHLIGHT, preedit);
  preedit->set_cursor(Util::CharsLen(value));
  return true;
}

bool Session::RevertZenzConversionToMozc(
    commands::Command* command) {
  if (!HasVisibleZenzConversion()) {
    return false;
  }
  SetPendingZenzFeedbackRejected("space_revert_zenz_to_mozc");
  ClearZenzConversionState();
  context_->mutable_converter()->SetCandidateListVisible(true);
  command->mutable_output()->set_consumed(true);
  Output(command);
  command->mutable_output()->set_zenz_conversion_pending(false);
  command->mutable_output()->set_zenz_conversion_applied(false);
  return true;
}

bool Session::CommitZenzConversionResult(commands::Command* command) {
  if (!HasVisibleZenzConversion()) {
    return false;
  }
  const std::string key = zenz_conversion_key_;
  const std::string value = zenz_conversion_value_;
  const std::string context_class = zenz_conversion_context_class_;
  SetPendingZenzFeedbackAccepted(key, context_class, value);
  ClearZenzConversionState();
  CommitStringDirectly(key, value, command);
  return true;
}


void Session::set_client_capability(commands::Capability capability) {
  *context_->mutable_client_capability() = std::move(capability);
}

void Session::set_application_info(commands::ApplicationInfo application_info) {
  *context_->mutable_application_info() = std::move(application_info);
}

const commands::ApplicationInfo& Session::application_info() const {
  return context_->application_info();
}

absl::Time Session::create_session_time() const {
  return context_->create_time();
}

absl::Time Session::last_command_time() const {
  return context_->last_command_time();
}

void Session::ClearPendingRerankedPreeditCommitAfterConvertCancel() {
  pending_reranked_preedit_commit_after_convert_cancel_ = false;
  pending_reranked_preedit_commit_key_.clear();
  pending_reranked_preedit_commit_value_.clear();
  pending_reranked_preedit_commit_segment_keys_.clear();
}

void Session::MaybeSetPendingRerankedPreeditCommitAfterConvertCancel() {
  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  const std::string key = context_->composer().GetQueryForConversion();
  const std::string value = context_->composer().GetStringForSubmission();
  if (key.empty() || value.empty()) {
    return;
  }

  pending_reranked_preedit_commit_after_convert_cancel_ = true;
  pending_reranked_preedit_commit_key_ = key;
  pending_reranked_preedit_commit_value_ = value;

  // Snapshot the visible conversion segment keys before converter->Cancel()
  // clears them.  If the user commits the restored hiragana unchanged, these
  // boundaries let user segment history learn each original conversion segment
  // separately instead of learning only the whole restored preedit string.
  std::vector<std::string> segment_keys;
  if (!context_->converter().GetConversionSegmentKeys(&segment_keys)) {
    return;
  }

  std::string joined_key;
  for (const std::string& segment_key : segment_keys) {
    if (segment_key.empty()) {
      segment_keys.clear();
      break;
    }
    joined_key.append(segment_key);
  }
  if (!segment_keys.empty() && joined_key == key) {
    pending_reranked_preedit_commit_segment_keys_ = std::move(segment_keys);
  }
}

bool Session::ShouldMarkPreeditCommitAsRerankedAfterConvertCancel() const {
  return ShouldMarkPreeditCommitAsRerankedAfterConvertCancel(
      context_->composer());
}

bool Session::ShouldMarkPreeditCommitAsRerankedAfterConvertCancel(
    const composer::Composer& composer) const {
  if (!pending_reranked_preedit_commit_after_convert_cancel_) {
    return false;
  }

  if (composer.GetInputFieldType() == commands::Context::PASSWORD) {
    return false;
  }

  const std::string key = composer.GetQueryForConversion();
  const std::string value = composer.GetStringForSubmission();
  if (key != pending_reranked_preedit_commit_key_ ||
      value != pending_reranked_preedit_commit_value_) {
    return false;
  }

  if (key != value) {
    return false;
  }

  if (Util::CharsLen(key) <
      kMinRerankedPreeditCommitCharsAfterConvertCancel) {
    return false;
  }

  return Util::IsScriptType(key, Util::HIRAGANA);
}

bool Session::CommitPendingRerankedPreeditAfterConvertCancelForDirectCommit(
    const composer::Composer& composer,
    const commands::Context& context,
    absl::string_view reason) {
  if (!pending_reranked_preedit_commit_after_convert_cancel_) {
    return false;
  }

  if (!ShouldMarkPreeditCommitAsRerankedAfterConvertCancel(composer)) {
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    return false;
  }

  const std::string key = composer.GetQueryForConversion();
  const std::string value = composer.GetStringForSubmission();

  const std::vector<std::string> segment_keys =
      pending_reranked_preedit_commit_segment_keys_;
  context_->mutable_converter()->CommitPreedit(
      composer, context, true, segment_keys);
  SetPendingDirectCommitLearning(key, value, reason);
  ClearPendingRerankedPreeditCommitAfterConvertCancel();
  return true;
}

bool Session::InsertCharacter(commands::Command* command) {
  if (!command->input().has_key()) {
    LOG(ERROR) << "No key event: " << command->input();
    return false;
  }
  const commands::KeyEvent& key = command->input().key();
  HandlePendingDirectCommitLearningForKeyEvent(key);
  HandlePendingZenzFeedbackForKeyEvent(key);

  if (key.input_style() == commands::KeyEvent::DIRECT_INPUT &&
      context_->state() == ImeContext::PRECOMPOSITION) {
    if (key.key_string().size() == 1 && key.key_code() == key.key_string()[0] &&
        key.key_code() != ' ') {
      return EchoBackAndClearUndoContext(command);
    }
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    context_->mutable_composer()->InsertCharacterKeyEvent(key);
    CommitCompositionDirectly(command);
    ClearUndoContext();
    return true;
  }

  command->mutable_output()->set_consumed(true);
  const bool had_visible_zenz_correction = HasVisibleZenzConversion();
  const std::string zenz_key_before_edit = zenz_conversion_key_;
  const std::string zenz_value_before_edit = zenz_conversion_value_;
  const std::string zenz_context_class_before_edit =
      zenz_conversion_context_class_.empty() ? "empty"
                                             : zenz_conversion_context_class_;
  const commands::Preedit zenz_mozc_preedit_before_edit =
      zenz_conversion_mozc_preedit_output_;

  // Typing while an inference is pending cancels it. Typing over a visible
  // Zenz overlay resumes ordinary Mozc conversion/editing semantics.
  if (pending_zenz_conversion_.pending || had_visible_zenz_correction) {
    ClearZenzConversionState();
  }

  if (MaybeSelectCandidate(command)) {
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    Output(command);
    return true;
  }

  const std::string composition = context_->composer().GetQueryForConversion();
  bool should_commit = context_->state() == ImeContext::CONVERSION;
  if (context_->GetRequest().space_on_alphanumeric() ==
          commands::Request::SPACE_OR_CONVERT_COMMITTING_COMPOSITION &&
      context_->state() == ImeContext::COMPOSITION && composition.ends_with(' ')) {
    should_commit = true;
  }

  bool committed_conversion_before_insert = false;
  if (should_commit) {
    CommitNotTriggeringZeroQuerySuggest(command);
    committed_conversion_before_insert = true;
    ConfirmPendingZenzFeedback();
    if (key.input_style() == commands::KeyEvent::DIRECT_INPUT) {
      ClearPendingRerankedPreeditCommitAfterConvertCancel();
      ClearUndoContext();
      context_->mutable_composer()->InsertCharacterKeyEvent(key);
      CommitCompositionDirectly(command);
      return true;
    }
  }

  const composer::Composer composer_before_insert = context_->composer();
  context_->mutable_composer()->InsertCharacterKeyEvent(key);
  ClearUndoContext();

  if (context_->composer().GetLength() == 1) {
    ClearZenzConversionState();
  }

  if (CanDirectCommitAfterPunctuation(key)) {
    const auto [direct_commit_key, direct_commit_value] =
        GetDirectCommitStringsWithDirectCommitSuffixFallback(
            composer_before_insert, key);
    const bool learned_reranked_preedit_after_cancel =
        CommitPendingRerankedPreeditAfterConvertCancelForDirectCommit(
            composer_before_insert, command->input().context(),
            "convert_cancel_direct_commit_punctuation");

    if (!learned_reranked_preedit_after_cancel &&
        had_visible_zenz_correction) {
      const size_t length = context_->composer().GetLength();
      const std::string preedit = context_->composer().GetStringForPreedit();
      const std::string last_char(Util::Utf8SubString(preedit, length - 1, 1));
      std::string commit_key = context_->composer().GetQueryForConversion();
      std::string commit_value = zenz_value_before_edit;
      commit_value.append(last_char);
      zenz_conversion_mozc_preedit_output_ = zenz_mozc_preedit_before_edit;
      SetPendingZenzFeedbackAccepted(zenz_key_before_edit,
                                     zenz_context_class_before_edit,
                                     zenz_value_before_edit);
      CommitStringDirectly(commit_key, commit_value, command);
      return true;
    }

    if (learned_reranked_preedit_after_cancel) {
      CommitStringDirectly(direct_commit_key, direct_commit_value, command);
      return true;
    }
    if (committed_conversion_before_insert) {
      SetPendingDirectCommitLearningFromCommittedResult(
          *command, "normal_conversion_direct_commit_punctuation");
    }
    if (direct_commit_key != context_->composer().GetQueryForConversion() ||
        direct_commit_value != context_->composer().GetStringForSubmission()) {
      CommitStringDirectly(direct_commit_key, direct_commit_value, command);
      return true;
    }
    CommitCompositionDirectly(command);
    return true;
  }

  ClearPendingRerankedPreeditCommitAfterConvertCancel();
  if (context_->mutable_composer()->ShouldCommit()) {
    CommitCompositionDirectly(command);
    return true;
  }
  size_t length_to_commit = 0;
  if (context_->composer().ShouldCommitHead(&length_to_commit)) {
    return CommitHead(length_to_commit, command);
  }

  SetSessionState(ImeContext::COMPOSITION, context_.get());
  if (CanStartAutoConversion(key)) {
    CancelPendingZenzConversion();
    return ConvertInternal(command, /*run_zenz=*/false);
  }
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::IsFullWidthInsertSpace(const commands::Input& input) const {
  // If IME is off, any space has to be half-width.
  if (context_->state() == ImeContext::DIRECT) {
    return false;
  }

  // In this method, we should not update the actual input mode stored in
  // the composer even when |input| has a new input mode. Note that this
  // method can be called from TestSendKey, where internal input mode is
  // is not expected to be changed. This is one of the reasons why this
  // method is a const method.
  // On the other hand, this method should behave as if the new input mode
  // in |input| was applied. For example, this method should behave as if
  // the current input mode was HALF_KATAKANA in the following situation.
  //   composer's input mode: HIRAGANA
  //   input.key().mode()   : HALF_KATAKANA
  // To achieve this, we create a temporary composer object to which the
  // new input mode will be stored when |input| has a new input mode.
  auto get_input_mode = [this, &input]() {
    const bool has_mode = (input.has_key() && input.key().has_mode());
    if (!has_mode) {
      return context_->composer().GetInputMode();
    }

    // Copy the current composer state just in case.
    composer::Composer temporary_composer = context_->composer();
    ApplyCompositionMode(input.key().mode(), &temporary_composer);
    // Refer to this temporary composer in this method.
    return temporary_composer.GetInputMode();
  };

  // Check the current config and the current input status.
  bool is_full_width = false;
  switch (context_->GetConfig().space_character_form()) {
    case config::Config::FUNDAMENTAL_INPUT_MODE: {
      const transliteration::TransliterationType input_mode = get_input_mode();
      if (transliteration::T13n::IsInHalfAsciiTypes(input_mode) ||
          transliteration::T13n::IsInHalfKatakanaTypes(input_mode)) {
        is_full_width = false;
      } else {
        is_full_width = true;
      }
      break;
    }
    case config::Config::FUNDAMENTAL_FULL_WIDTH:
      is_full_width = true;
      break;
    case config::Config::FUNDAMENTAL_HALF_WIDTH:
      is_full_width = false;
      break;
    default:
      LOG(WARNING) << "Unknown input mode";
      is_full_width = false;
      break;
  }

  return is_full_width;
}

bool Session::InsertSpace(commands::Command* command) {
  if (IsFullWidthInsertSpace(command->input())) {
    return InsertSpaceFullWidth(command);
  } else {
    return InsertSpaceHalfWidth(command);
  }
}

bool Session::InsertSpaceToggled(commands::Command* command) {
  if (IsFullWidthInsertSpace(command->input())) {
    return InsertSpaceHalfWidth(command);
  } else {
    return InsertSpaceFullWidth(command);
  }
}

bool Session::InsertSpaceHalfWidth(commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::COMPOSITION |
         ImeContext::CONVERSION))) {
    return DoNothing(command);
  }

  if (context_->state() == ImeContext::PRECOMPOSITION) {
    // TODO(komatsu): This is a hack to work around the problem with
    // the inconsistency between TestSendKey and SendKey.
    if (IsPureSpaceKey(command->input().key())) {
      return EchoBackAndClearUndoContext(command);
    }
    // UndoContext will be cleared in |InsertCharacter| in this case.
  }

  const bool has_mode = command->input().key().has_mode();
  const commands::CompositionMode mode = command->input().key().mode();
  command->mutable_input()->clear_key();
  commands::KeyEvent* key_event = command->mutable_input()->mutable_key();
  key_event->set_key_code(' ');
  key_event->set_key_string(" ");
  key_event->set_input_style(commands::KeyEvent::DIRECT_INPUT);
  if (has_mode) {
    key_event->set_mode(mode);
  }
  return InsertCharacter(command);
}

bool Session::InsertSpaceFullWidth(commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::COMPOSITION |
         ImeContext::CONVERSION))) {
    return DoNothing(command);
  }

  if (context_->state() == ImeContext::PRECOMPOSITION) {
    // UndoContext will be cleared in |InsertCharacter| in this case.

    // TODO(komatsu): make sure if
    // |context_->mutable_converter()->Reset()| is necessary here.
    context_->mutable_converter()->Reset();
  }

  const bool has_mode = command->input().key().has_mode();
  const commands::CompositionMode mode = command->input().key().mode();
  command->mutable_input()->clear_key();
  commands::KeyEvent* key_event = command->mutable_input()->mutable_key();
  key_event->set_key_code(' ');
  key_event->set_key_string("　");  // full-width space
  key_event->set_input_style(commands::KeyEvent::DIRECT_INPUT);
  if (has_mode) {
    key_event->set_mode(mode);
  }
  return InsertCharacter(command);
}

bool Session::TryCancelConvertReverse(commands::Command* command) {
  // If source_text is set, it usually means this session started by a
  // reverse conversion.
  if (context_->composer().source_text().empty()) {
    return false;
  }
  CommitSourceTextDirectly(command);
  return true;
}

bool Session::EditCancelOnPasswordField(commands::Command* command) {
  if (context_->composer().GetInputFieldType() != commands::Context::PASSWORD) {
    return false;
  }

  // In password mode, we should commit preedit and close keyboard
  // on Android.
  // TODO(matsuzakit): Remove this trick. b/5955618
  if (context_->composer().source_text().empty()) {
    CommitCompositionDirectly(command);
  } else {
    // Commits original text of reverse conversion.
    CommitSourceTextDirectly(command);
  }
  // Passes the key event through to MozcService.java
  // to continue the processes which are invoked by cancel operation.
  command->mutable_output()->set_consumed(false);

  return true;
}

bool Session::EditCancel(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "edit_cancel_after_direct_commit_learning");
  DiscardPendingZenzFeedback("edit_cancel_after_pending_feedback");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (EditCancelOnPasswordField(command)) {
    return true;
  }

  command->mutable_output()->set_consumed(true);

  TryCancelConvertReverse(command);

  SetStateToPredompositionAndCancel(context_.get());
  ClearZenzConversionState();
  Output(command);
  return true;
}

bool Session::EditCancelAndIMEOff(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "edit_cancel_and_ime_off_after_direct_commit_learning");
  DiscardPendingZenzFeedback("edit_cancel_and_ime_off_after_pending_feedback");

  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  if (EditCancelOnPasswordField(command)) {
    return true;
  }

  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::COMPOSITION |
         ImeContext::CONVERSION))) {
    return DoNothing(command);
  }

  command->mutable_output()->set_consumed(true);

  TryCancelConvertReverse(command);

  ClearUndoContext();

  // Reset the context.
  context_->mutable_converter()->Reset();

  SetSessionState(ImeContext::DIRECT, context_.get());
  ClearZenzConversionState();
  Output(command);
  return true;
}

bool Session::CommitInternal(commands::Command* command,
                             bool trigger_zero_query_suggest) {
  if (!(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  PushUndoContext();

  if (context_->state() == ImeContext::COMPOSITION) {
    const bool mark_preedit_as_reranked =
        ShouldMarkPreeditCommitAsRerankedAfterConvertCancel();
    std::vector<std::string> segment_keys;
    if (mark_preedit_as_reranked) {
      segment_keys = pending_reranked_preedit_commit_segment_keys_;
    }
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    context_->mutable_converter()->CommitPreedit(
        context_->composer(), command->input().context(),
        mark_preedit_as_reranked, segment_keys);
  } else {  // ImeContext::CONVERSION
    ClearPendingRerankedPreeditCommitAfterConvertCancel();
    context_->mutable_converter()->Commit(context_->composer(),
                                          command->input().context());
  }

  SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

  if (trigger_zero_query_suggest) {
    Suggest(command->input());
  }

  Output(command);
  // Copy the previous output for Undo.
  *context_->mutable_output() = command->output();

  ClearZenzConversionState();
  return true;
}

bool Session::Commit(commands::Command* command) {
  if (zenz_suggestion_selected_ && !zenz_suggestion_visible_key_.empty()) {
    return CommitZenzSuggestion(command);
  }
  if (CommitZenzConversionResult(command)) {
    return true;
  }

  return CommitInternal(command,
                        context_->GetRequest().zero_query_suggestion());
}

bool Session::CommitNotTriggeringZeroQuerySuggest(commands::Command* command) {
  return CommitInternal(command, false);
}

bool Session::CommitHead(size_t count, commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::PRECOMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  // TODO(yamaguchi): Support undo feature.
  ClearUndoContext();

  size_t committed_size;
  context_->mutable_converter()->CommitHead(count, context_->composer(),
                                            &committed_size);
  context_->mutable_composer()->DeleteRange(0, committed_size);
  Output(command);
  return true;
}

bool Session::CommitFirstSuggestion(commands::Command* command) {
  if (!(context_->state() == ImeContext::COMPOSITION ||
        context_->state() == ImeContext::PRECOMPOSITION)) {
    return DoNothing(command);
  }
  if (!zenz_suggestion_visible_key_.empty() &&
      zenz_suggestion_visible_key_ ==
          context_->composer().GetQueryForConversion()) {
    return CommitZenzSuggestion(command);
  }
  if (!context_->converter().IsActive()) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  PushUndoContext();

  constexpr int kFirstIndex = 0;
  size_t committed_key_size = 0;
  context_->mutable_converter()->CommitSuggestionByIndex(
      kFirstIndex, context_->composer(), command->input().context(),
      &committed_key_size);

  SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

  // Get suggestion if zero_query_suggestion is set.
  // zero_query_suggestion is usually set where the client is a mobile.
  if (context_->GetRequest().zero_query_suggestion()) {
    Suggest(command->input());
  }

  Output(command);
  // Copy the previous output for Undo.
  *context_->mutable_output() = command->output();
  return true;
}

bool Session::CommitSegment(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  PushUndoContext();

  size_t size;
  context_->mutable_converter()->CommitFirstSegment(
      context_->composer(), command->input().context(), &size);
  if (size > 0) {
    // Delete the key characters of the first segment from the preedit.
    context_->mutable_composer()->DeleteRange(0, size);
    // The number of segments should be more than one.
    DCHECK_GT(context_->composer().GetLength(), 0);
  }

  if (!context_->converter().IsActive()) {
    // If the converter is not active (ie. the segment size was one.),
    // the state should be switched to precomposition.
    SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

    // Get suggestion if zero_query_suggestion is set.
    // zero_query_suggestion is usually set where the client is a mobile.
    if (context_->GetRequest().zero_query_suggestion()) {
      Suggest(command->input());
    }
  }
  Output(command);
  // Copy the previous output for Undo.
  *context_->mutable_output() = command->output();
  return true;
}

void Session::CommitHeadToFocusedSegmentsInternal(
    const commands::Context& context) {
  size_t size;
  context_->mutable_converter()->CommitHeadToFocusedSegments(
      context_->composer(), context, &size);
  if (size > 0) {
    // Delete the key characters of the first segment from the preedit.
    context_->mutable_composer()->DeleteRange(0, size);
    // The number of segments should be more than one.
    DCHECK_GT(context_->composer().GetLength(), 0);
  }
}

void Session::CommitCompositionDirectly(commands::Command* command) {
  const std::string composition = context_->composer().GetQueryForConversion();
  const std::string conversion = context_->composer().GetStringForSubmission();
  CommitStringDirectly(composition, conversion, command);
}

void Session::CommitSourceTextDirectly(commands::Command* command) {
  // We cannot use a reference since composer will be cleared on
  // CommitStringDirectly.
  absl::string_view copied_source_text = context_->composer().source_text();
  CommitStringDirectly(copied_source_text, copied_source_text, command);
}

void Session::CommitRawTextDirectly(commands::Command* command) {
  const std::string raw_text = context_->composer().GetRawString();
  CommitStringDirectly(raw_text, raw_text, command);
}

void Session::CommitStringDirectly(absl::string_view key,
                                   absl::string_view preedit,
                                   commands::Command* command) {
  if (key.empty() || preedit.empty()) {
    return;
  }

  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->Reset();

  commands::Result* result = command->mutable_output()->mutable_result();
  DCHECK(result != nullptr);
  result->set_type(commands::Result::STRING);
  result->mutable_key()->append(key);
  result->mutable_value()->append(preedit);
  SetSessionState(ImeContext::PRECOMPOSITION, context_.get());

  // Get suggestion if zero_query_suggestion is set.
  // zero_query_suggestion is usually set where the client is a mobile.
  if (context_->GetRequest().zero_query_suggestion()) {
    Suggest(command->input());
  }

  Output(command);
}

namespace {
bool SuppressSuggestion(const commands::Input& input) {
  if (!input.has_context()) {
    return false;
  }
  if (input.context().has_suppress_suggestion() &&
      input.context().suppress_suggestion()) {
    return true;
  }
  // If the target input field is in Chrome's Omnibox or Google
  // search box, the suggest window is hidden.
  for (size_t i = 0; i < input.context().experimental_features_size(); ++i) {
    const std::string& feature = input.context().experimental_features(i);
    if (feature == "chrome_omnibox" || feature == "google_search_box") {
      return true;
    }
  }
  return false;
}

bool IsAsciiUpperAlpha(const char c) {
  return 'A' <= c && c <= 'Z';
}

bool IsAsciiLowerAlpha(const char c) {
  return 'a' <= c && c <= 'z';
}

bool LooksLikeShiftedAsciiRomanizedSuggestionContext(
    const composer::Composer& composer) {
  const transliteration::TransliterationType input_mode =
      composer.GetInputMode();
  if (input_mode == transliteration::HALF_ASCII ||
      input_mode == transliteration::FULL_ASCII) {
    return false;
  }

  const std::string raw = composer.GetRawString();
  if (raw.size() < 3 || !IsAsciiUpperAlpha(raw[0]) ||
      !IsAsciiUpperAlpha(raw[1])) {
    return false;
  }

  bool has_lower_alpha = false;
  for (const char c : raw) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      return false;
    }
    if (IsAsciiLowerAlpha(c)) {
      has_lower_alpha = true;
    }
  }
  if (!has_lower_alpha) {
    return false;
  }

  // Example:
  //   raw     = "AIde"
  //   preedit = "AI + Japanese text"
  //
  // This is the ambiguity caused by the shifted ASCII sequence being reverted
  // to Japanese input.  When dictionary suggest is disabled, do not surface the
  // raw ASCII rescue candidate automatically.
  return raw != composer.GetStringForPreedit();
}

bool ShouldSuppressShiftedAsciiAutoSuggestion(
    const config::Config& config,
    const composer::Composer& composer) {
  if (config.use_dictionary_suggest()) {
    return false;
  }
  return composer.is_in_shifted_ascii_revert_context() ||
         LooksLikeShiftedAsciiRomanizedSuggestionContext(composer);
}
}  // namespace

bool Session::Suggest(const commands::Input& input) {
  ++zenz_suggestion_generation_;
  pending_zenz_suggestion_ = PendingZenzSuggestion();
  zenz_suggestion_visible_key_.clear();
  zenz_suggestion_visible_value_.clear();
  zenz_suggestion_visible_context_class_.clear();
  zenz_suggestion_selected_ = false;

  if (SuppressSuggestion(input)) {
    return false;
  }

  if (ShouldSuppressShiftedAsciiAutoSuggestion(context_->GetConfig(),
                                               context_->composer())) {
    return false;
  }

  // |request_suggestion| is not supposed to always ensure suppressing
  // suggestion since this field is used for performance improvement
  // by skipping interim suggestions.  However, the implementation of
  // EngineConverter::SuggestWithPreferences does not perform suggest
  // whenever this flag is on.  So the caller should consider whether
  // this flag should be set or not.  Because the original logic was
  // implemented in Session::InserCharacter, we check the input.type()
  // is SEND_KEY assuming SEND_KEY results InsertCharacter (in most
  // cases).
  //
  // TODO(komatsu): Move the logic into EngineConverter.
  bool suggested = false;
  if (input.has_request_suggestion() &&
      input.type() == commands::Input::SEND_KEY) {
    ConversionPreferences conversion_preferences =
        context_->converter().conversion_preferences();
    conversion_preferences.request_suggestion = input.request_suggestion();
    suggested = context_->mutable_converter()->SuggestWithPreferences(
        context_->composer(), input.context(), conversion_preferences);
  } else {
    suggested = context_->mutable_converter()->Suggest(context_->composer(),
                                                       input.context());
  }

  if (suggested) {
    MaybeScheduleZenzSuggestion();
  }
  return suggested;
}

bool Session::CommitZenzSuggestion(commands::Command* command) {
  if (zenz_suggestion_visible_key_.empty() ||
      zenz_suggestion_visible_key_ !=
          context_->composer().GetQueryForConversion() ||
      !(context_->state() &
        (ImeContext::COMPOSITION | ImeContext::PRECOMPOSITION))) {
    return DoNothing(command);
  }
  const std::string key = zenz_suggestion_visible_key_;
  const std::string value = zenz_suggestion_visible_value_;
  const std::string context_class = zenz_suggestion_visible_context_class_;
  command->mutable_output()->set_consumed(true);
  PushUndoContext();
  SetPendingZenzFeedbackAccepted(key, context_class, value);
  ++zenz_suggestion_generation_;
  pending_zenz_suggestion_ = PendingZenzSuggestion();
  zenz_suggestion_visible_key_.clear();
  zenz_suggestion_visible_value_.clear();
  zenz_suggestion_visible_context_class_.clear();
  zenz_suggestion_selected_ = false;
  CommitStringDirectly(key, value, command);
  return true;
}



bool Session::ConvertToTransliteration(
    commands::Command* command,
    const transliteration::TransliterationType type) {
  if (!(context_->state() &
        (ImeContext::CONVERSION | ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  if (!context_->mutable_converter()->ConvertToTransliteration(
          context_->composer(), type)) {
    return false;
  }
  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::ConvertToHiragana(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::HIRAGANA);
}

bool Session::ConvertToFullKatakana(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::FULL_KATAKANA);
}

bool Session::ConvertToHalfKatakana(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::HALF_KATAKANA);
}

bool Session::ConvertToFullASCII(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::FULL_ASCII);
}

bool Session::ConvertToHalfASCII(commands::Command* command) {
  return ConvertToTransliteration(command, transliteration::HALF_ASCII);
}

bool Session::SwitchKanaType(commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::CONVERSION | ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  if (!context_->mutable_converter()->SwitchKanaType(context_->composer())) {
    return false;
  }
  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::DisplayAsHiragana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToHiragana(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(transliteration::HIRAGANA);
    OutputComposition(command);
    return true;
  }
}

bool Session::DisplayAsFullKatakana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToFullKatakana(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(transliteration::FULL_KATAKANA);
    OutputComposition(command);
    return true;
  }
}

bool Session::DisplayAsHalfKatakana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToHalfKatakana(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(transliteration::HALF_KATAKANA);
    OutputComposition(command);
    return true;
  }
}

bool Session::TranslateFullASCII(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToFullASCII(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(
        transliteration::T13n::ToggleFullAsciiTypes(
            context_->composer().GetOutputMode()));
    OutputComposition(command);
    return true;
  }
}

bool Session::TranslateHalfASCII(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToHalfASCII(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    context_->mutable_composer()->SetOutputMode(
        transliteration::T13n::ToggleHalfAsciiTypes(
            context_->composer().GetOutputMode()));
    OutputComposition(command);
    return true;
  }
}

bool Session::CompositionModeHiragana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::HIRAGANA, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeFullKatakana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::FULL_KATAKANA, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeHalfKatakana(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::HALF_KATAKANA, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeFullASCII(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::FULL_ASCII, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeHalfASCII(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  EnsureIMEIsOn();
  // The temporary mode should not be overridden.
  SwitchInputMode(transliteration::HALF_ASCII, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::CompositionModeSwitchKanaType(commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION) {
    return DoNothing(command);
  }

  command->mutable_output()->set_consumed(true);

  transliteration::TransliterationType current_type =
      context_->composer().GetInputMode();
  transliteration::TransliterationType next_type;

  switch (current_type) {
    case transliteration::HIRAGANA:
      next_type = transliteration::FULL_KATAKANA;
      break;

    case transliteration::FULL_KATAKANA:
      next_type = transliteration::HALF_KATAKANA;
      break;

    case transliteration::HALF_KATAKANA:
      next_type = transliteration::HIRAGANA;
      break;

    case transliteration::HALF_ASCII:
    case transliteration::FULL_ASCII:
      next_type = current_type;
      break;

    default:
      LOG(ERROR) << "Unknown input mode: " << current_type;
      // don't change input mode
      next_type = current_type;
      break;
  }

  // The temporary mode should not be overridden.
  SwitchInputMode(next_type, context_->mutable_composer());
  OutputFromState(command);
  return true;
}

bool Session::ConvertToHalfWidth(commands::Command* command) {
  if (!(context_->state() &
        (ImeContext::CONVERSION | ImeContext::COMPOSITION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);

  if (!context_->mutable_converter()->ConvertToHalfWidth(
          context_->composer())) {
    return false;
  }
  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::TranslateHalfWidth(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertToHalfWidth(command);
  } else {  // context_->state() == ImeContext::COMPOSITION
    const transliteration::TransliterationType type =
        context_->composer().GetOutputMode();
    if (type == transliteration::HIRAGANA ||
        type == transliteration::FULL_KATAKANA ||
        type == transliteration::HALF_KATAKANA) {
      context_->mutable_composer()->SetOutputMode(
          transliteration::HALF_KATAKANA);
    } else if (type == transliteration::FULL_ASCII) {
      context_->mutable_composer()->SetOutputMode(transliteration::HALF_ASCII);
    } else if (type == transliteration::FULL_ASCII_UPPER) {
      context_->mutable_composer()->SetOutputMode(
          transliteration::HALF_ASCII_UPPER);
    } else if (type == transliteration::FULL_ASCII_LOWER) {
      context_->mutable_composer()->SetOutputMode(
          transliteration::HALF_ASCII_LOWER);
    } else if (type == transliteration::FULL_ASCII_CAPITALIZED) {
      context_->mutable_composer()->SetOutputMode(
          transliteration::HALF_ASCII_CAPITALIZED);
    } else {
      // transliteration::HALF_ASCII_something
      return TranslateHalfASCII(command);
    }
    OutputComposition(command);
    return true;
  }
}

bool Session::LaunchConfigDialog(commands::Command* command) {
  command->mutable_output()->set_launch_tool_mode(
      commands::Output::CONFIG_DIALOG);
  return DoNothing(command);
}

bool Session::LaunchDictionaryTool(commands::Command* command) {
  command->mutable_output()->set_launch_tool_mode(
      commands::Output::DICTIONARY_TOOL);
  return DoNothing(command);
}

bool Session::LaunchWordRegisterDialog(commands::Command* command) {
  command->mutable_output()->set_launch_tool_mode(
      commands::Output::WORD_REGISTER_DIALOG);
  return DoNothing(command);
}

bool Session::UndoOrRewind(commands::Command* command) {
  // Undo is prioritized over rewind otherwise the undo operation for
  // partial commit doesn't work (rewind always consumes the event).
  if (HasUndoContext()) {
    return Undo(command);
  }

  // Rewind if the state is in composition.
  if (!(context_->state() & ImeContext::COMPOSITION)) {
    // Mozc decoder doesn't do anything for UNDO_OR_REWIND.
    // Echo back the event to the client to give it a chance to delegate
    // undo operation to the app.
    return EchoBack(command);
  }

  command->mutable_output()->set_consumed(true);
  context_->mutable_composer()->InsertCommandCharacter(
      composer::Composer::REWIND);
  ClearUndoContext();

  // InsertCommandCharacter method updates the preedit text
  // so we need to update suggest candidates.
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::StopKeyToggling(commands::Command* command) {
  if (!(context_->state() & ImeContext::COMPOSITION)) {
    return DoNothing(command);
  }

  command->mutable_output()->set_consumed(true);
  context_->mutable_composer()->InsertCommandCharacter(
      composer::Composer::STOP_KEY_TOGGLING);
  ClearUndoContext();

  // Since the output should not be changed on STOP_KEY_TOGGLING,
  // The last output is used instead of calling the converter operations.
  Output(command);
  return true;
}

bool Session::ToggleAlphanumericMode(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  context_->mutable_composer()->ToggleInputMode();

  OutputFromState(command);
  return true;
}

bool Session::DeleteCandidateFromHistory(commands::Command* command) {
  std::optional<int> id = std::nullopt;
  if (command->input().has_command() && command->input().command().has_id()) {
    id = command->input().command().id();
  }
  if (!context_->mutable_converter()->DeleteCandidateFromHistory(id)) {
    return DoNothing(command);
  }
  return ConvertCancel(command);
}

bool Session::Convert(commands::Command* command) {
  return ConvertInternal(command, /*run_zenz=*/true);
}

bool Session::ConvertInternal(commands::Command* command, bool run_zenz) {
  CancelPendingZenzConversion();
  ++zenz_suggestion_generation_;
  pending_zenz_suggestion_ = PendingZenzSuggestion();
  zenz_suggestion_visible_key_.clear();
  zenz_suggestion_visible_value_.clear();
  zenz_suggestion_visible_context_class_.clear();
  zenz_suggestion_selected_ = false;
  command->mutable_output()->set_consumed(true);
  const std::string composition = context_->composer().GetQueryForConversion();
  const bool should_show_candidate_window_on_initial_conversion =
      context_->state() == ImeContext::COMPOSITION &&
      context_->GetConfig().show_candidate_window_on_initial_conversion();

  if (context_->state() == ImeContext::COMPOSITION &&
      (context_->composer().GetInputMode() == transliteration::HALF_ASCII ||
       context_->composer().GetInputMode() == transliteration::FULL_ASCII) &&
      command->input().has_key() &&
      command->input().key().has_special_key() &&
      command->input().key().special_key() == commands::KeyEvent::SPACE) {
    if (!composition.ends_with(' ') ||
        context_->composer().GetLength() != context_->composer().GetCursor()) {
      if (context_->GetRequest().space_on_alphanumeric() ==
          commands::Request::COMMIT) {
        context_->mutable_composer()->InsertCharacterPreedit(" ");
        return Commit(command);
      }
      command->mutable_input()->mutable_key()->set_key_code(' ');
      return InsertCharacter(command);
    }
    if (!composition.empty()) {
      DCHECK_EQ(' ', composition[composition.size() - 1]);
      context_->mutable_composer()->Backspace();
      ClearUndoContext();
    }
  }

  if (!context_->mutable_converter()->Convert(context_->composer())) {
    LOG(ERROR) << "Conversion failed for some reasons.";
    OutputComposition(command);
    return true;
  }
  SetSessionState(ImeContext::CONVERSION, context_.get());
  if (should_show_candidate_window_on_initial_conversion) {
    context_->mutable_converter()->SetCandidateListVisible(true);
  }
  Output(command);
  if (run_zenz && MaybeApplyZenzFeedbackConversion(command)) {
    return true;
  }
  if (run_zenz && MaybeScheduleZenzConversion(command)) {
    return true;
  }
  return true;
}

bool Session::ConvertWithoutHistory(commands::Command* command) {
  CancelPendingZenzConversion();
  command->mutable_output()->set_consumed(true);

  ConversionPreferences preferences =
      context_->converter().conversion_preferences();
  preferences.use_history = false;
  if (!context_->mutable_converter()->ConvertWithPreferences(
          context_->composer(), preferences)) {
    LOG(ERROR) << "Conversion failed for some reasons.";
    OutputComposition(command);
    return true;
  }

  SetSessionState(ImeContext::CONVERSION, context_.get());
  Output(command);
  return true;
}

bool Session::CommitIfPassword(commands::Command* command) {
  if (context_->composer().GetInputFieldType() == commands::Context::PASSWORD) {
    CommitCompositionDirectly(command);
    return true;
  }
  return false;
}

bool Session::MoveCursorRight(commands::Command* command) {
  // In future, we may want to change the strategy of committing, to support
  // more flexible behavior.
  // - If the composing text has some "pending toggling character(s) at the
  //   end", we'd like to "fix" the toggling state, but not to commit.
  // - Otherwise (i.e. if there is no such character(s)), we'd like to commit
  //   (considering the use cases, probably we'd like to apply it only for
  //   alphabet mode).
  // Before supporting it, we'll need to support auto fixing by waiting
  // a period. Also, it is necessary to support displaying the current toggling
  // state (otherwise, users would be confused).
  // So, to keep users out from such confusion, we only commit if the current
  // composing mode doesn't has toggling state. Clients has the responsibility
  // to check if the keyboard has toggling state or not. Note that the server
  // should know the current table has toggling state or not. However,
  // a client may NOT want to auto committing even if the composition mode
  // doesn't have the toggling state, so the server just relies on the flag
  // passed from the client.
  // TODO(hidehiko): Support it, when it is prioritized.
  if (context_->GetRequest().crossing_edge_behavior() ==
          commands::Request::COMMIT_WITHOUT_CONSUMING &&
      context_->composer().GetLength() == context_->composer().GetCursor()) {
    Commit(command);

    // Do not consume.
    command->mutable_output()->set_consumed(false);
    return true;
  }

  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorRight();
  ClearUndoContext();
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::MoveCursorLeft(commands::Command* command) {
  if (context_->GetRequest().crossing_edge_behavior() ==
          commands::Request::COMMIT_WITHOUT_CONSUMING &&
      context_->composer().GetCursor() == 0) {
    CommitNotTriggeringZeroQuerySuggest(command);

    // Move the cursor to the beginning of the values.
    command->mutable_output()->mutable_result()->set_cursor_offset(
        -static_cast<int32_t>(
            Util::CharsLen(command->output().result().value())));

    // Do not consume.
    command->mutable_output()->set_consumed(false);
    return true;
  }

  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorLeft();
  ClearUndoContext();
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::MoveCursorToEnd(commands::Command* command) {
  return MoveCursorToEndInternal(command, true);
}

bool Session::MoveCursorToEndInternal(commands::Command* command,
                                      bool clear_undo) {
  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorToEnd();
  if (clear_undo) {
    ClearUndoContext();
  }
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::MoveCursorTo(commands::Command* command) {
  // This method moves the cursor *inside* the composition text.
  // Therefore on PRECOMPOSITION state, where there is no composition text,
  // this method shouldn't consume the event but send back it to the client.
  if (context_->state() == ImeContext::PRECOMPOSITION) {
    return EchoBack(command);
  }
  if (context_->state() != ImeContext::COMPOSITION) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorTo(
      command->input().command().cursor_position());
  ClearUndoContext();
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::MoveCursorToBeginning(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  if (CommitIfPassword(command)) {
    return true;
  }
  context_->mutable_composer()->MoveCursorToBeginning();
  ClearUndoContext();
  if (Suggest(command->input())) {
    Output(command);
    return true;
  }
  OutputComposition(command);
  return true;
}

bool Session::Delete(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "delete_after_direct_commit_learning");
  DiscardPendingZenzFeedback("delete_after_pending_feedback");
  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  command->mutable_output()->set_consumed(true);
  CancelPendingZenzConversion();
  ClearZenzConversionState();
  context_->mutable_composer()->Delete();
  ClearUndoContext();
  if (context_->mutable_composer()->Empty()) {
    SetStateToPredompositionAndCancel(context_.get());
    Output(command);
  } else if (Suggest(command->input())) {
    Output(command);
  } else {
    OutputComposition(command);
  }
  return true;
}

bool Session::Backspace(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "backspace_after_direct_commit_learning");
  DiscardPendingZenzFeedback("backspace_after_pending_feedback");
  ClearPendingRerankedPreeditCommitAfterConvertCancel();

  command->mutable_output()->set_consumed(true);
  CancelPendingZenzConversion();
  ClearZenzConversionState();
  context_->mutable_composer()->Backspace();
  ClearUndoContext();
  if (context_->mutable_composer()->Empty()) {
    SetStateToPredompositionAndCancel(context_.get());
    Output(command);
  } else if (Suggest(command->input())) {
    Output(command);
  } else {
    OutputComposition(command);
  }
  return true;
}

bool Session::SegmentFocusRight(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentFocusRight();
  Output(command);
  return true;
}

bool Session::SegmentFocusLast(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentFocusLast();
  Output(command);
  return true;
}

bool Session::SegmentFocusLeft(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentFocusLeft();
  Output(command);
  return true;
}

bool Session::SegmentFocusLeftEdge(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentFocusLeftEdge();
  Output(command);
  return true;
}

bool Session::SegmentWidthExpand(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentWidthExpand(context_->composer());
  Output(command);
  return true;
}

bool Session::SegmentWidthShrink(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->SegmentWidthShrink(context_->composer());
  Output(command);
  return true;
}

bool Session::ReportBug(commands::Command* command) {
  return DoNothing(command);
}

bool Session::ConvertNext(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->CandidateNext(context_->composer());
  Output(command);
  return true;
}

bool Session::ConvertNextPage(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->CandidateNextPage();
  Output(command);
  return true;
}

bool Session::ConvertPrev(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->CandidatePrev();
  Output(command);
  return true;
}

bool Session::ConvertPrevPage(commands::Command* command) {
  if (!(context_->state() & (ImeContext::CONVERSION))) {
    return DoNothing(command);
  }
  command->mutable_output()->set_consumed(true);
  context_->mutable_converter()->CandidatePrevPage();
  Output(command);
  return true;
}

bool Session::ConvertCancel(commands::Command* command) {
  DiscardPendingDirectCommitLearning(
      "convert_cancel_after_direct_commit_learning");
  DiscardPendingZenzFeedback("convert_cancel_after_pending_feedback");

  command->mutable_output()->set_consumed(true);

  MaybeSetPendingRerankedPreeditCommitAfterConvertCancel();
  ClearZenzConversionState();

  SetSessionState(ImeContext::COMPOSITION, context_.get());
  context_->mutable_converter()->Cancel();
  if (Suggest(command->input())) {
    Output(command);
  } else {
    OutputComposition(command);
  }
  return true;
}


bool Session::PredictAndConvert(commands::Command* command) {
  CancelPendingZenzConversion();

  if (context_->state() == ImeContext::CONVERSION) {
    return ConvertNext(command);
  }

  command->mutable_output()->set_consumed(true);
  if (context_->mutable_converter()->Predict(context_->composer())) {
    SetSessionState(ImeContext::CONVERSION, context_.get());
    Output(command);
  } else {
    OutputComposition(command);
  }
  return true;
}

void Session::OutputFromState(commands::Command* command) {
  if (context_->state() == ImeContext::DIRECT) {
    OutputMode(command);
    return;
  }
  Output(command);
}

namespace {

void AddZenzSuggestionCandidate(commands::Output* output,
                               absl::string_view key,
                               absl::string_view value,
                               bool focused) {
  if (output->has_all_candidate_words()) {
    const commands::CandidateList original = output->all_candidate_words();
    commands::CandidateList* candidates = output->mutable_all_candidate_words();
    *candidates = original;
    candidates->clear_candidates();
    if (focused) {
      candidates->set_focused_index(0);
    } else if (original.has_focused_index()) {
      candidates->set_focused_index(original.focused_index() + 1);
    }
    commands::CandidateWord* zenz_candidate = candidates->add_candidates();
    zenz_candidate->set_id(kZenzSuggestionCandidateId);
    zenz_candidate->set_index(0);
    zenz_candidate->set_key(std::string(key));
    zenz_candidate->set_value(std::string(value));
    zenz_candidate->set_num_segments_in_candidate(1);
    for (const commands::CandidateWord& candidate : original.candidates()) {
      commands::CandidateWord* copy = candidates->add_candidates();
      *copy = candidate;
      copy->set_index(candidate.index() + 1);
    }
  }

  if (output->has_candidate_window()) {
    const commands::CandidateWindow original = output->candidate_window();
    commands::CandidateWindow* window = output->mutable_candidate_window();
    *window = original;
    window->clear_candidate();
    window->set_size(original.size() + 1);
    if (focused) {
      window->set_focused_index(0);
    } else if (original.has_focused_index()) {
      window->set_focused_index(original.focused_index() + 1);
    }
    commands::CandidateWindow::Candidate* zenz_candidate =
        window->add_candidate();
    zenz_candidate->set_id(kZenzSuggestionCandidateId);
    zenz_candidate->set_index(0);
    zenz_candidate->set_value(std::string(value));
    for (int i = 0; i < original.candidate_size(); ++i) {
      commands::CandidateWindow::Candidate* copy = window->add_candidate();
      *copy = original.candidate(i);
      copy->set_index(original.candidate(i).index() + 1);
    }
  }
}

}  // namespace

void Session::Output(commands::Command* command) {
  OutputMode(command);
  context_->mutable_converter()->PopOutput(context_->composer(),
                                           command->mutable_output());
  if (pending_zenz_suggestion_.pending &&
      context_->GetConfig().use_zenz_conversion() &&
      (context_->state() &
       (ImeContext::COMPOSITION | ImeContext::PRECOMPOSITION)) &&
      pending_zenz_suggestion_.key ==
          context_->composer().GetQueryForConversion()) {
    AttachZenzSuggestionPollCallback(command);
  } else if (pending_zenz_suggestion_.pending) {
    ++zenz_suggestion_generation_;
    pending_zenz_suggestion_ = PendingZenzSuggestion();
  }
  if (!zenz_suggestion_visible_key_.empty() &&
      zenz_suggestion_visible_key_ ==
          context_->composer().GetQueryForConversion() &&
      (context_->state() &
       (ImeContext::COMPOSITION | ImeContext::PRECOMPOSITION))) {
    AddZenzSuggestionCandidate(command->mutable_output(),
                               zenz_suggestion_visible_key_,
                               zenz_suggestion_visible_value_,
                               zenz_suggestion_selected_);
  }
  ObservePendingZenzFeedbackCommittedResult(*command, "output_result");
}

void Session::OutputMode(commands::Command* command) const {
  const commands::CompositionMode mode =
      ToCompositionMode(context_->composer().GetInputMode());
  const commands::CompositionMode comeback_mode =
      ToCompositionMode(context_->composer().GetComebackInputMode());

  commands::Output* output = command->mutable_output();
  commands::Status* status = output->mutable_status();
  if (context_->state() == ImeContext::DIRECT) {
    output->set_mode(commands::DIRECT);
    status->set_activated(false);
  } else {
    output->set_mode(mode);
    status->set_activated(true);
  }
  status->set_mode(mode);
  status->set_comeback_mode(comeback_mode);
}

void Session::OutputComposition(commands::Command* command) const {
  OutputMode(command);
  context_->converter().FillPreedit(
      context_->composer(), command->mutable_output()->mutable_preedit());
}

void Session::OutputKey(commands::Command* command) const {
  OutputMode(command);
  commands::KeyEvent* key = command->mutable_output()->mutable_key();
  *key = command->input().key();
}

namespace {

bool MatchesKeyEvent(const commands::KeyEvent& key_event,
                     const uint32_t key_code,
                     std::initializer_list<absl::string_view> key_strings) {
  if (key_event.key_code() == key_code && key_event.key_string().empty()) {
    return true;
  }

  for (const absl::string_view s : key_strings) {
    if (key_event.key_string() == s) {
      return true;
    }
  }
  return false;
}

bool MatchesString(absl::string_view value,
                   std::initializer_list<absl::string_view> candidates) {
  for (const absl::string_view s : candidates) {
    if (value == s) {
      return true;
    }
  }
  return false;
}

bool SymbolMethodUsesMiddleDot(const config::Config& config) {
  return config.symbol_method() == config::Config::CORNER_BRACKET_MIDDLE_DOT ||
         config.symbol_method() == config::Config::SQUARE_BRACKET_MIDDLE_DOT;
}

std::string DirectCommitFallbackSuffixFromKeyEvent(
    const config::Config& config, const commands::KeyEvent& key_event) {
  if (MatchesString(key_event.key_string(),
                    {"。", "｡", "．", "、", "､", "，", "？", "！",
                     "（", "）", "「", "」", "［", "］", "・", "･"})) {
    return key_event.key_string();
  }

  if (MatchesString(key_event.key_string(), {".", "．"})) {
    return "。";
  }
  if (MatchesString(key_event.key_string(), {",", "，"})) {
    return "、";
  }
  if (key_event.key_string() == "?") {
    return "？";
  }
  if (key_event.key_string() == "!") {
    return "！";
  }
  if (key_event.key_string() == "(") {
    return "（";
  }
  if (key_event.key_string() == ")") {
    return "）";
  }
  if (MatchesString(key_event.key_string(), {"[", "［"})) {
    return "「";
  }
  if (MatchesString(key_event.key_string(), {"]", "］"})) {
    return "」";
  }
  if (SymbolMethodUsesMiddleDot(config) &&
      MatchesString(key_event.key_string(), {"/", "／"})) {
    return "・";
  }

  switch (key_event.key_code()) {
    case static_cast<uint32_t>('.'):
      return "。";
    case static_cast<uint32_t>(','):
      return "、";
    case static_cast<uint32_t>('?'):
      return "？";
    case static_cast<uint32_t>('!'):
      return "！";
    case static_cast<uint32_t>('('):
      return "（";
    case static_cast<uint32_t>(')'):
      return "）";
    case static_cast<uint32_t>('['):
      return "「";
    case static_cast<uint32_t>(']'):
      return "」";
    case static_cast<uint32_t>('/'):
      if (SymbolMethodUsesMiddleDot(config)) {
        return "・";
      }
      break;
    default:
      break;
  }

  return "";
}

// Auto conversion helper.
// NOTE: This checks the last character in preedit, not key_event.key_string().
bool IsValidAutoConversionKey(const config::Config& config,
                              const uint32_t key_code,
                              absl::string_view last_char) {
  return (((key_code == static_cast<uint32_t>('.') && last_char.empty()) ||
           last_char == "." || last_char == "．" || last_char == "。" ||
           last_char == "｡") &&
          (config.auto_conversion_key() &
           config::Config::AUTO_CONVERSION_KUTEN)) ||
         (((key_code == static_cast<uint32_t>(',') && last_char.empty()) ||
           last_char == "," || last_char == "，" || last_char == "、" ||
           last_char == "､") &&
          (config.auto_conversion_key() &
           config::Config::AUTO_CONVERSION_TOUTEN)) ||
         (((key_code == static_cast<uint32_t>('?') && last_char.empty()) ||
           last_char == "?" || last_char == "？") &&
          (config.auto_conversion_key() &
           config::Config::AUTO_CONVERSION_QUESTION_MARK)) ||
         (((key_code == static_cast<uint32_t>('!') && last_char.empty()) ||
           last_char == "!" || last_char == "！") &&
          (config.auto_conversion_key() &
           config::Config::AUTO_CONVERSION_EXCLAMATION_MARK));
}

bool IsValidDirectCommitChar(const config::Config& config,
                             absl::string_view last_char) {
  return
      (MatchesString(last_char, {".", "．", "。", "｡"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_KUTEN)) ||

      (MatchesString(last_char, {",", "，", "、", "､"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_TOUTEN)) ||

      (MatchesString(last_char, {"?", "？"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_QUESTION_MARK)) ||

      (MatchesString(last_char, {"!", "！"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_EXCLAMATION_MARK)) ||

      (MatchesString(last_char, {"(", "（"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_OPEN_PARENTHESIS)) ||

      (MatchesString(last_char, {")", "）"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_CLOSE_PARENTHESIS)) ||

      (MatchesString(last_char, {"[", "［", "「"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_OPEN_BRACKET)) ||

      (MatchesString(last_char, {"]", "］", "」"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_CLOSE_BRACKET)) ||

      (MatchesString(last_char, {"・", "･"}) &&
       (config.direct_commit_key() &
        config::Config::DIRECT_COMMIT_MIDDLE_DOT));
}

}  // namespace

std::pair<std::string, std::string>
Session::GetDirectCommitStringsWithDirectCommitSuffixFallback(
    const composer::Composer& composer_before_insert,
    const commands::KeyEvent& key) const {
  const std::string key_before_insert =
      composer_before_insert.GetQueryForConversion();
  const std::string value_before_insert =
      composer_before_insert.GetStringForSubmission();

  std::string key_to_commit = context_->composer().GetQueryForConversion();
  std::string value_to_commit = context_->composer().GetStringForSubmission();

  const std::string suffix = DirectCommitFallbackSuffixFromKeyEvent(
      context_->GetConfig(), key);

  // Some test and client paths provide the direct-commit trigger as the key
  // event itself while leaving the restored preedit unchanged in Composer.
  // In that case, explicitly append the trigger suffix so that the visible
  // commit remains `きょう。`, while the strong learning target remains the
  // suffix-free `きょう` captured before insertion.
  if (value_to_commit == value_before_insert) {
    if (!suffix.empty()) {
      key_to_commit = absl::StrCat(key_before_insert, suffix);
      value_to_commit = absl::StrCat(value_before_insert, suffix);
    }
  }

  return {key_to_commit, value_to_commit};
}

bool Session::CanStartAutoConversion(
    const commands::KeyEvent& key_event) const {
  if (!context_->GetConfig().use_auto_conversion()) {
    return false;
  }

  // Disable if the input comes from non-standard user keyboards, like numpad.
  if (key_event.input_style() != commands::KeyEvent::FOLLOW_MODE) {
    return false;
  }

  // We simply disable the auto conversion feature if the mode is ASCII.
  // We conclude that disabling this feature is better in this situation.
  // TODO(taku): fix the behavior. Converter module needs to be fixed.
  if (key_event.mode() == commands::HALF_ASCII ||
      key_event.mode() == commands::FULL_ASCII) {
    return false;
  }

  // We should NOT check key_string.
  // http://b/issue?id=3217992
  // Auto conversion is not triggered if the composition is empty or
  // only one character, or the cursor is not in the end of the
  // composition.
  const size_t length = context_->composer().GetLength();
  if (length <= 1 || length != context_->composer().GetCursor()) {
    return false;
  }

  const uint32_t key_code = key_event.key_code();
  const std::string preedit = context_->composer().GetStringForPreedit();
  const absl::string_view last_char = Util::Utf8SubString(preedit, length - 1, 1);
  if (last_char.empty()) {
    return false;
  }

  if (!IsValidAutoConversionKey(context_->GetConfig(), key_code, last_char)) {
    return false;
  }

  // Check the previous character of last_character.
  // when |last_prev_char| is number, we don't invoke auto_conversion
  // if the same invoke key is repeated, do not conversion.
  // http://b/issue?id=2932118
  const absl::string_view last_prev_char =
      Util::Utf8SubString(preedit, length - 2, 1);
  if (last_prev_char.empty() || last_prev_char == last_char ||
      Util::NUMBER == Util::GetScriptType(last_prev_char)) {
    return false;
  }
  return true;
}


bool Session::CanDirectCommitAfterPunctuation(
    const commands::KeyEvent& key_event) const {
  const config::Config& config = context_->GetConfig();

  if (!config.use_direct_commit()) {
    return false;
  }

  // Mutual exclusion guard. Even if both are accidentally enabled in config,
  // direct commit is disabled here.
  if (config.use_auto_conversion()) {
    return false;
  }

  if (!(context_->state() &
        (ImeContext::PRECOMPOSITION | ImeContext::COMPOSITION))) {
    return false;
  }

  // Disable if the input comes from non-standard user keyboards, like numpad.
  if (key_event.input_style() != commands::KeyEvent::FOLLOW_MODE) {
    return false;
  }

  // Disable in ASCII mode.
  if (key_event.mode() == commands::HALF_ASCII ||
      key_event.mode() == commands::FULL_ASCII) {
    return false;
  }

  const size_t length = context_->composer().GetLength();
  if (length == 0 || length != context_->composer().GetCursor()) {
    return false;
  }

  const std::string preedit = context_->composer().GetStringForPreedit();
  const absl::string_view last_char =
      Util::Utf8SubString(preedit, length - 1, 1);
  if (last_char.empty()) {
    return false;
  }

  return IsValidDirectCommitChar(config, last_char);
}

void Session::UpdateTime() {
  context_->set_last_command_time(Clock::GetAbslTime());
}

void Session::TransformInput(commands::Input* input) {
  if (!input->has_key()) {
    return;
  }

  context_->key_event_transformer().TransformKeyEvent(input->mutable_key());

  if (!input->has_context() || !input->context().vertical_writing()) {
    return;
  }

  VerticalWritingKeyState vertical_key_state = VerticalWritingKeyState::kOther;
  if (context_->state() == ImeContext::COMPOSITION &&
      context_->converter().CheckState(EngineConverterInterface::SUGGESTION)) {
    vertical_key_state = VerticalWritingKeyState::kSuggestion;
  } else if (context_->state() == ImeContext::CONVERSION) {
    vertical_key_state =
        context_->converter().CheckState(EngineConverterInterface::PREDICTION)
            ? VerticalWritingKeyState::kPrediction
            : VerticalWritingKeyState::kConversion;
  }

  TransformVerticalWritingCandidateArrowKey(
      /*vertical_writing=*/true, vertical_key_state,
      context_->GetKeyMapManager(), input->mutable_key());
}

bool Session::SwitchInputFieldType(commands::Command* command) {
  command->mutable_output()->set_consumed(true);
  context_->mutable_composer()->SetInputFieldType(
      command->input().context().input_field_type());
  Output(command);
  return true;
}

bool Session::HandleIndirectImeOnOff(commands::Command* command) {
  const commands::KeyEvent& key = command->input().key();
  if (!key.has_activated()) {
    return true;
  }
  const ImeContext::State state = context_->state();
  if (state == ImeContext::DIRECT && key.activated()) {
    // Indirect IME On found.
    commands::Command on_command;
    on_command = *command;
    if (!IMEOn(&on_command)) {
      return false;
    }
  } else if (state != ImeContext::DIRECT && !key.activated()) {
    // Indirect IME Off found.
    commands::Command off_command;
    off_command = *command;
    if (!IMEOff(&off_command)) {
      return false;
    }
  }
  return true;
}

bool Session::ImeAction(commands::Command* command) {
  if (context_->state() != ImeContext::PRECOMPOSITION ||
      context_->composer().GetInputFieldType() != commands::Context::NORMAL) {
    return false;
  }

  // ImeAction is triggered when the mobile-specific IME action buttons such as
  // Search, Go, or Next, are tapped. After a user finishes an IME session, they
  // might still perform manual edits using the Backspace key. To sync these
  // edits with the converter, we call CommitContext. Typical use case is the
  // partial-revert on user history training.
  context_->mutable_converter()->CommitContext(context_->composer(),
                                               command->input().context());

  return true;
}

bool Session::CommitRawText(commands::Command* command) {
  if (context_->composer().GetLength() == 0) {
    return false;
  }
  CommitRawTextDirectly(command);
  return true;
}

// TODO(komatsu): delete this function.
composer::Composer* Session::get_internal_composer_only_for_unittest() {
  return context_->mutable_composer();
}

const ImeContext& Session::context() const { return *context_; }

}  // namespace session
}  // namespace mozc
