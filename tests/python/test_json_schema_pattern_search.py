import json

import pytest

import xgrammar as xgr
from xgrammar.testing import _is_grammar_accept_string


def _accepts(pattern: str, value: str) -> bool:
    grammar = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": pattern}))
    return _is_grammar_accept_string(grammar, json.dumps(value))


@pytest.mark.parametrize(
    "pattern, accepted, rejected",
    [
        ("abc", "prefix-abc-suffix", "abXc"),
        ("^abc", "abc-suffix", "prefix-abc"),
        ("abc$", "prefix-abc", "abc-suffix"),
        ("^a|b$", "a-tail", "prefix-a"),
        ("^a|b$", "prefix-b", "b-tail"),
        (r"\^a\$", "prefix-^a$-suffix", "prefix-a-suffix"),
        (r"^[\^$]+$", "$^^$", "x$"),
        (r"^.+@.+$", "user@example.com", "user-example.com"),
        (r"^\{.*\}$", '{"quoted": "value"}', "not-an-object"),
        (r"^[^\x01-\x1f]+$", "Microsoft.PowerToys", "line\nbreak"),
        (r"^$|(^(?:\S+\s+){0,99}\S+$)", "two words", " two words"),
        (r"(^gs://(.+))|(^https://(.+))", "https://example.com/a", "ftp://example.com"),
        ("alpha|beta|gamma", "prefix-beta-suffix", "delta"),
        ("^alpha|^beta|gamma$|delta$", "beta-tail", "prefix-beta"),
        ("^alpha|^beta|gamma$|delta$", "prefix-delta", "delta-tail"),
    ],
)
def test_json_schema_pattern_search_and_anchor_semantics(
    pattern: str, accepted: str, rejected: str
):
    assert _accepts(pattern, accepted)
    assert not _accepts(pattern, rejected)


def test_pattern_properties_use_search_semantics():
    schema = {
        "type": "object",
        "patternProperties": {"needle": {"type": "integer"}},
        "additionalProperties": False,
    }
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema))
    assert _is_grammar_accept_string(grammar, '{"prefix-needle-suffix":1}')
    assert not _is_grammar_accept_string(grammar, '{"other":1}')


def test_pattern_properties_preserve_additional_properties():
    schema = {
        "type": "object",
        "patternProperties": {"sdkrel:.*": {"type": "string"}},
        "additionalProperties": {"type": "boolean"},
    }
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema))
    assert _is_grammar_accept_string(grammar, '{"sdkrel:path":"relative","MY_VAR":true}')
    assert not _is_grammar_accept_string(grammar, '{"MY_VAR":"not-a-boolean"}')


@pytest.mark.parametrize(
    "pattern, value",
    [
        (r"/^[0-9]{4}$/", "/2026/"),
        (r"^([^,]|$)", "name"),
        (r"^(\/?((\.{2})|([a-z0-9\-]*))($|\/))*$", "path/"),
        (r"^(?=[^=]+$)(?!\s+$)(.|\n)+$", "key"),
    ],
)
def test_nested_or_non_boundary_anchors_do_not_regress_compilation(pattern: str, value: str):
    grammar = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": pattern}))
    # The legacy parser approximates anchors embedded in larger expressions. Search rewriting must
    # not turn these previously compilable schemas into hard failures.
    assert isinstance(_is_grammar_accept_string(grammar, json.dumps(value)), bool)


def test_pattern_json_string_escape_spellings():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": r"^a[\"\\/]b$"})
    )
    assert _is_grammar_accept_string(grammar, r'"a\"b"')
    assert _is_grammar_accept_string(grammar, r'"a\\b"')
    assert _is_grammar_accept_string(grammar, r'"a\/b"')
    assert not _is_grammar_accept_string(grammar, r'"a\qb"')

    literal = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": r"^a$"}))
    assert _is_grammar_accept_string(literal, r'"\u0061"')
    assert not _is_grammar_accept_string(literal, r'"\u0062"')


def test_bounded_character_class_repeat_counts_decoded_characters():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": r"^[A-F\d]{2,3}$"})
    )
    assert _is_grammar_accept_string(grammar, r'"A\u0039"')
    assert _is_grammar_accept_string(grammar, r'"\u0041F0"')
    assert not _is_grammar_accept_string(grammar, r'"\u0041F09"')
    assert not _is_grammar_accept_string(grammar, r'"A\u0061"')


def test_pattern_and_length_constraints_are_conjoined():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "[0-9a-fA-F]+", "minLength": 4, "maxLength": 4})
    )
    assert _is_grammar_accept_string(grammar, '"01aF"')
    assert _is_grammar_accept_string(grammar, r'"0\u0031aF"')
    assert not _is_grammar_accept_string(grammar, '"01a"')
    assert not _is_grammar_accept_string(grammar, '"01aF5"')
    # JSON Schema patterns use search semantics, so every four-character value containing at
    # least one hexadecimal run is valid. A value with no matching substring is rejected.
    assert not _is_grammar_accept_string(grammar, '"wxyz"')


def test_exact_length_character_class_search_uses_whole_string_equivalence():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "[0-9]{10,10}", "minLength": 10, "maxLength": 10})
    )
    assert _is_grammar_accept_string(grammar, '"0123456789"')
    assert _is_grammar_accept_string(grammar, r'"01234\u00356789"')
    assert not _is_grammar_accept_string(grammar, '"01234x6789"')
    assert not _is_grammar_accept_string(grammar, '"012345678"')
    assert not _is_grammar_accept_string(grammar, '"01234567890"')


def test_pattern_and_length_count_unicode_code_points():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "a", "minLength": 2, "maxLength": 2})
    )
    assert _is_grammar_accept_string(grammar, '"éa"')
    assert _is_grammar_accept_string(grammar, '"😀a"')
    assert not _is_grammar_accept_string(grammar, '"a"')
    assert not _is_grammar_accept_string(grammar, '"aaa"')


def test_open_ended_simple_repeat_is_tightened_by_length():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "^[a-z]{2,}$", "minLength": 4, "maxLength": 5})
    )
    assert _is_grammar_accept_string(grammar, '"abcd"')
    assert _is_grammar_accept_string(grammar, '"abcde"')
    assert not _is_grammar_accept_string(grammar, '"abc"')
    assert not _is_grammar_accept_string(grammar, '"abcdef"')


def test_disjoint_simple_pattern_and_length_constraints_are_unsatisfiable():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "^[a-z]{1,2}$", "minLength": 3, "maxLength": 4})
    )
    assert not _is_grammar_accept_string(grammar, '"ab"')
    assert not _is_grammar_accept_string(grammar, '"abc"')


def test_unbounded_simple_pattern_repeat_is_tightened_by_length():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "^[0-9a-z-]*$", "minLength": 4, "maxLength": 63})
    )
    assert _is_grammar_accept_string(grammar, '"a-b9"')
    assert not _is_grammar_accept_string(grammar, '"abc"')
    assert not _is_grammar_accept_string(grammar, '"aBcd"')


def test_bounded_character_class_repeat_handles_json_escape_forms():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": r'^["\\/\n]{4}$'})
    )
    assert _is_grammar_accept_string(grammar, r'"\"\\\/\n"')
    assert _is_grammar_accept_string(grammar, r'"\u0022\u005c\u002f\u000a"')
    assert not _is_grammar_accept_string(grammar, r'"\"\\\/x"')


def test_zero_bounded_character_class_repeat():
    grammar = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": r"^[A]{0}$"}))
    assert _is_grammar_accept_string(grammar, '""')
    assert not _is_grammar_accept_string(grammar, '"A"')


@pytest.mark.parametrize("enable_dynamic_compilation", [False, True])
@pytest.mark.parametrize("cache_enabled", [False, True])
def test_pattern_search_mask_modes(enable_dynamic_compilation: bool, cache_enabled: bool):
    tokenizer_info = xgr.TokenizerInfo(['"prefix-', "abc", '-suffix"'], stop_token_ids=[])
    grammar = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": "abc"}))
    compiled = xgr.GrammarCompiler(
        tokenizer_info,
        cache_enabled=cache_enabled,
        enable_dynamic_compilation=enable_dynamic_compilation,
    ).compile_grammar(grammar)
    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    for token_id in range(3):
        assert matcher.accept_token(token_id)
    assert matcher.is_terminated()


@pytest.mark.parametrize("enable_dynamic_compilation", [False, True])
@pytest.mark.parametrize("cache_enabled", [False, True])
def test_pattern_escaped_json_mask_modes(enable_dynamic_compilation: bool, cache_enabled: bool):
    tokenizer_info = xgr.TokenizerInfo(['"{', r"\"", "quoted", r"\"", '}"'], stop_token_ids=[])
    grammar = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": r"^\{.*\}$"}))
    compiled = xgr.GrammarCompiler(
        tokenizer_info,
        cache_enabled=cache_enabled,
        enable_dynamic_compilation=enable_dynamic_compilation,
    ).compile_grammar(grammar)
    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    for token_id in range(5):
        assert matcher.accept_token(token_id)
    assert matcher.is_terminated()


@pytest.mark.parametrize("enable_dynamic_compilation", [False, True])
def test_pattern_length_compiled_grammar_serialization(enable_dynamic_compilation: bool):
    tokenizer_info = xgr.TokenizerInfo(['"', "1234567890", '"'], stop_token_ids=[])
    compiler = xgr.GrammarCompiler(
        tokenizer_info, enable_dynamic_compilation=enable_dynamic_compilation
    )
    compiled = compiler.compile_json_schema(
        json.dumps({"type": "string", "pattern": "[0-9]{10,10}", "minLength": 10, "maxLength": 10})
    )
    restored = xgr.CompiledGrammar.deserialize_json(compiled.serialize_json(), tokenizer_info)
    matcher = xgr.GrammarMatcher(restored, terminate_without_stop_token=True)
    for token_id in range(3):
        assert matcher.accept_token(token_id)
    assert matcher.is_terminated()
