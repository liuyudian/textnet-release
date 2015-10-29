#ifndef TEXTNET_LAYER_POS_PRED_REP_LAYER_INL_HPP_
#define TEXTNET_LAYER_POS_PRED_REP_LAYER_INL_HPP_

#include <mshadow/tensor.h>
#include "../layer.h"
#include "../op.h"

namespace textnet {
namespace layer {

// this layer is originally designed for pretraining LSTM
// this layer accepts representations generated by a BiLSTM
// and predicts the word on the position by the left and the right context representation
// the left and right context representations are concated
// the left and right border are padded by 0
template<typename xpu>
class PosPredRepLayer : public Layer<xpu>{
 public:
  PosPredRepLayer(LayerType type) { this->layer_type = type; }
  virtual ~PosPredRepLayer(void) {}
  
  // left  representaion list (batch_size, 1, length, feat_size)
  // right representaion list (batch_size, 1, length, feat_size)
  // position (batch_size, position_num, 1, 1)
  virtual int BottomNodeNum() { return 3; } 

  virtual int TopNodeNum() { return 1; }
  virtual int ParamNodeNum() { return 0; }
  
  virtual void Require() {
    // default value, just set the value you want

    // require value, set to SettingV(),
    // it will force custom to set in config
    
    Layer<xpu>::Require();
  }
  
  virtual void SetupLayer(std::map<std::string, SettingV> &setting,
                          const std::vector<Node<xpu>*> &bottom,
                          const std::vector<Node<xpu>*> &top,
                          mshadow::Random<xpu> *prnd) {
    Layer<xpu>::SetupLayer(setting, bottom, top, prnd);
  }
  
  virtual void Reshape(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top,
					   bool show_info = false) {
    utils::Check(bottom.size() == BottomNodeNum(), "PosPredRepLayer:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(), "PosPredRepLayer:top size problem.");

    mshadow::Shape<4> shape_in_l_rep  = bottom[0]->data.shape_;
    mshadow::Shape<4> shape_in_r_rep  = bottom[1]->data.shape_;
    mshadow::Shape<4> shape_in_pos    = bottom[2]->data.shape_;
    utils::Check(shape_in_l_rep == shape_in_r_rep, "PosPredRepLayer: shape error.");
    utils::Check(shape_in_pos[2] == 1 && shape_in_pos[3] == 1, "PosPredRepLayer: shape error.");
    feat_size = shape_in_l_rep[3];
    mshadow::Shape<4> shape_out = mshadow::Shape4(shape_in_pos[0], shape_in_pos[1], 1, feat_size * 2);
    top[0]->Resize(shape_out, true);

	if (show_info) {
      bottom[0]->PrintShape("bottom0");
      bottom[1]->PrintShape("bottom1");
      bottom[2]->PrintShape("bottom2");
      top[0]->PrintShape("top0");
	}
  }

  typedef mshadow::Tensor<xpu, 1> Tensor1D;
  typedef mshadow::Tensor<xpu, 2> Tensor2D;
  typedef mshadow::Tensor<xpu, 3> Tensor3D;
  typedef mshadow::Tensor<xpu, 4> Tensor4D;

  virtual void Forward(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top) {
    using namespace mshadow::expr;
    mshadow::Tensor<xpu, 4> l_rep    = bottom[0]->data;
    mshadow::Tensor<xpu, 2> l_len    = bottom[0]->length;
    mshadow::Tensor<xpu, 4> r_rep    = bottom[1]->data;
    mshadow::Tensor<xpu, 4> pos      = bottom[2]->data;
    mshadow::Tensor<xpu, 4> top_data = top[0]->data;

    top_data = 0;

    for (index_t batch_idx = 0; batch_idx < pos.size(0); ++batch_idx) {
      for (index_t pos_idx = 0; pos_idx < pos.size(1); ++pos_idx) {
        int length = l_len[batch_idx][0];
        int p = pos[batch_idx][pos_idx][0][0];
        utils::Check(p < length, "PosPredRepLayer: position error.");
        if (p > 0) {
          top_data[batch_idx][pos_idx][0].Slice(0, feat_size) = F<op::identity>(l_rep[batch_idx][0][p-1]);
          // top_data[batch_idx][pos_idx][0] += F<op::identity>(l_rep[batch_idx][0][p-1]);
        }
        if (p < length-1) {
          // top_data[batch_idx][pos_idx][0] += F<op::identity>(r_rep[batch_idx][0][p+1]);
          top_data[batch_idx][pos_idx][0].Slice(feat_size, feat_size*2) = F<op::identity>(r_rep[batch_idx][0][p+1]);
        }
      }
    }
  }
  
  virtual void Backprop(const std::vector<Node<xpu>*> &bottom,
                        const std::vector<Node<xpu>*> &top) {
    using namespace mshadow::expr;
    mshadow::Tensor<xpu, 4> l_diff   = bottom[0]->diff;
    mshadow::Tensor<xpu, 2> l_len    = bottom[0]->length;
    mshadow::Tensor<xpu, 4> r_diff   = bottom[1]->diff;
    mshadow::Tensor<xpu, 4> pos      = bottom[2]->data;
    mshadow::Tensor<xpu, 4> top_diff = top[0]->diff;

    for (index_t batch_idx = 0; batch_idx < pos.size(0); ++batch_idx) {
      for (index_t pos_idx = 0; pos_idx < pos.size(1); ++pos_idx) {
        int length = l_len[batch_idx][0];
        int p = pos[batch_idx][pos_idx][0][0];
        if (p > 0) {
          l_diff[batch_idx][0][p-1] += top_diff[batch_idx][pos_idx][0].Slice(0, feat_size);
          // l_diff[batch_idx][0][p-1] += top_diff[batch_idx][pos_idx][0];
        }
        if (p < length-1) {
          r_diff[batch_idx][0][p+1] += top_diff[batch_idx][pos_idx][0].Slice(feat_size, feat_size*2);
          // r_diff[batch_idx][0][p+1] += top_diff[batch_idx][pos_idx][0];
        }
      }
    }
  }
 protected:
  int feat_size;
};
}  // namespace layer
}  // namespace textnet
#endif 

