from concurrent.futures import ThreadPoolExecutor

import pytest
import torch

import xgrammar as xgr
from xgrammar.testing import bitmask_to_bool_mask


def _mask_trace(compiled_grammar: xgr.CompiledGrammar, input_string: str):
    matcher = xgr.GrammarMatcher(compiled_grammar, terminate_without_stop_token=True)
    bitmask = xgr.allocate_token_bitmask(1, compiled_grammar.tokenizer_info.vocab_size)
    trace = []
    for character in input_string:
        xgr.reset_token_bitmask(bitmask)
        need_apply = matcher.fill_next_token_bitmask(bitmask)
        trace.append((need_apply, bitmask.clone()))
        assert matcher.accept_string(character)
    assert matcher.is_terminated()
    return trace


def _assert_mask_traces_equal(eager, dynamic, input_string: str) -> None:
    eager_trace = _mask_trace(eager, input_string)
    dynamic_trace = _mask_trace(dynamic, input_string)
    assert len(dynamic_trace) == len(eager_trace)
    for (eager_apply, eager_mask), (dynamic_apply, dynamic_mask) in zip(eager_trace, dynamic_trace):
        assert dynamic_apply == eager_apply
        vocab_size = eager.tokenizer_info.vocab_size
        eager_tokens = bitmask_to_bool_mask(eager_mask, vocab_size)
        dynamic_tokens = bitmask_to_bool_mask(dynamic_mask, vocab_size)
        torch.testing.assert_close(dynamic_tokens, eager_tokens, rtol=0, atol=0)


def _assert_dynamic_masks_equal_eager(grammar: str, vocabulary, input_string: str) -> None:
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_grammar(grammar)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar(grammar)
    _assert_mask_traces_equal(eager, dynamic, input_string)


@pytest.mark.parametrize(
    "repeat_range,value",
    [
        ("{0}", ""),
        ("{1}", "a"),
        ("{0,1}", ""),
        ("{1,3}", "ab"),
        ("{63,65}", "a" * 64),
        ("{127,129}", "a" * 128),
        ("{2,}", "abc"),
    ],
)
def test_preserved_repetition_ranges_match_eager_masks(repeat_range: str, value: str):
    vocabulary = [">", "<", "a", "aa", "ab", "abc", "b", "ba", "c", b"\xc3", b"\xff"]
    grammar = f'root ::= ">" [a-z]{repeat_range} "<"'
    _assert_dynamic_masks_equal_eager(grammar, vocabulary, ">" + value + "<")


@pytest.mark.parametrize("prefix_length", [254, 255, 256, 257])
def test_large_preserved_repetition_range_masks_match_token_acceptance(prefix_length: int):
    vocabulary = [">", "<", "a", "aa", "ab", "abc", "b", "ba", "c", b"\xc3", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar('root ::= ">" [a-z]{255,257} "<"')
    prefix = ">" + "a" * prefix_length

    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    assert matcher.accept_string(prefix)
    bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    matcher.fill_next_token_bitmask(bitmask)
    allowed_tokens = bitmask_to_bool_mask(bitmask, tokenizer_info.vocab_size)[0]

    # Precompiled masks can conservatively retain a token that crosses the upper bound. Compare
    # this optimized boundary path with the matcher's exact token acceptance instead.
    for token_id in range(tokenizer_info.vocab_size):
        oracle = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
        assert oracle.accept_string(prefix)
        assert bool(allowed_tokens[token_id]) == oracle.accept_token(token_id)


BUCKET_TEST_VOCABULARY = [
    "",
    ">",
    "<",
    "\t",
    "\n",
    " ",
    "!",
    "a",
    "aa",
    "ab",
    "abc",
    "az",
    "b",
    "ba",
    "q",
    "Z",
    "ZZ",
    "0",
    "00",
    "é",
    "ê",
    "é0",
    "é00",
    b"\x00",
    b"\x7f",
    b"\x80",
    b"\xc3",
    b"\xff",
]


@pytest.mark.parametrize(
    "grammar,value",
    [
        ('root ::= ">" ("a" | "ab" | "abc" | "b") "<"', ">abc<"),
        ('root ::= ">" ("é" | "ê") "<"', ">ê<"),
        ("root ::= [a-z]{1,4}", "az"),
        ("root ::= [^a]{2}", "é!"),
        ('root ::= ("\\t" | "\\n" | " ") [A-Z]+', " Z"),
        ('root ::= ("a" | "é") ("0" | "00")', "é00"),
    ],
)
def test_first_byte_vocab_buckets_preserve_mask_results(grammar: str, value: str):
    _assert_dynamic_masks_equal_eager(grammar, BUCKET_TEST_VOCABULARY, value)


def test_first_byte_vocab_buckets_cover_every_byte_value():
    vocabulary = [bytes([value]) for value in range(256)]
    vocabulary += [bytes([value]) + b"x" for value in range(256)]
    _assert_dynamic_masks_equal_eager("root ::= [^a]", vocabulary, "z")


def test_first_byte_vocab_buckets_reused_across_grammars_and_cache_clear():
    tokenizer_info = xgr.TokenizerInfo(BUCKET_TEST_VOCABULARY, stop_token_ids=[])
    eager_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    )
    dynamic_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    )
    cases = [
        ('root ::= "a" | "ab" | "abc"', "abc"),
        ("root ::= [a-z]+", "az"),
        ('root ::= "é" | "ê"', "é"),
    ]

    for grammar, value in cases:
        _assert_mask_traces_equal(
            eager_compiler.compile_grammar(grammar),
            dynamic_compiler.compile_grammar(grammar),
            value,
        )

    eager_compiler.clear_cache()
    dynamic_compiler.clear_cache()
    for grammar, value in reversed(cases):
        _assert_mask_traces_equal(
            eager_compiler.compile_grammar(grammar),
            dynamic_compiler.compile_grammar(grammar),
            value,
        )


def test_first_byte_vocab_buckets_are_safe_for_concurrent_compilation():
    tokenizer_info = xgr.TokenizerInfo(BUCKET_TEST_VOCABULARY, stop_token_ids=[])
    dynamic_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    )
    cases = [
        ('root ::= "a" | "ab" | "abc"', "abc"),
        ("root ::= [a-z]{1,4}", "az"),
        ('root ::= "é" | "ê"', "ê"),
        ("root ::= [^a]{2}", "é!"),
    ]

    with ThreadPoolExecutor(max_workers=8) as executor:
        dynamic_grammars = list(
            executor.map(lambda case: dynamic_compiler.compile_grammar(case[0]), cases * 8)
        )

    eager_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    )
    for (grammar, value), dynamic in zip(cases * 8, dynamic_grammars):
        _assert_mask_traces_equal(eager_compiler.compile_grammar(grammar), dynamic, value)


def test_shared_parser_feature_metadata_preserves_matcher_behavior():
    tokenizer_info = xgr.TokenizerInfo(["ab ", "cd", " ", "</t>", "1", "<t>", "x"])
    token_budget_grammar = xgr.Grammar.from_lark(
        'start: r "<t>"\nr[max_tokens=3, capture]: TEXT\nTEXT: /(\\n|.)*/',
        tokenizer_info=tokenizer_info,
    )
    token_budget_compiled = xgr.GrammarCompiler(tokenizer_info).compile_grammar(
        token_budget_grammar
    )
    for _ in range(4):
        matcher = xgr.GrammarMatcher(token_budget_compiled, terminate_without_stop_token=True)
        assert all(matcher.accept_token(token_id) for token_id in [0, 1, 2])
        bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
        assert matcher.fill_next_token_bitmask(bitmask)
        assert bitmask_to_bool_mask(bitmask, tokenizer_info.vocab_size).nonzero().tolist() == [
            [0, 5]
        ]
        assert matcher.accept_token(5)
        assert matcher.is_terminated()
        assert matcher.get_captures() == [("r", b"ab cd ")]

    character_budget_grammar = xgr.Grammar.from_lark(
        'start[capture="outer"]: r "z"\nr[max_chars=2, capture="inner", suffix="!"]: /[a-z]*/'
    )
    character_budget_compiled = xgr.GrammarCompiler(xgr.TokenizerInfo([])).compile_grammar(
        character_budget_grammar
    )
    for _ in range(4):
        for value, expected_captures in [
            ("a!z", [("inner", b"a"), ("outer", b"a!z")]),
            ("abz", [("inner", b"ab"), ("outer", b"abz")]),
        ]:
            matcher = xgr.GrammarMatcher(
                character_budget_compiled, terminate_without_stop_token=True
            )
            assert matcher.accept_string(value) and matcher.is_terminated()
            assert matcher.get_captures() == expected_captures
