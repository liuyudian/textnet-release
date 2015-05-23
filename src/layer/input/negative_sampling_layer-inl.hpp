#ifndef TEXTNET_LAYER_NEGATIVE_SAMPLE_LAYER_INL_HPP_
#define TEXTNET_LAYER_NEGATIVE_SAMPLE_LAYER_INL_HPP_

#include <iostream>
#include <fstream>
#include <sstream>

#include <mshadow/tensor.h>
#include "../layer.h"
#include "../op.h"

namespace textnet {
namespace layer {

template<typename xpu>
class NegativeSample : public Layer<xpu>{
 public:
  NegativeSample(LayerType type) { this->layer_type = type; }
  virtual ~NegativeSample(void) {}
  
  virtual int BottomNodeNum() { return 0; }
  virtual int TopNodeNum() { return 4; } // x, pos, sample, y
  virtual int ParamNodeNum() { return 0; }
  virtual void Require() {
    // default value, just set the value you want
    this->defaults["shuffle_seed"] = SettingV(123);

    // require value, set to SettingV(),
    // it will force custom to set in config
    this->defaults["data_file"] = SettingV();
    this->defaults["batch_size"] = SettingV();
    this->defaults["max_doc_len"] = SettingV();
    this->defaults["negative_num"] = SettingV();
    this->defaults["position_num"] = SettingV();
    this->defaults["vocab_size"] = SettingV();
    
    Layer<xpu>::Require();
  }

  virtual void SetupLayer(std::map<std::string, SettingV> &setting,
                          const std::vector<Node<xpu>*> &bottom,
                          const std::vector<Node<xpu>*> &top,
                          mshadow::Random<xpu> *prnd) {
    Layer<xpu>::SetupLayer(setting, bottom, top, prnd);
    
    utils::Check(bottom.size() == BottomNodeNum(),
                  "NegativeSample:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(),
                  "NegativeSample:top size problem.");
                  
    data_file = setting["data_file"].s_val;
    batch_size = setting["batch_size"].i_val;
    max_doc_len = setting["max_doc_len"].i_val;
    negative_num = setting["negative_num"].i_val;
    position_num = setting["position_num"].i_val;
    vocab_size = setting["vocab_size"].i_val;

    ReadSequenceData();
    
    line_ptr = 0;
    sampler.Seed(shuffle_seed);
  }

  // return a list of negative samples
  void negative_sampler(vector<int> &negative_sample) {
    negative_sample.clear();
    for (int i = 0; i < negative_num; ++i) {
      int sample = this->sampler.NextUInt32(vocab_size);
      utils::Assert(sample >= 0 && sample < vocab_size, "NegativeSample: sampler error");
      negative_sample.push_back(sample)
    }
  }
  // return a list of prediction positions
  void position_sampler(int length, vector<int> &position_sample) {
    position_sample.clear();
    vector<int> shuffle_pos;
    for (int i = 0; i < length; ++i) {
      shuffle_pos.push_back(i);
    } 
    this->sampler.Shuffle(shuffle_pos);

    int end = length < position_num ? length : position_num;
    position_sample = vector<int>(shuffle_pos.begin(), shuffle_pos.begin() + end);
    for (int i = length; i < position_sample; ++i) {
      position_sample.push_back(-1);
    }
    utils::Check(position_sample.size() == position_num, "NegativeSample: sampler error.");
  }
  
  void ReadSequenceData() {
    utils::Printf("Open data file: %s\n", data_file.c_str());	
    std::vector<std::string> lines;
    std::ifstream fin(data_file.c_str());
    std::string s;
    utils::Check(fin, "Open data file problem.");
    while (!fin.eof()) {
      std::getline(fin, s);
      if (s.empty()) break;
      lines.push_back(s);
    }
    fin.close();
    
    line_count = lines.size();
	utils::Printf("Line count in file: %d\n", line_count);

    data_set.Resize(mshadow::Shape4(line_count, 1, 1, max_doc_len), 0);
    length.Resize(mshadow::Shape1(line_count), -1);

    std::istringstream iss;
    for (int i = 0; i < line_count; ++i) {
      iss.clear();
	  iss.seekg(0, iss.beg);
      iss.str(lines[i]);
      int j = 0;
      while (!iss.eof()) {
        iss >> data_set[i][0][0][j++];
      }
      length[i] = j;
      utils::Check(j <= max_doc_len, "NegativeSample: doc length error.");
    }

    // gen example ids
    for (int i = 0; i < line_count; ++i) {
      example_ids.push_back(i);
    }
  }
  
  virtual void Reshape(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top) {
    utils::Check(bottom.size() == BottomNodeNum(),
                  "NegativeSample:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(),
                  "NegativeSample:top size problem.");
    top[0]->Resize(batch_size, 1, 1, max_doc_len, true);              // x
    top[1]->Resize(batch_size, position_num, 1, 1, true);             // pos
    top[2]->Resize(batch_size, position_num, sample_num+1, 1, true);  // sample
    top[3]->Resize(batch_size, position_num, sample_num+1, 1, true);  // y
  }
  
  virtual void Forward(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top) {
    using namespace mshadow::expr;
    mshadow::Tensor<xpu, 4> x        = top[0]->data;
    mshadow::Tensor<xpu, 4> pos      = top[1]->data;
    mshadow::Tensor<xpu, 4> sample   = top[2]->data;
    mshadow::Tensor<xpu, 4> y        = top[3]->data;
    mshadow::Tensor<xpu, 2> x_length = top[0]->length;

    utils::Check(x.size(0) == batch_size, "ORC: error, need reshape.");
    x = 0.f, pos = 0.f, sample = 0.f, y = 0.f, x_length = -1.f;
    
    vector<int> position_sample, negative_sample;
    for (int batch_idx = 0; batch_idx < batch_size; ++batch_idx) {
      if (this->phrase_type == kTrain && line_ptr == 0) {
        this->sampler.Shuffle(example_ids);
      }
      int example_id = example_ids[line_ptr];
      x[batch_idx] = F<op::identity>(data_set[example_id]);
      x_length[batch_idx][0] = length[example_id];
      
      position_sampler(length, position_sample);
      for (int pos_idx = 0; pos_idx < position_num; ++pos_idx) {
        pos[batch_idx][pos_idx][0][0] = position_sample[pos_idx];
        if (position_sample[pos_idx] == -1) {
          continue;
        }
        int word_idx = x[batch_idx][0][0][position_sample[pos_idx]];
        sample[batch_idx][pos_idx][0][0] = word_idx;
        y[batch_idx][pos_idx][0][0] = 1;
        negative_sampler(negative_sample);
        for (int sample_idx = 0; sample_idx < sample_num; ++sample_idx) {
          sample[batch_idx][pos_idx][sample_idx+1][0] = negative_sample[sample_idx];
          y[batch_idx][pos_idx][sample_idx+1][0] = 0;
        }
      }
      
      line_ptr = (line_ptr + 1) % line_count;
    }
  }
  
  virtual void Backprop(const std::vector<Node<xpu>*> &bottom,
                        const std::vector<Node<xpu>*> &top) {
    using namespace mshadow::expr;
  }

 public:
  std::string data_file;
  int batch_size, max_doc_len, negative_num, position_num, vocab_size;
  mshadow::TensorContainer<xpu, 4> data_set;
  mshadow::TensorContainer<xpu, 1, int> length;
  std::vector<int> example_ids;
  int line_count, line_ptr, shuffle_seed;
  utils::RandomSampler sampler;
};
}  // namespace layer
}
#endif 

