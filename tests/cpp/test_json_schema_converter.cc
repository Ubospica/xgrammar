#include <gtest/gtest.h>

#include "json_schema_converter.h"

using namespace xgrammar;

TEST(XGrammarJSONSchemaConverterTest, GenerateRangeRegexTest) {
  std::string regex = GenerateRangeRegex(-10, -1);
  std::cout << regex << std::endl;
}
