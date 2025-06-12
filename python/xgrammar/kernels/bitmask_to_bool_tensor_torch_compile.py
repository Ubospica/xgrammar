"""Kernels for bitmask to bool tensor and bool tensor to bitmask. Implemented in torch.compile."""

import torch


@torch.compile(dynamic=True)
def bitmask_to_bool_tensor_kernel(bitmask: torch.Tensor, vocab_size: int) -> torch.Tensor:
    """Convert the bitmask to a boolean tensor. The bitmask is a 01 bitwise compressed tensor,
    where 0 means the token is masked and 1 means the token is not masked. The boolean tensor is
    the same shape as the bitmask, where True means the token is not masked and False means the token
    is masked.
    """
    mask_expanded = torch.repeat_interleave(bitmask, 32, dim=-1)
    # bit_indices: (32 * bitmask_size,)
    bit_indices = torch.arange(32, device=bitmask.device, dtype=torch.int32).repeat(
        bitmask.shape[-1]
    )
    # bit_masks: (batch_size, 32 * bitmask_size)
    bit_masks = (mask_expanded >> bit_indices) & 1
    bit_masks = bit_masks[..., :vocab_size]
    return bit_masks == 1


@torch.compile(dynamic=True)
def bool_tensor_to_bitmask_kernel(bool_tensor: torch.Tensor) -> torch.Tensor:
    """Convert the boolean tensor to a bitmask. The boolean tensor is a boolean tensor, where True
    means the token is not masked and False means the token is masked. The bitmask is a 01 bitwise
    compressed tensor, where 0 means the token is masked and 1 means the token is not masked.
    """
    vocab_size = bool_tensor.shape[-1]
    num_blocks = (vocab_size + 31) // 32

    pad_len = num_blocks * 32 - vocab_size
    if pad_len > 0:
        bool_tensor = torch.nn.functional.pad(bool_tensor, (0, pad_len), value=False)

    bit_weights = 1 << torch.arange(32, device=bool_tensor.device, dtype=torch.int32)

    bool_tensor = bool_tensor.view(*bool_tensor.shape[:-1], num_blocks, 32)
    bitmask = torch.sum(bool_tensor.to(torch.int32) * bit_weights, dim=-1).to(torch.int32)
    return bitmask
