/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018-2024 The LCZero Authors
  Copyright (C) 2023 Intel Corporation

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
   
  SPDX-License-Identifier:GNU General Public License v3.0 or later
*/

#define DPCT_COMPAT_RT_VERSION 12020

#include <sycl/sycl.hpp>
#include "dpct/dpct.hpp"
#include <algorithm>
#include <cassert>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <cctype>
#include <string>

#include "sycl_common.h"
#include "inputs_outputs.h"
#include "kernels.h"
#include "layers.h"
#include "neural/factory.h"
#include "neural/network_legacy.h"
#include "neural/tables/attention_policy_map.h"
#include "neural/tables/policy_map.h"
#include "utils/bititer.h"
#include "utils/exception.h"
#include <cmath>

namespace lczero {
using namespace sycldnn_backend;

template <typename DataType>
class SyclNetwork;

static size_t getMaxAttentionHeadSize(
    const MultiHeadWeights::PolicyHead& weights, int N) {
  const size_t embedding_op_size = weights.ip_pol_b.size();
  const size_t policy_d_model = weights.ip2_pol_b.size();
  assert(policy_d_model == weights.ip3_pol_b.size());

  size_t encoder_d_model = 0;
  size_t encoder_dff = 0;

  if (weights.pol_encoder.size() > 0) {
    encoder_d_model = weights.pol_encoder[0].mha.q_b.size();
    encoder_dff = weights.pol_encoder[0].ffn.dense1_b.size();

    assert(encoder_d_model == weights.pol_encoder[0].mha.k_b.size());
    assert(encoder_d_model == weights.pol_encoder[0].mha.v_b.size());
    assert(embedding_op_size == weights.pol_encoder[0].ffn.dense2_b.size());
  }

  const size_t encoder_heads = weights.pol_encoder_head_count;

  size_t size =
      N * 64 *
      std::max(std::max(embedding_op_size, encoder_dff), policy_d_model);

  // size of matmul_qk matrix = encoder_heads_ * Batch * 64 * 64
  const size_t matmul_qk_size = encoder_heads * N * 64 * 64;
  const size_t output_size = N * (64 * 64 + 8 * 24);
  size = std::max(size, std::max(matmul_qk_size, output_size));

  size_t qkv_size = N * 64 * encoder_d_model;
  // We store qkv in single allocation, and other intermediate tensors are
  // sometimes stored by splitting an allocation into two halves.
  size = std::max(2 * size, 3 * qkv_size);
  return size;
}

static size_t getMaxAttentionBodySize(const MultiHeadWeights& weights, int N) {
  const size_t embedding_op_size = weights.ip_emb_b.size();

  size_t encoder_d_model = 0;
  size_t encoder_dff = 0;

  if (weights.encoder.size() > 0) {
    encoder_d_model = weights.encoder[0].mha.q_b.size();
    encoder_dff = weights.encoder[0].ffn.dense1_b.size();

    assert(encoder_d_model == weights.encoder[0].mha.k_b.size());
    assert(encoder_d_model == weights.encoder[0].mha.v_b.size());
    assert(embedding_op_size == weights.encoder[0].ffn.dense2_b.size());
  }

  const size_t encoder_heads = weights.encoder_head_count;

  size_t size =
      N * 64 *
      std::max(std::max(embedding_op_size, encoder_dff), encoder_d_model);

  // size of matmul_qk matrix = encoder_heads_ * Batch * 64 * 64
  const size_t matmul_qk_size = encoder_heads * N * 64 * 64;
  const size_t output_size = N * (64 * 64 + 8 * 24);
  size = std::max(size, std::max(matmul_qk_size, output_size));

  size_t qkv_size = N * 64 * encoder_d_model;
  // We store qkv in single allocation, and other intermediate tensors are
  // sometimes stored by splitting an allocation into two halves.
  size = std::max(2 * size, 3 * qkv_size);
  return size;
}

template <typename DataType>
class SyclNetworkComputation : public NetworkComputation {
 public:
  SyclNetworkComputation(SyclNetwork<DataType>* network, bool wdl,
                         bool moves_left);
  ~SyclNetworkComputation();

  void AddInput(InputPlanes&& input) override {
    const auto iter_mask =
        &inputs_outputs_->input_masks_mem_shared_[batch_size_ * kInputPlanes];
    const auto iter_val =
        &inputs_outputs_->input_val_mem_shared_[batch_size_ * kInputPlanes];

    int i = 0;
    for (const auto& plane : input) {
      iter_mask[i] = plane.mask;
      iter_val[i] = plane.value;
      i++;
    }

    batch_size_++;
  }

  void ComputeBlocking() override;

  int GetBatchSize() const override { return batch_size_; }

  float GetQVal(int sample) const override {
    if (wdl_) {
      auto w = inputs_outputs_->op_value_mem_shared_[3 * sample + 0];
      auto l = inputs_outputs_->op_value_mem_shared_[3 * sample + 2];
      return w - l;
    }
    return inputs_outputs_->op_value_mem_shared_[sample];
  }

  float GetDVal(int sample) const override {
    if (wdl_) {
      auto d = inputs_outputs_->op_value_mem_shared_[3 * sample + 1];
      return d;
    }
    return 0.0f;
  }

  float GetPVal(int sample, int move_id) const override {
    return inputs_outputs_->op_policy_mem_[sample * kNumOutputPolicy + move_id];
  }

  float GetMVal(int sample) const override {
    if (moves_left_) {
      return inputs_outputs_->op_moves_left_mem_shared_[sample];
    }
    return 0.0f;
  }

 private:
  // Memory holding inputs, outputs.
  std::unique_ptr<InputsOutputs> inputs_outputs_;
  int batch_size_;
  bool wdl_;
  bool moves_left_;

  SyclNetwork<DataType>* network_;
};

template <typename DataType>
class SyclNetwork : public Network {
 public:
  SyclNetwork(const WeightsFile& file, const OptionsDict& options)
      : capabilities_{file.format().network_format().input(),
                      file.format().network_format().output(),
                      file.format().network_format().moves_left()} {
    MultiHeadWeights weights(file.weights());
    gpu_id_ = options.GetOrDefault<int>("gpu", 0);

    const auto nf = file.format().network_format();
    using NF = pblczero::NetworkFormat;
    conv_policy_ = nf.policy() == NF::POLICY_CONVOLUTION;
    attn_policy_ = nf.policy() == NF::POLICY_ATTENTION;
    attn_body_ = nf.network() == NF::NETWORK_ATTENTIONBODY_WITH_HEADFORMAT ||
                 nf.network() == NF::NETWORK_ATTENTIONBODY_WITH_MULTIHEADFORMAT;

    max_batch_size_ = options.GetOrDefault<int>("max_batch", 1024);
    std::string device_plat_ = options.GetOrDefault<std::string>("platform", "OpenCL");
    // Convert device_plat_ to lowercase
    std::string device_plat_lower = to_lower(device_plat_);

    // Get all the available platforms
    auto platforms = sycl::platform::get_platforms();

    // Iterate over all platforms and add devices.
    for (const auto& platform : platforms) {
        // Get the platform name and convert it to lowercase
        std::string platform_name_lower = to_lower(platform.get_info<sycl::info::platform::name>());
        if (platform_name_lower.find(device_plat_lower) != std::string::npos) {
            auto platform_devices = platform.get_devices();
            devices.insert(devices.end(), platform_devices.begin(), platform_devices.end());
        }
    }
    
    // Count the GPU's.
    for (const auto& device : devices) {
        if (device.is_gpu()) { total_gpus_++; }
    }

    if (gpu_id_ >= (int)devices.size())
      throw Exception("Invalid GPU Id: " + std::to_string(gpu_id_));
  
      // Get the sycl device.
    device_ = devices[gpu_id_];
      
    // Get the number of compute units(execution units).
    compute_units_ = device_.get_info<sycl::info::device::max_compute_units>();
    
    // Get context.
    sycl::context context{device_};
  
    
    auto exceptions_handler = [&] (sycl::exception_list exceptions) {
        for (std::exception_ptr const& e : exceptions) {
           try {
               std::rethrow_exception(e);
            } catch(sycl::exception const& e) {
				std::cout 
                << "Caught asynchronous SYCL exception during GEMM:\n" 
                << e.what() 
                << std::endl;
                std::terminate();
            }
        }
    };
    
    sycl_queue_ = new sycl::queue{context, device_, 
              exceptions_handler, sycl::property_list{sycl::property::queue::in_order{}} };

    showDeviceInfo(*sycl_queue_);

    l2_cache_size_ =  sycl_queue_->get_device().get_info<sycl::info::device::local_mem_size>();

    allow_cache_opt_ = options.GetOrDefault<bool>("cache_opt", false);

    // Select GPU to run on (for *the current* thread).
    multi_stream_ = options.GetOrDefault<bool>("multi_stream", false);
    
    // layout used by cuda backend is nchw.
    has_tensor_cores_ = false;
    constexpr bool fp16 = std::is_same<sycl::half, DataType>::value;

    //dpct::device_info deviceProp = {};
    //sycl_queue_->get_device().get_device_info(deviceProp);
    size_t sharedMemPerBlockOptin = device_.get_info<sycl::info::device::local_mem_size>();


    if (fp16) {
      dpct::has_capability_or_fail(sycl_queue_->get_device(), {sycl::aspect::fp16});
        CERR << "Using Fp16 "; 
    } else {
        CERR << "Using Fp32 ";
    }

    const int kNumInputPlanes = kInputPlanes;
    const int kNumFilters = (int)weights.input.biases.size();
    numBlocks_ = (int)weights.residual.size();
    numFilters_ = kNumFilters;

    num_encoder_blocks_ = (int)weights.encoder.size();
    if (attn_body_) {
      assert(weights.ip_emb_b.size() > 0);
    }

    // Warn if the memory required for storing transformed weights is
    // going to exceed 40% of total video memory, force custom_winograd off
    // if it's going to exceed 50% of memory.
    size_t residual_single_layer_weight_size =
        3 * 3 * kNumFilters * kNumFilters * sizeof(DataType);
    size_t residual_weight_size =
        residual_single_layer_weight_size * numBlocks_ * 2;
    size_t transformed_residual_weight_size = residual_weight_size * 4;

    size_t global_mem_size = sycl_queue_->get_device().get_info<sycl::info::device::max_mem_alloc_size>();

    if (transformed_residual_weight_size > 0.4 * global_mem_size) {
      CERR << "WARNING: Low GPU video memory. You may run into OOM errors. Try "
              "using a smaller network.";
    }

    // Disable res block fusing for fp32 for now (not worth it)
    // TODO: make it work for filters not a multiple of 32.
    // Note that when used with SE, the optimization
    // works only when filter count is <= 384 (pre-Ampere), or less than 512
    // (Ampere)
    // It turns dynamically off based on filter count (see
    // ResidualBlock<DataType>::Eval)
    // TODO: fix res_block_fusing.
    if (kNumFilters % 32 == 0 && std::is_same<sycl::half, DataType>::value) {
      use_res_block_winograd_fuse_opt_ = true;
    } else {
      use_res_block_winograd_fuse_opt_ = false;
    }
    // Override if set in backend-opts.
    if (!options.IsDefault<bool>("res_block_fusing")) {
      use_res_block_winograd_fuse_opt_ = options.Get<bool>("res_block_fusing");
    }

    // 0. Check for SE.
    has_se_ = false;
    if (numBlocks_ && weights.residual[0].has_se) {
      has_se_ = true;
    }

    // Have some minumum as we also use this for transforming weights.
    size_t max_weight_size = 128 * 1024 * 1024;

    // parts from scratch allocation are suballocated to hold various weights
    // and biases when transforming winograd weights (one layer at a time), 128
    // MB is way more than that what we need but make sure it's at least 3x of
    // single layer's weight size to be safe.
    if (max_weight_size < 3 * residual_single_layer_weight_size)
        max_weight_size = 3 * residual_single_layer_weight_size;

     scratch_size_ = max_weight_size;

    // times size (4x4 block transformed into 6x6).
    if (numBlocks_ > 0) {
      const size_t transformed_tensor_size =
          (size_t)(max_batch_size_ * kNumFilters * 64 * (36.0 / 16.0) *
                   sizeof(DataType));
      scratch_size_ = std::max(scratch_size_, 2 * transformed_tensor_size);
    }

    std::string policy_head =
        options.GetOrDefault<std::string>("policy_head", "vanilla");
    // Check that selected policy head exists.
    if (weights.policy_heads.count(policy_head) == 0) {
      throw Exception("The policy head you specified '" + policy_head +
                      "' does not exist in this net.");
    }
    std::string value_head =
        options.GetOrDefault<std::string>("value_head", "winner");
    // Check that selected value head exists.
    if (weights.value_heads.count(value_head) == 0) {
      throw Exception("The value head you specified '" + value_head +
                      "' does not exist in this net.");
    }

    // Attention policy head or body may need more memory
    const size_t attentionPolicySize =
        getMaxAttentionHeadSize(weights.policy_heads.at(policy_head),
                                max_batch_size_) *
        sizeof(DataType);

    const size_t attentionBodySize =
        getMaxAttentionBodySize(weights, max_batch_size_) * sizeof(DataType);
    scratch_size_ = std::max(scratch_size_,
                             std::max(attentionPolicySize, attentionBodySize));

    scratch_mem_ = (void*)sycl::malloc_device(scratch_size_, *sycl_queue_);

    const bool mish_net = file.format().network_format().default_activation() ==
                          pblczero::NetworkFormat::DEFAULT_ACTIVATION_MISH;

    ActivationFunction act = mish_net ? ACTIVATION_MISH : ACTIVATION_RELU;

    // 2. Build the network, and copy the weights to GPU memory.

    // Input conv only used if there are residual blocks in the network
    if (numBlocks_ > 0) {
      // Input.
      {
        auto inputConv = std::make_unique<FusedWinogradConvSELayer<DataType>>(
            nullptr, kNumFilters, 8, 8, kNumInputPlanes, act, true, false,
            false, 0,  *sycl_queue_, use_res_block_winograd_fuse_opt_);

        inputConv->LoadWeights(&weights.input.weights[0],
                               &weights.input.biases[0], scratch_mem_);
        network_.emplace_back(std::move(inputConv));
      }

      // Residual block.
      for (int block = 0; block < numBlocks_; block++) {
        bool has_se = weights.residual[block].has_se;
        int se_k = (int)weights.residual[block].se.b1.size();

           
        if (use_res_block_winograd_fuse_opt_) {
          auto layer = std::make_unique<ResidualBlock<DataType>>(
              getLastLayer(), kNumFilters, has_se, se_k,
              block == 0, block == (numBlocks_ - 1), act,
              sharedMemPerBlockOptin, *sycl_queue_);
          layer->LoadWeights0(&weights.residual[block].conv1.weights[0],
                              &weights.residual[block].conv1.biases[0],
                              scratch_mem_);
          layer->LoadWeights1(&weights.residual[block].conv2.weights[0],
                              &weights.residual[block].conv2.biases[0],
                              scratch_mem_);
          if (has_se)
            layer->LoadSEWeights(&weights.residual[block].se.w1[0],
                                 &weights.residual[block].se.b1[0],
                                 &weights.residual[block].se.w2[0],
                                 &weights.residual[block].se.b2[0],
                                 scratch_mem_);
          network_.emplace_back(std::move(layer));
        } else { 
          auto conv1 = std::make_unique<FusedWinogradConvSELayer<DataType>>(
              getLastLayer(), kNumFilters, 8, 8, kNumFilters, act, true, false,
              false, 0, *sycl_queue_);

          conv1->LoadWeights(&weights.residual[block].conv1.weights[0],
                             &weights.residual[block].conv1.biases[0],
                             scratch_mem_);
          network_.emplace_back(std::move(conv1));

          auto conv2 = std::make_unique<FusedWinogradConvSELayer<DataType>>(
              getLastLayer(), kNumFilters, 8, 8, kNumFilters, act, true, true,
              has_se, se_k, *sycl_queue_);
          conv2->LoadWeights(&weights.residual[block].conv2.weights[0],
                             &weights.residual[block].conv2.biases[0],
                             scratch_mem_);
          if (has_se)
            conv2->LoadSEWeights(&weights.residual[block].se.w1[0],
                                 &weights.residual[block].se.b1[0],
                                 &weights.residual[block].se.w2[0],
                                 &weights.residual[block].se.b2[0],
                                 scratch_mem_);
          network_.emplace_back(std::move(conv2));
        }
      }
      resi_last_ = getLastLayer();
    }

    if (attn_body_) {
      Activations activations;
      const auto smolgen_activation =
          file.format().network_format().smolgen_activation();
      activations.smolgen_activation =
          smolgen_activation == pblczero::NetworkFormat::ACTIVATION_DEFAULT
              ? act
              : static_cast<ActivationFunction>(smolgen_activation);
      const auto ffn_activation =
          file.format().network_format().ffn_activation();
      activations.ffn_activation =
          ffn_activation == pblczero::NetworkFormat::ACTIVATION_DEFAULT
              ? act
              : static_cast<ActivationFunction>(ffn_activation);
      activations.default_activation = act;

      auto attention_body = std::make_unique<AttentionBody<DataType>>(
          weights, scratch_mem_, activations, numBlocks_,
          numBlocks_ > 0 ? kNumFilters : kInputPlanes, max_batch_size_,
          static_cast<InputEmbedding>(
              file.format().network_format().input_embedding()) ==
              InputEmbedding::INPUT_EMBEDDING_PE_DENSE,
          *sycl_queue_);
      network_.emplace_back(std::move(attention_body));

      encoder_last_ = getLastLayer();
    }

    // Policy head.
    {
      MultiHeadWeights::PolicyHead& head = weights.policy_heads.at(policy_head);
      if (attn_policy_) {
        auto AttentionPolicy = std::make_unique<AttentionPolicyHead<DataType>>(
            getLastLayer(), head, scratch_mem_, attn_body_, act,
            max_batch_size_, *sycl_queue_);
        network_.emplace_back(std::move(AttentionPolicy));

        auto policymap = std::make_unique<PolicyMapLayer<DataType>>(
            getLastLayer(), kNumOutputPolicy, 1, 1, 64 * 64 + 8 * 24, true, *sycl_queue_);
        policymap->LoadWeights(kAttnPolicyMap, scratch_mem_);
        network_.emplace_back(std::move(policymap));

      } else {
        if (conv_policy_) {
          assert(!attn_body_);  // not supported with attention body
          auto conv1 = std::make_unique<FusedWinogradConvSELayer<DataType>>(
              resi_last_, kNumFilters, 8, 8, kNumFilters, act, true, false,
              false, 0, *sycl_queue_);
          conv1->LoadWeights(&head.policy1.weights[0], &head.policy1.biases[0],
                             scratch_mem_);
          network_.emplace_back(std::move(conv1));

          auto pol_channels = head.policy.biases.size();

          // No relu
          auto conv2 = std::make_unique<FusedWinogradConvSELayer<DataType>>(
              getLastLayer(), pol_channels, 8, 8, kNumFilters, ACTIVATION_NONE,
              true, false, false, 0, *sycl_queue_);
          conv2->LoadWeights(&head.policy.weights[0], &head.policy.biases[0],
                             scratch_mem_);
          network_.emplace_back(std::move(conv2));

          auto policymap = std::make_unique<PolicyMapLayer<DataType>>(
              getLastLayer(), kNumOutputPolicy, 1, 1, 73 * 8 * 8, false, *sycl_queue_);
          policymap->LoadWeights(kConvPolicyMap, scratch_mem_);

          network_.emplace_back(std::move(policymap));
        } else {
          assert(!attn_body_);  // not supported with attention body
          auto convPol = std::make_unique<Conv1Layer<DataType>>(
              resi_last_, head.policy.biases.size(), 8, 8, kNumFilters, act,
              true, *sycl_queue_);
          convPol->LoadWeights(&head.policy.weights[0], &head.policy.biases[0],
                               scratch_mem_);
          network_.emplace_back(std::move(convPol));

          auto FCPol = std::make_unique<FCLayer<DataType>>(
              getLastLayer(), head.ip_pol_b.size(), 1, 1, true,
              ACTIVATION_NONE, *sycl_queue_);
          FCPol->LoadWeights(&head.ip_pol_w[0], &head.ip_pol_b[0],
                             scratch_mem_);
          network_.emplace_back(std::move(FCPol));
        }
      }
    }

    // Value heads.
    {
      const MultiHeadWeights::ValueHead& head =
          weights.value_heads.at(value_head);
      wdl_ = file.format().network_format().value() ==
             pblczero::NetworkFormat::VALUE_WDL;

      BaseLayer<DataType>* lastlayer = attn_body_ ? encoder_last_ : resi_last_;
      auto value_main = std::make_unique<ValueHead<DataType>>(
          lastlayer, head, scratch_mem_, attn_body_, wdl_, act,
          max_batch_size_, *sycl_queue_);
      network_.emplace_back(std::move(value_main));
    }

    // Moves left head
    moves_left_ = (file.format().network_format().moves_left() ==
                   pblczero::NetworkFormat::MOVES_LEFT_V1) &&
                  options.GetOrDefault<bool>("mlh", true);
    if (moves_left_) {
      if (attn_body_) {
        auto embedded_mov = std::make_unique<EmbeddingLayer<DataType>>(
            encoder_last_, weights.ip_mov_w, weights.ip_mov_b, scratch_mem_,
            act, *sycl_queue_);
        network_.emplace_back(std::move(embedded_mov));
      } else {
        auto convMov = std::make_unique<Conv1Layer<DataType>>(
            resi_last_, weights.moves_left.biases.size(), 8, 8, kNumFilters,
            act, true, *sycl_queue_);
        convMov->LoadWeights(&weights.moves_left.weights[0],
                             &weights.moves_left.biases[0], scratch_mem_);
        network_.emplace_back(std::move(convMov));
      }
      auto FCMov1 = std::make_unique<FCLayer<DataType>>(
          getLastLayer(), weights.ip1_mov_b.size(), 1, 1, true, act, *sycl_queue_);
      FCMov1->LoadWeights(&weights.ip1_mov_w[0], &weights.ip1_mov_b[0],
                          scratch_mem_);
      network_.emplace_back(std::move(FCMov1));

      auto FCMov2 = std::make_unique<FCLayer<DataType>>(getLastLayer(), 1, 1, 1,
                                                        true, ACTIVATION_RELU, *sycl_queue_);
      FCMov2->LoadWeights(&weights.ip2_mov_w[0], &weights.ip2_mov_b[0],
                          scratch_mem_);
      network_.emplace_back(std::move(FCMov2));
    }

    // 3. Allocate GPU memory for running the network:
    //    - three buffers of max size are enough (one to hold input, second to
    //      hold output and third to hold skip connection's input).

    // size of input to the network
    size_t maxSize = max_batch_size_ * kNumInputPlanes * 64 * sizeof(DataType);

    // take max size of all layers
    for (auto& layer : network_) {
      maxSize = std::max(maxSize, layer->GetOutputSize(max_batch_size_));
    }

    if ((attn_policy_ || use_res_block_winograd_fuse_opt_ || attn_body_) &&
        (scratch_size_ > maxSize)) {
      maxSize = scratch_size_;
    }

    if (!multi_stream_) {
      for (auto& mem : tensor_mem_) {
            //mem = (typename std::remove_reference<decltype(mem)>::type)
            mem = (DataType *)sycl::malloc_device(maxSize, *sycl_queue_);
            sycl_queue_->memset(mem, 0, maxSize).wait();
      }
    }

    tensor_mem_size_ = multi_stream_ ? maxSize : 0;

    // pre-allocate one InputsOutputs object
    // The first call to allocate memory, create cublas,
    // strem, etc takes really long (600 ms)
    //CERR << "Creating Inputs Outputs. ";
    std::unique_ptr<InputsOutputs> io = GetInputsOutputs();
    //CERR << "Done loading network. ";
  }

  void forwardEval(InputsOutputs* io, int batchSize) {
    
    
    if (!multi_stream_) lock_.lock();

#ifdef DEBUG_RAW_NPS
    auto t_start = std::chrono::high_resolution_clock::now();
#endif

    // Expand packed planes to full planes.
    uint64_t* ipDataMasks = io->input_masks_mem_shared_;
    float* ipDataValues = io->input_val_mem_shared_;
    sycl::queue io_sycl_queue_ = io->q_ct1;

    DataType* tensor_mem[3];
    void* scratch_mem;
    DataType*** offset_pointers;
    DataType*** head_offset_pointers;

    if (multi_stream_) {
      
      // We use tensor and scratch memory from InputOutputs (so that multiple
      // requests can run in parallel)
      for (int i = 0; i < 3; i++) tensor_mem[i] = (DataType*)io->tensor_mem_[i];
      scratch_mem = io->scratch_mem_;
      offset_pointers = (DataType***)&io->offset_pointers_;
      head_offset_pointers = (DataType***)&io->head_offset_pointers_;
      //stream = io->stream_;
      //cublas = io->cublas_;
    } else {
      
      for (int i = 0; i < 3; i++) tensor_mem[i] = tensor_mem_[i];
      scratch_mem = scratch_mem_;
      offset_pointers = (DataType***)&offset_pointers_;
      head_offset_pointers = (DataType***)&head_offset_pointers_;
      //stream = &dpct::get_default_queue();  // default stream
      //cublas = cublas_;
    }

    
    bool fp16 = std::is_same<sycl::half, DataType>::value;
    if (fp16) {
      expandPlanes_Fp16_NCHW((sycl::half*)(tensor_mem[0]), ipDataMasks, ipDataValues,
                             batchSize * kInputPlanes, io_sycl_queue_);
    } else {
      expandPlanes_Fp32_NCHW((float*)(tensor_mem[0]), ipDataMasks, ipDataValues,
                             batchSize * kInputPlanes, io_sycl_queue_);
    }
    

    float* opPol = io->op_policy_mem_gpu_;
    float* opVal = (wdl_) ? io->op_value_mem_gpu_ : io->op_value_mem_shared_;
    float* opMov = io->op_moves_left_mem_shared_;
    float* values = sycl::malloc_device<float>(max_batch_size_, io_sycl_queue_);

    

    // Figure out if the memory requirment for running the res block would fit
    // in the L2 cache.
    bool enableCacheOpt = false;
    DataType* skip_connection =
        use_res_block_winograd_fuse_opt_ ? tensor_mem[1] : tensor_mem[2];

    
#if SYCL_LANGUAGE_VERSION >= 202001
    const int pre_transform_tensor_size =
        batchSize * numFilters_ * 8 * 8 * sizeof(DataType);
    const int transformed_tensor_size = pre_transform_tensor_size * 36 / 16;
    const int res_block_mem =
        transformed_tensor_size * 2 + pre_transform_tensor_size;

    if (allow_cache_opt_ && use_res_block_winograd_fuse_opt_ &&
       (res_block_mem <= scratch_size_) && (res_block_mem <= l2_cache_size_)) {
        //auto buffer_location_property = sycl::ext::oneapi::property::buffer_location{1};

        //sycl::buffer<DataType> tensor_buffer(tensor_mem[0], 
            //sycl::range<1>(res_block_mem / sizeof(DataType)), {buffer_location_property});

        enableCacheOpt = true;
        skip_connection = tensor_mem[0] + 2 * transformed_tensor_size / sizeof(DataType);
    }
#endif

    int l = 0;

    DataType* flow = tensor_mem[0];
    DataType* spare1 = tensor_mem[1];
    DataType* spare2 = tensor_mem[2];
    

    if (numBlocks_ > 0) {
      // Input.
      network_[l++]->Eval(batchSize, skip_connection, tensor_mem[0], nullptr,
                          scratch_mem, scratch_size_, io_sycl_queue_, nullptr);  // input conv
      

      // Residual block.
      for (int block = 0; block < numBlocks_; block++) {
        if (use_res_block_winograd_fuse_opt_) {
          network_[l++]->Eval(batchSize, tensor_mem[2], skip_connection,
                              nullptr, enableCacheOpt ? nullptr : scratch_mem,
                              scratch_size_, io_sycl_queue_, nullptr);  // block
        } else {
          network_[l++]->Eval(batchSize, tensor_mem[0], tensor_mem[2], nullptr,
                              scratch_mem, scratch_size_, io_sycl_queue_, nullptr);  // conv1

          network_[l++]->Eval(batchSize, tensor_mem[2], tensor_mem[0],
                              tensor_mem[2], scratch_mem, scratch_size_,
                              io_sycl_queue_, nullptr);  // conv2
        }

        
      }

      flow = tensor_mem[2];
      spare1 = tensor_mem[0];
      spare2 = tensor_mem[1];
    }

    
    if (attn_body_) {
      network_[l++]->Eval(
          batchSize, tensor_mem[1],
          (numBlocks_ > 0) ? tensor_mem[2] : tensor_mem[0],
          (numBlocks_ > 0) ? tensor_mem[0] : tensor_mem[2], scratch_mem,
          scratch_size_, io_sycl_queue_,
          offset_pointers);  // Entire attention body of the network

      flow = tensor_mem[1];
      spare1 = tensor_mem[0];
      spare2 = tensor_mem[2];
    }

    // Policy head.
   
    if (attn_policy_) {
      
      network_[l++]->Eval(
          batchSize, spare1, flow, spare2, scratch_mem, scratch_size_, io_sycl_queue_,
          head_offset_pointers);  // Entire Attention policy head except for the
                                  // policy map
          io_sycl_queue_.wait();
      if (fp16) {
        network_[l++]->Eval(batchSize, spare2, spare1, nullptr, scratch_mem,
                            scratch_size_, io_sycl_queue_, nullptr);  // policy map layer

        io_sycl_queue_.wait();

        copyTypeConverted(opPol, (sycl::half*)spare2,
                          batchSize * kNumOutputPolicy,
                          io_sycl_queue_);  // POLICY output
        io_sycl_queue_.wait();
      } else {
        network_[l++]->Eval(batchSize, (DataType*)opPol, spare1, nullptr,
                            scratch_mem, scratch_size_, io_sycl_queue_, nullptr);  // policy map layer  // POLICY output
        
        io_sycl_queue_.wait();
      }
 
    } else if (conv_policy_) {

      network_[l++]->Eval(batchSize, spare1, flow, nullptr, scratch_mem,
                          scratch_size_, io_sycl_queue_, nullptr);  // policy conv1
      io_sycl_queue_.wait();

      network_[l++]->Eval(batchSize, spare2, spare1, nullptr, scratch_mem,
                          scratch_size_, io_sycl_queue_, nullptr);  // policy conv2
      io_sycl_queue_.wait();

      if (fp16) {
        network_[l++]->Eval(batchSize, spare1, spare2, nullptr, scratch_mem,
                            scratch_size_, io_sycl_queue_, nullptr);  // policy map layer

        io_sycl_queue_.wait();
        copyTypeConverted(opPol, (sycl::half*)(spare1),
                          batchSize * kNumOutputPolicy,
                          io_sycl_queue_);  // POLICY output

        io_sycl_queue_.wait();

      } else {
        network_[l++]->Eval(batchSize, (DataType*)opPol, spare2, nullptr,
                            scratch_mem, scratch_size_, io_sycl_queue_, nullptr);  
                            // policy map layer  // POLICY output
        io_sycl_queue_.wait();
      }

    } else {
      
      network_[l++]->Eval(batchSize, spare1, flow, nullptr, scratch_mem,
                          scratch_size_, io_sycl_queue_, nullptr);  // pol conv
      io_sycl_queue_.wait();

      if (fp16) {
        network_[l++]->Eval(batchSize, spare2, spare1, nullptr, scratch_mem,
                            scratch_size_, io_sycl_queue_, nullptr);  // pol FC
        io_sycl_queue_.wait();

        copyTypeConverted(opPol, (sycl::half*)(spare2),
                          batchSize * kNumOutputPolicy,
                          io_sycl_queue_);  // POLICY
        io_sycl_queue_.wait();
      } else {
        network_[l++]->Eval(batchSize, (DataType*)opPol, spare1, nullptr,
                            scratch_mem, scratch_size_, io_sycl_queue_, nullptr);  // pol FC  // POLICY
        io_sycl_queue_.wait();
      }
    }

    // Copy policy output from device memory to host memory.

    io_sycl_queue_.memcpy(io->op_policy_mem_, io->op_policy_mem_gpu_, sizeof(float) * kNumOutputPolicy * batchSize);
    io_sycl_queue_.wait();


    // value head
    if (fp16) {
      network_[l++]->Eval(batchSize, spare1, flow, spare2, scratch_mem,
                          scratch_size_, io_sycl_queue_, nullptr);  // value head
      io_sycl_queue_.wait();

      copyTypeConverted(opVal, (sycl::half*)spare1, wdl_ ? 3 * batchSize : batchSize,
                        io_sycl_queue_);
      io_sycl_queue_.wait();
    } else {
      network_[l++]->Eval(batchSize, (DataType*)opVal, flow, spare2,
                          scratch_mem, scratch_size_, io_sycl_queue_, nullptr);  // value head
      io_sycl_queue_.wait();
    }

    if (moves_left_) {

      // Moves left head
      network_[l++]->Eval(batchSize, spare1, flow, nullptr, scratch_mem,
                          scratch_size_, io_sycl_queue_, nullptr);  // moves conv or embedding

      io_sycl_queue_.wait();

      network_[l++]->Eval(batchSize, spare2, spare1, nullptr, scratch_mem,
                          scratch_size_, io_sycl_queue_, nullptr);  // moves FC1

      io_sycl_queue_.wait();

      // Moves left FC2
      if (fp16) {
        // TODO: consider fusing the bias-add of FC2 with format conversion.
        
        
        network_[l++]->Eval(batchSize, spare1, spare2, nullptr, scratch_mem,
                            scratch_size_, io_sycl_queue_, nullptr);
        
        io_sycl_queue_.wait();

        copyTypeConverted(opMov, (sycl::half*)(spare1), batchSize, io_sycl_queue_);
        io_sycl_queue_.wait();
      
      } else {

        network_[l++]->Eval(batchSize, (DataType*)opMov, spare2, nullptr,
                            scratch_mem, scratch_size_, io_sycl_queue_, nullptr);
        io_sycl_queue_.wait();

      }
    }

    if (multi_stream_) {
        io_sycl_queue_.wait();
    } else {
        io_sycl_queue_.wait();
      //ReportCUDAErrors(
        //  DPCT_CHECK_ERROR(dpct::get_current_device().queues_wait_and_throw()));
      // The next thread can start using the GPU now.
      lock_.unlock();
    }
    
    io_sycl_queue_.memcpy(values, io->op_value_mem_gpu_,  sizeof(float) * batchSize * 3);
    io_sycl_queue_.wait();
    
    if (wdl_) {
        sycl::range<1> nd{batchSize};
        // Launch softmax kernel.
        io_sycl_queue_.parallel_for<class SyclNetwork>(nd, [=](sycl::id<1> idx) {
                float w = values[3 * idx + 0];
                float d = values[3 * idx + 1];
                float l = values[3 * idx + 2];
                float m = sycl::max(sycl::max(w, d), l);
                w = sycl::exp(w - m);
                d = sycl::exp(d - m);
                l = sycl::exp(l - m);
                float sum = w + d + l;
                w /= sum;
                l /= sum;
                d = 1.0f - w - l;
                values[3 * idx + 0] = w;
                values[3 * idx + 1] = d;
                values[3 * idx + 2] = l;
        }).wait_and_throw();
    }

    // Copy value from device to host.
    io_sycl_queue_.memcpy(io->op_value_mem_shared_, values, sizeof(float) * batchSize * 3);
    io_sycl_queue_.wait();

    // Free the allocated device memory
    sycl::free(io->op_value_mem_gpu_, io_sycl_queue_);

  }

  ~SyclNetwork() {
    if (scratch_mem_) 
        sycl::free(scratch_mem_, *sycl_queue_);
    if (!multi_stream_) {
      for (auto mem : tensor_mem_) {
        if (mem) 
          sycl::free(mem, *sycl_queue_);
      }
      if (offset_pointers_) 
          sycl::free(offset_pointers_, *sycl_queue_);
      if (head_offset_pointers_)
          sycl::free(head_offset_pointers_, *sycl_queue_);
      //*sycl_queue_ = nullptr;
    }
  }

  const NetworkCapabilities& GetCapabilities() const override {
    return capabilities_;
  }
  
  // A vector to store sycl devices.
  std::vector<sycl::device> devices;
  
  // Check if device is the cpu for thread handling.
  bool IsCpu() const override { return device_.is_cpu(); }
  
  // 2 threads for cpu and 1 + total_gpu's for the multiple gpu's. 
  int GetThreads() const override { return device_.is_cpu() ? 1 : 1 + total_gpus_; }
  
  int GetMiniBatchSize() const override {
     if (device_.is_cpu()) { return 47; }
    // Simple heuristic that seems to work for a wide range of GPUs.
    return 2 * compute_units_;
  }
  
  // Function to convert a string to lowercase using std::tolower
  std::string to_lower(const std::string& str) {
        std::string lower_str = str;
        std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(),
                   [](unsigned char c) { return std::tolower(c); });
        return lower_str;
  }

  std::unique_ptr<NetworkComputation> NewComputation() override {
    return std::make_unique<SyclNetworkComputation<DataType>>(this, wdl_,
                                                              moves_left_);
  }

  std::unique_ptr<InputsOutputs> GetInputsOutputs() {
    std::lock_guard<std::mutex> lock(inputs_outputs_lock_);
    if (free_inputs_outputs_.empty()) {
      return std::make_unique<InputsOutputs>(
          max_batch_size_, wdl_, moves_left_, *sycl_queue_, tensor_mem_size_, scratch_size_,
          !has_tensor_cores_ && std::is_same<sycl::half, DataType>::value);
    } else {
      std::unique_ptr<InputsOutputs> resource =
          std::move(free_inputs_outputs_.front());
      free_inputs_outputs_.pop_front();
      return resource;
    }
  }

  void ReleaseInputsOutputs(std::unique_ptr<InputsOutputs> resource) {
    std::lock_guard<std::mutex> lock(inputs_outputs_lock_);
    free_inputs_outputs_.push_back(std::move(resource));
  }


 private:
  const NetworkCapabilities capabilities_;
  int gpu_id_;
  int l2_cache_size_;
  int max_batch_size_;
  int compute_units_;
  int total_gpus_;
  bool wdl_;
  bool moves_left_;
  bool use_res_block_winograd_fuse_opt_;  // fuse operations inside the residual
                                          // tower
  bool multi_stream_;                     // run multiple parallel network evals
  bool allow_cache_opt_;  // try to fit residual block activations in L2 cache

  // Currently only one NN Eval can happen a time (we can fix this if needed
  // by allocating more memory).
  mutable std::mutex lock_;
  sycl::queue * sycl_queue_;
  sycl::device device_;

  int numBlocks_;
  int numFilters_;
  bool has_se_;
  bool conv_policy_;
  bool attn_policy_;
  bool attn_body_;
  int num_encoder_blocks_;
  std::vector<std::unique_ptr<BaseLayer<DataType>>> network_;
  BaseLayer<DataType>* getLastLayer() { return network_.back().get(); }

  BaseLayer<DataType>* resi_last_;
  BaseLayer<DataType>* encoder_last_;

  size_t tensor_mem_size_;
  size_t scratch_size_;

  // this copy is used only for initialization when multi-stream is enabled
  void* scratch_mem_;
  // this is only used when multi-stream is disabled
  void** offset_pointers_ = nullptr;
  void** head_offset_pointers_ = nullptr;

  bool has_tensor_cores_;

  // not used when multi-steam is enabled
  //dpct::queue_ptr cublas_;
  DataType* tensor_mem_[3];

  mutable std::mutex inputs_outputs_lock_;
  std::list<std::unique_ptr<InputsOutputs>> free_inputs_outputs_;

  void showDeviceInfo(const sycl::queue &mqueue) const {
    // Platform name
    std::cerr << "PLATFORM: " 
              << mqueue.get_device().get_platform().get_info<sycl::info::platform::name>() 
              << std::endl;
    // Device name
    std::string device_type = mqueue.get_device().is_gpu() ? "GPU" : "CPU";
    std::cerr << device_type << ": " 
              << mqueue.get_device().get_info<sycl::info::device::name>() 
              << std::endl;
    // Device memory (max_mem_alloc_size) in MB
    std::cerr << device_type << ": " 
              << mqueue.get_device().get_info<sycl::info::device::max_mem_alloc_size>() / (1024 * 1024) 
              << " MB" 
              << std::endl;
    // Device clock frequency (max_clock_frequency)
    std::cerr << device_type << " clock frequency: " 
              << mqueue.get_device().get_info<sycl::info::device::max_clock_frequency>() 
              << " MHz" 
              << std::endl;
    // L2 cache capacity (local_mem_size) in MB
    std::cerr << "L2 cache capacity: " 
              << mqueue.get_device().get_info<sycl::info::device::local_mem_size>() / (1024) 
              << " KB" 
              << std::endl;
    // Global memory size (global_mem_size) in MB
    std::cerr << "Global memory size: " 
              << mqueue.get_device().get_info<sycl::info::device::global_mem_size>() / (1024 * 1024) 
              << " MB" 
              << std::endl;          
    }
};

template <typename DataType>
SyclNetworkComputation<DataType>::SyclNetworkComputation(
    SyclNetwork<DataType>* network, bool wdl, bool moves_left)
    : wdl_(wdl), moves_left_(moves_left), network_(network) {
  batch_size_ = 0;
  inputs_outputs_ = network_->GetInputsOutputs();
}

template <typename DataType>
SyclNetworkComputation<DataType>::~SyclNetworkComputation() {
  network_->ReleaseInputsOutputs(std::move(inputs_outputs_));
}

template <typename DataType>
void SyclNetworkComputation<DataType>::ComputeBlocking() {
  network_->forwardEval(inputs_outputs_.get(), GetBatchSize());
}

template <typename DataType>
std::unique_ptr<Network> MakeSyclNetwork(const std::optional<WeightsFile>& w,
                                         const OptionsDict& options) {
  if (!w) {
    throw Exception(
        "The sycl" +
        std::string(std::is_same<sycl::half, DataType>::value ? "-fp16" : "") +
        " backend requires a network file.");
  }
  const WeightsFile& weights = *w;
  auto nf = weights.format().network_format();
  using NF = pblczero::NetworkFormat;
  switch (nf.network()) {
    case NF::NETWORK_CLASSICAL_WITH_HEADFORMAT:
    case NF::NETWORK_SE_WITH_HEADFORMAT:
    case NF::NETWORK_ATTENTIONBODY_WITH_HEADFORMAT:
    case NF::NETWORK_ATTENTIONBODY_WITH_MULTIHEADFORMAT:
      break;
    default:
      throw Exception("Network format " +
                      NF::NetworkStructure_Name(nf.network()) +
                      " is not supported by the SYCL backend.");
  }
  switch (nf.policy()) {
    case NF::POLICY_CLASSICAL:
    case NF::POLICY_CONVOLUTION:
    case NF::POLICY_ATTENTION:
      break;
    default:
      throw Exception("Policy format " + NF::PolicyFormat_Name(nf.policy()) +
                      " is not supported by the SYCL backend.");
  }
  switch (nf.value()) {
    case NF::VALUE_CLASSICAL:
    case NF::VALUE_WDL:
      break;
    default:
      throw Exception("Value format " + NF::ValueFormat_Name(nf.value()) +
                      " is not supported by the SYCL backend.");
  }
  switch (nf.moves_left()) {
    case NF::MOVES_LEFT_NONE:
    case NF::MOVES_LEFT_V1:
      break;
    default:
      throw Exception("Moves left head format " +
                      NF::MovesLeftFormat_Name(nf.moves_left()) +
                      " is not supported by the SYCL backend.");
  }
  switch (nf.default_activation()) {
    case NF::DEFAULT_ACTIVATION_RELU:
    case NF::DEFAULT_ACTIVATION_MISH:
      break;
    default:
      throw Exception("Default activation " +
                      NF::DefaultActivation_Name(nf.default_activation()) +
                      " is not supported by the SYCL backend.");
  }
  switch (nf.input_embedding()) {
    case NF::INPUT_EMBEDDING_NONE:
    case NF::INPUT_EMBEDDING_PE_MAP:
    case NF::INPUT_EMBEDDING_PE_DENSE:
      break;
    default:
      throw Exception("Input embedding " +
                      NF::InputEmbeddingFormat_Name(nf.input_embedding()) +
                      " is not supported by the SYCL backend.");
  }
  return std::make_unique<SyclNetwork<DataType>>(weights, options);
}

std::unique_ptr<Network> MakeSyclNetworkAuto(
    const std::optional<WeightsFile>& weights, const OptionsDict& options) {
  int gpu_id = options.GetOrDefault<int>("gpu", 0);

  try {
    CERR << "Trying to switch to [sycl-fp16]...";
    dpct::has_capability_or_fail(dpct::dev_mgr::instance().get_device(gpu_id),
                                 {sycl::aspect::fp16});
    CERR << "Switched to [sycl-fp16]...";
    return MakeSyclNetwork<sycl::half>(weights, options);
  } catch (std::exception& e) {
    CERR << "Device does not support sycl-fp16";
  }

  CERR << "Switched to [sycl]...";
  return MakeSyclNetwork<float>(weights, options);
}

REGISTER_NETWORK("sycl-auto", MakeSyclNetworkAuto, 132)
REGISTER_NETWORK("sycl", MakeSyclNetwork<float>, 131)
REGISTER_NETWORK("sycl-fp16", MakeSyclNetwork<sycl::half>, 130)

}  // namespace lczero
