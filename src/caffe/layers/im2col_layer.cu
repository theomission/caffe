#include <vector>

#include "caffe/util/im2col.hpp"
#include "caffe/vision_layers.hpp"

#ifdef USE_GREENTEA
#include "caffe/greentea/greentea.hpp"
#include "caffe/greentea/greentea_im2col.hpp"
#include "caffe/greentea/greentea_math_functions.hpp"
#endif

namespace caffe {

template<typename Dtype>
void Im2colLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
                                     const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->gpu_data();
  Dtype* top_data = top[0]->mutable_gpu_data();
  const int_tp num_kernels = channels_ * top[0]->count(channel_axis_ + 1);

  if (this->device_->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
    for (int_tp n = 0; n < num_; ++n) {
      if (!force_nd_im2col_ && num_spatial_axes_ == 2) {
        im2col_gpu<Dtype>(bottom_data + n * bottom_dim_, channels_,
                   bottom[0]->shape(channel_axis_ + 1),
                   bottom[0]->shape(channel_axis_ + 2),
                   kernel_shape_.cpu_data()[0], kernel_shape_.cpu_data()[1],
                   pad_.cpu_data()[0], pad_.cpu_data()[1],
                   stride_.cpu_data()[0], stride_.cpu_data()[1],
                   top_data + n * top_dim_);
      } else {
        im2col_nd_gpu<Dtype>(bottom_data + n * bottom_dim_, num_spatial_axes_,
                      num_kernels, bottom[0]->gpu_shape() + channel_axis_,
                      top[0]->gpu_shape() + channel_axis_,
                      kernel_shape_.gpu_data(), pad_.gpu_data(),
                      stride_.gpu_data(), top_data + n * top_dim_);
      }
    }
#endif  // USE_CUDA
  } else {
#ifdef USE_GREENTEA
    viennacl::ocl::context &ctx = viennacl::ocl::get_context(
        this->device_->id());
    viennacl::ocl::program &program = Caffe::Get().GetDeviceProgram(
        this->device_->id());

    for (int_tp n = 0; n < num_; ++n) {
      if (!force_nd_im2col_ && num_spatial_axes_ == 2) {
        greentea_im2col_gpu<Dtype>(&program, &ctx, (cl_mem) bottom_data,
                                   n * bottom_dim_, channels_,
                                   bottom[0]->shape(channel_axis_ + 1),
                                   bottom[0]->shape(channel_axis_ + 2),
                                   kernel_shape_.cpu_data()[0],
                                   kernel_shape_.cpu_data()[1],
                                   pad_.cpu_data()[0], pad_.cpu_data()[1],
                                   stride_.cpu_data()[0], stride_.cpu_data()[1],
                                   (cl_mem) top_data, n * top_dim_);
      } else {
        greentea_im2col_nd_gpu<Dtype>(&program, &ctx, (cl_mem) bottom_data,
                                      n * bottom_dim_, num_spatial_axes_,
                                      channel_axis_, num_kernels,
                                      (cl_mem) (bottom[0]->gpu_shape()),
                                      (cl_mem) (top[0]->gpu_shape()),
                                      (cl_mem) (kernel_shape_.gpu_data()),
                                      (cl_mem) (pad_.gpu_data()),
                                      (cl_mem) (stride_.gpu_data()),
                                      (cl_mem) top_data, n * top_dim_);
      }
    }
#endif  // USE_GREENTEA
  }
}

template<typename Dtype>
void Im2colLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top,
                                      const vector<bool>& propagate_down,
                                      const vector<Blob<Dtype>*>& bottom) {
  const Dtype* top_diff = top[0]->gpu_diff();
  Dtype* bottom_diff = bottom[0]->mutable_gpu_diff();

  if (this->device_->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
    for (int_tp n = 0; n < num_; ++n) {
      if (!force_nd_im2col_ && num_spatial_axes_ == 2) {
        col2im_gpu<Dtype>(top_diff + n * top_dim_, channels_,
                          bottom[0]->shape(channel_axis_ + 1),
                          bottom[0]->shape(channel_axis_ + 2),
                          kernel_shape_.cpu_data()[0],
                          kernel_shape_.cpu_data()[1], pad_.cpu_data()[0],
                          pad_.cpu_data()[1], stride_.cpu_data()[0],
                          stride_.cpu_data()[1], bottom_diff + n * bottom_dim_);
      } else {
        col2im_nd_gpu<Dtype>(top_diff + n * top_dim_, num_spatial_axes_,
                             bottom_dim_,
                             bottom[0]->gpu_shape() + channel_axis_,
                             top[0]->gpu_shape() + channel_axis_,
                             kernel_shape_.gpu_data(), pad_.gpu_data(),
                             stride_.gpu_data(), bottom_diff + n * bottom_dim_);
      }
    }
#endif  // USE_CUDA
  } else {
#ifdef USE_GREENTEA
    viennacl::ocl::context &ctx = viennacl::ocl::get_context(
        this->device_->id());
    viennacl::ocl::program &program = Caffe::Get().GetDeviceProgram(
        this->device_->id());

    for (int_tp n = 0; n < top[0]->num(); ++n) {
      if (!force_nd_im2col_ && num_spatial_axes_ == 2) {
        greentea_col2im_gpu<Dtype>(&program, &ctx, (cl_mem) top_diff,
                                   n * top_dim_, channels_,
                                   bottom[0]->shape(channel_axis_ + 1),
                                   bottom[0]->shape(channel_axis_ + 2),
                                   kernel_shape_.cpu_data()[0],
                                   kernel_shape_.cpu_data()[1],
                                   pad_.cpu_data()[0], pad_.cpu_data()[1],
                                   stride_.cpu_data()[0], stride_.cpu_data()[1],
                                   (cl_mem) bottom_diff, n * bottom_dim_);
      } else {
        greentea_col2im_nd_gpu<Dtype>(&program, &ctx, (cl_mem) top_diff,
                                      n * top_dim_, num_spatial_axes_,
                                      channel_axis_, bottom_dim_,
                                      (cl_mem) (bottom[0]->gpu_shape()),
                                      (cl_mem) (top[0]->gpu_shape()),
                                      (cl_mem) (kernel_shape_.gpu_data()),
                                      (cl_mem) (pad_.gpu_data()),
                                      (cl_mem) (stride_.gpu_data()),
                                      (cl_mem) bottom_diff, n * bottom_dim_);
      }
    }
#endif  // USE_GREENTEA
  }
}

INSTANTIATE_LAYER_GPU_FUNCS(Im2colLayer);

}  // namespace caffe
