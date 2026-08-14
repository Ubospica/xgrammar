/*!
 *  Copyright (c) 2025 by Contributors
 * \file xgrammar/earley_parser.h
 * \brief The header for the definition of the Earley parser.
 */

#ifndef XGRAMMAR_EARLEY_PARSER_H_
#define XGRAMMAR_EARLEY_PARSER_H_
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <ostream>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "grammar_impl.h"
#include "regex_fsm_cache.h"
#include "support/compact_2d_array.h"
#include "support/signed_decimal.h"
#include "support/utils.h"
#include "xgrammar/grammar.h"

namespace xgrammar {

struct ParserState;

class ReusableStatePointerQueue {
 public:
  bool empty() const { return head_ == entries_.size(); }

  const ParserState* front() const { return entries_[head_]; }

  void pop() { ++head_; }

  void push(const ParserState* state) {
    if (empty()) {
      entries_.clear();
      head_ = 0;
    }
    entries_.push_back(state);
  }

 private:
  std::vector<const ParserState*> entries_;
  size_t head_ = 0;
};

/*!
 * \brief The state of the Earley parser.
 * In the implementation, a rule can only be a kchoices or a ktagdispatch.
 * A kchoices rule must be composed of some ksequence rules, or a kemptyrule.
 * In the ksequence, every element in the sequence must be a kbytestring, a
 * kcharacterclass, a kcharacterclassstar, or a rule reference.
 *
 * - rule_id: The id of the rule.
 * - sequence_id: The id of the sequence in the rule.
 * - element_id: The id of the element in the sequence, or the id of the node in
 *   the tag dispatch fsm.
 * - rule_start_pos: The id of the parent node in the Earley parser. i.e. the rule
 *   is predicted from the k-th character.
 * - sub_element_id: The id of the sub element in the current element, i.e.:
 *   - kbytestring: the id of the byte in the string.
 *   - kcharacterclass: How many bytes are left to be read in the utf8 character.
 *   - kcharacterclassstar: How many bytes are left to be read in the utf8 character.
 */
struct ParserState {
  constexpr ParserState() = default;

  constexpr ParserState(
      const int32_t& rule_id,
      const int32_t& sequence_id,
      const int32_t& element_id,
      const int32_t& rule_start_pos,
      const int32_t& budget_deadline = -1,
      const int32_t& sub_element_id = 0,
      const int32_t& repeat_count = 0,
      const int32_t& partial_codepoint = 0,
      const int32_t& active_temperature_rule_id = -1,
      const int32_t& char_budget_deadline = -1,
      const int32_t& json_string_char_count = 0,
      const int32_t& json_string_decode_state = 0,
      const int32_t& json_string_pending_high_surrogate = 0,
      const int32_t& json_string_length_rule_id = -1,
      const int32_t& json_string_pattern_state = -1,
      const int32_t& json_object_required_rule_id = -1,
      const int32_t& json_object_required_state_id = -1
  )
      : rule_id(rule_id),
        sequence_id(sequence_id),
        element_id(element_id),
        rule_start_pos(rule_start_pos),
        budget_deadline(budget_deadline),
        sub_element_id(sub_element_id),
        repeat_count(repeat_count),
        partial_codepoint(partial_codepoint),
        active_temperature_rule_id(active_temperature_rule_id),
        char_budget_deadline(char_budget_deadline),
        json_string_char_count(json_string_char_count),
        json_string_decode_state(json_string_decode_state),
        json_string_pending_high_surrogate(json_string_pending_high_surrogate),
        json_string_length_rule_id(json_string_length_rule_id),
        json_string_pattern_state(json_string_pattern_state),
        json_object_required_rule_id(json_object_required_rule_id),
        json_object_required_state_id(json_object_required_state_id) {}

  /*!
   * \brief A rule_start_pos value of kNoPrevInputPos means this ParserState is the root of the
   * parsing stack.
   */
  static constexpr int32_t kNoPrevInputPos = -1;

  /*! \brief The rule's id. */
  int32_t rule_id = -1;

  /*! \brief Which choice in this rule is selected. */
  int32_t sequence_id = -1;

  /*!
   * \brief Which element of the choice sequence is to be visited. When the current sequence is
   * a tag dispatch rule, this element id is the current node.
   */
  int32_t element_id = -1;

  /*! \brief The position of the state, i.e. from which position, the rule starts. */
  int32_t rule_start_pos = -1;

  /*! \brief The last token index this state's derivation may consume, from the token budget
   * (Rule::max_tokens) of the rule it is inside; -1 means unlimited. Set when a budgeted rule
   * is predicted and inherited by the states inside it. */
  int32_t budget_deadline = -1;

  /*! \brief The id of the sub element in the current element of the sequence. */
  int32_t sub_element_id = 0;

  /*! \brief The number of times the element is repeated. It will be used in kRepeat.*/
  int32_t repeat_count = 0;

  /*! \brief Partial codepoint accumulated during UTF-8 decoding for positive character classes. */
  int32_t partial_codepoint = 0;

  /*! \brief The innermost active rule that specifies a sampling temperature. */
  int32_t active_temperature_rule_id = -1;

  /*! \brief The number of Unicode codepoints this derivation may consume before its active
   * character budget expires; -1 means unlimited. Stored as an absolute input position. */
  int32_t char_budget_deadline = -1;

  /*! \brief Runtime state for hard decoded JSON-string length constraints. Decode state 0 is a
   * code-point boundary; positive values represent an in-progress raw UTF-8 or JSON escape. */
  int32_t json_string_char_count = 0;
  int32_t json_string_decode_state = 0;
  int32_t json_string_pending_high_surrogate = 0;
  /*! \brief Rule whose decoded JSON-string bounds own the active decoder, or -1 when inactive. */
  int32_t json_string_length_rule_id = -1;
  /*! \brief State in the active rule's decoded JSON-string pattern DFA, or -1 when absent. */
  int32_t json_string_pattern_state = -1;

  /*! \brief The innermost active large-object `required` constraint and its interned bitset. */
  int32_t json_object_required_rule_id = -1;
  int32_t json_object_required_state_id = -1;

  // The fields above plus partial_codepoint are mutually exclusive runtime-constraint storage.
  // A constrained number reuses them as {significand remainder, fractional digit count,
  // exponent magnitude, owner rule id, parse/comparison flags, significant digit count}. The two
  // exponent-vs-bound comparisons occupy otherwise unused high bits: two above the <= 1e9
  // multipleOf remainder and one above each nonnegative count.
  static constexpr uint32_t kJSONNumberRemainderMask = (1U << 30) - 1;
  static constexpr uint32_t kJSONNumberCountMask = (1U << 31) - 1;

  int32_t GetJSONNumberRemainder() const {
    return static_cast<uint32_t>(json_string_char_count) & kJSONNumberRemainderMask;
  }
  void SetJSONNumberRemainder(int32_t value) {
    json_string_char_count = static_cast<int32_t>(
        (static_cast<uint32_t>(json_string_char_count) & ~kJSONNumberRemainderMask) |
        static_cast<uint32_t>(value)
    );
  }
  int32_t GetJSONNumberFractionalDigits() const {
    return static_cast<uint32_t>(json_string_decode_state) & kJSONNumberCountMask;
  }
  void SetJSONNumberFractionalDigits(int32_t value) {
    json_string_decode_state = static_cast<int32_t>(
        (static_cast<uint32_t>(json_string_decode_state) & ~kJSONNumberCountMask) |
        static_cast<uint32_t>(value)
    );
  }
  int32_t GetJSONNumberExponentMagnitude() const {
    return static_cast<uint32_t>(json_string_pending_high_surrogate) & kJSONNumberCountMask;
  }
  void SetJSONNumberExponentMagnitude(int32_t value) {
    json_string_pending_high_surrogate = static_cast<int32_t>(
        (static_cast<uint32_t>(json_string_pending_high_surrogate) & ~kJSONNumberCountMask) |
        static_cast<uint32_t>(value)
    );
  }
  int32_t GetJSONNumberExponentCompare(bool minimum) const {
    if (minimum) {
      return static_cast<uint32_t>(json_string_char_count) >> 30;
    }
    return (static_cast<uint32_t>(json_string_decode_state) >> 31) |
           ((static_cast<uint32_t>(json_string_pending_high_surrogate) >> 31) << 1);
  }
  void SetJSONNumberExponentCompare(bool minimum, int32_t compare) {
    if (minimum) {
      json_string_char_count = static_cast<int32_t>(
          (static_cast<uint32_t>(json_string_char_count) & kJSONNumberRemainderMask) |
          (static_cast<uint32_t>(compare) << 30)
      );
      return;
    }
    json_string_decode_state = static_cast<int32_t>(
        (static_cast<uint32_t>(json_string_decode_state) & kJSONNumberCountMask) |
        ((static_cast<uint32_t>(compare) & 1U) << 31)
    );
    json_string_pending_high_surrogate = static_cast<int32_t>(
        (static_cast<uint32_t>(json_string_pending_high_surrogate) & kJSONNumberCountMask) |
        ((static_cast<uint32_t>(compare) >> 1) << 31)
    );
  }
  int32_t GetJSONNumberRuleId() const { return json_string_length_rule_id; }
  void SetJSONNumberRuleId(int32_t value) { json_string_length_rule_id = value; }
  int32_t GetJSONNumberFlags() const { return json_string_pattern_state; }
  void SetJSONNumberFlags(int32_t value) { json_string_pattern_state = value; }
  int32_t GetJSONNumberSignificantDigits() const { return partial_codepoint; }
  void SetJSONNumberSignificantDigits(int32_t value) { partial_codepoint = value; }

  int32_t GetJSONStringDecodeState() const { return json_string_decode_state; }

  void SetJSONStringDecodeState(int32_t state) {
    XGRAMMAR_DCHECK(0 <= state && state <= 14);
    json_string_decode_state = state;
  }

  int32_t GetJSONStringPatternState() const { return json_string_pattern_state; }

  void SetJSONStringPatternState(int32_t state) {
    XGRAMMAR_DCHECK(state >= -1);
    json_string_pattern_state = state;
  }

  /*!
   * \brief Lexicographic order over all fields. It is only used to sort the states for
   * deterministic serialization, and is not needed during parsing.
   */
  bool operator<(const ParserState& other) const {
    if (rule_id != other.rule_id) return rule_id < other.rule_id;
    if (sequence_id != other.sequence_id) return sequence_id < other.sequence_id;
    if (element_id != other.element_id) return element_id < other.element_id;
    if (rule_start_pos != other.rule_start_pos) return rule_start_pos < other.rule_start_pos;
    if (budget_deadline != other.budget_deadline) return budget_deadline < other.budget_deadline;
    if (sub_element_id != other.sub_element_id) return sub_element_id < other.sub_element_id;
    if (repeat_count != other.repeat_count) return repeat_count < other.repeat_count;
    if (partial_codepoint != other.partial_codepoint) {
      return partial_codepoint < other.partial_codepoint;
    }
    if (active_temperature_rule_id != other.active_temperature_rule_id) {
      return active_temperature_rule_id < other.active_temperature_rule_id;
    }
    if (char_budget_deadline != other.char_budget_deadline) {
      return char_budget_deadline < other.char_budget_deadline;
    }
    if (json_string_char_count != other.json_string_char_count) {
      return json_string_char_count < other.json_string_char_count;
    }
    if (json_string_decode_state != other.json_string_decode_state) {
      return json_string_decode_state < other.json_string_decode_state;
    }
    if (json_string_pending_high_surrogate != other.json_string_pending_high_surrogate) {
      return json_string_pending_high_surrogate < other.json_string_pending_high_surrogate;
    }
    if (json_string_length_rule_id != other.json_string_length_rule_id) {
      return json_string_length_rule_id < other.json_string_length_rule_id;
    }
    if (json_string_pattern_state != other.json_string_pattern_state) {
      return json_string_pattern_state < other.json_string_pattern_state;
    }
    if (json_object_required_rule_id != other.json_object_required_rule_id) {
      return json_object_required_rule_id < other.json_object_required_rule_id;
    }
    return json_object_required_state_id < other.json_object_required_state_id;
  }

  friend std::ostream& operator<<(std::ostream& os, const ParserState& state) {
    os << state.ToString();
    return os;
  }

  std::string ToString() const {
    std::string result = "ParserState(rule_id=" + std::to_string(rule_id) +
                         ", sequence_id=" + std::to_string(sequence_id) +
                         ", element_id=" + std::to_string(element_id) +
                         ", rule_start_pos=" + std::to_string(rule_start_pos) +
                         ", sub_element_id=" + std::to_string(sub_element_id);
    if (repeat_count != 0) {
      result += ", repeat_count=" + std::to_string(repeat_count);
    }
    if (partial_codepoint != 0) {
      result += ", partial_codepoint=" + std::to_string(partial_codepoint);
    }
    if (budget_deadline != -1) {
      result += ", budget_deadline=" + std::to_string(budget_deadline);
    }
    if (active_temperature_rule_id != -1) {
      result += ", active_temperature_rule_id=" + std::to_string(active_temperature_rule_id);
    }
    if (char_budget_deadline != -1) {
      result += ", char_budget_deadline=" + std::to_string(char_budget_deadline);
    }
    if (json_string_length_rule_id >= 0 || json_string_char_count != 0 ||
        json_string_decode_state != 0 || json_string_pending_high_surrogate != 0) {
      result += ", json_string_char_count=" + std::to_string(json_string_char_count) +
                ", json_string_decode_state=" + std::to_string(json_string_decode_state) +
                ", json_string_pending_high_surrogate=" +
                std::to_string(json_string_pending_high_surrogate) +
                ", json_string_length_rule_id=" + std::to_string(json_string_length_rule_id) +
                ", json_string_pattern_state=" + std::to_string(GetJSONStringPatternState());
    }
    if (json_object_required_rule_id >= 0) {
      result += ", json_object_required_rule_id=" + std::to_string(json_object_required_rule_id) +
                ", json_object_required_state_id=" + std::to_string(json_object_required_state_id);
    }
    result += ")";
    return result;
  }
};

XGRAMMAR_MEMBER_ARRAY(
    ParserState,
    &ParserState::rule_id,
    &ParserState::sequence_id,
    &ParserState::element_id,
    &ParserState::rule_start_pos,
    &ParserState::budget_deadline,
    &ParserState::sub_element_id,
    &ParserState::repeat_count,
    &ParserState::partial_codepoint,
    &ParserState::active_temperature_rule_id,
    &ParserState::char_budget_deadline,
    &ParserState::json_string_char_count,
    &ParserState::json_string_decode_state,
    &ParserState::json_string_pending_high_surrogate,
    &ParserState::json_string_length_rule_id,
    &ParserState::json_string_pattern_state,
    &ParserState::json_object_required_rule_id,
    &ParserState::json_object_required_state_id
);

/*! \brief Compare the same parser-state prefix when the candidate's element id is supplied
 * separately by an FSM transition. */
inline bool EqualParserStatePrefixForFsmTransition(
    const ParserState& existing, const ParserState& source, int32_t target_element_id
) {
  static_assert(
      offsetof(ParserState, json_string_char_count) - offsetof(ParserState, rule_start_pos) ==
          7 * sizeof(int32_t),
      "ParserState transition prefix must not contain padding"
  );
  return existing.rule_id == source.rule_id && existing.sequence_id == source.sequence_id &&
         existing.element_id == target_element_id &&
         std::memcmp(
             &existing.rule_start_pos,
             &source.rule_start_pos,
             offsetof(ParserState, json_string_char_count) - offsetof(ParserState, rule_start_pos)
         ) == 0;
}

/*!
 * \brief Hash of a state used as the key of the adaptive token mask cache. The token mask of a
 * state does not depend on parent position or repetition counters. Runtime JSON-string decoder
 * fields are included because they change the remaining hard length language.
 */
class StateHashForCache {
 public:
  size_t operator()(const ParserState& state) const {
    if (state.json_string_length_rule_id < 0 && state.json_object_required_rule_id < 0) {
      return HashCombine(state.rule_id, state.sequence_id, state.element_id, state.sub_element_id);
    }
    return HashCombine(
        state.rule_id,
        state.sequence_id,
        state.element_id,
        state.sub_element_id,
        state.partial_codepoint,
        state.json_string_char_count,
        state.json_string_decode_state,
        state.json_string_pending_high_surrogate,
        state.json_string_length_rule_id,
        state.json_string_pattern_state,
        state.json_object_required_rule_id,
        state.json_object_required_state_id
    );
  }
};

/*!
 * \brief Equality of states used as the key of the adaptive token mask cache. Compares the same
 * fields as StateHashForCache hashes.
 */
class StateEqualForCache {
 public:
  bool operator()(const ParserState& lhs, const ParserState& rhs) const {
    if (lhs.rule_id != rhs.rule_id || lhs.sequence_id != rhs.sequence_id ||
        lhs.element_id != rhs.element_id || lhs.sub_element_id != rhs.sub_element_id ||
        lhs.json_string_length_rule_id != rhs.json_string_length_rule_id ||
        lhs.json_object_required_rule_id != rhs.json_object_required_rule_id ||
        lhs.json_object_required_state_id != rhs.json_object_required_state_id) {
      return false;
    }
    if (lhs.json_string_length_rule_id < 0) {
      return true;
    }
    return lhs.partial_codepoint == rhs.partial_codepoint &&
           lhs.json_string_char_count == rhs.json_string_char_count &&
           lhs.json_string_decode_state == rhs.json_string_decode_state &&
           lhs.json_string_pending_high_surrogate == rhs.json_string_pending_high_surrogate &&
           lhs.json_string_pattern_state == rhs.json_string_pattern_state;
  }
};

/*!
 * \brief When matching the state, we need to consider the rule_start_pos, since if two states
 * don't have the same rule_start_pos, they are not the same state.
 */
class StateEqualForParsing {
 public:
  bool operator()(const ParserState& lhs, const ParserState& rhs) const {
    if (lhs.rule_id != rhs.rule_id || lhs.sequence_id != rhs.sequence_id ||
        lhs.element_id != rhs.element_id || lhs.rule_start_pos != rhs.rule_start_pos ||
        lhs.sub_element_id != rhs.sub_element_id || lhs.repeat_count != rhs.repeat_count ||
        lhs.partial_codepoint != rhs.partial_codepoint ||
        lhs.budget_deadline != rhs.budget_deadline ||
        lhs.active_temperature_rule_id != rhs.active_temperature_rule_id ||
        lhs.char_budget_deadline != rhs.char_budget_deadline ||
        lhs.json_string_length_rule_id != rhs.json_string_length_rule_id ||
        lhs.json_object_required_rule_id != rhs.json_object_required_rule_id ||
        lhs.json_object_required_state_id != rhs.json_object_required_state_id) {
      return false;
    }
    if (lhs.json_string_length_rule_id < 0) {
      return true;
    }
    return lhs.json_string_char_count == rhs.json_string_char_count &&
           lhs.json_string_decode_state == rhs.json_string_decode_state &&
           lhs.json_string_pending_high_surrogate == rhs.json_string_pending_high_surrogate &&
           lhs.json_string_pattern_state == rhs.json_string_pattern_state;
  }
};

/*!
 * \brief This class is used to hash the ParserState for parsing.
 * If two ParserStates don't have the same rule_start_pos, they are not the same state.
 */
class StateHashForParsing {
 public:
  size_t operator()(const ParserState& state) const {
    if (state.json_string_length_rule_id < 0 && state.json_object_required_rule_id < 0) {
      return HashCombine(
          state.rule_id,
          state.sequence_id,
          state.element_id,
          state.rule_start_pos,
          state.sub_element_id,
          state.repeat_count,
          state.partial_codepoint,
          state.budget_deadline,
          state.active_temperature_rule_id,
          state.char_budget_deadline
      );
    }
    return HashCombine(
        state.rule_id,
        state.sequence_id,
        state.element_id,
        state.rule_start_pos,
        state.sub_element_id,
        state.repeat_count,
        state.partial_codepoint,
        state.budget_deadline,
        state.active_temperature_rule_id,
        state.char_budget_deadline,
        state.json_string_char_count,
        state.json_string_decode_state,
        state.json_string_pending_high_surrogate,
        state.json_string_length_rule_id,
        state.json_string_pattern_state,
        state.json_object_required_rule_id,
        state.json_object_required_state_id
    );
  }
};

/*! \brief This class is used to detect the repeated states. */
class RepeatDetector {
 private:
  const int transition_threshold_;

  std::vector<ParserState> visited_vector_;

  std::unordered_set<ParserState, StateHashForParsing, StateEqualForParsing> visited_set_;

  int size_ = 0;
  bool using_set_ = false;

  const ParserState* InsertInSet(const ParserState& state);

  void ClearSet();

 public:
  RepeatDetector(const int transition_threshold = 50)
      : transition_threshold_(transition_threshold), size_(0) {
    visited_vector_.resize(transition_threshold_);
  }

  /*! \brief Insert a state only if absent and return its stable address, or nullptr. */
  const ParserState* InsertIfAbsent(const ParserState& state) {
    if (!using_set_ && size_ < transition_threshold_) {
      for (int i = 0; i < size_; ++i) {
        if (StateEqualForParsing()(state, visited_vector_[i])) {
          return nullptr;
        }
      }
      visited_vector_[size_] = state;
      return &visited_vector_[size_++];
    }
    return InsertInSet(state);
  }

  /*! \brief Insert a copy of an FSM state with a new element id. */
  const ParserState* InsertFsmTransitionIfAbsent(
      const ParserState& state, int32_t target_element_id
  ) {
    if (!using_set_ && size_ < transition_threshold_) {
      for (int i = 0; i < size_; ++i) {
        const ParserState& existing = visited_vector_[i];
        if (EqualParserStatePrefixForFsmTransition(existing, state, target_element_id) &&
            existing.json_string_length_rule_id == state.json_string_length_rule_id &&
            existing.json_object_required_rule_id == state.json_object_required_rule_id &&
            existing.json_object_required_state_id == state.json_object_required_state_id &&
            (state.json_string_length_rule_id < 0 ||
             (existing.json_string_char_count == state.json_string_char_count &&
              existing.json_string_decode_state == state.json_string_decode_state &&
              existing.json_string_pending_high_surrogate ==
                  state.json_string_pending_high_surrogate &&
              existing.json_string_pattern_state == state.json_string_pattern_state))) {
          return nullptr;
        }
      }
      ParserState* inserted = &visited_vector_[size_++];
      *inserted = state;
      inserted->element_id = target_element_id;
      return inserted;
    }
    ParserState transitioned = state;
    transitioned.element_id = target_element_id;
    return InsertInSet(transitioned);
  }

  /*! \brief Reset the detector. */
  void Clear() {
    if (using_set_) {
      ClearSet();
    }
    size_ = 0;
    using_set_ = false;
  }
};

/*! \brief A concrete occurrence of a captured rule in an Earley parent chain. */
struct CaptureOccurrence {
  /*! \brief The id of the captured rule. */
  int32_t rule_id;
  /*! \brief The position where the rule occurrence started. */
  int32_t start_pos;

  bool operator==(const CaptureOccurrence& other) const {
    return rule_id == other.rule_id && start_pos == other.start_pos;
  }
};

/*!
 * \brief A completion event of a captured rule, recorded when the rule is completed during
 * parsing. The matched span is [start_pos, r) in input positions, where r is the position (i.e.
 * the history row) at which the event is recorded.
 */
struct CaptureEvent {
  /*! \brief The id of the completed rule. */
  int32_t rule_id;
  /*! \brief The position where the rule started matching. kNoPrevInputPos means position 0 (the
   * rule acts as the root). */
  int32_t start_pos;
  /*! \brief The unadjusted start position of this rule occurrence. This differs from start_pos
   * for the zero-width event inserted after a dynamic-dispatch marker. */
  int32_t occurrence_start_pos;
  /*! \brief Number of trailing bytes hidden only from this rule's own capture for this
   * completion. */
  int32_t hidden_suffix_bytes = 0;
  /*! \brief Number of trailing bytes hidden from every containing capture for this completion. */
  int32_t hidden_stop_bytes = 0;
  /*! \brief The captured rule occurrences whose concrete Earley parent chains contain this stop
   * completion. Includes this rule's occurrence when the rule itself is captured. */
  std::vector<CaptureOccurrence> stop_capture_targets;
};

/*! \brief Immutable grammar-wide features shared by short-lived Earley parsers. */
struct EarleyParserGrammarFeatures {
  enum FsmStateFlag : uint8_t {
    kFsmStateInitialized = 1 << 0,
    kFsmStateScanable = 1 << 1,
    kFsmStateNonTerminal = 1 << 2,
    kFsmStateEnd = 1 << 3,
    kFsmStateHasEdges = 1 << 4,
  };

  struct JSONStringPatternDFA {
    static constexpr uint16_t kNoTransition = std::numeric_limits<uint16_t>::max();
    // Runtime property-name exclusions share ParserState::json_string_pattern_state with the
    // ordinary pattern DFA. The base DFA is limited to 4,096 states (12 bits); the remaining 19
    // positive bits track a sparse exact-string trie, with state zero meaning the input has
    // already diverged from every excluded string.
    static constexpr uint32_t kExclusionStateBits = 19;
    static constexpr uint32_t kExclusionStateMask = (1U << kExclusionStateBits) - 1;

    int32_t start = -1;
    std::vector<uint8_t> is_end_state;
    std::vector<uint16_t> dense_transitions;
    std::vector<uint8_t> exclusion_is_terminal;
    std::vector<std::vector<std::pair<uint8_t, uint32_t>>> exclusion_transitions;

    int32_t GetStart() const { return start; }

    bool IsEndState(int32_t state) const {
      XGRAMMAR_DCHECK(state >= 0);
      uint32_t base_state = static_cast<uint32_t>(state);
      uint32_t exclusion_state = 0;
      if (!exclusion_transitions.empty()) {
        exclusion_state = base_state & kExclusionStateMask;
        base_state >>= kExclusionStateBits;
      }
      XGRAMMAR_DCHECK(base_state < is_end_state.size());
      XGRAMMAR_DCHECK(exclusion_state == 0 || exclusion_state < exclusion_is_terminal.size());
      return is_end_state[base_state] &&
             (exclusion_state == 0 || !exclusion_is_terminal[exclusion_state]);
    }

    int32_t Advance(int32_t state, uint8_t byte) const {
      XGRAMMAR_DCHECK(state >= 0);
      uint32_t base_state = static_cast<uint32_t>(state);
      uint32_t exclusion_state = 0;
      if (!exclusion_transitions.empty()) {
        exclusion_state = base_state & kExclusionStateMask;
        base_state >>= kExclusionStateBits;
      }
      XGRAMMAR_DCHECK(base_state * 256 + byte < dense_transitions.size());
      uint16_t next_base = dense_transitions[base_state * 256 + byte];
      if (next_base == kNoTransition) return -1;
      if (exclusion_transitions.empty()) return next_base;

      uint32_t next_exclusion = 0;
      if (exclusion_state != 0) {
        XGRAMMAR_DCHECK(exclusion_state < exclusion_transitions.size());
        for (const auto& [edge_byte, target] : exclusion_transitions[exclusion_state]) {
          if (edge_byte == byte) {
            next_exclusion = target;
            break;
          }
        }
      }
      uint32_t packed = (static_cast<uint32_t>(next_base) << kExclusionStateBits) | next_exclusion;
      XGRAMMAR_DCHECK(packed <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
      return static_cast<int32_t>(packed);
    }
  };

  struct JSONNumberBound {
    bool negative = false;
    std::string digits;
    SignedDecimalInteger order;
    std::optional<int64_t> small_order;
  };

  struct JSONNumberRange {
    std::optional<JSONNumberBound> minimum;
    std::optional<JSONNumberBound> maximum;
    bool exclusive_minimum = false;
    bool exclusive_maximum = false;
  };

  /*! \brief Grammar-wide canonical store for large-object required-key bitsets.
   *
   * Parser states keep only an integer id, which preserves compact state copies and makes the id
   * stable across the main matcher and the short-lived token-mask parsers sharing these features.
   */
  class JSONObjectRequiredStateInterner {
   public:
    int32_t InternEmpty(int32_t owner_rule_id, int32_t required_count);
    int32_t Mark(int32_t state_id, int32_t property_index);
    int32_t Union(int32_t lhs_state_id, int32_t rhs_state_id);
    int32_t SeenCount(int32_t state_id) const;
    bool IsComplete(int32_t state_id, int32_t required_count) const;
    size_t MemorySizeBytes() const;

   private:
    struct Key {
      int32_t owner_rule_id;
      std::vector<uint64_t> words;

      bool operator==(const Key& other) const {
        return owner_rule_id == other.owner_rule_id && words == other.words;
      }
    };
    struct KeyHash {
      size_t operator()(const Key& key) const {
        size_t result = std::hash<int32_t>()(key.owner_rule_id);
        for (uint64_t word : key.words) result = HashCombine(result, word);
        return result;
      }
    };

    int32_t InternLocked(Key key, int32_t seen_count);

    mutable std::mutex mutex_;
    std::vector<Key> states_;
    std::vector<int32_t> seen_counts_;
    std::unordered_map<Key, int32_t, KeyHash> ids_;
  };

  std::vector<uint8_t> fsm_state_flags;
  std::vector<uint8_t> rule_is_nullable;
  std::vector<std::optional<JSONStringPatternDFA>> json_string_pattern_dfas;
  std::vector<std::optional<JSONNumberRange>> json_number_ranges;
  std::shared_ptr<JSONObjectRequiredStateInterner> json_object_required_states;
  std::vector<int32_t> json_object_empty_state_ids;
  bool has_budget_rules = false;
  bool has_char_budget_rules = false;
  bool has_json_string_length_rules = false;
  bool has_json_object_required_rules = false;
  bool capture_tracking = false;
  bool has_hidden_capture_rules = false;

  explicit EarleyParserGrammarFeatures(
      const Grammar& grammar, RegexFSMCache* regex_fsm_cache = nullptr
  );

  const JSONStringPatternDFA* GetJSONStringPatternDFA(int32_t rule_id) const {
    return json_string_pattern_dfas[rule_id].has_value() ? &*json_string_pattern_dfas[rule_id]
                                                         : nullptr;
  }

  const JSONNumberRange* GetJSONNumberRange(int32_t rule_id) const {
    return json_number_ranges[rule_id].has_value() ? &*json_number_ranges[rule_id] : nullptr;
  }

  int32_t GetJSONObjectRequiredEmptyState(int32_t rule_id) const {
    XGRAMMAR_DCHECK(
        rule_id >= 0 && rule_id < static_cast<int32_t>(json_object_empty_state_ids.size())
    );
    return json_object_empty_state_ids[rule_id];
  }

  int32_t GetJSONStringPatternTransition(int32_t rule_id, int32_t state, uint8_t byte) const {
    return json_string_pattern_dfas[rule_id]->Advance(state, byte);
  }

  friend std::size_t MemorySize(const EarleyParserGrammarFeatures& features) {
    size_t result =
        MemorySize(features.fsm_state_flags) + MemorySize(features.rule_is_nullable) +
        sizeof(std::optional<JSONStringPatternDFA>) * features.json_string_pattern_dfas.size() +
        sizeof(std::optional<JSONNumberRange>) * features.json_number_ranges.size() +
        MemorySize(features.json_object_empty_state_ids) +
        (features.json_object_required_states == nullptr
             ? 0
             : features.json_object_required_states->MemorySizeBytes());
    for (const auto& pattern_dfa : features.json_string_pattern_dfas) {
      if (pattern_dfa.has_value()) {
        result += MemorySize(pattern_dfa->is_end_state) +
                  MemorySize(pattern_dfa->dense_transitions) +
                  MemorySize(pattern_dfa->exclusion_is_terminal) +
                  MemorySize(pattern_dfa->exclusion_transitions);
        for (const auto& transitions : pattern_dfa->exclusion_transitions) {
          result += MemorySize(transitions);
        }
      }
    }
    for (const auto& number_range : features.json_number_ranges) {
      if (number_range.has_value()) {
        if (number_range->minimum.has_value()) {
          result += MemorySize(number_range->minimum->digits);
          result += MemorySize(number_range->minimum->order.digits);
        }
        if (number_range->maximum.has_value()) {
          result += MemorySize(number_range->maximum->digits);
          result += MemorySize(number_range->maximum->order.digits);
        }
      }
    }
    return result;
  }
};

class EarleyParser {
  /*!
   * \brief Here is an article about Earley Parser.
   * https://en.wikipedia.org/wiki/Earley_parser#Pseudocode
   * We divide the parser states into three categories:
   * - Scanable (which will be stored in scanable_state_history_).
   * - Predictable(If it predict a new rule successfully, then it will be stored in
   * rule_id_to_completable_states).
   * - completable(which can perform a completion operation).
   * A state will be stored in rule_id_to_completable_states_ if it can be completed,
   * and it will be stored in scanable_state_history_ if it can be scanned. Otherwise,
   * it will be discarded.
   */
 protected:
  using GrammarExpr = Grammar::Impl::GrammarExpr;

  /*! \brief The grammar to be parsed. */
  Grammar grammar_;

  /*! \brief Direct view of the shared complete-FSM edges. */
  const Compact2DArray<FSMEdge>* complete_fsm_edges_;

  /*! \brief In this round of advancing, check if the stop token can be accepted. */
  bool tmp_accept_stop_token_ = false;

  /*! \brief store when accepting i characters, if the stop token can be accepted. */
  std::vector<bool> is_completed_;

  /*!
   * \brief rule_id_to_completable_states[i][j] is the i pos j rule_id states. Earley
   * parser needs it to complete.
   */
  Compact2DArray<std::pair<int32_t, ParserState>> rule_id_to_completable_states_;

  /*!
   * \brief The states history. state_stack[i] is a vector storing the states after accepting the
   * input[i-1].
   */
  Compact2DArray<ParserState> scanable_state_history_;

  /*!
   * \brief A temporary vector only used in Advance, used to add states in the
   * scanable_state_history.
   */
  std::vector<const ParserState*> tmp_states_to_be_added_;

  /*! \brief Stable pointers to visited states awaiting prediction/completion. */
  ReusableStatePointerQueue tmp_process_state_queue_;

  /*! \brief The class is used to check if a state has been added into the queue. */
  RepeatDetector tmp_states_visited_in_queue_;

  /*! \brief Check if the stop token is accepted. */
  bool stop_token_is_accepted_ = false;

  using FsmStateFlag = EarleyParserGrammarFeatures::FsmStateFlag;
  static constexpr uint8_t kFsmStateInitialized = EarleyParserGrammarFeatures::kFsmStateInitialized;
  static constexpr uint8_t kFsmStateScanable = EarleyParserGrammarFeatures::kFsmStateScanable;
  static constexpr uint8_t kFsmStateNonTerminal = EarleyParserGrammarFeatures::kFsmStateNonTerminal;
  static constexpr uint8_t kFsmStateEnd = EarleyParserGrammarFeatures::kFsmStateEnd;
  static constexpr uint8_t kFsmStateHasEdges = EarleyParserGrammarFeatures::kFsmStateHasEdges;

  /*! \brief Grammar-wide parser features shared by every parser for this grammar. */
  std::shared_ptr<const EarleyParserGrammarFeatures> grammar_features_;

  /*! \brief Return shared properties for a state in the complete FSM. */
  uint8_t GetFsmStateFlags(int32_t rule_id, int32_t state_id) const {
    XGRAMMAR_DCHECK(rule_id >= 0 && rule_id < grammar_->NumRules());
    XGRAMMAR_DCHECK(
        state_id >= 0 && state_id < static_cast<int32_t>(grammar_features_->fsm_state_flags.size())
    );
    return grammar_features_->fsm_state_flags[state_id];
  }

  bool IsRuleNullable(int32_t rule_id) const {
    return grammar_features_->rule_is_nullable[rule_id] != 0;
  }

  /*! \brief The index of the LLM token currently being accepted, set by the matcher; -1
   * before any token. budget_deadline values are compared against it. */
  int32_t current_token_index_ = -1;

  /*! \brief Whether states past their budget deadline are skipped when scanning. Enabled by
   * the matcher for accepts that follow an enforcing mask computation. */
  bool skip_expired_states_ = false;

  /*! \brief Whether any rule of the grammar has a token budget. */
  bool has_budget_rules_ = false;

  /*! \brief The number of Unicode codepoints accepted at every parser history row. */
  std::vector<int32_t> char_count_history_;

  /*! \brief Whether any rule of the grammar has a character budget. */
  bool has_char_budget_rules_ = false;

  /*! \brief Whether any rule validates decoded JSON-string length at runtime. */
  bool has_json_string_length_rules_ = false;

  /*! \brief Whether any rule tracks a large JSON object's required keys at runtime. */
  bool has_json_object_required_rules_ = false;

  /*! \brief Owned scratch rows used to merge equivalent runtime-required parser histories. */
  std::vector<ParserState> tmp_merged_json_object_states_;
  std::vector<std::pair<int32_t, ParserState>> tmp_merged_json_object_completable_states_;

  /*! \brief Store the latest scanable row after unioning equivalent required-key histories. */
  void PushLatestScanableStates();

  /*! \brief Whether a character-budgeted occurrence was entered since the initial parser row. */
  std::vector<bool> char_budget_entry_history_;

  /*! \brief Entry-history value for the row currently being expanded. */
  bool tmp_char_budget_entered_ = false;

  /*! \brief Whether the state's derivation may not consume the next token. */
  bool IsExpiredState(const ParserState& state) const {
    return state.budget_deadline >= 0 && current_token_index_ > state.budget_deadline;
  }

  /*! \brief The deadline for a newly predicted occurrence of the rule: its own budget counted
   * from the current token, capped by the parent's deadline for nested budgets. */
  int32_t DeadlineForRule(int32_t rule_id, int32_t parent_deadline) const {
    int32_t own = grammar_->GetRule(rule_id).max_tokens;
    if (own < 0) {
      return parent_deadline;
    }
    int32_t deadline = current_token_index_ + own;
    return parent_deadline >= 0 ? std::min(deadline, parent_deadline) : deadline;
  }

  /*! \brief The character deadline for a newly predicted rule occurrence. */
  int32_t CharDeadlineForRule(int32_t rule_id, int32_t parent_deadline) const {
    int32_t own = grammar_->GetRule(rule_id).max_chars;
    if (own < 0) {
      return parent_deadline;
    }
    int32_t current_char_index = GetCurrentCharIndex();
    int32_t deadline = own > std::numeric_limits<int32_t>::max() - current_char_index
                           ? std::numeric_limits<int32_t>::max()
                           : current_char_index + own;
    return parent_deadline >= 0 ? std::min(deadline, parent_deadline) : deadline;
  }

  /*! \brief Whether the state's derivation may not consume another Unicode codepoint. */
  bool IsCharExpiredState(const ParserState& state) const {
    return state.char_budget_deadline >= 0 && GetCurrentCharIndex() >= state.char_budget_deadline;
  }

  static bool StartsUTF8Codepoint(uint8_t byte) { return (byte & 0xC0) != 0x80; }

  /*! \brief Whether any rule of the grammar has a capture or stop_capture name. Fixed at
   * construction. When false, the capture machinery is fully disabled and has no overhead. */
  bool capture_tracking_ = false;

  /*! \brief Whether the grammar contains suffix/stop spans that may affect captures. */
  bool has_hidden_capture_rules_ = false;

  /*!
   * \brief Whether capture events are currently recorded in Complete(). Only enabled during
   * definitive advances (accepting a token or string), not during speculative exploration
   * (mask computation, jump-forward search, lookahead checks), so that speculative completions
   * never produce capture events.
   */
  bool capture_recording_ = false;

  /*!
   * \brief The history of capture events. capture_event_history_[i] stores the events recorded
   * when input position i was created. Kept aligned with scanable_state_history_ row-by-row
   * whenever capture_tracking_ is true, so PopLastStates rolls back events automatically.
   */
  Compact2DArray<CaptureEvent> capture_event_history_;

  /*! \brief Returns true if the rule exists and has a capture name. */
  bool RuleHasCapture(int32_t rule_id) const {
    return capture_tracking_ && rule_id >= 0 && !grammar_->GetRule(rule_id).capture_name.empty();
  }

  /*! \brief Returns true if completing this rule can hide bytes from a capture. */
  bool RuleHasHiddenBytes(int32_t rule_id) const {
    if (!capture_tracking_ || !has_hidden_capture_rules_ || rule_id < 0) {
      return false;
    }
    const auto* suffix_stop_info = grammar_->GetSuffixStopInfo(rule_id);
    return suffix_stop_info != nullptr &&
           (suffix_stop_info->hidden_suffix_bytes > 0 || suffix_stop_info->hidden_stop_bytes > 0);
  }

  /*! \brief Returns true if completing this rule must produce a capture-history event. */
  bool RuleNeedsCaptureEvent(int32_t rule_id) const {
    return RuleHasCapture(rule_id) || RuleHasHiddenBytes(rule_id);
  }

  /*! \brief Record a capture or hidden-span event for a completed rule in the current row. */
  void RecordCaptureEvent(const ParserState& state, bool marker_present);

  /*!
   * \brief Whether this completion of a suffix/stop rule actually consumed the trailing marker.
   * A non-looping TagDispatch can also complete before its trigger is encountered, which is what
   * lets a free-text tail end normally. Only the terminal post-dispatch state has no outgoing
   * edges; completions in the trigger-scanning states did not consume a suffix/stop marker.
   */
  bool CompletionConsumedMarker(const ParserState& state) const;

  /*! \brief Collect the captured rule occurrences whose concrete Earley parent chains contain
   * the given stop completion. Includes the completed rule's own occurrence when captured. */
  std::vector<CaptureOccurrence> CollectStopCaptureTargets(const ParserState& state) const;

  /*!
   * \brief The lazy rule occurrences (rule_id, rule_start_pos) completed while building the
   * current row. Committed-shortest matching: their remaining states are removed when the row is
   * finalized, so the occurrence cannot be extended further.
   */
  std::vector<std::pair<int32_t, int32_t>> tmp_completed_lazy_occurrences_;

  /*! \brief Remove the states of the lazy occurrences completed in the current row. */
  void RemoveCommittedLazyStates();

  /*!
   * \brief The scanning operation of the Earley parser. Put the new states in the queue.
   */
  void Scan(const ParserState& state, const uint8_t ch);

  /*!
   * \brief The completion operation of the Earley parser.
   * \param state The state to be completed.
   * \param debug_print Whether to print the debug information.
   * \details The reason is that if the state can't be scanned, then
   * add it into the next states is useless. Moreover, the end
   * of the grammar is used to check if the grammar is completed,
   * so it should be added into the next states.
   */
  void Complete(const ParserState& state, bool debug_print = false, bool marker_present = true);

  /*!
   * \brief The prediction operation of the Earley parser.
   * \param state The state to be predicted.
   * \param debug_print Whether to print the debug information.
   * \return First: If the state scanable, or the state is the end of the grammar,
   * then return true, otherwise return false.
   * \return Second: If the state is completable, then return true, otherwise return false.
   */
  std::pair<bool, bool> Predict(const ParserState& state, bool debug_print = false);

  /*! \brief The initial state expanded from the root rule of the grammar. */
  ParserState RootInitialState() const;

  /*! \brief Resolve the active temperature rule when entering a rule. */
  int32_t ResolveActiveTemperatureRule(int32_t rule_id, int32_t inherited_rule_id) const;

  /*! \brief Inherit, start, or update compact required-key tracking when entering a rule. */
  void EnterJSONObjectRequiredRule(
      const ParserState& parent_state, int32_t ref_rule_id, ParserState* child_state
  ) const;

  /*!
   * \brief Expand the rule, used for RuleRef and kTagDispatch.
   * \param state The state to be expanded, which is the parent state.
   * The type of the state is kTagDispatch or kSequence. Moreover, the
   * element of the sequence should be a rule reference; the node in
   * the kTagDispatch should be an end node.
   * \param grammar_expr The grammar expression to be expanded.
   * \param sub_grammar_expr The sub grammar expression to be expanded, especially
   * when the rule is a kSequence, and the sub rule is a kRuleRef.
   * \param debug_print Whether to print the debug information.
   */
  void ExpandNextRuleRefElement(
      const ParserState& state,
      const GrammarExpr& grammar_expr,
      const GrammarExpr* sub_grammar_expr,
      bool debug_print = false
  );

  /*!
   * \brief Expand the rule, used for RuleRef and kTagDispatch.
   * \param state The state to be expanded, and it's should be on the FSM.
   * \param debug_print Whether to print the debug information.
   */
  void ExpandNextRuleRefElementOnFSM(const ParserState& state, bool debug_print = false);

  /*!
   * \brief Advance the parser to the next state, with the sub sequence is kCharacterClass.
   * \param state The state to be advanced.
   * \param ch The character to be advanced.
   * \param sub_sequence The sub sequence to be checked.
   * \note The advanced states are enqueued; nothing is enqueued if the character is not accepted.
   */
  void AdvanceCharacterClass(
      const ParserState& state, const uint8_t ch, const GrammarExpr& sub_sequence
  );

  /*!
   * \brief Advance the parser to the next state, with the sub sequence is kByteString.
   * \param state The state to be advanced.
   * \param ch The character to be advanced.
   * \param sub_sequence The sub sequence to be checked.
   * \note The advanced states are enqueued; nothing is enqueued if the character is not accepted.
   */
  void AdvanceByteString(
      const ParserState& state, const uint8_t ch, const GrammarExpr& sub_sequence
  );

  /*!
   * \brief Advance the parser to the next state, with the sub sequence is kCharacterClassStar.
   * \param state The state to be advanced.
   * \param ch The character to be advanced.
   * \param sub_sequence The sub sequence to be checked.
   * \note The advanced states are enqueued; nothing is enqueued if the character is not accepted.
   */
  void AdvanceCharacterClassStar(
      const ParserState& state, const uint8_t ch, const GrammarExpr& sub_sequence
  );

  /*!
   * \brief Advance the parser to the next state, with the sequence is kTagDispatch.
   * \param state The state to be advanced.
   * \param ch The character to be advanced.
   * \note The advanced states are enqueued; nothing is enqueued if the character is not accepted.
   */
  void AdvanceFsm(const ParserState& state, const uint8_t ch);

  /*! \brief Advance one JSON source byte for a rule carrying decoded-string length bounds.
   * Returns false when the source prefix is invalid or already exceeds the upper bound. */
  bool AdvanceJSONStringLength(ParserState* state, uint8_t ch) const;

  /*! \brief Check only the active decoded JSON-string side constraints for one token. */
  bool JSONStringRuntimeConstraintsAllowToken(ParserState state, const std::string& token) const;

  bool AdvanceJSONStringPattern(ParserState* state, int32_t codepoint) const;

  /*! \brief Advance the exact decimal number multipleOf state by one JSON source byte. */
  bool AdvanceJSONNumber(ParserState* state, uint8_t ch) const;

  /*! \brief Whether the active exact decimal multipleOf constraint accepts at rule completion. */
  bool JSONNumberMultipleOfAccepts(const ParserState& state) const;

  bool JSONNumberRangeAccepts(const ParserState& state) const;

  /*!
   * \brief Scan a token edge: check if token_id matches any kToken or kExcludeToken edge from
   * state.
   */
  void ScanAtomicToken(const ParserState& state, int32_t token_id);

  /*!
   * \brief Advance the parser by accepting a whole token via kToken/kExcludeToken edges.
   * \param token_id The token ID to accept.
   * \param debug_print Whether to print debug info.
   * \return True if any state advanced, false otherwise.
   */
  bool AdvanceAtomicToken(int32_t token_id, bool debug_print = false, int32_t token_char_count = 0);

  /*!
   * \brief Enqueue the state into the queue.
   * \param state The state to be enqueued.
   * \details The state is enqueued if it is not visited in the queue.
   */
  void Enqueue(const ParserState& state) {
    if (const ParserState* inserted = tmp_states_visited_in_queue_.InsertIfAbsent(state)) {
      tmp_process_state_queue_.push(inserted);
    }
  }

  /*!
   * \brief Enqueue the state into the queue, without prediction and completion.
   * \param state The state to be enqueued.
   */
  void EnqueueWithoutProcessing(const ParserState& state) {
    if (const ParserState* inserted = tmp_states_visited_in_queue_.InsertIfAbsent(state)) {
      tmp_states_to_be_added_.push_back(inserted);
    }
  }

  void EnqueueFsmTransition(const ParserState& state, int32_t target_element_id) {
    if (const ParserState* inserted =
            tmp_states_visited_in_queue_.InsertFsmTransitionIfAbsent(state, target_element_id)) {
      tmp_process_state_queue_.push(inserted);
    }
  }

  void EnqueueFsmTransitionWithoutProcessing(const ParserState& state, int32_t target_element_id) {
    if (const ParserState* inserted =
            tmp_states_visited_in_queue_.InsertFsmTransitionIfAbsent(state, target_element_id)) {
      tmp_states_to_be_added_.push_back(inserted);
    }
  }

 public:
  /*!
   * \brief Constructor of the Earley parser.
   * \param grammar The grammar to be parsed. It must be optimized.
   * \param initial_state The state to start parsing from. If not provided, parsing starts
   * from the root rule of the grammar.
   */
  explicit EarleyParser(
      const Grammar& grammar,
      std::optional<ParserState> initial_state = std::nullopt,
      std::shared_ptr<const EarleyParserGrammarFeatures> grammar_features = nullptr
  );

  /*!
   * \brief From the current states, advance to the next state.
   * \param ch The character to be advanced.
   * \param debug_print Whether to print the debug information.
   * \return True if the character is accepted, false otherwise.
   * \note If the character isn't accepted, then the states won't be changed.
   */
  bool Advance(const uint8_t ch, bool debug_print = false);

  /*!
   * \brief Remove the newly added states.
   * \param count The number of states to be removed.
   */
  void PopLastStates(int32_t count = 1);

  /*!
   * \brief Check whether any of the multiple states stored in the parser has already completed.
   * \note Since the parser contains multiple parallel states, some may have already completed,
   * while others might still be able to accept more characters.
   * \return True if the root rule is completed, false otherwise.
   */
  bool IsCompleted() const;

  /*!
   * \brief Push the initial state into the Earley parser.
   * \param state The initial state to be pushed.
   */
  void PushStateAndExpand(const ParserState& state);

  /*!
   * \brief Reset the parser.
   * \note This function is used to reset the parser, and initialize the
   * parser with the root rule.
   */
  void Reset();

  /*!
   * \brief Get the current scanable states.
   * \return The scanable states.
   */
  std::vector<ParserState> GetLatestScanableStates() const {
    std::vector<ParserState> latest_states;
    for (const auto& state : scanable_state_history_[scanable_state_history_.size() - 1]) {
      latest_states.push_back(state);
    }
    return latest_states;
  }

  /*!
   * \brief Push one state to check if it can accept the token.
   * \param state The state to be pushed.
   */
  void PushOneStateToCheck(const ParserState& state) {
    PushStatesToCheck(std::vector<ParserState>{state}, is_completed_.back());
  }

  /*! \brief Push a temporary parser row for token-mask checking. */
  void PushStatesToCheck(const std::vector<ParserState>& states, bool completed) {
    rule_id_to_completable_states_.PushBack(std::vector<std::pair<int32_t, ParserState>>());
    is_completed_.push_back(completed);
    scanable_state_history_.PushBack(states);
    if (capture_tracking_) {
      capture_event_history_.PushBack(std::vector<CaptureEvent>());
    }
    if (has_char_budget_rules_) {
      char_count_history_.push_back(GetCurrentCharIndex());
      char_budget_entry_history_.push_back(char_budget_entry_history_.back());
    }
  }

  /*! \brief Push a character-count row for a parser row created by the matcher. */
  void PushCharCountRow(int32_t char_count, bool char_budget_entered) {
    if (!has_char_budget_rules_) {
      return;
    }
    char_count_history_.push_back(char_count);
    char_budget_entry_history_.push_back(char_budget_entered);
  }

  int32_t GetCurrentCharIndex() const {
    return char_count_history_.empty() ? 0 : char_count_history_.back();
  }

  bool HasEnteredCharBudget() const {
    return has_char_budget_rules_ && char_budget_entry_history_.back();
  }

  /*! \brief Whether the grammar has any captured rule. */
  bool IsCaptureTrackingEnabled() const { return capture_tracking_; }

  /*! \brief Copy the capture events of the latest input position. */
  std::vector<CaptureEvent> CopyLastCaptureRow() const {
    if (!capture_tracking_) {
      return {};
    }
    auto row = capture_event_history_[capture_event_history_.size() - 1];
    return std::vector<CaptureEvent>(row.begin(), row.end());
  }

  /*!
   * \brief Push a new row of capture events. Used when a new input position is created outside
   * of Advance / AdvanceAtomicToken (e.g. when merging parallel advance results), to keep the
   * capture history aligned with the state history.
   */
  void PushCaptureRow(const std::vector<CaptureEvent>& events) {
    if (capture_tracking_) {
      capture_event_history_.PushBack(events);
    }
  }

  std::string PrintStates() const {
    std::string result;
    result += "There are " + std::to_string(scanable_state_history_.size()) +
              " steps in history. Last step: [\n";
    for (const auto& state : scanable_state_history_[scanable_state_history_.size() - 1]) {
      result += state.ToString() + ", \n";
    }
    result += "]";
    return result;
  }
};

}  // namespace xgrammar

#endif  // XGRAMMAR_EARLEY_PARSER_H_
