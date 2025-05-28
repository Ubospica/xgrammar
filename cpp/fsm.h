/*!
 *  Copyright (c) 2025 by Contributors
 * \file xgrammar/fsm.h
 */
#ifndef XGRAMMAR_FSM_H_
#define XGRAMMAR_FSM_H_

#include <xgrammar/object.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "../cpp/support/csr_array.h"

namespace xgrammar {

struct FSMEdge {
  /*
    The min and max are used to represent the range of characters.
    When min == -1 and max == -1, it means the edge is an epsilon transition.
    When min == -1 and max >= 0, then max represents the rule id.
    When min >= 0 and max >= 0, then it represents a range of characters.
    target is the target state id.
  */
  short min, max;

  int target;

  FSMEdge(short min, short max, int target) : min(min), max(max), target(target) {
    XGRAMMAR_DCHECK(!IsCharRange() || min <= max)
        << "Invalid FSMEdge: min > max. min=" << min << ", max=" << max;
  }

  /*!
    \brief Compare the edges. Used to sort the edges in the FSM.
  */
  friend bool operator==(const FSMEdge& lhs, const FSMEdge& rhs) {
    return lhs.min == rhs.min && lhs.max == rhs.max && lhs.target == rhs.target;
  }

  /*!
    \brief Compare the edges. Used to sort the edges in the FSM.
  */
  friend bool operator<(const FSMEdge& lhs, const FSMEdge& rhs) {
    return std::make_tuple(lhs.target, lhs.min, lhs.max) <
           std::make_tuple(rhs.target, rhs.min, rhs.max);
  }

  /*!
    \brief Check if the edge is an epsilon transition.
  */
  bool IsEpsilon() const;

  /*!
    \brief Check if the edge is a rule reference.
  */
  bool IsRuleRef() const;

  /*!
    \brief Get the rule id of the edge.
    \return The rule id of the edge. -1 if the edge is not a rule reference.
  */
  int GetRefRuleId() const;

  /*!
    \brief Check if the edge is a character range.
  */
  bool IsCharRange() const;
};

}  // namespace xgrammar

// Define the hash function for FSMEdge
namespace std {

template <>
struct hash<xgrammar::FSMEdge> {
  size_t operator()(const xgrammar::FSMEdge& edge) const {
    return std::hash<std::tuple<short, short, int>>()(
        std::make_tuple(edge.min, edge.max, edge.target)
    );
  }
};

}  // namespace std

namespace xgrammar {

class CompactFSM;

class FSM {
 public:
  FSM(int num_states = 0);

  FSM(const std::vector<std::vector<FSMEdge>>& edges);

  FSM(std::vector<std::vector<FSMEdge>>&& edges);

  /****************** FSM Accessors and Mutators ******************/

  int NumStates() const;

  /*!
    \brief Adds a new state to the FSM.
    \return The index of the newly added state.
  */
  int AddState();

  /*!
   * \brief Adds a transition edge between states with a character range.
   * \param from The source state.
   * \param to The target state.
   * \param min_ch The minimum character in the range (inclusive).
   * \param max_ch The maximum character in the range (inclusive).
   */
  void AddEdge(int from, int to, int16_t min_ch, int16_t max_ch);

  /*!
    \brief Add an epsilon transition between two states.
    \param from The source state.
    \param to The target state.
  */
  void AddEpsilonEdge(int from, int to);

  /*!
   * \brief Add a whole FSM to the current FSM.
   * \param fsm The FSM to be added.
   * \param state_mapping The mapping from the state ids of the added FSM to the new ids in the
   * current FSM.
   */
  void AddFSM(const FSM& fsm, std::unordered_map<int, int>* state_mapping = nullptr);

  const std::vector<std::vector<FSMEdge>>& GetEdges() const;

  std::string PrintEdges() const;

  /*!
    \brief Return a copy of the FSM.
  */
  FSM Copy() const;

  /****************** FSM Traversal Algorithms ******************/

  inline static constexpr int kNoNextState = -1;

  /*!
   * \brief Advance the FSM from a given state based on an input character. If there are multiple
   * transitions, the first one will be returned.
   * \param from The source state to transition from.
   * \param character The input character.
   * \return The target state if a valid transition exists, kNoNextState otherwise.
   */
  int GetNextState(int from, int16_t character) const;

  /*!
    \brief Advance the FSM to the next state.
    \param from The current states.
    \param value The input value.
    \param result The next states, which can be seen as the result of the
    transition. The result is not cleared at the beginning.
    \param is_closure Whether from is an epsilon closure.
    \param is_rule Whether the input value is a rule id.
  */
  void Advance(
      const std::vector<int>& from,
      int value,
      std::vector<int>* result,
      bool is_closure = false,
      bool is_rule = false
  ) const;

  /*!
    \brief Get all the possible rule numbers for a given state.
    \param state_num The state number.
    \param rules The set of possible rule numbers.
  */
  void GetPossibleRules(const int& state_num, std::unordered_set<int>* rules) const;

  /*!
   * \brief Get the epsilon closure of a set of states and store the result in the set.
   * \param state_set The current states.
   */
  void GetEpsilonClosure(std::unordered_set<int>* state_set) const;

  /****************** FSM Construction Algorithms ******************/

  FSM RebuildWithMapping(std::unordered_map<int, int>& state_mapping, int new_num_states);

  /*!
    \brief Transform a FSM to a compact FSM.
    \return The compact FSM.
  */
  CompactFSM ToCompact();

  XGRAMMAR_DEFINE_PIMPL_METHODS(FSM);
};

class FSMWithStartEnd {
 public:
  /*! \brief Constructs an FSMWithStartEnd with the specified number of states. */
  FSMWithStartEnd(int num_states = 0, bool is_dfa = false) : fsm_(num_states), is_dfa_(is_dfa) {}

  /*! \brief Constructs an FSMWithStartEnd with a given FSM, start state, and end states. */
  FSMWithStartEnd(
      const FSM& fsm, int start, const std::unordered_set<int>& ends, bool is_dfa = false
  )
      : fsm_(fsm), start_(start), ends_(ends), is_dfa_(is_dfa) {}

  /*!
   * \brief Construct a FSM from a regex string.
   * \details The regex string should only be the format like "abx" or [a-c0-9].
   * \details Any symbols like "a|b" or "a*b" are not supported.
   * \param regex The regex string.
   * \return The FSM with start and end states.
   */
  static FSMWithStartEnd BuildFSMFromRegex(const std::string& regex);

  /****************** Member Accessors and Mutators ******************/

  FSM GetFSM() const { return fsm_; }

  /*! \brief Returns the start state of the FSM. */
  int GetStart() const { return start_; }

  const std::unordered_set<int>& GetEnds() const { return ends_; }

  /*!
   * \brief Checks if a given state is an end/accepting state.
   * \param state The state to check.
   * \return True if the state is an end state, false otherwise.
   */
  bool IsEndState(int state) const {
    return std::any_of(ends_.begin(), ends_.end(), [state](int end_state) {
      return end_state == state;
    });
  }

  /*!
   * \brief Sets the start state of the FSM.
   * \param state The state to set as the start state.
   */
  void SetStartState(int state) {
    XGRAMMAR_DCHECK(state < NumStates());
    start_ = state;
  }

  /*!
   * \brief Adds an end/accepting state to the FSM.
   * \param state The state to add as an end state.
   */
  void AddEndState(int state) {
    XGRAMMAR_DCHECK(state < NumStates());
    ends_.insert(state);
  }

  /*! \brief Returns the total number of states in the FSM. */
  int NumStates() const { return fsm_.NumStates(); }

  /*!
    \brief Access the methods of the underlying FSM.
  */
  FSM* operator->() { return &fsm_; }

  /*!
    \brief Access the methods of the underlying FSM.
  */
  const FSM* operator->() const { return &fsm_; }

  /*!
    \brief Return a copy of the FSMWithStartEnd.
  */
  FSMWithStartEnd Copy() const;

  /*!
    \brief Print the FSM.
    \return The string representation of the FSMWithStartEnd.
  */
  std::string Print() const;

  friend std::ostream& operator<<(std::ostream& os, const FSMWithStartEnd& fsm);

  /****************** FSM Traversal Algorithms ******************/

  /*!
    \brief Check if the FSM accepts the string.
    \param str The input string.
    \return True if the FSM accepts the string, false otherwise.
  */
  bool AcceptsString(const std::string& str) const;

  void GetReachableStates(std::unordered_set<int>* states) const;

  /****************** FSM Construction Algorithms ******************/

  /*!
    \brief Return a new FSM representing FSM*
    \return The FSM that accepts FSM*.
  */
  FSMWithStartEnd Star() const;

  /*!
    \brief Return a new FSM representing rule1+.
    \return The FSM that accepts rule1+.
  */
  FSMWithStartEnd Plus() const;

  /*!
    \brief Return a new FSM representing rule1?.
    \return The FSM that accepts rule1?.
  */
  FSMWithStartEnd Optional() const;

  /*!
    \brief Return a new FSM representing the complement of the language.
    \return The complement FSM.
  */
  FSMWithStartEnd Not() const;

  /*!
    \brief Intersect the FSMs.
    \param lhs The left FSM.
    \param rhs The right FSM.
    \return The intersection of the FSMs.
  */
  static Result<FSMWithStartEnd> Intersect(
      const FSMWithStartEnd& lhs, const FSMWithStartEnd& rhs, const int& num_of_states_limited = 1e6
  );

  /*!
    \brief Union the FSMs.
    \param fsms The FSMs to be unioned.
    \return The union of the FSMs.
  */
  static FSMWithStartEnd Union(const std::vector<FSMWithStartEnd>& fsms);

  /*!
    \brief Concatenate the FSMs.
    \param fsms The FSMs to be concatenated, which should be in order.
    \return The concatenation of the FSMs.
  */
  static FSMWithStartEnd Concat(const std::vector<FSMWithStartEnd>& fsms);

  /*!
    \brief Transform the FSM to a DFA.
    \return The DFA.
  */
  FSMWithStartEnd ToDFA() const;

  /*!
    \brief Minimize the DFA.
    \return The minimized DFA.
  */
  FSMWithStartEnd MinimizeDFA() const;

  /*!
   * \brief Check if the FSM is a DFA.
   * \return True if the FSM is a DFA, false otherwise.
   */
  bool IsDFA();

  /*!
    \brief Check if the FSM is a leaf FSM.
    \return True if the FSM is a leaf FSM, false otherwise.
  */
  bool IsLeaf() const;

  /*!
   * \brief Merge some states by removing some epsilon transitions.
   * \details If a --\epsilon--> b, and either 1) b doesn't have any other inward edges, or
   * 2) a doesn't have any other outward edges, we can merge a and b.
   */
  void SimplifyEpsilon();

  /*!
   * \brief Merge equivalent states in the FSM.
   * \details If two states are 1) pointed to by edges with the same label from the same state, and
   * 2) they are not pointed to by other edges, then we can merge them.
   * \example n0 --(c)--> n1, n0 --(c)--> n2, then we can merge n1 and n2.
   */
  void SimplifyEquivalentStates();

  /*!
    \brief Rebuild the FSM with the new state ids.
    \param old_to_new The mapping from old state ids to new state ids.
  */
  FSMWithStartEnd RebuildWithMapping(
      std::unordered_map<int, int>& state_mapping, int new_num_states
  );

 private:
  /*! \brief The underlying finite state machine. */
  FSM fsm_;
  /*! \brief The start state of the FSM. */
  int start_;
  /*! \brief The set of accepting/end states. */
  std::unordered_set<int> ends_;
  /*! \brief Whether this FSM is a deterministic finite automaton. */
  bool is_dfa_ = false;
};

class CompactFSM {
 public:
  /*!
    \brief Get the epsilon closure of a state.
    \param state_set The current states.
    \param result The epsilon closure of the state. If nullptr,
           then the result will be stored in state_set.
  */
  void GetEpsilonClosure(
      std::unordered_set<int>* state_set, std::unordered_set<int>* result = nullptr
  ) const;

  /*!
   \brief Advance the FSM to the next state.
   \param from The current states.
   \param value The input value.
   \param result The next states, which can be seen as the result of the
   transition.
   \param is_closure Whether from is an epsilon closure.
   \param is_rule Whether the input value is a rule id.
  */
  void Advance(
      const std::vector<int>& from,
      int value,
      std::vector<int>* result,
      bool is_closure = false,
      bool is_rule = false
  ) const;

  /*!
    \brief Transform the compact FSM to a FSM.
    \return The FSM.
  */
  FSM ToFSM();

  CSRArray<FSMEdge> edges;

  friend class CompactFSMWithStartEnd;
};

class CompactFSMWithStartEnd {
 public:
  bool is_dfa = false;

  CompactFSM fsm;

  int start;

  std::unordered_set<int> ends;

  /*!
    \brief Print the FSM.
    \return The string representation of the FSM.
  */
  std::string Print() const;

  /*!
    \brief Check if the FSM accepts the string.
    \param str The input string.
    \return True if the FSM accepts the string, false otherwise.
  */
  bool AcceptsString(const std::string& str) const;

  inline static constexpr int NO_TRANSITION = -1;

  int Transition(int from, int16_t character) const {
    auto edges = fsm.edges[from];
    // TODO(yixin): test correctness for both cases
    if (edges.size() <= 16) {
      for (const auto& edge : edges) {
        if (edge.min > character) {
          return NO_TRANSITION;
        } else if (edge.max >= character) {
          return edge.target;
        }
      }
      return NO_TRANSITION;
    } else {
      auto it = std::lower_bound(
          edges.begin(),
          edges.end(),
          character,
          [](const FSMEdge& edge, int16_t character) { return edge.min <= character; }
      );
      if (it != edges.end() && it->min <= character) {
        return it->target;
      }
      return NO_TRANSITION;
    }
  }

  /*! \brief Returns the start state of the FSM. */
  int StartState() const { return start; }

  /*!
   * \brief Checks if a given state is an end/accepting state.
   * \param state The state to check.
   * \return True if the state is an end state, false otherwise.
   */
  bool IsEndState(int state) const {
    return std::any_of(ends.begin(), ends.end(), [state](int end_state) {
      return end_state == state;
    });
  }

  /*! \brief Returns the total number of states in the FSM. */
  int NumStates() const { return fsm.edges.Size(); }

  friend std::ostream& operator<<(std::ostream& os, const CompactFSM& fsm);

  friend std::size_t MemorySize(const CompactFSMWithStartEnd& self) {
    return MemorySize(self.fsm.edges) + MemorySize(self.ends);
  }

  /*!
    \brief Get all the possible rule numbers for a given state.
    \param state_num The state number.
    \param rules The set of possible rule numbers.s
  */
  void GetPossibleRules(const int& state_num, std::unordered_set<int>* rules) const;
};

/*!
  \brief Converts a regex string to a FSM. The parsing range is [start, end).
  \param regex The regex string.
  \return The FSM with start and end states.
*/
Result<FSMWithStartEnd> RegexToFSM(const std::string& regex);

class RegexIR {
 public:
  struct Leaf;

  struct Symbol;

  struct Union;

  struct Bracket;

  struct Repeat;

  static constexpr int REPEATNOUPPERBOUND = -1;

  using State = std::variant<Leaf, Symbol, Union, Bracket, Repeat>;

  // This struct is used to store the string in regex, or
  // the character class in regex.
  struct Leaf {
    std::string regex;
  };

  // This struct is used to store the symbol in regex, i.e.
  // +, *, ?
  enum class RegexSymbol {
    star,
    plus,
    optional,
  };

  struct Bracket {
    std::vector<State> states;
  };

  struct Symbol {
    RegexSymbol symbol;
    std::vector<State> state;
  };

  // This struct is used to represent a union symbol.
  struct Union {
    std::vector<State> states;
  };

  struct Repeat {
    std::vector<State> states;
    int lower_bound = 0;
    int upper_bound = 0;
  };

  struct LookAhead {
    bool is_positive;
    std::vector<State> states;
  };

  // This struct is used to represent a bracket in regex.
  std::vector<State> states;

  /*!
    \brief Constructs a NFA from the regex IR.
  */
  Result<FSMWithStartEnd> Build() const;

  /*!
    \brief the visit function for the variant.
  */
  Result<FSMWithStartEnd> visit(const Leaf& state) const;

  Result<FSMWithStartEnd> visit(const Symbol& state) const;

  Result<FSMWithStartEnd> visit(const Union& state) const;

  Result<FSMWithStartEnd> visit(const Bracket& state) const;

  Result<FSMWithStartEnd> visit(const Repeat& state) const;

  Result<FSMWithStartEnd> visit(const LookAhead& state) const;
};

/*!
  \brief Build a FSM from a list of patterns.
  \param patterns The patterns to be built.
  \param end_states The end states of the FSM.
  \return The FSM with start and end states.
*/
FSMWithStartEnd BuildTrie(
    const std::vector<std::string>& patterns, std::vector<int32_t>* end_states = nullptr
);

}  // namespace xgrammar

#endif  // XGRAMMAR_FSM_H_
