import json
from typing import Any, Callable, Dict, List, Literal, Type, Union

# from .base import _core
from pydantic import BaseModel, Field

# ---------- Basic Types ----------


class LiteralFormat(BaseModel):
    type: Literal["literal"] = "literal"
    text: str


class JSONSchemaFormat(BaseModel):
    type: Literal["json_schema"] = "json_schema"
    json_schema: Union[Dict[str, Any], BaseModel]


class WildcardTextFormat(BaseModel):
    type: Literal["wildcard_text"] = "wildcard_text"


# ---------- Combinatorial Formats ----------


class SequenceFormat(BaseModel):
    type: Literal["sequence"] = "sequence"
    elements: List["Format"]


class TagFormat(BaseModel):
    type: Literal["tag"] = "tag"
    begin: str
    content: "Format"
    end: str


class TriggeredTagsFormat(BaseModel):
    type: Literal["triggered_tags"] = "triggered_tags"
    triggers: List[str]
    tags: List[TagFormat]
    at_least_one: bool = False
    stop_after_first: bool = False


class TagsWithSeparatorFormat(BaseModel):
    type: Literal["tags_with_separator"] = "tags_with_separator"
    tags: List[TagFormat]
    separator: str
    at_least_one: bool = False
    stop_after_first: bool = False


# ---------- Discriminated Union ----------

Format = Union[
    LiteralFormat,
    JSONSchemaFormat,
    WildcardTextFormat,
    SequenceFormat,
    TagFormat,
    TriggeredTagsFormat,
    TagsWithSeparatorFormat,
]

# Solve forward references
SequenceFormat.model_rebuild()
TagFormat.model_rebuild()
TriggeredTagsFormat.model_rebuild()
TagsWithSeparatorFormat.model_rebuild()


# ---------- Top Level ----------


class StructuralTagItem(BaseModel):
    """Deprecated. Definition of a structural tag item.

    See Grammar.from_structural_tag() for more details.
    """

    begin: str
    """The begin tag."""
    schema_: Union[str, Type[BaseModel], Dict[str, Any]] = Field(alias="schema")
    """The schema."""
    end: str
    """The end tag."""


_TOOL_CALLING_TEMPLATE_REGISTRY: Dict[str, Callable] = {}
_REASONING_TEMPLATE_REGISTRY: Dict[str, "StructuralTag"] = {}


def register_tool_calling_template(name: str, override: bool = False):
    def decorator(func: Callable[..., "StructuralTag"]) -> Callable:
        if name in _TOOL_CALLING_TEMPLATE_REGISTRY and not override:
            raise ValueError(
                f"Tool calling template '{name}' is already registered. "
                f"Use override=True to replace it."
            )
        _TOOL_CALLING_TEMPLATE_REGISTRY[name] = func
        return func

    return decorator


def register_reasoning_template(name: str, override: bool = False):
    def decorator(func: Callable[..., "StructuralTag"]) -> Callable:
        if name in _REASONING_TEMPLATE_REGISTRY and not override:
            raise ValueError(
                f"Reasoning template '{name}' is already registered. "
                f"Use override=True to replace it."
            )

        _REASONING_TEMPLATE_REGISTRY[name] = func
        return func

    return decorator


class StructuralTag(BaseModel):
    """
    Top level object, corresponding to `"response_format": {"type":"structural_tag", "format":{...}}` in API
    """

    type: Literal["structural_tag"] = "structural_tag"
    format: Format

    @staticmethod
    def from_legacy_structural_tag(
        tags: List[StructuralTagItem], triggers: List[str]
    ) -> "StructuralTag":
        """Convert a legacy structural tag item to a structural tag."""
        return StructuralTag(
            type="structural_tag",
            format=TriggeredTagsFormat(
                type="triggered_tags",
                triggers=triggers,
                tags=[
                    TagFormat(
                        begin=tag.begin,
                        content=JSONSchemaFormat(
                            json_schema=(
                                json.loads(tag.schema_)
                                if isinstance(tag.schema_, str)
                                else tag.schema_
                            )
                        ),
                        end=tag.end,
                    )
                    for tag in tags
                ],
            ),
        )

    @staticmethod
    def from_openai_tool_calling(
        template: str,
        tools: List[Dict[str, Any]],
        tool_choice: Union[str, Dict[str, Any]],
        *,
        allow_text: bool = True,
        enable_thinking: bool = False,
    ) -> "StructuralTag":
        """Convert an OpenAI API format to a structural tag."""
        return _TOOL_CALLING_TEMPLATE_REGISTRY[template](
            tools=tools,
            tool_choice=tool_choice,
            allow_text=allow_text,
            enable_thinking=enable_thinking,
        )


@register_tool_calling_template("llama-3.1-8b-instruct")
def llama_3_1_8b_instruct_tool_calling_template(
    tools: List[Dict[str, Any]], template: str
) -> StructuralTag:
    return StructuralTag(type="structural_tag", format=tools)


@register_reasoning_template("deepseek")
def deepseek_reasoning_template() -> StructuralTag:
    return StructuralTag(
        type="structural_tag",
        format=TagFormat(begin="<think>", content=WildcardTextFormat(), end="</think>"),
    )


def main():
    pass
    # stag_str = """
    # {
    #     "type": "structural_tag",
    #     "format": {
    #         "type": "sequence",
    #         "elements": [{"type": "literal", "text": "Hello"}, {"type": "literal", "text": "World"}]
    #     }
    # }
    # """
    # stag = StructuralTag.model_validate_json(stag_str)
    # print(stag)

    # stag_obj = {
    #     "type": "structural_tag",
    #     "format": {
    #         "type": "sequence",
    #         "elements": [
    #             {"type": "literal", "text": "Hello"},
    #             {"type": "literal", "text": "World"},
    #         ],
    #     },
    # }
    # stag = StructuralTag.model_validate(stag_obj)
    # print(stag)

    # stag_triggered_tags_obj = {
    #     "type": "triggered_tags",
    #     "triggers": ["<", ">"],
    #     "tags": [{"begin": "<", "content": {"type": "literal", "text": "Hello"}, "end": ">"}],
    # }
    # stag_triggered_tags = TriggeredTagsFormat.model_validate(stag_triggered_tags_obj)
    # print(stag_triggered_tags)


if __name__ == "__main__":
    main()
