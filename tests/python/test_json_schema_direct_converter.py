import json
from typing import Any, Dict

import pytest

import xgrammar as xgr
from xgrammar.testing import _is_grammar_accept_string


def _accepts(schema: Dict[str, Any], text: str, **kwargs: Any) -> bool:
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), **kwargs)
    return _is_grammar_accept_string(grammar, text)


@pytest.mark.parametrize(
    "text, expected",
    [
        ('{"name":"ab","count":0}', True),
        ('{"name": "abcd", "count": -2, "choice": "a", "nullable": null}', True),
        ('{"name":"a","count":0}', False),
        ('{"count":0,"name":"ab"}', False),
        ('{"name":"ab","count":9}', False),
        ('{"name":"ab","count":0,"extra":true}', False),
    ],
)
def test_direct_converter_basic_types_and_compositions(text: str, expected: bool):
    schema = {
        "type": "object",
        "properties": {
            "name": {"type": "string", "minLength": 2, "maxLength": 4},
            "count": {"type": "integer", "minimum": -2, "maximum": 8},
            "choice": {"enum": ["a", "b", 3]},
            "nullable": {"type": ["string", "null"]},
        },
        "required": ["name", "count"],
        "additionalProperties": False,
    }
    assert _accepts(schema, text) is expected


@pytest.mark.parametrize(
    "text, expected",
    [
        ('["header",{"id":3,"code":"AB12"}]', True),
        ('["header", {"id": 6, "code": "XY9"}, {"id": -3, "code": "AA0"}]', True),
        ('["header"]', False),
        ('["header",{"id":4,"code":"AB12"}]', False),
        ('["header",{"id":3,"code":"ab12"}]', False),
    ],
)
def test_direct_converter_arrays_references_and_patterns(text: str, expected: bool):
    schema = {
        "$defs": {
            "entry": {
                "type": "object",
                "properties": {
                    "id": {"type": "integer", "multipleOf": 3},
                    "code": {"type": "string", "pattern": "^[A-Z]{2}[0-9]+$"},
                },
                "required": ["id", "code"],
                "additionalProperties": False,
            }
        },
        "type": "array",
        "prefixItems": [{"const": "header"}],
        "items": {"$ref": "#/$defs/entry"},
        "minItems": 2,
        "maxItems": 3,
    }
    assert _accepts(schema, text) is expected


@pytest.mark.parametrize(
    "text, expected",
    [
        ('{"fixed":true}', True),
        ('{"x_a":2}', True),
        ('{"other":"value"}', True),
        ('{"fixed":false,"x_test":3,"other":"value"}', True),
        ("{}", False),
        ('{"fixed":true,"x_a":1,"one":"1","two":"2"}', False),
    ],
)
def test_direct_converter_property_constraints_and_additional_keys(text: str, expected: bool):
    schema = {
        "type": "object",
        "properties": {"fixed": {"type": "boolean"}, "optional": {"type": "number"}},
        "patternProperties": {"^x_[a-z]+$": {"type": "integer"}},
        "additionalProperties": {"type": "string"},
        "minProperties": 1,
        "maxProperties": 3,
    }
    assert _accepts(schema, text) is expected


def test_direct_converter_formatting_and_any_order():
    schema = {
        "type": "object",
        "properties": {"first": {"type": "string"}, "second": {"type": "integer"}},
        "required": ["first", "second"],
        "additionalProperties": False,
    }
    options = {"any_whitespace": False, "indent": 2, "strict_mode": True, "any_order": True}
    assert _accepts(schema, '{\n  "first": "x",\n  "second": 1\n}', **options)
    assert _accepts(schema, '{\n  "second": 1,\n  "first": "x"\n}', **options)
    assert not _accepts(schema, '{"first":"x","second":1}', **options)

    bounded = {"any_whitespace": True, "max_whitespace_cnt": 3}
    assert _accepts(schema, '{"first":"x","second":1}', **bounded)
    assert _accepts(schema, '{"first": "x", "second": 1}', **bounded)
    assert not _accepts(schema, '{    "first":"x","second":1}', **bounded)


def test_direct_converter_any_order_keeps_indentation_context():
    repeated_schema = {
        "type": "object",
        "properties": {"name": {"type": "string"}, "count": {"type": "integer"}},
        "required": ["name", "count"],
        "additionalProperties": False,
    }
    schema = {
        "type": "object",
        "properties": {
            "first": repeated_schema,
            "wrapper": {
                "type": "object",
                "properties": {"second": repeated_schema},
                "required": ["second"],
                "additionalProperties": False,
            },
        },
        "required": ["first", "wrapper"],
        "additionalProperties": False,
    }
    instance = {
        "first": {"count": 1, "name": "a"},
        "wrapper": {"second": {"count": 2, "name": "b"}},
    }
    valid_text = json.dumps(instance, indent=2)
    invalid_indentation_text = valid_text.replace('\n      "count": 2', '\n    "count": 2')

    grammar = xgr.Grammar.from_json_schema(
        json.dumps(schema), any_whitespace=False, indent=2, any_order=True
    )
    assert _is_grammar_accept_string(grammar, valid_text)
    assert not _is_grammar_accept_string(grammar, invalid_indentation_text)


def test_direct_converter_non_strict_mode_keeps_indentation_context():
    repeated_schema = {
        "type": "object",
        "properties": {"known": {"type": "integer"}},
        "required": ["known"],
    }
    schema = {
        "type": "object",
        "properties": {
            "first": repeated_schema,
            "wrapper": {
                "type": "object",
                "properties": {"second": repeated_schema},
                "required": ["second"],
                "additionalProperties": False,
            },
        },
        "required": ["first", "wrapper"],
        "additionalProperties": False,
    }
    instance = {
        "first": {"known": 1, "extra": "a"},
        "wrapper": {"second": {"known": 2, "extra": "b"}},
    }
    valid_text = json.dumps(instance, indent=2)
    invalid_indentation_text = valid_text.replace('\n      "known": 2', '\n    "known": 2')

    non_strict_grammar = xgr.Grammar.from_json_schema(
        json.dumps(schema), any_whitespace=False, indent=2, strict_mode=False
    )
    strict_grammar = xgr.Grammar.from_json_schema(
        json.dumps(schema), any_whitespace=False, indent=2, strict_mode=True
    )
    assert _is_grammar_accept_string(non_strict_grammar, valid_text)
    assert not _is_grammar_accept_string(non_strict_grammar, invalid_indentation_text)
    assert not _is_grammar_accept_string(strict_grammar, valid_text)


def test_direct_converter_indent_respects_max_whitespace_count_in_unconstrained_values():
    schema = {
        "type": "object",
        "properties": {"payload": {"type": "object"}},
        "required": ["payload"],
        "additionalProperties": False,
    }
    grammar = xgr.Grammar.from_json_schema(
        json.dumps(schema), any_whitespace=False, indent=2, max_whitespace_cnt=1
    )

    assert _is_grammar_accept_string(grammar, '{\n  "payload": {"value": 1}\n}')
    assert not _is_grammar_accept_string(grammar, '{\n  "payload": {"value":  1}\n}')


def test_direct_converter_same_schema_keeps_compact_and_indented_formats_separate():
    schema = {
        "type": "object",
        "properties": {"name": {"type": "string"}, "count": {"type": "integer"}},
        "required": ["name", "count"],
        "additionalProperties": False,
    }
    compact_text = '{"name": "x", "count": 1}'
    indented_text = json.dumps({"name": "x", "count": 1}, indent=2)

    compact_grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False)
    indented_grammar = xgr.Grammar.from_json_schema(
        json.dumps(schema), any_whitespace=False, indent=2
    )
    assert _is_grammar_accept_string(compact_grammar, compact_text)
    assert not _is_grammar_accept_string(compact_grammar, indented_text)
    assert _is_grammar_accept_string(indented_grammar, indented_text)
    assert not _is_grammar_accept_string(indented_grammar, compact_text)


@pytest.mark.parametrize(
    "text, expected",
    [
        ('{"value":1,"next":null}', True),
        ('{"value":1,"next":{"value":2,"next":null}}', True),
        ('{"value":1}', False),
        ('{"value":1,"next":{"value":"bad","next":null}}', False),
    ],
)
def test_direct_converter_recursive_reference(text: str, expected: bool):
    schema: Dict[str, Any] = {
        "type": "object",
        "properties": {
            "value": {"type": "integer"},
            "next": {"anyOf": [{"$ref": "#"}, {"type": "null"}]},
        },
        "required": ["value", "next"],
        "additionalProperties": False,
    }
    assert _accepts(schema, text) is expected


@pytest.mark.parametrize(
    "text, expected",
    [
        ('"user@example.com"', True),
        ('"first.last+tag@example.co.uk"', True),
        ('"not-an-email"', False),
        ('"@example.com"', False),
    ],
)
def test_direct_converter_builtin_string_format(text: str, expected: bool):
    assert _accepts({"type": "string", "format": "email"}, text) is expected


def test_direct_converter_reuses_identical_schema_rules():
    repeated_schema = {"type": "string", "minLength": 2, "maxLength": 8}
    schema = {
        "type": "object",
        "properties": {"first": repeated_schema, "second": repeated_schema},
        "required": ["first", "second"],
        "additionalProperties": False,
    }

    grammar_text = str(xgr.Grammar.from_json_schema(json.dumps(schema)))
    assert "root_prop_0 ::=" in grammar_text
    assert "root_prop_1 ::=" not in grammar_text


def test_direct_converter_schema_cache_separates_default_type_after_untyped_schema():
    pattern_schema = {"pattern": "^[a-z]+$"}
    schema = {
        "type": "object",
        "properties": {"value": pattern_schema},
        "propertyNames": pattern_schema,
        "additionalProperties": True,
    }

    assert _accepts(schema, '{"valid":1}')
    assert not _accepts(schema, '{"BAD":1}')


def test_direct_converter_schema_cache_separates_untyped_schema_after_default_type():
    pattern_schema = {"pattern": "^[a-z]+$"}
    schema = {
        "type": "object",
        "properties": {
            "first": {
                "type": "object",
                "propertyNames": pattern_schema,
                "additionalProperties": True,
            },
            "value": pattern_schema,
        },
        "required": ["first", "value"],
        "additionalProperties": False,
    }

    assert _accepts(schema, '{"first":{},"value":1}')


@pytest.mark.parametrize(
    "first_schema, second_schema, expected_reuse",
    [
        (
            {"type": "string", "minLength": 2, "maxLength": 4},
            {"maxLength": 4, "type": "string", "minLength": 2},
            True,
        ),
        (
            {"type": "string", "minLength": 2, "maxLength": 4, "title": "First value"},
            {
                "description": "Second value",
                "default": "ab",
                "maxLength": 4,
                "type": "string",
                "minLength": 2,
            },
            False,
        ),
    ],
    ids=["key-order", "annotation-values"],
)
def test_direct_converter_schema_cache_uses_canonical_key(
    first_schema: Dict[str, Any], second_schema: Dict[str, Any], expected_reuse: bool
):
    schema = {
        "type": "object",
        "properties": {"first": first_schema, "second": second_schema},
        "required": ["first", "second"],
        "additionalProperties": False,
    }

    grammar = xgr.Grammar.from_json_schema(json.dumps(schema))
    assert _is_grammar_accept_string(grammar, '{"first":"ab","second":"cd"}')
    assert not _is_grammar_accept_string(grammar, '{"first":"a","second":"cd"}')
    assert ("root_prop_1 ::=" not in str(grammar)) is expected_reuse


@pytest.mark.parametrize(
    "property_name",
    [
        "title",
        "default",
        "description",
        "examples",
        "deprecated",
        "readOnly",
        "writeOnly",
        "$comment",
        "$schema",
    ],
)
def test_direct_converter_schema_cache_preserves_property_names(property_name: str):
    leaf_schema = {"type": "object", "properties": {}, "additionalProperties": False}
    nested_schema = {
        "type": "object",
        "properties": {property_name: leaf_schema},
        "additionalProperties": False,
    }
    schema = {
        "type": "object",
        "properties": {property_name: nested_schema},
        "required": [property_name],
        "additionalProperties": False,
    }

    assert _accepts(schema, json.dumps({property_name: {property_name: {}}}))
    assert not _accepts(schema, json.dumps({property_name: {property_name: {property_name: {}}}}))


def test_direct_converter_schema_cache_escapes_property_names():
    injected_property_name = 'x":{"type":"string"},"y'
    first_schema = {
        "type": "object",
        "properties": {injected_property_name: {"type": "number"}},
        "additionalProperties": False,
    }
    second_schema = {
        "type": "object",
        "properties": {"x": {"type": "string"}, "y": {"type": "number"}},
        "additionalProperties": False,
    }
    schema = {
        "type": "object",
        "properties": {"first": first_schema, "second": second_schema},
        "required": ["first", "second"],
        "additionalProperties": False,
    }

    assert _accepts(
        schema, json.dumps({"first": {injected_property_name: 1}, "second": {"x": "value", "y": 2}})
    )
    assert not _accepts(schema, json.dumps({"first": {}, "second": {injected_property_name: 1}}))


@pytest.mark.parametrize("explicit_string_type", [False, True], ids=["implicit", "explicit"])
@pytest.mark.parametrize("composition", ["direct", "anyOf", "oneOf", "allOf"])
def test_direct_converter_reference_preserves_property_name_string_context(
    explicit_string_type: bool, composition: str
):
    pattern_schema = {"pattern": "^[a-z]+$"}
    name_schema = pattern_schema if composition == "direct" else {composition: [pattern_schema]}
    reference_schema = {"$ref": "#/$defs/name"}
    if explicit_string_type:
        reference_schema["type"] = "string"
    schema = {
        "$defs": {"name": name_schema},
        "type": "array",
        "prefixItems": [
            reference_schema,
            {"type": "object", "propertyNames": name_schema, "additionalProperties": True},
            {"type": "object", "propertyNames": reference_schema, "additionalProperties": True},
        ],
        "minItems": 3,
        "maxItems": 3,
    }

    assert _accepts(schema, '["value",{"valid":1},{"validtoo":2}]')
    assert not _accepts(schema, '["value",{"valid":1},{"INVALID":2}]')


def test_direct_converter_reuses_cached_reference_targets():
    repeated_schema = {"type": "string", "minLength": 2, "maxLength": 8}
    schema = {
        "$defs": {"shared": repeated_schema},
        "type": "object",
        "properties": {"inline": repeated_schema, "referenced": {"$ref": "#/$defs/shared"}},
        "required": ["inline", "referenced"],
        "additionalProperties": False,
    }

    grammar = xgr.Grammar.from_json_schema(json.dumps(schema))
    assert _is_grammar_accept_string(grammar, '{"inline":"ab","referenced":"cd"}')
    assert "defs_shared ::=" not in str(grammar)


def test_direct_converter_keeps_lazy_reference_target_indentation_context():
    schema = {
        "$defs": {"shared": {"type": "array", "items": {"type": "integer"}, "minItems": 1}},
        "type": "object",
        "properties": {"payload": {"$ref": "#/$defs/shared"}},
        "required": ["payload"],
        "additionalProperties": False,
    }
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)
    assert _is_grammar_accept_string(grammar, '{\n  "payload": [\n    1\n  ]\n}')
    assert not _is_grammar_accept_string(grammar, '{\n  "payload": [\n  1\n  ]\n}')


def test_direct_converter_keeps_indented_rules_at_different_depths_separate():
    repeated_schema = {
        "type": "object",
        "properties": {"value": {"type": "string", "minLength": 2}},
        "required": ["value"],
        "additionalProperties": False,
    }
    schema = {
        "type": "object",
        "properties": {
            "first": repeated_schema,
            "wrapper": {
                "type": "object",
                "properties": {"second": repeated_schema},
                "required": ["second"],
                "additionalProperties": False,
            },
        },
        "required": ["first", "wrapper"],
        "additionalProperties": False,
    }
    text = (
        "{\n"
        '  "first": {\n'
        '    "value": "ab"\n'
        "  },\n"
        '  "wrapper": {\n'
        '    "second": {\n'
        '      "value": "cd"\n'
        "    }\n"
        "  }\n"
        "}"
    )

    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)
    assert _is_grammar_accept_string(grammar, text)
    assert not _is_grammar_accept_string(
        grammar, text.replace('\n      "value": "cd"', '\n    "value": "cd"')
    )
    assert not _is_grammar_accept_string(grammar, text.replace('"value": "cd"', '"value": 2'))


@pytest.mark.parametrize(
    "repeated_schema, first_value, second_value, invalid_second_value",
    [
        (
            {"type": "array", "items": {"type": "integer"}, "minItems": 1},
            [1, 2],
            [3, 4],
            [3, "invalid"],
        ),
        (
            {
                "anyOf": [
                    {
                        "type": "object",
                        "properties": {"value": {"type": "string"}},
                        "required": ["value"],
                        "additionalProperties": False,
                    },
                    {"type": "null"},
                ]
            },
            {"value": "ab"},
            {"value": "cd"},
            {"value": 1},
        ),
        (
            {
                "allOf": [
                    {
                        "type": "object",
                        "properties": {"value": {"type": "string"}},
                        "required": ["value"],
                        "additionalProperties": False,
                    }
                ]
            },
            {"value": "ab"},
            {"value": "cd"},
            {"value": 1},
        ),
        (
            {
                "type": ["object", "null"],
                "properties": {"value": {"type": "string"}},
                "required": ["value"],
                "additionalProperties": False,
            },
            {"value": "ab"},
            {"value": "cd"},
            {"value": 1},
        ),
        (
            {
                "oneOf": [
                    {
                        "type": "object",
                        "properties": {"value": {"type": "string"}},
                        "required": ["value"],
                        "additionalProperties": False,
                    },
                    {"type": "array", "items": {"type": "integer"}},
                ]
            },
            {"value": "ab"},
            {"value": "cd"},
            {"value": 1},
        ),
        (
            {
                "type": "object",
                "patternProperties": {"^x_[a-z]+$": {"type": "integer"}},
                "additionalProperties": False,
                "minProperties": 1,
            },
            {"x_first": 1},
            {"x_second": 2},
            {"x_second": "invalid"},
        ),
        (
            {
                "type": "object",
                "propertyNames": {"pattern": "^[a-z]+$"},
                "additionalProperties": {"type": "integer"},
                "minProperties": 1,
            },
            {"first": 1},
            {"second": 2},
            {"INVALID": 2},
        ),
        (
            {
                "type": "object",
                "additionalProperties": {
                    "type": "object",
                    "properties": {"value": {"type": "string"}},
                    "required": ["value"],
                    "additionalProperties": False,
                },
                "minProperties": 1,
            },
            {"first": {"value": "ab"}},
            {"second": {"value": "cd"}},
            {"second": {"value": 1}},
        ),
    ],
    ids=[
        "array",
        "any-of",
        "all-of",
        "type-array",
        "one-of",
        "pattern-properties",
        "property-names",
        "additional-properties",
    ],
)
@pytest.mark.parametrize("use_reference", [False, True], ids=["inline", "reference"])
def test_direct_converter_keeps_composite_cache_entries_at_their_indentation_depth(
    repeated_schema: Dict[str, Any],
    first_value: Any,
    second_value: Any,
    invalid_second_value: Any,
    use_reference: bool,
):
    property_schema = {"$ref": "#/$defs/shared"} if use_reference else repeated_schema
    schema = {
        "type": "object",
        "properties": {
            "first": property_schema,
            "wrapper": {
                "type": "object",
                "properties": {"second": property_schema},
                "required": ["second"],
                "additionalProperties": False,
            },
        },
        "required": ["first", "wrapper"],
        "additionalProperties": False,
    }
    if use_reference:
        schema["$defs"] = {"shared": repeated_schema}
    instance = {"first": first_value, "wrapper": {"second": second_value}}
    invalid_instance = {"first": first_value, "wrapper": {"second": invalid_second_value}}

    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)
    valid_text = json.dumps(instance, indent=2)
    assert _is_grammar_accept_string(grammar, valid_text)
    assert not _is_grammar_accept_string(grammar, valid_text.replace("\n      ", "\n    "))
    assert not _is_grammar_accept_string(grammar, json.dumps(invalid_instance, indent=2))


def test_direct_converter_unconstrained_rules_allow_any_indentation():
    schema = {
        "type": "object",
        "properties": {"object": {"type": "object"}, "array": {"type": "array"}},
        "required": ["object", "array"],
        "additionalProperties": False,
    }
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)

    pretty = json.dumps(
        {"object": {"nested": [1, {"value": True}]}, "array": [1, {"value": 2}]}, indent=2
    )
    compact_values = """{
  "object": {"nested":[1,{"value":true}]},
  "array": [1,{"value":2}]
}"""
    assert _is_grammar_accept_string(grammar, pretty)
    assert _is_grammar_accept_string(grammar, compact_values)
    assert not _is_grammar_accept_string(grammar, '{\n  "object": [],\n  "array": {}\n}')


def test_direct_converter_keeps_repeated_reference_targets_at_their_indentation_depth():
    schema = {
        "$defs": {
            "shared": {
                "type": "object",
                "properties": {"value": {"type": "integer"}},
                "required": ["value"],
                "additionalProperties": False,
            }
        },
        "type": "object",
        "properties": {
            "first": {"$ref": "#/$defs/shared"},
            "wrapper": {
                "type": "object",
                "properties": {"second": {"$ref": "#/$defs/shared"}},
                "required": ["second"],
                "additionalProperties": False,
            },
        },
        "required": ["first", "wrapper"],
        "additionalProperties": False,
    }
    instance = {"first": {"value": 1}, "wrapper": {"second": {"value": 2}}}
    valid_text = json.dumps(instance, indent=2)
    invalid_text = valid_text.replace('\n      "value": 2', '\n    "value": 2')

    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)
    assert _is_grammar_accept_string(grammar, valid_text)
    assert not _is_grammar_accept_string(grammar, invalid_text)
    assert not _is_grammar_accept_string(
        grammar,
        json.dumps({"first": {"value": 1}, "wrapper": {"second": {"value": "invalid"}}}, indent=2),
    )


def test_direct_converter_recursive_reference_uses_any_whitespace_after_first_depth():
    schema: Dict[str, Any] = {
        "type": "object",
        "properties": {
            "value": {"type": "integer"},
            "next": {"anyOf": [{"$ref": "#"}, {"type": "null"}]},
        },
        "required": ["value", "next"],
        "additionalProperties": False,
    }
    instance = {"value": 1, "next": {"value": 2, "next": {"value": 3, "next": None}}}

    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)
    assert _is_grammar_accept_string(grammar, json.dumps(instance, indent=2))
    assert _is_grammar_accept_string(
        grammar, '{\n  "value": 1,\n  "next": {"value":2,"next":{"value":3,"next":null}}\n}'
    )
    assert not _is_grammar_accept_string(
        grammar, '{\n  "value": 1,\n  "next": {"value": "invalid", "next": null}\n}'
    )


def test_direct_converter_recursive_fallback_precedes_same_depth_cache_hit():
    node_schema = {
        "type": "object",
        "properties": {
            "value": {"type": "integer"},
            "next": {"anyOf": [{"$ref": "#/$defs/node"}, {"type": "null"}]},
        },
        "required": ["value", "next"],
        "additionalProperties": False,
    }
    schema = {
        "$defs": {"node": node_schema},
        "type": "object",
        "properties": {
            "a": {
                "type": "object",
                "properties": {"node": {"$ref": "#/$defs/node"}},
                "required": ["node"],
                "additionalProperties": False,
            },
            "b": {"$ref": "#/$defs/node"},
        },
        "required": ["b"],
        "additionalProperties": False,
    }
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)

    assert _is_grammar_accept_string(
        grammar, '{\n  "b": {\n    "value": 2,\n    "next": {"value":3,"next":null}\n  }\n}'
    )
    assert not _is_grammar_accept_string(
        grammar, '{\n  "b": {\n    "value": 2,\n    "next": {"value":"invalid","next":null}\n  }\n}'
    )


def test_direct_converter_recursive_fallback_preserves_custom_separators():
    unconstrained_grammar = xgr.Grammar.from_json_schema(
        json.dumps({"type": "object"}),
        any_whitespace=False,
        indent=2,
        separators=(",", " => "),
        strict_mode=False,
    )
    assert _is_grammar_accept_string(unconstrained_grammar, '{"value" => 1}')
    assert not _is_grammar_accept_string(unconstrained_grammar, '{"value":1}')

    recursive_schema: Dict[str, Any] = {
        "type": "object",
        "properties": {
            "value": {"type": "integer"},
            "next": {"anyOf": [{"$ref": "#"}, {"type": "null"}]},
        },
        "required": ["value", "next"],
        "additionalProperties": False,
    }
    recursive_grammar = xgr.Grammar.from_json_schema(
        json.dumps(recursive_schema), any_whitespace=False, indent=2, separators=(",", " => ")
    )
    assert _is_grammar_accept_string(
        recursive_grammar, '{\n  "value" => 1,\n  "next" => {"value"=>2,"next"=>null}\n}'
    )
    assert not _is_grammar_accept_string(
        recursive_grammar, '{\n  "value" => 1,\n  "next" => {"value":2,"next":null}\n}'
    )


@pytest.mark.parametrize(
    "schema, instance, compact_recursive_text, invalid_instance",
    [
        (
            {
                "$defs": {
                    "first": {
                        "type": "object",
                        "properties": {
                            "name": {"type": "string"},
                            "next": {"anyOf": [{"$ref": "#/$defs/second"}, {"type": "null"}]},
                        },
                        "required": ["name", "next"],
                        "additionalProperties": False,
                    },
                    "second": {
                        "type": "object",
                        "properties": {
                            "count": {"type": "integer"},
                            "next": {"anyOf": [{"$ref": "#/$defs/first"}, {"type": "null"}]},
                        },
                        "required": ["count", "next"],
                        "additionalProperties": False,
                    },
                },
                "$ref": "#/$defs/first",
            },
            {"name": "root", "next": {"count": 1, "next": {"name": "leaf", "next": None}}},
            (
                "{\n"
                '  "name": "root",\n'
                '  "next": {\n'
                '    "count": 1,\n'
                '    "next": {"name":"leaf","next":null}\n'
                "  }\n"
                "}"
            ),
            {"name": "root", "next": {"count": 1, "next": {"name": 2, "next": None}}},
        ),
        (
            {
                "$defs": {
                    "node": {
                        "type": "object",
                        "properties": {
                            "value": {"type": "integer"},
                            "children": {"type": "array", "items": {"$ref": "#/$defs/node"}},
                        },
                        "required": ["value", "children"],
                        "additionalProperties": False,
                    }
                },
                "$ref": "#/$defs/node",
            },
            {"value": 1, "children": [{"value": 2, "children": []}]},
            (
                "{\n"
                '  "value": 1,\n'
                '  "children": [\n'
                '    {"value":2,"children":[]}\n'
                "  ]\n"
                "}"
            ),
            {"value": 1, "children": [{"value": "invalid", "children": []}]},
        ),
        (
            {
                "$defs": {
                    "alias_a": {"$ref": "#/$defs/alias_b"},
                    "alias_b": {"$ref": "#/$defs/node"},
                    "node": {
                        "type": "object",
                        "properties": {
                            "value": {"type": "integer"},
                            "next": {"anyOf": [{"$ref": "#/$defs/alias_a"}, {"type": "null"}]},
                        },
                        "required": ["value", "next"],
                        "additionalProperties": False,
                    },
                },
                "$ref": "#/$defs/alias_a",
            },
            {"value": 1, "next": {"value": 2, "next": None}},
            ("{\n" '  "value": 1,\n' '  "next": {"value":2,"next":null}\n' "}"),
            {"value": 1, "next": {"value": "invalid", "next": None}},
        ),
    ],
    ids=["mutual-definitions", "object-array-object", "reference-alias-chain"],
)
def test_direct_converter_recursive_reference_shapes(
    schema: Dict[str, Any], instance: Any, compact_recursive_text: str, invalid_instance: Any
):
    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)

    assert _is_grammar_accept_string(grammar, json.dumps(instance, indent=2))
    assert _is_grammar_accept_string(grammar, compact_recursive_text)
    assert not _is_grammar_accept_string(grammar, json.dumps(invalid_instance, indent=2))


def test_direct_converter_keeps_recursive_targets_at_their_initial_indentation_depth():
    node_schema = {
        "type": "object",
        "properties": {
            "value": {"type": "integer"},
            "next": {"anyOf": [{"$ref": "#/$defs/node"}, {"type": "null"}]},
        },
        "required": ["value", "next"],
        "additionalProperties": False,
    }
    schema = {
        "$defs": {"node": node_schema},
        "type": "object",
        "properties": {
            "first": {"$ref": "#/$defs/node"},
            "wrapper": {
                "type": "object",
                "properties": {"second": {"$ref": "#/$defs/node"}},
                "required": ["second"],
                "additionalProperties": False,
            },
        },
        "required": ["first", "wrapper"],
        "additionalProperties": False,
    }
    instance = {
        "first": {"value": 1, "next": None},
        "wrapper": {"second": {"value": 2, "next": {"value": 3, "next": None}}},
    }
    valid_text = json.dumps(instance, indent=2)
    compact_recursive_text = valid_text.replace(
        '{\n        "value": 3,\n        "next": null\n      }', '{"value":3,"next":null}'
    )
    invalid_indentation_text = valid_text.replace('\n      "value": 2', '\n    "value": 2')

    grammar = xgr.Grammar.from_json_schema(json.dumps(schema), any_whitespace=False, indent=2)
    assert _is_grammar_accept_string(grammar, valid_text)
    assert _is_grammar_accept_string(grammar, compact_recursive_text)
    assert not _is_grammar_accept_string(grammar, invalid_indentation_text)


def test_direct_converter_limits_whitespace_in_recursive_rules():
    schema: Dict[str, Any] = {
        "type": "object",
        "properties": {
            "value": {"type": "integer"},
            "next": {"anyOf": [{"$ref": "#"}, {"type": "null"}]},
        },
        "required": ["value", "next"],
        "additionalProperties": False,
    }
    grammar = xgr.Grammar.from_json_schema(
        json.dumps(schema), any_whitespace=False, indent=2, max_whitespace_cnt=1
    )

    assert _is_grammar_accept_string(
        grammar, '{\n  "value": 1,\n  "next": {"value": 2,"next": null}\n}'
    )
    assert not _is_grammar_accept_string(
        grammar, '{\n  "value": 1,\n  "next": {"value":  2,"next": null}\n}'
    )
