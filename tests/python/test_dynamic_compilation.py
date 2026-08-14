import threading
from concurrent.futures import ThreadPoolExecutor

import pytest
import torch

import xgrammar as xgr
from xgrammar.base import _core
from xgrammar.testing import bitmask_to_bool_mask

VOCABULARY = [chr(value) for value in range(32, 127)] + ["Ada", "hello", "42", "<call>"]


def _mask_trace(compiled_grammar: xgr.CompiledGrammar, input_string: str):
    matcher = xgr.GrammarMatcher(compiled_grammar, terminate_without_stop_token=True)
    bitmask = xgr.allocate_token_bitmask(1, compiled_grammar.tokenizer_info.vocab_size)
    trace = []
    for char in input_string:
        xgr.reset_token_bitmask(bitmask)
        trace.append((matcher.fill_next_token_bitmask(bitmask), bitmask.clone()))
        assert matcher.accept_string(char)
    assert matcher.is_terminated()
    return trace


def _compile_ebnf(compiler: xgr.GrammarCompiler) -> xgr.CompiledGrammar:
    return compiler.compile_grammar(
        """
root ::= greeting punctuation
greeting ::= "hello" | "hi"
punctuation ::= "!" | "?"
"""
    )


def _compile_json_schema(compiler: xgr.GrammarCompiler) -> xgr.CompiledGrammar:
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


def _compile_regex(compiler: xgr.GrammarCompiler) -> xgr.CompiledGrammar:
    return compiler.compile_regex(r"[A-Z][a-z]{2}[0-9]+")


def _compile_builtin_json(compiler: xgr.GrammarCompiler) -> xgr.CompiledGrammar:
    return compiler.compile_builtin_json_grammar()


def _compile_structural_tag(compiler: xgr.GrammarCompiler) -> xgr.CompiledGrammar:
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


CASES = [
    (_compile_ebnf, "hello!"),
    (_compile_json_schema, '{"name":"Ada","scores":[1,2]}'),
    (_compile_regex, "Ada42"),
    (_compile_builtin_json, '{"name":"Ada"}'),
    (_compile_structural_tag, "prefix<call>X"),
]


def test_existing_native_constructor_remains_available():
    tokenizer_info = xgr.TokenizerInfo(["a"], stop_token_ids=[])
    compiler = _core.GrammarCompiler(tokenizer_info._handle, 1, True, -1)
    compiler.compile_builtin_json_grammar()


@pytest.mark.parametrize(
    "compile_grammar,input_string",
    CASES,
    ids=["ebnf", "json-schema", "regex", "builtin-json", "structural-tag"],
)
def test_dynamic_compilation_matches_eager_masks(compile_grammar, input_string):
    tokenizer_info = xgr.TokenizerInfo(VOCABULARY, stop_token_ids=[])
    eager = compile_grammar(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, enable_dynamic_compilation=False)
    )
    dynamic = compile_grammar(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, enable_dynamic_compilation=True)
    )

    expected = _mask_trace(eager, input_string)
    actual = _mask_trace(dynamic, input_string)
    assert len(actual) == len(expected)
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
        assert actual_apply == expected_apply
        torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


@pytest.mark.parametrize("repeat_range", ["{3,}", "{3,5}"])
def test_right_recursive_rule_inside_repetition_matches_eager_masks(repeat_range):
    tokenizer_info = xgr.TokenizerInfo(VOCABULARY, stop_token_ids=[])
    grammar = (
        'root ::= "[" item tail "]"\n'
        f"tail ::= unit{repeat_range}\n"
        'unit ::= ", " item\n'
        "item ::= [0-9]+"
    )
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_grammar(grammar)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar(grammar)

    expected = _mask_trace(eager, "[1, 2, 3, 4]")
    actual = _mask_trace(dynamic, "[1, 2, 3, 4]")
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
        assert actual_apply == expected_apply
        torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


@pytest.mark.parametrize("cache_enabled", [False, True], ids=["cache-off", "cache-on"])
def test_recursive_json_string_character_class_summary_matches_eager(cache_enabled):
    vocabulary = [chr(value) for value in range(32, 127)] + [
        "plainascii",
        "中文",
        '中文"',
        '中文"}',
        "\\n",
        b"\xe4",
        b"\xe4\xb8",
        b"\xff",
    ]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    schema = {
        "type": "object",
        "properties": {"value": {"type": "string"}},
        "required": ["value"],
        "additionalProperties": False,
    }
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, cache_enabled=cache_enabled, enable_dynamic_compilation=False
    ).compile_json_schema(schema, any_whitespace=False, strict_mode=True)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, cache_enabled=cache_enabled, enable_dynamic_compilation=True
    ).compile_json_schema(schema, any_whitespace=False, strict_mode=True)

    expected = _mask_trace(eager, '{"value": "中文\\nA"}')
    actual = _mask_trace(dynamic, '{"value": "中文\\nA"}')
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
        assert actual_apply == expected_apply
        torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


def test_dynamic_masks_are_cached():
    tokenizer_info = xgr.TokenizerInfo(VOCABULARY, stop_token_ids=[])
    dynamic = _compile_builtin_json(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, enable_dynamic_compilation=True)
    )
    eager = _compile_builtin_json(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, enable_dynamic_compilation=False)
    )

    initial_size = dynamic.memory_size_bytes
    assert initial_size < eager.memory_size_bytes
    _mask_trace(dynamic, '{"name":"Ada"}')
    populated_size = dynamic.memory_size_bytes
    assert populated_size > initial_size
    _mask_trace(dynamic, '{"name":"Ada"}')
    assert dynamic.memory_size_bytes == populated_size


def test_serialization_materializes_dynamic_masks():
    tokenizer_info = xgr.TokenizerInfo(VOCABULARY, stop_token_ids=[])
    dynamic = _compile_ebnf(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, enable_dynamic_compilation=True)
    )
    initial_size = dynamic.memory_size_bytes

    restored = xgr.CompiledGrammar.deserialize_json(dynamic.serialize_json(), tokenizer_info)
    assert dynamic.memory_size_bytes > initial_size
    for input_string in ["hello!", "hi?"]:
        expected = _mask_trace(dynamic, input_string)
        actual = _mask_trace(restored, input_string)
        for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
            assert actual_apply == expected_apply
            torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


def test_concurrent_dynamic_mask_generation():
    tokenizer_info = xgr.TokenizerInfo(VOCABULARY, stop_token_ids=[])
    dynamic = _compile_json_schema(
        xgr.GrammarCompiler(tokenizer_info, max_threads=1, enable_dynamic_compilation=True)
    )
    input_string = '{"name":"Ada","scores":[1,2]}'

    with ThreadPoolExecutor(max_workers=8) as executor:
        traces = list(executor.map(lambda _: _mask_trace(dynamic, input_string), range(32)))

    expected = traces[0]
    for actual in traces[1:]:
        for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
            assert actual_apply == expected_apply
            torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


def test_shared_character_class_repeat_masks_survive_compiler_cache_clear():
    vocabulary = [">", "<", "[", "]", "a", "b", "ab", "ab<", "ab]", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiler = xgr.GrammarCompiler(tokenizer_info, max_threads=1, enable_dynamic_compilation=True)
    first = compiler.compile_grammar('root ::= ">" value "<"\nvalue ::= [a-z]{2,4}')
    expected_first = _mask_trace(first, ">ab<")

    compiler.clear_cache()
    second = compiler.compile_grammar('root ::= "[" value "]"\nvalue ::= [a-z]{2,4}')
    expected_second = _mask_trace(second, "[ab]")

    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = [
            executor.submit(_mask_trace, grammar, value)
            for _ in range(8)
            for grammar, value in ((first, ">ab<"), (second, "[ab]"))
        ]
    for index, future in enumerate(futures):
        expected = expected_first if index % 2 == 0 else expected_second
        actual = future.result()
        for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
            assert actual_apply == expected_apply
            torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


def test_shared_repeat_masks_preserve_runtime_json_length_states_concurrently():
    vocabulary = ['"', "é", "ê", "a", "éa", "êa", "éaêa", "êaéa"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    schema = {"type": "string", "pattern": "^(?:[éê]a){1,3}$", "minLength": 4, "maxLength": 4}
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_json_schema(schema)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_json_schema(schema)
    inputs = ['"éaêa"', '"êaéa"']
    expected = {input_string: _mask_trace(eager, input_string) for input_string in inputs}

    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = [
            executor.submit(_mask_trace, dynamic, input_string)
            for _ in range(16)
            for input_string in inputs
        ]
    for index, future in enumerate(futures):
        input_string = inputs[index % len(inputs)]
        actual = future.result()
        for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(
            expected[input_string], actual
        ):
            assert actual_apply == expected_apply
            torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


def test_runtime_json_string_mask_cache_matches_fresh_replay_across_contexts():
    vocabulary = [
        '{"x": "',
        "a",
        "aa",
        r"\u0061",
        '", "y": "',
        "b",
        "bb",
        '"}',
        'a"}',
        'aa", "y": "',
        r"\"",
        "\\",
        b"\xc3",
        b"\xff",
    ]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_json_schema(
        {
            "type": "object",
            "properties": {
                "x": {"type": "string", "minLength": 2, "maxLength": 64},
                "y": {"type": "string", "minLength": 2, "maxLength": 64},
            },
            "required": ["x", "y"],
            "additionalProperties": False,
        },
        any_whitespace=False,
        strict_mode=True,
    )
    sequences = [
        [0, 1, 2, 3, 4, 5, 6, 7],
        [0, 2, 1, 2, 4, 6, 5, 6, 7],
        [0, *([2] * 31), 1, 4, 5, 6, 7],
    ]

    cached = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    cached_mask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    fresh_mask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    for sequence in sequences:
        cached.reset()
        prefix = []
        for token_id in sequence:
            xgr.reset_token_bitmask(cached_mask)
            cached.fill_next_token_bitmask(cached_mask)

            fresh = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
            assert all(fresh.accept_token(previous) for previous in prefix)
            xgr.reset_token_bitmask(fresh_mask)
            fresh.fill_next_token_bitmask(fresh_mask)
            torch.testing.assert_close(cached_mask, fresh_mask, rtol=0, atol=0)

            assert cached.accept_token(token_id)
            prefix.append(token_id)
        assert cached.is_terminated()

    cached.reset()
    rollback_prefix = [0, 2, 1, 2]
    for token_id in rollback_prefix:
        xgr.reset_token_bitmask(cached_mask)
        cached.fill_next_token_bitmask(cached_mask)
        assert cached.accept_token(token_id)
    cached.rollback(2)
    rollback_prefix = rollback_prefix[:-2]
    fresh = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    assert all(fresh.accept_token(token_id) for token_id in rollback_prefix)
    xgr.reset_token_bitmask(fresh_mask)
    fresh.fill_next_token_bitmask(fresh_mask)
    for restored in (cached, cached.fork()):
        xgr.reset_token_bitmask(cached_mask)
        restored.fill_next_token_bitmask(cached_mask)
        torch.testing.assert_close(cached_mask, fresh_mask, rtol=0, atol=0)


def test_runtime_json_string_mask_cache_replacement_matches_fresh_replay():
    property_names = [f"value_{index:02d}" for index in range(40)]
    first_property = f'{{"{property_names[0]}": "'
    property_transitions = [f'", "{name}": "' for name in property_names[1:]]
    vocabulary = [first_property, "a", r"\u0061", *property_transitions, '"}']
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    string_schema = {"type": "string", "minLength": 1, "maxLength": 64}
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_json_schema(
        {
            "type": "object",
            "properties": {name: string_schema for name in property_names},
            "required": property_names,
            "additionalProperties": False,
        },
        any_whitespace=False,
        strict_mode=True,
    )

    sequence = [0]
    for index in range(len(property_names)):
        sequence.extend([1, 2, 1])
        sequence.append(3 + index if index + 1 < len(property_names) else len(vocabulary) - 1)

    cached = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    cached_mask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    fresh_mask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    prefix = []
    for token_id in sequence:
        xgr.reset_token_bitmask(cached_mask)
        cached.fill_next_token_bitmask(cached_mask)

        fresh = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
        assert all(fresh.accept_token(previous) for previous in prefix)
        xgr.reset_token_bitmask(fresh_mask)
        fresh.fill_next_token_bitmask(fresh_mask)
        torch.testing.assert_close(cached_mask, fresh_mask, rtol=0, atol=0)

        assert cached.accept_token(token_id)
        prefix.append(token_id)
    assert cached.is_terminated()


def test_streaming_literal_pattern_masks_are_shared_concurrently():
    alternatives = ["bcd", "abce", "he", "she", "his", "hers"] + [
        f"region-{index:03d}" for index in range(96)
    ]
    vocabulary = [chr(value) for value in range(32, 127)] + [
        "abcd",
        "ushers",
        "prefix-region-095-suffix",
        r"\u0061bcd",
    ]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    schema = {"type": "string", "pattern": "|".join(alternatives), "minLength": 4, "maxLength": 32}
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_json_schema(schema)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_json_schema(schema)
    inputs = ['"abcd"', '"ushers"', '"prefix-region-095-suffix"', r'"\u0061bcd"']
    expected = {input_string: _mask_trace(eager, input_string) for input_string in inputs}

    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = [
            executor.submit(_mask_trace, dynamic, input_string)
            for _ in range(8)
            for input_string in inputs
        ]
    for index, future in enumerate(futures):
        input_string = inputs[index % len(inputs)]
        actual = future.result()
        for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(
            expected[input_string], actual
        ):
            assert actual_apply == expected_apply
            torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


def test_limited_compiler_cache_does_not_retain_growing_grammar():
    tokenizer_info = xgr.TokenizerInfo(VOCABULARY, stop_token_ids=[])
    cache_limit = 64 * 1024
    compiler = xgr.GrammarCompiler(
        tokenizer_info,
        max_threads=1,
        cache_enabled=True,
        cache_limit_bytes=cache_limit,
        enable_dynamic_compilation=True,
    )
    dynamic = _compile_builtin_json(compiler)
    assert compiler.get_cache_size_bytes() == 0

    barrier = threading.Barrier(9)

    def generate_masks():
        barrier.wait()
        return _mask_trace(dynamic, '{"name":"Ada"}')

    def read_cache_sizes():
        barrier.wait()
        return [compiler.get_cache_size_bytes() for _ in range(100)]

    with ThreadPoolExecutor(max_workers=9) as executor:
        mask_futures = [executor.submit(generate_masks) for _ in range(8)]
        size_future = executor.submit(read_cache_sizes)
        for future in mask_futures:
            future.result()
        observed_sizes = size_future.result()

    assert all(0 <= size <= cache_limit for size in observed_sizes)
    assert 0 <= compiler.get_cache_size_bytes() <= cache_limit


def test_rule_mask_sharing_does_not_cross_context_dependent_rules():
    tokenizer_info = xgr.TokenizerInfo(VOCABULARY, stop_token_ids=[])
    dynamic_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    )
    eager_compiler = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    )

    unbounded = dynamic_compiler.compile_grammar('root ::= "a" shared "z"\nshared ::= [x]+')
    _mask_trace(unbounded, "axxz")

    bounded_grammar = 'root ::= "a" shared "z"\nshared[max_tokens=1] ::= [x]+'
    dynamic_bounded = dynamic_compiler.compile_grammar(bounded_grammar)
    eager_bounded = eager_compiler.compile_grammar(bounded_grammar)
    expected = _mask_trace(eager_bounded, "axz")
    actual = _mask_trace(dynamic_bounded, "axz")
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
        assert actual_apply == expected_apply
        torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


@pytest.mark.parametrize(
    "repeat_range,value",
    [
        ("{0}", ""),
        ("{1}", "a"),
        ("{0,1}", ""),
        ("{1,3}", "ab"),
        ("{63,65}", "a" * 64),
        ("{127,129}", "a" * 128),
        ("{255,257}", "a" * 256),
        ("{2,}", "abc"),
    ],
)
def test_preserved_repetition_ranges_match_eager_masks(repeat_range: str, value: str):
    vocabulary = [">", "<", "a", "aa", "ab", "abc", "b", "ba", "c", b"\xc3", b"\xff"]
    grammar = f'root ::= ">" [a-z]{repeat_range} "<"'
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_grammar(grammar)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar(grammar)
    expected = _mask_trace(eager, ">" + value + "<")
    actual = _mask_trace(dynamic, ">" + value + "<")
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
        assert actual_apply == expected_apply
        expected_tokens = bitmask_to_bool_mask(expected_mask, tokenizer_info.vocab_size)
        actual_tokens = bitmask_to_bool_mask(actual_mask, tokenizer_info.vocab_size)
        torch.testing.assert_close(actual_tokens, expected_tokens, rtol=0, atol=0)


@pytest.mark.parametrize("cache_enabled", [False, True], ids=["cache-off", "cache-on"])
@pytest.mark.parametrize(
    "grammar,value,vocabulary",
    [
        (
            'root ::= unit{0,1} unit "x"\nunit ::= "a"',
            "aax",
            ["a", "x", "aa", "ax", "aaa", "aax", b"\xc3", b"\xff"],
        ),
        (
            'root ::= unit unit{0,1} "x"\nunit[capture="u"] ::= "a"',
            "aax",
            ["a", "x", "aa", "ax", "aaa", "aax", b"\xc3", b"\xff"],
        ),
        (
            'root ::= unit{2,3} unit "x"\nunit ::= "a"',
            "aaaax",
            ["a", "x", "aa", "ax", "aaa", "aax", "aaaa", "aaax", "aaaax", b"\xc3", b"\xff"],
        ),
        (
            'root ::= unit{0,1} unit "x"\nunit ::= "é"',
            "ééx",
            ["é", "x", "éé", "éx", "ééx", b"\xc3", b"\xa9", b"\xff"],
        ),
    ],
    ids=[
        "repeat-then-normal",
        "normal-then-repeat",
        "multi-repeat-then-normal",
        "unicode-repeat-boundary",
    ],
)
def test_preserved_repetition_ranges_match_multitokens_across_repeat_boundaries(
    cache_enabled, grammar, value, vocabulary
):
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, cache_enabled=cache_enabled, enable_dynamic_compilation=False
    ).compile_grammar(grammar)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, cache_enabled=cache_enabled, enable_dynamic_compilation=True
    ).compile_grammar(grammar)

    expected = _mask_trace(eager, value)
    actual = _mask_trace(dynamic, value)
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
        assert actual_apply == expected_apply
        torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)

    if value == "aax":
        for compiled_grammar in [eager, dynamic]:
            matcher = xgr.GrammarMatcher(compiled_grammar, terminate_without_stop_token=True)
            assert matcher.accept_token(vocabulary.index("aa"))
            assert not matcher.is_terminated()
            assert matcher.accept_token(vocabulary.index("x"))
            assert matcher.is_terminated()

            matcher = xgr.GrammarMatcher(compiled_grammar, terminate_without_stop_token=True)
            assert matcher.accept_token(vocabulary.index("aax"))
            assert matcher.is_terminated()


@pytest.mark.parametrize(
    "character_class,value", [("[a-z]", "a"), ("[^b]", "é"), ("[a-zа-я一-龥]", "中")]
)
def test_dynamic_single_character_class_masks_match_eager(character_class, value):
    vocabulary = [">", "<", "a", "ab", "a<", "b", "é", "中", b"\xe4", b"\xe4\xb8", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    grammar = f'root ::= ">" value "<"\nvalue ::= {character_class}'
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_grammar(grammar)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar(grammar)

    expected = _mask_trace(eager, ">" + value + "<")
    actual = _mask_trace(dynamic, ">" + value + "<")
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
        assert actual_apply == expected_apply
        torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


@pytest.mark.parametrize("repeat_range", ["+", "{1,255}"])
def test_json_pattern_repeat_with_escape_choices_matches_eager(repeat_range):
    vocabulary = ['"', "alpha_42", "alpha-42", r"\u0061", "!", b"\xff"]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    schema = {"type": "string", "pattern": rf"^[A-Za-z0-9_-]{repeat_range}$"}
    eager = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=False
    ).compile_json_schema(schema)
    dynamic = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_json_schema(schema)

    expected = _mask_trace(eager, '"alpha_42"')
    actual = _mask_trace(dynamic, '"alpha_42"')
    for (expected_apply, expected_mask), (actual_apply, actual_mask) in zip(expected, actual):
        assert actual_apply == expected_apply
        torch.testing.assert_close(actual_mask, expected_mask, rtol=0, atol=0)


def test_continuation_transition_cache_isolated_between_parser_states(capfd):
    left_suffixes = [
        chr(first) + chr(second)
        for first in range(ord("a"), ord("n"))
        for second in range(ord("a"), ord("n"))
    ]
    right_suffixes = [
        chr(first) + chr(second)
        for first in range(ord("n"), ord("{"))
        for second in range(ord("n"), ord("{"))
    ]
    vocabulary = (
        [suffix + "!" for suffix in left_suffixes]
        + [suffix + "?" for suffix in right_suffixes]
        + ["a", "n", "!", "?", "x", b"\xff"]
    )
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=True
    ).compile_grammar(
        'root ::= left "!" | right "?"\n' 'left ::= [a-m] left | ""\n' 'right ::= [n-z] right | ""'
    )

    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    assert matcher.fill_next_token_bitmask(bitmask, debug_print=True)
    assert capfd.readouterr().err.count("ContinuationTransitionCache(") == 2

    allowed_tokens = bitmask_to_bool_mask(bitmask, tokenizer_info.vocab_size)[0]
    for token_id in range(tokenizer_info.vocab_size):
        oracle = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
        assert bool(allowed_tokens[token_id]) == oracle.accept_token(token_id), token_id


def test_shared_parser_features_preserve_budget_and_capture_behavior():
    tokenizer_info = xgr.TokenizerInfo(["ab ", "cd", " ", "</t>", "1", "<t>", "x"])
    grammar = xgr.Grammar.from_lark(
        'start: r "<t>"\nr[max_tokens=3, capture]: TEXT\nTEXT: /(\\n|.)*/',
        tokenizer_info=tokenizer_info,
    )
    compiled = xgr.GrammarCompiler(tokenizer_info).compile_grammar(grammar)
    for _ in range(4):
        matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
        assert all(matcher.accept_token(token_id) for token_id in [0, 1, 2])
        bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
        assert matcher.fill_next_token_bitmask(bitmask)
        assert bitmask_to_bool_mask(bitmask, tokenizer_info.vocab_size).nonzero().tolist() == [
            [0, 5]
        ]
        assert matcher.accept_token(5)
        assert matcher.is_terminated()
        assert matcher.get_captures() == [("r", b"ab cd ")]

    grammar = xgr.Grammar.from_lark(
        'start[capture="outer"]: r "z"\n' 'r[max_chars=2, capture="inner", suffix="!"]: /[a-z]*/'
    )
    compiled = xgr.GrammarCompiler(xgr.TokenizerInfo([])).compile_grammar(grammar)
    for _ in range(4):
        for value, expected_captures in [
            ("a!z", [("inner", b"a"), ("outer", b"a!z")]),
            ("abz", [("inner", b"ab"), ("outer", b"abz")]),
        ]:
            matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
            assert matcher.accept_string(value) and matcher.is_terminated()
            assert matcher.get_captures() == expected_captures


@pytest.mark.parametrize("enable_dynamic_compilation", [False, True])
@pytest.mark.parametrize("cache_enabled", [False, True])
def test_number_multiple_of_runtime_mask_matches_completion(
    enable_dynamic_compilation: bool, cache_enabled: bool
):
    vocabulary = [
        '{"v":',
        "0.1",
        "0.25",
        "}",
        "e1}",
        "0.1}",
        "0.25}",
        "1e9999999999}",
        "1e-9999999999}",
    ]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiler = xgr.GrammarCompiler(
        tokenizer_info,
        max_threads=1,
        cache_enabled=cache_enabled,
        enable_dynamic_compilation=enable_dynamic_compilation,
    )
    compiled = compiler.compile_json_schema(
        {
            "type": "object",
            "properties": {"v": {"type": "number", "multipleOf": 0.25}},
            "required": ["v"],
            "additionalProperties": False,
        },
        any_whitespace=False,
        separators=(",", ":"),
    )
    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    assert matcher.accept_token(0)
    matcher.fill_next_token_bitmask(bitmask)
    allowed = bitmask_to_bool_mask(bitmask, tokenizer_info.vocab_size)[0]
    assert allowed[1] and allowed[2]
    assert not allowed[5]
    assert allowed[6]
    assert allowed[7]
    assert not allowed[8]

    # A non-multiple prefix remains extendable, but its delimiter is masked until an exponent
    # makes the completed number divisible by 0.25.
    assert matcher.accept_token(1)
    matcher.fill_next_token_bitmask(bitmask)
    allowed = bitmask_to_bool_mask(bitmask, tokenizer_info.vocab_size)[0]
    assert not allowed[3]
    assert allowed[4]

    stop_vocabulary = ["0.1", "0.25", "e1", "<eos>"]
    stop_tokenizer_info = xgr.TokenizerInfo(stop_vocabulary, stop_token_ids=[3])
    stop_compiled = xgr.GrammarCompiler(
        stop_tokenizer_info,
        max_threads=1,
        cache_enabled=cache_enabled,
        enable_dynamic_compilation=enable_dynamic_compilation,
    ).compile_json_schema({"type": "number", "multipleOf": 0.25}, any_whitespace=False)
    for first_token, stop_allowed in ((0, False), (1, True)):
        stop_matcher = xgr.GrammarMatcher(stop_compiled)
        assert stop_matcher.accept_token(first_token)
        stop_bitmask = xgr.allocate_token_bitmask(1, stop_tokenizer_info.vocab_size)
        stop_matcher.fill_next_token_bitmask(stop_bitmask)
        allowed = bitmask_to_bool_mask(stop_bitmask, stop_tokenizer_info.vocab_size)[0]
        assert bool(allowed[3]) == stop_allowed
        assert stop_matcher.accept_token(3) == stop_allowed


@pytest.mark.parametrize("enable_dynamic_compilation", [False, True])
@pytest.mark.parametrize("cache_enabled", [False, True])
def test_number_range_runtime_mask_matches_completion(
    enable_dynamic_compilation: bool, cache_enabled: bool
):
    vocabulary = [
        '{"v":',
        "0.0160425",
        "0.01604248",
        "}",
        "0.0160425}",
        "0.01604248}",
        "160425e-7}",
        "160424e-7}",
    ]
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info,
        max_threads=1,
        cache_enabled=cache_enabled,
        enable_dynamic_compilation=enable_dynamic_compilation,
    ).compile_json_schema(
        {
            "type": "object",
            "properties": {"v": {"type": "number", "minimum": 0.01604249, "maximum": 0.01604251}},
            "required": ["v"],
            "additionalProperties": False,
        },
        any_whitespace=False,
        separators=(",", ":"),
    )
    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    assert matcher.accept_token(0)
    matcher.fill_next_token_bitmask(bitmask)
    allowed = bitmask_to_bool_mask(bitmask, tokenizer_info.vocab_size)[0]
    assert allowed[1] and allowed[2]
    assert allowed[4] and allowed[6]
    assert not allowed[5] and not allowed[7]

    # The out-of-range prefix is structurally extendable, but completing it is rejected.
    assert matcher.accept_token(2)
    matcher.fill_next_token_bitmask(bitmask)
    allowed = bitmask_to_bool_mask(bitmask, tokenizer_info.vocab_size)[0]
    assert not allowed[3]
