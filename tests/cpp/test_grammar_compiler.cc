#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "compiled_grammar_impl.h"
#include "grammar_impl.h"
#include "xgrammar/compiler.h"
#include "xgrammar/tokenizer_info.h"

namespace xgrammar {
namespace {

TEST(GrammarCompilerTest, EagerCompilationCachesEveryScanableState) {
  TokenizerInfo tokenizer_info(
      {"hello", "hi", " ", "a", "alice", "bob"},
      VocabType::RAW,
      std::nullopt,
      std::vector<int32_t>{}
  );
  GrammarCompiler compiler(
      tokenizer_info,
      /*max_threads=*/4,
      /*cache_enabled=*/false,
      /*max_memory_bytes=*/-1
  );
  const auto compiled_grammar = compiler.CompileGrammar(
      "root ::= greeting \" \" name\n"
      "greeting ::= \"hello\" | \"hi\"\n"
      "name ::= [a-z]+"
  );

  int32_t scanable_state_count = 0;
  for (int32_t rule_id = 0; rule_id < compiled_grammar->grammar->NumRules(); ++rule_id) {
    const auto& rule = compiled_grammar->grammar->GetRule(rule_id);
    const auto& rule_fsm = compiled_grammar->grammar->per_rule_fsms[rule_id];
    ASSERT_TRUE(rule_fsm.has_value());
    std::unordered_set<int> reachable_states;
    rule_fsm->GetFsm().GetReachableStates(&reachable_states);
    for (int32_t element_id : reachable_states) {
      if (!rule_fsm->GetFsm().IsScanableState(element_id)) {
        continue;
      }
      const ParserState state(
          rule_id, rule.body_expr_id, element_id, ParserState::kNoPrevInputPos, 0
      );
      EXPECT_NE(
          compiled_grammar->adaptive_token_mask_cache.find(state),
          compiled_grammar->adaptive_token_mask_cache.end()
      );
      ++scanable_state_count;
    }
  }

  EXPECT_EQ(compiled_grammar->adaptive_token_mask_cache.size(), scanable_state_count);
}

}  // namespace
}  // namespace xgrammar
