#ifndef GREENTEA_IM2COL_HPP_
#define GREENTEA_IM2COL_HPP_
#ifdef USE_GREENTEA

#include "caffe/greentea/greentea.hpp"
#include "caffe/greentea/greentea_math_functions.hpp"
#include "viennacl/ocl/backend.hpp"
#include "viennacl/ocl/context.hpp"
#include "viennacl/ocl/device.hpp"
#include "viennacl/ocl/platform.hpp"
#include "viennacl/vector.hpp"

namespace caffe {

template<typename Dtype>
void greentea_im2col_gpu(viennacl::ocl::program *prog,
                         viennacl::ocl::context *ctx, const cl_mem data_im,
                         const int_tp data_im_off, const int_tp channels,
                         const int_tp height, const int_tp width,
                         const int_tp kernel_h, const int_tp kernel_w,
                         const int_tp pad_h, const int_tp pad_w,
                         const int_tp stride_h, const int_tp stride_w,
                         cl_mem data_col, const int_tp data_col_off);

template<typename Dtype>
void greentea_col2im_gpu(viennacl::ocl::program *prog,
                         viennacl::ocl::context *ctx, const cl_mem data_col,
                         const int_tp data_col_off, const int_tp channels,
                         const int_tp height, const int_tp width,
                         const int_tp patch_h, const int_tp patch_w,
                         const int_tp pad_h, const int_tp pad_w,
                         const int_tp stride_h, const int_tp stride_w,
                         cl_mem data_im, const int_tp data_im_off);

template<typename Dtype>
void greentea_im2col_sk_gpu(viennacl::ocl::program *prog,
                            viennacl::ocl::context *ctx, const cl_mem data_im,
                            const int_tp data_offset, const int_tp channels,
                            const int_tp height, const int_tp width,
                            const int_tp kernel_h, const int_tp kernel_w,
                            const int_tp pad_h, const int_tp pad_w,
                            const int_tp stride_h, const int_tp stride_w,
                            const int_tp kstride_h, const int_tp kstride_w,
                            cl_mem data_col);

template<typename Dtype>
void greentea_col2im_sk_gpu(viennacl::ocl::program *prog,
                            viennacl::ocl::context *ctx, const cl_mem data_col,
                            const int_tp channels, const int_tp height,
                            const int_tp width, const int_tp patch_h,
                            const int_tp patch_w, const int_tp pad_h,
                            const int_tp pad_w, const int_tp stride_h,
                            const int_tp stride_w, const int_tp kstride_h,
                            const int_tp kstride_w, cl_mem data_im,
                            const int_tp data_offset);

template<typename Dtype>
void greentea_im2col_nd_gpu(viennacl::ocl::program *prog,
                            viennacl::ocl::context *ctx, cl_mem data_im,
                            const int_tp data_off,
                            const int_tp num_spatial_axes,
                            const int_tp channel_axis, const int_tp num_kernels,
                            cl_mem im_shape, cl_mem col_shape,
                            cl_mem kernel_shape, cl_mem pad, cl_mem stride,
                            cl_mem data_col, int_tp data_col_off);

template<typename Dtype>
void greentea_col2im_nd_gpu(viennacl::ocl::program *prog,
                            viennacl::ocl::context *ctx, cl_mem data_col,
                            const int_tp data_col_off,
                            const int_tp num_spatial_axes,
                            const int_tp channel_axis, const int_tp im_size,
                            cl_mem im_shape, cl_mem col_shape,
                            cl_mem kernel_shape, cl_mem pad, cl_mem stride,
                            cl_mem data_im, int_tp data_off);

template<typename Dtype>
void greentea_im2col_ndsk_gpu(viennacl::ocl::program *prog,
                              viennacl::ocl::context *ctx, cl_mem data_im,
                              const int_tp data_off,
                              const int_tp num_spatial_axes,
                              const int_tp num_kernels, cl_mem im_shape,
                              cl_mem col_shape, cl_mem kernel_shape, cl_mem pad,
                              cl_mem stride, cl_mem kstride, cl_mem data_col,
                              int_tp data_col_off);

template<typename Dtype>
void greentea_col2im_ndsk_gpu(viennacl::ocl::program *prog,
                              viennacl::ocl::context *ctx, cl_mem data_col,
                              const int_tp data_col_off,
                              const int_tp num_spatial_axes,
                              const int_tp im_size, cl_mem im_shape,
                              cl_mem col_shape, cl_mem kernel_shape, cl_mem pad,
                              cl_mem stride, cl_mem kstride, cl_mem data_im,
                              int_tp data_off);

}  // namespace caffe

#endif  // USE_GREENTEA
#endif  /* GREENTEA_IM2COL_HPP_ */
