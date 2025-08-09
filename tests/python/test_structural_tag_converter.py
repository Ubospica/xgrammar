import json
import sys
from typing import Any, Dict, List, Union

import pytest

import xgrammar as xgr
from xgrammar.testing import _is_grammar_accept_string, _stag_to_ebnf

# TODO: add the correct ebnf grammar
# TODO: add the invalid format and error message
# TODO: add more corner cases


def check_stag_with_grammar(stag_schema: Dict[str, Any], expected_grammar_ebnf: str):
    stag_ebnf = _stag_to_ebnf(stag_schema)
    assert stag_ebnf == expected_grammar_ebnf


def check_stag_with_instance(
    stag_schema: Dict[str, Any],
    instance: Union[str, Any],
    is_accepted: bool = True,
    debug_print: bool = False,
):
    stag_grammar = xgr.Grammar.from_structural_tag(stag_schema)

    if isinstance(instance, str):
        instance = json.dumps(instance)

    accepted = _is_grammar_accept_string(stag_grammar, instance, debug_print=debug_print)
    assert accepted == is_accepted


literal_test_data = [
    (
        {"type": "literal", "text": "Hello!"},
        "<TODO>",
        ["Hello!", "Hello", "Hello!!", "HELLO!"],
        [True, False, False, False],
    )
]


@pytest.mark.parametrize("stag_schema, expected_grammar, instances, is_accepted", literal_test_data)
def test_literal_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


json_schema_test_data = [
    (
        {"type": "json_schema", "json_schema": {"type": "string"}},
        "<TODO>",
        [{"Hello!"}, {1}, "???"],
        [True, False, False],
    )
]


@pytest.mark.parametrize(
    "stag_schema, expected_grammar, instances, is_accepted", json_schema_test_data
)
def test_json_schema_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


sequence_test_data = [
    (
        {
            "type": "sequence",
            "elements": [
                {"type": "literal", "text": "Hello!"},
                {"type": "json_schema", "json_schema": {"type": "number"}},
            ],
        },
        "<TODO>",
        [r'Hello!"str"', r"Hello!Hello!", r"Hello!", r'"str"Hello!', r"???"],
        [True, False, False, False, False],
    )
]


@pytest.mark.parametrize(
    "stag_schema, expected_grammar, instances, is_accepted", sequence_test_data
)
def test_sequence_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


or_test_data = [
    (
        {
            "type": "or",
            "elements": [
                {"type": "literal", "text": "Hello!"},
                {"type": "json_schema", "json_schema": {"type": "number"}},
            ],
        },
        "<TODO>",
        [r"Hello!", r'"str', r"Hello!Hello!", r'"str"Hello!', r"???"],
        [True, True, False, False, False],
    )
]


@pytest.mark.parametrize("stag_schema, expected_grammar, instances, is_accepted", or_test_data)
def test_or_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


tag_test_data = [
    (
        {
            "type": "tag",
            "begin": "BEG",
            "content": {"type": "literal", "text": "Hello!"},
            "end": "END",
        },
        "<TODO>",
        [r"BEGHello!END", r"BGHello!END", r"BEG???END", r"BEGHello!ED", r"BEGHello!ENDEND"],
        [True, False, False, False, False],
    )
]


@pytest.mark.parametrize("stag_schema, expected_grammar, instances, is_accepted", tag_test_data)
def test_tag_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


wildcard_test_data = [
    (
        {"type": "tag", "begin": "BEG", "content": {"type": "wildcard_test"}, "end": "END"},
        "<TODO>",
        [r"BEGHello!END", r"BEGENENNDENEND", r"BEGENENDEN", r"BEGBEGENDEND"],
        [True, True, False, False],
    )
]


@pytest.mark.parametrize(
    "stag_schema, expected_grammar, instances, is_accepted", wildcard_test_data
)
def test_wildcard_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


triggered_tag_test_data = [
    (
        {
            "type": "triggered_tags",
            "triggers": ["A"],
            "tags": [
                {
                    "type": "tag",
                    "begin": "A1",
                    "content": {"type": "literal", "text": "L1"},
                    "end": "A",
                },
                {"begin": "A2", "content": {"type": "literal", "text": "L2"}, "end": "A"},
            ],
            "at_least_one": False,
            "stop_after_first": False,
        },
        "<TODO>",
        [
            r"<text>A1L1A<text>A2L2A<text>A1L1A<text>",
            r"A1L1AA2L2AA2L2A",
            r"<text>",
            r"",
            r"A<trigger_but_no_beg>A1L1A<text>",
            r"<text>A1<wrong_content>A<text>",
        ],
        [True, True, True, True, False, False],
    ),
    (
        {
            "type": "triggered_tags",
            "triggers": ["A"],
            "tags": [
                {
                    "type": "tag",
                    "begin": "A1",
                    "content": {"type": "literal", "text": "L1"},
                    "end": "A",
                },
                {"begin": "A2", "content": {"type": "literal", "text": "L2"}, "end": "A"},
            ],
            "at_least_one": True,
            "stop_after_first": False,
        },
        "<TODO>",
        [r"<text>A1L1A<text>A2L2A<text>A1L1A<text>", r"A1L1AA2L2AA2L2A", r"<text>", r""],
        [False, True, False, False],  # may need to confirm
    ),
    (
        {
            "type": "triggered_tags",
            "triggers": ["A"],
            "tags": [
                {
                    "type": "tag",
                    "begin": "A1",
                    "content": {"type": "literal", "text": "L1"},
                    "end": "A",
                },
                {"begin": "A2", "content": {"type": "literal", "text": "L2"}, "end": "A"},
            ],
            "at_least_one": False,
            "stop_after_first": True,
        },
        "<TODO>",
        [r"<text>A1L1A<text>A2L2A<text>A1L1A<text>", r"<text>A1L1A", r"<text>", r""],
        [False, True, True, True],
    ),
    (
        {
            "type": "triggered_tags",
            "triggers": ["A"],
            "tags": [
                {
                    "type": "tag",
                    "begin": "A1",
                    "content": {"type": "literal", "text": "L1"},
                    "end": "A",
                },
                {"begin": "A2", "content": {"type": "literal", "text": "L2"}, "end": "A"},
            ],
            "at_least_one": True,
            "stop_after_first": True,
        },
        "<TODO>",
        [r"<text>A1L1A<text>A2L2A<text>A1L1A<text>", r"A1L1A", r"<text>", r""],
        [False, True, False, False],
    ),
]


@pytest.mark.parametrize(
    "stag_schema, expected_grammar, instances, is_accepted", triggered_tag_test_data
)
def test_triggered_tag_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


@pytest.mark.parametrize(
    "stag_schema, expected_grammar, instances, is_accepted", wildcard_test_data
)
def test_wildcard_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


tags_with_separator_test_data = [
    (
        {
            "type": "tags_with_separator",
            "tags": [
                {
                    "type": "tag",
                    "begin": "<A1",
                    "content": {"type": "literal", "text": "L1"},
                    "end": "A>",
                },
                {"begin": "<A2", "content": {"type": "literal", "text": "L2"}, "end": "A>"},
            ],
            "separator": "<AA>",
            "at_least_one": False,
            "stop_after_first": False,
        },
        "<TODO>",
        [
            r"<A1L1A><AA><A2L2A><AA><A1L1A>",
            r"",
            r"<A1L1A>",
            r"<A1L1A><A2L2A><A2L2A>",
            r"<A1L1A><AA><A2L2A><AA>",
            r"<AA><A1L1A>",
            r"<A1L1A><wrong_separator><A2L2A>",
            r"<A1L1A><AA><AA><A2L2A>",
        ],
        [True, True, True, False, False, False, False, False],
    ),
    (
        {
            "type": "tags_with_separator",
            "tags": [
                {
                    "type": "tag",
                    "begin": "<A1",
                    "content": {"type": "literal", "text": "L1"},
                    "end": "A>",
                },
                {"begin": "<A2", "content": {"type": "literal", "text": "L2"}, "end": "A>"},
            ],
            "separator": "<AA>",
            "at_least_one": True,
            "stop_after_first": False,
        },
        "<TODO>",
        [r"<A1L1A><AA><A2L2A><AA><A1L1A>", r"", r"<A1L1A>"],
        [True, False, True],
    ),
    (
        {
            "type": "tags_with_separator",
            "tags": [
                {
                    "type": "tag",
                    "begin": "<A1",
                    "content": {"type": "literal", "text": "L1"},
                    "end": "A>",
                },
                {"begin": "<A2", "content": {"type": "literal", "text": "L2"}, "end": "A>"},
            ],
            "separator": "<AA>",
            "at_least_one": False,
            "stop_after_first": True,
        },
        "<TODO>",
        [r"<A1L1A><AA><A2L2A><AA><A1L1A>", r"", r"<A1L1A>"],
        [False, True, True],
    ),
    (
        {
            "type": "tags_with_separator",
            "tags": [
                {
                    "type": "tag",
                    "begin": "<A1",
                    "content": {"type": "literal", "text": "L1"},
                    "end": "A>",
                },
                {"begin": "<A2", "content": {"type": "literal", "text": "L2"}, "end": "A>"},
            ],
            "separator": "<AA>",
            "at_least_one": True,
            "stop_after_first": True,
        },
        "<TODO>",
        [r"<A1L1A><AA><A2L2A><AA><A1L1A>", r"", r"<A1L1A>"],
        [False, False, True],
    ),
]


@pytest.mark.parametrize(
    "stag_schema, expected_grammar, instances, is_accepted", tags_with_separator_test_data
)
def test_tags_with_separator_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


compound_test_data = [
    # Llama JSON-based tool calling
    (
        {
            "type": "structural_tag",
            "format": {
                "type": "triggered_tags",
                "triggers": ['{"name":'],
                "tags": [
                    {
                        "begin": '{"name": "func1", "parameters": ',
                        "content": {"type": "json_schema", "json_schema": {"type": "object"}},
                        "end": "}",
                    },
                    {
                        "begin": '{"name": "func2", "parameters": ',
                        "content": {"type": "json_schema", "json_schema": {"type": "object"}},
                        "end": "}",
                    },
                ],
            },
        },
        [
            r"""<text>{"name": "func2", "parameters": {"arg": 10}<text>{"name": "func1", "parameters": {"arg": 10}<text>
         """
        ],
        [True],
    ),
    # Force think
    (
        {
            "type": "structural_tag",
            "format": {
                "type": "sequence",
                "elements": [
                    {
                        "type": "tag",
                        "begin": "<think>",
                        "content": {"type": "wildcard_text"},
                        "end": "</think>",
                    },
                    {
                        "type": "triggered_tags",
                        "triggers": ["<function="],
                        "tags": [
                            {
                                "begin": "<function=func1>",
                                "content": {
                                    "type": "json_schema",
                                    "json_schema": {"type": "object"},
                                },
                                "end": "</function>",
                            },
                            {
                                "begin": "<function=func2>",
                                "content": {
                                    "type": "json_schema",
                                    "json_schema": {"type": "object"},
                                },
                                "end": "</function>",
                            },
                        ],
                    },
                ],
            },
        },
        [
            (
                r"""<think>[any_text]</think>[any_text]<function=func2>{"arg": 10}</function>"""
                r"""[any_text]<function=func1>{"arg": 10}</function>[any_text]"""
            )
        ],
        [True],
    ),
    # Think & Force tool calling (Llama style)
    (
        {
            "type": "structural_tag",
            "format": {
                "type": "sequence",
                "elements": [
                    {
                        "type": "tag",
                        "begin": "<think>",
                        "content": {"type": "wildcard_text"},
                        "end": "</think>",
                    },
                    {
                        "type": "triggered_tags",
                        "triggers": ["<function="],
                        "tags": [
                            {
                                "begin": "<function=func1>",
                                "content": {
                                    "type": "json_schema",
                                    "json_schema": {"type": "object"},
                                },
                                "end": "</function>",
                            },
                            {
                                "begin": "<function=func2>",
                                "content": {
                                    "type": "json_schema",
                                    "json_schema": {"type": "object"},
                                },
                                "end": "</function>",
                            },
                        ],
                        "stop_after_first": True,
                        "at_least_one": True,
                    },
                ],
            },
        },
        [
            r"""<think>[any_text]</think>[any_text]<function=func2>{"arg": 10}</function>""",
            (
                r"""<think>[any_text]</think>[any_text]<function=func2>{"arg": 10}</function>"""
                r"""[any_text]<function=func1>{"arg": 10}</function>[any_text]"""
            ),
            r"""<think>[any_text]</think>[any_text]""",
        ],
        [True, False, False],
    ),
    # Think & force tool calling (DeepSeek style)
    (
        {
            "type": "structural_tag",
            "format": {
                "type": "sequence",
                "elements": [
                    {
                        "type": "tag",
                        "begin": "<think>",
                        "content": {"type": "wildcard_text"},
                        "end": "</think>",
                    },
                    {
                        "type": "triggered_tags",
                        "triggers": ["<｜tool▁calls▁begin｜>"],
                        "tags": [
                            {
                                "begin": "<｜tool▁calls▁begin｜>",
                                "end": "<｜tool▁calls▁end｜>",
                                "content": {
                                    "type": "tags_with_separator",
                                    "separator": "\n",
                                    "tags": [
                                        {
                                            "begin": "<｜tool▁call▁begin｜>function<｜tool▁sep｜>function_name_1\n```jsonc\n",
                                            "content": {
                                                "type": "json_schema",
                                                "json_schema": {"type": "object"},
                                            },
                                            "end": "\n```<｜tool▁call▁end｜>",
                                        },
                                        {
                                            "begin": "<｜tool▁call▁begin｜>function<｜tool▁sep｜>function_name_2\n```jsonc\n",
                                            "content": {
                                                "type": "json_schema",
                                                "json_schema": {"type": "object"},
                                            },
                                            "end": "\n```<｜tool▁call▁end｜>",
                                        },
                                    ],
                                    "at_least_one": True,
                                    "stop_after_first": True,
                                },
                            }
                        ],
                        "stop_after_first": True,
                    },
                ],
            },
        },
        # need to confirm
        [
            (
                r"""<think>[any_text]</think>[any_text]<｜tool▁calls▁begin｜>"""
                r"""<｜tool▁call▁begin｜>function<｜tool▁sep｜>function_name_1\n```jsonc\n"""
                r"""{"arg": 10}\n```<｜tool▁call▁end｜>"""
            )
        ],
        [True],
    ),
    # Force non-think mode
    (
        {
            "type": "structural_tag",
            "format": {
                "type": "sequence",
                "elements": [
                    {"type": "literal", "text": "<think></think>"},
                    {
                        "type": "triggered_tags",
                        "triggers": ["<tool_call>"],
                        "tags": [
                            {
                                "begin": '<tool_call>\n{"name": "func1", "arguments": ',
                                "content": {
                                    "type": "json_schema",
                                    "json_schema": {"type": "object"},
                                },
                                "end": "}\n</tool_call>",
                            },
                            {
                                "begin": '<tool_call>\n{"name": "func2", "arguments": ',
                                "content": {
                                    "type": "json_schema",
                                    "json_schema": {"type": "object"},
                                },
                                "end": "}\n</tool_call>",
                            },
                        ],
                    },
                ],
            },
        },
        [
            (
                r"""<think></think>[any_text]<tool_call>\n{"name": "func1", "arguments": {"arg": 10}}\n</tool_call>""",
                r"""[any_text]<tool_call>\n{"name": "func2", "arguments": {"arg": 10}}\n</tool_call>""",
                r"""[any_text]<tool_call>\n{"name": "func2", "arguments": {"arg": 10}}\n</tool_call>""",
            )
        ],
        [True],
    ),
]


@pytest.mark.parametrize("stag_schema, instances, is_accepted", compound_test_data)
def test_compound_format(
    stag_schema: Dict[str, Any], instances: List[Any], is_accepted: List[bool]
):
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


detect_end_str_test_data = [
    (
        {
            "type": "tag",
            "begin": "<start>",
            "content": {
                "type": "triggered_tag",
                "triggers": ["<start"],
                "tag": [
                    {
                        "begin": "<start2>",
                        "content": {"type": "literal", "text": "[TEXT]"},
                        "end": "<end2>",
                    }
                ],
            },
            "end": "<end>",
        },
        "<TODO>",
        [
            r"<start>[any_text]<start2>[TEXT]<end2><end>",
            r"<start>[any_text]<start2>[TEXT]<end2><end><start2>[TEXT]<end2>",
        ],
        [True, False],
    ),
    (
        {
            "type": "tag",
            "begin": "<start>",
            "content": {
                "type": "sequence",
                "elements": [
                    {"type": "literal", "text": "[TEXT}"},
                    {"type": "sequence", "elements": [{"type": "wildcard_text"}]},
                ],
            },
            "end": "<end>",
        },
        "<TODO>",
        [
            r"<start>[TEXT]<any_wildcard_text><end>",
            r"<start>[TEXT]<any_wildcard_text><end><any_wildcard_text>",
        ],
        [True, False],
    ),
    (
        {
            "type": "tag",
            "begin": "<start>",
            "content": {
                "type": "tags_with_separator",
                "tags": [
                    {
                        "begin": "<start2>",
                        "content": {"type": "literal", "text": "[TEXT]"},
                        "end": "<end2>",
                    }
                ],
                "separator": "<end>",
            },
            "end": "<end>",
        },
        "<TODO>",
        [
            r"<start><start2>[TEXT]<end2><end>",
            r"<start><start2>[TEXT]<end2><end><start2>[TEXT]<end2><end>",
        ],
        [True, False],
    ),
]


@pytest.mark.parametrize(
    "stag_schema, expected_grammar, instances, is_accepted", detect_end_str_test_data
)
def test_tags_with_separator_format(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


wrong_unlimited_stag_test_data = [
    (
        {
            "type": "sequence",
            "elements": [
                {
                    "type": "tags_with_separator",
                    "tags": [
                        {
                            "begin": "<start>",
                            "content": {"type": "literal", "text": "[TEXT]"},
                            "end": "<end>",
                        }
                    ],
                    "separator": "<sep>",
                },
                {"type": "literal", "text": "[TEXT]"},
            ],
        },
        False,
        "<TODO>",
    ),
    (
        {
            "type": "sequence",
            "elements": [
                {"type": "literal", "text": "[TEXT]"},
                {
                    "type": "tags_with_separator",
                    "tags": [
                        {
                            "begin": "<start>",
                            "content": {"type": "literal", "text": "[TEXT]"},
                            "end": "<end>",
                        }
                    ],
                    "separator": "<sep>",
                },
            ],
        },
        True,
        "",
    ),
    (
        {
            "type": "or",
            "elements": [
                {
                    "type": "tags_with_separator",
                    "tags": [
                        {
                            "begin": "<start>",
                            "content": {"type": "literal", "text": "[TEXT]"},
                            "end": "<end>",
                        }
                    ],
                    "separator": "<sep>",
                },
                {
                    "type": "tags_with_separator",
                    "tags": [
                        {
                            "begin": "<start>",
                            "content": {"type": "literal", "text": "[TEXT]"},
                            "end": "<end>",
                        }
                    ],
                    "separator": "<sep>",
                },
            ],
        },
        True,
        "",
    ),
    (
        {
            "type": "or",
            "elements": [
                {
                    "type": "tags_with_separator",
                    "tags": [
                        {
                            "begin": "<start>",
                            "content": {"type": "literal", "text": "[TEXT]"},
                            "end": "<end>",
                        }
                    ],
                    "separator": "<sep>",
                },
                {"type": "literal", "text": "[TEXT]"},
            ],
        },
        False,
        "<TODO>",
    ),
    (
        {
            "type": "tag",
            "begin": "<start>",
            "content": {
                "type": "tags_with_separator",
                "tags": [
                    {
                        "begin": "<start2>",
                        "content": {"type": "literal", "text": "[TEXT]"},
                        "end": "<end2>",
                    }
                ],
                "separator": "<sep>",
            },
            "end": "",
        },
        False,
        "<TODO>",
    ),
]


@pytest.mark.parametrize("stag_schema, is_valid, error_info", wrong_unlimited_stag_test_data)
def test_unlimited_stag(stag_schema: Dict[str, Any], is_valid: bool, error_info: str):
    if is_valid:
        _stag_to_ebnf(stag_schema)
    else:
        with pytest.raises(ValueError) as excinfo:
            _stag_to_ebnf(stag_schema)
        assert error_info in str(excinfo.value)


# TODO: add more corner cases
corner_test_data = [
    (
        {
            "type": "triggered_tag",
            "triggers": ["<start>"],
            "tag": [
                {
                    "begin": "<start>",
                    "content": {"type": "literal", "text": "[TEXT]"},
                    "end": "<end>",
                }
            ],
        },
        "<TODO>",
        [r"<start>[TEXT]<end>[TEXT]<start>[TEXT]<end>[TEXT]"],
        [True],
    )
]


@pytest.mark.parametrize("stag_schema, expected_grammar, instances, is_accepted", corner_test_data)
def test_corner_case(
    stag_schema: Dict[str, Any],
    expected_grammar: str,
    instances: List[Any],
    is_accepted: List[bool],
):
    check_stag_with_grammar(stag_schema, expected_grammar)
    for instance in instances:
        check_stag_with_instance(stag_schema, instance, is_accepted)


# TODO: look into the error message
# def test_invalid_form ...

if __name__ == "__main__":
    pytest.main(sys.argv)
