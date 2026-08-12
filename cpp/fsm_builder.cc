/*!
 *  Copyright (c) 2025 by Contributors
 * \file xgrammar/fsm_builder.cc
 */
#include "fsm_builder.h"

#include <sys/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <stack>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "fsm.h"
#include "support/logging.h"
#include "support/utils.h"

namespace xgrammar {

namespace {

int HexDigitValue(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

bool MustEscapeNormalizedRegexByte(uint8_t byte, bool in_character_class) {
  if (byte == '\\') {
    return true;
  }
  const std::string syntax = in_character_class ? "[]^-" : ".+*?()|{}[]^$";
  return syntax.find(static_cast<char>(byte)) != std::string::npos;
}

/*! \brief Convert valid ECMAScript \xHH escapes into one byte before the legacy parser, which
 * otherwise consumes every escape as exactly two source characters. Regex syntax bytes stay
 * escaped, while ordinary bytes are emitted literally so values such as \x6e cannot turn into
 * the legacy parser's special \n escape. */
std::string NormalizeRegexHexEscapes(const std::string& regex) {
  std::string result;
  result.reserve(regex.size());
  bool in_character_class = false;
  for (size_t i = 0; i < regex.size(); ++i) {
    if (regex[i] == '\\' && i + 1 < regex.size()) {
      if (regex[i + 1] == 'x' && i + 3 < regex.size()) {
        int high = HexDigitValue(regex[i + 2]);
        int low = HexDigitValue(regex[i + 3]);
        if (high != -1 && low != -1) {
          uint8_t byte = static_cast<uint8_t>((high << 4) | low);
          if (MustEscapeNormalizedRegexByte(byte, in_character_class)) {
            result.push_back('\\');
          }
          result.push_back(static_cast<char>(byte));
          i += 3;
          continue;
        }
      }
      // Preserve every other escape verbatim. Advancing over its escaped byte also prevents an
      // escaped '[' or ']' from changing character-class state.
      result.push_back(regex[i]);
      result.push_back(regex[i + 1]);
      ++i;
      continue;
    }
    result.push_back(regex[i]);
    if (regex[i] == '[' && !in_character_class) {
      in_character_class = true;
    } else if (regex[i] == ']' && in_character_class) {
      in_character_class = false;
    }
  }
  return result;
}

void AddFixedByteSequence(FSM* fsm, int from, int to, const std::string& bytes) {
  int current = from;
  for (size_t i = 0; i < bytes.size(); ++i) {
    int next = i + 1 == bytes.size() ? to : fsm->AddState();
    uint8_t byte = static_cast<uint8_t>(bytes[i]);
    fsm->AddEdge(current, next, byte, byte);
    current = next;
  }
}

void AddHexDigitRange(FSM* fsm, int from, int to, int low, int high) {
  XGRAMMAR_DCHECK(0 <= low && low <= high && high <= 15);
  if (low <= 9) {
    fsm->AddEdge(from, to, '0' + low, '0' + std::min(high, 9));
  }
  if (high >= 10) {
    int letter_low = std::max(low, 10) - 10;
    int letter_high = high - 10;
    fsm->AddEdge(from, to, 'A' + letter_low, 'A' + letter_high);
    fsm->AddEdge(from, to, 'a' + letter_low, 'a' + letter_high);
  }
}

/*! \brief Add the compact family of \u00XX spellings for the ASCII range [low, high]. */
void AddASCIIJSONUnicodeEscapeRange(FSM* fsm, int from, int to, int low, int high) {
  XGRAMMAR_DCHECK(0 <= low && low <= high && high <= 0x7F);
  int after_prefix = fsm->AddState();
  AddFixedByteSequence(fsm, from, after_prefix, "\\u00");

  int first_high = low >> 4;
  int last_high = high >> 4;
  auto add_branch = [&](int high_low, int high_high, int low_low, int low_high) {
    int after_high = fsm->AddState();
    AddHexDigitRange(fsm, after_prefix, after_high, high_low, high_high);
    AddHexDigitRange(fsm, after_high, to, low_low, low_high);
  };
  if (first_high == last_high) {
    add_branch(first_high, first_high, low & 0xF, high & 0xF);
    return;
  }
  add_branch(first_high, first_high, low & 0xF, 0xF);
  if (first_high + 1 <= last_high - 1) {
    add_branch(first_high + 1, last_high - 1, 0, 0xF);
  }
  add_branch(last_high, last_high, 0, high & 0xF);
}

void AddJSONStringByteRange(FSM* fsm, int from, int to, int low, int high) {
  static constexpr std::array<std::pair<int, int>, 3> kRawAllowed = {
      std::pair<int, int>{0x20, 0x21}, {0x23, 0x5B}, {0x5D, 0xFF}
  };
  for (const auto& allowed : kRawAllowed) {
    int raw_low = std::max(low, allowed.first);
    int raw_high = std::min(high, allowed.second);
    if (raw_low <= raw_high) {
      fsm->AddEdge(from, to, raw_low, raw_high);
    }
  }

  int ascii_low = std::max(low, 0);
  int ascii_high = std::min(high, 0x7F);
  if (ascii_low <= ascii_high) {
    AddASCIIJSONUnicodeEscapeRange(fsm, from, to, ascii_low, ascii_high);
  }

  static constexpr std::array<std::pair<int, char>, 8> kShortEscapes = {
      std::pair<int, char>{'\"', '\"'},
      {'\\', '\\'},
      {'/', '/'},
      {'\b', 'b'},
      {'\f', 'f'},
      {'\n', 'n'},
      {'\r', 'r'},
      {'\t', 't'},
  };
  for (const auto& escaped : kShortEscapes) {
    if (low <= escaped.first && escaped.first <= high) {
      AddFixedByteSequence(fsm, from, to, std::string{'\\', escaped.second});
    }
  }
}

}  // namespace

class RegexIR {
 public:
  struct Leaf;

  struct Symbol;

  struct Union;

  struct Bracket;

  struct Repeat;

  static constexpr int kRepeatNoUpperBound = -1;

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

 private:
  /*!
   * \brief Construct a FSM from a regex string.
   * \details The regex string should only be the format like "abx" or [a-c0-9].
   * \details Any symbols like "a|b" or "a*b" are not supported.
   * \param regex The regex string.
   * \return The FSM with start and end states.
   */
  static FSMWithStartEnd BuildLeafFSMFromRegex(const std::string& regex);

  /*!
   * \brief Handle escape characters.
   * \param regex the corresponding string.
   * \param start the pos escape characters start.
   */
  static std::vector<std::pair<int, int>> HandleEscapes(const std::string& regex, int start);

  /*!
   * \brief Check repeat in regex. i.e {...} and {...,...}
   * \param regex The regex string.
   * \param start The start position of the repeat. i.e. regex[start] == '{'.
   * After the function, start will be the position of '}'.
   * \return The repeat range.
   */
  static Result<std::pair<int, int>> CheckRepeat(const std::string& regex, int& start);

  friend class RegexFSMBuilder;
};

Result<std::pair<int, int>> RegexIR::CheckRepeat(const std::string& regex, int& start) {
  if (regex[start] != '{') {
    return ResultErr("Invalid repeat format1");
  }
  int lower_bound = 0;
  int upper_bound = RegexIR::kRepeatNoUpperBound;
  std::string num_str;
  XGRAMMAR_DCHECK(regex[start] == '{');
  start++;
  while (static_cast<size_t>(start) < regex.size() && regex[start] == ' ') {
    start++;
  }
  while (static_cast<size_t>(start) < regex.size() && std::isdigit(regex[start])) {
    num_str += regex[start];
    start++;
  }
  if (num_str.empty()) {
    return ResultErr("Invalid repeat format2");
  }
  lower_bound = std::stoi(num_str);
  while (static_cast<size_t>(start) < regex.size() && regex[start] == ' ') {
    start++;
  }
  // The format is {n}
  if (regex[start] == '}') {
    upper_bound = lower_bound;
    return ResultOk(std::make_pair(lower_bound, upper_bound));
  }
  if (regex[start] != ',') {
    return ResultErr("Invalid repeat format3");
  }
  XGRAMMAR_DCHECK(regex[start] == ',');
  start++;
  while (static_cast<size_t>(start) < regex.size() && regex[start] == ' ') {
    start++;
  }
  // The format is {n,}
  if (regex[start] == '}') {
    return ResultOk(std::make_pair(lower_bound, upper_bound));
  }
  num_str.clear();
  while (static_cast<size_t>(start) < regex.size() && std::isdigit(regex[start])) {
    num_str += regex[start];
    start++;
  }
  if (num_str.empty()) {
    return ResultErr("Invalid repeat format4");
  }
  upper_bound = std::stoi(num_str);
  if (upper_bound < lower_bound) {
    return ResultErr("Invalid repeat: the lower bound is larger than the upper bound");
  }
  while (static_cast<size_t>(start) < regex.size() && regex[start] == ' ') {
    start++;
  }
  if (regex[start] != '}') {
    return ResultErr("Invalid repeat format5");
  }
  XGRAMMAR_DCHECK(regex[start] == '}');
  return ResultOk(std::make_pair(lower_bound, upper_bound));
}

Result<FSMWithStartEnd> RegexIR::Build() const {
  if (states.empty()) {
    FSM empty_fsm(1);
    FSMWithStartEnd result(empty_fsm, 0, {0}, false);
    return ResultOk(std::move(result));
  }
  std::vector<FSMWithStartEnd> fsm_list;
  for (const auto& state : states) {
    auto visited = std::visit([&](auto&& arg) { return visit(arg); }, state);
    if (visited.IsErr()) {
      return visited;
    }
    fsm_list.push_back(std::move(visited).Unwrap());
  }
  if (fsm_list.size() > 1) {
    return ResultOk(FSMWithStartEnd::Concat(fsm_list));
  } else {
    // If there is only one FSM, return it directly.
    return ResultOk(std::move(fsm_list[0]));
  }
}

Result<FSMWithStartEnd> RegexIR::visit(const RegexIR::Leaf& state) const {
  FSMWithStartEnd result = BuildLeafFSMFromRegex(state.regex);
  return ResultOk(std::move(result));
}

Result<FSMWithStartEnd> RegexIR::visit(const RegexIR::Union& state) const {
  std::vector<FSMWithStartEnd> fsm_list;
  for (const auto& child : state.states) {
    auto visited = std::visit([&](auto&& arg) { return RegexIR::visit(arg); }, child);
    if (visited.IsErr()) {
      return visited;
    }
    fsm_list.push_back(std::move(visited).Unwrap());
  }
  if (fsm_list.size() <= 1) {
    return ResultErr("Invalid union");
  }
  return ResultOk(FSMWithStartEnd::Union(fsm_list));
}

Result<FSMWithStartEnd> RegexIR::visit(const RegexIR::Symbol& state) const {
  if (state.state.size() != 1) {
    return ResultErr("Invalid symbol");
  }
  Result<FSMWithStartEnd> child_result =
      std::visit([&](auto&& arg) { return RegexIR::visit(arg); }, state.state[0]);
  if (child_result.IsErr()) {
    return child_result;
  }
  auto child = std::move(child_result).Unwrap();

  switch (state.symbol) {
    case RegexIR::RegexSymbol::plus: {
      return ResultOk(child.Plus());
    }
    case RegexIR::RegexSymbol::star: {
      return ResultOk(child.Star());
    }
    case RegexIR::RegexSymbol::optional: {
      return ResultOk(child.Optional());
    }
    default: {
      XGRAMMAR_LOG(FATAL) << "Unknown regex symbol: " << static_cast<int>(state.symbol);
      XGRAMMAR_UNREACHABLE();
    }
  }
}

Result<FSMWithStartEnd> RegexIR::visit(const RegexIR::Bracket& state) const {
  std::vector<FSMWithStartEnd> fsm_list;
  for (const auto& child : state.states) {
    auto visited = std::visit([&](auto&& arg) { return RegexIR::visit(arg); }, child);
    if (visited.IsErr()) {
      return visited;
    }
    fsm_list.push_back(std::move(visited).Unwrap());
  }
  if (fsm_list.empty()) {
    return ResultErr("Invalid bracket");
  }
  return ResultOk(FSMWithStartEnd::Concat(fsm_list));
}

Result<FSMWithStartEnd> RegexIR::visit(const RegexIR::Repeat& state) const {
  if (state.states.size() != 1) {
    return ResultErr("Invalid repeat");
  }
  bool has_upper_bound = state.upper_bound != RegexIR::kRepeatNoUpperBound;
  if (has_upper_bound && state.upper_bound == 0) {
    // {0} / {0,0}: matches exactly the empty string. The general path below cannot express
    // this: it starts from one copy of the child whose end states stay accepting.
    FSM empty_fsm(1);
    return ResultOk(FSMWithStartEnd(empty_fsm, 0, {0}, false));
  }
  Result<FSMWithStartEnd> child_result =
      std::visit([&](auto&& arg) { return RegexIR::visit(arg); }, state.states[0]);
  if (child_result.IsErr()) {
    return child_result;
  }
  FSMWithStartEnd child = std::move(child_result).Unwrap();
  FSMWithStartEnd result = child.Copy();
  std::unordered_set<int> new_ends;

  if (state.lower_bound <= 1 && (!has_upper_bound || state.upper_bound >= 1)) {
    // A single copy is accepting when the lower bound is at most 1.
    for (int end = 0; end < result.NumStates(); ++end) {
      if (result.IsEndState(end)) {
        new_ends.insert(end);
      }
    }
  }

  // Add a fresh accepting start state so that zero repetitions match. A fresh state is
  // required: making the original start accepting would also accept strings that merely
  // loop back to the start inside the first copy.
  auto allow_zero_repetitions = [](FSMWithStartEnd* fsm) {
    int new_start = fsm->AddState();
    fsm->GetFsm().AddEpsilonEdge(new_start, fsm->GetStart());
    fsm->SetStartState(new_start);
    fsm->AddEndState(new_start);
  };

  // Handling {n,}
  if (!has_upper_bound) {
    for (int i = 2; i < state.lower_bound; i++) {
      result = FSMWithStartEnd::Concat(std::vector<FSMWithStartEnd>{result, child});
    }
    int end_state_of_lower_bound_fsm = -1;
    for (int end = 0; end < result.NumStates(); ++end) {
      if (result.IsEndState(end)) {
        end_state_of_lower_bound_fsm = end;
        break;
      }
    }
    XGRAMMAR_DCHECK(end_state_of_lower_bound_fsm != -1)
        << "No end state found in the lower bound FSM.";
    result = FSMWithStartEnd::Concat(std::vector<FSMWithStartEnd>{result, child});
    for (int end = 0; end < result.NumStates(); ++end) {
      if (result.IsEndState(end)) {
        result.GetFsm().AddEpsilonEdge(end, end_state_of_lower_bound_fsm);
      }
    }
    for (const auto& end : new_ends) {
      result.AddEndState(end);
    }
    if (state.lower_bound == 0) {
      allow_zero_repetitions(&result);
    }
    return ResultOk(std::move(result));
  }
  // Handling {n, m} or {n}
  for (int i = 2; i <= state.upper_bound; i++) {
    result = FSMWithStartEnd::Concat(std::vector<FSMWithStartEnd>{result, child});
    if (i >= state.lower_bound) {
      for (int end = 0; end < result.NumStates(); ++end) {
        if (result.IsEndState(end)) {
          new_ends.insert(end);
        }
      }
    }
  }
  for (const auto& end : new_ends) {
    result.AddEndState(end);
  }
  if (state.lower_bound == 0) {
    allow_zero_repetitions(&result);
  }
  return ResultOk(std::move(result));
}

FSMWithStartEnd RegexIR::BuildLeafFSMFromRegex(const std::string& regex) {
  FSM empty_fsm(0);
  FSMWithStartEnd result(empty_fsm, 0, {}, true);
  // Handle the regex string.
  if (!(regex[0] == '[' && regex[regex.size() - 1] == ']')) {
    result.AddState();
    for (size_t i = 0; i < regex.size(); i++) {
      if (regex[i] != '\\') {
        if (regex[i] == '.') {
          result.GetFsm().AddEdge(result.NumStates() - 1, result.NumStates(), 0, 0xFF);
        } else {
          result.GetFsm().AddEdge(
              result.NumStates() - 1,
              result.NumStates(),
              static_cast<uint8_t>(regex[i]),
              static_cast<uint8_t>(regex[i])
          );
        }
        result.AddState();
        continue;
      }
      std::vector<std::pair<int, int>> escape_vector = HandleEscapes(regex, i);
      for (const auto& escape : escape_vector) {
        result.GetFsm().AddEdge(
            result.NumStates() - 1,
            result.NumStates(),
            static_cast<uint8_t>(escape.first),
            static_cast<uint8_t>(escape.second)
        );
      }
      result.AddState();
      i++;
    }
    result.AddEndState(result.NumStates() - 1);
  } else if (regex[0] == '[' && regex[regex.size() - 1] == ']') {
    // Handle the character class.
    result.AddState();
    result.AddState();
    result.AddEndState(1);
    bool reverse = regex[1] == '^';
    for (size_t i = reverse ? 2 : 1; i < regex.size() - 1; i++) {
      if (regex[i] != '\\') {
        if (!(((i + 2) < regex.size() - 1) && regex[i + 1] == '-')) {
          // A single char.
          result.GetFsm().AddEdge(
              0, 1, static_cast<uint8_t>(regex[i]), static_cast<uint8_t>(regex[i])
          );
          continue;
        }
        // Handle the char range.
        if (regex[i + 2] != '\\') {
          result.GetFsm().AddEdge(
              0, 1, static_cast<uint8_t>(regex[i]), static_cast<uint8_t>(regex[i + 2])
          );
          i = i + 2;
          continue;
        }
        auto escaped_edges = HandleEscapes(regex, i + 2);
        // Means it's not a range.
        if (escaped_edges.size() != 1 || escaped_edges[0].first != escaped_edges[0].second) {
          result.GetFsm().AddEdge(
              0, 1, static_cast<uint8_t>(regex[i]), static_cast<uint8_t>(regex[i])
          );
          continue;
        }
        result.GetFsm().AddEdge(
            0, 1, static_cast<uint8_t>(regex[0]), static_cast<uint8_t>(escaped_edges[0].first)
        );
        i = i + 3;
        continue;
      }
      auto escaped_edges = HandleEscapes(regex, i);
      i = i + 1;
      if (escaped_edges.size() != 1 || escaped_edges[0].first != escaped_edges[0].second) {
        // It's a multi-match escape char.
        for (const auto& edge : escaped_edges) {
          result.GetFsm().AddEdge(
              0, 1, static_cast<uint8_t>(edge.first), static_cast<uint8_t>(edge.second)
          );
        }
        continue;
      }
      if (!(((i + 2) < regex.size() - 1) && regex[i + 1] == '-')) {
        result.GetFsm().AddEdge(
            0,
            1,
            static_cast<uint8_t>(escaped_edges[0].first),
            static_cast<uint8_t>(escaped_edges[0].second)
        );
        continue;
      }
      if (regex[i + 2] != '\\') {
        result.GetFsm().AddEdge(
            0, 1, static_cast<uint8_t>(escaped_edges[0].first), static_cast<uint8_t>(regex[i + 2])
        );
        i = i + 2;
        continue;
      }
      auto rhs_escaped_edges = HandleEscapes(regex, i + 2);
      if (rhs_escaped_edges.size() != 1 ||
          rhs_escaped_edges[0].first != rhs_escaped_edges[0].second) {
        result.GetFsm().AddEdge(
            0,
            1,
            static_cast<uint8_t>(escaped_edges[0].first),
            static_cast<uint8_t>(escaped_edges[0].second)
        );
        continue;
      }
      result.GetFsm().AddEdge(
          0,
          1,
          static_cast<uint8_t>(escaped_edges[0].first),
          static_cast<uint8_t>(rhs_escaped_edges[0].first)
      );
      i = i + 3;
      continue;
    }
    bool has_edge[0x100];
    memset(has_edge, 0, sizeof(has_edge));
    FSM new_fsm(2);
    for (const auto& edge : result.GetFsm().GetEdges(0)) {
      for (int i = edge.min; i <= edge.max; i++) {
        has_edge[i] = true;
      }
    }
    // Simplify the edges. e.g [abc] -> [a-c]
    int last = -1;
    if (reverse) {
      for (int i = 0; i < 0x100; i++) {
        if (!has_edge[i]) {
          if (last == -1) {
            last = i;
          }
          continue;
        }
        if (last != -1) {
          new_fsm.AddEdge(0, 1, last, i - 1);
          last = -1;
        }
      }
      if (last != -1) {
        new_fsm.AddEdge(0, 1, last, 0xFF);
      }
    } else {
      for (int i = 0; i < 0x100; i++) {
        if (has_edge[i]) {
          if (last == -1) {
            last = i;
          }
          continue;
        }
        if (last != -1) {
          new_fsm.AddEdge(0, 1, last, i - 1);
          last = -1;
        }
      }
      if (last != -1) {
        new_fsm.AddEdge(0, 1, last, 0xFF);
      }
    }
    result = FSMWithStartEnd(new_fsm, 0, {1}, false);
  } else {
    // TODO: The support for rules.
    XGRAMMAR_LOG(WARNING) << "rule is not supported yet.";
  }
  return result;
}

std::vector<std::pair<int, int>> RegexIR::HandleEscapes(const std::string& regex, int start) {
  std::vector<std::pair<int, int>> result;
  switch (regex[start + 1]) {
    case 'n': {
      return std::vector<std::pair<int, int>>(1, std::make_pair('\n', '\n'));
    }
    case 't': {
      return std::vector<std::pair<int, int>>(1, std::make_pair('\t', '\t'));
    }
    case 'r': {
      return std::vector<std::pair<int, int>>(1, std::make_pair('\r', '\r'));
    }

    case '0': {
      return std::vector<std::pair<int, int>>(1, std::make_pair('\0', '\0'));
    }
    case 's': {
      return std::vector<std::pair<int, int>>(1, std::make_pair(0, ' '));
    }
    case 'S': {
      return std::vector<std::pair<int, int>>(1, std::make_pair(' ' + 1, 0x00FF));
    }
    case 'd': {
      return std::vector<std::pair<int, int>>(1, std::make_pair('0', '9'));
    }
    case 'D': {
      std::vector<std::pair<int, int>> result;
      result.emplace_back(0, '0' - 1);
      result.emplace_back('9' + 1, 0x00FF);
      return result;
    }
    case 'w': {
      std::vector<std::pair<int, int>> result;
      result.emplace_back('0', '9');
      result.emplace_back('a', 'z');
      result.emplace_back('A', 'Z');
      result.emplace_back('_', '_');
      return result;
    }
    case 'W': {
      std::vector<std::pair<int, int>> result;
      result.emplace_back(0, '0' - 1);
      result.emplace_back('9' + 1, 'A' - 1);
      result.emplace_back('Z' + 1, '_' - 1);
      result.emplace_back('_' + 1, 'a' - 1);
      result.emplace_back('z' + 1, 0x00FF);
      return result;
    }
    default: {
      return std::vector<std::pair<int, int>>(
          1, std::make_pair(regex[start + 1], regex[start + 1])
      );
    }
  }
}

Result<FSMWithStartEnd> RegexFSMBuilder::Build(const std::string& input_regex) {
  const std::string regex = NormalizeRegexHexEscapes(input_regex);
  RegexIR ir;
  using IRState = std::variant<RegexIR::State, char>;
  // We use a stack to store the states.
  std::stack<IRState> stack;
  int left_middle_bracket = -1;
  for (int i = 0; i < static_cast<int>(regex.size()); i++) {
    if (i == 0 && regex[i] == '^') {
      continue;
    }
    if (i == static_cast<int>(regex.size()) - 1 && regex[i] == '$') {
      continue;
    }
    // Handle The class.
    if (regex[i] == '[') {
      if (left_middle_bracket != -1) {
        return ResultErr("Nested middle bracket!");
      }
      left_middle_bracket = i;
      continue;
    }
    if (regex[i] == ']') {
      if (left_middle_bracket == -1) {
        return ResultErr("Invalid middle bracket!");
      }
      RegexIR::Leaf leaf;
      leaf.regex = regex.substr(left_middle_bracket, i - left_middle_bracket + 1);
      stack.push(leaf);
      left_middle_bracket = -1;
      continue;
    }
    if (left_middle_bracket != -1) {
      if (regex[i] == '\\') {
        i++;
      }
      continue;
    }
    if (regex[i] == '+' || regex[i] == '*' || regex[i] == '?') {
      if (stack.empty()) {
        return ResultErr("Invalid regex: no state before operator!");
      }
      auto state = stack.top();
      if (std::holds_alternative<char>(state)) {
        return ResultErr("Invalid regex: no state before operator!");
      }
      stack.pop();
      auto child = std::get<RegexIR::State>(state);
      RegexIR::Symbol symbol;
      symbol.state.push_back(child);
      switch (regex[i]) {
        case '+': {
          symbol.symbol = RegexIR::RegexSymbol::plus;
          break;
        }
        case '*': {
          symbol.symbol = RegexIR::RegexSymbol::star;
          break;
        }
        case '?': {
          symbol.symbol = RegexIR::RegexSymbol::optional;
          break;
        }
      }
      stack.push(symbol);
      continue;
    }
    if (regex[i] == '(' || regex[i] == '|') {
      stack.push(regex[i]);
      if (i < static_cast<int>(regex.size()) - 2 && regex[i] == '(' && regex[i + 1] == '?' &&
          regex[i + 2] == ':') {
        i += 2;
        continue;
      }
      if (i < static_cast<int>(regex.size()) - 2 && regex[i] == '(' && regex[i + 1] == '?' &&
          (regex[i + 2] == '!' || regex[i + 2] == '=')) {
        i += 2;
        // TODO(Linzhang Li): Handling the lookahead.
        continue;
      }
      continue;
    }
    if (regex[i] == ')') {
      std::stack<IRState> states;
      bool paired = false;
      bool unioned = false;
      while ((!stack.empty()) && (!paired)) {
        auto state = stack.top();
        stack.pop();
        if (std::holds_alternative<char>(state)) {
          char c = std::get<char>(state);
          if (c == '(') {
            paired = true;
            break;
          }
          if (c == '|') {
            unioned = true;
          }
          states.push(state);
        } else {
          states.push(state);
        }
      }
      if (!paired) {
        return ResultErr("Invalid regex: no paired bracket!" + std::to_string(__LINE__));
      }
      if (states.empty()) {
        continue;
      }
      if (!unioned) {
        RegexIR::Bracket bracket;
        while (!states.empty()) {
          auto state = states.top();
          states.pop();
          auto child = std::get<RegexIR::State>(state);
          bracket.states.push_back(child);
        }
        stack.push(bracket);
      } else {
        RegexIR::Union union_state;
        RegexIR::Bracket bracket;
        while (!states.empty()) {
          auto state = states.top();
          states.pop();
          if (std::holds_alternative<char>(state)) {
            char c = std::get<char>(state);
            if (c == '|') {
              union_state.states.push_back(bracket);
              bracket.states.clear();
              continue;
            }
            return ResultErr("Invalid regex: no paired bracket!" + std::to_string(__LINE__));
          }
          if (std::holds_alternative<RegexIR::State>(state)) {
            auto child = std::get<RegexIR::State>(state);
            bracket.states.push_back(child);
            continue;
          }
          return ResultErr("Invalid regex: no paired bracket!" + std::to_string(__LINE__));
        }
        union_state.states.push_back(bracket);
        stack.push(union_state);
      }
      continue;
    }
    if (regex[i] == '{') {
      if (stack.empty()) {
        return ResultErr("Invalid regex: no state before repeat!");
      }
      auto state = stack.top();
      if (std::holds_alternative<char>(state)) {
        return ResultErr("Invalid regex: no state before repeat!");
      }
      stack.pop();
      auto bounds_result = RegexIR::CheckRepeat(regex, i);
      if (bounds_result.IsErr()) {
        return ResultErr(std::move(bounds_result).UnwrapErr());
      }
      auto bounds = std::move(bounds_result).Unwrap();
      auto child = std::get<RegexIR::State>(state);
      RegexIR::Repeat repeat;
      repeat.lower_bound = bounds.first;
      repeat.upper_bound = bounds.second;
      repeat.states.push_back(child);
      stack.push(repeat);
      continue;
    }
    RegexIR::Leaf leaf;
    if (regex[i] != '\\') {
      leaf.regex = regex[i];
    } else {
      leaf.regex = regex.substr(i, 2);
      i++;
    }
    stack.push(leaf);
    continue;
  }
  std::vector<RegexIR::State> res_states;
  std::vector<decltype(res_states)> union_state_list;
  bool unioned = false;
  while (!stack.empty()) {
    if (std::holds_alternative<char>(stack.top())) {
      char c = std::get<char>(stack.top());
      if (c == '|') {
        union_state_list.push_back(res_states);
        res_states.clear();
        unioned = true;
        stack.pop();
        continue;
      }
      return ResultErr("Invalid regex: no paired!");
    }
    auto state = stack.top();
    stack.pop();
    auto child = std::get<RegexIR::State>(state);
    res_states.push_back(std::move(child));
  }
  if (!unioned) {
    for (auto it = res_states.rbegin(); it != res_states.rend(); ++it) {
      ir.states.push_back(std::move(*it));
    }
  } else {
    union_state_list.push_back(res_states);
    RegexIR::Union union_state;
    for (auto it = union_state_list.begin(); it != union_state_list.end(); ++it) {
      RegexIR::Bracket bracket;
      for (auto state = it->rbegin(); state != it->rend(); ++state) {
        bracket.states.push_back(std::move(*state));
      }
      union_state.states.push_back(std::move(bracket));
    }
    ir.states.push_back(std::move(union_state));
  }
  return ir.Build();
}

Result<FSMWithStartEnd> RegexFSMBuilder::BuildWithForbiddenChars(
    const std::string& regex, const std::bitset<256>& forbidden_chars
) {
  auto build_result = Build(regex);
  if (build_result.IsErr() || forbidden_chars.none()) {
    return build_result;
  }
  auto fsm_wse = std::move(build_result).Unwrap();
  const auto& fsm = fsm_wse.GetFsm();
  FSM new_fsm(fsm_wse.NumStates());
  for (int state = 0; state < fsm_wse.NumStates(); ++state) {
    for (const auto& edge : fsm.GetEdges(state)) {
      if (!edge.IsCharRange()) {
        new_fsm.AddEdge(state, edge.target, edge.min, edge.max);
        continue;
      }
      // Split the character range into the maximal sub-ranges of allowed characters.
      int range_start = -1;
      for (int c = edge.min; c <= edge.max + 1; ++c) {
        if (c <= edge.max && !forbidden_chars[c]) {
          if (range_start == -1) {
            range_start = c;
          }
        } else if (range_start != -1) {
          new_fsm.AddEdge(state, edge.target, range_start, c - 1);
          range_start = -1;
        }
      }
    }
  }
  return ResultOk(FSMWithStartEnd(new_fsm, fsm_wse.GetStart(), fsm_wse.GetEnds()));
}

Result<FSMWithStartEnd> RegexFSMBuilder::BuildForJSONString(const std::string& regex) {
  auto build_result = Build(regex);
  if (build_result.IsErr()) {
    return build_result;
  }
  auto fsm_wse = std::move(build_result).Unwrap();
  const auto& fsm = fsm_wse.GetFsm();
  FSM new_fsm(fsm_wse.NumStates());
  for (int state = 0; state < fsm_wse.NumStates(); ++state) {
    for (const auto& edge : fsm.GetEdges(state)) {
      if (edge.IsCharRange()) {
        AddJSONStringByteRange(&new_fsm, state, edge.target, edge.min, edge.max);
      } else {
        new_fsm.AddEdge(state, edge.target, edge.min, edge.max);
      }
    }
  }
  return ResultOk(FSMWithStartEnd(new_fsm, fsm_wse.GetStart(), fsm_wse.GetEnds()));
}

class TrieFSMBuilderImpl {
 public:
  TrieFSMBuilderImpl() = default;
  std::optional<FSMWithStartEnd> Build(
      const std::vector<std::string>& patterns,
      const std::vector<std::string>& excluded_patterns,
      std::vector<int32_t>* end_states,
      bool allow_overlap,
      bool add_back_edges
  );
  void AddBackEdges(FSM* fsm, int start, const std::unordered_set<int>& ends);
};

std::optional<FSMWithStartEnd> TrieFSMBuilderImpl::Build(
    const std::vector<std::string>& patterns,
    const std::vector<std::string>& excluded_patterns,
    std::vector<int32_t>* end_states,
    bool allow_overlap,
    bool add_back_edges
) {
  FSM fsm(1);
  int start = 0;
  std::unordered_set<int> ends;

  if (end_states) {
    end_states->clear();
  }

  for (const auto& pattern : patterns) {
    // Check for empty patterns
    if (!allow_overlap && pattern.empty()) {
      return std::nullopt;
    }

    int current_state = 0;
    for (const auto& ch : pattern) {
      int32_t ch_int32 = static_cast<int32_t>(static_cast<uint8_t>(ch));
      int next_state = fsm.GetNextState(current_state, ch_int32);
      if (next_state == FSM::kNoNextState) {
        next_state = fsm.AddState();
        fsm.AddEdge(current_state, next_state, ch_int32, ch_int32);
      }
      current_state = next_state;
      if (!allow_overlap && ends.count(current_state) > 0) {
        return std::nullopt;
      }
    }
    if (!allow_overlap && fsm.GetEdges(current_state).size() > 0) {
      return std::nullopt;
    }
    ends.insert(current_state);
    if (end_states) {
      end_states->push_back(current_state);
    }
  }

  std::unordered_set<int32_t> dead_state_set;

  if (add_back_edges) {
    // Build trie for excluded patterns.
    for (const auto& excluded_pattern : excluded_patterns) {
      if (!allow_overlap && excluded_pattern.empty()) {
        return std::nullopt;
      }

      int current_state = 0;
      for (const auto& ch : excluded_pattern) {
        int32_t ch_int32 = static_cast<int32_t>(static_cast<uint8_t>(ch));
        int next_state = fsm.GetNextState(current_state, ch_int32);
        if (next_state == FSM::kNoNextState) {
          next_state = fsm.AddState();
          fsm.AddEdge(current_state, next_state, ch_int32, ch_int32);
        }
        current_state = next_state;
        if (!allow_overlap && ends.count(current_state) > 0) {
          return std::nullopt;
        }
      }
      if (!allow_overlap && fsm.GetEdges(current_state).size() > 0) {
        return std::nullopt;
      }

      ends.insert(current_state);
      dead_state_set.insert(current_state);
    }

    // Add back edges.
    AddBackEdges(&fsm, start, ends);

    // Remove the edges to excluded end states.
    if (dead_state_set.size() != 0) {
      for (int state = 0; state < fsm.NumStates(); state++) {
        std::vector<FSMEdge>& edges = fsm.GetEdges(state);
        std::vector<FSMEdge> new_edges;
        for (const auto& edge : edges) {
          if (dead_state_set.count(edge.target) == 0) {
            new_edges.push_back(edge);
          }
        }
        edges = std::move(new_edges);
      }
    }
  } else if (excluded_patterns.size() > 0) {
    XGRAMMAR_LOG(WARNING) << "Excluded patterns are ignored when back edges are not added.";
  }

  return FSMWithStartEnd(fsm, start, std::vector<int32_t>(ends.begin(), ends.end()));
}

void TrieFSMBuilderImpl::AddBackEdges(FSM* fsm, int start, const std::unordered_set<int>& ends) {
  // Build an Aho-Corasick automaton by adding back edges.
  // When matching on the trie fails at state u on byte b, the matcher must resume from
  // the longest proper suffix of u's prefix that is still a path in the trie (the
  // failure state), and retry b from there. Falling back only to the start state (or to
  // the start state's direct children) loses matches whose start lies inside an
  // already-followed branch of another pattern. Example: patterns {"bcd", "abce"} on
  // input "abcd" -- after following "abc" of the "abce" branch, 'd' must transition to
  // the "bcd" end state via the failure state "bc", not back to the start state.

  int num_states = fsm->NumStates();

  // Step 1. Record the BFS order of the trie (a tree at this point), so that shallower
  // states are always processed first.
  std::vector<int> bfs_order;
  bfs_order.reserve(num_states);
  bfs_order.push_back(start);
  for (size_t head = 0; head < bfs_order.size(); head++) {
    for (const auto& edge : fsm->GetEdges(bfs_order[head])) {
      XGRAMMAR_DCHECK(edge.min == edge.max && edge.min >= 0 && edge.min <= 255);
      bfs_order.push_back(edge.target);
    }
  }
  XGRAMMAR_DCHECK(static_cast<int>(bfs_order.size()) == num_states);

  // Step 2. Compute the failure link and the fully resolved transition table with the
  // textbook O(num_states * 256) dynamic program: delta[u][b] is the trie child when it
  // exists, and delta[fail[u]][b] otherwise -- fail[u] is strictly shallower than u, so
  // its row is already final when u is processed in BFS order.
  std::vector<int> fail(num_states, start);
  std::vector<std::array<int, 256>> delta(num_states);
  for (auto& row : delta) {
    row.fill(FSM::kNoNextState);
  }
  for (int u = 0; u < num_states; u++) {
    for (const auto& edge : fsm->GetEdges(u)) {
      delta[u][edge.min] = edge.target;
    }
  }
  for (int u : bfs_order) {
    for (int byte = 0; byte < 256; byte++) {
      // Entries of deeper states are untouched so far, so a non-empty entry here is
      // exactly a trie child of u.
      int child = delta[u][byte];
      int fallback = (u == start) ? start : delta[fail[u]][byte];
      if (child == FSM::kNoNextState) {
        delta[u][byte] = fallback;
      } else {
        fail[child] = fallback;
      }
    }
  }

  // Step 3. Overwrite the edges of every non-end state with its resolved row,
  // compressing consecutive bytes with the same target into range edges.
  for (int u = 0; u < num_states; u++) {
    if (u != start && ends.count(u) > 0) {
      continue;
    }
    const auto& row = delta[u];
    std::vector<FSMEdge> new_edges;
    for (int byte = 0; byte < 256;) {
      int target = row[byte];
      int range_end = byte;
      while (range_end + 1 < 256 && row[range_end + 1] == target) {
        range_end++;
      }
      new_edges.push_back(FSMEdge(byte, range_end, target));
      byte = range_end + 1;
    }
    fsm->GetEdges(u) = std::move(new_edges);
  }
}

std::optional<FSMWithStartEnd> TrieFSMBuilder::Build(
    const std::vector<std::string>& patterns,
    const std::vector<std::string>& exclude_patterns,
    std::vector<int32_t>* end_states,
    bool allow_overlap,
    bool add_back_edges
) {
  return TrieFSMBuilderImpl().Build(
      patterns, exclude_patterns, end_states, allow_overlap, add_back_edges
  );
}

}  // namespace xgrammar
