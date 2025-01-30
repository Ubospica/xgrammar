"""This script benchmarks the time for grammar compilation and mask generation."""

import argparse
import json
import time

import datasets
import torch
from lmformatenforcer import JsonSchemaParser, TokenEnforcer
from lmformatenforcer.integrations.transformers import (
    TokenEnforcerTokenizerData,
    build_token_enforcer_tokenizer_data,
)

# from outlines.fsm.guide import Guide, RegexGuide
# from outlines.fsm.json_schema import convert_json_schema_to_str
# from outlines.generate.generator import bias_logits
# from outlines.generate.json import build_regex_from_schema
# from outlines.models import TransformerTokenizer
from tqdm import tqdm
from transformers import AutoTokenizer

import xgrammar as xgr

wrong_data_indices = [1]


xml_grammar = """root ::= element

content ::= "" | [^<&] content | element content | reference content | COMMENT content

element ::= "<" Name ([ \\t\\r\\n] [ \\t\\r\\n]* attribute)* (">" content "<" "/" Name ">" | "/>")

reference ::= "&" Name ";" | "&#" [0-9]+ ";" | "&#x" [a-fA-F0-9]+ ";"

attribute ::= Name [ \\t\\r\\n]* "=" [ \\t\\r\\n]* STRING

COMMENT ::= "<!--" ( [^-] | "-" [^-] | "--" [^>] )* "-->"

Name ::= [_:a-zA-Z] [_:a-zA-Z\\-.0-9]* (=[ \\t\\r\\n;>=/])

STRING ::= "\\"" [^<"]* "\\"" | "\\'" [^<"]* "\\'"
"""

input_str = """<RestaurantReservation>
  <reservationID>AH-158394</reservationID>
  <guestName>Alexander Hamilton</guestName>
  <reservationTime>2023-04-15T19:30:00Z</reservationTime>
  <specialRequests count="2">
    <request>Table by the window</request>
    <request>Surprise dessert for a special occasion</request>
  </specialRequests>
</RestaurantReservation>"""


def xgrammar_build(grammar_str: str, grammar_compiler: xgr.GrammarCompiler):
    compiled_grammar = grammar_compiler.compile_grammar(grammar_str)
    print(compiled_grammar.grammar)
    matcher = xgr.GrammarMatcher(compiled_grammar)
    return matcher


def xgrammar_exec(matcher: xgr.GrammarMatcher, bitmask: torch.Tensor, token_id: int):
    # Logits processing
    matcher.fill_next_token_bitmask(bitmask)
    assert matcher.accept_token(token_id, debug_print=False)
    return


# def outlines_build(schema: str, tokenizer: TransformerTokenizer):
#     schema_str = convert_json_schema_to_str(json_schema=schema)
#     regex_string = build_regex_from_schema(schema_str, whitespace_pattern=None)
#     guide = RegexGuide.from_regex(regex_string, tokenizer)
#     return guide


# def outlines_exec(guide: Guide, logits: torch.Tensor, token_id: int, state=None):
#     if state is None:
#         state = guide.initial_state
#     # Logits processing
#     allowed_tokens = guide.get_next_instruction(state).tokens
#     biased_logits = bias_logits(logits.view(1, -1), [allowed_tokens])
#     # Update state
#     next_state = guide.get_next_state(state, token_id)
# return next_state


def lmformatenforcer_build(schema: str, tokenizer: TokenEnforcerTokenizerData):
    parser = JsonSchemaParser(json.loads(schema))
    token_enforcer = TokenEnforcer(tokenizer, parser)
    return token_enforcer


def lmformatenforcer_exec(token_enforcer: TokenEnforcer, logits: torch.Tensor, token_ids):
    # Logits processing
    allowed_tokens = token_enforcer.get_allowed_tokens(token_ids)
    logits[allowed_tokens] = float("-inf")
    # Update state
    return


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--backend",
        type=str,
        choices=["xgrammar", "outlines", "lmformatenforcer"],
        default="xgrammar",
    )
    args = parser.parse_args()

    backend = args.backend

    hf_model_path = "meta-llama/Llama-3.1-8B-Instruct"

    hf_tokenizer = AutoTokenizer.from_pretrained(hf_model_path)
    xgrammar_tokenizer_info = xgr.TokenizerInfo.from_huggingface(hf_tokenizer)
    xgrammar_grammar_compiler = xgr.GrammarCompiler(xgrammar_tokenizer_info, max_threads=1)
    # outlines_tokenizer = TransformerTokenizer(hf_tokenizer)
    lmformatenforcer_tokenizer = build_token_enforcer_tokenizer_data(hf_tokenizer)

    vocab_size = len(hf_tokenizer)
    for iter in range(3):
        start = time.perf_counter()
        if backend == "xgrammar":
            worker = xgrammar_build(xml_grammar, xgrammar_grammar_compiler)
        # elif backend == "outlines":
        #     worker = outlines_build(schema, outlines_tokenizer)
        # elif backend == "lmformatenforcer":
        #     worker = lmformatenforcer_build(schema, lmformatenforcer_tokenizer)

        build_time = time.perf_counter() - start

        print(f"Grammar preprocessing time (ms): {build_time * 1e3:.4f}")

        input_ids = hf_tokenizer.encode(input_str, add_special_tokens=False)
        bitmask = xgr.allocate_token_bitmask(1, xgrammar_tokenizer_info.vocab_size)
        start = time.perf_counter()
        for idx, token_id in enumerate(input_ids):
            if backend == "xgrammar":
                xgrammar_exec(worker, bitmask, token_id)
            # elif backend == "outlines":
            #     if idx == 0:
            #         state = None
            #     state = outlines_exec(worker, logits[idx], token_id, state)
            # elif backend == "lmformatenforcer":
            #     lmformatenforcer_exec(worker, logits[idx], prompt_token_ids + token_ids[:idx])
        exec_time = time.perf_counter() - start

        print(f"Mask generation time (ms): {exec_time / len(input_ids) * 1e3:.4f}")
