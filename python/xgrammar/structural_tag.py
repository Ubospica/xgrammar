from __future__ import annotations

from typing import Any, Dict, List, Literal, Optional, Union

from pydantic import BaseModel, Field, ValidationError

# ---------- Basic Types ----------


class LiteralFormat(BaseModel):
    type: Literal["literal"] = "literal"
    text: str


class JSONSchemaFormat(BaseModel):
    type: Literal["json_schema"] = "json_schema"
    json_schema: Union[Dict[str, Any], BaseModel]


# ---------- Combinatorial Formats ----------


class SequenceFormat(BaseModel):
    type: Literal["sequence"] = "sequence"
    elements: List["Format"]


class TagFormat(BaseModel):
    type: Literal["tag"] = "tag"
    begin: str
    content: "Format"
    end: str


class WildcardTagFormat(BaseModel):
    type: Literal["wildcard_tag"] = "wildcard_tag"
    begin: str
    end: str


class _NoTypeTagFormat(BaseModel):
    begin: str
    content: "Format"
    end: str


class TriggeredTagsFormat(BaseModel):
    type: Literal["triggered_tags"] = "triggered_tags"
    triggers: List[str]
    tags: List[TagFormat]
    at_least_one: Optional[bool] = None
    stop_after_first: Optional[bool] = None


class TagsWithSeparatorFormat(BaseModel):
    type: Literal["tags_with_separator"] = "tags_with_separator"
    tags: List[TagFormat]
    separator: str
    at_least_one: Optional[bool] = None
    stop_after_first: Optional[bool] = None


# ---------- Top Level ----------


class StructuralTag(BaseModel):
    """
    Top level object, corresponding to `"response_format": {"type":"structural_tag", "format":{...}}` in API
    """

    type: Literal["structural_tag"] = "structural_tag"
    format: "Format"


# ---------- Discriminated Union ----------

Format = Union[
    LiteralFormat,
    JSONSchemaFormat,
    WildcardTextFormat,
    SequenceFormat,
    TagFormat,
    TagsAndTextFormat,
    TagsWithSeparatorFormat,
]

# Solve forward references
SequenceFormat.model_rebuild()
TagFormat.model_rebuild()
TagsAndTextFormat.model_rebuild()
TagsWithSeparatorFormat.model_rebuild()
StructuralTag.model_rebuild()
