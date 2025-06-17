import sys
from typing import Any, Dict, List, Tuple

import pytest

from xgrammar.testing import parser_structural_tag


def test_basic_function_tag():
    """Test basic function tag with JSON content."""
    input_str = '<function="get_weather">{"location": "Beijing"}</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == '<function="get_weather">'
    assert parsed_tags[0][1] == {"location": "Beijing"}
    assert parsed_tags[0][2] == "</function>"


def test_function_tag_with_surrounding_text():
    """Test function tag with surrounding text."""
    input_str = 'I\'ll call the weather function: <function="get_weather">{"location": "Beijing"}</function> to get the weather for Beijing.'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == "I'll call the weather function:  to get the weather for Beijing."
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == '<function="get_weather">'
    assert parsed_tags[0][1] == {"location": "Beijing"}
    assert parsed_tags[0][2] == "</function>"


def test_multiple_tags():
    """Test multiple tags in one input."""
    input_str = '<function="get_weather">{"location": "Beijing"}</function> I\'ll also check the time: <function="get_time">{"timezone": "Asia/Shanghai"}</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == " I'll also check the time: "
    assert len(parsed_tags) == 2
    assert parsed_tags[0][0] == '<function="get_weather">'
    assert parsed_tags[0][1] == {"location": "Beijing"}
    assert parsed_tags[0][2] == "</function>"
    assert parsed_tags[1][0] == '<function="get_time">'
    assert parsed_tags[1][1] == {"timezone": "Asia/Shanghai"}
    assert parsed_tags[1][2] == "</function>"


def test_malformed_tag_no_closing_tag():
    """Test malformed tag with no closing tag."""
    input_str = '<function="get_weather">{"location": "Beijing"}'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert len(parsed_tags) == 0


def test_invalid_json_content():
    """Test tag with invalid JSON content."""
    input_str = '<function="get_weather">{"location": "Beijing</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert len(parsed_tags) == 0


def test_non_function_tag():
    """Test non-function tag without requiring JSON validation."""
    input_str = "<ipython>print('Hello world')</ipython>"
    triggers = ["<ipython>"]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == "<ipython>"
    # This shouldn't be parsed as JSON since it's not a function tag
    assert parsed_tags[0][1] == "print('Hello world')"
    assert parsed_tags[0][2] == "</ipython>"


def test_mixed_tag_types():
    """Test multiple tag types in the same input."""
    input_str = 'Using different tags: <function="calc">{"operation": "add", "values": [1, 2]}</function> and <ipython>import math; print(math.pi)</ipython>'
    triggers = ["<function=", "<ipython>"]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == "Using different tags:  and "
    assert len(parsed_tags) == 2
    assert parsed_tags[0][0] == '<function="calc">'
    assert parsed_tags[0][1] == {"operation": "add", "values": [1, 2]}
    assert parsed_tags[0][2] == "</function>"
    assert parsed_tags[1][0] == "<ipython>"
    assert parsed_tags[1][1] == "import math; print(math.pi)"
    assert parsed_tags[1][2] == "</ipython>"


def test_nested_structure_in_json():
    """Test JSON content with nested structure."""
    input_str = '<function="complex_data">{"user": {"name": "John", "age": 30}, "items": [1, 2, 3]}</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == '<function="complex_data">'
    assert parsed_tags[0][1] == {"user": {"name": "John", "age": 30}, "items": [1, 2, 3]}
    assert parsed_tags[0][2] == "</function>"


def test_empty_json_object():
    """Test function tag with empty JSON object."""
    input_str = '<function="ping">{}</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == '<function="ping">'
    assert parsed_tags[0][1] == {}
    assert parsed_tags[0][2] == "</function>"


def test_json_with_special_values():
    """Test JSON with special values like null, true, false, numbers."""
    input_str = '<function="test_values">{"null_value": null, "bool_true": true, "bool_false": false, "number": 42, "float": 3.14}</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == '<function="test_values">'
    assert parsed_tags[0][1] == {
        "null_value": None,
        "bool_true": True,
        "bool_false": False,
        "number": 42,
        "float": 3.14,
    }
    assert parsed_tags[0][2] == "</function>"


def test_unicode_in_json():
    """Test JSON with Unicode characters."""
    input_str = '<function="translate">{"text": "こんにちは世界", "target": "español"}</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == '<function="translate">'
    assert parsed_tags[0][1] == {"text": "こんにちは世界", "target": "español"}
    assert parsed_tags[0][2] == "</function>"


def test_empty_input():
    """Test with empty input string."""
    input_str = ""
    triggers = ["<function=", "<ipython>"]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 0


def test_no_tags_found():
    """Test with input that doesn't contain any of the trigger strings."""
    input_str = "This is just some regular text with no tags in it."
    triggers = ["<function=", "<ipython>"]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == input_str
    assert len(parsed_tags) == 0


def test_whitespace_handling():
    """Test with various whitespace in the input."""
    input_str = """
        <function="test">
            {
                "param": "value"
            }
        </function>
    """
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    # Whitespace outside tags should be preserved
    assert raw_text.strip() == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == '<function="test">'
    assert parsed_tags[0][1] == {"param": "value"}
    assert parsed_tags[0][2] == "</function>"


def test_invalid_json_array():
    """Test with invalid JSON array content."""
    input_str = '<function="test">[1, 2, 3</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert len(parsed_tags) == 0


def test_escaped_quotes_in_json():
    """Test with escaped quotes in JSON content."""
    input_str = '<function="message">{"text": "He said \\"Hello\\""}</function>'
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == '<function="message">'
    assert parsed_tags[0][1] == {"text": 'He said "Hello"'}
    assert parsed_tags[0][2] == "</function>"


def test_code_tag():
    """Test with a code tag."""
    input_str = '<code>def hello():\n    print("Hello, world!")</code>'
    triggers = ["<code>"]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 1
    assert parsed_tags[0][0] == "<code>"
    # This is not JSON, so it should be kept as a string
    assert parsed_tags[0][1] == 'def hello():\n    print("Hello, world!")'
    assert parsed_tags[0][2] == "</code>"


def test_adjacent_tags():
    """Test with tags that are adjacent with no text between them."""
    input_str = (
        '<function="first">{"value": 1}</function><function="second">{"value": 2}</function>'
    )
    triggers = ["<function="]
    raw_text, parsed_tags = parser_structural_tag(input_str, triggers)

    assert raw_text == ""
    assert len(parsed_tags) == 2
    assert parsed_tags[0][0] == '<function="first">'
    assert parsed_tags[0][1] == {"value": 1}
    assert parsed_tags[0][2] == "</function>"
    assert parsed_tags[1][0] == '<function="second">'
    assert parsed_tags[1][1] == {"value": 2}
    assert parsed_tags[1][2] == "</function>"


if __name__ == "__main__":
    pytest.main(sys.argv)
