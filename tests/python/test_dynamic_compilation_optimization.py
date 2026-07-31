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


def _assert_dynamic_masks_equal_eager(grammar: str, vocabulary, input_string: str) -> None:
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_grammar(grammar)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar(grammar)

    eager_trace = _mask_trace(eager, input_string)
    dynamic_trace = _mask_trace(dynamic, input_string)
    assert len(dynamic_trace) == len(eager_trace)
    for (eager_apply, eager_mask), (dynamic_apply, dynamic_mask) in zip(eager_trace, dynamic_trace):
        assert dynamic_apply == eager_apply
        torch.testing.assert_close(dynamic_mask, eager_mask, rtol=0, atol=0)


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
