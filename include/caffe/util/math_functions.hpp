#ifndef CAFFE_UTIL_MATH_FUNCTIONS_H_
#define CAFFE_UTIL_MATH_FUNCTIONS_H_

#include <stdint.h>
#include <cmath>  // for std::fabs and std::signbit

#include "glog/logging.h"

#include "caffe/common.hpp"
#include "caffe/util/device_alternate.hpp"
#include "caffe/util/mkl_alternate.hpp"

namespace caffe {

// Caffe gemm provides a simpler interface to the gemm functions, with the
// limitation that the data has to be contiguous in memory.
template<typename Dtype>
void caffe_cpu_gemm(const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB,
                    const int_tp M, const int_tp N, const int_tp K,
                    const Dtype alpha, const Dtype* A, const Dtype* B,
                    const Dtype beta, Dtype* C);

template<typename Dtype>
void caffe_cpu_gemv(const CBLAS_TRANSPOSE TransA, const int_tp M,
                    const int_tp N, const Dtype alpha, const Dtype* A,
                    const Dtype* x, const Dtype beta, Dtype* y);

template<typename Dtype>
void caffe_axpy(const int_tp N, const Dtype alpha, const Dtype* X, Dtype* Y);

template<typename Dtype>
void caffe_cpu_axpby(const int_tp N, const Dtype alpha, const Dtype* X,
                     const Dtype beta, Dtype* Y);

template<typename Dtype>
void caffe_cpu_copy(const int_tp N, const Dtype* X, Dtype* Y);

template<typename Dtype>
void caffe_copy(const int_tp N, const Dtype *X, Dtype *Y);

template<typename Dtype>
void caffe_set(const int_tp N, const Dtype alpha, Dtype *X);

inline void caffe_memset(const uint_tp N, const int_tp alpha, void* X) {
  memset(X, alpha, N);  // NOLINT(caffe/alt_fn)
}

template<typename Dtype>
void caffe_add_scalar(const int_tp N, const Dtype alpha, Dtype *X);

template<typename Dtype>
void caffe_scal(const int_tp N, const Dtype alpha, Dtype *X);

template<typename Dtype>
void caffe_sqr(const int_tp N, const Dtype* a, Dtype* y);

template<typename Dtype>
void caffe_add(const int_tp N, const Dtype* a, const Dtype* b, Dtype* y);

template<typename Dtype>
void caffe_sub(const int_tp N, const Dtype* a, const Dtype* b, Dtype* y);

template<typename Dtype>
void caffe_mul(const int_tp N, const Dtype* a, const Dtype* b, Dtype* y);

template<typename Dtype>
void caffe_div(const int_tp N, const Dtype* a, const Dtype* b, Dtype* y);

template<typename Dtype>
void caffe_powx(const int_tp n, const Dtype* a, const Dtype b, Dtype* y);

uint_tp caffe_rng_rand();

template<typename Dtype>
Dtype caffe_nextafter(const Dtype b);

void caffe_rng_uniform(const int_tp n, uint_tp* r);

template<typename Dtype>
void caffe_rng_uniform(const int_tp n, const Dtype a, const Dtype b, Dtype* r);

template<typename Dtype>
void caffe_rng_gaussian(const int_tp n, const Dtype mu, const Dtype sigma,
                        Dtype* r);

template<typename Dtype, typename Itype>
void caffe_rng_bernoulli(const int_tp n, const Dtype p, Itype* r);

template<typename Dtype>
void caffe_exp(const int_tp n, const Dtype* a, Dtype* y);

template<typename Dtype>
void caffe_log(const int_tp n, const Dtype* a, Dtype* y);

template<typename Dtype>
void caffe_abs(const int_tp n, const Dtype* a, Dtype* y);

template<typename Dtype>
Dtype caffe_cpu_dot(const int_tp n, const Dtype* x, const Dtype* y);

template<typename Dtype>
Dtype caffe_cpu_strided_dot(const int_tp n, const Dtype* x, const int_tp incx,
                            const Dtype* y, const int_tp incy);

template<typename Dtype>
int_tp caffe_cpu_hamming_distance(const int_tp n, const Dtype* x,
                                  const Dtype* y);

// Returns the sum of the absolute values of the elements of vector x
template<typename Dtype>
Dtype caffe_cpu_asum(const int_tp n, const Dtype* x);

// the branchless, type-safe version from
// http://stackoverflow.com/questions/1903954/is-there-a-standard-sign-function-signum-sgn-in-c-c
template<typename Dtype>
inline int8_t caffe_sign(Dtype val) {
  return (Dtype(0) < val) - (val < Dtype(0));
}

// The following two macros are modifications of DEFINE_VSL_UNARY_FUNC
//   in include/caffe/util/mkl_alternate.hpp authored by @Rowland Depp.
// Please refer to commit 7e8ef25c7 of the boost-eigen branch.
// Git cherry picking that commit caused a conflict hard to resolve and
//   copying that file in convenient for code reviewing.
// So they have to be pasted here temporarily.
#define DEFINE_CAFFE_CPU_UNARY_FUNC(name, operation) \
  template<typename Dtype> \
  void caffe_cpu_##name(const int_tp n, const Dtype* x, Dtype* y) { \
    CHECK_GT(n, 0); CHECK(x); CHECK(y); \
    for (int_tp i = 0; i < n; ++i) { \
      operation; \
    } \
  }

// output is 1 for the positives, 0 for zero, and -1 for the negatives
DEFINE_CAFFE_CPU_UNARY_FUNC(sign, y[i] = caffe_sign<Dtype>(x[i]));

// This returns a nonzero value if the input has its sign bit set.
// The name sngbit is meant to avoid conflicts with std::signbit in the macro.
// The extra parens are needed because CUDA < 6.5 defines signbit as a macro,
// and we don't want that to expand here when CUDA headers are also included.
DEFINE_CAFFE_CPU_UNARY_FUNC(sgnbit,
                            y[i] = static_cast<bool>((std::signbit)(x[i])));

DEFINE_CAFFE_CPU_UNARY_FUNC(fabs, y[i] = std::fabs(x[i]));

template<typename Dtype>
void caffe_cpu_scale(const int_tp n, const Dtype alpha, const Dtype *x,
                     Dtype* y);

#ifndef CPU_ONLY  // GPU
#ifdef USE_CUDA

// Decaf gpu gemm provides an interface that is almost the same as the cpu
// gemm function - following the c convention and calling the fortran-order
// gpu code under the hood.
template<typename Dtype>
void caffe_gpu_gemm(const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB,
                    const int_tp M, const int_tp N, const int_tp K,
                    const Dtype alpha, const Dtype* A, const Dtype* B,
                    const Dtype beta, Dtype* C);

template<typename Dtype>
void caffe_gpu_gemv(const CBLAS_TRANSPOSE TransA, const int_tp M,
                    const int_tp N, const Dtype alpha, const Dtype* A,
                    const Dtype* x, const Dtype beta, Dtype* y);

template<typename Dtype>
void caffe_gpu_axpy(const int_tp N, const Dtype alpha, const Dtype* X,
                    Dtype* Y);

template<typename Dtype>
void caffe_gpu_axpby(const int_tp N, const Dtype alpha, const Dtype* X,
                     const Dtype beta, Dtype* Y);

void caffe_gpu_memcpy(const uint_tp N, const void *X, void *Y);

template<typename Dtype>
void caffe_gpu_set(const int_tp N, const Dtype alpha, Dtype *X);

inline void caffe_gpu_memset(const uint_tp N, const int_tp alpha, void* X) {
  CUDA_CHECK(cudaMemset(X, alpha, N));  // NOLINT(caffe/alt_fn)
}

template<typename Dtype>
void caffe_gpu_add_scalar(const int_tp N, const Dtype alpha, Dtype *X);

template<typename Dtype>
void caffe_gpu_scal(const int_tp N, const Dtype alpha, Dtype *X);

template<typename Dtype>
void caffe_gpu_add(const int_tp N, const Dtype* a, const Dtype* b, Dtype* y);

template<typename Dtype>
void caffe_gpu_sub(const int_tp N, const Dtype* a, const Dtype* b, Dtype* y);

template<typename Dtype>
void caffe_gpu_mul(const int_tp N, const Dtype* a, const Dtype* b, Dtype* y);

template<typename Dtype>
void caffe_gpu_div(const int_tp N, const Dtype* a, const Dtype* b, Dtype* y);

template<typename Dtype>
void caffe_gpu_abs(const int_tp n, const Dtype* a, Dtype* y);

template<typename Dtype>
void caffe_gpu_exp(const int_tp n, const Dtype* a, Dtype* y);

template<typename Dtype>
void caffe_gpu_log(const int_tp n, const Dtype* a, Dtype* y);

template<typename Dtype>
void caffe_gpu_powx(const int_tp n, const Dtype* a, const Dtype b, Dtype* y);

// caffe_gpu_rng_uniform with two arguments generates integers in the range
// [0, UINT_MAX].
void caffe_gpu_rng_uniform(const int_tp n, unsigned int* r);  // NOLINT
void caffe_gpu_rng_uniform(const int_tp n, unsigned long long* r);  // NOLINT

// caffe_gpu_rng_uniform with four arguments generates floats in the range
// (a, b] (strictly greater than a, less than or equal to b) due to the
// specification of curandGenerateUniform.  With a = 0, b = 1, just calls
// curandGenerateUniform; with other limits will shift and scale the outputs
// appropriately after calling curandGenerateUniform.
template<typename Dtype>
void caffe_gpu_rng_uniform(const int_tp n, const Dtype a, const Dtype b,
                           Dtype* r);

template<typename Dtype>
void caffe_gpu_rng_gaussian(const int_tp n, const Dtype mu, const Dtype sigma,
                            Dtype* r);

template<typename Dtype>
void caffe_gpu_rng_bernoulli(const int_tp n, const Dtype p, int_tp* r);

template<typename Dtype>
void caffe_gpu_dot(const int_tp n, const Dtype* x, const Dtype* y, Dtype* out);

template<typename Dtype>
uint32_t caffe_gpu_hamming_distance(const int_tp n, const Dtype* x,
                                    const Dtype* y);

template<typename Dtype>
void caffe_gpu_asum(const int_tp n, const Dtype* x, Dtype* y);

template<typename Dtype>
void caffe_gpu_sign(const int_tp n, const Dtype* x, Dtype* y);

template<typename Dtype>
void caffe_gpu_sgnbit(const int_tp n, const Dtype* x, Dtype* y);

template<typename Dtype>
void caffe_gpu_fabs(const int_tp n, const Dtype* x, Dtype* y);

template<typename Dtype>
void caffe_gpu_scale(const int_tp n, const Dtype alpha, const Dtype *x,
                     Dtype* y);

#define DEFINE_AND_INSTANTIATE_GPU_UNARY_FUNC(name, operation) \
template<typename Dtype> \
__global__ void name##_kernel(const int_tp n, const Dtype* x, Dtype* y) { \
  CUDA_KERNEL_LOOP(index, n) { \
    operation; \
  } \
} \
template <> \
void caffe_gpu_##name<float>(const int_tp n, const float* x, float* y) { \
  /* NOLINT_NEXT_LINE(whitespace/operators) */ \
  name##_kernel<float><<<CAFFE_GET_BLOCKS(n), CAFFE_CUDA_NUM_THREADS>>>( \
      n, x, y); \
} \
template <> \
void caffe_gpu_##name<double>(const int_tp n, const double* x, double* y) { \
  /* NOLINT_NEXT_LINE(whitespace/operators) */ \
  name##_kernel<double><<<CAFFE_GET_BLOCKS(n), CAFFE_CUDA_NUM_THREADS>>>( \
      n, x, y); \
}

#endif  // USE_CUDA
#endif  // !CPU_ONLY

}  // namespace caffe

#endif  // CAFFE_UTIL_MATH_FUNCTIONS_H_
