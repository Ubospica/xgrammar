import json

import pytest

import xgrammar as xgr
from xgrammar.testing import _get_matcher_from_grammar, _is_grammar_accept_string


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
        (
            r"^[a-zA-Z]{2,3}(-[a-zA-Z]{4})?(-([a-zA-Z]{2}|[0-9]{3}))?(-[a-zA-Z]{5,8})?(-x(-[a-zA-Z0-9]{1,8})+)?$",
            "en-US",
            "invalid",
        ),
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


def test_empty_pattern_alternative_cannot_consume_json_structure_as_string_content():
    schema = {
        "type": "object",
        "properties": {
            "text": {"type": "string", "pattern": r"^$|(^(?:\S+\s+){0,99}\S+$)"},
            "weight": {"type": "integer", "minimum": 5, "maximum": 20},
        },
        "required": ["text", "weight"],
        "additionalProperties": False,
    }
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), strict_mode=False, any_order=True)
    assert "json_schema_streaming_pattern" in str(grammar)

    valid = _get_matcher_from_grammar(grammar)
    assert valid.accept_string(r'{"text":"","weight":5}')
    assert valid.is_terminated()

    # The invalid number cannot complete its property.  In the old CFG fallback, the non-empty
    # pattern arm started at the first empty string and swallowed both quotes plus the remaining
    # object as `\S`/`\s` source characters, so every byte was accepted as an unfinished prefix.
    invalid = _get_matcher_from_grammar(grammar)
    assert not invalid.accept_string(r'{"text":"","weight":4}')


def test_optional_wide_character_class_requires_one_character_when_present():
    pattern = r"^[^:\s]+:[^:\s]+(:[^\s]+)?$"
    assert _accepts(pattern, "chrome:latest")
    assert _accepts(pattern, "chrome:latest:stable")
    assert not _accepts(pattern, "chrome:latest:")


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


def test_streaming_search_pattern_decodes_surrogate_pairs():
    grammar = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": r"[\s\S]"}))
    assert _is_grammar_accept_string(grammar, r'"\ud83d\ude00"')


@pytest.mark.parametrize(
    "encoded_value", ['"é"', r'"\u00e9"', '"😀"', r'"\ud83d\ude00"', r'"\n"', r'"\t"']
)
def test_streaming_any_character_decodes_equivalent_json_spellings(encoded_value: str):
    grammar = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": r"[\s\S]"}))
    assert _is_grammar_accept_string(grammar, encoded_value)
    assert not _is_grammar_accept_string(grammar, '""')


def test_streaming_ascii_literal_decodes_unicode_escape_and_short_escape():
    literal = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": "a"}))
    assert _is_grammar_accept_string(literal, '"prefix-a-suffix"')
    assert _is_grammar_accept_string(literal, r'"prefix-\u0061-suffix"')

    newline = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": r"\n"}))
    assert _is_grammar_accept_string(newline, r'"prefix\nsuffix"')
    assert _is_grammar_accept_string(newline, r'"prefix\u000asuffix"')
    assert not _is_grammar_accept_string(newline, '"prefix-n-suffix"')


def test_large_literal_alternative_search_handles_failure_transitions():
    alternatives = ["bcd", "abce", "he", "she", "his", "hers"] + [
        f"region-{index:03d}" for index in range(128)
    ]
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "|".join(alternatives)})
    )
    assert "json_schema_streaming_pattern" in str(grammar)
    for value in ["abcd", "ushers", "prefix-region-127-suffix"]:
        assert _is_grammar_accept_string(grammar, json.dumps(value))
    for value in ["abcf", "prefix-region-128-suffix", "nothing"]:
        assert not _is_grammar_accept_string(grammar, json.dumps(value))


def test_streaming_pattern_properties_and_length_constraints():
    alternatives = ["needle", "bcd", "abce"] + [f"zone-{index:03d}" for index in range(64)]
    schema = {
        "type": "object",
        "patternProperties": {"|".join(alternatives): {"type": "integer"}},
        "additionalProperties": False,
    }
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema))
    assert "json_schema_streaming_pattern" in str(grammar)
    assert _is_grammar_accept_string(grammar, '{"prefix-zone-063-suffix":1}')
    assert _is_grammar_accept_string(grammar, '{"abcd":2}')
    assert not _is_grammar_accept_string(grammar, '{"prefix-zone-064-suffix":1}')
    assert not _is_grammar_accept_string(grammar, '{"prefix-zone-063-suffix":"wrong"}')

    string_grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "target|bcd|abce", "minLength": 7, "maxLength": 7})
    )
    assert _is_grammar_accept_string(string_grammar, '"😀target"')
    assert _is_grammar_accept_string(string_grammar, r'"\ud83d\ude00target"')
    assert not _is_grammar_accept_string(string_grammar, '"target"')
    assert not _is_grammar_accept_string(string_grammar, '"😀targetx"')


@pytest.mark.parametrize("encoded_value", ['"é"', r'"\u00e9"', '"😀"', r'"\ud83d\ude00"'])
def test_anchored_dot_counts_decoded_unicode_codepoints(encoded_value: str):
    one = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": "^.$"}))
    two = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": "^..$"}))
    assert _is_grammar_accept_string(one, encoded_value)
    assert not _is_grammar_accept_string(two, encoded_value)


@pytest.mark.parametrize(
    "pattern,accepted,rejected",
    [("^é$", '"é"', '"ê"'), ("^[éê]$", '"ê"', '"a"'), ("^[😀-😿]$", '"😀"', '"🌀"')],
)
def test_anchored_unicode_pattern_decodes_equivalent_json_spellings(
    pattern: str, accepted: str, rejected: str
):
    grammar = xgr.Grammar.from_json_schema(json.dumps({"type": "string", "pattern": pattern}))
    accepted_value = json.loads(accepted)
    escaped_accepted = json.dumps(accepted_value, ensure_ascii=True)
    assert _is_grammar_accept_string(grammar, accepted)
    assert _is_grammar_accept_string(grammar, escaped_accepted)
    assert not _is_grammar_accept_string(grammar, rejected)


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


@pytest.mark.parametrize("encoded_value", ['"aa"', '"éa"', '"€a"', '"😀a"', r'"\na"', r'"\u0061a"'])
def test_runtime_pattern_length_decodes_json_source(encoded_value: str):
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "a", "minLength": 2, "maxLength": 2})
    )
    assert _is_grammar_accept_string(grammar, encoded_value)


@pytest.mark.parametrize("encoded_value", [r'"\ud83d"', r'"\ude00"', r'"\ud83d\u0061"'])
def test_runtime_pattern_length_rejects_unpaired_surrogates(encoded_value: str):
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": ".*", "minLength": 1, "maxLength": 1})
    )
    assert not _is_grammar_accept_string(grammar, encoded_value)


def test_runtime_pattern_length_follows_cfg_subrules():
    grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "string", "pattern": "^(?:éa){1,3}$", "minLength": 4, "maxLength": 4})
    )
    assert _is_grammar_accept_string(grammar, '"éaéa"')
    assert not _is_grammar_accept_string(grammar, '"éa"')
    assert not _is_grammar_accept_string(grammar, '"éaéaéa"')


@pytest.mark.parametrize(
    "bounds,accepted,rejected",
    [
        ({"minLength": 2}, '"aa"', '"a"'),
        ({"maxLength": 2}, '"aa"', '"aaa"'),
        ({"minLength": 2, "maxLength": 2}, '"aa"', '"aaa"'),
    ],
)
def test_runtime_pattern_one_sided_and_exact_length_bounds(bounds, accepted, rejected):
    schema = {"type": "string", "pattern": "a", **bounds}
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema))
    assert _is_grammar_accept_string(grammar, accepted)
    assert not _is_grammar_accept_string(grammar, rejected)


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
    tokenizer_info = xgr.TokenizerInfo(['"', "01aF", "5", '"'], stop_token_ids=[])
    compiler = xgr.GrammarCompiler(
        tokenizer_info, enable_dynamic_compilation=enable_dynamic_compilation
    )
    compiled = compiler.compile_json_schema(
        json.dumps({"type": "string", "pattern": "[0-9a-fA-F]+", "minLength": 4, "maxLength": 4})
    )
    restored = xgr.CompiledGrammar.deserialize_json(compiled.serialize_json(), tokenizer_info)
    matcher = xgr.GrammarMatcher(restored, terminate_without_stop_token=True)
    for token_id in [0, 1, 3]:
        assert matcher.accept_token(token_id)
    assert matcher.is_terminated()

    matcher = xgr.GrammarMatcher(restored, terminate_without_stop_token=True)
    for token_id in [0, 1]:
        assert matcher.accept_token(token_id)
    assert not matcher.accept_token(2)


@pytest.mark.parametrize("enable_dynamic_compilation", [False, True])
def test_large_streaming_pattern_serialization_preserves_masks(enable_dynamic_compilation: bool):
    alternatives = ["bcd", "abce", "hers"] + [f"region-{index:03d}" for index in range(96)]
    vocabulary = ['"', "prefix-", "abcd", "region-095", "region-096", "-suffix", '"']
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=enable_dynamic_compilation
    ).compile_json_schema(
        json.dumps(
            {"type": "string", "pattern": "|".join(alternatives), "minLength": 4, "maxLength": 32}
        )
    )
    restored = xgr.CompiledGrammar.deserialize_json(compiled.serialize_json(), tokenizer_info)
    for token_ids in ([0, 2, 6], [0, 1, 3, 5, 6]):
        original = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
        recovered = xgr.GrammarMatcher(restored, terminate_without_stop_token=True)
        original_mask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
        recovered_mask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
        for token_id in token_ids:
            original.fill_next_token_bitmask(original_mask)
            recovered.fill_next_token_bitmask(recovered_mask)
            assert original_mask.equal(recovered_mask)
            assert original.accept_token(token_id)
            assert recovered.accept_token(token_id)
        assert original.is_terminated()
        assert recovered.is_terminated()


@pytest.mark.parametrize("enable_dynamic_compilation", [False, True])
def test_runtime_pattern_length_mask_matches_acceptance(enable_dynamic_compilation: bool):
    vocabulary = ['"', "01aF", "01aF5", "5", '"']
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=enable_dynamic_compilation
    ).compile_json_schema(
        json.dumps({"type": "string", "pattern": "[0-9a-fA-F]+", "minLength": 4, "maxLength": 4})
    )
    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    assert matcher.accept_token(0)
    bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    matcher.fill_next_token_bitmask(bitmask)
    rejected = set(xgr.testing._get_masked_tokens_from_bitmask(bitmask, tokenizer_info.vocab_size))
    assert 1 not in rejected
    assert 2 in rejected


@pytest.mark.parametrize("enable_dynamic_compilation", [False, True])
def test_runtime_pattern_length_cfg_mask_matches_acceptance(enable_dynamic_compilation: bool):
    vocabulary = ['"', "éaéa", "éa", "éaéaéa", '"']
    tokenizer_info = xgr.TokenizerInfo(vocabulary, stop_token_ids=[])
    compiled = xgr.GrammarCompiler(
        tokenizer_info, max_threads=1, enable_dynamic_compilation=enable_dynamic_compilation
    ).compile_json_schema(
        json.dumps({"type": "string", "pattern": "^(?:éa){1,3}$", "minLength": 4, "maxLength": 4})
    )
    matcher = xgr.GrammarMatcher(compiled, terminate_without_stop_token=True)
    assert matcher.accept_token(0)
    bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    matcher.fill_next_token_bitmask(bitmask)
    rejected = set(xgr.testing._get_masked_tokens_from_bitmask(bitmask, tokenizer_info.vocab_size))
    assert 1 not in rejected
    assert 2 not in rejected
    assert 3 in rejected

    assert matcher.accept_token(2)
    matcher.fill_next_token_bitmask(bitmask)
    rejected = set(xgr.testing._get_masked_tokens_from_bitmask(bitmask, tokenizer_info.vocab_size))
    assert 1 in rejected
    assert 2 not in rejected
    assert 3 in rejected
    assert 4 in rejected
