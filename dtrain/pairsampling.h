#ifndef _DTRAIN_PAIRSAMPLING_H_
#define _DTRAIN_PAIRSAMPLING_H_

namespace dtrain
{


bool
accept_pair(score_t a, score_t b, score_t threshold)
{
  if (fabs(a - b) < threshold) return false;
  return true;
}

bool
cmp_hyp_by_score_d(ScoredHyp a, ScoredHyp b)
{
  return a.score > b.score;
}

inline void
all_pairs(vector<ScoredHyp>* s, vector<pair<ScoredHyp,ScoredHyp> >& training, score_t threshold, float _unused=1)
{
  sort(s->begin(), s->end(), cmp_hyp_by_score_d);
  unsigned sz = s->size();
  for (unsigned i = 0; i < sz-1; i++) {
    for (unsigned j = i+1; j < sz; j++) {
      if (threshold > 0) {
        if (accept_pair((*s)[i].score, (*s)[j].score, threshold))
          training.push_back(make_pair((*s)[i], (*s)[j]));
      } else {
        if ((*s)[i].score != (*s)[j].score)
          training.push_back(make_pair((*s)[i], (*s)[j]));
      }
    }
  }
}

/*
 * multipartite ranking
 *  sort (descending) by bleu
 *  compare top X to middle Y and low X
 *  cmp middle Y to low X
 */

inline void
partXYX(vector<ScoredHyp>* s, vector<pair<ScoredHyp,ScoredHyp> >& training, score_t threshold, float hi_lo)
{
  sort(s->begin(), s->end(), cmp_hyp_by_score_d);
  unsigned sz = s->size();
  unsigned sep = round(sz*hi_lo);
  for (unsigned i = 0; i < sep; i++) {
    for (unsigned j = sep; j < sz; j++) {
      if (threshold > 0) {
        if (accept_pair((*s)[i].score, (*s)[j].score, threshold))
          training.push_back(make_pair((*s)[i], (*s)[j]));
      } else {
        if ((*s)[i].score != (*s)[j].score)
          training.push_back(make_pair((*s)[i], (*s)[j]));
      }
    }
  }
  for (unsigned i = sep; i < sz-sep; i++) {
    for (unsigned j = sz-sep; j < sz; j++) {
      if (threshold > 0) {
        if (accept_pair((*s)[i].score, (*s)[j].score, threshold))
          training.push_back(make_pair((*s)[i], (*s)[j]));
      } else {
        if ((*s)[i].score != (*s)[j].score)
          training.push_back(make_pair((*s)[i], (*s)[j]));
      }
    }
  }
}

/*
 * pair sampling as in
 * 'Tuning as Ranking' (Hopkins & May, 2011)
 *     count = 5000
 * threshold = 5% BLEU (0.05 for param 3)
 *       cut = top 50
 */
bool
_PRO_cmp_pair_by_diff_d(pair<ScoredHyp,ScoredHyp> a, pair<ScoredHyp,ScoredHyp> b)
{
  return (fabs(a.first.score - a.second.score)) > (fabs(b.first.score - b.second.score));
}
inline void
PROsampling(vector<ScoredHyp>* s, vector<pair<ScoredHyp,ScoredHyp> >& training, score_t threshold, float _unused=1)
{
  unsigned max_count = 5000, count = 0, sz = s->size();
  bool b = false;
  for (unsigned i = 0; i < sz-1; i++) {
    for (unsigned j = i+1; j < sz; j++) {
      if (accept_pair((*s)[i].score, (*s)[j].score, threshold)) {
        training.push_back(make_pair((*s)[i], (*s)[j]));
        if (++count == max_count) {
          b = true;
          break;
        }
      }
    }
    if (b) break;
  }
  if (training.size() > 50) {
    sort(training.begin(), training.end(), _PRO_cmp_pair_by_diff_d);
    training.erase(training.begin()+50, training.end());
  }
  return;
}


} // namespace

#endif

