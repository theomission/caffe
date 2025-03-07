#include <vector>

#include "caffe/sgd_solvers.hpp"

namespace caffe {

template <typename Dtype>
void AdamSolver<Dtype>::AdamPreSolve() {
  // Add the extra history entries for Adam after those from
  // SGDSolver::PreSolve
  const vector<Blob<Dtype>*>& net_params = this->net_->learnable_params();
  for (uint_tp i = 0; i < net_params.size(); ++i) {
    const vector<int_tp>& shape = net_params[i]->shape();
    this->history_.push_back(
            shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape)));
  }
}

template <typename Dtype>
void AdamSolver<Dtype>::ComputeUpdateValue(uint_tp param_id, Dtype rate) {
  const vector<Blob<Dtype>*>& net_params = this->net_->learnable_params();
  const vector<float>& net_params_lr = this->net_->params_lr();
  Dtype local_rate = rate * net_params_lr[param_id];
  const Dtype beta1 = this->param_.momentum();
  const Dtype beta2 = this->param_.momentum2();

  // we create aliases for convenience
  uint_tp update_history_offset = net_params.size();
  Blob<Dtype>* val_m = this->history_[param_id].get();
  Blob<Dtype>* val_v = this->history_[param_id + update_history_offset].get();
  Blob<Dtype>* val_t = this->temp_[param_id].get();

  const uint_tp t = this->iter_  + 1;
  const Dtype correction = std::sqrt(Dtype(1) - pow(beta2, t)) /
      (Dtype(1.) - pow(beta1, t));
  const uint_tp N = net_params[param_id]->count();
  const Dtype eps_hat = this->param_.delta();

  switch (Caffe::mode()) {
    case Caffe::CPU: {
    // update m <- \beta_1 m_{t-1} + (1-\beta_1)g_t
    caffe_cpu_axpby(N, Dtype(1)-beta1,
        net_params[param_id]->cpu_diff(), beta1,
        val_m->mutable_cpu_data());

    // update v <- \beta_2 m_{t-1} + (1-\beta_2)g_t^2
    caffe_mul(N,
        net_params[param_id]->cpu_diff(),
        net_params[param_id]->cpu_diff(),
    val_t->mutable_cpu_data());
    caffe_cpu_axpby(N, Dtype(1)-beta2,
        val_t->cpu_data(), beta2,
        val_v->mutable_cpu_data());

    // set update
    caffe_powx(N,
        val_v->cpu_data(), Dtype(0.5),
        val_t->mutable_cpu_data());
    caffe_add_scalar(N, eps_hat, val_t->mutable_cpu_data());
    caffe_div(N,
        val_m->cpu_data(),
        val_t->cpu_data(),
        val_t->mutable_cpu_data());

    caffe_cpu_scale(N, local_rate*correction,
        val_t->cpu_data(),
        net_params[param_id]->mutable_cpu_diff());
    break;
  }
  case Caffe::GPU: {
#ifndef CPU_ONLY
    if (this->device_->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
        // update m <- \beta_1 m_{t-1} + (1-\beta_1)g_t
        caffe_gpu_axpby(N, Dtype(1) - beta1, net_params[param_id]->gpu_diff(),
                        beta1, val_m->mutable_gpu_data());

        // update v <- \beta_2 m_{t-1} + (1-\beta_2)g_t^2
        caffe_gpu_mul(N, net_params[param_id]->gpu_diff(),
                      net_params[param_id]->gpu_diff(),
                      val_t->mutable_gpu_data());
        caffe_gpu_axpby(N, Dtype(1) - beta2, val_t->gpu_data(), beta2,
                        val_v->mutable_gpu_data());

        // set update
        caffe_gpu_powx(N, val_v->gpu_data(), Dtype(0.5),
                       val_t->mutable_gpu_data());
        caffe_gpu_add_scalar(N, eps_hat, val_t->mutable_gpu_data());
        caffe_gpu_div(N, val_m->gpu_data(), val_t->gpu_data(),
                      val_t->mutable_gpu_data());

        caffe_gpu_scale(N, local_rate * correction, val_t->gpu_data(),
                        net_params[param_id]->mutable_gpu_diff());
#endif  // USE_CUDA
    } else {
#ifdef USE_GREENTEA
        // update m <- \beta_1 m_{t-1} + (1-\beta_1)g_t
        greentea_gpu_axpby<Dtype>(this->device_->id(), N,
                                  Dtype(1) - beta1,
                                  (cl_mem) (net_params[param_id]->gpu_diff()),
                                  0, beta1,
                                  (cl_mem) (val_m->mutable_gpu_data()), 0);

        // update v <- \beta_2 m_{t-1} + (1-\beta_2)g_t^2
        greentea_gpu_mul<Dtype>(this->device_->id(), N,
                                (cl_mem) (net_params[param_id]->gpu_diff()), 0,
                                (cl_mem) (net_params[param_id]->gpu_diff()), 0,
                                (cl_mem) (val_t->mutable_gpu_data()), 0);
        greentea_gpu_axpby<Dtype>(this->device_->id(), N,
                                  Dtype(1) - beta2,
                                  (cl_mem) (val_t->gpu_data()), 0, beta2,
                                  (cl_mem) (val_v->mutable_gpu_data()), 0);

        // set update
        greentea_gpu_powx<Dtype>(this->device_->id(), N,
                                 (cl_mem) (val_v->gpu_data()), 0, Dtype(0.5),
                                 (cl_mem) (val_t->mutable_gpu_data()), 0);
        greentea_gpu_add_scalar<Dtype>(this->device_->id(), N, eps_hat,
                                       (cl_mem) (val_t->mutable_gpu_data()), 0);
        greentea_gpu_div<Dtype>(this->device_->id(), N,
                                (cl_mem) (val_m->gpu_data()), 0,
                                (cl_mem) (val_t->gpu_data()), 0,
                                (cl_mem) (val_t->mutable_gpu_data()), 0);

        greentea_gpu_scale<Dtype>(
            this->device_->id(), N, local_rate * correction,
            (cl_mem) (val_t->gpu_data()), 0,
            (cl_mem) (net_params[param_id]->mutable_gpu_diff()), 0);
#endif  // USE_GREENTA
      }
#else
    NO_GPU;
#endif
    break;
  }
  default:
    LOG(FATAL) << "Unknown caffe mode: " << Caffe::mode();
  }
}

INSTANTIATE_CLASS(AdamSolver);
REGISTER_SOLVER_CLASS(Adam);

}  // namespace caffe
