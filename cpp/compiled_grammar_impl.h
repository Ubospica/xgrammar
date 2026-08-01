/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/compiled_grammar_impl.h
 * \brief The header for the data structures of the compiled grammar.
 */
#ifndef XGRAMMAR_COMPILED_GRAMMAR_IMPL_H_
#define XGRAMMAR_COMPILED_GRAMMAR_IMPL_H_

#include <xgrammar/grammar.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "earley_parser.h"
#include "support/dynamic_bitset.h"
#include "support/reflection.h"
#include "xgrammar/compiler.h"
#include "xgrammar/exception.h"

namespace xgrammar {

class RuleLevelCache;

/******************* CompiledGrammar Datastructures *******************/

struct CharacterClassTokenSummary {
  int32_t sorted_vocab_index;
  int32_t consumed_characters;
  bool consumed_whole_token;
  bool has_completed_character_prefix;
};

struct IntVectorHash {
  size_t operator()(const std::vector<int32_t>& values) const {
    size_t result = 0;
    for (int32_t value : values) {
      result ^= std::hash<int32_t>{}(value) + 0x9e3779b9 + (result << 6) + (result >> 2);
    }
    return result;
  }
};

struct RepeatedCharacterClassTokenMaskKey {
  std::vector<int32_t> character_class;
  int32_t max_characters;

  bool operator==(const RepeatedCharacterClassTokenMaskKey& other) const {
    return max_characters == other.max_characters && character_class == other.character_class;
  }

  friend std::size_t MemorySize(const RepeatedCharacterClassTokenMaskKey& key) {
    return MemorySize(key.character_class);
  }
};

struct RepeatedCharacterClassTokenMaskKeyHash {
  size_t operator()(const RepeatedCharacterClassTokenMaskKey& key) const {
    size_t result = std::hash<int32_t>{}(key.max_characters);
    for (int32_t value : key.character_class) {
      result ^= std::hash<int32_t>{}(value) + 0x9e3779b9 + (result << 6) + (result >> 2);
    }
    return result;
  }
};

/*!
 * \brief Preprocessed information, for a given specific ParserState, divides the token set
 * into three categories: accepted, rejected, and uncertain.
 * Accepted: tokens that can be determined by the current ParserState to be acceptable
 * Rejected: tokens that can be determined by the current ParserState to be unacceptable
 * Uncertain: tokens that need the state of the parent ParserStates to determine if acceptable
 *
 * \note uncertain indices are stored directly. Accepted / rejected indices have three ways to
 * store to reduce memory and computation usage. See StoreType.
 * \note These indices are the indices of sorted_decoded_vocab in the CompiledGrammar
 * object, instead of the token ids. That helps the matching process.
 */
struct AdaptiveTokenMask {
  enum class StoreType {
    // Only store all accepted token indices. Then rejected indices = all_indices - accepted_indices
    // - uncertain_indices. This is useful when |accepted_indices| < |rejected_indices|.
    kAccepted = 0,
    // Only store all rejected token indices. Then accepted indices = all_indices - rejected_indices
    // - uncertain_indices. This is useful when |accepted_indices| > |rejected_indices|.
    kRejected = 1,
    // Store all accepted token indices in a bitset. This is useful when both |accepted_indices| and
    // |rejected_indices| are large.
    kAcceptedBitset = 2
  };
  StoreType store_type;

  static constexpr int USE_BITSET_THRESHOLD = 1000;

  std::vector<int32_t> accepted_indices;
  std::vector<int32_t> rejected_indices;
  DynamicBitset accepted_bitset;

  std::vector<int32_t> uncertain_indices;

  /*! \brief Default constructor. Only for deserialization. */
  AdaptiveTokenMask() = default;

  AdaptiveTokenMask(
      size_t vocab_size,
      const std::vector<std::pair<int32_t, std::string>>& sorted_decoded_vocab,
      const std::vector<int32_t>& accepted_indices,
      const std::vector<int32_t>& rejected_indices,
      const std::vector<int32_t>& uncertain_indices
  );

  AdaptiveTokenMask(
      size_t vocab_size,
      const std::vector<std::pair<int32_t, std::string>>& sorted_decoded_vocab,
      const std::vector<int32_t>& accepted_indices,
      const std::vector<int32_t>& uncertain_indices
  );

  std::string Print(const TokenizerInfo& tokenizer_info) const;

  friend std::size_t MemorySize(const AdaptiveTokenMask& mask) {
    return MemorySize(mask.uncertain_indices) + MemorySize(mask.accepted_indices) +
           MemorySize(mask.rejected_indices) + MemorySize(mask.accepted_bitset);
  }
};

XGRAMMAR_MEMBER_TABLE(
    AdaptiveTokenMask,
    "store_type",
    &AdaptiveTokenMask::store_type,
    "accepted_indices",
    &AdaptiveTokenMask::accepted_indices,
    "rejected_indices",
    &AdaptiveTokenMask::rejected_indices,
    "accepted_bitset",
    &AdaptiveTokenMask::accepted_bitset,
    "uncertain_indices",
    &AdaptiveTokenMask::uncertain_indices
);

/*!
 * \brief All information that we need to match tokens in the tokenizer to the specified grammar.
 * It is the result of preprocessing.
 * \sa xgrammar::GrammarMatcher
 */
class CompiledGrammar::Impl {
 public:
  /*! \brief The grammar for the GrammarMatcher. */
  Grammar grammar{NullObj{}};

  /*! \brief The tokenizer information. */
  TokenizerInfo tokenizer_info{NullObj{}};

  /*! \brief Immutable Earley parser metadata shared by token mask generators and matchers. */
  EarleyParserGrammarMetadata earley_parser_metadata;

  /*! \brief Default constructor. */
  Impl() = default;

  /*! \brief Mapping from the parser state to the adaptive token mask. */
  std::unordered_map<ParserState, AdaptiveTokenMask, StateHashForCache, StateEqualForCache>
      adaptive_token_mask_cache;

  /*! \brief Protects token masks generated after compilation. */
  mutable std::mutex adaptive_token_mask_cache_mutex;

  /*! \brief Whether missing token masks should be generated on first use. */
  bool jit_mode{false};

  /*! \brief Cache shared by grammars compiled by the same compiler. */
  std::shared_ptr<RuleLevelCache> rule_level_cache;

  /*! \brief Tag dispatch data retained for token mask generation. */
  std::unordered_map<int32_t, DynamicBitset> tag_dispatch_rule_id_to_second_slicing_bitset;

  /*! \brief Token summaries and masks generated for repeated character classes. */
  std::unordered_map<std::vector<int32_t>, std::vector<CharacterClassTokenSummary>, IntVectorHash>
      character_class_token_summaries;
  std::unordered_map<
      RepeatedCharacterClassTokenMaskKey,
      AdaptiveTokenMask,
      RepeatedCharacterClassTokenMaskKeyHash>
      repeated_character_class_token_masks;
  mutable std::mutex repeated_character_class_token_masks_mutex;

  /*! \brief Get a cached token mask, generating it when jit_mode is enabled. */
  const AdaptiveTokenMask& GetAdaptiveTokenMask(const ParserState& state, bool is_root_rule);

  /*! \brief Get the token mask for a repeated character class. */
  const AdaptiveTokenMask& GetRepeatedCharacterClassTokenMask(
      int32_t character_class_expr_id, int32_t max_characters
  );

  /*! \brief Generate every token mask before serialization. */
  void MaterializeAdaptiveTokenMaskCache();

  Grammar GetGrammar() const { return grammar; }

  TokenizerInfo GetTokenizerInfo() const { return tokenizer_info; }

  friend struct member_trait<Impl>;
  friend picojson::value SerializeJSONValue(const Impl& impl);
  friend std::optional<SerializationError> DeserializeJSONValue(
      CompiledGrammar::Impl* impl,
      const picojson::value& json_value,
      const TokenizerInfo& tokenizer_info
  );
  friend std::size_t MemorySize(const Impl& impl);
};

XGRAMMAR_MEMBER_TABLE(
    CompiledGrammar::Impl,
    "grammar",
    &CompiledGrammar::Impl::grammar,
    "tokenizer_info",
    &CompiledGrammar::Impl::tokenizer_info,
    "adaptive_token_mask_cache",
    &CompiledGrammar::Impl::adaptive_token_mask_cache
);

}  // namespace xgrammar

#endif  // XGRAMMAR_COMPILED_GRAMMAR_IMPL_H_
