/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/kernels/dataset.h"

#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/tensor.h"

namespace tensorflow {

namespace {

// See documentation in ../ops/dataset_ops.cc for a high-level
// description of the following op.

class DenseToSparseBatchDatasetOp : public UnaryDatasetOpKernel {
 public:
  explicit DenseToSparseBatchDatasetOp(OpKernelConstruction* ctx)
      : UnaryDatasetOpKernel(ctx) {}

  void MakeDataset(OpKernelContext* ctx, DatasetBase* input,
                   DatasetBase** output) override {
    // Create a new DenseToSparseBatchDatasetOp::Dataset, insert it in the
    // step-local container, and return it as the output.
    OP_REQUIRES(
        ctx, input->output_dtypes().size() == 1,
        errors::InvalidArgument("DenseToSparseBatchDataset only supports "
                                "inputs with a single component."));

    int64 batch_size;
    OP_REQUIRES_OK(ctx,
                   ParseScalarArgument<int64>(ctx, "batch_size", &batch_size));
    OP_REQUIRES(
        ctx, batch_size > 0,
        errors::InvalidArgument("Batch size must be greater than zero."));

    const Tensor* row_shape_t;
    OP_REQUIRES_OK(ctx, ctx->input("row_shape", &row_shape_t));
    OP_REQUIRES(ctx, TensorShapeUtils::IsVector(row_shape_t->shape()),
                errors::InvalidArgument("row_shape must be a vector"));
    TensorShape row_shape;
    for (size_t i = 0; i < row_shape_t->dim_size(0); ++i) {
      row_shape.AddDim(row_shape_t->vec<int64>()(i));
    }

    *output = nullptr;

#define HANDLE_TYPE(T)                                      \
  case DataTypeToEnum<T>::value: {                          \
    *output = new Dataset<T>(batch_size, row_shape, input); \
    break;                                                  \
  }

    switch (input->output_dtypes()[0]) {
      TF_CALL_DATASET_TYPES(HANDLE_TYPE);
#undef HANDLE_TYPE
      default:
        OP_REQUIRES(ctx, false,
                    errors::Unimplemented(
                        "DenseToSparseBatchDataset unhandled data type: ",
                        input->output_dtypes()[0]));
    }
  }

 private:
  // TODO(mrry): Push the templated code down to the raw copying routine.
  template <class T>
  class Dataset : public DatasetBase {
   public:
    Dataset(int64 batch_size, const TensorShape& row_shape,
            const DatasetBase* input)
        : batch_size_(batch_size), row_shape_(row_shape), input_(input) {
      input_->Ref();

      output_shapes_.reserve(3);
      // Outputs represent a SparseTensor as (indices, values, dense_shape).
      output_shapes_.push_back({-1, row_shape_.dims() + 1});
      output_shapes_.push_back({-1});
      output_shapes_.push_back({row_shape_.dims() + 1});
    }

    ~Dataset() override { input_->Unref(); }

    std::unique_ptr<IteratorBase> MakeIterator(
        const string& prefix) const override {
      return std::unique_ptr<IteratorBase>(new Iterator(
          {this, strings::StrCat(prefix, "::DenseToSparseBatch")}));
    }

    const DataTypeVector& output_dtypes() const override {
      static DataTypeVector* output_dtypes_ =
          new DataTypeVector({DT_INT64, DataTypeToEnum<T>::value, DT_INT64});
      return *output_dtypes_;
    }

    const std::vector<PartialTensorShape>& output_shapes() const override {
      return output_shapes_;
    }

    string DebugString() override {
      return strings::StrCat("DenseToSparseBatchDatasetOp(", batch_size_,
                             ")::Dataset");
    }

   private:
    class Iterator : public DatasetIterator<Dataset<T>> {
     public:
      explicit Iterator(const typename Iterator::Params& params)
          : DatasetIterator<Dataset<T>>(params),
            input_impl_(params.dataset->input_->MakeIterator(params.prefix)) {}

      Status GetNextInternal(IteratorContext* ctx,
                             std::vector<Tensor>* out_tensors,
                             bool* end_of_sequence) override {
        // Each row of the output SparseTensor is an individual tensor
        // from the input iterator.
        std::vector<Tensor> batch_elements;
        int64 total_elements = 0;
        batch_elements.reserve(
            DatasetIterator<Dataset<T>>::dataset()->batch_size_);
        const TensorShape& row_shape =
            DatasetIterator<Dataset<T>>::dataset()->row_shape_;
        const int row_ndims = row_shape.dims();
        {
          mutex_lock l(mu_);
          *end_of_sequence = false;
          for (int i = 0;
               i < DatasetIterator<Dataset<T>>::dataset()->batch_size_ &&
               !*end_of_sequence;
               ++i) {
            std::vector<Tensor> batch_element_tuple;
            TF_RETURN_IF_ERROR(input_impl_->GetNext(ctx, &batch_element_tuple,
                                                    end_of_sequence));
            if (!*end_of_sequence) {
              DCHECK_EQ(1, batch_element_tuple.size());
              batch_elements.push_back(std::move(batch_element_tuple[0]));
              total_elements += batch_element_tuple[0].NumElements();

              // TODO(mrry): Investigate how to hoist this check when we
              // have static information that renders it unnecessary.
              if (batch_element_tuple[0].shape().dims() != row_ndims) {
                return errors::InvalidArgument(
                    "Input element had shape (",
                    batch_element_tuple[0].shape().DebugString(),
                    ") that is incompatible with the row shape (",
                    row_shape.DebugString(), ").");
              }
              for (int i = 0; i < row_ndims; ++i) {
                if (batch_element_tuple[0].shape().dim_size(i) >
                    row_shape.dim_size(i)) {
                  return errors::DataLoss(
                      "Input element had shape (",
                      batch_element_tuple[0].shape().DebugString(),
                      ") that is larger than the row shape (",
                      row_shape.DebugString(), ").");
                }
              }
            }
          }
        }

        if (batch_elements.empty()) {
          DCHECK(*end_of_sequence);
          return Status::OK();
        }

        // Determine the size of the output tensors:
        // * indices will be [`total_elements`, `row_shape + 1`].
        // * values will be [`total_elements`].
        // * dense_shape will be [`row_shape + 1`].
        Tensor indices(cpu_allocator(), DT_INT64,
                       {total_elements, row_ndims + 1});
        Tensor values(
            cpu_allocator(),
            DatasetIterator<Dataset<T>>::dataset()->output_dtypes()[1],
            {total_elements});
        Tensor dense_shape(cpu_allocator(), DT_INT64, {row_ndims + 1});
        auto indices_matrix = indices.matrix<int64>();
        auto values_flat = values.flat<T>();
        auto dense_shape_vec = dense_shape.vec<int64>();

        int64 current_position_in_values = 0;
        for (int64 i = 0; i < batch_elements.size(); ++i) {
          const Tensor& t = batch_elements[i];
          const auto& t_flat = t.flat<T>();
          // TODO(mrry): Replace with a memcpy or something more
          // efficient. (Maybe an Eigen assign op?)
          gtl::InlinedVector<int64, 4> strides(row_ndims);
          if (!strides.empty()) {
            strides[row_ndims - 1] = 1;
            for (int64_t row_dim = strides.size() - 2; row_dim >= 0;
                 --row_dim) {
              strides[row_dim] =
                  strides[row_dim + 1] * t.shape().dim_size(row_dim + 1);
            }
          }

          for (int64 j = 0; j < t.NumElements(); ++j) {
            values_flat(current_position_in_values) = t_flat(j);
            indices_matrix(current_position_in_values, 0) = i;
            int64 index = j;
            for (size_t k = 0; k < strides.size(); ++k) {
              indices_matrix(current_position_in_values, k + 1) =
                  index / strides[k];
              index %= strides[k];
            }
            ++current_position_in_values;
          }
        }

        dense_shape_vec(0) = batch_elements.size();
        for (size_t i = 0; i < row_ndims; ++i) {
          dense_shape_vec(i + 1) = row_shape.dim_size(i);
        }

        out_tensors->push_back(std::move(indices));
        out_tensors->push_back(std::move(values));
        out_tensors->push_back(std::move(dense_shape));

        *end_of_sequence = false;
        return Status::OK();
      }

     private:
      mutex mu_;
      int64 i_ GUARDED_BY(mu_);
      std::unique_ptr<IteratorBase> input_impl_ GUARDED_BY(mu_);
    };

    const int64 batch_size_;
    const TensorShape row_shape_;
    const DatasetBase* const input_;
    std::vector<PartialTensorShape> output_shapes_;
  };
};

REGISTER_KERNEL_BUILDER(Name("DenseToSparseBatchDataset").Device(DEVICE_CPU),
                        DenseToSparseBatchDatasetOp);

}  // namespace

}  // namespace tensorflow
