/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "codegen/embedding_forward_split_cpu.h"

using namespace at;

Tensor split_embedding_codegen_forward_cpu(
    Tensor weights,
    Tensor weights_offsets,
    Tensor D_offsets,
    int64_t total_D,
    Tensor indices,
    Tensor offsets,
    int64_t pooling_mode,
    Tensor indice_weights) {
  int64_t T = D_offsets.numel() - 1;
  TORCH_CHECK(T > 0);
  // offsets = [T x B  + 1]
  int64_t B = (offsets.size(0) - 1) / T;
  TORCH_CHECK(B > 0);

  const auto D_offsets_data = D_offsets.accessor<int, 1>();
  const auto weights_offsets_data = weights_offsets.accessor<int64_t, 1>();
  const auto offsets_data = offsets.accessor<int64_t, 1>();
  const auto indices_data = indices.accessor<int64_t, 1>();

  Tensor output;
  if (weights.scalar_type() == at::kHalf) {
    output = zeros({B, total_D}, weights.options().dtype(at::kFloat));
  } else {
    output = zeros({B, total_D}, weights.options());
  }
  for (int64_t t = 0; t < T; ++t) {
    const auto D_begin = D_offsets_data[t];
    const auto D = D_offsets_data[t + 1] - D_offsets_data[t];
    const auto table_begin = weights_offsets_data[t];
    for (int64_t b = 0; b < B; ++b) {
      const auto pool_begin = offsets_data[t * B + b];
      const auto pool_end = offsets_data[t * B + b + 1];
      const auto L = pool_end - pool_begin;
      const double scale_factor =
          // NOTE: MEAN pooling will not work with indice_weights!
          (pooling_mode == MEAN && !indice_weights.defined() && L > 0) ? 1.0 / L
                                                                       : 1.0;
      for (auto p = pool_begin; p < pool_end; ++p) {
        const int64_t embedding_begin = table_begin + indices_data[p] * D;
        for (int64_t d = 0; d < D; ++d) {
          output[b][D_begin + d] += scale_factor *
              (indice_weights.defined()
                   ? weights[embedding_begin + d] * indice_weights[p]
                   : weights[embedding_begin + d]);
        }
      }
    }
  }
  return output;
}

Tensor split_embedding_codegen_grad_indice_weights_cpu(
    Tensor grad_output,
    Tensor weights,
    Tensor weights_offsets,
    Tensor D_offsets,
    Tensor indices,
    Tensor offsets,
    Tensor feature_requires_grad) {
  int64_t T = D_offsets.numel() - 1;
  TORCH_CHECK(T > 0);
  // offsets = [T x B  + 1]
  int64_t B = (offsets.size(0) - 1) / T;
  TORCH_CHECK(B > 0);

  const auto D_offsets_data = D_offsets.accessor<int, 1>();
  const auto weights_offsets_data = weights_offsets.accessor<int64_t, 1>();
  const auto offsets_data = offsets.accessor<int64_t, 1>();
  const auto indices_data = indices.accessor<int64_t, 1>();

  auto grad_indice_weights =
      zeros_like(indices, indices.options().dtype(grad_output.dtype()));
  for (int64_t t = 0; t < T; ++t) {
    if (feature_requires_grad.defined() &&
        !feature_requires_grad[t].is_nonzero()) {
      // NOTE: skip if the table does not require gradient computation!
      continue;
    }
    const auto D_begin = D_offsets_data[t];
    const auto D = D_offsets_data[t + 1] - D_offsets_data[t];
    const auto table_begin = weights_offsets_data[t];
    for (int64_t b = 0; b < B; ++b) {
      const auto pool_begin = offsets_data[t * B + b];
      const auto pool_end = offsets_data[t * B + b + 1];
      for (auto p = pool_begin; p < pool_end; ++p) {
        const int64_t embedding_begin = table_begin + indices_data[p] * D;
        for (int64_t d = 0; d < D; ++d) {
          grad_indice_weights[p] +=
              grad_output[b][D_begin + d] * weights[embedding_begin + d];
        }
      }
    }
  }
  return grad_indice_weights;
}
