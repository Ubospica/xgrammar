import pytest
import torch

import xgrammar as xgr

JIT_MODE_VOCABULARY = [chr(value) for value in range(32, 127)] + ["Ada", "hello", "42", "<call>"]


def _mask_trace(compiled_grammar, input_string):
    matcher = xgr.GrammarMatcher(compiled_grammar, terminate_without_stop_token=True)
    bitmask = xgr.allocate_token_bitmask(1, compiled_grammar.tokenizer_info.vocab_size)
    trace = []
    for character in input_string:
        xgr.reset_token_bitmask(bitmask)
        trace.append((matcher.fill_next_token_bitmask(bitmask), bitmask.clone()))
        assert matcher.accept_string(character)
    assert matcher.is_terminated()
    return trace


def _assert_traces_equal(expected_trace, actual_trace):
    assert len(actual_trace) == len(expected_trace)
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(
        expected_trace, actual_trace
    ):
        assert actual_apply == expected_apply
        assert torch.equal(actual_mask, expected_mask)


def _compile_ebnf(compiler):
    return compiler.compile_grammar(
        """
root ::= greeting punctuation
greeting ::= "hello" | "hi"
punctuation ::= "!" | "?"
"""
    )


def _compile_json_schema(compiler):
    return compiler.compile_json_schema(
        {
            "type": "object",
            "properties": {
                "name": {"type": "string"},
                "scores": {"type": "array", "items": {"type": "integer"}},
            },
            "required": ["name", "scores"],
            "additionalProperties": False,
        },
        any_whitespace=True,
    )


def _compile_regex(compiler):
    return compiler.compile_regex(r"[A-Z][a-z]{2}[0-9]+")


def _compile_builtin_json(compiler):
    return compiler.compile_builtin_json_grammar()


def _compile_structural_tag(compiler):
    return compiler.compile_structural_tag(
        {
            "type": "structural_tag",
            "format": {
                "type": "dispatch",
                "rules": [["<call>", {"type": "const_string", "value": "X"}]],
                "loop": False,
                "excludes": [],
            },
        }
    )


@pytest.mark.parametrize(
    "compile_grammar,input_string",
    [
        (_compile_ebnf, "hello!"),
        (_compile_json_schema, '{"name":"Ada","scores":[1,2]}'),
        (_compile_regex, "Ada42"),
        (_compile_builtin_json, '{"name":"Ada"}'),
        (_compile_structural_tag, "prefix<call>X"),
    ],
    ids=["ebnf", "json-schema", "regex", "builtin-json", "structural-tag"],
)
def test_jit_mode_matches_eager_masks(compile_grammar, input_string):
    tokenizer_info = xgr.TokenizerInfo(JIT_MODE_VOCABULARY, stop_token_ids=[])
    eager_grammar = compile_grammar(xgr.GrammarCompiler(tokenizer_info, max_threads=1))
    jit_grammar = compile_grammar(xgr.GrammarCompiler(tokenizer_info, max_threads=1, jit_mode=True))

    _assert_traces_equal(
        _mask_trace(eager_grammar, input_string), _mask_trace(jit_grammar, input_string)
    )


def test_jit_mode_populates_and_reuses_masks():
    tokenizer_info = xgr.TokenizerInfo(JIT_MODE_VOCABULARY, stop_token_ids=[])
    jit_grammar = _compile_builtin_json(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, cache_enabled=False, jit_mode=True)
    )
    eager_grammar = _compile_builtin_json(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, cache_enabled=False)
    )

    initial_size = jit_grammar.memory_size_bytes
    assert initial_size < eager_grammar.memory_size_bytes
    _mask_trace(jit_grammar, '{"name":"Ada"}')
    populated_size = jit_grammar.memory_size_bytes
    assert populated_size > initial_size
    _mask_trace(jit_grammar, '{"name":"Ada"}')
    assert jit_grammar.memory_size_bytes == populated_size


def test_jit_mode_serialization_materializes_masks():
    tokenizer_info = xgr.TokenizerInfo(JIT_MODE_VOCABULARY, stop_token_ids=[])
    jit_grammar = _compile_ebnf(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, cache_enabled=False, jit_mode=True)
    )
    initial_size = jit_grammar.memory_size_bytes

    serialized = jit_grammar.serialize_json()
    assert jit_grammar.memory_size_bytes > initial_size
    restored_grammar = xgr.CompiledGrammar.deserialize_json(serialized, tokenizer_info)

    for input_string in ["hello!", "hi?"]:
        _assert_traces_equal(
            _mask_trace(jit_grammar, input_string), _mask_trace(restored_grammar, input_string)
        )


def test_jit_mode_respects_limited_compiler_cache():
    tokenizer_info = xgr.TokenizerInfo(JIT_MODE_VOCABULARY, stop_token_ids=[])
    cache_limit = 64 * 1024
    compiler = xgr.GrammarCompiler(
        tokenizer_info,
        max_threads=1,
        cache_enabled=True,
        cache_limit_bytes=cache_limit,
        jit_mode=True,
    )
    jit_grammar = _compile_builtin_json(compiler)

    assert compiler.get_cache_size_bytes() == 0
    _mask_trace(jit_grammar, '{"name":"Ada"}')
    assert 0 <= compiler.get_cache_size_bytes() <= cache_limit

    second_grammar = _compile_builtin_json(compiler)
    assert second_grammar.memory_size_bytes < jit_grammar.memory_size_bytes


def test_jit_mode_empty_vocabulary_round_trip():
    tokenizer_info = xgr.TokenizerInfo([], stop_token_ids=[])
    jit_grammar = xgr.GrammarCompiler(
        tokenizer_info, cache_enabled=False, jit_mode=True
    ).compile_grammar('root ::= "a"')
    matcher = xgr.GrammarMatcher(jit_grammar, terminate_without_stop_token=True)
    assert matcher.accept_string("a")
    assert matcher.is_terminated()

    restored_grammar = xgr.CompiledGrammar.deserialize_json(
        jit_grammar.serialize_json(), tokenizer_info
    )
    restored_matcher = xgr.GrammarMatcher(restored_grammar, terminate_without_stop_token=True)
    assert restored_matcher.accept_string("a")
    assert restored_matcher.is_terminated()


def _next_mask(matcher, tokenizer_info):
    bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    return matcher.fill_next_token_bitmask(bitmask), bitmask


def _assert_next_masks_equal(eager_matcher, jit_matcher, tokenizer_info):
    eager_apply, eager_mask = _next_mask(eager_matcher, tokenizer_info)
    jit_apply, jit_mask = _next_mask(jit_matcher, tokenizer_info)
    assert eager_apply == jit_apply
    assert torch.equal(eager_mask, jit_mask)


def test_jit_mode_supports_fork_rollback_and_reset():
    tokenizer_info = xgr.TokenizerInfo(["a", "b", "c", "ab", "ac"], stop_token_ids=[])
    eager_grammar = xgr.GrammarCompiler(tokenizer_info, cache_enabled=False).compile_grammar(
        'root ::= "a" ("b" | "c")'
    )
    jit_grammar = xgr.GrammarCompiler(
        tokenizer_info, cache_enabled=False, jit_mode=True
    ).compile_grammar('root ::= "a" ("b" | "c")')
    eager_matcher = xgr.GrammarMatcher(eager_grammar, terminate_without_stop_token=True)
    jit_matcher = xgr.GrammarMatcher(jit_grammar, terminate_without_stop_token=True)

    _assert_next_masks_equal(eager_matcher, jit_matcher, tokenizer_info)
    assert eager_matcher.accept_token(0)
    assert jit_matcher.accept_token(0)
    eager_fork = eager_matcher.fork()
    jit_fork = jit_matcher.fork()
    _assert_next_masks_equal(eager_fork, jit_fork, tokenizer_info)

    assert eager_matcher.accept_token(1)
    assert jit_matcher.accept_token(1)
    assert eager_matcher.is_terminated()
    assert jit_matcher.is_terminated()
    eager_matcher.rollback(1)
    jit_matcher.rollback(1)
    _assert_next_masks_equal(eager_matcher, jit_matcher, tokenizer_info)

    eager_matcher.reset()
    jit_matcher.reset()
    _assert_next_masks_equal(eager_matcher, jit_matcher, tokenizer_info)


def test_jit_mode_matches_eager_lazy_rule():
    tokenizer_info = xgr.TokenizerInfo(["x", "!", "z", "x!"], stop_token_ids=[])
    grammar = xgr.Grammar.from_lark(
        """
start: head "z"
head[max_tokens=2, lazy, capture, temperature=0.7]: TEXT "!"
TEXT: /(\\n|.)*/
""",
        tokenizer_info=tokenizer_info,
    )
    eager_grammar = xgr.GrammarCompiler(tokenizer_info, cache_enabled=False).compile_grammar(
        grammar
    )
    jit_grammar = xgr.GrammarCompiler(
        tokenizer_info, cache_enabled=False, jit_mode=True
    ).compile_grammar(grammar)
    assert xgr.GrammarMatcher(eager_grammar).temperature == pytest.approx(0.7)
    assert xgr.GrammarMatcher(jit_grammar).temperature == pytest.approx(0.7)

    _assert_traces_equal(_mask_trace(eager_grammar, "x!z"), _mask_trace(jit_grammar, "x!z"))


def _token_trace(compiled_grammar, tokenizer_info, token_ids):
    matcher = xgr.GrammarMatcher(compiled_grammar, terminate_without_stop_token=True)
    trace = []
    for token_id in token_ids:
        need_apply, bitmask = _next_mask(matcher, tokenizer_info)
        trace.append((need_apply, bitmask.clone()))
        word = int(bitmask[0, token_id // 32].item())
        if not word & (1 << (token_id % 32)):
            return trace, False
        if not matcher.accept_token(token_id):
            return trace, False
    return trace, matcher.is_terminated()


@pytest.mark.parametrize(
    "token_ids,expected", [([1, 0, 2], True), ([0, 0, 0, 2], False), ([1, 1, 2], False)]
)
def test_jit_mode_matches_eager_with_token_and_character_budgets(token_ids, expected):
    tokenizer_info = xgr.TokenizerInfo(["a", "ab", ">"], stop_token_ids=[])
    grammar = xgr.Grammar.from_lark(
        """
start: reasoning ">"
reasoning[max_tokens=2, max_chars=3]: TEXT
TEXT: /(\\n|.)*/
""",
        tokenizer_info=tokenizer_info,
    )
    eager_grammar = xgr.GrammarCompiler(tokenizer_info, cache_enabled=False).compile_grammar(
        grammar
    )
    jit_grammar = xgr.GrammarCompiler(
        tokenizer_info, cache_enabled=False, jit_mode=True
    ).compile_grammar(grammar)

    expected_trace, eager_result = _token_trace(eager_grammar, tokenizer_info, token_ids)
    actual_trace, jit_result = _token_trace(jit_grammar, tokenizer_info, token_ids)
    _assert_traces_equal(expected_trace, actual_trace)
    assert eager_result == expected
    assert jit_result == expected


def test_jit_mode_matches_eager_suffix_stop():
    tokenizer_info = xgr.TokenizerInfo(["aa!", "a>!", "aa>!"], stop_token_ids=[])
    grammar = xgr.Grammar.from_lark(
        """
start: reasoning "!"
reasoning[max_chars=2, suffix=">"]: TEXT
TEXT: /(\\n|.)*/
""",
        tokenizer_info=tokenizer_info,
    )
    eager_grammar = xgr.GrammarCompiler(tokenizer_info, cache_enabled=False).compile_grammar(
        grammar
    )
    jit_grammar = xgr.GrammarCompiler(
        tokenizer_info, cache_enabled=False, jit_mode=True
    ).compile_grammar(grammar)

    for token_id, expected in [(0, True), (1, True), (2, False)]:
        expected_trace, eager_result = _token_trace(eager_grammar, tokenizer_info, [token_id])
        actual_trace, jit_result = _token_trace(jit_grammar, tokenizer_info, [token_id])
        _assert_traces_equal(expected_trace, actual_trace)
        assert eager_result == expected
        assert jit_result == expected
