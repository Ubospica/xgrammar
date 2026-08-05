#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "compiled_grammar_impl.h"
#include "support/container.h"

TEST(XGrammarCacheMemoryTest, ListEraseReleasesValue) {
  xgrammar::List<std::shared_ptr<std::vector<int>>> values;
  auto value = std::make_shared<std::vector<int>>(1024);
  std::weak_ptr<std::vector<int>> weak_value = value;
  auto iterator = values.PushBack(value);
  value.reset();

  EXPECT_FALSE(weak_value.expired());
  values.Erase(iterator);
  EXPECT_TRUE(weak_value.expired());
}

TEST(XGrammarCacheMemoryTest, AdaptiveTokenMaskCountsReservedCapacity) {
  xgrammar::AdaptiveTokenMask token_mask;
  token_mask.accepted_indices.reserve(128);

  EXPECT_GE(MemorySize(token_mask), 128 * sizeof(int32_t));
}
