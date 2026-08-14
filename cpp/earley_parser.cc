/*!
 *  Copyright (c) 2025 by Contributors
 * \file xgrammar/earley_parser.cc
 */

#include "earley_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <utility>
#include <vector>

#include "fsm.h"
#include "fsm_builder.h"
#include "grammar_impl.h"
#include "json_schema_converter.h"
#include "support/encoding.h"
#include "support/logging.h"
#include "xgrammar/grammar.h"

namespace xgrammar {

using GrammarExprType = Grammar::Impl::GrammarExprType;

using GrammarExpr = Grammar::Impl::GrammarExpr;

namespace {

using JSONNumberBound = EarleyParserGrammarFeatures::JSONNumberBound;

constexpr uint32_t kNumberSeenFraction = 1U << 0;
constexpr uint32_t kNumberInExponent = 1U << 1;
constexpr uint32_t kNumberExponentNegative = 1U << 2;
constexpr uint32_t kNumberNonzero = 1U << 3;
constexpr uint32_t kNumberExponentOverflow = 1U << 4;
constexpr uint32_t kNumberFractionOverflow = 1U << 5;
constexpr uint32_t kNumberSignificantOverflow = 1U << 6;
constexpr uint32_t kNumberNegative = 1U << 7;
constexpr int32_t kNumberMinimumCompareShift = 8;
constexpr int32_t kNumberMaximumCompareShift = 10;
constexpr uint32_t kNumberCompareMask = 3;
constexpr uint32_t kNumberTrailingOverflow = 1U << 12;
constexpr int32_t kNumberTrailingShift = 13;
constexpr uint32_t kNumberLowFlagsMask = (1U << kNumberTrailingShift) - 1;
constexpr uint32_t kNumberTrailingMax =
    std::numeric_limits<uint32_t>::max() >> kNumberTrailingShift;
constexpr int32_t kNumberCompareEqual = 0;
constexpr int32_t kNumberCompareLess = 1;
constexpr int32_t kNumberCompareGreater = 2;

struct JSONObjectRequiredMergeStateHash {
  size_t operator()(ParserState state) const {
    state.json_object_required_state_id = -1;
    return StateHashForParsing()(state);
  }
};

struct JSONObjectRequiredMergeStateEqual {
  bool operator()(ParserState lhs, ParserState rhs) const {
    lhs.json_object_required_state_id = -1;
    rhs.json_object_required_state_id = -1;
    return StateEqualForParsing()(lhs, rhs);
  }
};

int32_t GetNumberCompare(uint32_t flags, int32_t shift) {
  return (flags >> shift) & kNumberCompareMask;
}

void SetNumberCompare(uint32_t* flags, int32_t shift, int32_t compare) {
  *flags = (*flags & ~(kNumberCompareMask << shift)) | (compare << shift);
}

std::optional<JSONNumberBound> ParseJSONNumberBound(const std::string& text) {
  size_t position = 0;
  bool negative = false;
  if (position < text.size() && text[position] == '-') {
    negative = true;
    ++position;
  }
  const size_t integer_begin = position;
  while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (position == integer_begin || (text[integer_begin] == '0' && position - integer_begin != 1)) {
    return std::nullopt;
  }
  std::string digits = text.substr(integer_begin, position - integer_begin);
  int64_t fractional_digits = 0;
  if (position < text.size() && text[position] == '.') {
    const size_t fraction_begin = ++position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
      digits.push_back(text[position++]);
    }
    if (position == fraction_begin) return std::nullopt;
    fractional_digits = static_cast<int64_t>(position - fraction_begin);
  }

  SignedDecimalInteger exponent;
  bool exponent_negative = false;
  if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
      exponent_negative = text[position++] == '-';
    }
    const size_t exponent_begin = position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
      ++position;
    }
    if (position == exponent_begin) return std::nullopt;
    exponent = MakeSignedDecimalInteger(
        exponent_negative, std::string_view(text).substr(exponent_begin, position - exponent_begin)
    );
  }
  if (position != text.size()) return std::nullopt;

  const size_t first_nonzero = digits.find_first_not_of('0');
  if (first_nonzero == std::string::npos) {
    return JSONNumberBound{false, "0", {}, 0};
  }
  digits.erase(0, first_nonzero);
  SignedDecimalInteger order =
      AddSignedDecimalInteger(exponent, static_cast<int64_t>(digits.size()) - fractional_digits);
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
  }
  auto small_order = TryConvertSignedDecimalIntegerToInt64(order);
  return JSONNumberBound{negative, std::move(digits), std::move(order), small_order};
}

struct JSONNumberExponentThreshold {
  bool negative = false;
  uint64_t small_magnitude = 0;
  std::string large_magnitude;

  bool IsZero() const {
    return large_magnitude.empty() ? small_magnitude == 0 : large_magnitude == "0";
  }

  int32_t DigitCount() const {
    if (!large_magnitude.empty()) {
      return large_magnitude == "0" ? 0 : static_cast<int32_t>(large_magnitude.size());
    }
    int32_t result = 0;
    for (uint64_t value = small_magnitude; value != 0; value /= 10) {
      ++result;
    }
    return result;
  }

  int32_t DigitAt(int32_t index) const {
    if (!large_magnitude.empty()) {
      return index < static_cast<int32_t>(large_magnitude.size()) ? large_magnitude[index] - '0'
                                                                  : 0;
    }
    const int32_t digit_count = DigitCount();
    if (index >= digit_count) {
      return 0;
    }
    uint64_t divisor = 1;
    for (int32_t position = index + 1; position < digit_count; ++position) {
      divisor *= 10;
    }
    return static_cast<int32_t>(small_magnitude / divisor % 10);
  }
};

bool TryAddInt64(int64_t lhs, int64_t rhs, int64_t* result) {
  if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)) {
    return false;
  }
  *result = lhs + rhs;
  return true;
}

uint64_t UnsignedMagnitude(int64_t value) {
  return value >= 0 ? static_cast<uint64_t>(value) : static_cast<uint64_t>(-(value + 1)) + 1;
}

JSONNumberExponentThreshold MakeJSONNumberExponentThreshold(
    const JSONNumberBound& bound, int32_t significand_order, bool exponent_negative
) {
  // For a positive exponent, compare exponent against bound.order - significand_order.
  // For a negative exponent, compare it against significand_order - bound.order and invert the
  // result. Construct the latter by negating the former.
  const int64_t adjustment = -static_cast<int64_t>(significand_order);
  int64_t small_threshold = 0;
  if (bound.small_order.has_value() &&
      TryAddInt64(*bound.small_order, adjustment, &small_threshold)) {
    if (exponent_negative && small_threshold != std::numeric_limits<int64_t>::min()) {
      small_threshold = -small_threshold;
    } else if (exponent_negative) {
      SignedDecimalInteger exact =
          NegateSignedDecimalInteger(AddSignedDecimalInteger(bound.order, adjustment));
      return {exact.negative, 0, std::move(exact.digits)};
    }
    return {small_threshold < 0, UnsignedMagnitude(small_threshold), {}};
  }
  SignedDecimalInteger exact = AddSignedDecimalInteger(bound.order, adjustment);
  if (exponent_negative) {
    exact = NegateSignedDecimalInteger(std::move(exact));
  }
  return {exact.negative, 0, std::move(exact.digits)};
}

int32_t JSONNumberExponentDigitCount(uint32_t flags, int32_t exponent_storage) {
  if (flags & kNumberExponentOverflow) {
    return exponent_storage;
  }
  int32_t result = 0;
  for (int32_t value = exponent_storage; value != 0; value /= 10) {
    ++result;
  }
  return result;
}

}  // namespace

bool EarleyParser::IsCompleted() const { return is_completed_.back(); }

bool EarleyParser::CompletionConsumedMarker(const ParserState& state) const {
  const auto& body = grammar_->GetGrammarExpr(grammar_->GetRule(state.rule_id).body_expr_id);
  if (body.type != GrammarExprType::kTagDispatch) {
    return true;
  }
  XGRAMMAR_DCHECK(grammar_->per_rule_fsms[state.rule_id].has_value());
  return (*complete_fsm_edges_)[state.element_id].size() == 0;
}

std::vector<CaptureOccurrence> EarleyParser::CollectStopCaptureTargets(const ParserState& state
) const {
  // Follow only the parent links of this concrete rule occurrence. A byte-overlap test at
  // materialization time cannot distinguish an actual captured ancestor from an unrelated
  // Earley branch that happens to cover the same input.
  std::vector<CaptureOccurrence> targets;
  std::vector<CaptureOccurrence> pending{{state.rule_id, state.rule_start_pos}};
  std::unordered_set<int64_t> visited;
  while (!pending.empty()) {
    CaptureOccurrence occurrence = pending.back();
    pending.pop_back();
    int64_t occurrence_key = (static_cast<int64_t>(occurrence.rule_id) << 32) |
                             static_cast<uint32_t>(occurrence.start_pos);
    if (!visited.insert(occurrence_key).second) {
      continue;
    }
    if (RuleHasCapture(occurrence.rule_id)) {
      targets.push_back(occurrence);
    }
    if (occurrence.start_pos == ParserState::kNoPrevInputPos) {
      continue;
    }
    const auto& parent_states = rule_id_to_completable_states_[occurrence.start_pos];
    for (const auto& [ref_rule_id, parent_state] : parent_states) {
      if (ref_rule_id != occurrence.rule_id || parent_state.rule_id < 0) {
        continue;
      }
      pending.push_back({parent_state.rule_id, parent_state.rule_start_pos});
    }
  }
  return targets;
}

void EarleyParser::RecordCaptureEvent(const ParserState& state, bool marker_present) {
  const auto* suffix_stop_info = grammar_->GetSuffixStopInfo(state.rule_id);
  bool marker_consumed =
      marker_present && suffix_stop_info != nullptr &&
      (suffix_stop_info->hidden_suffix_bytes > 0 || suffix_stop_info->hidden_stop_bytes > 0) &&
      CompletionConsumedMarker(state);
  const int32_t hidden_suffix_bytes = marker_consumed ? suffix_stop_info->hidden_suffix_bytes : 0;
  const int32_t hidden_stop_bytes = marker_consumed ? suffix_stop_info->hidden_stop_bytes : 0;

  int32_t event_start_pos = state.rule_start_pos;
  if (marker_consumed && suffix_stop_info->body_rule_id == state.rule_id) {
    // A self-referencing body helper marks the zero-width event inserted immediately after a
    // dynamic string trigger. Its capture span is the fixed-length marker that precedes it.
    XGRAMMAR_DCHECK(event_start_pos != ParserState::kNoPrevInputPos);
    event_start_pos -= std::max(hidden_suffix_bytes, hidden_stop_bytes);
    XGRAMMAR_DCHECK(event_start_pos >= 0);
  }

  std::vector<CaptureOccurrence> stop_capture_targets =
      hidden_stop_bytes > 0 ? CollectStopCaptureTargets(state) : std::vector<CaptureOccurrence>{};

  capture_event_history_.PushBackInLatestRow(
      {state.rule_id,
       event_start_pos,
       state.rule_start_pos,
       hidden_suffix_bytes,
       hidden_stop_bytes,
       std::move(stop_capture_targets)}
  );
}

int32_t EarleyParser::ResolveActiveTemperatureRule(int32_t rule_id, int32_t inherited_rule_id)
    const {
  return grammar_->GetRule(rule_id).temperature.has_value() ? rule_id : inherited_rule_id;
}

void EarleyParser::EnterJSONObjectRequiredRule(
    const ParserState& parent_state, int32_t ref_rule_id, ParserState* child_state
) const {
  child_state->json_object_required_rule_id = parent_state.json_object_required_rule_id;
  child_state->json_object_required_state_id = parent_state.json_object_required_state_id;
  const auto& ref_rule = grammar_->GetRule(ref_rule_id);
  if (ref_rule.HasJSONObjectRequiredConstraint()) {
    child_state->json_object_required_rule_id = ref_rule_id;
    child_state->json_object_required_state_id =
        grammar_features_->GetJSONObjectRequiredEmptyState(ref_rule_id);
    XGRAMMAR_DCHECK(child_state->json_object_required_state_id >= 0);
  }
}

void EarleyParser::PopLastStates(int32_t cnt) {
  stop_token_is_accepted_ = false;
  if (cnt >= static_cast<int32_t>(rule_id_to_completable_states_.size())) {
    XGRAMMAR_LOG(FATAL) << "The number of states to be popped is larger than the size of states.";
  }
  rule_id_to_completable_states_.PopBack(cnt);
  is_completed_.erase(is_completed_.end() - cnt, is_completed_.end());
  scanable_state_history_.PopBack(cnt);
  if (capture_tracking_) {
    capture_event_history_.PopBack(cnt);
  }
  if (has_char_budget_rules_) {
    char_count_history_.erase(char_count_history_.end() - cnt, char_count_history_.end());
    char_budget_entry_history_.erase(
        char_budget_entry_history_.end() - cnt, char_budget_entry_history_.end()
    );
  }
}

void EarleyParser::Complete(const ParserState& state, bool debug_print, bool marker_present) {
  auto runtime_constraints_allow_completion = [&]() {
    if (state.json_string_length_rule_id < 0) {
      return true;
    }
    const auto& completed_rule = grammar_->GetRule(state.json_string_length_rule_id);
    if (completed_rule.HasJSONNumberConstraint()) {
      return (completed_rule.json_number_multiple_of_coefficient == 0 ||
              JSONNumberMultipleOfAccepts(state)) &&
             JSONNumberRangeAccepts(state);
    }
    const auto* pattern_dfa =
        grammar_features_->GetJSONStringPatternDFA(state.json_string_length_rule_id);
    return state.GetJSONStringDecodeState() == 0 && state.json_string_pending_high_surrogate == 0 &&
           state.json_string_char_count >= completed_rule.json_string_min_chars &&
           (completed_rule.json_string_max_chars < 0 ||
            state.json_string_char_count <= completed_rule.json_string_max_chars) &&
           (pattern_dfa == nullptr || pattern_dfa->IsEndState(state.GetJSONStringPatternState()));
  };
  const bool completes_runtime_constraint_rule =
      state.rule_id >= 0 && state.rule_id == state.json_string_length_rule_id;
  if (completes_runtime_constraint_rule && !runtime_constraints_allow_completion()) {
    return;
  }
  const bool completes_json_object_required_rule =
      state.rule_id >= 0 && state.rule_id == state.json_object_required_rule_id;
  if (completes_json_object_required_rule) {
    const auto& completed_rule = grammar_->GetRule(state.json_object_required_rule_id);
    XGRAMMAR_DCHECK(completed_rule.HasJSONObjectRequiredConstraint());
    if (!grammar_features_->json_object_required_states->IsComplete(
            state.json_object_required_state_id, completed_rule.json_object_required_count
        )) {
      return;
    }
  }
  int32_t completed_json_object_required_state_id = state.json_object_required_state_id;
  if (state.rule_id >= 0) {
    const auto& completed_rule = grammar_->GetRule(state.rule_id);
    if (completed_rule.IsJSONObjectRequiredProperty() &&
        state.json_object_required_rule_id == completed_rule.json_object_required_owner_rule_id) {
      completed_json_object_required_state_id =
          grammar_features_->json_object_required_states->Mark(
              state.json_object_required_state_id,
              completed_rule.json_object_required_property_index
          );
      if (debug_print) {
        XGRAMMAR_LOG(INFO) << "Completed required property " << completed_rule.name << ": state "
                           << state.json_object_required_state_id << " -> "
                           << completed_json_object_required_state_id;
      }
    }
  }
  // Record capture and hidden-span events. This is only enabled during definitive advances;
  // speculative completions (mask computation, lookahead) never record events.
  if (capture_recording_ && RuleNeedsCaptureEvent(state.rule_id)) {
    RecordCaptureEvent(state, marker_present);
  }
  if (state.rule_id != -1 && grammar_->GetRule(state.rule_id).is_lazy) {
    tmp_completed_lazy_occurrences_.emplace_back(state.rule_id, state.rule_start_pos);
  }
  // Check if a rule is completed.
  if (state.rule_start_pos == ParserState::kNoPrevInputPos) {
    // A terminal-like constrained body can be flattened into its enclosing FSM. In that case the
    // active owner rule has no separate completion state, so its constraint must be checked when
    // the derivation leaves the active runtime-constraint scope instead.
    if (!runtime_constraints_allow_completion()) {
      return;
    }
    // assert: if a root rule can achieve here, then it must be completed.
    if (debug_print) {
      XGRAMMAR_LOG(INFO) << "The root rule is completed.";
    }
    tmp_accept_stop_token_ = true;
    return;
  }
  if (debug_print) {
    XGRAMMAR_LOG(INFO) << "The rule " << state.rule_id << ": "
                       << grammar_->GetRule(state.rule_id).name << " is completed with " << state
                       << ", trying to complete its parent states.";
  }

  // Check all the possible parent states.
  const auto& parent_states_map = rule_id_to_completable_states_[state.rule_start_pos];
  const bool completed_without_input =
      state.rule_start_pos == static_cast<int32_t>(rule_id_to_completable_states_.size()) - 1;
  for (const auto& [ref_id, parent_state] : parent_states_map) {
    if (ref_id != state.rule_id) {
      continue;
    }
    if (state.json_string_length_rule_id >= 0 &&
        parent_state.json_string_length_rule_id != state.json_string_length_rule_id &&
        !runtime_constraints_allow_completion()) {
      continue;
    }
    auto resume_parent_state = [&](ParserState result) {
      if (parent_state.json_string_length_rule_id >= 0 &&
          parent_state.json_string_length_rule_id == state.json_string_length_rule_id) {
        result.partial_codepoint = state.partial_codepoint;
        result.json_string_char_count = state.json_string_char_count;
        result.json_string_decode_state = state.json_string_decode_state;
        result.json_string_pending_high_surrogate = state.json_string_pending_high_surrogate;
        result.json_string_length_rule_id = state.json_string_length_rule_id;
        result.json_string_pattern_state = state.json_string_pattern_state;
      }
      if (parent_state.json_object_required_rule_id >= 0 &&
          parent_state.json_object_required_rule_id == state.json_object_required_rule_id) {
        result.json_object_required_rule_id = state.json_object_required_rule_id;
        result.json_object_required_state_id = completed_json_object_required_state_id;
      } else {
        result.json_object_required_rule_id = parent_state.json_object_required_rule_id;
        result.json_object_required_state_id = parent_state.json_object_required_state_id;
      }
      return result;
    };
    XGRAMMAR_DCHECK(
        parent_state.rule_id == -1 || grammar_->per_rule_fsms[parent_state.rule_id].has_value()
    );
    if (parent_state.rule_id == -1) {
      const auto& parent_expr = grammar_->GetGrammarExpr(parent_state.sequence_id);
      const auto& element_expr = grammar_->GetGrammarExpr(parent_expr[parent_state.element_id]);
      // The new rule is not referenced by a fsm.
      XGRAMMAR_DCHECK(
          element_expr.type == GrammarExprType::kRuleRef ||
          element_expr.type == GrammarExprType::kRepeat
      );
      if (element_expr.type == GrammarExprType::kRuleRef) {
        Enqueue(resume_parent_state(ParserState{
            parent_state.rule_id,
            parent_state.sequence_id,
            parent_state.element_id + 1,
            parent_state.rule_start_pos,
            parent_state.budget_deadline,
            0,
            0,
            0,
            parent_state.active_temperature_rule_id,
            parent_state.char_budget_deadline
        }));
        continue;
      }
      XGRAMMAR_DCHECK(element_expr.type == GrammarExprType::kRepeat);
      // The parent state is a repeat, we need to increase the repeat count.
      auto new_state = parent_state;
      const int32_t& min_repeat_count = element_expr[1];
      const int32_t& max_repeat_count = element_expr[2];
      new_state.repeat_count++;
      // The repeat rule can be completed, and we advance the state. Don't forget to
      // reset the repeat count.
      if (new_state.repeat_count >= min_repeat_count) {
        Enqueue(resume_parent_state(ParserState{
            parent_state.rule_id,
            parent_state.sequence_id,
            parent_state.element_id + 1,
            parent_state.rule_start_pos,
            parent_state.budget_deadline,
            0,
            0,
            0,
            parent_state.active_temperature_rule_id,
            parent_state.char_budget_deadline
        }));
      }
      // If the repeat count is less than the max repeat count, we can continue to
      // visit the repeat state for another round.
      if ((max_repeat_count == -1 &&
           (!completed_without_input || new_state.repeat_count < min_repeat_count)) ||
          (max_repeat_count != -1 && new_state.repeat_count < max_repeat_count)) {
        Enqueue(resume_parent_state(new_state));
      }
      continue;
    }
    // If the rule is referenced by a fsm, we need to advance the fsm.
    XGRAMMAR_DCHECK(grammar_->per_rule_fsms[parent_state.rule_id].has_value());

    // A repeat edge is the only outgoing edge of its source state, so repeat_count belongs to
    // exactly one repetition.
    const auto& parent_edges = (*complete_fsm_edges_)[parent_state.element_id];
    if (parent_edges.size() == 1 && parent_edges[0].IsRepeatRef()) {
      const auto& edge = parent_edges[0];
      auto info = grammar_->complete_fsm.GetRepeatEdgeInfo(edge.GetAuxIndex());
      // A normal rule reference may land on a repeat source state. It is a repeat completion
      // only when the completed rule matches the repeat edge.
      if (info.RuleId() != ref_id) {
        Enqueue(resume_parent_state(parent_state));
        continue;
      }
      int32_t new_count = parent_state.repeat_count + 1;
      const bool parent_tracks_json_string_length = parent_state.json_string_length_rule_id >= 0;
      const auto make_repeated_parent_state = [&](int32_t element_id, int32_t repeat_count) {
        ParserState result{
            parent_state.rule_id,
            parent_state.sequence_id,
            element_id,
            parent_state.rule_start_pos,
            parent_state.budget_deadline,
            0,
            repeat_count,
            0,
            parent_state.active_temperature_rule_id,
            parent_state.char_budget_deadline
        };
        if (parent_tracks_json_string_length) {
          result.partial_codepoint = parent_state.partial_codepoint;
          result.json_string_char_count = parent_state.json_string_char_count;
          result.json_string_decode_state = parent_state.json_string_decode_state;
          result.json_string_pending_high_surrogate =
              parent_state.json_string_pending_high_surrogate;
          result.json_string_length_rule_id = parent_state.json_string_length_rule_id;
          result.json_string_pattern_state = parent_state.json_string_pattern_state;
        }
        return resume_parent_state(std::move(result));
      };
      if (new_count >= info.Lower() && (info.Upper() == -1 || new_count <= info.Upper())) {
        Enqueue(make_repeated_parent_state(edge.target, 0));
      }
      if ((info.Upper() == -1 && (!completed_without_input || new_count < info.Lower())) ||
          (info.Upper() != -1 && new_count < info.Upper())) {
        Enqueue(make_repeated_parent_state(parent_state.element_id, new_count));
      }
      continue;
    }
    Enqueue(resume_parent_state(parent_state));
  }
}

std::pair</* scanable */ bool, /* completable */ bool> EarleyParser::Predict(
    const ParserState& state, bool debug_print
) {
  // Check if the rule has a corresponding FSM.
  if (state.rule_id != -1) {
    XGRAMMAR_DCHECK(grammar_->per_rule_fsms[state.rule_id].has_value());
    const uint8_t flags = GetFsmStateFlags(state.rule_id, state.element_id);
    if (flags & kFsmStateNonTerminal) {
      ExpandNextRuleRefElementOnFSM(state, debug_print);
    }
    return std::make_pair(flags & kFsmStateScanable, flags & kFsmStateEnd);
  }
  const GrammarExpr& grammar_expr = grammar_->GetGrammarExpr(state.sequence_id);
  XGRAMMAR_DCHECK(
      grammar_expr.type == GrammarExprType::kSequence ||
      grammar_expr.type == GrammarExprType::kEmptyStr
  );
  if (state.element_id == grammar_expr.size()) {
    // The rule is completed.
    return std::make_pair(false, true);
  }
  const auto& element_expr = grammar_->GetGrammarExpr(grammar_expr[state.element_id]);
  switch (element_expr.type) {
    case GrammarExprType::kRuleRef: {
      ExpandNextRuleRefElement(state, grammar_expr, &element_expr, debug_print);
      return std::make_pair(false, false);
    }
    case GrammarExprType::kCharacterClassStar: {
      if (state.sub_element_id == 0) {
        auto next_state = state;
        ++next_state.element_id;
        next_state.sub_element_id = 0;
        next_state.repeat_count = 0;
        Enqueue(next_state);
      }
      return std::make_pair(true, false);
    }
    case GrammarExprType::kRepeat: {
      const int32_t& min_repeat_count = element_expr[1];
      const int32_t& max_repeat_count = element_expr[2];
      // If the current repeat count is less than the max repeat count,
      // we can expand the next rule reference element.
      XGRAMMAR_DCHECK(max_repeat_count == -1 || state.repeat_count <= max_repeat_count);
      if (max_repeat_count == -1 || state.repeat_count < max_repeat_count) {
        ExpandNextRuleRefElement(state, grammar_expr, &element_expr, debug_print);
      }
      if (state.repeat_count >= min_repeat_count) {
        auto next_state = state;
        ++next_state.element_id;
        next_state.sub_element_id = 0;
        next_state.repeat_count = 0;
        Enqueue(next_state);
      }
      return std::make_pair(false, false);
    }
    case GrammarExprType::kByteString:
    case GrammarExprType::kCharacterClass: {
      return std::make_pair(true, false);  // The element is scanable, but not completable.
    }
    case GrammarExprType::kToken:
    case GrammarExprType::kExcludeToken: {
      return std::make_pair(false, false);
    }
    default: {
      XGRAMMAR_LOG(FATAL) << "The element type is not supported! The type is: "
                          << int(element_expr.type);
      XGRAMMAR_UNREACHABLE();
    }
  }
}

void EarleyParser::Scan(const ParserState& state, const uint8_t ch) {
  XGRAMMAR_DCHECK(state.rule_id == -1 || grammar_->per_rule_fsms[state.rule_id].has_value());
  if (state.rule_id == -1) {
    const auto& cur_rule = grammar_->GetGrammarExpr(state.sequence_id);
    const auto& element_expr = grammar_->GetGrammarExpr(cur_rule[state.element_id]);
    // The element is a rule reference, we do not need to scan it.
    switch (element_expr.type) {
      case (GrammarExprType::kByteString): {
        AdvanceByteString(state, ch, element_expr);
        break;
      }
      case (GrammarExprType::kCharacterClass): {
        AdvanceCharacterClass(state, ch, element_expr);
        break;
      }
      case (GrammarExprType::kCharacterClassStar): {
        AdvanceCharacterClassStar(state, ch, element_expr);
        break;
      }
      default: {
        XGRAMMAR_LOG(FATAL) << "The element type is not supported! The type is: "
                            << int(element_expr.type);
        XGRAMMAR_UNREACHABLE();
      }
    }
  } else {
    AdvanceFsm(state, ch);
  }
}

/*!
  \note The workflow of Advance is as follows:
  1. Scan all the states in the latest states. Add all the possible states
  to the next states.
  2. If the next states are empty, then the character is not accepted.
  3. If the next states are not empty, then the character is accepted. Moreover,
  we need to complete and predict the next states.

  \note Thus, when initializing the Earley parser, we need to add the initial state
  to the history_states[0], and perform prediction and completion on the initial state.
*/
bool EarleyParser::Advance(const uint8_t ch, bool debug_print) {
  // Initialize the containers.
  XGRAMMAR_DCHECK(tmp_process_state_queue_.empty())
      << "The tmp_process_state_queue_ should be empty before the scan.";
  tmp_states_visited_in_queue_.Clear();
  tmp_states_to_be_added_.clear();
  tmp_accept_stop_token_ = false;
  tmp_completed_lazy_occurrences_.clear();
  if (has_char_budget_rules_) {
    tmp_char_budget_entered_ = char_budget_entry_history_.back();
    char_count_history_.push_back(GetCurrentCharIndex() + StartsUTF8Codepoint(ch));
  }
  const auto& latest_states = scanable_state_history_[scanable_state_history_.size() - 1];
  // Scan all the scanable states.
  for (const auto& state : latest_states) {
    if (skip_expired_states_ && IsExpiredState(state)) {
      continue;
    }
    Scan(state, ch);
  }

  // Check if the character is accepted.
  if (tmp_process_state_queue_.empty() && tmp_states_to_be_added_.empty()) {
    if (has_char_budget_rules_) {
      char_count_history_.pop_back();
    }
    return false;
  }

  // execute Predict and Complete for all states in the queue until empty.
  rule_id_to_completable_states_.PushBackEmpty();
  if (capture_tracking_) {
    capture_event_history_.PushBack(std::vector<CaptureEvent>());
  }
  while (!tmp_process_state_queue_.empty()) {
    const ParserState& state = *tmp_process_state_queue_.front();
    tmp_process_state_queue_.pop();
    auto [scanable, completable] = Predict(state, debug_print);
    if (completable) {
      Complete(state, debug_print);
    }
    if (scanable) {
      tmp_states_to_be_added_.push_back(&state);
    }
  }

  // Check if the grammar is completed, and add the scannable states to the history.
  if (!tmp_completed_lazy_occurrences_.empty()) {
    RemoveCommittedLazyStates();
  }
  is_completed_.push_back(tmp_accept_stop_token_);
  PushLatestScanableStates();
  if (has_char_budget_rules_) {
    char_budget_entry_history_.push_back(tmp_char_budget_entered_);
  }
  return true;
}

void EarleyParser::RemoveCommittedLazyStates() {
  auto is_committed = [&](const ParserState* state) {
    for (const auto& [rule_id, rule_start_pos] : tmp_completed_lazy_occurrences_) {
      if (state->rule_id == rule_id && state->rule_start_pos == rule_start_pos) {
        return true;
      }
    }
    return false;
  };
  tmp_states_to_be_added_.erase(
      std::remove_if(tmp_states_to_be_added_.begin(), tmp_states_to_be_added_.end(), is_committed),
      tmp_states_to_be_added_.end()
  );
}

int32_t EarleyParserGrammarFeatures::JSONObjectRequiredStateInterner::InternLocked(
    Key key, int32_t seen_count
) {
  auto existing = ids_.find(key);
  if (existing != ids_.end()) return existing->second;
  const int32_t id = static_cast<int32_t>(states_.size());
  states_.push_back(std::move(key));
  seen_counts_.push_back(seen_count);
  ids_.emplace(states_.back(), id);
  return id;
}

int32_t EarleyParserGrammarFeatures::JSONObjectRequiredStateInterner::InternEmpty(
    int32_t owner_rule_id, int32_t required_count
) {
  XGRAMMAR_CHECK(owner_rule_id >= 0 && required_count >= 0);
  std::lock_guard<std::mutex> lock(mutex_);
  return InternLocked(
      Key{owner_rule_id, std::vector<uint64_t>((required_count + 63) / 64, uint64_t{0})}, 0
  );
}

int32_t EarleyParserGrammarFeatures::JSONObjectRequiredStateInterner::Mark(
    int32_t state_id, int32_t property_index
) {
  std::lock_guard<std::mutex> lock(mutex_);
  XGRAMMAR_DCHECK(state_id >= 0 && state_id < static_cast<int32_t>(states_.size()));
  const Key& current = states_[state_id];
  XGRAMMAR_DCHECK(
      property_index >= 0 && property_index < static_cast<int32_t>(current.words.size() * 64)
  );
  const size_t word_index = static_cast<size_t>(property_index) / 64;
  const uint64_t bit = uint64_t{1} << (property_index % 64);
  if (current.words[word_index] & bit) return state_id;
  Key next = current;
  next.words[word_index] |= bit;
  return InternLocked(std::move(next), seen_counts_[state_id] + 1);
}

int32_t EarleyParserGrammarFeatures::JSONObjectRequiredStateInterner::Union(
    int32_t lhs_state_id, int32_t rhs_state_id
) {
  if (lhs_state_id == rhs_state_id) return lhs_state_id;
  std::lock_guard<std::mutex> lock(mutex_);
  XGRAMMAR_DCHECK(lhs_state_id >= 0 && lhs_state_id < static_cast<int32_t>(states_.size()));
  XGRAMMAR_DCHECK(rhs_state_id >= 0 && rhs_state_id < static_cast<int32_t>(states_.size()));
  const Key& lhs = states_[lhs_state_id];
  const Key& rhs = states_[rhs_state_id];
  XGRAMMAR_DCHECK(lhs.owner_rule_id == rhs.owner_rule_id && lhs.words.size() == rhs.words.size());
  Key merged = lhs;
  int32_t seen_count = 0;
  for (size_t index = 0; index < merged.words.size(); ++index) {
    merged.words[index] |= rhs.words[index];
    seen_count += __builtin_popcountll(merged.words[index]);
  }
  return InternLocked(std::move(merged), seen_count);
}

bool EarleyParserGrammarFeatures::JSONObjectRequiredStateInterner::IsComplete(
    int32_t state_id, int32_t required_count
) const {
  std::lock_guard<std::mutex> lock(mutex_);
  XGRAMMAR_DCHECK(state_id >= 0 && state_id < static_cast<int32_t>(seen_counts_.size()));
  return seen_counts_[state_id] == required_count;
}

int32_t EarleyParserGrammarFeatures::JSONObjectRequiredStateInterner::SeenCount(int32_t state_id
) const {
  std::lock_guard<std::mutex> lock(mutex_);
  XGRAMMAR_DCHECK(state_id >= 0 && state_id < static_cast<int32_t>(seen_counts_.size()));
  return seen_counts_[state_id];
}

size_t EarleyParserGrammarFeatures::JSONObjectRequiredStateInterner::MemorySizeBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t result = sizeof(*this) + states_.capacity() * sizeof(Key) + MemorySize(seen_counts_);
  for (const auto& state : states_) result += MemorySize(state.words);
  return result;
}

EarleyParserGrammarFeatures::EarleyParserGrammarFeatures(
    const Grammar& grammar, RegexFSMCache* regex_fsm_cache
)
    : fsm_state_flags(grammar->complete_fsm.NumStates(), kFsmStateInitialized),
      rule_is_nullable(grammar->NumRules(), 0),
      json_string_pattern_dfas(grammar->NumRules()),
      json_number_ranges(grammar->NumRules()),
      json_object_required_states(std::make_shared<JSONObjectRequiredStateInterner>()),
      json_object_empty_state_ids(grammar->NumRules(), -1) {
  XGRAMMAR_CHECK(grammar->optimized)
      << "Cannot build Earley parser features for an unoptimized grammar";

  const auto& complete_fsm_edges = grammar->complete_fsm.GetEdges();
  for (int32_t state_id = 0; state_id < grammar->complete_fsm.NumStates(); ++state_id) {
    const auto edges = complete_fsm_edges[state_id];
    uint8_t& flags = fsm_state_flags[state_id];
    if (edges.size() != 0) {
      flags |= kFsmStateHasEdges;
    }
    for (const auto& edge : edges) {
      if (edge.IsCharRange() || edge.IsToken() || edge.IsExcludeToken()) {
        flags |= kFsmStateScanable;
      } else if (edge.IsRuleRef() || edge.IsEpsilon() || edge.IsRepeatRef()) {
        flags |= kFsmStateNonTerminal;
      }
    }
  }

  for (int32_t rule_id = 0; rule_id < grammar->NumRules(); ++rule_id) {
    const auto& rule = grammar->GetRule(rule_id);
    const auto& rule_fsm = grammar->per_rule_fsms[rule_id];
    XGRAMMAR_DCHECK(rule_fsm.has_value());
    for (int32_t end_state : rule_fsm->GetFsm().GetEnds()) {
      fsm_state_flags[end_state] |= kFsmStateEnd;
    }
    const auto* suffix_stop_info = grammar->GetSuffixStopInfo(rule_id);
    has_budget_rules = has_budget_rules || rule.max_tokens >= 0;
    has_char_budget_rules = has_char_budget_rules || rule.max_chars >= 0;
    has_json_string_length_rules = has_json_string_length_rules ||
                                   rule.json_string_min_chars >= 0 ||
                                   rule.HasJSONNumberConstraint();
    if (rule.HasJSONObjectRequiredConstraint()) {
      has_json_object_required_rules = true;
      json_object_empty_state_ids[rule_id] =
          json_object_required_states->InternEmpty(rule_id, rule.json_object_required_count);
    }
    if (rule.HasJSONNumberRange()) {
      JSONNumberRange range;
      if (!rule.json_number_minimum.empty()) {
        range.minimum = ParseJSONNumberBound(rule.json_number_minimum);
        XGRAMMAR_CHECK(range.minimum.has_value())
            << "Invalid exact JSON-number minimum on rule " << rule.name;
      }
      if (!rule.json_number_maximum.empty()) {
        range.maximum = ParseJSONNumberBound(rule.json_number_maximum);
        XGRAMMAR_CHECK(range.maximum.has_value())
            << "Invalid exact JSON-number maximum on rule " << rule.name;
      }
      range.exclusive_minimum = rule.json_number_exclusive_minimum;
      range.exclusive_maximum = rule.json_number_exclusive_maximum;
      json_number_ranges[rule_id] = std::move(range);
    }
    if (!rule.json_string_pattern.empty()) {
      const std::string rewritten_pattern =
          RewriteJSONSchemaPatternForFullMatch(rule.json_string_pattern);
      const std::string cache_key = MakeRegexFSMCacheKey(rewritten_pattern, /*json_string=*/false);
      FSMWithStartEnd* pattern_fsm = nullptr;
      if (regex_fsm_cache != nullptr) {
        auto cached = regex_fsm_cache->find(cache_key);
        if (cached != regex_fsm_cache->end()) {
          pattern_fsm = &cached->second;
        }
      }
      std::optional<FSMWithStartEnd> built_fsm;
      if (pattern_fsm == nullptr) {
        auto built = BuildJSONSchemaPatternFSM(rule.json_string_pattern, 4096);
        XGRAMMAR_CHECK(built.IsOk())
            << "Failed to rebuild decoded JSON-string pattern for rule " << rule.name;
        built_fsm.emplace(std::move(built).Unwrap());
        pattern_fsm = &*built_fsm;
      }
      XGRAMMAR_CHECK(pattern_fsm->IsDFA())
          << "Decoded JSON-string pattern is no longer deterministic for rule " << rule.name;
      if (regex_fsm_cache != nullptr && built_fsm.has_value()) {
        pattern_fsm =
            &regex_fsm_cache->insert_or_assign(cache_key, std::move(*built_fsm)).first->second;
      }
      const int32_t num_states = pattern_fsm->GetFsm().NumStates();
      XGRAMMAR_CHECK(num_states < static_cast<int32_t>(JSONStringPatternDFA::kNoTransition))
          << "Decoded JSON-string pattern has too many states for its dense transition table";
      JSONStringPatternDFA pattern_dfa;
      pattern_dfa.start = pattern_fsm->GetStart();
      pattern_dfa.is_end_state.assign(num_states, false);
      for (int32_t end_state : pattern_fsm->GetEnds()) {
        pattern_dfa.is_end_state[end_state] = true;
      }
      pattern_dfa.dense_transitions.assign(num_states * 256, JSONStringPatternDFA::kNoTransition);
      for (int32_t state = 0; state < num_states; ++state) {
        for (const auto& edge : pattern_fsm->GetFsm().GetEdges(state)) {
          XGRAMMAR_DCHECK(edge.IsCharRange());
          for (int32_t byte = edge.min; byte <= edge.max; ++byte) {
            XGRAMMAR_DCHECK(
                pattern_dfa.dense_transitions[state * 256 + byte] ==
                JSONStringPatternDFA::kNoTransition
            );
            pattern_dfa.dense_transitions[state * 256 + byte] = static_cast<uint16_t>(edge.target);
          }
        }
      }
      json_string_pattern_dfas[rule_id] = std::move(pattern_dfa);
    }
    capture_tracking =
        capture_tracking || !rule.capture_name.empty() ||
        (suffix_stop_info != nullptr && !suffix_stop_info->stop_capture_name.empty());
    has_hidden_capture_rules =
        has_hidden_capture_rules ||
        (suffix_stop_info != nullptr &&
         (suffix_stop_info->hidden_suffix_bytes > 0 || suffix_stop_info->hidden_stop_bytes > 0));
  }
  for (int32_t rule_id : grammar->allow_empty_rule_ids) {
    rule_is_nullable[rule_id] = true;
  }
}

EarleyParser::EarleyParser(
    const Grammar& grammar,
    std::optional<ParserState> initial_state,
    std::shared_ptr<const EarleyParserGrammarFeatures> grammar_features
)
    : grammar_(grammar),
      complete_fsm_edges_(&grammar_->complete_fsm.GetEdges()),
      grammar_features_(
          grammar_features != nullptr ? std::move(grammar_features)
                                      : std::make_shared<const EarleyParserGrammarFeatures>(grammar)
      ) {
  if (!grammar->optimized) {
    XGRAMMAR_LOG(FATAL) << "The grammar is not optimized. Please optimize the grammar before using "
                           "the Earley parser.";
  }
  has_budget_rules_ = grammar_features_->has_budget_rules;
  has_char_budget_rules_ = grammar_features_->has_char_budget_rules;
  has_json_string_length_rules_ = grammar_features_->has_json_string_length_rules;
  has_json_object_required_rules_ = grammar_features_->has_json_object_required_rules;
  capture_tracking_ = grammar_features_->capture_tracking;
  has_hidden_capture_rules_ = grammar_features_->has_hidden_capture_rules;
  PushStateAndExpand(initial_state.has_value() ? *initial_state : RootInitialState());
}

ParserState EarleyParser::RootInitialState() const {
  const auto root_rule_id = grammar_->GetRootRuleId();
  XGRAMMAR_DCHECK(grammar_->per_rule_fsms[root_rule_id].has_value());
  ParserState result(
      root_rule_id,
      grammar_->GetRule(root_rule_id).body_expr_id,
      grammar_->per_rule_fsms[root_rule_id]->GetFsm().GetStart(),
      ParserState::kNoPrevInputPos,
      DeadlineForRule(root_rule_id, -1),
      0,
      0,
      0,
      ResolveActiveTemperatureRule(root_rule_id, -1),
      CharDeadlineForRule(root_rule_id, -1)
  );
  if (grammar_->GetRule(root_rule_id).json_string_min_chars >= 0) {
    result.json_string_length_rule_id = root_rule_id;
    if (const auto* pattern_dfa = grammar_features_->GetJSONStringPatternDFA(root_rule_id)) {
      result.SetJSONStringPatternState(pattern_dfa->GetStart());
    }
  } else if (grammar_->GetRule(root_rule_id).HasJSONNumberConstraint()) {
    const auto& rule = grammar_->GetRule(root_rule_id);
    result.SetJSONNumberRuleId(root_rule_id);
    result.SetJSONNumberExponentMagnitude(
        rule.json_number_multiple_of_coefficient > 0 ? 1 % rule.json_number_multiple_of_coefficient
                                                     : 0
    );
    result.SetJSONNumberFlags(0);
  }
  if (grammar_->GetRule(root_rule_id).HasJSONObjectRequiredConstraint()) {
    result.json_object_required_rule_id = root_rule_id;
    result.json_object_required_state_id =
        grammar_features_->GetJSONObjectRequiredEmptyState(root_rule_id);
  }
  return result;
}

void EarleyParser::PushLatestScanableStates() {
  if (!has_json_object_required_rules_) {
    scanable_state_history_.PushBackIndirect(tmp_states_to_be_added_);
    return;
  }

  using StateIndex = std::unordered_map<
      ParserState,
      size_t,
      JSONObjectRequiredMergeStateHash,
      JSONObjectRequiredMergeStateEqual>;
  auto merge_state =
      [&](const ParserState& state, StateIndex* indices, std::vector<ParserState>* merged) {
        auto [iterator, inserted] = indices->emplace(state, merged->size());
        if (inserted) {
          merged->push_back(state);
          return;
        }
        ParserState& existing = (*merged)[iterator->second];
        if (state.json_object_required_rule_id >= 0) {
          existing.json_object_required_state_id =
              grammar_features_->json_object_required_states->Union(
                  existing.json_object_required_state_id, state.json_object_required_state_id
              );
        }
      };

  tmp_merged_json_object_states_.clear();
  tmp_merged_json_object_states_.reserve(tmp_states_to_be_added_.size());
  StateIndex scanable_indices;
  scanable_indices.reserve(tmp_states_to_be_added_.size());
  for (const ParserState* state : tmp_states_to_be_added_) {
    merge_state(*state, &scanable_indices, &tmp_merged_json_object_states_);
  }
  scanable_state_history_.PushBack(tmp_merged_json_object_states_);

  const auto completable_row = rule_id_to_completable_states_.Back();
  tmp_merged_json_object_completable_states_.clear();
  tmp_merged_json_object_completable_states_.reserve(completable_row.size());
  std::unordered_map<int32_t, StateIndex> completable_indices;
  for (const auto& [ref_rule_id, state] : completable_row) {
    StateIndex& indices = completable_indices[ref_rule_id];
    auto [iterator, inserted] =
        indices.emplace(state, tmp_merged_json_object_completable_states_.size());
    if (inserted) {
      tmp_merged_json_object_completable_states_.emplace_back(ref_rule_id, state);
      continue;
    }
    ParserState& existing = tmp_merged_json_object_completable_states_[iterator->second].second;
    if (state.json_object_required_rule_id >= 0) {
      existing.json_object_required_state_id =
          grammar_features_->json_object_required_states->Union(
              existing.json_object_required_state_id, state.json_object_required_state_id
          );
    }
  }
  rule_id_to_completable_states_.PopBack(1);
  rule_id_to_completable_states_.PushBack(tmp_merged_json_object_completable_states_);
}

void EarleyParser::PushStateAndExpand(const ParserState& state) {
  tmp_states_visited_in_queue_.Clear();
  tmp_accept_stop_token_ = false;
  tmp_states_to_be_added_.clear();
  tmp_completed_lazy_occurrences_.clear();
  Enqueue(state);
  rule_id_to_completable_states_.PushBackEmpty();
  if (capture_tracking_) {
    capture_event_history_.PushBack(std::vector<CaptureEvent>());
  }
  while (!tmp_process_state_queue_.empty()) {
    const ParserState& state = *tmp_process_state_queue_.front();
    tmp_process_state_queue_.pop();
    auto [scanable, completable] = Predict(state);
    if (completable) {
      Complete(state);
    }
    if (scanable) {
      tmp_states_to_be_added_.push_back(&state);
    }
  }
  if (!tmp_completed_lazy_occurrences_.empty()) {
    RemoveCommittedLazyStates();
  }
  is_completed_.push_back(tmp_accept_stop_token_);
  PushLatestScanableStates();
  if (has_char_budget_rules_) {
    char_count_history_.push_back(GetCurrentCharIndex());
    char_budget_entry_history_.push_back(tmp_char_budget_entered_);
  }
}

void EarleyParser::Reset() {
  rule_id_to_completable_states_.PopBack(rule_id_to_completable_states_.size());
  scanable_state_history_.PopBack(scanable_state_history_.size());
  is_completed_.clear();
  stop_token_is_accepted_ = false;
  if (capture_tracking_) {
    capture_event_history_.PopBack(capture_event_history_.size());
  }
  char_count_history_.clear();
  char_budget_entry_history_.clear();
  tmp_char_budget_entered_ = false;
  capture_recording_ = false;
  XGRAMMAR_DCHECK(tmp_process_state_queue_.empty());
  PushStateAndExpand(RootInitialState());
}

void EarleyParser::ExpandNextRuleRefElement(
    const ParserState& state,
    const GrammarExpr& grammar_expr,
    const GrammarExpr* sub_grammar_expr,
    bool debug_print
) {
  // Path A. The rule has a corresponding FSM.
  XGRAMMAR_DCHECK(!(state.rule_id != -1 && grammar_->per_rule_fsms[state.rule_id].has_value()));
  XGRAMMAR_DCHECK(grammar_expr.type == GrammarExprType::kSequence);
  XGRAMMAR_DCHECK(
      sub_grammar_expr->type == GrammarExprType::kRuleRef ||
      sub_grammar_expr->type == GrammarExprType::kRepeat
  );
  auto ref_rule_id = (*sub_grammar_expr)[0];

  if (debug_print) {
    XGRAMMAR_LOG(INFO) << "The rule " << state.rule_id << ": "
                       << grammar_->GetRule(state.rule_id).name << " predict the new rule "
                       << ref_rule_id << ": " << grammar_->GetRule(ref_rule_id).name << ".";
  }

  bool right_recursion_to_root = false;
  // The right-recursion optimization elides the completion of the parent rule (and, in the
  // to-root case, corrupts the start position of the child rule), so it must be disabled when
  // either rule produces capture-history events.
  if (state.element_id != grammar_expr.size() - 1 ||
      sub_grammar_expr->type == GrammarExprType::kRepeat ||
      (state.rule_start_pos == rule_id_to_completable_states_.size() - 1) ||
      state.json_string_length_rule_id >= 0 || state.json_object_required_rule_id >= 0 ||
      RuleNeedsCaptureEvent(state.rule_id) || RuleNeedsCaptureEvent(ref_rule_id)) {
    // It's not the right recursion, or it's the root rule.
    rule_id_to_completable_states_.PushBackInLatestRow(std::make_pair(ref_rule_id, state));
  } else {
    if (state.rule_start_pos == ParserState::kNoPrevInputPos) {
      right_recursion_to_root = true;
    } else {
      // If it's the right recursion, we need to add the ancestors of the parent state.
      const auto in_vec = [&](const ParserState& state_) {
        return std::find_if(
                   rule_id_to_completable_states_.Back().begin(),
                   rule_id_to_completable_states_.Back().end(),
                   [&](const auto& s) {
                     return StateEqualForParsing()(s.second, state_) && s.first == ref_rule_id;
                   }
               ) != rule_id_to_completable_states_.Back().end();
      };
      const auto& parent_states_map = rule_id_to_completable_states_[state.rule_start_pos];
      std::vector<std::pair<int32_t, ParserState>> to_added_states;
      for (const auto& parent_state_iter : parent_states_map) {
        if (parent_state_iter.first != state.rule_id) continue;
        const auto& parent_state = parent_state_iter.second;
        if (!in_vec(parent_state)) {
          to_added_states.push_back({ref_rule_id, parent_state});
        }
      }
      for (const auto& to_add_state : to_added_states) {
        rule_id_to_completable_states_.PushBackInLatestRow(to_add_state);
      }
    }
  }

  if (IsRuleNullable(ref_rule_id)) {
    XGRAMMAR_DCHECK(grammar_expr.type == GrammarExprType::kSequence);
    ParserState nullable_parent{
        state.rule_id,
        state.sequence_id,
        state.element_id + 1,
        state.rule_start_pos,
        state.budget_deadline,
        0,
        0,
        0,
        state.active_temperature_rule_id,
        state.char_budget_deadline
    };
    if (state.json_string_length_rule_id >= 0) {
      nullable_parent.partial_codepoint = state.partial_codepoint;
      nullable_parent.json_string_char_count = state.json_string_char_count;
      nullable_parent.json_string_decode_state = state.json_string_decode_state;
      nullable_parent.json_string_pending_high_surrogate = state.json_string_pending_high_surrogate;
      nullable_parent.json_string_length_rule_id = state.json_string_length_rule_id;
      nullable_parent.json_string_pattern_state = state.json_string_pattern_state;
    }
    nullable_parent.json_object_required_rule_id = state.json_object_required_rule_id;
    nullable_parent.json_object_required_state_id = state.json_object_required_state_id;
    Enqueue(nullable_parent);
  }

  // If the reference rule is not visited, we need to add it to the queue.
  const auto& ref_rule = grammar_->GetRule(ref_rule_id);
  if (ref_rule.max_chars >= 0) {
    tmp_char_budget_entered_ = true;
  }
  const auto& ref_grammar_expr_id = ref_rule.body_expr_id;

  XGRAMMAR_DCHECK(grammar_->per_rule_fsms[ref_rule_id].has_value());
  const auto& ref_fsm = grammar_->per_rule_fsms[ref_rule_id].value();
  ParserState child_state{
      ref_rule_id,
      ref_grammar_expr_id,
      ref_fsm.GetFsm().GetStart(),
      right_recursion_to_root ? ParserState::kNoPrevInputPos
                              : int32_t(rule_id_to_completable_states_.size() - 1),
      DeadlineForRule(ref_rule_id, state.budget_deadline),
      0,
      0,
      0,
      ResolveActiveTemperatureRule(ref_rule_id, state.active_temperature_rule_id),
      CharDeadlineForRule(ref_rule_id, state.char_budget_deadline)
  };
  if (state.json_string_length_rule_id >= 0) {
    child_state.partial_codepoint = state.partial_codepoint;
    child_state.json_string_char_count = state.json_string_char_count;
    child_state.json_string_decode_state = state.json_string_decode_state;
    child_state.json_string_pending_high_surrogate = state.json_string_pending_high_surrogate;
    child_state.json_string_length_rule_id = state.json_string_length_rule_id;
    child_state.json_string_pattern_state = state.json_string_pattern_state;
  } else if (ref_rule.json_string_min_chars >= 0) {
    child_state.json_string_length_rule_id = ref_rule_id;
    if (const auto* pattern_dfa = grammar_features_->GetJSONStringPatternDFA(ref_rule_id)) {
      child_state.SetJSONStringPatternState(pattern_dfa->GetStart());
    }
  } else if (ref_rule.HasJSONNumberConstraint()) {
    child_state.SetJSONNumberRuleId(ref_rule_id);
    child_state.SetJSONNumberExponentMagnitude(
        ref_rule.json_number_multiple_of_coefficient > 0
            ? 1 % ref_rule.json_number_multiple_of_coefficient
            : 0
    );
    child_state.SetJSONNumberFlags(0);
  }
  EnterJSONObjectRequiredRule(state, ref_rule_id, &child_state);
  Enqueue(child_state);
}

void EarleyParser::ExpandNextRuleRefElementOnFSM(const ParserState& state, bool debug_print) {
  XGRAMMAR_DCHECK(state.rule_id != -1 && grammar_->per_rule_fsms[state.rule_id].has_value());
  const bool tracks_json_string_length = state.json_string_length_rule_id >= 0;
  const auto make_parent_state = [&](int32_t element_id, int32_t repeat_count = 0) {
    ParserState result{
        state.rule_id,
        state.sequence_id,
        element_id,
        state.rule_start_pos,
        state.budget_deadline,
        0,
        repeat_count,
        0,
        state.active_temperature_rule_id,
        state.char_budget_deadline
    };
    if (tracks_json_string_length) {
      result.partial_codepoint = state.partial_codepoint;
      result.json_string_char_count = state.json_string_char_count;
      result.json_string_decode_state = state.json_string_decode_state;
      result.json_string_pending_high_surrogate = state.json_string_pending_high_surrogate;
      result.json_string_length_rule_id = state.json_string_length_rule_id;
      result.json_string_pattern_state = state.json_string_pattern_state;
    }
    result.json_object_required_rule_id = state.json_object_required_rule_id;
    result.json_object_required_state_id = state.json_object_required_state_id;
    return result;
  };
  // Add the rule reference pairs, and enqueue the epsilon edges.
  for (const auto& edge : (*complete_fsm_edges_)[state.element_id]) {
    if (edge.IsEpsilon()) {
      Enqueue(make_parent_state(edge.target));
      continue;
    }

    int target;
    int ref_rule_id;
    bool is_repeat = false;
    RepeatEdgeRef repeat_info{nullptr};

    if (edge.IsRuleRef()) {
      target = edge.target;
      ref_rule_id = edge.GetRefRuleId();
    } else if (edge.IsRepeatRef()) {
      XGRAMMAR_DCHECK((*complete_fsm_edges_)[state.element_id].size() == 1)
          << "A state with a kRepeatRef edge must have exactly one outgoing edge.";
      is_repeat = true;
      repeat_info = grammar_->complete_fsm.GetRepeatEdgeInfo(edge.GetAuxIndex());
      target = edge.target;
      ref_rule_id = repeat_info.RuleId();

      if (state.repeat_count >= repeat_info.Lower() &&
          (repeat_info.Upper() == -1 || state.repeat_count <= repeat_info.Upper())) {
        Enqueue(make_parent_state(target));
      }
      if (repeat_info.Upper() != -1 && state.repeat_count >= repeat_info.Upper()) {
        continue;
      }
    } else {
      continue;
    }
    bool right_recursion_to_root = false;
    if (debug_print) {
      XGRAMMAR_LOG(INFO) << "The rule " << state.rule_id << ": "
                         << grammar_->GetRule(state.rule_id).name << " predict the new rule "
                         << ref_rule_id << ": " << grammar_->GetRule(ref_rule_id).name << ".";
    }
    const uint8_t target_flags = GetFsmStateFlags(state.rule_id, target);
    const bool can_elide_parent_completion =
        !tracks_json_string_length && state.json_object_required_rule_id < 0 && !is_repeat &&
        !(target_flags & kFsmStateHasEdges) && (target_flags & kFsmStateEnd) &&
        state.rule_start_pos != static_cast<int32_t>(rule_id_to_completable_states_.size() - 1) &&
        !RuleNeedsCaptureEvent(state.rule_id) && !RuleNeedsCaptureEvent(ref_rule_id);
    // Eliding this rule's completion would also elide the repeat-count increment in a parent
    // kRepeatRef. A state can represent several parent occurrences, so one repeated parent makes
    // the optimization unsafe for the merged state.
    bool parent_occurrence_is_repeated = false;
    if (can_elide_parent_completion && state.rule_start_pos != ParserState::kNoPrevInputPos) {
      const auto& parent_states_map = rule_id_to_completable_states_[state.rule_start_pos];
      for (const auto& [completed_rule_id, parent_state] : parent_states_map) {
        if (completed_rule_id != state.rule_id || parent_state.rule_id < 0) {
          continue;
        }
        const auto& parent_edges = (*complete_fsm_edges_)[parent_state.element_id];
        if (parent_edges.size() == 1 && parent_edges[0].IsRepeatRef() &&
            grammar_->complete_fsm.GetRepeatEdgeInfo(parent_edges[0].GetAuxIndex()).RuleId() ==
                state.rule_id) {
          parent_occurrence_is_repeated = true;
          break;
        }
      }
    }
    if (can_elide_parent_completion && !parent_occurrence_is_repeated) {
      // It's a right recursion. We can optimize it. The optimization elides the completion of
      // the parent rule, so it is disabled when either rule produces capture-history events.
      // If it's the right recursion, we need to add the ancestors of the parent state.
      if (state.rule_start_pos == ParserState::kNoPrevInputPos) {
        // In this case, we can mark the new state as the root state to speed up.
        right_recursion_to_root = true;
      } else {
        const auto in_vec = [&](const ParserState& state_) {
          return std::find_if(
                     rule_id_to_completable_states_.Back().begin(),
                     rule_id_to_completable_states_.Back().end(),
                     [&](const auto& s) {
                       return StateEqualForParsing()(s.second, state_) && s.first == ref_rule_id;
                     }
                 ) != rule_id_to_completable_states_.Back().end();
        };
        const auto& parent_states_map = rule_id_to_completable_states_[state.rule_start_pos];
        std::vector<std::pair<int32_t, ParserState>> to_added_states;
        for (const auto& parent_state_iter : parent_states_map) {
          if (parent_state_iter.first != state.rule_id) continue;
          const auto& parent_state = parent_state_iter.second;
          if (!in_vec(parent_state)) {
            to_added_states.push_back({ref_rule_id, parent_state});
          }
        }
        for (const auto& to_add_state : to_added_states) {
          rule_id_to_completable_states_.PushBackInLatestRow(to_add_state);
        }
      }
    } else {
      if (is_repeat) {
        // For kRepeatRef: store element_id = source state, preserve repeat_count
        rule_id_to_completable_states_.PushBackInLatestRow(
            {ref_rule_id, make_parent_state(state.element_id, state.repeat_count)}
        );
      } else {
        // For kRuleRef: store element_id = target (post-transition state)
        rule_id_to_completable_states_.PushBackInLatestRow({ref_rule_id, make_parent_state(target)}
        );
      }
    }

    // Check if the reference rule can be empty.
    if (!is_repeat && IsRuleNullable(ref_rule_id)) {
      Enqueue(make_parent_state(target));
    }

    // If the reference rule is not visited, we need to add it to the queue.
    const auto& ref_rule = grammar_->GetRule(ref_rule_id);
    if (ref_rule.max_chars >= 0) {
      tmp_char_budget_entered_ = true;
    }
    const auto& ref_grammar_expr_id = ref_rule.body_expr_id;

    XGRAMMAR_DCHECK(grammar_->per_rule_fsms[ref_rule_id].has_value());
    const auto& ref_fsm = grammar_->per_rule_fsms[ref_rule_id].value();
    ParserState child_state{
        ref_rule_id,
        ref_grammar_expr_id,
        ref_fsm.GetFsm().GetStart(),
        right_recursion_to_root ? ParserState::kNoPrevInputPos
                                : int32_t(rule_id_to_completable_states_.size() - 1),
        DeadlineForRule(ref_rule_id, state.budget_deadline),
        0,
        0,
        0,
        ResolveActiveTemperatureRule(ref_rule_id, state.active_temperature_rule_id),
        CharDeadlineForRule(ref_rule_id, state.char_budget_deadline)
    };
    if (tracks_json_string_length) {
      child_state.partial_codepoint = state.partial_codepoint;
      child_state.json_string_char_count = state.json_string_char_count;
      child_state.json_string_decode_state = state.json_string_decode_state;
      child_state.json_string_pending_high_surrogate = state.json_string_pending_high_surrogate;
      child_state.json_string_length_rule_id = state.json_string_length_rule_id;
      child_state.json_string_pattern_state = state.json_string_pattern_state;
    } else if (ref_rule.json_string_min_chars >= 0) {
      child_state.json_string_length_rule_id = ref_rule_id;
      if (const auto* pattern_dfa = grammar_features_->GetJSONStringPatternDFA(ref_rule_id)) {
        child_state.SetJSONStringPatternState(pattern_dfa->GetStart());
      }
    } else if (ref_rule.HasJSONNumberConstraint()) {
      child_state.SetJSONNumberRuleId(ref_rule_id);
      child_state.SetJSONNumberExponentMagnitude(
          ref_rule.json_number_multiple_of_coefficient > 0
              ? 1 % ref_rule.json_number_multiple_of_coefficient
              : 0
      );
      child_state.SetJSONNumberFlags(0);
    }
    EnterJSONObjectRequiredRule(state, ref_rule_id, &child_state);
    Enqueue(child_state);
  }
}

void EarleyParser::AdvanceByteString(
    const ParserState& state, const uint8_t ch, const GrammarExpr& sub_rule
) {
  XGRAMMAR_DCHECK(sub_rule.type == GrammarExprType::kByteString);
  XGRAMMAR_DCHECK(sub_rule.size() > state.sub_element_id);
  if (static_cast<uint8_t>(sub_rule[state.sub_element_id]) == ch) {
    auto new_state = state;
    new_state.sub_element_id++;
    if (new_state.sub_element_id == sub_rule.size()) {
      new_state.element_id++;
      new_state.sub_element_id = 0;
      Enqueue(new_state);
      // Assert: In a sequence, the bytestring can't be skipped. So the state can't be repeated.
    } else {
      EnqueueWithoutProcessing(new_state);
    }
  }
  return;
}

void EarleyParser::AdvanceCharacterClass(
    const ParserState& state, const uint8_t ch, const GrammarExpr& sub_sequence
) {
  XGRAMMAR_DCHECK(sub_sequence.type == GrammarExprType::kCharacterClass)
      << "The element type is not supported!";

  bool is_negative = static_cast<bool>(sub_sequence[0]);

  // The state is matching a UTF8 character (continuation bytes).
  if (state.sub_element_id > 0) {
    if ((ch & 0xC0) == 0x80) {
      auto new_state = state;
      new_state.sub_element_id--;
      // Accumulate the codepoint from continuation byte
      new_state.partial_codepoint = (new_state.partial_codepoint << 6) | (ch & 0x3F);

      // Check if the UTF8 character is completed.
      if (new_state.sub_element_id == 0) {
        if (is_negative) {
          // For negative classes, accept if codepoint is NOT in any range
          bool matches_range = false;
          for (int i = 1; i < sub_sequence.size(); i += 2) {
            if (new_state.partial_codepoint >= sub_sequence[i] &&
                new_state.partial_codepoint <= sub_sequence[i + 1]) {
              matches_range = true;
              break;
            }
          }
          if (!matches_range) {
            new_state.element_id++;
            new_state.partial_codepoint = 0;
            Enqueue(new_state);
          }
        } else {
          // For positive classes, accept if codepoint IS in a range
          bool matches_range = false;
          for (int i = 1; i < sub_sequence.size(); i += 2) {
            if (new_state.partial_codepoint >= sub_sequence[i] &&
                new_state.partial_codepoint <= sub_sequence[i + 1]) {
              matches_range = true;
              break;
            }
          }
          if (matches_range) {
            new_state.element_id++;
            new_state.partial_codepoint = 0;
            Enqueue(new_state);
          }
        }
      } else {
        // Check if partial codepoint could still potentially match any range
        int32_t remaining_bytes = new_state.sub_element_id;
        int32_t min_codepoint = new_state.partial_codepoint << (6 * remaining_bytes);
        int32_t max_codepoint = min_codepoint | ((1 << (6 * remaining_bytes)) - 1);

        bool could_match = false;
        for (int i = 1; i < sub_sequence.size(); i += 2) {
          int32_t lower = sub_sequence[i];
          int32_t upper = sub_sequence[i + 1];
          if (max_codepoint >= lower && min_codepoint <= upper) {
            could_match = true;
            break;
          }
        }

        // For negative classes: always continue (will verify on final byte)
        // For positive classes: only continue if some range could match
        bool should_continue = is_negative ? true : could_match;
        if (should_continue) {
          EnqueueWithoutProcessing(new_state);
        }
      }
    }
    return;
  }

  // Handle non-ASCII first bytes
  if (!isascii(ch)) {
    auto [accepted, num_bytes, partial] = HandleUTF8FirstByte(ch);
    if (!accepted) {
      return;
    }

    XGRAMMAR_DCHECK(num_bytes > 1);

    // Compute possible codepoint range for this first byte
    int32_t min_codepoint = partial << (6 * (num_bytes - 1));
    int32_t max_codepoint = min_codepoint | ((1 << (6 * (num_bytes - 1))) - 1);

    // Check if any stored range could potentially match
    bool could_match = false;
    for (int i = 1; i < sub_sequence.size(); i += 2) {
      int32_t lower = sub_sequence[i];
      int32_t upper = sub_sequence[i + 1];
      // Check for overlap between [min_codepoint, max_codepoint] and [lower, upper]
      if (max_codepoint >= lower && min_codepoint <= upper) {
        could_match = true;
        break;
      }
    }

    // For negative classes: accept if no range could match (will verify on final byte)
    // For positive classes: accept if some range could match (will verify on final byte)
    bool should_continue = is_negative ? true : could_match;

    if (should_continue) {
      auto new_state = state;
      new_state.sub_element_id = num_bytes - 1;
      new_state.partial_codepoint = partial;
      EnqueueWithoutProcessing(new_state);
    }
    return;
  }

  // ASCII handling (unchanged)
  for (int i = 1; i < sub_sequence.size(); i += 2) {
    if (static_cast<uint8_t>(sub_sequence[i]) <= ch &&
        ch <= static_cast<uint8_t>(sub_sequence[i + 1])) {
      if (!is_negative) {
        auto new_state = state;
        new_state.element_id++;
        new_state.sub_element_id = 0;
        Enqueue(new_state);
      }
      return;
    }
  }
  if (is_negative) {
    auto new_state = state;
    new_state.element_id++;
    new_state.sub_element_id = 0;
    Enqueue(new_state);
  }
}

void EarleyParser::AdvanceCharacterClassStar(
    const ParserState& state, const uint8_t ch, const GrammarExpr& sub_sequence
) {
  XGRAMMAR_DCHECK(sub_sequence.type == GrammarExprType::kCharacterClassStar)
      << "The element type is not supported!";

  bool is_negative = static_cast<bool>(sub_sequence[0]);

  // The state is matching a UTF8 character (continuation bytes).
  if (state.sub_element_id > 0) {
    if ((ch & 0xC0) == 0x80) {
      auto new_state = state;
      new_state.sub_element_id--;
      // Accumulate the codepoint from continuation byte
      new_state.partial_codepoint = (new_state.partial_codepoint << 6) | (ch & 0x3F);

      // Check if the UTF8 character is completed.
      if (new_state.sub_element_id == 0) {
        if (is_negative) {
          // For negative classes, accept if codepoint is NOT in any range
          bool matches_range = false;
          for (int i = 1; i < sub_sequence.size(); i += 2) {
            if (new_state.partial_codepoint >= sub_sequence[i] &&
                new_state.partial_codepoint <= sub_sequence[i + 1]) {
              matches_range = true;
              break;
            }
          }
          if (!matches_range) {
            new_state.partial_codepoint = 0;
            Enqueue(new_state);
          }
        } else {
          // For positive classes, accept if codepoint IS in a range
          bool matches_range = false;
          for (int i = 1; i < sub_sequence.size(); i += 2) {
            if (new_state.partial_codepoint >= sub_sequence[i] &&
                new_state.partial_codepoint <= sub_sequence[i + 1]) {
              matches_range = true;
              break;
            }
          }
          if (matches_range) {
            new_state.partial_codepoint = 0;
            Enqueue(new_state);
          }
        }
      } else {
        // Check if partial codepoint could still potentially match any range
        int32_t remaining_bytes = new_state.sub_element_id;
        int32_t min_codepoint = new_state.partial_codepoint << (6 * remaining_bytes);
        int32_t max_codepoint = min_codepoint | ((1 << (6 * remaining_bytes)) - 1);

        bool could_match = false;
        for (int i = 1; i < sub_sequence.size(); i += 2) {
          int32_t lower = sub_sequence[i];
          int32_t upper = sub_sequence[i + 1];
          if (max_codepoint >= lower && min_codepoint <= upper) {
            could_match = true;
            break;
          }
        }

        // For negative classes: always continue (will verify on final byte)
        // For positive classes: only continue if some range could match
        bool should_continue = is_negative ? true : could_match;
        if (should_continue) {
          EnqueueWithoutProcessing(new_state);
        }
      }
    }
    return;
  }

  // Handle non-ASCII first bytes
  if (!isascii(ch)) {
    auto [accepted, num_bytes, partial] = HandleUTF8FirstByte(ch);
    if (!accepted) {
      return;
    }

    XGRAMMAR_DCHECK(num_bytes > 1);

    // Compute possible codepoint range for this first byte
    int32_t min_codepoint = partial << (6 * (num_bytes - 1));
    int32_t max_codepoint = min_codepoint | ((1 << (6 * (num_bytes - 1))) - 1);

    // Check if any stored range could potentially match
    bool could_match = false;
    for (int i = 1; i < sub_sequence.size(); i += 2) {
      int32_t lower = sub_sequence[i];
      int32_t upper = sub_sequence[i + 1];
      // Check for overlap between [min_codepoint, max_codepoint] and [lower, upper]
      if (max_codepoint >= lower && min_codepoint <= upper) {
        could_match = true;
        break;
      }
    }

    // For negative classes: accept if no range could match (will verify on final byte)
    // For positive classes: accept if some range could match (will verify on final byte)
    bool should_continue = is_negative ? true : could_match;

    if (should_continue) {
      auto new_state = state;
      new_state.sub_element_id = num_bytes - 1;
      new_state.partial_codepoint = partial;
      EnqueueWithoutProcessing(new_state);
    }
    return;
  }

  // ASCII handling (unchanged)
  for (int i = 1; i < sub_sequence.size(); i += 2) {
    if (static_cast<uint8_t>(sub_sequence[i]) <= ch &&
        ch <= static_cast<uint8_t>(sub_sequence[i + 1])) {
      if (!is_negative) {
        Enqueue(state);
      }
      return;
    }
  }
  if (is_negative) {
    Enqueue(state);
  }
}

void EarleyParser::AdvanceFsm(const ParserState& state, const uint8_t ch) {
  XGRAMMAR_DCHECK(state.rule_id != -1 && grammar_->per_rule_fsms[state.rule_id].has_value());
  for (const auto& edge : (*complete_fsm_edges_)[state.element_id]) {
    if ((!edge.IsCharRange()) || ch < edge.min || ch > edge.max) {
      continue;
    }
    ParserState advanced_state = state;
    if (state.json_string_length_rule_id >= 0) {
      const auto& runtime_rule = grammar_->GetRule(state.json_string_length_rule_id);
      const bool advanced = runtime_rule.HasJSONNumberConstraint()
                                ? AdvanceJSONNumber(&advanced_state, ch)
                                : AdvanceJSONStringLength(&advanced_state, ch);
      if (!advanced) {
        continue;
      }
    }
    const uint8_t flags = GetFsmStateFlags(state.rule_id, edge.target);
    if (!(flags & kFsmStateNonTerminal) && !(flags & kFsmStateEnd) && (flags & kFsmStateScanable)) {
      EnqueueFsmTransitionWithoutProcessing(advanced_state, edge.target);
    } else {
      EnqueueFsmTransition(advanced_state, edge.target);
    }
  }
}

bool EarleyParser::AdvanceJSONStringLength(ParserState* state, uint8_t ch) const {
  XGRAMMAR_DCHECK(state->json_string_length_rule_id >= 0);
  const auto& rule = grammar_->GetRule(state->json_string_length_rule_id);
  XGRAMMAR_DCHECK(rule.json_string_min_chars >= 0);

  // 0 boundary; 1 after '\\'; 2--5 collect a \uXXXX code unit; 6 expects the '\\'
  // starting a low-surrogate escape; 7 expects its 'u'; 8--11 collect that code unit;
  // 12--14 are remaining raw UTF-8 continuation-byte counts.
  int32_t decode_state = state->GetJSONStringDecodeState();
  int32_t& partial = state->json_string_pending_high_surrogate;
  auto finish_code_point = [&](int32_t codepoint) {
    if (!AdvanceJSONStringPattern(state, codepoint)) {
      return false;
    }
    if (rule.json_string_max_chars < 0 &&
        state->json_string_char_count >= rule.json_string_min_chars) {
      // Once an unbounded rule has met its minimum, larger exact counts are language-equivalent.
      // Saturating here keeps parser states and on-demand mask-cache entries bounded.
      state->json_string_char_count = rule.json_string_min_chars;
    } else {
      ++state->json_string_char_count;
    }
    decode_state = 0;
    state->SetJSONStringDecodeState(decode_state);
    partial = 0;
    return rule.json_string_max_chars < 0 ||
           state->json_string_char_count <= rule.json_string_max_chars;
  };
  auto hex_value = [](uint8_t byte) -> int32_t {
    if ('0' <= byte && byte <= '9') return byte - '0';
    if ('a' <= byte && byte <= 'f') return byte - 'a' + 10;
    if ('A' <= byte && byte <= 'F') return byte - 'A' + 10;
    return -1;
  };

  if (decode_state == 0) {
    if (ch == '\\') {
      decode_state = 1;
      state->SetJSONStringDecodeState(decode_state);
      return true;
    }
    if (ch <= 0x7F) {
      return ch >= 0x20 && ch != '"' && finish_code_point(ch);
    }
    if (0xC2 <= ch && ch <= 0xDF) {
      decode_state = 12;
    } else if (0xE0 <= ch && ch <= 0xEF) {
      decode_state = 13;
    } else if (0xF0 <= ch && ch <= 0xF4) {
      decode_state = 14;
    } else {
      return false;
    }
    partial = ch <= 0xDF ? ch & 0x1F : ch <= 0xEF ? ch & 0x0F : ch & 0x07;
    state->SetJSONStringDecodeState(decode_state);
    return true;
  }
  if (decode_state >= 12) {
    const int32_t remaining = decode_state - 11;
    if ((ch & 0xC0) != 0x80) {
      return false;
    }
    // Tighten the first continuation byte to valid scalar-value UTF-8.
    if ((remaining == 2 && partial == 0 && ch < 0xA0) ||
        (remaining == 2 && partial == 0x0D && ch > 0x9F) ||
        (remaining == 3 && partial == 0 && ch < 0x90) ||
        (remaining == 3 && partial == 4 && ch > 0x8F)) {
      return false;
    }
    partial = (partial << 6) | (ch & 0x3F);
    if (remaining == 1) {
      return finish_code_point(partial);
    }
    --decode_state;
    state->SetJSONStringDecodeState(decode_state);
    return true;
  }
  if (decode_state == 1) {
    constexpr std::string_view kEscapes = "\"\\/bfnrt";
    constexpr std::string_view kDecoded = "\"\\/\b\f\n\r\t";
    const size_t escape_index = kEscapes.find(static_cast<char>(ch));
    if (escape_index != std::string_view::npos) {
      return finish_code_point(static_cast<uint8_t>(kDecoded[escape_index]));
    }
    if (ch == 'u') {
      decode_state = 2;
      partial = 0;
      state->SetJSONStringDecodeState(decode_state);
      return true;
    }
    return false;
  }
  if (2 <= decode_state && decode_state <= 5) {
    int32_t nibble = hex_value(ch);
    if (nibble < 0) return false;
    partial = (partial << 4) | nibble;
    if (decode_state++ != 5) {
      state->SetJSONStringDecodeState(decode_state);
      return true;
    }
    if (0xD800 <= partial && partial <= 0xDBFF) {
      // Store the high-surrogate offset plus one in the high half. This keeps the accumulator
      // positive while reserving zero for "no pending scalar".
      partial = (partial - 0xD800 + 1) << 16;
      decode_state = 6;
      state->SetJSONStringDecodeState(decode_state);
      return true;
    }
    if (0xDC00 <= partial && partial <= 0xDFFF) return false;
    return finish_code_point(partial);
  }
  if (decode_state == 6) {
    if (ch != '\\') return false;
    decode_state = 7;
    state->SetJSONStringDecodeState(decode_state);
    return true;
  }
  if (decode_state == 7) {
    if (ch != 'u') return false;
    decode_state = 8;
    state->SetJSONStringDecodeState(decode_state);
    return true;
  }
  XGRAMMAR_DCHECK(8 <= decode_state && decode_state <= 11);
  int32_t nibble = hex_value(ch);
  if (nibble < 0) return false;
  partial = static_cast<int32_t>(
      (static_cast<uint32_t>(partial) & 0xFFFF0000U) |
      (((static_cast<uint32_t>(partial) & 0xFFFFU) << 4) | static_cast<uint32_t>(nibble))
  );
  if (decode_state++ != 11) {
    state->SetJSONStringDecodeState(decode_state);
    return true;
  }
  const uint32_t encoded_pair = static_cast<uint32_t>(partial);
  const int32_t high_offset = (encoded_pair >> 16) - 1;
  const int32_t low = encoded_pair & 0xFFFF;
  if (low < 0xDC00 || low > 0xDFFF) return false;
  return finish_code_point(0x10000 + (high_offset << 10) + (low - 0xDC00));
}

bool EarleyParser::AdvanceJSONNumber(ParserState* state, uint8_t ch) const {
  const auto& rule = grammar_->GetRule(state->GetJSONNumberRuleId());
  const int32_t coefficient = rule.json_number_multiple_of_coefficient;
  uint32_t flags = static_cast<uint32_t>(state->GetJSONNumberFlags());
  if (ch == '.') {
    flags |= kNumberSeenFraction;
    state->SetJSONNumberFlags(static_cast<int32_t>(flags));
    return true;
  }
  if (ch == 'e' || ch == 'E') {
    const auto* range = grammar_features_->GetJSONNumberRange(state->GetJSONNumberRuleId());
    auto finish_significand_compare = [&](const std::optional<JSONNumberBound>& bound,
                                          int32_t shift) {
      if (bound.has_value() && GetNumberCompare(flags, shift) == kNumberCompareEqual &&
          static_cast<size_t>(state->GetJSONNumberSignificantDigits()) < bound->digits.size()) {
        SetNumberCompare(&flags, shift, kNumberCompareLess);
      }
    };
    if (range != nullptr) {
      finish_significand_compare(range->minimum, kNumberMinimumCompareShift);
      finish_significand_compare(range->maximum, kNumberMaximumCompareShift);
    }
    state->SetJSONNumberSignificantDigits(
        state->GetJSONNumberSignificantDigits() - state->GetJSONNumberFractionalDigits()
    );
    flags |= kNumberInExponent;
    state->SetJSONNumberExponentMagnitude(0);
    state->SetJSONNumberFlags(static_cast<int32_t>(flags));
    return true;
  }
  if (ch == '-' || ch == '+') {
    if (ch == '-') {
      flags |= (flags & kNumberInExponent) ? kNumberExponentNegative : kNumberNegative;
    }
    state->SetJSONNumberFlags(static_cast<int32_t>(flags));
    return true;
  }
  if (ch < '0' || ch > '9') {
    return false;
  }
  const int32_t digit = ch - '0';
  if (flags & kNumberInExponent) {
    int32_t exponent = state->GetJSONNumberExponentMagnitude();
    const int32_t exponent_digits = JSONNumberExponentDigitCount(flags, exponent);
    if (exponent_digits != 0 || digit != 0) {
      const auto* range = grammar_features_->GetJSONNumberRange(state->GetJSONNumberRuleId());
      auto advance_exponent_compare = [&](const std::optional<JSONNumberBound>& bound,
                                          bool minimum) {
        if (!bound.has_value() ||
            state->GetJSONNumberExponentCompare(minimum) != kNumberCompareEqual) {
          return;
        }
        const auto threshold = MakeJSONNumberExponentThreshold(
            *bound, state->GetJSONNumberSignificantDigits(), (flags & kNumberExponentNegative) != 0
        );
        if (threshold.negative) {
          state->SetJSONNumberExponentCompare(minimum, kNumberCompareGreater);
          return;
        }
        const int32_t threshold_digit = threshold.DigitAt(exponent_digits);
        if (digit < threshold_digit) {
          state->SetJSONNumberExponentCompare(minimum, kNumberCompareLess);
        } else if (digit > threshold_digit) {
          state->SetJSONNumberExponentCompare(minimum, kNumberCompareGreater);
        }
      };
      if (range != nullptr) {
        advance_exponent_compare(range->minimum, true);
        advance_exponent_compare(range->maximum, false);
      }

      if (flags & kNumberExponentOverflow) {
        exponent = exponent_digits == std::numeric_limits<int32_t>::max() ? exponent_digits
                                                                          : exponent_digits + 1;
      } else if (exponent > (std::numeric_limits<int32_t>::max() - digit) / 10) {
        flags |= kNumberExponentOverflow;
        exponent = exponent_digits + 1;
      } else {
        exponent = exponent * 10 + digit;
      }
    }
    state->SetJSONNumberExponentMagnitude(exponent);
    state->SetJSONNumberFlags(static_cast<int32_t>(flags));
    return true;
  }

  if (flags & kNumberSeenFraction) {
    int32_t fractional_digits = state->GetJSONNumberFractionalDigits();
    if (fractional_digits == std::numeric_limits<int32_t>::max()) {
      flags |= kNumberFractionOverflow;
    } else {
      state->SetJSONNumberFractionalDigits(fractional_digits + 1);
    }
  }

  if ((flags & kNumberNonzero) || digit != 0) {
    int32_t significant_digits = state->GetJSONNumberSignificantDigits();
    if (significant_digits == std::numeric_limits<int32_t>::max()) {
      flags |= kNumberSignificantOverflow;
    } else {
      const auto* range = grammar_features_->GetJSONNumberRange(state->GetJSONNumberRuleId());
      auto advance_compare = [&](const std::optional<JSONNumberBound>& bound, int32_t shift) {
        if (!bound.has_value() || GetNumberCompare(flags, shift) != kNumberCompareEqual) {
          return;
        }
        const int32_t bound_digit = significant_digits < static_cast<int32_t>(bound->digits.size())
                                        ? bound->digits[significant_digits] - '0'
                                        : 0;
        if (digit < bound_digit) {
          SetNumberCompare(&flags, shift, kNumberCompareLess);
        } else if (digit > bound_digit) {
          SetNumberCompare(&flags, shift, kNumberCompareGreater);
        }
      };
      if (range != nullptr) {
        advance_compare(range->minimum, kNumberMinimumCompareShift);
        advance_compare(range->maximum, kNumberMaximumCompareShift);
      }
      state->SetJSONNumberSignificantDigits(significant_digits + 1);
    }
  }

  if (digit != 0) {
    flags |= kNumberNonzero;
  }
  if (coefficient > 0) {
    int32_t trailing_zeros = flags >> kNumberTrailingShift;
    if (digit == 0) {
      state->SetJSONNumberExponentMagnitude(static_cast<int32_t>(
          static_cast<int64_t>(state->GetJSONNumberExponentMagnitude()) * 10 % coefficient
      ));
      if (trailing_zeros == kNumberTrailingMax) {
        flags |= kNumberTrailingOverflow;
      } else {
        ++trailing_zeros;
      }
    } else {
      int64_t remainder = state->GetJSONNumberRemainder();
      remainder = remainder * state->GetJSONNumberExponentMagnitude() % coefficient;
      remainder = (remainder * 10 + digit) % coefficient;
      state->SetJSONNumberRemainder(static_cast<int32_t>(remainder));
      state->SetJSONNumberExponentMagnitude(1 % coefficient);
      trailing_zeros = 0;
      flags &= ~kNumberTrailingOverflow;
    }
    flags = (flags & kNumberLowFlagsMask) | (trailing_zeros << kNumberTrailingShift);
  }
  state->SetJSONNumberFlags(static_cast<int32_t>(flags));
  return true;
}

bool EarleyParser::JSONNumberMultipleOfAccepts(const ParserState& state) const {
  const auto& rule = grammar_->GetRule(state.GetJSONNumberRuleId());
  const int32_t coefficient = rule.json_number_multiple_of_coefficient;
  XGRAMMAR_DCHECK(coefficient > 0);
  const uint32_t flags = static_cast<uint32_t>(state.GetJSONNumberFlags());
  if (!(flags & kNumberNonzero)) {
    return true;
  }
  // Fractional/trailing counts beyond their storage range require billions of source bytes and
  // are deliberately rejected. Exponent overflow is different: a short scientific-notation
  // literal can trigger it, and its saturated sign and magnitude are sufficient for an exact
  // divisibility decision.
  if (flags & (kNumberFractionOverflow | kNumberTrailingOverflow)) {
    return false;
  }
  int64_t exponent = 0;
  int64_t fractional_digits = state.GetJSONNumberFractionalDigits();
  const int64_t trailing_zeros = flags >> kNumberTrailingShift;
  if (flags & kNumberInExponent) {
    if (flags & kNumberExponentOverflow) {
      if (flags & kNumberExponentNegative) {
        return false;
      }
      exponent = std::numeric_limits<int64_t>::max() / 4;
    } else {
      exponent = state.GetJSONNumberExponentMagnitude();
    }
    if (flags & kNumberExponentNegative) {
      exponent = -exponent;
    }
  }
  const int64_t decimal_shift = static_cast<int64_t>(rule.json_number_multiple_of_decimal_scale) -
                                fractional_digits + exponent + trailing_zeros;
  if (decimal_shift < 0) {
    return false;
  }

  int32_t coprime_coefficient = coefficient;
  int32_t power_of_two = 0;
  int32_t power_of_five = 0;
  while (coprime_coefficient % 2 == 0) {
    coprime_coefficient /= 2;
    ++power_of_two;
  }
  while (coprime_coefficient % 5 == 0) {
    coprime_coefficient /= 5;
    ++power_of_five;
  }
  if (decimal_shift >= std::max(power_of_two, power_of_five)) {
    return state.GetJSONNumberRemainder() % coprime_coefficient == 0;
  }
  int64_t remainder = state.GetJSONNumberRemainder();
  for (int64_t index = 0; index < decimal_shift; ++index) {
    remainder = remainder * 10 % coefficient;
  }
  return remainder == 0;
}

bool EarleyParser::JSONNumberRangeAccepts(const ParserState& state) const {
  const auto* range = grammar_features_->GetJSONNumberRange(state.GetJSONNumberRuleId());
  if (range == nullptr) {
    return true;
  }
  const uint32_t flags = static_cast<uint32_t>(state.GetJSONNumberFlags());
  if (flags & (kNumberFractionOverflow | kNumberSignificantOverflow)) {
    return false;
  }

  const bool input_zero = !(flags & kNumberNonzero);
  const bool input_negative = !input_zero && (flags & kNumberNegative);

  auto compare_input_order = [&](const JSONNumberBound& bound, bool minimum) {
    if (!(flags & kNumberInExponent)) {
      const int64_t input_order = static_cast<int64_t>(state.GetJSONNumberSignificantDigits()) -
                                  state.GetJSONNumberFractionalDigits();
      return CompareSignedDecimalInteger(MakeSignedDecimalInteger(input_order), bound.order);
    }

    const bool exponent_negative = (flags & kNumberExponentNegative) != 0;
    const auto threshold = MakeJSONNumberExponentThreshold(
        bound, state.GetJSONNumberSignificantDigits(), exponent_negative
    );
    int32_t exponent_compare = 0;
    const int32_t exponent_digits =
        JSONNumberExponentDigitCount(flags, state.GetJSONNumberExponentMagnitude());
    if (threshold.negative) {
      exponent_compare = 1;
    } else if (threshold.IsZero()) {
      exponent_compare = exponent_digits == 0 ? 0 : 1;
    } else if (exponent_digits != threshold.DigitCount()) {
      exponent_compare = exponent_digits < threshold.DigitCount() ? -1 : 1;
    } else {
      const int32_t tracked = state.GetJSONNumberExponentCompare(minimum);
      exponent_compare = tracked == kNumberCompareLess      ? -1
                         : tracked == kNumberCompareGreater ? 1
                                                            : 0;
    }
    return exponent_negative ? -exponent_compare : exponent_compare;
  };

  auto compare_to_bound = [&](const JSONNumberBound& bound, int32_t compare_shift) {
    const bool bound_zero = bound.digits == "0";
    if (input_zero || bound_zero) {
      if (input_zero && bound_zero) return 0;
      if (input_zero) return bound.negative ? 1 : -1;
      return input_negative ? -1 : 1;
    }
    if (input_negative != bound.negative) {
      return input_negative ? -1 : 1;
    }

    int32_t magnitude_compare = 0;
    const int32_t order_compare =
        compare_input_order(bound, compare_shift == kNumberMinimumCompareShift);
    if (order_compare != 0) {
      magnitude_compare = order_compare;
    } else {
      const int32_t tracked = GetNumberCompare(flags, compare_shift);
      if (tracked == kNumberCompareLess) {
        magnitude_compare = -1;
      } else if (tracked == kNumberCompareGreater) {
        magnitude_compare = 1;
      } else if (!(flags & kNumberInExponent) &&
                 static_cast<size_t>(state.GetJSONNumberSignificantDigits()) <
                     bound.digits.size()) {
        // Bound digits are canonicalized without trailing zeros, so an unvisited suffix contains
        // a nonzero digit and is strictly greater than the input's implicit zero suffix.
        magnitude_compare = -1;
      }
    }
    return input_negative ? -magnitude_compare : magnitude_compare;
  };

  if (range->minimum.has_value()) {
    const int32_t compare = compare_to_bound(*range->minimum, kNumberMinimumCompareShift);
    if (compare < 0 || (compare == 0 && range->exclusive_minimum)) {
      return false;
    }
  }
  if (range->maximum.has_value()) {
    const int32_t compare = compare_to_bound(*range->maximum, kNumberMaximumCompareShift);
    if (compare > 0 || (compare == 0 && range->exclusive_maximum)) {
      return false;
    }
  }
  return true;
}

bool EarleyParser::AdvanceJSONStringPattern(ParserState* state, int32_t codepoint) const {
  const auto* pattern_dfa =
      grammar_features_->GetJSONStringPatternDFA(state->json_string_length_rule_id);
  if (pattern_dfa == nullptr) {
    return true;
  }
  int32_t pattern_state = state->GetJSONStringPatternState();
  XGRAMMAR_DCHECK(pattern_state >= 0);
  auto advance_byte = [&](uint8_t byte) {
    const int32_t next_state = grammar_features_->GetJSONStringPatternTransition(
        state->json_string_length_rule_id, pattern_state, byte
    );
    if (next_state < 0) {
      return false;
    }
    pattern_state = next_state;
    return true;
  };
  if (codepoint <= 0x7F) {
    if (!advance_byte(static_cast<uint8_t>(codepoint))) {
      return false;
    }
  } else {
    uint8_t encoded[4];
    int32_t width;
    if (codepoint <= 0x7FF) {
      encoded[0] = 0xC0 | (codepoint >> 6);
      encoded[1] = 0x80 | (codepoint & 0x3F);
      width = 2;
    } else if (codepoint <= 0xFFFF) {
      encoded[0] = 0xE0 | (codepoint >> 12);
      encoded[1] = 0x80 | ((codepoint >> 6) & 0x3F);
      encoded[2] = 0x80 | (codepoint & 0x3F);
      width = 3;
    } else {
      encoded[0] = 0xF0 | (codepoint >> 18);
      encoded[1] = 0x80 | ((codepoint >> 12) & 0x3F);
      encoded[2] = 0x80 | ((codepoint >> 6) & 0x3F);
      encoded[3] = 0x80 | (codepoint & 0x3F);
      width = 4;
    }
    for (int32_t index = 0; index < width; ++index) {
      if (!advance_byte(encoded[index])) {
        return false;
      }
    }
  }
  state->SetJSONStringPatternState(pattern_state);
  return true;
}

bool EarleyParser::JSONStringRuntimeConstraintsAllowToken(
    ParserState state, const std::string& token
) const {
  XGRAMMAR_DCHECK(state.json_string_length_rule_id >= 0);
  const auto& rule = grammar_->GetRule(state.json_string_length_rule_id);
  const auto* pattern_dfa =
      grammar_features_->GetJSONStringPatternDFA(state.json_string_length_rule_id);
  for (uint8_t byte : token) {
    if (state.GetJSONStringDecodeState() == 0 && byte == '"') {
      return state.json_string_pending_high_surrogate == 0 &&
             state.json_string_char_count >= rule.json_string_min_chars &&
             (rule.json_string_max_chars < 0 ||
              state.json_string_char_count <= rule.json_string_max_chars) &&
             (pattern_dfa == nullptr || pattern_dfa->IsEndState(state.GetJSONStringPatternState()));
    }
    if (!AdvanceJSONStringLength(&state, byte)) {
      return false;
    }
  }
  return true;
}

void EarleyParser::ScanAtomicToken(const ParserState& state, int32_t token_id) {
  if (state.rule_id == -1) return;
  XGRAMMAR_DCHECK(grammar_->per_rule_fsms[state.rule_id].has_value());
  const auto& current_fsm = grammar_->per_rule_fsms[state.rule_id].value();
  for (const auto& edge : (*complete_fsm_edges_)[state.element_id]) {
    bool matched = false;
    if (edge.IsToken()) {
      auto info = current_fsm.GetFsm().GetFsm().GetTokenEdgeInfo(edge.GetAuxIndex());
      matched = info.Contains(token_id);
    } else if (edge.IsExcludeToken()) {
      auto info = current_fsm.GetFsm().GetFsm().GetExcludeTokenEdgeInfo(edge.GetAuxIndex());
      matched = info.Accepts(token_id);
    }
    if (!matched) continue;
    auto new_state = state;
    new_state.element_id = edge.target;
    const uint8_t flags = GetFsmStateFlags(state.rule_id, edge.target);
    if (!(flags & kFsmStateNonTerminal) && !(flags & kFsmStateEnd) && (flags & kFsmStateScanable)) {
      EnqueueWithoutProcessing(std::move(new_state));
    } else {
      Enqueue(std::move(new_state));
    }
  }
}

bool EarleyParser::AdvanceAtomicToken(
    int32_t token_id, bool debug_print, int32_t token_char_count
) {
  XGRAMMAR_DCHECK(tmp_process_state_queue_.empty())
      << "The tmp_process_state_queue_ should be empty before AdvanceAtomicToken.";
  tmp_states_visited_in_queue_.Clear();
  tmp_states_to_be_added_.clear();
  tmp_accept_stop_token_ = false;
  tmp_completed_lazy_occurrences_.clear();
  if (has_char_budget_rules_) {
    tmp_char_budget_entered_ = char_budget_entry_history_.back();
    char_count_history_.push_back(GetCurrentCharIndex() + token_char_count);
  }
  const auto& latest_states = scanable_state_history_[scanable_state_history_.size() - 1];
  for (const auto& state : latest_states) {
    if (skip_expired_states_ && IsExpiredState(state)) {
      continue;
    }
    ScanAtomicToken(state, token_id);
  }
  if (tmp_process_state_queue_.empty() && tmp_states_to_be_added_.empty()) {
    if (has_char_budget_rules_) {
      char_count_history_.pop_back();
    }
    return false;
  }
  rule_id_to_completable_states_.PushBackEmpty();
  if (capture_tracking_) {
    capture_event_history_.PushBack(std::vector<CaptureEvent>());
  }
  while (!tmp_process_state_queue_.empty()) {
    const ParserState& state = *tmp_process_state_queue_.front();
    tmp_process_state_queue_.pop();
    auto [scanable, completable] = Predict(state, debug_print);
    if (completable) {
      Complete(state, debug_print);
    }
    if (scanable) {
      tmp_states_to_be_added_.push_back(&state);
    }
  }
  if (!tmp_completed_lazy_occurrences_.empty()) {
    RemoveCommittedLazyStates();
  }
  is_completed_.push_back(tmp_accept_stop_token_);
  PushLatestScanableStates();
  if (has_char_budget_rules_) {
    char_budget_entry_history_.push_back(tmp_char_budget_entered_);
  }
  return true;
}

const ParserState* RepeatDetector::InsertInSet(const ParserState& state) {
  if (!using_set_) {
    for (const auto& existing : visited_vector_) {
      visited_set_.insert(existing);
    }
    using_set_ = true;
  }
  const auto [it, inserted] = visited_set_.insert(state);
  if (inserted) {
    ++size_;
    return &*it;
  }
  return nullptr;
}

void RepeatDetector::ClearSet() { visited_set_.clear(); }

}  // namespace xgrammar
