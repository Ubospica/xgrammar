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
