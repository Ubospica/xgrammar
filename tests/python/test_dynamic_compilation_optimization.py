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


def _next_token_mask(matcher: xgr.GrammarMatcher, vocab_size: int) -> torch.Tensor:
    bitmask = xgr.allocate_token_bitmask(1, vocab_size)
    matcher.fill_next_token_bitmask(bitmask)
    return bitmask_to_bool_mask(bitmask, vocab_size)


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


def _assert_mask_matches_token_acceptance(
    compiled_grammar: xgr.CompiledGrammar, prefix: str, skip_token_ids=()
) -> None:
    matcher = xgr.GrammarMatcher(compiled_grammar, terminate_without_stop_token=True)
    assert matcher.accept_string(prefix)
    bitmask = xgr.allocate_token_bitmask(1, compiled_grammar.tokenizer_info.vocab_size)
    xgr.reset_token_bitmask(bitmask)
    need_apply = matcher.fill_next_token_bitmask(bitmask)
    if need_apply:
        allowed_tokens = bitmask_to_bool_mask(bitmask, compiled_grammar.tokenizer_info.vocab_size)[
            0
        ]

    for token_id in range(compiled_grammar.tokenizer_info.vocab_size):
        if token_id in skip_token_ids:
            continue
        oracle = xgr.GrammarMatcher(compiled_grammar, terminate_without_stop_token=True)
        assert oracle.accept_string(prefix)
        expected = oracle.accept_token(token_id)
        actual = bool(allowed_tokens[token_id]) if need_apply else True
        assert actual == expected, (prefix, token_id)


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


def test_single_character_class_masks_are_reused_safely():
    vocabulary = [">", "<", "[", "]", "a", "ab", "abc", "z", "é", b"\xc3", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    eager_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    )
    dynamic_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    )
    cases = [('root ::= ">" [a-z] "<"', ">a<"), ('root ::= "[" [a-z] "]"', "[z]")]

    def compile_dynamic(case):
        return dynamic_compiler.compile_grammar(case[0])

    with ThreadPoolExecutor(max_workers=8) as executor:
        compiled = list(executor.map(compile_dynamic, cases * 8))
    for (grammar, value), dynamic in zip(cases * 8, compiled):
        _assert_mask_traces_equal(eager_compiler.compile_grammar(grammar), dynamic, value)

    dynamic_compiler.clear_cache()
    for grammar, value in reversed(cases):
        _assert_mask_traces_equal(
            eager_compiler.compile_grammar(grammar),
            dynamic_compiler.compile_grammar(grammar),
            value,
        )


ASCII_JSON_VOCABULARY = [
    "",
    '"',
    "\\",
    "safe",
    "safe ASCII",
    "AZ09 !#[]{}",
    'quote"',
    "slash\\",
    "\n",
    b"\x00",
    b"\x1f",
    b"\x7f",
    "é",
    b"\xc3",
    b"\xff",
]


@pytest.mark.parametrize(
    "schema,value,prefixes",
    [
        ({"type": "string", "minLength": 1}, '"safe ASCII"', ['"', '"safe']),
        ({"type": "string", "minLength": 2, "maxLength": 4}, '"safe"', ['"', '"sa', '"safe']),
        ({"type": "string", "pattern": "^[A-Z0-9 ]+$"}, '"AZ09"', ['"', '"AZ']),
        ({"enum": ["safe ASCII", 'quote"', "slash\\", "é"]}, '"safe ASCII"', ['"', '"safe']),
    ],
)
def test_json_safe_ascii_token_classification_preserves_masks(schema, value, prefixes):
    tokenizer_info = xgr.TokenizerInfo(ASCII_JSON_VOCABULARY, stop_token_ids=[])
    eager_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    )
    dynamic_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    )

    for clear_cache in [False, True]:
        if clear_cache:
            eager_compiler.clear_cache()
            dynamic_compiler.clear_cache()
        eager = eager_compiler.compile_json_schema(schema)
        dynamic = dynamic_compiler.compile_json_schema(schema)
        _assert_mask_traces_equal(eager, dynamic, value)
        for prefix in prefixes:
            # Token 0 is empty and treated as a special token. The full eager/dynamic mask
            # comparison above covers it without asking accept_token to emit a warning.
            _assert_mask_matches_token_acceptance(dynamic, prefix, skip_token_ids=(0,))


def test_reusable_parser_work_queue_survives_reset_fork_failure_and_rollback():
    vocabulary = ["a", "b", "c", "d", "ab", "bc", "abc", "cd", "x", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar('root ::= [a-c]{0,8} "d"')
    token_ids = {token: vocabulary.index(token) for token in ["a", "b", "c", "d", "x"]}

    def fresh_mask(prefix):
        fresh = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
        assert all(fresh.accept_token(token_id) for token_id in prefix)
        return _next_token_mask(fresh, tokenizer_info.vocab_size)

    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    for _ in range(32):
        matcher.reset()
        prefix = []
        for token in ["a", "b"]:
            expected = fresh_mask(prefix)
            torch.testing.assert_close(
                _next_token_mask(matcher, tokenizer_info.vocab_size), expected, rtol=0, atol=0
            )
            torch.testing.assert_close(
                _next_token_mask(matcher, tokenizer_info.vocab_size), expected, rtol=0, atol=0
            )
            assert matcher.accept_token(token_ids[token])
            prefix.append(token_ids[token])

        forked = matcher.fork()
        torch.testing.assert_close(
            _next_token_mask(forked, tokenizer_info.vocab_size), fresh_mask(prefix), rtol=0, atol=0
        )
        assert forked.accept_token(token_ids["c"])
        assert forked.accept_token(token_ids["d"])
        assert forked.is_terminated()

        before_failure = _next_token_mask(matcher, tokenizer_info.vocab_size)
        assert not matcher.accept_token(token_ids["x"])
        torch.testing.assert_close(
            _next_token_mask(matcher, tokenizer_info.vocab_size), before_failure, rtol=0, atol=0
        )

        matcher.rollback(1)
        prefix.pop()
        torch.testing.assert_close(
            _next_token_mask(matcher, tokenizer_info.vocab_size), fresh_mask(prefix), rtol=0, atol=0
        )
        assert matcher.accept_token(token_ids["c"])
        assert matcher.accept_token(token_ids["d"])
        assert matcher.is_terminated()


def test_reused_mask_state_scratch_is_isolated_across_matchers():
    vocabulary = ["a", "b", "c", "d", "ab", "bc", "abc", "cd", "x", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar('root ::= [a-c]{0,8} "d"')
    token_ids = [vocabulary.index(token) for token in ["a", "b", "c", "d"]]

    def run_trace(_):
        matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
        trace = []
        for token_id in token_ids:
            trace.append(_next_token_mask(matcher, tokenizer_info.vocab_size))
            assert matcher.accept_token(token_id)
        assert matcher.is_terminated()
        return trace

    with ThreadPoolExecutor(max_workers=8) as pool:
        traces = list(pool.map(run_trace, range(64)))
    for trace in traces[1:]:
        for expected, actual in zip(traces[0], trace):
            torch.testing.assert_close(actual, expected, rtol=0, atol=0)
