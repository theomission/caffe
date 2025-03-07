/*
 TODO:
 - only load parts of the file, in accordance with a prototxt param "max_mem"
 */

#include <stdint.h>
#include <vector>

#include "hdf5.h"
#include "hdf5_hl.h"

#include "caffe/data_layers.hpp"

namespace caffe {

template<typename Dtype>
void HDF5DataLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
                                       const vector<Blob<Dtype>*>& top) {
  if (this->device_->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
    const int_tp batch_size = this->layer_param_.hdf5_data_param().batch_size();
    for (int_tp i = 0; i < batch_size; ++i, ++current_row_) {
      if (current_row_ == hdf_blobs_[0]->shape(0)) {
        if (num_files_ > 1) {
          current_file_ += 1;
          if (current_file_ == num_files_) {
            current_file_ = 0;
            if (this->layer_param_.hdf5_data_param().shuffle()) {
              std::random_shuffle(file_permutation_.begin(),
                                  file_permutation_.end());
            }
            DLOG(INFO)<< "Looping around to first file.";
          }
          LoadHDF5FileData(
              hdf_filenames_[file_permutation_[current_file_]].c_str());
        }
        current_row_ = 0;
        if (this->layer_param_.hdf5_data_param().shuffle())
          std::random_shuffle(data_permutation_.begin(),
                              data_permutation_.end());
      }
      for (int_tp j = 0; j < this->layer_param_.top_size(); ++j) {
        int_tp data_dim = top[j]->count() / top[j]->shape(0);
        caffe_copy(
            data_dim,
            &hdf_blobs_[j]->cpu_data()[data_permutation_[current_row_]
                * data_dim],
            &top[j]->mutable_gpu_data()[i * data_dim]);
      }
    }
#endif  // USE_CUDA
  } else {
#ifdef USE_GREENTEA
    Forward_cpu(bottom, top);
#endif  // USE_GREENTEA
  }
}

INSTANTIATE_LAYER_GPU_FUNCS(HDF5DataLayer);

}  // namespace caffe
