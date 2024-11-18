"""This script benchmarks the time for grammar compilation and mask
generation for unconstrained JSON."""

import argparse
import json
import time

import datasets
import torch
from outlines import disable_cache
from outlines.fsm.guide import CFGGuide, Guide
from outlines.generate.generator import bias_logits
from outlines.models.transformers import TransformerTokenizer
from tqdm import tqdm
from transformers import AutoTokenizer

from xgrammar import (
    BuiltinGrammar,
    CachedGrammarCompiler,
    CompiledGrammar,
    GrammarMatcher,
    TokenizerInfo,
)

JSON_GRAMMAR_LARK = r"""
?start: object | array

?value: object
| array
| UNESCAPED_STRING
| SIGNED_NUMBER      -> number
| "true"             -> true
| "false"            -> false
| "null"             -> null

array  : "[" [value ("," value)*] "]"
object : "{" [pair ("," pair)*] "}"
pair   : UNESCAPED_STRING ":" value

%import common.UNESCAPED_STRING
%import common.SIGNED_NUMBER
%import common.WS

%ignore WS
"""


def xgrammar_build(tokenizer_info: TokenizerInfo):
    json_grammar = BuiltinGrammar.json()
    compiled_grammar = CompiledGrammar(json_grammar, tokenizer_info)
    return GrammarMatcher(compiled_grammar)


def xgrammar_exec(
    matcher: GrammarMatcher, logits: torch.Tensor, bitmask: torch.Tensor, token_id: int
):
    # Logits processing
    matcher.fill_next_token_bitmask(bitmask)
    matcher.apply_token_bitmask_inplace(logits, bitmask)
    # Update state
    assert matcher.accept_token(token_id)
    return


def outlines_build(tokenizer: TransformerTokenizer):
    guide = CFGGuide(JSON_GRAMMAR_LARK, tokenizer)
    return guide


def outlines_exec(guide: Guide, logits: torch.Tensor, token_id: int, state=None):
    if state is None:
        state = guide.initial_state
    # Logits processing
    allowed_tokens = guide.get_next_instruction(state).tokens
    biased_logits = bias_logits(logits.view(1, -1), [allowed_tokens])
    # Update state
    next_state = guide.get_next_state(state, token_id)
    return next_state


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--backend",
        type=str,
        choices=["xgrammar", "outlines", "outlines_new"],
        default="xgrammar",
    )
    parser.add_argument("--num_iters", type=int, default=5)
    parser.add_argument("--num_warmup", type=int, default=-1)
    args = parser.parse_args()

    backend = args.backend
    num_iters = args.num_iters
    num_warmup = args.num_warmup if args.num_warmup != -1 else 5 if num_iters >= 40 else 1

    dataset = datasets.load_dataset("NousResearch/json-mode-eval", split="train")

    hf_model_path = "meta-llama/Llama-3.1-8B-Instruct"

    hf_tokenizer = AutoTokenizer.from_pretrained(hf_model_path)
    xgrammar_tokenizer_info = TokenizerInfo.from_huggingface(hf_tokenizer)
    grammar_compiler = CachedGrammarCompiler(xgrammar_tokenizer_info)
    outlines_tokenizer = TransformerTokenizer(hf_tokenizer)

    vocab_size = len(hf_tokenizer)

    build_time = 0
    exec_time = 0
    total_data_points = 0
    total_tokens = 0
    fail_cnt = 0

    if backend == "outlines" or backend == "outlines_new":
        disable_cache()

    tqdm_iter = tqdm(range(-num_warmup, num_iters))
    for iter in tqdm_iter:
        if iter < 0:
            tqdm_iter.set_description(f"Backend: {backend}, Warmup Iter: {iter + num_warmup}")
        else:
            tqdm_iter.set_description(f"Backend: {backend}, Iter: {iter}")

        if iter == 0:
            # Reset time
            build_time = 0
            exec_time = 0

        tqdm_data_point_iter = tqdm(range(len(dataset)))
        for data_point_idx in tqdm_data_point_iter:
            tqdm_data_point_iter.set_description(
                f"Backend: {backend}, Data Point: {data_point_idx}"
            )

            schema = dataset["schema"][data_point_idx]
            completion = dataset["completion"][data_point_idx]
            token_ids = hf_tokenizer.encode(completion, add_special_tokens=False)
            prompt = hf_tokenizer.apply_chat_template(
                dataset["prompt"][data_point_idx], tokenize=False
            )
            prompt_token_ids = hf_tokenizer.encode(prompt)
            # print(f"Prompt: {prompt}, Schema: {schema}")

            start = time.perf_counter()
            try:
                if backend == "xgrammar":
                    worker = xgrammar_build(xgrammar_tokenizer_info)
                    bitmask = GrammarMatcher.allocate_token_bitmask(worker.vocab_size)
                elif backend == "outlines" or backend == "outlines_new":
                    worker = outlines_build(outlines_tokenizer)
            except Exception as e:
                raise e
                if iter >= 0:
                    fail_cnt += 1
                continue

            build_time += time.perf_counter() - start
            print(f"Build time: {time.perf_counter() - start:.4f}s")

            # use different logits for each mask generation process
            # to avoid caching effects between different tokens
            logits = [torch.randn(vocab_size).cuda() for _ in range(len(token_ids))]

            torch.cuda.synchronize()
            start = time.perf_counter()
            fail_flag = False
            print("num tokens: ", len(token_ids))
            for idx, token_id in enumerate(token_ids):
                # Logits processing
                # start = time.perf_counter()
                try:
                    if backend == "xgrammar":
                        print(f"string: {hf_tokenizer.decode(token_ids[:idx+1])}")
                        xgrammar_exec(worker, logits[idx], bitmask, token_id)
                    elif backend == "outlines" or backend == "outlines_new":
                        if idx == 0:
                            state = None
                        state = outlines_exec(worker, logits[idx], token_id, state)
                    # end = time.perf_counter()
                    # print(f"Exec time: {end - start:.4f}s")
                except Exception as e:
                    raise e
                    if iter >= 0:
                        fail_cnt += 1
                    fail_flag = True
                    break

            if fail_flag:
                continue

            torch.cuda.synchronize()
            exec_time += time.perf_counter() - start

            if iter >= 0:
                total_data_points += 1
                total_tokens += len(token_ids)

    print(f"Backend: {backend}")
    print(f"Fail count: {fail_cnt / num_iters:.0f} / {len(dataset)}")
    print(f"Grammar preprocessing time (ms): {build_time/total_data_points * 1e3:.4f}")
    print(f"Mask generation time (us/token): {exec_time/total_tokens * 1e6:.4f}")
