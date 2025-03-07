#include <algorithm>
#include <cmath>
#include <vector>

#include "caffe/loss_layers.hpp"
#include "caffe/util/math_functions.hpp"

namespace caffe {

template <typename Dtype>
void MultinomialLogisticLossLayer<Dtype>::Reshape(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  LossLayer<Dtype>::Reshape(bottom, top);
  CHECK_EQ(bottom[1]->channels(), 1);
  CHECK_EQ(bottom[1]->height(), 1);
  CHECK_EQ(bottom[1]->width(), 1);
}

template <typename Dtype>
void MultinomialLogisticLossLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->cpu_data();
  const Dtype* bottom_label = bottom[1]->cpu_data();
  int_tp num = bottom[0]->num();
  int_tp dim = bottom[0]->count() / bottom[0]->num();
  Dtype loss = 0;
  for (int_tp i = 0; i < num; ++i) {
    int_tp label = static_cast<int_tp>(bottom_label[i]);
    Dtype prob = std::max(
        bottom_data[i * dim + label], Dtype(kLOG_THRESHOLD));
    loss -= log(prob);
  }
  top[0]->mutable_cpu_data()[0] = loss / num;
}

template <typename Dtype>
void MultinomialLogisticLossLayer<Dtype>::Backward_cpu(
    const vector<Blob<Dtype>*>& top, const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom) {
  if (propagate_down[1]) {
    LOG(FATAL) << this->type()
               << " Layer cannot backpropagate to label inputs.";
  }
  if (propagate_down[0]) {
    const Dtype* bottom_data = bottom[0]->cpu_data();
    const Dtype* bottom_label = bottom[1]->cpu_data();
    Dtype* bottom_diff = bottom[0]->mutable_cpu_diff();
    int_tp num = bottom[0]->num();
    int_tp dim = bottom[0]->count() / bottom[0]->num();
    caffe_set(bottom[0]->count(), Dtype(0), bottom_diff);
    const Dtype scale = - top[0]->cpu_diff()[0] / num;
    for (int_tp i = 0; i < num; ++i) {
      int_tp label = static_cast<int_tp>(bottom_label[i]);
      Dtype prob = std::max(
          bottom_data[i * dim + label], Dtype(kLOG_THRESHOLD));
      bottom_diff[i * dim + label] = scale / prob;
    }
  }
}

INSTANTIATE_CLASS(MultinomialLogisticLossLayer);
REGISTER_LAYER_CLASS(MultinomialLogisticLoss);

}  // namespace caffe
