#include <sstream>
#include <iostream>
#include <vector>
#include <limits>

#include <boost/program_options.hpp>
#include <boost/program_options/variables_map.hpp>

#include "filelib.h"
#include "stringlib.h"
#include "weights.h"
#include "hg_io.h"
#include "kbest.h"
#include "viterbi.h"
#include "ns.h"
#include "ns_docscorer.h"

using namespace std;
namespace po = boost::program_options;

void InitCommandLine(int argc, char** argv, po::variables_map* conf) {
  po::options_description opts("Configuration options");
  opts.add_options()
        ("reference,r",po::value<vector<string> >(), "[REQD] Reference translation (tokenized text)")
        ("weights,w",po::value<string>(), "[REQD] Weights files from current iterations")
        ("input,i",po::value<string>()->default_value("-"), "Input file to map (- is STDIN)")
        ("evaluation_metric,m",po::value<string>()->default_value("IBM_BLEU"), "Evaluation metric (ibm_bleu, koehn_bleu, nist_bleu, ter, meteor, etc.)")
        ("kbest_size,k",po::value<unsigned>()->default_value(500u), "Top k-hypotheses to extract")
        ("cccp_iterations,I", po::value<unsigned>()->default_value(10u), "CCCP iterations (T')")
        ("ssd_iterations,J", po::value<unsigned>()->default_value(5u), "Stochastic subgradient iterations (T'')")
        ("eta", po::value<double>()->default_value(1e-4), "Step size")
        ("regularization_strength,C", po::value<double>()->default_value(1.0), "L2 regularization strength")
        ("alpha,a", po::value<double>()->default_value(10.0), "Cost scale (alpha); alpha * [1-metric(y,y')]")
        ("help,h", "Help");
  po::options_description dcmdline_options;
  dcmdline_options.add(opts);
  po::store(parse_command_line(argc, argv, dcmdline_options), *conf);
  bool flag = false;
  if (!conf->count("reference")) {
    cerr << "Please specify one or more references using -r <REF.TXT>\n";
    flag = true;
  }
  if (!conf->count("weights")) {
    cerr << "Please specify weights using -w <WEIGHTS.TXT>\n";
    flag = true;
  }
  if (flag || conf->count("help")) {
    cerr << dcmdline_options << endl;
    exit(1);
  }
}

struct HypInfo {
  HypInfo() : g(-100.0f) {}
  HypInfo(const vector<WordID>& h,
          const SparseVector<weight_t>& feats,
          const SegmentEvaluator& scorer, const EvaluationMetric* metric) : hyp(h), x(feats) {
    SufficientStats ss;
    scorer.Evaluate(hyp, &ss);
    g = metric->ComputeScore(ss);
    if (!metric->IsErrorMetric()) g = 1 - g;
  }

  vector<WordID> hyp;
  float g;
  SparseVector<weight_t> x;
};

void CostAugmentedSearch(const vector<HypInfo>& kbest,
                         const SparseVector<double>& w,
                         double alpha,
                         SparseVector<double>* fmap) {
  unsigned best_i = 0;
  double best = -numeric_limits<double>::infinity();
  for (unsigned i = 0; i < kbest.size(); ++i) {
    double s = kbest[i].x.dot(w) + alpha * kbest[i].g;
    if (s > best) {
      best = s;
      best_i = i;
    }
  }
  *fmap = kbest[best_i].x;
}

// runs lines 4--15 of rampion algorithm
int main(int argc, char** argv) {
  po::variables_map conf;
  InitCommandLine(argc, argv, &conf);
  const string evaluation_metric = conf["evaluation_metric"].as<string>();

  EvaluationMetric* metric = EvaluationMetric::Instance(evaluation_metric);
  DocumentScorer ds(metric, conf["reference"].as<vector<string> >());
  cerr << "Loaded " << ds.size() << " references for scoring with " << evaluation_metric << endl;
  double goodsign = -1;
  double badsign = -goodsign;

  Hypergraph hg;
  string last_file;
  ReadFile in_read(conf["input"].as<string>());
  istream &in=*in_read.stream();
  const unsigned kbest_size = conf["kbest_size"].as<unsigned>();
  const unsigned tp = conf["cccp_iterations"].as<unsigned>();
  const unsigned tpp = conf["ssd_iterations"].as<unsigned>();
  const double eta = conf["eta"].as<double>();
  const double reg = conf["regularization_strength"].as<double>();
  const double alpha = conf["alpha"].as<double>();
  SparseVector<weight_t> weights;
  {
    vector<weight_t> vweights;
    const string weightsf = conf["weights"].as<string>();
    Weights::InitFromFile(weightsf, &vweights);
    Weights::InitSparseVector(vweights, &weights);
  }
  string line, file;
  vector<vector<HypInfo> > kis;
  cerr << "Loading hypergraphs...\n";
  while(getline(in, line)) {
    istringstream is(line);
    int sent_id;
    kis.resize(kis.size() + 1);
    vector<HypInfo>& curkbest = kis.back();
    is >> file >> sent_id;
    ReadFile rf(file);
    if (kis.size() % 5 == 0) { cerr << '.'; }
    if (kis.size() % 200 == 0) { cerr << " [" << kis.size() << "]\n"; }
    HypergraphIO::ReadFromJSON(rf.stream(), &hg);
    hg.Reweight(weights);
    KBest::KBestDerivations<vector<WordID>, ESentenceTraversal> kbest(hg, kbest_size);

    for (int i = 0; i < kbest_size; ++i) {
      const KBest::KBestDerivations<vector<WordID>, ESentenceTraversal>::Derivation* d =
        kbest.LazyKthBest(hg.nodes_.size() - 1, i);
      if (!d) break;
      curkbest.push_back(HypInfo(d->yield, d->feature_values, *ds[sent_id], metric));
    }
  }
  cerr << "\nHypergraphs loaded.\n";

  vector<SparseVector<weight_t> > goals(kis.size());  // f(x_i,y+,h+)
  SparseVector<weight_t> fear;  // f(x,y-,h-)
  for (unsigned iterp = 1; iterp <= tp; ++iterp) {
    cerr << "CCCP Iteration " << iterp << endl;
    for (int i = 0; i < goals.size(); ++i)
      CostAugmentedSearch(kis[i], weights, goodsign * alpha, &goals[i]);
    for (unsigned iterpp = 1; iterpp <= tpp; ++iterpp) {
      cerr << "  SSD Iteration " << iterpp << endl;
      for (int i = 0; i < goals.size(); ++i) {
        CostAugmentedSearch(kis[i], weights, badsign * alpha, &fear);
        weights -= weights * (eta * reg / goals.size());
        weights += (goals[i] - fear) * eta;
      }
    }
  }
  vector<weight_t> w;
  weights.init_vector(&w);
  Weights::WriteToFile("-", w);
  return 0;
}

