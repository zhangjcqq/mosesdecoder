/*
 * LexicalReordering.cpp
 *
 *  Created on: 15 Dec 2015
 *      Author: hieu
 */

#include <boost/foreach.hpp>
#include "LexicalReordering.h"
#include "LRModel.h"
#include "PhraseBasedReorderingState.h"
#include "BidirectionalReorderingState.h"
#include "../../TranslationModel/PhraseTable.h"
#include "../../System.h"
#include "../../PhraseImpl.h"
#include "../../PhraseBased/Manager.h"
#include "../../PhraseBased/Hypothesis.h"
#include "../../legacy/InputFileStream.h"
#include "../../legacy/Util2.h"
#include "../../legacy/CompactPT/LexicalReorderingTableCompact.h"

using namespace std;

namespace Moses2 {

///////////////////////////////////////////////////////////////////////

LexicalReordering::LexicalReordering(size_t startInd, const std::string &line)
:StatefulFeatureFunction(startInd, line)
,m_compactModel(NULL)
,m_blank(NULL)
,m_propertyInd(-1)
,m_coll(NULL)
,m_lrModel(NULL)
{
	ReadParameters();
	assert(m_lrModel);
	//assert(m_numScores == 6);
}

LexicalReordering::~LexicalReordering()
{
	delete m_compactModel;
	delete m_coll;
	delete m_blank;
	delete m_lrModel;
}

void LexicalReordering::Load(System &system)
{
  MemPool &pool = system.GetSystemPool();

  if (m_propertyInd >= 0) {
	  // Using integrate Lex RO. No loading needed
  }
  else if (FileExists(m_path + ".minlexr") ) {
	  m_compactModel = new LexicalReorderingTableCompact(m_path + ".minlexr", m_FactorsF,
			  m_FactorsE, m_FactorsC);
	  m_blank = new (pool.Allocate<PhraseImpl>()) PhraseImpl(pool, 0);
  }
  else {
	  m_coll = new Coll();
	  InputFileStream file(m_path);
	  string line;
	  size_t lineNum = 0;

	  while(getline(file, line)) {
		if (++lineNum % 1000000 == 0) {
			cerr << lineNum << " ";
		}

		std::vector<std::string> toks = TokenizeMultiCharSeparator(line, "|||");
		assert(toks.size() == 3);
		PhraseImpl *source = PhraseImpl::CreateFromString(pool, system.GetVocab(), system, toks[0]);
		PhraseImpl *target = PhraseImpl::CreateFromString(pool, system.GetVocab(), system, toks[1]);
		std::vector<SCORE> scores = Tokenize<SCORE>(toks[2]);
		std::transform(scores.begin(), scores.end(), scores.begin(), TransformScore);
		std::transform(scores.begin(), scores.end(), scores.begin(), FloorScore);

		Key key(source, target);
		(*m_coll)[key] = scores;
	  }
  }
}

#include "util/exception.hh"

void LexicalReordering::SetParameter(const std::string& key, const std::string& value)
{
  if (key == "path") {
	  m_path = value;
  }
  else if (key == "type") {
	  m_lrModel = new LRModel(value);
	  if (m_lrModel->IsPhraseBased()) {
		  UTIL_THROW_IF2(value != "msd-bidirectional-fe"
				  && value != "wbe-msd-bidirectional-fe-allff",
				  "Lex RO type not supported");
	  }
	  else {
		  UTIL_THROW_IF2(value != "hier-mslr-birectional-fe",
				  "Lex RO type not supported");
	  }
  }
  else if (key == "input-factor") {
	  m_FactorsF = Tokenize<FactorType>(value);
  }
  else if (key == "output-factor") {
	  m_FactorsE = Tokenize<FactorType>(value);
  }
  else if (key == "property-index") {
	  m_propertyInd = Scan<int>(value);
  }
  else {
	  StatefulFeatureFunction::SetParameter(key, value);
  }
}

FFState* LexicalReordering::BlankState(MemPool &pool) const
{
  FFState *ret = m_lrModel->CreateLRState();
  return ret;
}

void LexicalReordering::EmptyHypothesisState(FFState &state,
		const ManagerBase &mgr,
		const InputType &input,
		const Hypothesis &hypo) const
{
  if (m_lrModel->IsPhraseBased()) {
	PhraseBasedReorderingState &stateCast = static_cast<PhraseBasedReorderingState&>(state);
	stateCast.prevPath = &hypo.GetInputPath();
	stateCast.prevTP = &hypo.GetTargetPhrase();
  }
  else {
    BidirectionalReorderingState &stateCast = static_cast<BidirectionalReorderingState&>(state);

  }
}

void LexicalReordering::EvaluateInIsolation(MemPool &pool,
		const System &system,
		const Phrase &source,
		const TargetPhrase &targetPhrase,
		Scores &scores,
		SCORE *estimatedScore) const
{
}

void LexicalReordering::EvaluateAfterTablePruning(MemPool &pool,
		const TargetPhrases &tps,
		const Phrase &sourcePhrase) const
{
  BOOST_FOREACH(const TargetPhrase *tp, tps) {
	  EvaluateAfterTablePruning(pool, *tp, sourcePhrase);
  }
}

void LexicalReordering::EvaluateAfterTablePruning(MemPool &pool,
		const TargetPhrase &targetPhrase,
		const Phrase &sourcePhrase) const
{
  if (m_propertyInd >= 0) {
	  SCORE *scoreArr = targetPhrase.GetScoresProperty(m_propertyInd);
	  targetPhrase.ffData[m_PhraseTableInd] = scoreArr;
  }
  else if (m_compactModel) {
	  // using external compact binary model
	  const Values values = m_compactModel->GetScore(sourcePhrase, targetPhrase, *m_blank);
	  if (values.size()) {
		assert(values.size() == 6);
		SCORE *scoreArr = pool.Allocate<SCORE>(6);
		for (size_t i = 0; i < 6; ++i) {
			scoreArr[i] = values[i];
		}
		targetPhrase.ffData[m_PhraseTableInd] = scoreArr;
	  }
	  else {
		targetPhrase.ffData[m_PhraseTableInd] = NULL;
	  }
  }
  else if (m_coll) {
	  // using external memory model

	  // cache data in target phrase
	  const Values *values = GetValues(sourcePhrase, targetPhrase);
	  if (values) {
		SCORE *scoreArr = pool.Allocate<SCORE>(6);
		for (size_t i = 0; i < 6; ++i) {
			scoreArr[i] = (*values)[i];
		}
		targetPhrase.ffData[m_PhraseTableInd] = scoreArr;
	  }
	  else {
		targetPhrase.ffData[m_PhraseTableInd] = NULL;
	  }
  }
}

void LexicalReordering::EvaluateWhenApplied(const ManagerBase &mgr,
  const Hypothesis &hypo,
  const FFState &prevState,
  Scores &scores,
  FFState &state) const
{
  const LRState &prevStateCast = static_cast<const LRState&>(prevState);
  prevStateCast.Expand(mgr.system, *this, hypo, m_PhraseTableInd, scores, state);
}

const LexicalReordering::Values *LexicalReordering::GetValues(const Phrase &source, const Phrase &target) const
{
	Key key(&source, &target);
	Coll::const_iterator iter;
	iter = m_coll->find(key);
	if (iter == m_coll->end()) {
		return NULL;
	}
	else {
		return &iter->second;
	}
}

} /* namespace Moses2 */
