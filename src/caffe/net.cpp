#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hdf5.h"

#include "caffe/common.hpp"
#include "caffe/layer.hpp"
#include "caffe/net.hpp"
#include "caffe/parallel.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/util/hdf5.hpp"
#include "caffe/util/insert_splits.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/upgrade_proto.hpp"

#include "caffe/test/test_caffe_main.hpp"

namespace caffe {


template<typename Dtype>
Net<Dtype>::Net(const NetParameter& param, const Net* root_net)
    : root_net_(root_net) {
  Init(param);
}

template<typename Dtype>
Net<Dtype>::Net(const string& param_file, Phase phase, const Net* root_net)
    : root_net_(root_net) {
  NetParameter param;
  ReadNetParamsFromTextFileOrDie(param_file, &param);
  param.mutable_state()->set_phase(phase);
  Init(param);
}

template<typename Dtype>
void Net<Dtype>::Init(const NetParameter& in_param) {
  CHECK(Caffe::root_solver() || root_net_)
      << "root_net_ needs to be set for all non-root solvers";
  // Set phase from the state.
  phase_ = in_param.state().phase();
  // Filter layers based on their include/exclude rules and
  // the current NetState.
  NetParameter filtered_param;
  FilterNet(in_param, &filtered_param);
  if (Caffe::root_solver()) {
    LOG(INFO) << "Initializing net from parameters: " << std::endl
              << filtered_param.DebugString();
  }
  // Create a copy of filtered_param with splits added where necessary.
  NetParameter param;
  InsertSplits(filtered_param, &param);
  // Basically, build all the layers and set up its connections.
  name_ = param.name();
  map<string, int_tp> blob_name_to_idx;
  set<string> available_blobs;
  CHECK(param.input_dim_size() == 0 || param.input_shape_size() == 0)
      << "Must specify either input_shape OR deprecated input_dim, not both.";
  if (param.input_dim_size() > 0) {
    // Deprecated 4D dimensions.
    CHECK_EQ(param.input_size() * 4, param.input_dim_size())
        << "Incorrect input blob dimension specifications.";
  } else {
    CHECK_EQ(param.input_size(), param.input_shape_size())
    << "Exactly one input_shape must be specified per input.";
  }
  memory_used_ = 0;
  // set the input blobs
  for (int_tp input_id = 0; input_id < param.input_size(); ++input_id) {
    const int_tp layer_id = -1;  // inputs have fake layer ID -1
    AppendTop(param, layer_id, input_id, &available_blobs, &blob_name_to_idx);
  }
  DLOG_IF(INFO, Caffe::root_solver())
      << "Memory required for data: " << memory_used_ * sizeof(Dtype);
  // For each layer, set up its input and output
  bottom_vecs_.resize(param.layer_size());
  top_vecs_.resize(param.layer_size());
  bottom_id_vecs_.resize(param.layer_size());
  param_id_vecs_.resize(param.layer_size());
  top_id_vecs_.resize(param.layer_size());
  bottom_need_backward_.resize(param.layer_size());
  for (int_tp layer_id = 0; layer_id < param.layer_size(); ++layer_id) {
    // For non-root solvers, whether this layer is shared from root_net_.
    bool share_from_root = !Caffe::root_solver()
        && root_net_->layers_[layer_id]->ShareInParallel();
    // Inherit phase from net if unset.
    if (!param.layer(layer_id).has_phase()) {
      param.mutable_layer(layer_id)->set_phase(phase_);
    }
    // Setup layer.
    const LayerParameter& layer_param = param.layer(layer_id);
    if (layer_param.propagate_down_size() > 0) {
      CHECK_EQ(layer_param.propagate_down_size(),
          layer_param.bottom_size())<< "propagate_down param must be specified "
      << "either 0 or bottom_size times ";
    }
    if (share_from_root) {
      LOG(INFO) << "Sharing layer " << layer_param.name() << " from root net";
      layers_.push_back(root_net_->layers_[layer_id]);
      layers_[layer_id]->SetShared(true);
    } else {
      layers_.push_back(LayerRegistry<Dtype>::CreateLayer(layer_param));
    }
    layer_names_.push_back(layer_param.name());
    if (Caffe::root_solver()) {
      LOG(INFO) << "Creating Layer " << layer_param.name();
    }
    bool need_backward = false;

    // Figure out this layer's input and output
    for (int_tp bottom_id = 0; bottom_id < layer_param.bottom_size();
        ++bottom_id) {
      const int_tp blob_id = AppendBottom(param, layer_id, bottom_id,
                                       &available_blobs, &blob_name_to_idx);
      // If a blob needs backward, this layer should provide it.
      need_backward |= blob_need_backward_[blob_id];
    }
    int_tp num_top = layer_param.top_size();
    for (int_tp top_id = 0; top_id < num_top; ++top_id) {
      AppendTop(param, layer_id, top_id, &available_blobs, &blob_name_to_idx);
    }
    // If the layer specifies that AutoTopBlobs() -> true and the LayerParameter
    // specified fewer than the required number (as specified by
    // ExactNumTopBlobs() or MinTopBlobs()), allocate them here.
    Layer<Dtype>* layer = layers_[layer_id].get();
    if (layer->AutoTopBlobs()) {
      const int_tp needed_num_top = std::max(layer->MinTopBlobs(),
                                          layer->ExactNumTopBlobs());
      for (; num_top < needed_num_top; ++num_top) {
        // Add "anonymous" top blobs -- do not modify available_blobs or
        // blob_name_to_idx as we don't want these blobs to be usable as input
        // to other layers.
        AppendTop(param, layer_id, num_top, NULL, NULL);
      }
    }
    // After this layer is connected, set it up.
    if (share_from_root) {
      // Set up size of top blobs using root_net_
      const vector<Blob<Dtype>*>& base_top = root_net_->top_vecs_[layer_id];
      const vector<Blob<Dtype>*>& this_top = this->top_vecs_[layer_id];
      for (int_tp top_id = 0; top_id < base_top.size(); ++top_id) {
        this_top[top_id]->ReshapeLike(*base_top[top_id]);
        LOG(INFO) << "Created top blob " << top_id << " (shape: "
            << this_top[top_id]->shape_string() <<  ") for shared layer "
            << layer_param.name();
      }
    } else {
      layers_[layer_id]->SetUp(bottom_vecs_[layer_id], top_vecs_[layer_id]);
    }
    if (Caffe::root_solver()) {
      LOG(INFO) << "Setting up " << layer_names_[layer_id];
    }
    for (int_tp top_id = 0; top_id < top_vecs_[layer_id].size(); ++top_id) {
      if (blob_loss_weights_.size() <= top_id_vecs_[layer_id][top_id]) {
        blob_loss_weights_.resize(top_id_vecs_[layer_id][top_id] + 1, Dtype(0));
      }
      blob_loss_weights_[top_id_vecs_[layer_id][top_id]] = layer->loss(top_id);
      if (Caffe::root_solver()) {
        LOG(INFO) << "Top shape: "
                  << top_vecs_[layer_id][top_id]->shape_string();
      }
      if (layer->loss(top_id)) {
        if (Caffe::root_solver()) {
          LOG(INFO) << "    with loss weight " << layer->loss(top_id);
        }
      }
      memory_used_ += top_vecs_[layer_id][top_id]->count();
    }
    if (Caffe::root_solver()) {
      DLOG(INFO) << "Memory required for data: "
                 << memory_used_ * sizeof(Dtype);
    }
    const int_tp param_size = layer_param.param_size();
    const int_tp num_param_blobs = layers_[layer_id]->blobs().size();
    CHECK_LE(param_size, num_param_blobs)
        << "Too many params specified for layer " << layer_param.name();
    ParamSpec default_param_spec;
    for (int_tp param_id = 0; param_id < num_param_blobs; ++param_id) {
      const ParamSpec* param_spec = (param_id < param_size) ?
          &layer_param.param(param_id) : &default_param_spec;
      const bool param_need_backward = param_spec->lr_mult() != 0;
      need_backward |= param_need_backward;
      layers_[layer_id]->set_param_propagate_down(param_id,
                                                  param_need_backward);
    }
    for (int_tp param_id = 0; param_id < num_param_blobs; ++param_id) {
      AppendParam(param, layer_id, param_id);
    }
    // Finally, set the backward flag
    layer_need_backward_.push_back(need_backward);
    if (need_backward) {
      for (int_tp top_id = 0; top_id < top_id_vecs_[layer_id].size();
          ++top_id) {
        blob_need_backward_[top_id_vecs_[layer_id][top_id]] = true;
      }
    }
  }

  // Go through the net backwards to determine which blobs contribute to the
  // loss.  We can skip backward computation for blobs that don't contribute
  // to the loss.
  // Also checks if all bottom blobs don't need backward computation (possible
  // because the skip_propagate_down param) and so we can skip bacward
  // computation for the entire layer
  set<string> blobs_under_loss;
  set<string> blobs_skip_backp;
  for (int_tp layer_id = layers_.size() - 1; layer_id >= 0; --layer_id) {
    bool layer_contributes_loss = false;
    bool layer_skip_propagate_down = true;
    for (int_tp top_id = 0; top_id < top_vecs_[layer_id].size(); ++top_id) {
      const string& blob_name = blob_names_[top_id_vecs_[layer_id][top_id]];
      if (layers_[layer_id]->loss(top_id)
          || (blobs_under_loss.find(blob_name) != blobs_under_loss.end())) {
        layer_contributes_loss = true;
      }
      if (blobs_skip_backp.find(blob_name) == blobs_skip_backp.end()) {
        layer_skip_propagate_down = false;
      }
      if (layer_contributes_loss && !layer_skip_propagate_down)
        break;
    }
    // If this layer can skip backward computation, also all his bottom blobs
    // don't need backpropagation
    if (layer_need_backward_[layer_id] && layer_skip_propagate_down) {
      layer_need_backward_[layer_id] = false;
      for (int_tp bottom_id = 0; bottom_id < bottom_vecs_[layer_id].size();
          ++bottom_id) {
        bottom_need_backward_[layer_id][bottom_id] = false;
      }
    }
    if (!layer_contributes_loss) {
      layer_need_backward_[layer_id] = false;
    }
    if (layer_need_backward_[layer_id]) {
      if (Caffe::root_solver()) {
        LOG(INFO) << layer_names_[layer_id] << " needs backward computation.";
      }
    } else {
      if (Caffe::root_solver()) {
        LOG(INFO) << layer_names_[layer_id]
                  << " does not need backward computation.";
      }
    }
    for (int_tp bottom_id = 0; bottom_id < bottom_vecs_[layer_id].size();
        ++bottom_id) {
      if (layer_contributes_loss) {
        const string& blob_name =
            blob_names_[bottom_id_vecs_[layer_id][bottom_id]];
        blobs_under_loss.insert(blob_name);
      } else {
        bottom_need_backward_[layer_id][bottom_id] = false;
      }
      if (!bottom_need_backward_[layer_id][bottom_id]) {
        const string& blob_name =
            blob_names_[bottom_id_vecs_[layer_id][bottom_id]];
        blobs_skip_backp.insert(blob_name);
      }
    }
  }
  // Handle force_backward if needed.
  if (param.force_backward()) {
    for (int_tp layer_id = 0; layer_id < layers_.size(); ++layer_id) {
      layer_need_backward_[layer_id] = true;
      for (int_tp bottom_id = 0;
          bottom_id < bottom_need_backward_[layer_id].size(); ++bottom_id) {
        bottom_need_backward_[layer_id][bottom_id] =
            bottom_need_backward_[layer_id][bottom_id]
                || layers_[layer_id]->AllowForceBackward(bottom_id);
        blob_need_backward_[bottom_id_vecs_[layer_id][bottom_id]] =
            blob_need_backward_[bottom_id_vecs_[layer_id][bottom_id]]
                || bottom_need_backward_[layer_id][bottom_id];
      }
      for (int_tp param_id = 0; param_id < layers_[layer_id]->blobs().size();
          ++param_id) {
        layers_[layer_id]->set_param_propagate_down(param_id, true);
      }
    }
  }
  // In the end, all remaining blobs are considered output blobs.
  for (set<string>::iterator it = available_blobs.begin();
      it != available_blobs.end(); ++it) {
    if (Caffe::root_solver()) {
      LOG(INFO) << "This network produces output " << *it;
    }
    net_output_blobs_.push_back(blobs_[blob_name_to_idx[*it]].get());
    net_output_blob_indices_.push_back(blob_name_to_idx[*it]);
  }
  for (uint_tp blob_id = 0; blob_id < blob_names_.size(); ++blob_id) {
    blob_names_index_[blob_names_[blob_id]] = blob_id;
  }
  for (uint_tp layer_id = 0; layer_id < layer_names_.size(); ++layer_id) {
    layer_names_index_[layer_names_[layer_id]] = layer_id;
  }
  ShareWeights();
  debug_info_ = param.debug_info();
  if (Caffe::root_solver()) {
    LOG(INFO) << "Network initialization done.";
    LOG(INFO) << "Memory required for data: " << memory_used_ * sizeof(Dtype);
  }
}

template<typename Dtype>
void Net<Dtype>::FilterNet(const NetParameter& param,
                           NetParameter* param_filtered) {
  NetState net_state(param.state());
  param_filtered->CopyFrom(param);
  param_filtered->clear_layer();
  for (int_tp i = 0; i < param.layer_size(); ++i) {
    const LayerParameter& layer_param = param.layer(i);
    const string& layer_name = layer_param.name();
    CHECK(layer_param.include_size() == 0 || layer_param.exclude_size() == 0)
        << "Specify either include rules or exclude rules; not both.";
    // If no include rules are specified, the layer is included by default and
    // only excluded if it meets one of the exclude rules.
    bool layer_included = (layer_param.include_size() == 0);
    for (int_tp j = 0; layer_included && j < layer_param.exclude_size(); ++j) {
      if (StateMeetsRule(net_state, layer_param.exclude(j), layer_name)) {
        layer_included = false;
      }
    }
    for (int_tp j = 0; !layer_included && j < layer_param.include_size(); ++j) {
      if (StateMeetsRule(net_state, layer_param.include(j), layer_name)) {
        layer_included = true;
      }
    }
    if (layer_included) {
      param_filtered->add_layer()->CopyFrom(layer_param);
    }
  }
}

template<typename Dtype>
bool Net<Dtype>::StateMeetsRule(const NetState& state, const NetStateRule& rule,
                                const string& layer_name) {
  // Check whether the rule is broken due to phase.
  if (rule.has_phase()) {
    if (rule.phase() != state.phase()) {
      if (Caffe::root_solver()) {
        LOG(INFO)<< "The NetState phase (" << state.phase()
        << ") differed from the phase (" << rule.phase()
        << ") specified by a rule in layer " << layer_name;
      }
      return false;
    }
  }
  // Check whether the rule is broken due to min level.
  if (rule.has_min_level()) {
    if (state.level() < rule.min_level()) {
      if (Caffe::root_solver()) {
        LOG(INFO) << "The NetState level (" << state.level()
                  << ") is above the min_level (" << rule.min_level()
                  << ") specified by a rule in layer " << layer_name;
      }
      return false;
    }
  }
  // Check whether the rule is broken due to max level.
  if (rule.has_max_level()) {
    if (state.level() > rule.max_level()) {
      if (Caffe::root_solver()) {
        LOG(INFO) << "The NetState level (" << state.level()
                  << ") is above the max_level (" << rule.max_level()
                  << ") specified by a rule in layer " << layer_name;
      }
      return false;
    }
  }
  // Check whether the rule is broken due to stage. The NetState must
  // contain ALL of the rule's stages to meet it.
  for (int_tp i = 0; i < rule.stage_size(); ++i) {
    // Check that the NetState contains the rule's ith stage.
    bool has_stage = false;
    for (int_tp j = 0; !has_stage && j < state.stage_size(); ++j) {
      if (rule.stage(i) == state.stage(j)) {has_stage = true;}
    }
    if (!has_stage) {
      if (Caffe::root_solver()) {
        LOG(INFO) << "The NetState did not contain stage '" << rule.stage(i)
                  << "' specified by a rule in layer " << layer_name;
      }
      return false;
    }
  }
  // Check whether the rule is broken due to not_stage. The NetState must
  // contain NONE of the rule's not_stages to meet it.
  for (int_tp i = 0; i < rule.not_stage_size(); ++i) {
    // Check that the NetState contains the rule's ith not_stage.
    bool has_stage = false;
    for (int_tp j = 0; !has_stage && j < state.stage_size(); ++j) {
      if (rule.not_stage(i) == state.stage(j)) {has_stage = true;}
    }
    if (has_stage) {
      if (Caffe::root_solver()) {
        LOG(INFO) << "The NetState contained a not_stage '" << rule.not_stage(i)
                  << "' specified by a rule in layer " << layer_name;
      }
      return false;
    }
  }
  return true;
}

// Helper for Net::Init: add a new input or top blob to the net.  (Inputs have
// layer_id == -1, tops have layer_id >= 0.)
template<typename Dtype>
void Net<Dtype>::AppendTop(const NetParameter& param, const int_tp layer_id,
                           const int_tp top_id, set<string>* available_blobs,
                           map<string, int_tp>* blob_name_to_idx) {
  shared_ptr<LayerParameter> layer_param(
      (layer_id >= 0) ? (new LayerParameter(param.layer(layer_id))) : NULL);
  const string& blob_name =
      layer_param ?
          (layer_param->top_size() > top_id ?
              layer_param->top(top_id) : "(automatic)") :
          param.input(top_id);
  // Check if we are doing in-place computation
  if (blob_name_to_idx && layer_param && layer_param->bottom_size() > top_id
      && blob_name == layer_param->bottom(top_id)) {
    // In-place computation
    if (Caffe::root_solver()) {
      LOG(INFO) << layer_param->name() << " -> " << blob_name << " (in-place)";
    }
    top_vecs_[layer_id].push_back(blobs_[(*blob_name_to_idx)[blob_name]].get());
    top_id_vecs_[layer_id].push_back((*blob_name_to_idx)[blob_name]);
  } else if (blob_name_to_idx &&
      blob_name_to_idx->find(blob_name) != blob_name_to_idx->end()) {
    // If we are not doing in-place computation but have duplicated blobs,
    // raise an error.
    LOG(FATAL) << "Top blob '" << blob_name
               << "' produced by multiple sources.";
  } else {
    // Normal output.
    if (Caffe::root_solver()) {
      if (layer_param) {
        LOG(INFO) << layer_param->name() << " -> " << blob_name;
      } else {
        LOG(INFO) << "Input " << top_id << " -> " << blob_name;
      }
    }
    shared_ptr<Blob<Dtype> > blob_pointer(new Blob<Dtype>());
    const int_tp blob_id = blobs_.size();
    blobs_.push_back(blob_pointer);
    blob_names_.push_back(blob_name);
    blob_need_backward_.push_back(false);
    if (blob_name_to_idx) {(*blob_name_to_idx)[blob_name] = blob_id;}
    if (layer_id == -1) {
      // Set the (explicitly specified) dimensions of the input blob.
      if (param.input_dim_size() > 0) {
        blob_pointer->Reshape(param.input_dim(top_id * 4),
            param.input_dim(top_id * 4 + 1),
            param.input_dim(top_id * 4 + 2),
            param.input_dim(top_id * 4 + 3));
      } else {
        blob_pointer->Reshape(param.input_shape(top_id));
      }
      net_input_blob_indices_.push_back(blob_id);
      net_input_blobs_.push_back(blob_pointer.get());
    } else {
      top_id_vecs_[layer_id].push_back(blob_id);
      top_vecs_[layer_id].push_back(blob_pointer.get());
    }
  }
  if (available_blobs) {
    available_blobs->insert(blob_name);
  }
}

// Helper for Net::Init: add a new bottom blob to the net.

template<typename Dtype>
int_tp Net<Dtype>::AppendBottom(const NetParameter& param,
                                const int_tp layer_id, const int_tp bottom_id,
                                set<string>* available_blobs,
                                map<string, int_tp>* blob_name_to_idx) {
  const LayerParameter& layer_param = param.layer(layer_id);
  const string& blob_name = layer_param.bottom(bottom_id);
  if (available_blobs->find(blob_name) == available_blobs->end()) {
    LOG(FATAL) << "Unknown bottom blob '" << blob_name << "' (layer '"
               << layer_param.name() << "', bottom index " << bottom_id << ")";
  }
  const int_tp blob_id = (*blob_name_to_idx)[blob_name];
  if (Caffe::root_solver()) {
    LOG(INFO) << layer_names_[layer_id] << " <- " << blob_name;
  }
  bottom_vecs_[layer_id].push_back(blobs_[blob_id].get());
  bottom_id_vecs_[layer_id].push_back(blob_id);
  available_blobs->erase(blob_name);
  bool propagate_down = true;
  // Check if the backpropagation on bottom_id should be skipped
  if (layer_param.propagate_down_size() > 0)
    propagate_down = layer_param.propagate_down(bottom_id);
  const bool need_backward = blob_need_backward_[blob_id] && propagate_down;
  bottom_need_backward_[layer_id].push_back(need_backward);
  return blob_id;
}

template<typename Dtype>
void Net<Dtype>::AppendParam(const NetParameter& param, const int_tp layer_id,
                             const int_tp param_id) {
  const LayerParameter& layer_param = layers_[layer_id]->layer_param();
  const int_tp param_size = layer_param.param_size();
  string param_name =
      (param_size > param_id) ? layer_param.param(param_id).name() : "";
  if (param_name.size()) {
    param_display_names_.push_back(param_name);
  } else {
    ostringstream param_display_name;
    param_display_name << param_id;
    param_display_names_.push_back(param_display_name.str());
  }
  const int_tp net_param_id = params_.size();
  params_.push_back(layers_[layer_id]->blobs()[param_id]);
  param_id_vecs_[layer_id].push_back(net_param_id);
  param_layer_indices_.push_back(make_pair(layer_id, param_id));
  ParamSpec default_param_spec;
  const ParamSpec* param_spec = (layer_param.param_size() > param_id) ?
      &layer_param.param(param_id) : &default_param_spec;
  if (!param_size || !param_name.size() || (param_name.size() &&
      param_names_index_.find(param_name) == param_names_index_.end())) {
    // This layer "owns" this parameter blob -- it is either anonymous
    // (i.e., not given a param_name) or explicitly given a name that we
    // haven't already seen.
    param_owners_.push_back(-1);
    if (param_name.size()) {
      param_names_index_[param_name] = net_param_id;
    }
    const int_tp learnable_param_id = learnable_params_.size();
    learnable_params_.push_back(params_[net_param_id].get());
    learnable_param_ids_.push_back(learnable_param_id);
    has_params_lr_.push_back(param_spec->has_lr_mult());
    has_params_decay_.push_back(param_spec->has_decay_mult());
    params_lr_.push_back(param_spec->lr_mult());
    params_weight_decay_.push_back(param_spec->decay_mult());
  } else {
    // Named param blob with name we've seen before: share params
    const int_tp owner_net_param_id = param_names_index_[param_name];
    param_owners_.push_back(owner_net_param_id);
    const pair<int_tp, int_tp>& owner_index =
        param_layer_indices_[owner_net_param_id];
    const int_tp owner_layer_id = owner_index.first;
    const int_tp owner_param_id = owner_index.second;
    LOG_IF(INFO, Caffe::root_solver()) << "Sharing parameters '" << param_name
        << "' owned by "
        << "layer '" << layer_names_[owner_layer_id] << "', param "
        << "index " << owner_param_id;
    Blob<Dtype>* this_blob = layers_[layer_id]->blobs()[param_id].get();
    Blob<Dtype>* owner_blob = layers_[owner_layer_id]->blobs()[owner_param_id]
        .get();
    const int_tp param_size = layer_param.param_size();
    if (param_size > param_id
        && (layer_param.param(param_id).share_mode()
            == ParamSpec_DimCheckMode_PERMISSIVE)) {
      // Permissive dimension checking -- only check counts are the same.
      CHECK_EQ(this_blob->count(), owner_blob->count())
          << "Cannot share param '" << param_name << "' owned by layer '"
          << layer_names_[owner_layer_id] << "' with layer '"
          << layer_names_[layer_id] << "'; count mismatch.  Owner layer param "
          << "shape is " << owner_blob->shape_string() << "; sharing layer "
          << "shape is " << this_blob->shape_string();
    } else {
      // Strict dimension checking -- all dims must be the same.
      CHECK(this_blob->shape() == owner_blob->shape())
          << "Cannot share param '" << param_name << "' owned by layer '"
          << layer_names_[owner_layer_id] << "' with layer '"
          << layer_names_[layer_id] << "'; shape mismatch.  Owner layer param "
          << "shape is " << owner_blob->shape_string() << "; sharing layer "
          << "expects shape " << this_blob->shape_string();
    }

    const int_tp learnable_param_id = learnable_param_ids_[owner_net_param_id];
    learnable_param_ids_.push_back(learnable_param_id);
    if (param_spec->has_lr_mult()) {
      if (has_params_lr_[learnable_param_id]) {
        CHECK_EQ(param_spec->lr_mult(), params_lr_[learnable_param_id])
            << "Shared param '" << param_name << "' has mismatched lr_mult.";
      } else {
        has_params_lr_[learnable_param_id] = true;
        params_lr_[learnable_param_id] = param_spec->lr_mult();
      }
    }
    if (param_spec->has_decay_mult()) {
      if (has_params_decay_[learnable_param_id]) {
        CHECK_EQ(param_spec->decay_mult(),
                 params_weight_decay_[learnable_param_id])
            << "Shared param '" << param_name << "' has mismatched decay_mult.";
      } else {
        has_params_decay_[learnable_param_id] = true;
        params_weight_decay_[learnable_param_id] = param_spec->decay_mult();
      }
    }
  }
}

template<typename Dtype>
Dtype Net<Dtype>::ForwardFromTo(int_tp start, int_tp end) {
  CHECK_GE(start, 0);
  CHECK_LT(end, layers_.size());
  Dtype loss = 0;
  if (debug_info_) {
    for (int_tp i = 0; i < net_input_blobs_.size(); ++i) {
      InputDebugInfo(i);
    }
  }
  for (int_tp i = start; i <= end; ++i) {
    // LOG(ERROR) << "Forwarding " << layer_names_[i];
    Dtype layer_loss = layers_[i]->Forward(bottom_vecs_[i], top_vecs_[i]);
    loss += layer_loss;
    if (debug_info_) {
      ForwardDebugInfo(i);
    }
  }
  return loss;
}

template<typename Dtype>
Dtype Net<Dtype>::ForwardFrom(int_tp start) {
  return ForwardFromTo(start, layers_.size() - 1);
}

template<typename Dtype>
Dtype Net<Dtype>::ForwardTo(int_tp end) {
  return ForwardFromTo(0, end);
}

template<typename Dtype>
const vector<Blob<Dtype>*>& Net<Dtype>::ForwardPrefilled(Dtype* loss) {
  if (loss != NULL) {
    *loss = ForwardFromTo(0, layers_.size() - 1);
  } else {
    ForwardFromTo(0, layers_.size() - 1);
  }
  return net_output_blobs_;
}

template<typename Dtype>
const vector<Blob<Dtype>*>& Net<Dtype>::Forward(
    const vector<Blob<Dtype>*> & bottom, Dtype* loss) {
  // Copy bottom to internal bottom
  for (int_tp i = 0; i < bottom.size(); ++i) {
    net_input_blobs_[i]->CopyFrom(*bottom[i]);
  }
  return ForwardPrefilled(loss);
}

template<typename Dtype>
string Net<Dtype>::Forward(const string& input_blob_protos, Dtype* loss) {
  BlobProtoVector blob_proto_vec;
  if (net_input_blobs_.size()) {
    blob_proto_vec.ParseFromString(input_blob_protos);
    CHECK_EQ(blob_proto_vec.blobs_size(), net_input_blobs_.size())
        << "Incorrect input size.";
    for (int_tp i = 0; i < blob_proto_vec.blobs_size(); ++i) {
      net_input_blobs_[i]->FromProto(blob_proto_vec.blobs(i));
    }
  }
  ForwardPrefilled(loss);
  blob_proto_vec.Clear();
  for (int_tp i = 0; i < net_output_blobs_.size(); ++i) {
    net_output_blobs_[i]->ToProto(blob_proto_vec.add_blobs());
  }
  string output;
  blob_proto_vec.SerializeToString(&output);
  return output;
}

template<typename Dtype>
void Net<Dtype>::BackwardFromTo(int_tp start, int_tp end) {
  CHECK_GE(end, 0);
  CHECK_LT(start, layers_.size());
  for (int_tp i = start; i >= end; --i) {
    if (layer_need_backward_[i]) {
      layers_[i]->Backward(top_vecs_[i], bottom_need_backward_[i],
                           bottom_vecs_[i]);
      if (debug_info_) {
        BackwardDebugInfo(i);
      }
    }
  }
}

template<typename Dtype>
void Net<Dtype>::InputDebugInfo(const int_tp input_id) {
  const Blob<Dtype>& blob = *net_input_blobs_[input_id];
  const string& blob_name = blob_names_[net_input_blob_indices_[input_id]];
  const Dtype data_abs_val_mean = blob.asum_data() / blob.count();
  if (Caffe::root_solver()) {
    LOG(INFO) << "    [Forward] "
              << "Input " << blob_name << " data: " << data_abs_val_mean;
  }
}

template<typename Dtype>
void Net<Dtype>::ForwardDebugInfo(const int_tp layer_id) {
  for (int_tp top_id = 0; top_id < top_vecs_[layer_id].size(); ++top_id) {
    const Blob<Dtype>& blob = *top_vecs_[layer_id][top_id];
    const string& blob_name = blob_names_[top_id_vecs_[layer_id][top_id]];
    const Dtype data_abs_val_mean = blob.asum_data() / blob.count();
    if (Caffe::root_solver()) {
      LOG(INFO) << "    [Forward] "
                << "Layer " << layer_names_[layer_id]
                << ", top blob " << blob_name
                << " data: " << data_abs_val_mean;
    }
  }
  for (int_tp param_id = 0; param_id < layers_[layer_id]->blobs().size();
      ++param_id) {
    const Blob<Dtype>& blob = *layers_[layer_id]->blobs()[param_id];
    const int_tp net_param_id = param_id_vecs_[layer_id][param_id];
    const string& blob_name = param_display_names_[net_param_id];
    const Dtype data_abs_val_mean = blob.asum_data() / blob.count();
    if (Caffe::root_solver()) {
      LOG(INFO) << "    [Forward] "
                << "Layer " << layer_names_[layer_id]
                << ", param blob " << blob_name
                << " data: " << data_abs_val_mean;
    }
  }
}

template<typename Dtype>
void Net<Dtype>::BackwardDebugInfo(const int_tp layer_id) {
  const vector<Blob<Dtype>*>& bottom_vec = bottom_vecs_[layer_id];
  for (int_tp bottom_id = 0; bottom_id < bottom_vec.size(); ++bottom_id) {
    if (!bottom_need_backward_[layer_id][bottom_id]) {
      continue;
    }
    const Blob<Dtype>& blob = *bottom_vec[bottom_id];
    const string& blob_name = blob_names_[bottom_id_vecs_[layer_id][bottom_id]];
    const Dtype diff_abs_val_mean = blob.asum_diff() / blob.count();
    if (Caffe::root_solver()) {
      LOG(INFO) << "    [Backward] "
                << "Layer " << layer_names_[layer_id]
                << ", bottom blob " << blob_name
                << " diff: " << diff_abs_val_mean;
    }
  }
  for (int_tp param_id = 0; param_id < layers_[layer_id]->blobs().size();
      ++param_id) {
    if (!layers_[layer_id]->param_propagate_down(param_id)) {
      continue;
    }
    const Blob<Dtype>& blob = *layers_[layer_id]->blobs()[param_id];
    const Dtype diff_abs_val_mean = blob.asum_diff() / blob.count();
    if (Caffe::root_solver()) {
      LOG(INFO) << "    [Backward] "
                << "Layer " << layer_names_[layer_id]
                << ", param blob " << param_id
                << " diff: " << diff_abs_val_mean;
    }
  }
}

template<typename Dtype>
void Net<Dtype>::UpdateDebugInfo(const int_tp param_id) {
  const Blob<Dtype>& blob = *params_[param_id];
  const int_tp param_owner = param_owners_[param_id];
  const string& layer_name = layer_names_[param_layer_indices_[param_id].first];
  const string& param_display_name = param_display_names_[param_id];
  const Dtype diff_abs_val_mean = blob.asum_diff() / blob.count();
  if (param_owner < 0) {
    const Dtype data_abs_val_mean = blob.asum_data() / blob.count();
    if (Caffe::root_solver()) {
      LOG(INFO) << "    [Update] Layer " << layer_name
                << ", param " << param_display_name
                << " data: " << data_abs_val_mean
                << "; diff: " << diff_abs_val_mean;
    }
  } else {
    const string& owner_layer_name =
        layer_names_[param_layer_indices_[param_owner].first];
    if (Caffe::root_solver()) {
      LOG(INFO) << "    [Update] Layer " << layer_name
                << ", param blob " << param_display_name
                << " (owned by layer " << owner_layer_name << ", " << "param "
                << param_display_names_[param_owners_[param_id]] << ")"
                << " diff: " << diff_abs_val_mean;
    }
  }
}

template<typename Dtype>
void Net<Dtype>::ShareTrainedLayersWith(const Net* other) {
  int_tp num_source_layers = other->layers().size();
  for (int_tp i = 0; i < num_source_layers; ++i) {
    Layer<Dtype>* source_layer = other->layers()[i].get();
    const string& source_layer_name = other->layer_names()[i];
    int_tp target_layer_id = 0;
    while (target_layer_id != layer_names_.size()
        && layer_names_[target_layer_id] != source_layer_name) {
      ++target_layer_id;
    }
    if (target_layer_id == layer_names_.size()) {
      LOG(INFO) << "Ignoring source layer " << source_layer_name;
      continue;
    }
    DLOG(INFO)<< "Copying source layer " << source_layer_name;
    vector<shared_ptr<Blob<Dtype> > >& target_blobs = layers_[target_layer_id]
        ->blobs();
    CHECK_EQ(target_blobs.size(), source_layer->blobs().size())
        << "Incompatible number of blobs for layer " << source_layer_name;
    for (int_tp j = 0; j < target_blobs.size(); ++j) {
      Blob<Dtype>* source_blob = source_layer->blobs()[j].get();
      CHECK(target_blobs[j]->shape() == source_blob->shape())
          << "Cannot share param " << j << " weights from layer '"
          << source_layer_name << "'; shape mismatch.  Source param shape is "
          << source_blob->shape_string() << "; target param shape is "
          << target_blobs[j]->shape_string();
      target_blobs[j]->ShareData(*source_blob);
    }
  }
}

template<typename Dtype>
void Net<Dtype>::BackwardFrom(int_tp start) {
  BackwardFromTo(start, 0);
}

template<typename Dtype>
void Net<Dtype>::BackwardTo(int_tp end) {
  BackwardFromTo(layers_.size() - 1, end);
}

template<typename Dtype>
void Net<Dtype>::Backward() {
  BackwardFromTo(layers_.size() - 1, 0);
  if (debug_info_) {
    Dtype asum_data = 0, asum_diff = 0, sumsq_data = 0, sumsq_diff = 0;
    for (int_tp i = 0; i < learnable_params_.size(); ++i) {
      asum_data += learnable_params_[i]->asum_data();
      asum_diff += learnable_params_[i]->asum_diff();
      sumsq_data += learnable_params_[i]->sumsq_data();
      sumsq_diff += learnable_params_[i]->sumsq_diff();
    }
    const Dtype l2norm_data = std::sqrt(sumsq_data);
    const Dtype l2norm_diff = std::sqrt(sumsq_diff);
    LOG(ERROR) << "    [Backward] All net params (data, diff): "
               << "L1 norm = (" << asum_data << ", " << asum_diff << "); "
               << "L2 norm = (" << l2norm_data << ", " << l2norm_diff << ")";
  }
}

template<typename Dtype>
void Net<Dtype>::Reshape() {
  for (int_tp i = 0; i < layers_.size(); ++i) {
    layers_[i]->Reshape(bottom_vecs_[i], top_vecs_[i]);
  }
}

template<typename Dtype>
void Net<Dtype>::CopyTrainedLayersFrom(const NetParameter& param) {
  int_tp num_source_layers = param.layer_size();
  for (int_tp i = 0; i < num_source_layers; ++i) {
    const LayerParameter& source_layer = param.layer(i);
    const string& source_layer_name = source_layer.name();
    int_tp target_layer_id = 0;
    while (target_layer_id != layer_names_.size()
        && layer_names_[target_layer_id] != source_layer_name) {
      ++target_layer_id;
    }
    if (target_layer_id == layer_names_.size()) {
      LOG(INFO) << "Ignoring source layer " << source_layer_name;
      continue;
    }
    DLOG(INFO)<< "Copying source layer " << source_layer_name;
    vector<shared_ptr<Blob<Dtype> > >& target_blobs = layers_[target_layer_id]
        ->blobs();
    CHECK_EQ(target_blobs.size(), source_layer.blobs_size())
        << "Incompatible number of blobs for layer " << source_layer_name;
    for (int_tp j = 0; j < target_blobs.size(); ++j) {
      if (!target_blobs[j]->ShapeEquals(source_layer.blobs(j))) {
        Blob<Dtype> source_blob;
        const bool kReshape = true;
        source_blob.FromProto(source_layer.blobs(j), kReshape);
        LOG(FATAL) << "Cannot copy param " << j << " weights from layer '"
            << source_layer_name << "'; shape mismatch.  Source param shape is "
            << source_blob.shape_string() << "; target param shape is "
            << target_blobs[j]->shape_string() << ". "
            << "To learn this layer's parameters from scratch rather than "
            << "copying from a saved net, rename the layer.";
      }
      const bool kReshape = false;
      target_blobs[j]->FromProto(source_layer.blobs(j), kReshape);
    }
  }
}

template<typename Dtype>
void Net<Dtype>::CopyTrainedLayersFrom(const string trained_filename) {
  if (trained_filename.size() >= 3 &&
      trained_filename.compare(trained_filename.size() - 3, 3, ".h5") == 0) {
    CopyTrainedLayersFromHDF5(trained_filename);
  } else {
    CopyTrainedLayersFromBinaryProto(trained_filename);
  }
}

template <typename Dtype>
void Net<Dtype>::CopyTrainedLayersFromBinaryProto(
    const string trained_filename) {
  NetParameter param;
  ReadNetParamsFromBinaryFileOrDie(trained_filename, &param);
  CopyTrainedLayersFrom(param);
}

template <typename Dtype>
void Net<Dtype>::CopyTrainedLayersFromHDF5(const string trained_filename) {
  hid_t file_hid = H5Fopen(trained_filename.c_str(), H5F_ACC_RDONLY,
                           H5P_DEFAULT);
  CHECK_GE(file_hid, 0) << "Couldn't open " << trained_filename;
  hid_t data_hid = H5Gopen2(file_hid, "data", H5P_DEFAULT);
  CHECK_GE(data_hid, 0) << "Error reading weights from " << trained_filename;
  int_tp num_layers = hdf5_get_num_links(data_hid);
  for (int_tp i = 0; i < num_layers; ++i) {
    string source_layer_name = hdf5_get_name_by_idx(data_hid, i);
    if (!layer_names_index_.count(source_layer_name)) {
      LOG(INFO) << "Ignoring source layer " << source_layer_name;
      continue;
    }
    int_tp target_layer_id = layer_names_index_[source_layer_name];
    DLOG(INFO) << "Copying source layer " << source_layer_name;
    vector<shared_ptr<Blob<Dtype> > >& target_blobs =
        layers_[target_layer_id]->blobs();
    hid_t layer_hid = H5Gopen2(data_hid, source_layer_name.c_str(),
        H5P_DEFAULT);
    CHECK_GE(layer_hid, 0)
        << "Error reading weights from " << trained_filename;
    // Check that source layer doesn't have more params than target layer
    int_tp num_source_params = hdf5_get_num_links(layer_hid);
    CHECK_LE(num_source_params, target_blobs.size())
        << "Incompatible number of blobs for layer " << source_layer_name;
    for (int_tp j = 0; j < target_blobs.size(); ++j) {
      ostringstream oss;
      oss << j;
      string dataset_name = oss.str();
      int_tp target_net_param_id = param_id_vecs_[target_layer_id][j];
      if (!H5Lexists(layer_hid, dataset_name.c_str(), H5P_DEFAULT)) {
        // Target param doesn't exist in source weights...
        if (param_owners_[target_net_param_id] != -1) {
          // ...but it's weight-shared in target, so that's fine.
          continue;
        } else {
          LOG(FATAL) << "Incompatible number of blobs for layer "
              << source_layer_name;
        }
      }
      hdf5_load_nd_dataset(layer_hid, dataset_name.c_str(), 0, kMaxBlobAxes,
          target_blobs[j].get());
    }
    H5Gclose(layer_hid);
  }
  H5Gclose(data_hid);
  H5Fclose(file_hid);
}

template <typename Dtype>
void Net<Dtype>::ToProto(NetParameter* param, bool write_diff) const {
  param->Clear();
  param->set_name(name_);
  // Add bottom and top
  for (int_tp i = 0; i < net_input_blob_indices_.size(); ++i) {
    param->add_input(blob_names_[net_input_blob_indices_[i]]);
  }
  DLOG(INFO)<< "Serializing " << layers_.size() << " layers";
  for (int_tp i = 0; i < layers_.size(); ++i) {
    LayerParameter* layer_param = param->add_layer();
    layers_[i]->ToProto(layer_param, write_diff);
  }
}


template <typename Dtype>
void Net<Dtype>::ToHDF5(const string& filename, bool write_diff) const {
  hid_t file_hid = H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
      H5P_DEFAULT);
  CHECK_GE(file_hid, 0)
      << "Couldn't open " << filename << " to save weights.";
  hid_t data_hid = H5Gcreate2(file_hid, "data", H5P_DEFAULT, H5P_DEFAULT,
      H5P_DEFAULT);
  CHECK_GE(data_hid, 0) << "Error saving weights to " << filename << ".";
  hid_t diff_hid = -1;
  if (write_diff) {
    diff_hid = H5Gcreate2(file_hid, "diff", H5P_DEFAULT, H5P_DEFAULT,
        H5P_DEFAULT);
    CHECK_GE(diff_hid, 0) << "Error saving weights to " << filename << ".";
  }
  for (int_tp layer_id = 0; layer_id < layers_.size(); ++layer_id) {
    const LayerParameter& layer_param = layers_[layer_id]->layer_param();
    string layer_name = layer_param.name();
    hid_t layer_data_hid = H5Gcreate2(data_hid, layer_name.c_str(),
        H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    CHECK_GE(layer_data_hid, 0)
        << "Error saving weights to " << filename << ".";
    hid_t layer_diff_hid = -1;
    if (write_diff) {
      layer_diff_hid = H5Gcreate2(diff_hid, layer_name.c_str(),
          H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      CHECK_GE(layer_diff_hid, 0)
          << "Error saving weights to " << filename << ".";
    }
    int_tp num_params = layers_[layer_id]->blobs().size();
    for (int_tp param_id = 0; param_id < num_params; ++param_id) {
      ostringstream dataset_name;
      dataset_name << param_id;
      const int_tp net_param_id = param_id_vecs_[layer_id][param_id];
      if (param_owners_[net_param_id] == -1) {
        // Only save params that own themselves
        hdf5_save_nd_dataset<Dtype>(layer_data_hid, dataset_name.str(),
            *params_[net_param_id]);
      }
      if (write_diff) {
        // Write diffs regardless of weight-sharing
        hdf5_save_nd_dataset<Dtype>(layer_diff_hid, dataset_name.str(),
            *params_[net_param_id], true);
      }
    }
    H5Gclose(layer_data_hid);
    if (write_diff) {
      H5Gclose(layer_diff_hid);
    }
  }
  H5Gclose(data_hid);
  if (write_diff) {
    H5Gclose(diff_hid);
  }
  H5Fclose(file_hid);
}

template <typename Dtype>
void Net<Dtype>::Update() {
  for (int_tp i = 0; i < learnable_params_.size(); ++i) {
    learnable_params_[i]->Update();
  }
}

template <typename Dtype>
void Net<Dtype>::ClearParamDiffs() {
  for (int_tp i = 0; i < learnable_params_.size(); ++i) {
    Blob<Dtype>* blob = learnable_params_[i];
    switch (Caffe::mode()) {
    case Caffe::CPU:
      caffe_set(blob->count(), static_cast<Dtype>(0),
                blob->mutable_cpu_diff());
      break;
    case Caffe::GPU:
#ifndef CPU_ONLY
      if (Caffe::GetDefaultDevice()->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
      caffe_gpu_set(blob->count(), static_cast<Dtype>(0),
                    blob->mutable_gpu_diff());
#endif  // USE_CUDA
      } else {
#ifdef USE_GREENTEA
          greentea_gpu_set(Caffe::GetDefaultDevice()->id(),
                           blob->count(), static_cast<Dtype>(0),
                           (cl_mem)(blob->mutable_gpu_diff()), 0);
#endif  // USE_GREENTEA
      }
#else
        NO_GPU;
#endif
      break;
    }
  }
}

template <typename Dtype>
void Net<Dtype>::ShareWeights() {
  for (int_tp i = 0; i < params_.size(); ++i) {
    if (param_owners_[i] < 0) { continue; }
    params_[i]->ShareData(*params_[param_owners_[i]]);
    params_[i]->ShareDiff(*params_[param_owners_[i]]);
  }
}

template<typename Dtype>
bool Net<Dtype>::has_blob(const string& blob_name) const {
  return blob_names_index_.find(blob_name) != blob_names_index_.end();
}

template<typename Dtype>
const shared_ptr<Blob<Dtype> > Net<Dtype>::blob_by_name(
    const string& blob_name) const {
  shared_ptr<Blob<Dtype> > blob_ptr;
  if (has_blob(blob_name)) {
    blob_ptr = blobs_[blob_names_index_.find(blob_name)->second];
  } else {
    blob_ptr.reset((Blob<Dtype>*) (NULL));
    LOG(WARNING)<< "Unknown blob name " << blob_name;
  }
  return blob_ptr;
}

template<typename Dtype>
bool Net<Dtype>::has_layer(const string& layer_name) const {
  return layer_names_index_.find(layer_name) != layer_names_index_.end();
}

template<typename Dtype>
const shared_ptr<Layer<Dtype> > Net<Dtype>::layer_by_name(
    const string& layer_name) const {
  shared_ptr<Layer<Dtype> > layer_ptr;
  if (has_layer(layer_name)) {
    layer_ptr = layers_[layer_names_index_.find(layer_name)->second];
  } else {
    layer_ptr.reset((Layer<Dtype>*) (NULL));
    LOG(WARNING)<< "Unknown layer name " << layer_name;
  }
  return layer_ptr;
}

INSTANTIATE_CLASS(Net);

}  // namespace caffe
