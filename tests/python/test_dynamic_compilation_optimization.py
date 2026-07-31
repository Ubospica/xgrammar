from concurrent.futures import ThreadPoolExecutor

import pytest
import torch

import xgrammar as xgr


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
        torch.testing.assert_close(dynamic_mask, eager_mask, rtol=0, atol=0)


def _assert_dynamic_masks_equal_eager(grammar: str, vocabulary, input_string: str) -> None:
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_grammar(grammar)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar(grammar)
    _assert_mask_traces_equal(eager, dynamic, input_string)


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
