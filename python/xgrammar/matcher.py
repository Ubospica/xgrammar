"""Match the output of the LLM to the specified grammar, then generate the mask for the next
token.
"""

from typing import List, Optional, Union

import torch

from .base import XGRObject, _core
from .bitmask import bitmask_dtype
from .compiler import CompiledGrammar


class GrammarMatcher(XGRObject):
    """Match the output of the LLM to the specified grammar, then generate the mask for the next
    token. This is the core class in the grammar-guided generation.

    This class maintains a stateful matcher that can accept tokens and strings, then match them
    to the specified grammar. The matcher can provide a bitmask for the next token prediction,
    so that the output of the LLM follows the specified grammar. Its state can be reset and
    rolled back by tokens. It also provides utilities for jump-forward decoding.

    After matching the whole grammar, the matcher will accept a stop token. The token mask at
    this time will only allow stop tokens. After accepting the stop token, the matcher will
    terminate, then it cannot accept any new token or generate a new token mask, meaning the
    generation is finished.

    Under the hood, it utilizes a pushdown automaton with backtracking to match the grammar,
    with optimizations specific to LLM token mask generation.

    Parameters
    ----------
    compiled_grammar : CompiledGrammar
        The initialization context for the grammar matcher.

    override_stop_tokens : Optional[Union[int, List[int]]], default: None
        If not None, the stop tokens to override the ones in the grammar.

    terminate_without_stop_token : bool, default: False
        Whether to terminate the matcher without accepting a stop token.

    max_rollback_tokens : int, default: 0
        The maximum number of rollback tokens allowed. The rollback operation is useful for
        jump-forward decoding and speculative decoding.
    """

    def __init__(
        self,
        compiled_grammar: CompiledGrammar,
        *,
        override_stop_tokens: Optional[Union[int, List[int]]] = None,
        terminate_without_stop_token: bool = False,
        max_rollback_tokens: int = 0,
    ) -> None:
        if not isinstance(compiled_grammar, CompiledGrammar):
            raise ValueError("The grammar should be compiled before passing it to GrammarMatcher.")

        if isinstance(override_stop_tokens, int):
            override_stop_tokens = [override_stop_tokens]

        self._init_handle(
            _core.GrammarMatcher(
                compiled_grammar._handle,
                override_stop_tokens,
                terminate_without_stop_token,
                max_rollback_tokens,
            )
        )

    def accept_token(self, token_id: int, *, debug_print: bool = False) -> bool:
        """Accept one token and update the state of the matcher.

        In the following cases, the matcher will not accept the token and return False:

        1. The token does not match the grammar.
        2. The matcher has terminated after accepting the stop token, but is trying to accept a
           new token.
        3. The token id is out of range.
        4. The token is a special token.

        The user should capture the return value and handle the cases where the token is not
        accepted.

        Parameters
        ----------
        token_id : int
            The id of the token to accept.

        debug_print : bool, default: False
            Whether to print information about the internal state of the matcher. Helpful
            for debugging.

        Returns
        -------
        accepted : bool
            Whether the token is accepted.

        Raises
        ------
        RuntimeError
            If the recursion depth is exceeded.
        """
        return self._handle.accept_token(token_id, debug_print)

    def accept_string(self, input_str: Union[str, bytes], *, debug_print: bool = False) -> bool:
        """Accept a string and update the state of the matcher. The whole string is considered
        as one step in rollback. It is used to complement the functionality of accept_token, and
        accept_token should always be used to accept tokens.

        Parameters
        ----------
        input_str : Union[str, bytes]
            The string to be accepted.

        debug_print : bool, default: False
            Whether to print information about the internal state of the matcher. Helpful for
            debugging.

        Returns
        -------
        accepted : bool
            Whether the string is accepted.

        Raises
        ------
        RuntimeError
            If the recursion depth is exceeded.
        """
        return self._handle.accept_string(input_str, debug_print)

    def fill_next_token_bitmask(
        self, bitmask: torch.Tensor, index: int = 0, *, debug_print: bool = False
    ) -> bool:
        """Fill the bitmask for the next token prediction. The input bitmask can be generated
        by allocate_token_bitmask, and must be on CPU. bitmask[index] will be filled with the
        next token bitmask.

        This method does not change the matcher state.

        Parameters
        ----------
        bitmask : torch.Tensor
            The bitmask for the next token prediction.

        index : int, default: 0
            The batch id of the bitmask.

        debug_print : bool, default: False
            Whether to print information about generated bitmask. Helpful for debugging.

        Returns
        -------
        need_apply : bool
            Whether the bitmask need to be applied (not all-true). An optimization: if False,
            this means the bitmask is already all-true, so no need to apply it.

        Raises
        ------
        RuntimeError
            If the recursion depth is exceeded.
        """
        if bitmask.device.type != "cpu":
            raise ValueError("bitmask should be on CPU.")
        if bitmask.dtype != bitmask_dtype:
            raise ValueError(f"bitmask should be of type {bitmask_dtype}.")
        return self._handle.fill_next_token_bitmask(
            bitmask.data_ptr(), list(bitmask.shape), index, debug_print
        )

    def find_jump_forward_string(self) -> str:
        """Find the jump-forward string for jump-forward decoding. This is the longest string that
        certainly conforms with the current grammar from the current matcher state. This string
        can become the output of the LLM without requiring LLM decoding.

        This method does not change the matcher state.

        Returns
        -------
        jump_forward_string : str
            The jump-forward string.

        Raises
        ------
        RuntimeError
            If the recursion depth is exceeded.
        """
        return self._handle.find_jump_forward_string()

    def rollback(self, num_tokens: int = 1) -> None:
        """Rollback the matcher to a previous state by several tokens.

        Parameters
        ----------
        num_tokens : int, default: 1
            The number of tokens to rollback. It cannot exceed the current number of steps, nor can
            it exceed the specified maximum number of rollback tokens.
        """
        self._handle.rollback(num_tokens)

    def is_terminated(self) -> bool:
        """Check if the matcher has terminated. If terminate_without_stop_token is False, the
        matcher will terminate if it has accepted the stop token. Otherwise, the matcher will
        terminate after matching the whole grammar.

        Returns
        -------
        terminated : bool
            Whether the matcher has terminated.
        """
        return self._handle.is_terminated()

    def reset(self) -> None:
        """Reset the matcher to the initial state."""
        return self._handle.reset()

    @property
    def max_rollback_tokens(self) -> int:
        """Get the maximum number of rollback tokens allowed.

        Returns
        -------
        max_rollback_tokens : int
            The maximum number of rollback tokens.
        """
        return self._handle.max_rollback_tokens

    @property
    def stop_token_ids(self) -> List[int]:
        """The ids of the stop tokens used in the matcher. If specified, the provided stop tokens
        will be used. Otherwise, the stop tokens will be detected from the vocabulary.

        Returns
        -------
        stop_token_ids : List[int]
            The ids of the stop tokens.
        """
        return self._handle.stop_token_ids

    def _debug_print_internal_state(self) -> str:
        """Print the internal state of the matcher. This is used for debugging. The
        representation of the internal state is subject to change.

        Returns
        -------
        internal_state : str
            The internal state of the matcher.
        """
        return self._handle._debug_print_internal_state()
