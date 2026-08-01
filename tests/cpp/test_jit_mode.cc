#include <gtest/gtest.h>

#include <future>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "compiled_grammar_impl.h"
#include "xgrammar/compiler.h"
#include "xgrammar/tokenizer_info.h"

namespace xgrammar {
namespace {

TEST(JitModeTest, ConcurrentGenerationReusesOneMask) {
  std::vector<std::string> vocabulary;
  for (int value = 32; value < 127; ++value) {
    vocabulary.emplace_back(1, static_cast<char>(value));
  }
  vocabulary.insert(vocabulary.end(), {"alpha", "beta", "gamma", "delta"});
  TokenizerInfo tokenizer_info(vocabulary, VocabType::RAW, std::nullopt, std::vector<int32_t>{});
  GrammarCompiler compiler(
      tokenizer_info,
      /*max_threads=*/1,
      /*cache_enabled=*/false,
      /*max_memory_bytes=*/-1,
      /*jit_mode=*/true
  );
  CompiledGrammar compiled_grammar = compiler.CompileGrammar(R"(root ::= [a-z]+ "!")");

  const int32_t root_rule_id = compiled_grammar->grammar->GetRootRuleId();
  const auto root_rule = compiled_grammar->grammar->GetRule(root_rule_id);
  const auto& root_fsm = compiled_grammar->grammar->per_rule_fsms[root_rule_id]->GetFsm();
  std::unordered_set<int> reachable_states;
  root_fsm.GetReachableStates(&reachable_states);
  int32_t scanable_state_id = -1;
  for (int32_t state_id : reachable_states) {
    if (root_fsm.IsScanableState(state_id)) {
      scanable_state_id = state_id;
      break;
    }
  }
  ASSERT_NE(scanable_state_id, -1);

  const ParserState state(
      root_rule_id, root_rule.body_expr_id, scanable_state_id, ParserState::kNoPrevInputPos
  );
  constexpr int kThreadCount = 32;
  std::vector<const AdaptiveTokenMask*> generated_masks(kThreadCount);
  std::promise<void> start_promise;
  std::shared_future<void> start_signal = start_promise.get_future().share();
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int index = 0; index < kThreadCount; ++index) {
    threads.emplace_back([&, index] {
      start_signal.wait();
      generated_masks[index] = &compiled_grammar->GetAdaptiveTokenMask(state, true);
    });
  }

  start_promise.set_value();
  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_NE(generated_masks[0], nullptr);
  for (const AdaptiveTokenMask* mask : generated_masks) {
    EXPECT_EQ(mask, generated_masks[0]);
  }
  EXPECT_EQ(compiled_grammar->adaptive_token_mask_cache.size(), 1);
}

}  // namespace
}  // namespace xgrammar
