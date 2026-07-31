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


def _assert_dynamic_masks_equal_eager(grammar: str, vocabulary, input_string: str) -> None:
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_grammar(grammar)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar(grammar)

    _assert_compiled_masks_equal(eager, dynamic, input_string)


def _assert_compiled_masks_equal(
    eager: xgr.CompiledGrammar, dynamic: xgr.CompiledGrammar, input_string: str
) -> None:
    tokenizer_info = eager.tokenizer_info

    eager_trace = _mask_trace(eager, input_string)
    dynamic_trace = _mask_trace(dynamic, input_string)
    assert len(dynamic_trace) == len(eager_trace)
    for (eager_apply, eager_mask), (dynamic_apply, dynamic_mask) in zip(eager_trace, dynamic_trace):
        assert dynamic_apply == eager_apply
        eager_tokens = bitmask_to_bool_mask(eager_mask, tokenizer_info.vocab_size)
        dynamic_tokens = bitmask_to_bool_mask(dynamic_mask, tokenizer_info.vocab_size)
        torch.testing.assert_close(dynamic_tokens, eager_tokens, rtol=0, atol=0)


@pytest.mark.parametrize(
    "repeat_range,value",
    [("{0,1}", ""), ("{1,3}", "ab"), ("{127,129}", "a" * 128), ("{2,}", "abc")],
)
def test_preserved_repetition_ranges_match_eager_masks(repeat_range: str, value: str):
    vocabulary = [">", "<", "a", "aa", "ab", "abc", "b", "ba", "c", b"\xff"]
    grammar = f'root ::= ">" [a-z]{repeat_range} "<"'
    _assert_dynamic_masks_equal_eager(grammar, vocabulary, ">" + value + "<")


@pytest.mark.parametrize(
    "grammar,value",
    [
        ('root ::= ">" ("a" | "ab" | "abc" | "b") "<"', ">abc<"),
        ('root ::= ">" ("é" | "ê") "<"', ">ê<"),
    ],
)
def test_first_byte_vocab_buckets_preserve_mask_results(grammar: str, value: str):
    vocabulary = [">", "<", "a", "aa", "ab", "abc", "b", "ba", "é", "ê", b"\xc3", b"\xff"]
    _assert_dynamic_masks_equal_eager(grammar, vocabulary, value)


def test_common_character_class_masks_are_reused_without_changing_results():
    vocabulary = [">", "<", "[", "]", "a", "ab", "abc", "z", "é", b"\xc3", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    eager_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    )
    dynamic_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    )
    for grammar, value in [('root ::= ">" [a-z] "<"', ">a<"), ('root ::= "[" [a-z] "]"', "[z]")]:
        _assert_compiled_masks_equal(
            eager_compiler.compile_grammar(grammar),
            dynamic_compiler.compile_grammar(grammar),
            value,
        )


def test_common_ascii_json_classification_preserves_masks():
    vocabulary = ['"', "safe", "safe ASCII", 'quote"', "slash\\", "\n", "é", b"\xc3", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    schema = {"type": "string", "minLength": 1}
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_json_schema(schema)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_json_schema(schema)
    _assert_compiled_masks_equal(eager, dynamic, '"safe ASCII"')


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


def test_reused_matcher_scratch_survives_fork_reset_failure_and_rollback():
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


def test_reused_matcher_scratch_is_isolated_across_matchers():
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
