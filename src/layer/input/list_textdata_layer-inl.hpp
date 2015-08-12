#ifndef TEXTNET_LAYER_LIST_TEXTDATA_LAYER_INL_HPP_
#define TEXTNET_LAYER_LIST_TEXTDATA_LAYER_INL_HPP_

#include <iostream>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>
#include <algorithm>
#include "stdlib.h"

#include <mshadow/tensor.h>
#include "../layer.h"
#include "../op.h"

namespace textnet {
namespace layer {

bool list_size_cmp(const vector<int> &x1, const vector<int> &x2); 

template<typename xpu>
class ListTextDataLayer : public Layer<xpu>{
 public:
  ListTextDataLayer(LayerType type) { this->layer_type = type; }
  virtual ~ListTextDataLayer(void) {}
  
  virtual int BottomNodeNum() { return 0; }
  virtual int TopNodeNum() { return 2; }
  virtual int ParamNodeNum() { return 0; }
  
  virtual void Require() {
    // default value, just set the value you want
    this->defaults["min_doc_len"] = SettingV(1);
	this->defaults["speedup_list"] = SettingV(true);
    // require value, set to SettingV(),
    // it will force custom to set in config
    this->defaults["data_file"] = SettingV();
    this->defaults["max_doc_len"] = SettingV();
    
    Layer<xpu>::Require();
  }
  
  virtual void SetupLayer(std::map<std::string, SettingV> &setting,
                          const std::vector<Node<xpu>*> &bottom,
                          const std::vector<Node<xpu>*> &top,
                          mshadow::Random<xpu> *prnd) {
    Layer<xpu>::SetupLayer(setting, bottom, top, prnd);
    
    utils::Check(bottom.size() == BottomNodeNum(),
                  "TextDataLayer:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(),
                  "TextDataLayer:top size problem.");

    data_file = setting["data_file"].sVal();
    max_doc_len = setting["max_doc_len"].iVal();
    min_doc_len = setting["min_doc_len"].iVal();
	speedup_list = setting["speedup_list"].bVal();
    
    ReadTextData();
    
    line_ptr = 0;
  }

  int ReadLabel(string &line) {
    std::istringstream iss;
    int label = -1;
    iss.clear();
    iss.seekg(0, iss.beg);
    iss.str(line);
    iss >> label;
    return label;
  }
  
  void ReadLine(int idx, string &line) {
    std::istringstream iss;
    int len_s1 = 0;
    int len_s2 = 0;
	int label = -1;
    iss.clear();
    iss.seekg(0, iss.beg);
    iss.str(line);
    iss >> label >> len_s1 >> len_s2;
	label_set.push_back(label);

	vector<int> q(len_s1);
    for (int j = 0; j < len_s1; ++j) {
      iss >> q[j];
    }
	q_data_set.push_back(q);

	vector<int> a(len_s2);
    for (int j = 0; j < len_s2; ++j) {
      iss >> a[j];
    }
	a_data_set.push_back(a);
  }

  void ReadTextData() {
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

    for (int i = 0; i < line_count; ++i) {
      ReadLine(i, lines[i]);
    }

	MakeLists(label_set, list_set);

    utils::Printf("Line count in file: %d\n", line_count);
    utils::Printf("Max list length: %d\n", max_list);
  }
  
  void MakeLists(vector<int> &label_set, vector<vector<int> > &list_set) {
    vector<int> list;
    max_list = 0;
    list.push_back(0);
    for (int i = 1; i < line_count; ++i) {
      if (label_set[i] == 1) {
        list_set.push_back(list);
        max_list = std::max(max_list, (int)list.size());
        list = vector<int>();
        list.push_back(i);
      } else {
        list.push_back(i);
      }
    }
    list_set.push_back(list);
    max_list = std::max(max_list, (int)list.size());

    // for speed up we can sort list by list.size()
	if (speedup_list)
      sort(list_set.begin(), list_set.end(), list_size_cmp);
  }

  virtual void Reshape(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top,
                       bool show_info = false) {
    utils::Check(bottom.size() == BottomNodeNum(),
                  "TextDataLayer:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(),
                  "TextDataLayer:top size problem.");
    
    utils::Check(max_doc_len > 0, "max_doc_len <= 0");

    top[0]->Resize(max_list, 2, 1, max_doc_len, true);
    top[1]->Resize(max_list, 1, 1, 1, true);
    
    if (show_info) {
        top[0]->PrintShape("top0");
        top[1]->PrintShape("top1");
    }
  }
  
  virtual void CheckReshape(const std::vector<Node<xpu>*> &bottom,
                            const std::vector<Node<xpu>*> &top) {
    // Check for reshape
    bool need_reshape = false;
    if (max_list != list_set[line_ptr].size()) {
        need_reshape = true;
        max_list = list_set[line_ptr].size();
    }

    // Do reshape 
    if (need_reshape) {
        this->Reshape(bottom, top);
    }
  }

  virtual void Forward(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top) {
    using namespace mshadow::expr;
    mshadow::Tensor<xpu, 3> top0_data = top[0]->data_d3();
    mshadow::Tensor<xpu, 2> top0_length = top[0]->length;
    mshadow::Tensor<xpu, 1> top1_data = top[1]->data_d1();

	top0_data = -1;
	top0_length = 0;
	top1_data = -1;

    for (int i = 0; i < list_set[line_ptr].size(); ++i) {
	  int idx = list_set[line_ptr][i];
	  for (int j = 0; j < q_data_set[idx].size(); ++j) {
		top0_data[i][0][j] = q_data_set[idx][j];
	  }
	  for (int j = 0; j < a_data_set[idx].size(); ++j) {
		top0_data[i][1][j] = a_data_set[idx][j];
	  }
      top0_length[i][0] = q_data_set[idx].size();
	  top0_length[i][1] = a_data_set[idx].size();
      top1_data[i] = label_set[idx];
    }
    line_ptr = (line_ptr + 1) % list_set.size();
  }
  
  virtual void Backprop(const std::vector<Node<xpu>*> &bottom,
                        const std::vector<Node<xpu>*> &top) {
    using namespace mshadow::expr;
    
  }
  
 protected:
  std::string data_file;
  int batch_size;
  int max_doc_len;
  int min_doc_len;
  bool shuffle;
  bool speedup_list;

  vector<vector<int> > q_data_set;
  vector<vector<int> > a_data_set;
  vector<int> label_set;
  int line_count;
  int max_list;
  int line_ptr;

  vector<vector<int> > list_set;
};
}  // namespace layer
}  // namespace textnet
#endif  // LAYER_LIST_TEXTDATA_LAYER_INL_HPP_

