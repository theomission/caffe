#ifndef CAFFE_TEST_GRADIENT_CHECK_UTIL_H_
#define CAFFE_TEST_GRADIENT_CHECK_UTIL_H_

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "caffe/layer.hpp"
#include "caffe/net.hpp"

namespace caffe {

// The gradient checker adds a L2 normalization loss function on top of the
// top blobs, and checks the gradient.
template<typename Dtype>
class GradientChecker {
 public:
  // kink and kink_range specify an ignored nonsmooth region of the form
  // kink - kink_range <= |feature value| <= kink + kink_range,
  // which accounts for all nonsmoothness in use by caffe
  GradientChecker(const Dtype stepsize, const Dtype threshold,
                  const uint_tp seed = 1701, const Dtype kink = 0.,
                  const Dtype kink_range = -1)
      : stepsize_(stepsize), threshold_(threshold), seed_(seed), kink_(kink),
        kink_range_(kink_range) {
  }
  // Checks the gradient of a layer, with provided bottom layers and top
  // layers.
  // Note that after the gradient check, we do not guarantee that the data
  // stored in the layer parameters and the blobs are unchanged.
  void CheckGradient(Layer<Dtype>* layer, const vector<Blob<Dtype>*>& bottom,
                     const vector<Blob<Dtype>*>& top,
                     int_tp check_bottom = -1) {
    layer->SetUp(bottom, top);
    CheckGradientSingle(layer, bottom, top, check_bottom, -1, -1);
  }
  void CheckGradientExhaustive(Layer<Dtype>* layer,
                               const vector<Blob<Dtype>*>& bottom,
                               const vector<Blob<Dtype>*>& top,
                               int_tp check_bottom = -1);

  // CheckGradientEltwise can be used to test layers that perform element-wise
  // computation only (e.g., neuron layers) -- where (d y_i) / (d x_j) = 0 when
  // i != j.
  void CheckGradientEltwise(Layer<Dtype>* layer,
                            const vector<Blob<Dtype>*>& bottom,
                            const vector<Blob<Dtype>*>& top);

  // Checks the gradient of a single output with respect to particular input
  // blob(s).  If check_bottom = i >= 0, check only the ith bottom Blob.
  // If check_bottom == -1, check everything -- all bottom Blobs and all
  // param Blobs.  Otherwise (if check_bottom < -1), check only param Blobs.
  void CheckGradientSingle(Layer<Dtype>* layer,
                           const vector<Blob<Dtype>*>& bottom,
                           const vector<Blob<Dtype>*>& top, int_tp check_bottom,
                           int_tp top_id,
                           int_tp top_data_id, bool element_wise = false);

  // Checks the gradient of a network. This network should not have any data
  // layers or loss layers, since the function does not explicitly deal with
  // such cases yet. All input blobs and parameter blobs are going to be
  // checked, layer-by-layer to avoid numerical problems to accumulate.
  void CheckGradientNet(const Net<Dtype>& net,
                        const vector<Blob<Dtype>*>& input);

 protected:
  Dtype GetObjAndGradient(const Layer<Dtype>& layer,
                          const vector<Blob<Dtype>*>& top, int_tp top_id = -1,
                          int_tp top_data_id = -1);
  Dtype stepsize_;
  Dtype threshold_;
  uint_tp seed_;
  Dtype kink_;
  Dtype kink_range_;
};

template<typename Dtype>
void GradientChecker<Dtype>::CheckGradientSingle(
    Layer<Dtype>* layer, const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top, int_tp check_bottom, int_tp top_id,
    int_tp top_data_id,
    bool element_wise) {
  if (element_wise) {
    CHECK_EQ(0, layer->blobs().size());
    CHECK_LE(0, top_id);
    CHECK_LE(0, top_data_id);
    const int_tp top_count = top[top_id]->count();
    for (int_tp blob_id = 0; blob_id < bottom.size(); ++blob_id) {
      CHECK_EQ(top_count, bottom[blob_id]->count());
    }
  }
  // First, figure out what blobs we need to check against, and zero init
  // parameter blobs.
  vector<Blob<Dtype>*> blobs_to_check;
  vector<bool> propagate_down(bottom.size(), check_bottom == -1);
  for (int_tp i = 0; i < layer->blobs().size(); ++i) {
    Blob<Dtype>* blob = layer->blobs()[i].get();
    caffe_set(blob->count(), static_cast<Dtype>(0), blob->mutable_cpu_diff());
    blobs_to_check.push_back(blob);
  }
  if (check_bottom == -1) {
    for (int_tp i = 0; i < bottom.size(); ++i) {
      blobs_to_check.push_back(bottom[i]);
    }
  } else if (check_bottom >= 0) {
    CHECK_LT(check_bottom, bottom.size());
    blobs_to_check.push_back(bottom[check_bottom]);
    propagate_down[check_bottom] = true;
  }
  CHECK_GT(blobs_to_check.size(), 0)<< "No blobs to check.";
  // Compute the gradient analytically using Backward
  Caffe::set_random_seed(seed_);
  // Ignore the loss from the layer (it's just the weighted sum of the losses
  // from the top blobs, whose gradients we may want to test individually).
  layer->Forward(bottom, top);
  // Get additional loss from the objective
  GetObjAndGradient(*layer, top, top_id, top_data_id);
  layer->Backward(top, propagate_down, bottom);
  // Store computed gradients for all checked blobs
  vector<shared_ptr<Blob<Dtype> > > computed_gradient_blobs(
      blobs_to_check.size());
  for (int_tp blob_id = 0; blob_id < blobs_to_check.size(); ++blob_id) {
    Blob<Dtype>* current_blob = blobs_to_check[blob_id];
    computed_gradient_blobs[blob_id].reset(new Blob<Dtype>());
    computed_gradient_blobs[blob_id]->ReshapeLike(*current_blob);
    const int_tp count = blobs_to_check[blob_id]->count();
    const Dtype* diff = blobs_to_check[blob_id]->cpu_diff();
    Dtype* computed_gradients = computed_gradient_blobs[blob_id]
        ->mutable_cpu_data();

    caffe_cpu_copy(count, diff, computed_gradients);
  }
  // Compute derivative of top w.r.t. each bottom and parameter input using
  // finite differencing.
  // LOG(ERROR) << "Checking " << blobs_to_check.size() << " blobs.";
  for (int_tp blob_id = 0; blob_id < blobs_to_check.size(); ++blob_id) {
    Blob<Dtype>* current_blob = blobs_to_check[blob_id];
    const Dtype* computed_gradients =
        computed_gradient_blobs[blob_id]->cpu_data();
    // LOG(ERROR) << "Blob " << blob_id << ": checking "
    //     << current_blob->count() << " parameters.";
    for (int_tp feat_id = 0; feat_id < current_blob->count(); ++feat_id) {
      // For an element-wise layer, we only need to do finite differencing to
      // compute the derivative of top[top_id][top_data_id] w.r.t.
      // bottom[blob_id][i] only for i == top_data_id.  For any other
      // i != top_data_id, we know the derivative is 0 by definition, and simply
      // check that that's true.
      Dtype estimated_gradient = 0;
      Dtype positive_objective = 0;
      Dtype negative_objective = 0;
      if (!element_wise || (feat_id == top_data_id)) {
        // Do finite differencing.
        // Compute loss with stepsize_ added to input.
        current_blob->mutable_cpu_data()[feat_id] += stepsize_;
        Caffe::set_random_seed(seed_);
        layer->Forward(bottom, top);
        positive_objective = GetObjAndGradient(*layer, top, top_id,
                                               top_data_id);
        // Compute loss with stepsize_ subtracted from input.
        current_blob->mutable_cpu_data()[feat_id] -= stepsize_ * 2;
        Caffe::set_random_seed(seed_);
        layer->Forward(bottom, top);
        negative_objective = GetObjAndGradient(*layer, top, top_id,
                                               top_data_id);
        // Recover original input value.
        current_blob->mutable_cpu_data()[feat_id] += stepsize_;
        estimated_gradient = (positive_objective - negative_objective)
            / stepsize_ / 2.;
      }
      Dtype computed_gradient = computed_gradients[feat_id];
      Dtype feature = current_blob->cpu_data()[feat_id];
      // LOG(ERROR) << "debug: " << current_blob->cpu_data()[feat_id] << " "
      //     << current_blob->cpu_diff()[feat_id];
      if (kink_ - kink_range_ > fabs(feature)
          || fabs(feature) > kink_ + kink_range_) {
        // We check relative accuracy, but for too small values, we threshold
        // the scale factor by 1.
        Dtype scale = std::max(
            std::max(fabs(computed_gradient), fabs(estimated_gradient)), 1.);
        EXPECT_NEAR(computed_gradient, estimated_gradient, threshold_ * scale)
            << "debug: (top_id, top_data_id, blob_id, feat_id)=" << top_id
            << "," << top_data_id << "," << blob_id << "," << feat_id
            << "; feat = " << feature << "; objective+ = " << positive_objective
            << "; objective- = " << negative_objective;
      }
      // LOG(ERROR) << "Feature: " << current_blob->cpu_data()[feat_id];
      // LOG(ERROR) << "computed gradient: " << computed_gradient
      //    << " estimated_gradient: " << estimated_gradient;
    }
  }
}

template<typename Dtype>
void GradientChecker<Dtype>::CheckGradientExhaustive(
    Layer<Dtype>* layer, const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top, int_tp check_bottom) {
  layer->SetUp(bottom, top);
  CHECK_GT(top.size(), 0)<< "Exhaustive mode requires at least one top blob.";
  // LOG(ERROR) << "Exhaustive Mode.";
  for (int_tp i = 0; i < top.size(); ++i) {
    // LOG(ERROR) << "Exhaustive: blob " << i << " size " << top[i]->count();
    for (int_tp j = 0; j < top[i]->count(); ++j) {
      // LOG(ERROR) << "Exhaustive: blob " << i << " data " << j;
      CheckGradientSingle(layer, bottom, top, check_bottom, i, j);
    }
  }
}

template<typename Dtype>
void GradientChecker<Dtype>::CheckGradientEltwise(
    Layer<Dtype>* layer, const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  layer->SetUp(bottom, top);
  CHECK_GT(top.size(), 0)<< "Eltwise mode requires at least one top blob.";
  const int_tp check_bottom = -1;
  const bool element_wise = true;
  for (int_tp i = 0; i < top.size(); ++i) {
    for (int_tp j = 0; j < top[i]->count(); ++j) {
      CheckGradientSingle(layer, bottom, top, check_bottom, i, j, element_wise);
    }
  }
}

template<typename Dtype>
void GradientChecker<Dtype>::CheckGradientNet(
    const Net<Dtype>& net, const vector<Blob<Dtype>*>& input) {
  const vector<shared_ptr<Layer<Dtype> > >& layers = net.layers();
  vector<vector<Blob<Dtype>*> >& bottom_vecs = net.bottom_vecs();
  vector<vector<Blob<Dtype>*> >& top_vecs = net.top_vecs();
  for (int_tp i = 0; i < layers.size(); ++i) {
    net.Forward(input);
    LOG(ERROR)<< "Checking gradient for " << layers[i]->layer_param().name();
    CheckGradientExhaustive(*(layers[i].get()), bottom_vecs[i], top_vecs[i]);
  }
}

template<typename Dtype>
Dtype GradientChecker<Dtype>::GetObjAndGradient(const Layer<Dtype>& layer,
                                                const vector<Blob<Dtype>*>& top,
                                                int_tp top_id,
                                                int_tp top_data_id) {
  Dtype loss = 0;
  if (top_id < 0) {
    // the loss will be half of the sum of squares of all outputs
    for (int_tp i = 0; i < top.size(); ++i) {
      Blob<Dtype>* top_blob = top[i];
      const Dtype* top_blob_data = top_blob->cpu_data();
      Dtype* top_blob_diff = top_blob->mutable_cpu_diff();
      int_tp count = top_blob->count();
      for (int_tp j = 0; j < count; ++j) {
        loss += top_blob_data[j] * top_blob_data[j];
      }
      // set the diff: simply the data.
      caffe_cpu_copy(top_blob->count(), top_blob_data, top_blob_diff);
    }
    loss /= 2.;
  } else {
    // the loss will be the top_data_id-th element in the top_id-th blob.
    for (int_tp i = 0; i < top.size(); ++i) {
      Blob<Dtype>* top_blob = top[i];
      Dtype* top_blob_diff = top_blob->mutable_cpu_diff();
      caffe_set(top_blob->count(), Dtype(0), top_blob_diff);
    }
    const Dtype loss_weight = 2;
    loss = top[top_id]->cpu_data()[top_data_id] * loss_weight;
    top[top_id]->mutable_cpu_diff()[top_data_id] = loss_weight;
  }
  return loss;
}

}  // namespace caffe

#endif  // CAFFE_TEST_GRADIENT_CHECK_UTIL_H_
