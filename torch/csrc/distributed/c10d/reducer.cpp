#include <torch/csrc/distributed/c10d/reducer.h>

#include <functional>

#include <c10/core/DeviceGuard.h>
#include <c10/core/StreamGuard.h>
#include <c10/util/Exception.h>
#include <torch/csrc/autograd/engine.h>
#include <torch/csrc/autograd/function_hook.h>
#include <torch/csrc/autograd/functions/accumulate_grad.h>
#include <torch/csrc/autograd/profiler.h>
#include <torch/csrc/autograd/utils/grad_layout_contract.h>
#include <torch/csrc/autograd/utils/lambda_post_hook.h>
#include <torch/csrc/distributed/c10d/comm.h>
#include <torch/csrc/utils/hash.h>
#include <torch/csrc/utils/memory.h>

namespace c10d {
namespace {

inline int64_t current_time_in_nanos() {
  return torch::autograd::profiler::getTime();
}

} // namespace

Reducer::Reducer(
    std::vector<std::vector<torch::autograd::Variable>> replicas,
    std::vector<std::vector<size_t>> bucket_indices,
    std::shared_ptr<c10d::ProcessGroup> process_group,
    std::vector<std::vector<bool>> expect_sparse_gradients,
    int64_t bucket_bytes_cap,
    bool find_unused_parameters)
    : replicas_(std::move(replicas)),
      process_group_(std::move(process_group)),
      expect_sparse_gradients_(std::move(expect_sparse_gradients)),
      expect_autograd_hooks_(false),
      require_finalize_(false),
      next_bucket_(0),
      has_marked_unused_parameters_(false),
      find_unused_parameters_(find_unused_parameters),
      local_used_maps_reduced_(false),
      backward_stats_base_(0),
      has_rebuilt_bucket_(false),
      bucket_bytes_cap_(bucket_bytes_cap),
      comm_hook_(nullptr) {
  C10_LOG_API_USAGE_ONCE("torch.distributed.ddp.reducer");
  TORCH_CHECK(replicas_.size() >= 1, "Expected at least one model replica.");
  TORCH_CHECK(replicas_[0].size() >= 1, "Expected at least one parameter.");

  // If `expect_sparse_gradients` is not specified, initialize it such that
  // we do not expect sparse gradients for any parameter.
  if (expect_sparse_gradients_.empty()) {
    expect_sparse_gradients_ = std::vector<std::vector<bool>>(
        replicas_.size(), std::vector<bool>(replicas_[0].size(), false));
  }
  TORCH_INTERNAL_ASSERT(expect_sparse_gradients_.size() == replicas_.size());

  // Corresponding params' layouts (strides) must match across
  // replicas within this process and across processes.
  // (see Note:  "Gradient Layout Contract" in initialize_buckets).
  verify_replicas_within_process();
  verify_replica0_across_processes();

  // Initialize variable bucketing.
  // This can be reinitialized later after capturing runtime information.
  std::unique_lock<std::mutex> lock(this->mutex_);
  initialize_buckets(std::move(bucket_indices));
  lock.unlock();

  // All variables are expected to have their `grad_fn` set to the gradient
  // accumulation function (since they are leafs in the autograd graph).
  // We store pointers to these functions such that we can check if they are
  // used in an autograd pass. If they are not, we know their grad tensors
  // can be marked as ready for reduction.
  {
    const auto replica_count = replicas_.size();
    grad_accumulators_.resize(replica_count);
    for (size_t replica_index = 0; replica_index < replica_count;
         replica_index++) {
      const auto variable_count = replicas_[replica_index].size();
      grad_accumulators_[replica_index].resize(variable_count);
      for (size_t variable_index = 0; variable_index < variable_count;
           variable_index++) {
        auto& variable = replicas_[replica_index][variable_index];
        const auto index = VariableIndex{
            .replica_index = replica_index,
            .variable_index = variable_index,
        };

        // The gradient accumulator function is lazily initialized once.
        // Therefore we can use its presence in the autograd graph as
        // evidence that the parameter has participated in an iteration.
        auto grad_accumulator =
            torch::autograd::impl::grad_accumulator(variable);

        using torch::distributed::autograd::ThreadLocalDistAutogradContext;
        // Hook to execute after the gradient accumulator has executed.
        hooks_.emplace_back(
            grad_accumulator->add_post_hook(
                torch::make_unique<torch::autograd::utils::LambdaPostHook>(
                    [=](const torch::autograd::variable_list& outputs,
                        const torch::autograd::variable_list& /* unused */) {
                      this->rpc_context_.set(
                          ThreadLocalDistAutogradContext::getContextPtr());
                      this->autograd_hook(index);
                      return outputs;
                    })),
            grad_accumulator);

        // Map raw function pointer to replica index and parameter index.
        // This is used later on when the autograd graph is traversed
        // to check for parameters for which no gradient is computed.
        func_[grad_accumulator.get()] = index;

        // The gradient accumulator is stored as weak_ptr in the autograd
        // metadata of the variable, so we have to keep it alive here for
        // the raw pointer to be valid.
        grad_accumulators_[replica_index][variable_index] =
            std::move(grad_accumulator);
      }
    }
  }

  // Initialize backward stats vector.
  {
    const auto replica_count = replicas_.size();
    backward_stats_.resize(replica_count);
    const auto variable_count = replicas_[0].size();
    std::for_each(
        backward_stats_.begin(),
        backward_stats_.end(),
        [=](std::vector<int64_t>& v) { v.resize(variable_count); });
  }

  // See Note [Skip allreducing local_used_maps_dev]
  if (find_unused_parameters_) {
    // Initialize locally used parameter maps
    {
      const auto replica_count = replicas_.size();
      const auto variable_count = replicas_[0].size();
      local_used_maps_.resize(replica_count);
      local_used_maps_dev_.resize(replica_count);

      for (size_t i = 0; i < replica_count; i++) {
        at::TensorOptions options;
        options = options.dtype(at::kInt);

        if (replicas_[i][0].is_cuda()) {
          at::DeviceGuard g(replicas_[i][0].device());
          local_used_maps_[i] = at::zeros(
              {static_cast<long>(variable_count)}, options.pinned_memory(true));
        } else {
          local_used_maps_[i] =
              at::zeros({static_cast<long>(variable_count)}, options);
        }

        // This tensor needs to be on the same device as replica because backend
        // such as NCCL may not support CPU tensors, and hence it might not work
        // if we always put it on CPU.
        options = options.device(replicas_[i][0].device());
        local_used_maps_dev_[i] =
            at::empty({static_cast<long>(variable_count)}, options);
      }
    }
  }
}

// Note [Skip allreducing local_used_maps_dev]
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
// If find_unused_parameters_ is set to false, there is no need to allreduce
// local_used_maps_dev_, because all parameters will be reduced anyway.
// Therefore, we can avoid allocating memory for local_used_maps and
// local_used_maps_dev_ if find_unused_parameters_ is false.

// Note [DDP Communication Hook]
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
// If DDP communication hook is not registered, the reducer reduces the buckets
// by just calling allreduce. If registered, it calls the hook and uses future
// work handle. If registered, reducer also skips dividing grads by world size.
// The reason for this is that the communication hook is expected to completely
// override how we perform communication and the user should have complete
// control over how the grads are handled.
//
// DDP communication hook is an enhancement that provides a hook which can be
// used to override how DDP communicates gradients across ranks, this can be
// used for algorithms like Gradient Compression/GossipGrad. This hook can be
// registered from Python API using `register_comm_hook`. `PythonCommHook`
// enables registering a Python hook and is a sub class of `CommHookInterface`.
// `CommHookInterface` can be used to implement CPP hooks in the future.

Reducer::~Reducer() noexcept(false) {
  // Remove all hooks on variables registered by this Reducer. This is necessary
  // to make DDP failure recoverable. Otherwise, multiple Reducer instances
  // (from recoveries) will add their hooks to the original model, and those
  // hooks will try to invoke methods on a deleted Reducer objects.
  for (auto& hook : hooks_) {
    auto& key = hook.first;
    auto& grad_accumulator = hook.second;
    TORCH_CHECK(
        grad_accumulator->del_post_hook(key),
        "Reducer attempts to delete a non-existing hook.");
  }
}

// Verifies replicas in this process treat the same number of params,
// all params require grad, and corresponding params across replicas
// have the same dtype/size/layout.
void Reducer::verify_replicas_within_process() {
  const auto replica_count = replicas_.size();
  for (size_t replica_index = 0; replica_index < replica_count;
       replica_index++) {
    const auto variable_count = replicas_[replica_index].size();
    TORCH_CHECK(
        replicas_[replica_index].size() == replicas_[0].size(),
        "Model replicas must have an equal number of parameters.");
    TORCH_CHECK(
        expect_sparse_gradients_[replica_index].size() ==
            expect_sparse_gradients_[0].size(),
        "Expected number of entries in expect_sparse_gradients ",
        "to be equal across replicas.");
    for (size_t variable_index = 0; variable_index < variable_count;
         variable_index++) {
      TORCH_CHECK(
          replicas_[replica_index][variable_index].requires_grad(),
          "Variables must require gradients (have `requires_grad` set).");
      TORCH_CHECK(
          replicas_[replica_index][variable_index].sizes() ==
              replicas_[0][variable_index].sizes(),
          "Variables across model replicas must have identical sizes.");
      TORCH_CHECK(
          replicas_[replica_index][variable_index].strides() ==
              replicas_[0][variable_index].strides(),
          "Variables across model replicas must have identical strides.");
      TORCH_CHECK(
          replicas_[replica_index][variable_index].dtype() ==
              replicas_[0][variable_index].dtype(),
          "Variables across model replicas must have identical dtype.");
      TORCH_CHECK(
          expect_sparse_gradients_[replica_index][variable_index] ==
              expect_sparse_gradients_[0][variable_index],
          "Expected the same variables across replicas to either both ",
          "or neither expect a sparse gradient.");
    }
  }
}

// Verifies corresponding params in replica 0 have the same sizes/strides
// across processes.
void Reducer::verify_replica0_across_processes() {
  size_t i = 0;
  for (const auto& t : replicas_[0]) {
    i += 2 * t.dim();
  }
  at::TensorOptions options;
  options = options.dtype(at::kLong);
  auto metadata = at::empty({static_cast<long>(i)}, options);

  // Technically, process 0 is the broadcast source, so only process 0 needs
  // to populate metadata.  But no harm keeping work aligned across processes.
  auto metadata_accessor = metadata.accessor<int64_t, 1>();
  i = 0;
  for (const auto& t : replicas_[0]) {
    for (const auto& sz : t.sizes()) {
      metadata_accessor[i++] = sz;
    }
    for (const auto& str : t.strides()) {
      metadata_accessor[i++] = str;
    }
  }

  auto metadata_dev = metadata.clone().to(replicas_[0][0].device());
  std::vector<at::Tensor> vec{metadata_dev};
  process_group_->broadcast(vec)->wait();

  // Technically, process 0 doesn't need to double-check metadata, because it
  // was the source.  But no harm keeping work aligned.
  auto control = at::empty({static_cast<long>(i)}, options);
  control.copy_(metadata_dev, /*non_blocking=*/false);
  auto control_accessor = control.accessor<int64_t, 1>();
  i = 0;
  for (size_t p = 0; p < replicas_[0].size(); p++) {
    const auto& t = replicas_[0][p];
    // I'd like to include which process we are in the message,
    // but ProcessGroup::getRank is not public!
    for (const auto& sz : t.sizes()) {
      TORCH_CHECK(
          sz == control_accessor[i++],
          "replicas[0][",
          p,
          "] in this process"
          " with sizes ",
          t.sizes(),
          " appears not to match sizes of the same param in process 0.");
    }
    for (const auto& str : t.strides()) {
      TORCH_CHECK(
          str == control_accessor[i++],
          "replicas[0][",
          p,
          "] in this process"
          " with strides ",
          t.strides(),
          " appears not to match strides of the same param in process 0.");
    }
  }
}

void Reducer::mark_variable_ready_dense(VariableIndex index) {
  const auto replica_index = index.replica_index;
  const auto variable_index = index.variable_index;
  const auto& bucket_index = variable_locators_[variable_index];
  auto& bucket = buckets_[bucket_index.bucket_index];
  auto& replica = bucket.replicas[replica_index];
  auto& variable = replica.variables[bucket_index.intra_bucket_index];
  const auto offset = replica.offsets[bucket_index.intra_bucket_index];
  const auto length = replica.lengths[bucket_index.intra_bucket_index];
  auto& bucket_view = replica.bucket_views[bucket_index.intra_bucket_index];

  runGradCallbackForVariable(variable, [&](auto& grad) {
    if (grad.defined()) {
      // Copy grad to bucket view buffer if grad and bucket_view are pointing
      // to different storages, and then let grad point to bucket_view
      // for saving memory and avoiding copies in subsquent iterations.
      // In most cases, the copy is needed only at first
      // iteration, there will be no copies in subsquent iterations.
      // In rare cases, if users explicitly set grad to be None after every
      // iteration, then it needs to copy grad to bucket_view in every
      // iteration.
      if (!grad.is_alias_of(bucket_view)) {
        // Ensure that the gradient type matches the bucket type.
        TORCH_CHECK(
            grad.options().type_equal(bucket_view.options()),
            "Expected ",
            bucket_view.toString(),
            ", got ",
            grad.toString());
        TORCH_INTERNAL_ASSERT(grad.device() == bucket_view.device());
        TORCH_INTERNAL_ASSERT(grad.numel() == bucket_view.numel());
        // AccumulateGrad doesn't HAVE to obey the grad layout contract.
        // The penalty for disobedience is reduced performance, not numerical
        // death. Warnings here help diagnose poor DDP performance.
        if (grad.strides() != bucket_view.strides()) {
          TORCH_WARN_ONCE(
              "Grad strides do not match bucket view strides. "
              "This may indicate grad was not created according to the "
              "gradient layout contract, or that the param's strides "
              "changed since DDP was constructed.  This is not an error, "
              "but may impair performance.\n"
              "grad.sizes() = ",
              grad.sizes(),
              ", strides() = ",
              grad.strides(),
              "\n",
              "bucket_view.sizes() = ",
              bucket_view.sizes(),
              ", strides() = ",
              bucket_view.strides());
        }
        // See Note [DDP Communication Hook]
        if (comm_hook_ == nullptr) {
          // imitates wrapped_scalar_tensor in ATen/native/BinaryOps.cpp
          auto wrapped =
              c10::scalar_to_tensor(double(1.) / process_group_->getSize());
          wrapped.unsafeGetTensorImpl()->set_wrapped_number(true);
          // Divides while copying into the bucket view.
          at::native::mul_out(bucket_view, grad, wrapped);
        } else {
          bucket_view.copy_(grad);
        }
        // Let grad point to bucket_view buffer.
        grad = bucket_view;
        // The grad is modified and need to be written back.
        return true;
      } else {
        // If grad and bucket view point to the same storage, no need to copy
        if (comm_hook_ == nullptr) {
          bucket_view.div_(process_group_->getSize());
        }
      }
    } else {
      bucket_view.zero_();
    }
    // The grad is not modified and doesn't need to be written back.
    return false;
  });
}

void Reducer::mark_variable_ready_sparse(VariableIndex index) {
  const auto replica_index = index.replica_index;
  const auto variable_index = index.variable_index;
  const auto& bucket_index = variable_locators_[variable_index];
  auto& bucket = buckets_[bucket_index.bucket_index];
  auto& replica = bucket.replicas[replica_index];
  auto& variable = replica.variables[bucket_index.intra_bucket_index];

  runGradCallbackForVariable(variable, [&](auto& grad) {
    TORCH_CHECK(grad.defined(), "Expected sparse gradient to be defined.");
    TORCH_CHECK(
        grad.options().layout() == c10::kSparse,
        "Expected variable to have sparse gradient.");

    // Sparse tensors cannot be grouped together with other sparse tensors
    // in a single reduction operation like we can for dense tensors.
    // Therefore, the `offsets` and `lengths` vectors in the bucket replica
    // struct are empty, and there is no pre-existing accumulation tensor.
    // Directly assign the sparse tensor to the `contents` field.
    replica.contents = grad;
    // See Note [DDP Communication Hook]
    if (comm_hook_ == nullptr) {
      replica.contents.div_(process_group_->getSize());
    }
    // The grad is modified in place and needs to be written back.
    return true;
  });
}

// The function `autograd_hook` is called after the gradient for a
// model parameter has been accumulated into its gradient tensor.
// This function is only to be called from the autograd thread.
void Reducer::autograd_hook(VariableIndex index) {
  std::lock_guard<std::mutex> lock(this->mutex_);

  // See Note [Skip allreducing local_used_maps_dev]
  if (find_unused_parameters_) {
    // Since it gets here, this param has been used for this iteration. We want
    // to mark it in local_used_maps_. During no_sync session, the same var can
    // be set multiple times, which is OK as does not affect correctness. As
    // long as it is used once during no_sync session, it is marked as used.
    local_used_maps_[index.replica_index][index.variable_index] = 1;
  }

  // Ignore if we don't expect to be called.
  // This may be the case if the user wants to accumulate gradients
  // for number of iterations before reducing them.
  if (!expect_autograd_hooks_) {
    return;
  }

  // Rebuild bucket only if 1) it is the first time to rebuild bucket 2)
  // find_unused_parameters_ is false, currently it does not support when there
  // are unused parameters 3) this backward pass needs to run allreduce. Here,
  // we just dump tensors and their parameter indices into rebuilt_params_ and
  // rebuilt_param_indices_ based on gradient arriving order, and then at the
  // end of finalize_backward(), buckets will be rebuilt based on
  // rebuilt_params_ and rebuilt_param_indices_, and then will be broadcasted
  // and intialized. Also we only need to dump tensors and parameter indcies of
  // one replica.
  if (!has_rebuilt_bucket_ && !find_unused_parameters_ &&
      index.replica_index == 0) {
    rebuilt_params_.push_back(
        replicas_[index.replica_index][index.variable_index]);
    rebuilt_param_indices_.push_back(index.variable_index);
  }

  // If `find_unused_parameters_` is true there may be model parameters that
  // went unused when computing the model output, they won't be part of the
  // autograd graph, and won't receive gradients. These parameters are
  // discovered in the `prepare_for_backward` function and their indexes stored
  // in the `unused_parameters_` vector.
  if (!has_marked_unused_parameters_ && find_unused_parameters_) {
    has_marked_unused_parameters_ = true;
    for (const auto& unused_index : unused_parameters_) {
      mark_variable_ready(unused_index);
    }
  }

  // Finally mark variable for which this function was originally called.
  mark_variable_ready(index);
}

void Reducer::mark_variable_ready(VariableIndex index) {
  const auto replica_index = index.replica_index;
  const auto variable_index = index.variable_index;
  TORCH_CHECK(replica_index < replicas_.size(), "Out of range replica index.");
  TORCH_CHECK(
      variable_index < variable_locators_.size(),
      "Out of range variable index.");
  backward_stats_[replica_index][variable_index] =
      current_time_in_nanos() - backward_stats_base_;

  // Any time we mark a variable ready (be it in line due to unused parameters,
  // or via an autograd hook), we require a call to the finalize function. If
  // this doesn't happen before the next iteration (or call to
  // `prepare_for_backwards`), we know something is wrong.
  require_finalize_ = true;

  const auto& bucket_index = variable_locators_[variable_index];
  auto& bucket = buckets_[bucket_index.bucket_index];
  auto& replica = bucket.replicas[replica_index];

  // Something is wrong if all variables contained in this bucket replica have
  // already been marked as ready.
  if (replica.pending == 0) {
    const auto common_error = c10::str(
        "Expected to mark a variable ready only once. ",
        "",
        "This error is caused by one of the following reasons: ",
        "1) Use of a module parameter outside the `forward` function. ",
        "Please make sure model parameters are not shared across multiple ",
        "concurrent forward-backward passes",
        "2) Reused parameters in multiple reentrant backward passes. For ",
        "example, if you use multiple `checkpoint` functions to wrap the ",
        "same part of your model, it would result in the same set of ",
        "parameters been used by different reentrant backward passes ",
        "multiple times, and hence marking a variable ready multiple times. ",
        "DDP does not support such use cases yet.");
    TORCH_CHECK(
        has_marked_unused_parameters_,
        common_error,
        "3) Incorrect unused parameter detection. The return value of the ",
        "`forward` function is inspected by the distributed data parallel ",
        "wrapper to figure out if any of the module's parameters went ",
        "unused. For unused parameters, DDP would not expect gradients from ",
        "then. However, if an unused parameter becomes part of the autograd ",
        "graph at a later point in time (e.g., in a reentrant backward when ",
        "using `checkpoint`), the gradient will show up unexpectedly. If all ",
        "parameters in the model participate in the backward pass, you can ",
        "disable unused parameter detection by passing the keyword argument ",
        "`find_unused_parameters=False` to ",
        "`torch.nn.parallel.DistributedDataParallel`.");
    TORCH_CHECK(!has_marked_unused_parameters_, common_error);
  }

  if (bucket.expect_sparse_gradient) {
    mark_variable_ready_sparse(index);
  } else {
    mark_variable_ready_dense(index);
  }

  // TODO(@pietern): Make this work for both CPU/CUDA tensors.
  // When using CPU tensors we don't need to do this.
  // // Record event so that we can wait for all of them.
  // auto& event = replica.events[bucket_index.intra_bucket_index];
  // event.record();

  // Check if this was the final gradient for this bucket.
  if (--replica.pending == 0) {
    // Kick off reduction if all replicas for this bucket are ready.
    if (--bucket.pending == 0) {
      mark_bucket_ready(bucket_index.bucket_index);
    }
  }

  // Run finalizer function and kick off reduction for local_used_maps once the
  // final bucket was marked ready.
  if (next_bucket_ == buckets_.size()) {
    // See Note [Skip allreducing local_used_maps_dev]
    if (find_unused_parameters_) {
      // H2D from local_used_maps_ to local_used_maps_dev_
      for (size_t i = 0; i < local_used_maps_.size(); i++) {
        // We do async H2D to avoid the blocking overhead. The async copy and
        // allreduce respect the current stream, so will be sequenced correctly.
        local_used_maps_dev_[i].copy_(local_used_maps_[i], true);
      }
      local_used_work_ = process_group_->allreduce(local_used_maps_dev_);
    }

    // The autograd engine uses the default stream when running callbacks, so we
    // pass in the current CUDA stream in case it is not the default.
    c10::DeviceType deviceType = replica.contents.device().type();
    const c10::impl::VirtualGuardImpl guard =
        c10::impl::VirtualGuardImpl{deviceType};
    const c10::Stream currentStream =
        guard.getStream(replica.contents.device());
    torch::autograd::Engine::get_default_engine().queue_callback([=] {
      std::lock_guard<std::mutex> lock(this->mutex_);
      // Run callback with the current stream
      c10::OptionalStreamGuard currentStreamGuard{currentStream};
      this->finalize_backward();
    });
  }
}

// Called when the bucket at the specified index is ready to be reduced.
void Reducer::mark_bucket_ready(size_t bucket_index) {
  TORCH_INTERNAL_ASSERT(bucket_index >= next_bucket_);

  // Buckets are reduced in sequence. Ignore this bucket if
  // it's not its turn to be reduced.
  if (bucket_index > next_bucket_) {
    return;
  }

  // Keep going, until we either:
  // - have kicked off reduction for all buckets, or
  // - found a bucket that's not yet ready for reduction.
  for (; next_bucket_ < buckets_.size() && buckets_[next_bucket_].pending == 0;
       next_bucket_++) {
    auto& bucket = buckets_[next_bucket_];
    std::vector<at::Tensor> tensors;
    tensors.reserve(bucket.replicas.size());
    for (const auto& replica : bucket.replicas) {
      // TODO(@pietern): Ensure proper synchronization with the CUDA events
      // that recorded copies into this contents tensor. If these copies are
      // executed on non-default streams, the current stream for the device
      // that holds the contents tensor must wait on these events.
      //
      // As long as autograd uses the default stream for every device,
      // these operations are implicitly sequenced, and we don't need to
      // do any extra synchronization here.
      //
      tensors.push_back(replica.contents);
    }
    // See Note [DDP Communication Hook]
    // TODO(@sinannasir): merge `work` and `future_work`. Related to GH Issue
    // #41266.
    if (comm_hook_ == nullptr) {
      bucket.work = process_group_->allreduce(tensors);
    } else {
      bucket.future_work = comm_hook_->runHook(GradBucket(tensors));
    }
  }
}

void Reducer::initialize_buckets(
    std::vector<std::vector<size_t>> bucket_indices) {
  // If initialize_buckets is called inside DDP constructor, then
  // it does not matter rpc context ptr is nullptr or not, as grad
  // will not be mutated.
  // If initialize_buckets is called during training loop, e.g, inside
  // rebuild_buckets(), since grad could be mutated and be pointed to
  // bucket_view, then it needs to check rpc context ptr is nullptr or not,
  // If rpc context ptr is nullptr, mutate variable.grad(); otherwise,
  // mutate grad in rpc context.
  using torch::distributed::autograd::ThreadLocalDistAutogradContext;
  this->rpc_context_.set(ThreadLocalDistAutogradContext::getContextPtr());

  // This shouldn't be called if we're expecting autograd hooks to fire.
  TORCH_CHECK(
      !expect_autograd_hooks_,
      "`initialize_buckets` must NOT be called during autograd execution.");

  // Clear current bucket assignment.
  buckets_.clear();
  variable_locators_.clear();

  // Ensure we have a bucket index for every variable.
  variable_locators_.resize(replicas_[0].size());

  // Iterate over buckets.
  const auto bucket_count = bucket_indices.size();
  const auto replica_count = replicas_.size();
  buckets_.reserve(bucket_count);
  for (size_t bucket_index = 0; bucket_index < bucket_count; bucket_index++) {
    Bucket bucket;

    // TODO(@pietern): Validate indices.
    // Must be non-empty, unique, and unique across buckets.
    TORCH_CHECK(
        bucket_indices[bucket_index].size() > 0, "Empty bucket specified.");

    // Variables that expect sparse gradients must have their own bucket.
    if (bucket_indices[bucket_index].size() == 1) {
      const auto variable_index = bucket_indices[bucket_index].front();
      bucket.expect_sparse_gradient =
          expect_sparse_gradients_[0][variable_index];
    } else {
      for (const auto variable_index : bucket_indices[bucket_index]) {
        TORCH_CHECK(
            !expect_sparse_gradients_[0][variable_index],
            "Buckets with more than one variable cannot include variables ",
            "that expect a sparse gradient.");
      }
    }

    // Iterate over model replicas.
    for (size_t replica_index = 0; replica_index < replica_count;
         replica_index++) {
      BucketReplica replica;

      if (bucket.expect_sparse_gradient) {
        const auto variable_index = bucket_indices[bucket_index].front();
        const auto& variable = replicas_[replica_index][variable_index];
        TORCH_INTERNAL_ASSERT(bucket_indices[bucket_index].size() == 1);
        replica.variables = {variable};
      } else {
        at::TensorOptions options;
        size_t offset = 0;

        // Iterate over bucket variables.
        for (const auto variable_index : bucket_indices[bucket_index]) {
          TORCH_CHECK(
              variable_index < replicas_[replica_index].size(),
              "Out of range variable index specified.");
          const auto& variable = replicas_[replica_index][variable_index];
          if (!options.has_device()) {
            options = options.device(variable.device());
          } else {
            TORCH_CHECK(
                variable.device() == options.device(),
                "All parameters in a bucket must be ",
                "placed on the same device.");
          }
          if (!options.has_dtype()) {
            options = options.dtype(variable.dtype());
          } else {
            TORCH_CHECK(
                variable.dtype() == options.dtype(),
                "All parameters in a bucket must have the same dtype.");
          }
          const auto length = variable.numel();
          replica.variables.push_back(variable);
          replica.offsets.push_back(offset);
          replica.lengths.push_back(length);
          offset += length;
        }

        // Allocate bucket contents tensor.
        replica.contents = at::empty({static_cast<long>(offset)}, options);
        // Note:  "Gradient Layout Contract"
        //
        // Here, create views into the contents tensor for each variable's grad.
        // Views serve as entry points to copy_ each grad's data in/out of the
        // flat contents tensor.
        //
        // Gradients may have dense memory but non-row-major-contiguous strides
        // (e.g. channels_last or channels_last_3d). For coalesced accesses
        // during copy_s, it's beneficial for each view's layout to match its
        // grad's layout.
        //
        // Specifically, we expect torch/csrc/autograd/AccumulateGrad.h produces
        // grads that obey there "Gradient Layout Contract":
        //   (1) if variable.is_non_overlapping_and_dense(), the stashed grad's
        //       strides match variable.
        //   (2) else, stashed grad is rowmajor contiguous.
        // and create views to match.
        //
        // If AccumulateGrad breaks the contract, and produces a grad with an
        // unexpected layout, performance will degrade due to poor memory access
        // patterns when copy_ing grad data in and out of its bucket view.
        // However, numerics remain correct, because the bucket view is the same
        // on either end of the raw allreduce.  bucket_view.copy(grad) tranposes
        // (+ densifies) to the bucket view's layout, the data is allreduced,
        // then grad.copy_(bucket_view) transposes it back to grad's layout.
        //
        // The only way the numerics can go haywire is if the bucket views
        // themselves have different layouts across processes (or replicas).
        // Bucket views' sizes and strides are set based on param layouts, using
        // the same logic that (we expect) AccumulateGrad uses for their grads.
        // Therefore, the only way a bucket view could have different layouts in
        // different processes is if its param has a different layout in
        // different processes. We can check that param layouts match across
        // processes and replicas in Reducer's constructor by allreducing some
        // metadata.  Checking just once won't catch if someone messes with
        // param layouts over time, but not messing with params after DDP
        // construction is already a documented constraint.
        initialize_bucket_views(replica, replica.contents, true);
      }

      // Add bucket replica to enclosing bucket.
      bucket.replicas.push_back(std::move(replica));
    }

    // Map participating variables to this bucket.
    // This is identical across replicas so we only need to do this once.
    size_t intra_bucket_index = 0;
    for (const auto variable_index : bucket_indices[bucket_index]) {
      TORCH_CHECK(
          variable_index < variable_locators_.size(),
          "Out of range variable index specified.");
      variable_locators_[variable_index] = VariableLocator{
          .bucket_index = bucket_index,
          .intra_bucket_index = intra_bucket_index++,
      };
    }
    bucket.variable_indices = std::move(bucket_indices[bucket_index]);

    buckets_.push_back(std::move(bucket));
  }
}

// (see Note:  "Gradient Layout Contract" in initialize_buckets).
void Reducer::initialize_bucket_views(
    Reducer::BucketReplica& replica,
    at::Tensor& contents,
    bool copy_to_bucket_view) {
  for (size_t i = 0; i < replica.variables.size(); i++) {
    auto& v = replica.variables[i];
    const auto offset = replica.offsets[i];
    const auto length = replica.lengths[i];
    at::Tensor bucket_view;
    if (v.is_non_overlapping_and_dense()) {
      // If the param's memory is dense, match its layout, anticipating
      // the autograd engine (AccumulateGrad) will also create gradients
      // matching its layout.
      bucket_view = contents.as_strided(v.sizes(), v.strides(), offset);
    } else {
      // Fall back to a C-style contiguous view, again anticipating
      // AccumulateGrad will do the same when stashing grads for non-dense
      // params.
      bucket_view = contents.narrow(0, offset, length).view(v.sizes());
    }
    replica.bucket_views.push_back(bucket_view);
    // There are three cases to handle:
    // 1. initialize_bucket_views could be called inside communication hook,
    // bucket_view has the updated results in new tensor, just let grad point to
    // bucket_view, copy_to_bucket_view is false in this case.
    // 2. initialize_bucket_views could be called inside initialize_buckets when
    // rebuild_buckets, if grad has already been defined/calculated in previous
    // iteration, old grad needs to be copied into new bucket_view
    // and let grad point to the new bucket_view,
    // copy_to_bucket_view is true in this case.
    // 3. initialize_bucket_views could be called inside initialize_buckets
    // during construction. copy_to_bucket_view is true in this case. But mostly
    // grads are not defined during construction time, when grad is not defined,
    // do not let grad point to bucket_view, because grads should be kept as
    // being undefined for globally unused parameters.
    runGradCallbackForVariable(v, [&](auto& grad) {
      if (grad.defined() && !grad.is_alias_of(bucket_view)) {
        if (copy_to_bucket_view) {
          bucket_view.copy_(grad);
        }
        grad = bucket_view;
        // The grad is modefied and needs to be written back.
        return true;
      }
      // The grad is not modified and does not need to be written back.
      return false;
    });
  }
}

void Reducer::prepare_forward() {
  std::lock_guard<std::mutex> lock(mutex_);
  rebuild_buckets();
}

// Traverse the autograd graph starting at the specified output.
// All parameters for which we have a pointer to their gradient accumulation
// functions, but don't show up in the autograd graph will be marked ready for
// for reduction as soon as the first autograd hook is called. This is not
// done immediately because the model output may be ignored, and we only
// want to start performing reductions on `torch.autograd.backward()`.
void Reducer::prepare_for_backward(
    const std::vector<torch::autograd::Variable>& outputs) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::unordered_set<torch::autograd::Node*> seen;
  std::vector<torch::autograd::Node*> queue;

  // Check that any prior reduction has finished.
  // The variable `require_finalize_` is true until all gradients
  // have been computed and reduction of all buckets has been kicked off.
  if (require_finalize_) {
    TORCH_CHECK(
        false,
        "Expected to have finished reduction in the prior iteration before ",
        "starting a new one. ",
        "",
        "This error indicates that your module has parameters that were ",
        "not used in producing loss. ",
        "",
        "You can enable unused parameter detection by (1) passing the keyword "
        "argument `find_unused_parameters=True` to ",
        "`torch.nn.parallel.DistributedDataParallel`; (2) making sure all ",
        "`forward` function outputs participate in calculating loss. "
        "",
        "If you already have done the above two steps, then the distributed ",
        "data parallel module wasn't able to locate the output tensors in the ",
        "return value of your module's `forward` function. ",
        "Please include the loss function and the structure of the return ",
        "value of `forward` of your module when reporting this issue (e.g. ",
        "list, dict, iterable).");
  }

  // Reset accounting.
  expect_autograd_hooks_ = true;
  next_bucket_ = 0;
  backward_stats_base_ = current_time_in_nanos();
  for (auto& bucket : buckets_) {
    for (auto& replica : bucket.replicas) {
      replica.pending = replica.variables.size();
    }
    bucket.pending = bucket.replicas.size();
  }

  // Reset unused parameter accounting.
  has_marked_unused_parameters_ = false;
  unused_parameters_.clear();

  // If find_unused_parameters_ is false, we assume that autograd hooks for ALL
  // variables will be called, and we don't have to search the autograd graph
  // for presence of these hooks.
  if (!find_unused_parameters_) {
    return;
  }

  // Seed queue with the grad functions of all outputs.
  for (const auto& output : outputs) {
    const auto& grad_fn = output.grad_fn();
    if (grad_fn) {
      queue.push_back(grad_fn.get());
    }
  }

  // Traverse the autograd graph starting at the specified output.
  while (!queue.empty()) {
    auto fn = queue.back();
    queue.pop_back();
    for (const auto& edge : fn->next_edges()) {
      if (auto next_ptr = edge.function.get()) {
        const bool was_inserted = seen.insert(next_ptr).second;
        if (was_inserted) {
          queue.push_back(next_ptr);
        }
      }
    }
  }

  // Find accumulator functions that don't show up in this graph.
  for (const auto& it : func_) {
    // If the accumulator function is present in the graph, we know
    // a gradient will be computed for the corresponding parameter.
    if (seen.count(it.first) > 0) {
      continue;
    }

    unused_parameters_.push_back(it.second);
  }
}

// A bucket with one or more dense tensors needs to be unflattened.
void Reducer::finalize_bucket_dense(Bucket& bucket) {
  for (size_t replica_index = 0; replica_index < bucket.replicas.size();
       replica_index++) {
    auto& replica = bucket.replicas[replica_index];
    for (size_t intra_bucket_index = 0;
         intra_bucket_index < replica.variables.size();
         intra_bucket_index++) {
      auto& variable = replica.variables[intra_bucket_index];
      const auto offset = replica.offsets[intra_bucket_index];
      const auto length = replica.lengths[intra_bucket_index];

      bool global_unused = false;
      // See Note [Skip allreducing local_used_maps_dev]
      if (find_unused_parameters_) {
        // Determine if this param has been used globally or not.
        //
        // If the variable was used locally, it is also used globally and then
        // we don't need to wait for the reduction. Otherwise we lazily wait for
        // the reduction to complete, only when we see a variable that was
        // unused locally. Then we end up delaying the synchronization point
        // that local_used_work_->wait() implies. If we don't have any unused
        // parameters at all, we can skip waiting for the work to complete
        // altogether, and cause negligible performance overhead for models
        // where all parameters are used. Such lazily waiting means minimizing
        // performance impact for the big majority of models where all
        // parameters are always used. Then we only pay the overhead cost if
        // there is indeed a parameter that is locally unused, because we need
        // to check if it's also globally unused.
        size_t variable_index = bucket.variable_indices[intra_bucket_index];
        // Note: global_unused might not be global yet. As we lazily wait for
        // the reduction to complete, it becomes really global only if we get to
        // the point as below where we wait for the reduction work, make D2H
        // copy, and update global_unused with the real global consensus, i.e.
        // local_used_maps_reduced_ is true.
        global_unused =
            local_used_maps_[replica_index][variable_index].item<int>() == 0;
        if (global_unused && !local_used_maps_reduced_) {
          // Wait for local_used_maps reduction to complete.
          local_used_work_->wait();
          // D2H from local_used_maps_dev_ to local_used_maps_
          for (size_t i = 0; i < local_used_maps_.size(); i++) {
            local_used_maps_[i].copy_(local_used_maps_dev_[i]);
          }
          global_unused =
              local_used_maps_[replica_index][variable_index].item<int>() == 0;
          local_used_maps_reduced_ = true;
        }
      }

      const auto& bucket_view = replica.bucket_views[intra_bucket_index];
      runGradCallbackForVariable(variable, [&](auto& grad) {
        // If a parameter is globally unused, we keep its grad untouched.
        if (!global_unused) {
          // If grad is globally used but locally unused, let grad point to
          // bucket_view
          if (!grad.defined()) {
            grad = bucket_view;
          } else {
            TORCH_INTERNAL_ASSERT(
                grad.is_alias_of(bucket_view),
                "Grad should have been pointed to bucket_view if grad is defined");
          }
          // The grad is modified and needs to be written back.
          return true;
        }
        // The grad is not modified.
        return false;
      });
    }
  }
}

void Reducer::finalize_backward() {
  // No longer expect autograd hooks to fire after this function returns.
  TORCH_INTERNAL_ASSERT(expect_autograd_hooks_);
  expect_autograd_hooks_ = false;

  // No longer require call to finalize after this function returns.
  TORCH_INTERNAL_ASSERT(require_finalize_);
  require_finalize_ = false;

  // Check that all buckets were completed and had their work kicked off.
  TORCH_INTERNAL_ASSERT(next_bucket_ == buckets_.size());

  // Wait for asynchronous reduction to complete and unflatten contents.
  for (auto& bucket : buckets_) {
    // See Note [DDP Communication Hook]
    if (comm_hook_ == nullptr) {
      TORCH_INTERNAL_ASSERT(
          bucket.work,
          "Expected bucket.work not to be null. "
          "This may indicate that allreduce hooks were not properly installed.");
      bucket.work->wait();
    } else {
      TORCH_INTERNAL_ASSERT(
          bucket.future_work,
          "Expected bucket.future_work not to be null. "
          "This may indicate that communication hook was not properly installed.");
      bucket.future_work->wait();

      auto future_result =
          comm_hook_->processFuture(bucket.future_work->value());

      for (size_t i = 0; i < future_result.size(); i++) {
        if (bucket.expect_sparse_gradient) {
          bucket.replicas[i].contents.copy_(future_result[i]);
        } else {
          // Reinitialize bucket_views with the future_result by following
          // the same logic in `inititalize_buckets`.
          bucket.replicas[i].bucket_views.clear();
          initialize_bucket_views(bucket.replicas[i], future_result[i], false);
        }
      }
    }
    if (!bucket.expect_sparse_gradient) {
      // We don't need to finalize the sparse bucket since the sparse grad and
      // the bucket essentially point to the same storage. As a result, once
      // the allreduce is done, the sparse grads are automatically updated.
      finalize_bucket_dense(bucket);
    }
  }

  // See Note [Skip allreducing local_used_maps_dev]
  if (find_unused_parameters_) {
    // Reset unused parameter accounting.
    for (auto& local_used : local_used_maps_) {
      local_used.fill_(0);
    }
    // Due to the lazy wait, it is possible that reduction of the current
    // iteration is still going when the one for next iteration gets kicked off.
    // For such case, we want to wait explicitly to make sure the reduction does
    // complete before kicking off next one. Otherwise the previous one may
    // interfere, write to the device-side memory and clobber the content of
    // local_unused_maps_dev_.
    if (!local_used_maps_reduced_) {
      local_used_work_->wait();
    }
    local_used_maps_reduced_ = false;
  }
}

void Reducer::runGradCallbackForVariable(
    torch::autograd::Variable& variable,
    GradCallback&& cb) {
  auto context_ptr = rpc_context_.context_ptr.load();
  if (context_ptr == nullptr) {
    cb(variable.mutable_grad());
  } else {
    // Under distributed autograd
    context_ptr->runGradCallbackForVariable(variable, std::move(cb));
  }
}

void Reducer::RpcContext::set(ContextPtr&& new_context_ptr) {
  // We should set 'new_context_ptr' even if it's nullptr. That means the
  // reducer is under a local backward run.
  const auto new_context_raw_ptr = new_context_ptr.get();
  if (context_ptr.exchange(new_context_raw_ptr) != new_context_raw_ptr) {
    // Set the shared ptr to the context only if it's set first time.
    // All call sites should use the same context ptr.
    // Use an atomic to avoid data race from multiple threads.
    context_ptr_holder = std::move(new_context_ptr);
  }
}

void Reducer::sync_bucket_indices(
    std::vector<std::vector<size_t>>& bucket_indices) {
  auto num_buckets = bucket_indices.size();
  std::vector<size_t> bucket_sizes;
  bucket_sizes.reserve(num_buckets);
  int64_t total_size = 0;
  for (size_t i = 0; i < num_buckets; i++) {
    auto bucket_size = bucket_indices.at(i).size();
    bucket_sizes.push_back(bucket_size);
    total_size += bucket_size;
  }

  at::TensorOptions options;
  options = options.dtype(at::kInt);
  options = options.device(replicas_[0][0].device());

  // Group indices and num_bucket together into indices_tensor
  // Broadcast this tensor first, as its size is equal among all processes
  auto indices_tensor = at::empty({total_size + 1}, at::kInt);
  auto indices_accessor = indices_tensor.accessor<int, 1>();
  auto indices_accessor_Index = 0;
  for (size_t i = 0; i < num_buckets; i++) {
    const auto& bucket_size = bucket_indices.at(i).size();
    for (size_t j = 0; j < bucket_size; j++) {
      indices_accessor[indices_accessor_Index++] = bucket_indices[i][j];
    }
  }
  indices_accessor[indices_accessor_Index] = num_buckets;

  // Copy CPU tensor to device tensor, as the process_group_ could be NCCL and
  // it can only broadcast device tensors.
  auto indices_tensor_device = at::empty({total_size + 1}, options);
  indices_tensor_device.copy_(indices_tensor, /*non_blocking=*/true);
  std::vector<at::Tensor> indices_tensor_list = {indices_tensor_device};
  process_group_->broadcast(indices_tensor_list)->wait();
  indices_tensor.copy_(indices_tensor_list.front(), /*non_blocking=*/false);

  // Update num_buckets after receiving it from rank 0
  num_buckets = indices_accessor[indices_accessor_Index];

  // Broadcast bucket_sizes
  auto bucket_sizes_tensor = at::empty({(int64_t)num_buckets}, at::kInt);
  auto bucket_sizes_accessor = bucket_sizes_tensor.accessor<int, 1>();
  for (size_t i = 0; i < num_buckets; i++) {
    // For rank != 0, it is possible that local num buckets bucket_sizes.size()
    // is smaller than broadcasted num_buckets
    bucket_sizes_accessor[i] =
        bucket_sizes.at(std::min(i, (bucket_sizes.size() - 1)));
  }
  auto bucket_sizes_tensor_device = at::empty({(int64_t)num_buckets}, options);
  bucket_sizes_tensor_device.copy_(bucket_sizes_tensor, /*non_blocking=*/true);
  std::vector<at::Tensor> bucket_sizes_tensor_list = {
      bucket_sizes_tensor_device};
  process_group_->broadcast(bucket_sizes_tensor_list)->wait();
  bucket_sizes_tensor.copy_(
      bucket_sizes_tensor_list.front(), /*non_blocking=*/false);

  // Clear bucket_indices first, and then update bucket_indices using received
  // num_buckets, bucket_sizes_tensor and indices_tensor from rank 0
  bucket_indices.clear();
  bucket_indices.reserve(num_buckets);
  indices_accessor_Index = 0;
  for (size_t i = 0; i < num_buckets; i++) {
    const auto& bucket_size = bucket_sizes_accessor[i];
    std::vector<size_t> bucket;
    bucket.reserve(bucket_size);
    for (size_t j = 0; j < bucket_size; j++) {
      bucket.push_back(indices_accessor[indices_accessor_Index++]);
    }
    bucket_indices.emplace_back(std::move(bucket));
  }
}

void Reducer::rebuild_buckets() {
  if (rebuilt_params_.empty()) {
    return;
  }

  TORCH_INTERNAL_ASSERT(
      rebuilt_params_.size() == rebuilt_param_indices_.size(),
      "rebuilt parameter tensors size is not same as rebuilt parameter indices size.");
  TORCH_INTERNAL_ASSERT(
      replicas_[0].size() == rebuilt_param_indices_.size(),
      "rebuilt parameter indices size is not same as original model parameters size.");
  std::vector<std::vector<size_t>> rebuilt_bucket_indices;
  std::vector<size_t> bucket_size_limits;
  bucket_size_limits.push_back(kDefaultFirstBucketBytes);
  bucket_size_limits.push_back(bucket_bytes_cap_);
  rebuilt_bucket_indices = compute_bucket_assignment_by_size(
      rebuilt_params_,
      bucket_size_limits,
      expect_sparse_gradients_[0],
      rebuilt_param_indices_);

  // For rebuilt bucket indices, it needs to be synced across all ranks.
  // Broadcast the newly rebuilt bucket indices from rank 0 in default.
  // After syncing up rebuilt bucket indices, initialize buckets for reducer.
  sync_bucket_indices(rebuilt_bucket_indices);

  has_rebuilt_bucket_ = true;
  rebuilt_params_.clear();
  rebuilt_param_indices_.clear();

  initialize_buckets(std::move(rebuilt_bucket_indices));
}

// See Note [DDP Communication Hook]
void Reducer::register_comm_hook(std::unique_ptr<CommHookInterface> iface) {
  TORCH_CHECK(
      comm_hook_ == nullptr, "register_comm_hook can only be called once.");
  // TODO(@sinannasir): Single-process multiple-device mode support for DDP
  // communication hook. Related to GH Issue #42542.
  TORCH_CHECK(
      replicas_.size() == 1,
      "Communication hook does not support single-process multiple-device mode.");

  comm_hook_ = std::move(iface);
}

namespace {

// Tensors may be coalesced into buckets. Buckets must contain tensors of
// the same type, on the same device, so a bucket can identified by a
// composite key of a tensor's type identifier and its device.
struct BucketKey {
  BucketKey(c10::ScalarType type, c10::Device device)
      : type(std::move(type)), device(std::move(device)) {}

  const c10::ScalarType type;
  const c10::Device device;

  // See torch/csrc/utils/hash.h for dispatch code.
  static size_t hash(const BucketKey& key) {
    return torch::get_hash(key.type, key.device);
  }
};

inline bool operator==(const BucketKey& lhs, const BucketKey& rhs) {
  return lhs.type == rhs.type && lhs.device == rhs.device;
}

} // namespace

// This is equivalent to take_tensors but returns indices into the
// tensor list argument for bucket assignment. Also, it is aware
// of device placement and will not allow buckets to span devices.
// The index of tensors[i] assigned to bucket is tensor_indices[i],
// when tensor_indices is empty, the index of tensors[i] assigned to
// bucket is i.
std::vector<std::vector<size_t>> compute_bucket_assignment_by_size(
    const std::vector<at::Tensor>& tensors,
    const std::vector<size_t>& bucket_size_limits,
    const std::vector<bool>& expect_sparse_gradient,
    const std::vector<int64_t>& tensor_indices) {
  // Either expect_sparse_gradient is not specified or it has as many elements
  // as the vector with tensors.
  TORCH_INTERNAL_ASSERT(
      expect_sparse_gradient.empty() ||
      (tensors.size() == expect_sparse_gradient.size()));
  TORCH_INTERNAL_ASSERT(tensors.size() > 0);

  std::vector<std::vector<size_t>> result;
  result.reserve(tensors.size());

  // Keep iterator into the size_limit vector by tensor type and device.
  // This is done so that we can use the consecutive bucket limits per type.
  std::unordered_map<
      BucketKey,
      std::vector<size_t>::const_iterator,
      torch::hash<BucketKey>>
      bucket_size_limit_iterators;

  // Local accumulator type for a single bucket.
  struct BucketAccumulator {
    std::vector<size_t> indices;
    size_t size = 0;
  };

  // Keep vector of indices and size accumulator by tensor type and device.
  std::unordered_map<BucketKey, BucketAccumulator, torch::hash<BucketKey>>
      buckets;

  for (size_t i = 0; i < tensors.size(); i++) {
    const auto& tensor = tensors[i];
    TORCH_CHECK(!tensor.is_sparse(), "No support for sparse tensors.");

    // when tensor_indices is empty, the index of tensors[i] assigned to
    // bucket is i, otherwise the tensor index is tensor_indices[i].
    auto tensor_index = i;
    if (!tensor_indices.empty()) {
      tensor_index = tensor_indices[i];
    }
    // If we expect a sparse gradient to be produced for this tensor, it cannot
    // be grouped together with other gradients and gets its own bucket.
    if (!expect_sparse_gradient.empty() &&
        expect_sparse_gradient[tensor_index]) {
      result.push_back({tensor_index});
      continue;
    }

    auto key = BucketKey(tensor.scalar_type(), tensor.device());
    auto& bucket = buckets[key];
    bucket.indices.push_back(tensor_index);
    bucket.size += tensor.numel() * tensor.element_size();

    // Initialize bucket size limit iterator if necessary.
    if (bucket_size_limit_iterators.count(key) == 0) {
      bucket_size_limit_iterators[key] = bucket_size_limits.begin();
    }

    auto& bucket_size_limit_iterator = bucket_size_limit_iterators[key];
    const auto bucket_size_limit = *bucket_size_limit_iterator;
    if (bucket.size >= bucket_size_limit) {
      result.emplace_back(std::move(bucket.indices));
      bucket = BucketAccumulator();

      // Advance to the next bucket size limit for this type/device.
      auto next = bucket_size_limit_iterator + 1;
      if (next != bucket_size_limits.end()) {
        bucket_size_limit_iterator = next;
      }
    }
  }

  // Add remaining buckets.
  for (auto& it : buckets) {
    auto& bucket = it.second;
    if (!bucket.indices.empty()) {
      result.emplace_back(std::move(bucket.indices));
    }
  }

  // If tensor_indices is not empty, the order of the tensors is in the gradient
  // ready order, so no need to sort.
  // If tensor_indices is empty, sort resulting buckets by the minimum tensor
  // index they include. We assume that the order of the tensors is the order in
  // which they are used (or the reverse order in which their gradients are
  // produced). This sorting step ensures that the buckets are ready in
  // consecutive order.
  if (tensor_indices.empty()) {
    std::sort(
        result.begin(),
        result.end(),
        [](const std::vector<size_t>& a, const std::vector<size_t>& b) {
          const auto amin = std::min_element(a.begin(), a.end());
          const auto bmin = std::min_element(b.begin(), b.end());
          return *amin < *bmin;
        });
  }

  return result;
}

} // namespace c10d
