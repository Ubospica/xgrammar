"""Tests for Token() edge support in grammar parsing, matching, and bitmask generation."""

import pytest
import torch

import xgrammar as xgr
from xgrammar.testing import (
    _ebnf_to_grammar_no_normalization,
    _get_masked_tokens_from_bitmask,
    _get_matcher_from_grammar_and_tokenizer_info,
    _is_grammar_accept_string,
)

# --- Parser / Printer roundtrip tests ---


def test_parse_token_basic():
    before = "root ::= Token(1, 2, 3)\n"
    expected = "root ::= ((Token(1, 2, 3)))\n"
    grammar = _ebnf_to_grammar_no_normalization(before)
    assert str(grammar) == expected


def test_parse_token_single():
    before = "root ::= Token(42)\n"
    expected = "root ::= ((Token(42)))\n"
    grammar = _ebnf_to_grammar_no_normalization(before)
    assert str(grammar) == expected


def test_parse_token_sorted_deduped():
    before = "root ::= Token(3, 1, 2, 1, 3)\n"
    expected = "root ::= ((Token(1, 2, 3)))\n"
    grammar = _ebnf_to_grammar_no_normalization(before)
    assert str(grammar) == expected


def test_parse_token_in_sequence():
    before = 'root ::= Token(1, 2) "hello"\n'
    expected = 'root ::= ((Token(1, 2) "hello"))\n'
    grammar = _ebnf_to_grammar_no_normalization(before)
    assert str(grammar) == expected


def test_parse_token_in_alternation():
    before = 'root ::= Token(1) | "hello"\n'
    expected = 'root ::= ((Token(1)) | ("hello"))\n'
    grammar = _ebnf_to_grammar_no_normalization(before)
    assert str(grammar) == expected


# --- Matcher accept_token tests ---


STOP_TOKEN_ID = 1  # "</s>" in our test vocab


def _make_matcher(vocab, grammar_str):
    """Create a matcher with a custom vocab and grammar."""
    tokenizer_info = xgr.TokenizerInfo(vocab)
    grammar = xgr.Grammar.from_ebnf(grammar_str)
    return _get_matcher_from_grammar_and_tokenizer_info(grammar, tokenizer_info)


def test_accept_token_basic():
    """Token(2, 4) should accept token IDs 2 and 4 but reject others."""
    vocab = ["<s>", "</s>", "aa", "bb", "cc", "dd"]
    #         0      1       2     3     4     5
    matcher = _make_matcher(vocab, "root ::= Token(2, 4)\n")

    assert matcher.accept_token(2)
    assert matcher.accept_token(STOP_TOKEN_ID)
    assert matcher.is_terminated()


def test_accept_token_reject():
    """Tokens not in the Token() set should be rejected."""
    vocab = ["<s>", "</s>", "aa", "bb", "cc", "dd"]
    matcher = _make_matcher(vocab, "root ::= Token(2, 4)\n")

    assert not matcher.accept_token(3)
    assert not matcher.accept_token(5)
    assert matcher.accept_token(4)
    assert matcher.accept_token(STOP_TOKEN_ID)
    assert matcher.is_terminated()


def test_token_then_string():
    """Token followed by string literal: Token(2) "bb" ."""
    vocab = ["<s>", "</s>", "aa", "bb", "cc"]
    matcher = _make_matcher(vocab, 'root ::= Token(2) "bb"\n')

    assert matcher.accept_token(2)  # Token(2) = "aa"
    assert matcher.accept_token(3)  # "bb"
    assert matcher.accept_token(STOP_TOKEN_ID)
    assert matcher.is_terminated()


def test_token_or_string():
    """Alternation: Token(2) | "bb" ."""
    vocab = ["<s>", "</s>", "aa", "bb", "cc"]

    # Accept via token path
    matcher = _make_matcher(vocab, 'root ::= Token(2) | "bb"\n')
    assert matcher.accept_token(2)
    assert matcher.accept_token(STOP_TOKEN_ID)
    assert matcher.is_terminated()

    # Accept via string path
    matcher2 = _make_matcher(vocab, 'root ::= Token(2) | "bb"\n')
    assert matcher2.accept_token(3)  # "bb"
    assert matcher2.accept_token(STOP_TOKEN_ID)
    assert matcher2.is_terminated()


# --- Bitmask tests ---


def test_bitmask_token_only():
    """FillNextTokenBitmask should allow only tokens in Token() set (and stop token)."""
    vocab = ["<s>", "</s>", "aa", "bb", "cc", "dd"]
    tokenizer_info = xgr.TokenizerInfo(vocab)
    grammar = xgr.Grammar.from_ebnf("root ::= Token(2, 4)\n")
    matcher = _get_matcher_from_grammar_and_tokenizer_info(grammar, tokenizer_info)

    token_bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    matcher.fill_next_token_bitmask(token_bitmask)
    rejected = _get_masked_tokens_from_bitmask(token_bitmask, tokenizer_info.vocab_size)

    assert 2 not in rejected
    assert 4 not in rejected
    assert 0 in rejected  # <s>
    assert 3 in rejected  # "bb"
    assert 5 in rejected  # "dd"


def test_bitmask_token_and_string():
    """Bitmask for Token(2) | "bb" should allow token 2 and token whose text is "bb"."""
    vocab = ["<s>", "</s>", "aa", "bb", "cc"]
    tokenizer_info = xgr.TokenizerInfo(vocab)
    grammar = xgr.Grammar.from_ebnf('root ::= Token(2) | "bb"\n')
    matcher = _get_matcher_from_grammar_and_tokenizer_info(grammar, tokenizer_info)

    token_bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    matcher.fill_next_token_bitmask(token_bitmask)
    rejected = _get_masked_tokens_from_bitmask(token_bitmask, tokenizer_info.vocab_size)

    assert 2 not in rejected  # Token(2) via kTokenSet
    assert 3 not in rejected  # "bb" via char path


def test_bitmask_after_token():
    """After accepting a Token, the bitmask should reflect the next expected tokens."""
    vocab = ["<s>", "</s>", "aa", "bb", "cc"]
    tokenizer_info = xgr.TokenizerInfo(vocab)
    grammar = xgr.Grammar.from_ebnf('root ::= Token(2) "bb"\n')
    matcher = _get_matcher_from_grammar_and_tokenizer_info(grammar, tokenizer_info)

    # First: only token 2 should be allowed
    token_bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    matcher.fill_next_token_bitmask(token_bitmask)
    rejected = _get_masked_tokens_from_bitmask(token_bitmask, tokenizer_info.vocab_size)
    assert 2 not in rejected

    # Accept token 2
    assert matcher.accept_token(2)

    # Second: "bb" tokens should be allowed
    matcher.fill_next_token_bitmask(token_bitmask)
    rejected2 = _get_masked_tokens_from_bitmask(token_bitmask, tokenizer_info.vocab_size)
    assert 3 not in rejected2  # "bb" token


def test_token_multiple_choices():
    """Token set with multiple IDs in alternation with other rules."""
    vocab = ["<s>", "</s>", "x", "y", "z", "w"]
    tokenizer_info = xgr.TokenizerInfo(vocab)
    grammar = xgr.Grammar.from_ebnf('root ::= Token(2, 3, 4) | "w"\n')
    matcher = _get_matcher_from_grammar_and_tokenizer_info(grammar, tokenizer_info)

    token_bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    matcher.fill_next_token_bitmask(token_bitmask)
    rejected = _get_masked_tokens_from_bitmask(token_bitmask, tokenizer_info.vocab_size)

    assert 2 not in rejected  # "x" via kTokenSet
    assert 3 not in rejected  # "y" via kTokenSet
    assert 4 not in rejected  # "z" via kTokenSet
    assert 5 not in rejected  # "w" via char path
    assert 0 in rejected  # "<s>" rejected
