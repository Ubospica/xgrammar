.. _how-to-engine-integration:

Integration with LLM Engine
===========================

XGrammar enables efficient structured generation. In this tutorial, we go over the key components
of XGrammar and how to integrate XGrammar into an LLM engine.

We first lay out the concepts in :ref:`High-Level Flow <how-to-engine-integration-flow>`.
We then demonstrate how XGrammar enables
:ref:`Structured Generation for Batched Inference <how-to-engine-integration-batched>`.

The code snippets below are actual runnable code as we simulate the LLM generation.


Install XGrammar
----------------

:ref:`XGrammar <installation_prebuilt_package>` is available via pip.
It is always recommended to install it in an isolated conda virtual environment.


.. _how-to-engine-integration-flow:

High-Level Flow
---------------

In this section, we go over the key components of XGrammar when integrating it into an LLM engine
for structured generation.

First, import necessary libraries for the tutorial.

.. code:: python

  import xgrammar as xgr
  import torch
  import numpy as np
  from transformers import AutoTokenizer, AutoConfig

xgr.TokenizerInfo
^^^^^^^^^^^^^^^^^

``xgr.TokenizerInfo`` is a per-model construct that encapsulates tokenizer information, including
all its vocabulary. There are several ways of instantiating it, and the most convenient way
is using an ``AutoTokenizer``. Note that for some models, ``AutoConfig.vocab_size`` can be larger
than ``AutoTokenizer.vocab_size`` due to paddings, with the former being the shape of the model's
logits. To be safe, always pass in the former when instantiating ``xgr.TokenizerInfo``.

.. code:: python

  # Get tokenizer info
  model_id = "meta-llama/Llama-3.2-1B-Instruct"
  tokenizer = AutoTokenizer.from_pretrained(model_id)
  config = AutoConfig.from_pretrained(model_id)
  # This can be larger than tokenizer.vocab_size due to paddings
  full_vocab_size = config.vocab_size
  tokenizer_info = xgr.TokenizerInfo.from_huggingface(tokenizer, vocab_size=full_vocab_size)


xgr.GrammarCompiler
^^^^^^^^^^^^^^^^^^^

With an ``xgr.TokenizerInfo``, we can instantiate an ``xgr.GrammarCompiler``. This is a construct
that compiles a grammar according to the model's tokenizer info. Therefore, for each model, you
can use the same ``xgr.GrammarCompiler`` persistently, as it can compile different grammars for
the same ``xgr.TokenizerInfo``. Note that the ``compiler`` behavior can be configured with
``max_threads`` for multithreading, and ``enable_cache`` (defaults to true) for caching
compiled grammars.

.. code:: python

  compiler = xgr.GrammarCompiler(tokenizer_info, max_threads=8)


xgr.CompiledGrammar
^^^^^^^^^^^^^^^^^^^

Then, using the ``xgr.GrammarCompiler``, we can compile a grammar, with the result being an
``xgr.CompiledGrammar``. Here we use a built-in JSON grammar. For other grammars, see
:ref:`how-to-json-generation` and :ref:`how-to-ebnf-generation`.
Every thing we have seen up to now are per-model (rather than per-generation).

.. code:: python

  compiled_grammar: xgr.CompiledGrammar = compiler.compile_builtin_json_grammar()

xgr.GrammarMatcher
^^^^^^^^^^^^^^^^^^

With the compiled grammar, we can instantiate a ``xgr.GrammarMatcher``. It is the main construct
an LLM engine interacts with that maintains the state of the structured generation. Note that
each request should have its own ``xgr.GrammarMatcher`` since each has a different generation state,
as we will see in :ref:`how-to-engine-integration-batched`.

.. code:: python

  # Instantiate grammar matcher with the compiled grammar
  matcher = xgr.GrammarMatcher(compiled_grammar)

Bitmasking Logits in Auto-regressive Generation
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Now we simulate a single-request auto-regressive generation. See later section for
:ref:`how-to-engine-integration-batched`.

First, we pre-allocate a token bitmask with ``xgr.allocate_token_bitmask()``,
which is essentially a ``torch.Tensor`` of shape ``(batch_size, vocab_size)``. You can also
use your own implementation for allocating a bitmask.

In each auto-regressive step, we fill the token bitmask according to the current state
of the matcher with ``xgr.GrammarMatcher.fill_next_token_bitmask()``. Then, we apply the bitmask
into the model's logits with ``xgr.apply_token_bitmask_inplace()``, which calls a CUDA kernel
if ``logits`` is on CUDA (recommended), otherwise a CPU implementation.

After masking, the logits for illegal tokens are set to negative infinity, so that
we will never sample them. After sampling the token, update the ``xgr.GrammarMatcher``'s state with
``xgr.GrammarMatcher.accept_token()``. Finally, use  ``xgr.GrammarMatcher.reset()`` to prepare
for the next generation.

.. code:: python

  # Here we simulate a valid sampled response
  sim_sampled_response = '{ "library": "xgrammar" }<|end_of_text|>'
  sim_sampled_token_ids = tokenizer.encode(sim_sampled_response, add_special_tokens=False)

  # Allocate a token bitmask
  token_bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)

  # Each loop iteration is a simulated auto-regressive step
  for i, sim_token_id in enumerate(sim_sampled_token_ids):
      # LLM inference to get logits, here we use randn to simulate.
      # logits is a tensor of shape (full_vocab_size,) on GPU
      # logits = LLM.inference()
      logits = torch.randn(full_vocab_size).cuda()

      # Apply bitmask to logits to mask invalid tokens
      matcher.fill_next_token_bitmask(token_bitmask)
      xgr.apply_token_bitmask_inplace(logits, token_bitmask.to(logits.device))

      # Sample next token
      probs = torch.softmax(logits, dim=-1).cpu().numpy()
      next_token_id = np.random.choice(list(range(full_vocab_size)), p=probs)

      # Accept token from matcher to update its state, so that the next bitmask
      # generated will enforce the next token to be generated. Assert to make
      # sure the token is indeed valid. Here we accept the simulated response
      # assert matcher.accept_token(next_token_id)
      assert matcher.accept_token(sim_token_id)

  # Since we accepted a stop token `<|end_of_text|>`, we have terminated
  assert matcher.is_terminated()

  # Reset to be ready for the next auto-regressive generation
  matcher.reset()


.. _how-to-engine-integration-batched:

Structured Generation for Batched Inference
-------------------------------------------

The code snippets above assume a single request generation.
This section demonstrates how the same concept works with batched generation.

First, follow the exact same steps above for the per-model constructs
``xgr.TokenizerInfo`` and ``xgr.GrammarCompiler``. Say each request needs
to generate a valid JSON.

.. code:: python

  import xgrammar as xgr
  import torch
  import numpy as np
  from transformers import AutoTokenizer, AutoConfig

  # Get tokenizer info
  model_id = "meta-llama/Llama-3.2-1B-Instruct"
  tokenizer = AutoTokenizer.from_pretrained(model_id)
  config = AutoConfig.from_pretrained(model_id)
  # This can be larger than tokenizer.vocab_size due to paddings
  full_vocab_size = config.vocab_size
  tokenizer_info = xgr.TokenizerInfo.from_huggingface(tokenizer, vocab_size=full_vocab_size)

  # Compile a JSON grammar
  compiler = xgr.GrammarCompiler(tokenizer_info, max_threads=8)
  compiled_grammar: xgr.CompiledGrammar = compiler.compile_builtin_json_grammar()

Now, we need to maintain an ``xgr.GrammarMatcher`` for each request in the batch, since
each has a different generation state. Note that each request in the batch can follow a different
``xgr.CompiledGrammar``, but here for simplicity, they are all just following the general
JSON grammar.

.. code:: python

  batch_size = 2
  matchers = [
      xgr.GrammarMatcher(compiled_grammar)
      for i in range(batch_size)
  ]
  token_bitmask = xgr.allocate_token_bitmask(batch_size, tokenizer_info.vocab_size)

We simulate an auto-regressive generation of batched inference. Note that here we
assume the generation lengths of the two requests are the same for simplicity. But
it should be easy to generalize based on how your engine supports batched inference.
The key difference from single-request generation is that, in batched-request generation,
each request has its own ``xgr.GrammarMatcher`` to maintain.

.. code:: python

  sim_sampled_responses = ['{"name": "a"}<|end_of_text|>', '{"name": "b"}<|end_of_text|>']
  sim_sampled_token_ids = [
    tokenizer.encode(response, add_special_tokens=False)
    for response in sim_sampled_responses
  ]

  # Each loop iteration is a simulated auto-regressive step
  for loop_iter in range(len(sim_sampled_token_ids[0])):
      # LLM batched inference to get logits, here we use randn to simulate
      # Now, logits is a tensor of shape (batch_size, full_vocab_size) on GPU
      # logits = LLM.inference()
      logits = torch.randn(batch_size, full_vocab_size).cuda()

      # This for loop is parallelizable using threading.Thread. But estimate
      # the overhead in your engine.
      for i in range(batch_size):
          matchers[i].fill_next_token_bitmask(token_bitmask, i)
      xgr.apply_token_bitmask_inplace(logits, token_bitmask.to(logits.device))

      # Sample next token
      probs = torch.softmax(logits, dim=-1).cpu().numpy()
      next_token_ids = [
          np.random.choice(list(range(full_vocab_size)), p=probs[i])
          for i in range(batch_size)
      ]

      # Update the matcher for each request
      for i in range(batch_size):
          # Here we accept the simulated response
          # assert matchers[i].accept_token(next_token_ids[i])
          matchers[i].accept_token(sim_sampled_token_ids[i][loop_iter])

  # In our simulated case, all requests should have terminated since we accepted
  # a stop token `<|end_of_text|>`
  for i in range(batch_size):
      assert matchers[i].is_terminated()
      # Reset to be ready for the next generation
      matchers[i].reset()

Tool-calling with Structural Tag
-------------------------------------------

In this section, we see how to use XGrammar in an LLM engine to ensure that the output always follows the expected tool-calling format. All code snippets below are actual runnable code as we simulate the LLM generation.

We leverage the Structural Tag to ensure valid tool call. The structural tag handles the dispatching of different grammars based on the tags and triggers: it initially allows any output, until a trigger is encountered, then dispatch to the corresponding tag; when the end tag is encountered, the grammar will allow any following output, until the next trigger is encountered.

It's useful for LLM tool-calling since it provides a flexible yet precise way to control the output with a wide range of exact tool-calling formats. For example, if the tool-calling format is <function=function_name>json_parameters</function>, the Structural Tag can be as:

* Each tag pattern consists of three parts: a begin tag ("<function=func_name>"), a parameter list according to some schema ({"arg1": …, "arg2": …}), and an end tag ("</function>");

* The trigger pattern is [“<function=”].

Note that the Structural Tag is a functional superset of OpenAI’s function calling, supporting features like tool_choice, parallel function calling, and strict mode. Therefore, the Structural Tag can also be used to enable strict-mode function calling.

In the following content, we will demonstrate the specific implementation of applying Structural Tag to tool-calling in LLM engines. First, we still follow the exact same steps above for the per-model constructs ``xgr.TokenizerInfo``.

.. code:: python

    import xgrammar as xgr
    import torch
    import numpy as np
    from transformers import AutoTokenizer, AutoConfig

    # Get tokenizer info
    model_id = "meta-llama/Llama-3.2-1B-Instruct"
    tokenizer = AutoTokenizer.from_pretrained(model_id)
    config = AutoConfig.from_pretrained(model_id)
    # This can be larger than tokenizer.vocab_size due to paddings
    full_vocab_size = config.vocab_size
    tokenizer_info = xgr.TokenizerInfo.from_huggingface(tokenizer, vocab_size=full_vocab_size)

Than we compile the grammar from structural tags to support structured function calling. Here we take tool-calling format <function=function_name>json_parameters</function> for example, which is preferred by Llama3 models.

.. code:: python

    # define tools
    tools = [
      {
          "type": "function",
          "function": {
              "name": "get_current_weather",
              "description": "Get the current weather in a given location",
              "parameters": {
                  "type": "object",
                  "properties": {
                      "location": {
                          "type": "string",
                          "description": "The city and state, e.g. San Francisco, CA",
                      },
                      "unit": {"type": "string", "enum": ["celsius", "fahrenheit"]},
                  },
                  "required": ["location"],
              },
          },
      }
    ]

    # define structural tag
    # take the tool-calling format '<function=function_name>json_parameters</function>' for example
    tags = [
      xgr.StructuralTagItem(
          begin=f"<function={tool['function']['name']}>",
          schema=json.dumps(tool['function']['parameters']),
          end="</function>",
      )
      for tool in tools
    ]
    triggers = ["<function="]
    stag: xgr.Grammar = xgr.StructuralTag(tags=tags, triggers=triggers, tool_choice="auto")

    # compile from Structural Tag
    compiler = xgr.GrammarCompiler(tokenizer_info, max_threads=8)
    compiled_grammar = compiler.compile_structural_tag(stag)

We still simulate an auto-regressive generation of batched inference. Note that when the trigger is not encountered, the mask will likely be all-1 and not have to be used (``fill_next_token_bitmask`` returns False, meaning no token is masked), and when a trigger is encountered, the mask should be enforced (``fill_next_token_bitmask`` will return True, meaning some token is masked) to the output logits. This method optimizes performance by avoiding unnecessary masking when no trigger is encountered.

.. code:: python

    batch_size = 2
    matchers = [xgr.GrammarMatcher(compiled_grammar) for _ in range(batch_size)]
    token_bitmask = xgr.allocate_token_bitmask(batch_size, tokenizer_info.vocab_size)

    sim_sampled_responses = [
        '<function=get_current_weather>{"location": "Pittsburgh, PA", "unit": "fahrenheit"}</function><|end_of_text|>',
        'I need to call the tool as <function=get_current_weather>{"location": "San Francisco, CA"}</function>.<|end_of_text|>',
    ]
    sim_sampled_token_ids = [
        tokenizer.encode(response, add_special_tokens=False)
        for response in sim_sampled_responses
    ]


    # Each loop iteration is a simulated auto-regressive step
    no_terminate_incides = set(range(batch_size))
    for loop_iter in range(max([len(sub_token_ids) for sub_token_ids in sim_sampled_token_ids])):
        # LLM batched inference to get logits, here we use randn to simulate
        # Now, logits is a tensor of shape (batch_size, full_vocab_size) on GPU
        # logits = LLM.inference()
        logits = torch.randn(batch_size, full_vocab_size).cuda()

        # This for loop is parallelizable using threading.Thread. But estimate
        # the overhead in your engine.
        indices = []
        for i in no_terminate_incides:
            need_apply = matchers[i].fill_next_token_bitmask(token_bitmask, i)
            if need_apply:
                indices.append(i)
        xgr.apply_token_bitmask_inplace(
            logits=logits,
            token_bitmask=token_bitmask.to(logits.device),
            indices=indices if len(indices) > 0 else None,
        )

        # Sample next token
        probs = torch.softmax(logits, dim=-1).cpu().numpy()
        next_token_ids = [
            np.random.choice(list(range(full_vocab_size)), p=probs[i])
            for i in no_terminate_incides
        ]

        # Update the matcher for each request
        for i in no_terminate_incides:
            # Here we accept the simulated response
            # assert matchers[i].accept_token(next_token_ids[i])
            assert matchers[i].accept_token(sim_sampled_token_ids[i][loop_iter])

        # handle the situation when some requests are terminated
        for i in range(batch_size):
            if i in no_terminate_incides and matchers[i].is_terminated():
                no_terminate_incides.remove(i)


    # In our simulated case, all requests should have terminated since we accepted
    # a stop token `<|end_of_text|>`
    for i in range(batch_size):
        assert matchers[i].is_terminated()
        # Reset to be ready for the next generation
        matchers[i].reset()
