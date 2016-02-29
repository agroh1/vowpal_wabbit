/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include "vw.h"
#include "reductions.h"
#include "gd.h"
#include "cb_algs.h"

using namespace std;
using namespace LEARNER;

namespace MWT {  
  struct policy_data 
  { double cost;
    uint32_t action;
    bool seen;
  };

  struct mwt
  { bool namespaces[256]; // the set of namespaces to evaluate.   
    v_array<policy_data > evals; // accrued losses of features.
    CB::cb_class* observation;
    v_array<uint64_t> policies;
    double total;
    vw* all;
  }; 
  
  inline bool observed_cost(CB::cb_class* cl)
  { //cost observed for this action if it has non zero probability and cost != FLT_MAX
    if (cl != nullptr)
      if (cl->cost != FLT_MAX && cl->probability > .0)
	return true;
    return false;
  }
  
  CB::cb_class* get_observed_cost(CB::label& ld)
  { for (auto& cl : ld.costs)
      if( observed_cost(&cl) )
	return &cl;
    return nullptr;
  }
  
  void value_policy(mwt& c, float val, uint64_t index)//estimate the value of a single feature.
  {
    if (val < 0 || floor(val) != val)
      cout << "error " << val << " is not a valid action " << endl;
    
    uint32_t value = (uint32_t) val;
    uint64_t new_index = ((index & c.all->reg.weight_mask) >> c.all->reg.stride_shift);

    if (!c.evals[new_index].seen)
      {
	c.evals[new_index].seen = true;
	c.policies.push_back(new_index);
      }

    c.evals[new_index].action = value;
  }
  
  template <bool is_learn>
  void predict_or_learn(mwt& c, base_learner& base, example& ec)
  {
    v_array<float> preds = ec.pred.scalars;

    c.observation = get_observed_cost(ec.l.cb);

    if (c.observation != nullptr)
      {
	c.total++;
	//For each nonzero feature in observed namespaces, check it's value.
	for (unsigned char ns : ec.indices)
	  if (c.namespaces[ns])
	    GD::foreach_feature<mwt, value_policy>(c.all->reg.weight_vector, c.all->reg.weight_mask, ec.feature_space[ns], c);
	for (uint64_t policy : c.policies)
	  {
	    c.evals[policy].cost += get_unbiased_cost(c.observation, c.evals[policy].action); 
	    c.evals[policy].action = 0;
	  }
      }
    
    //modify the predictions to use a vector with a score for each evaluated feature.
    preds.erase();
    for(uint64_t index : c.policies)
      preds.push_back(c.evals[index].cost / c.total);
    
    ec.pred.scalars = preds;
  }

  void print_scalars(int f, v_array<float>& scalars, v_array<char>& tag)
  { if (f >= 0)
      { std::stringstream ss;
	
	for (size_t i = 0; i < scalars.size(); i++)
	  { if (i > 0)
	      ss << ' ';
	    ss << scalars[i];
	  }
	ss << '\n';
	ssize_t len = ss.str().size();
	ssize_t t = io_buf::write_file_or_socket(f, ss.str().c_str(), (unsigned int)len);
	if (t != len)
	  cerr << "write error: " << strerror(errno) << endl;
      }
  }

  void finish_example(vw& all, mwt& c, example& ec)
  {     
    float loss = 0.;
    if (c.observation != nullptr)
      loss = get_unbiased_cost(c.observation, ec.pred.scalars[0]);
    
    all.sd->update(ec.test_only, loss, 1.f, ec.num_features);
    
    for (int sink : all.final_prediction_sink)
      print_scalars(sink, ec.pred.scalars, ec.tag);
    
    v_array<float> temp = ec.pred.scalars;
    ec.pred.multiclass = (uint32_t)temp[0];
    CB::print_update(all, c.observation != nullptr, ec, nullptr, false);
    ec.pred.scalars = temp; 
    VW::finish_example(all, &ec);
  }
  
  void finish(mwt& c){ c.evals.delete_v(); c.policies.delete_v(); }
}
using namespace MWT;

void delete_scalars(void* v)
{ v_array<float>* preds = (v_array<float>*)v;
  preds->delete_v();
}

base_learner* mwt_setup(vw& all)
{ if (missing_option<string, true>(all, "multiworld_test", "Evaluate features as a policies"))
    return nullptr;
  
  mwt& c = calloc_or_throw<mwt>();
  
  string s = all.vm["multiworld_test"].as<string>();
  for (size_t i = 0; i < s.size(); i++)
    c.namespaces[(unsigned char)s[i]] = true;
  c.all = &all;
  
  calloc_reserve(c.evals, all.length());

  all.delete_prediction = delete_scalars;
  all.p->lp = CB::cb_label;

  learner<mwt>& l = init_learner(&c, setup_base(all), predict_or_learn<true>, predict_or_learn<false>, 1);
  l.set_finish_example(finish_example);
  l.set_finish(finish);
  return make_base(l);
}