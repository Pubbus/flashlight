/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "flashlight/app/asr/criterion/Seq2SeqCriterion.h"

#include <algorithm>
#include <numeric>
#include <queue>

#include "flashlight/app/asr/common/Defines.h"
#include "flashlight/app/asr/criterion/CriterionUtils.h"

using namespace fl::ext;

namespace fl {
namespace app {
namespace asr {

namespace detail {
Seq2SeqState concatState(std::vector<Seq2SeqState>& stateVec) {
  if (stateVec.size() < 1) {
    throw std::runtime_error("Empty stateVec");
  }

  int nAttnRound = stateVec[0].hidden.size();
  Seq2SeqState newState(nAttnRound);
  newState.step = stateVec[0].step;
  newState.peakAttnPos = stateVec[0].peakAttnPos;
  newState.isValid = stateVec[0].isValid;

  std::vector<Variable> alphaVec;
  std::vector<std::vector<Variable>> hiddenVec(nAttnRound);
  std::vector<Variable> summaryVec;
  for (auto& state : stateVec) {
    if (state.step != newState.step) {
      throw std::runtime_error("step unmatched");
    } else if (state.isValid != newState.isValid) {
      throw std::runtime_error("isValid unmatched");
    }
    alphaVec.push_back(state.alpha);
    for (int i = 0; i < nAttnRound; i++) {
      hiddenVec[i].push_back(state.hidden[i]);
    }
    summaryVec.push_back(state.summary);
  }
  newState.alpha = concatenate(alphaVec, 2);
  for (int i = 0; i < nAttnRound; i++) {
    newState.hidden[i] = concatenate(hiddenVec[i], 1);
  }
  newState.summary = concatenate(summaryVec, 2);
  return newState;
}

Seq2SeqState selectState(Seq2SeqState& state, int batchIdx) {
  int nAttnRound = state.hidden.size();
  Seq2SeqState newState(nAttnRound);
  newState.step = state.step;
  newState.peakAttnPos = state.peakAttnPos;
  newState.isValid = state.isValid;
  newState.alpha = state.alpha(af::span, af::span, batchIdx);
  newState.summary = state.summary(af::span, af::span, batchIdx);
  for (int i = 0; i < nAttnRound; i++) {
    newState.hidden[i] = state.hidden[i](af::span, batchIdx);
  }
  return newState;
}
} // namespace detail

Seq2SeqCriterion::Seq2SeqCriterion(
    int nClass,
    int hiddenDim,
    int eos,
    int pad,
    int maxDecoderOutputLen,
    const std::vector<std::shared_ptr<AttentionBase>>& attentions,
    std::shared_ptr<WindowBase> window /* = nullptr*/,
    bool trainWithWindow /* false */,
    int pctTeacherForcing /* = 100 */,
    double labelSmooth /* = 0.0 */,
    bool inputFeeding /* = false */,
    std::string samplingStrategy, /* = fl::app::asr::kRandSampling */
    double gumbelTemperature /* = 1.0 */,
    int nRnnLayer /* = 1 */,
    int nAttnRound /* = 1 */,
    float dropOut /* = 0.0 */)
    : eos_(eos),
      pad_(pad),
      maxDecoderOutputLen_(maxDecoderOutputLen),
      window_(window),
      trainWithWindow_(trainWithWindow),
      pctTeacherForcing_(pctTeacherForcing),
      labelSmooth_(labelSmooth),
      inputFeeding_(inputFeeding),
      nClass_(nClass),
      samplingStrategy_(samplingStrategy),
      gumbelTemperature_(gumbelTemperature),
      nAttnRound_(nAttnRound) {
  // 1. Embedding
  add(std::make_shared<Embedding>(hiddenDim, nClass_));

  // 2. RNN
  for (int i = 0; i < nAttnRound_; i++) {
    add(std::make_shared<RNN>(
        hiddenDim, hiddenDim, nRnnLayer, RnnMode::GRU, false, dropOut));
  }

  // 3. Linear
  add(std::make_shared<Linear>(hiddenDim, nClass_));
  // FIXME: Having a linear layer in between RNN and attention is only for
  // backward compatibility.

  // 4. Attention
  for (int i = 0; i < nAttnRound_; i++) {
    add(attentions[i]);
  }

  // 5. Initial hidden state
  params_.push_back(fl::uniform(af::dim4{hiddenDim}, -1e-1, 1e-1));
  setUseSequentialDecoder();
}

/**
 * Symbols describing tensor shapes used in this file:
 * - B: batch size
 * - C: number of class/tokens
 * - H: hidden dimension
 * - U: target length
 * - T: length of the time frames in the encoded X
 */

std::vector<Variable> Seq2SeqCriterion::forward(
    const std::vector<Variable>& inputs) {
  if (inputs.size() < 2 || (inputs.size() > 4)) {
    throw std::invalid_argument(
        "Invalid inputs size; Seq2Seq criterion takes input, target, inputSizes [optional]");
  }
  const auto& input = inputs[0];
  const auto& target = inputs[1];
  const auto& inputSizes =
      inputs.size() == 2 ? af::array() : inputs[2].array(); // 1 x B
  const auto& targetSizes =
      inputs.size() == 3 ? af::array() : inputs[3].array(); // 1 x B

  Variable out, alpha;
  if (useSequentialDecoder_) {
    std::tie(out, alpha) = decoder(input, target, inputSizes, targetSizes);
  } else {
    std::tie(out, alpha) =
        vectorizedDecoder(input, target, inputSizes, targetSizes);
  }

  out = logSoftmax(out, 0); // C x U x B

  auto losses = moddims(
      sum(categoricalCrossEntropy(out, target, ReduceMode::NONE, pad_), {0}),
      -1);
  if (train_ && labelSmooth_ > 0) {
    size_t nClass = out.dims(0);
    auto targetTiled = af::tile(
        af::moddims(
            target.array(), af::dim4(1, target.dims(0), target.dims(1))),
        nClass);
    out = applySeq2SeqMask(out, targetTiled, pad_);
    auto smoothLoss = moddims(sum(out, {0, 1}), -1);
    losses = (1 - labelSmooth_) * losses - (labelSmooth_ / nClass) * smoothLoss;
  }

  return {losses, out};
}

std::pair<Variable, Variable> Seq2SeqCriterion::vectorizedDecoder(
    const Variable& input,
    const Variable& target,
    const af::array& inputSizes,
    const af::array& targetSizes) {
  int U = target.dims(0);
  int B = target.dims(1);
  int T = input.dims(1);

  auto hy = tile(startEmbedding(), {1, 1, B}); // H x 1 x B

  if (U > 1) {
    // Slice off eos
    auto y = target(af::seq(0, U - 2), af::span);
    if (train_) {
      if (samplingStrategy_ == fl::app::asr::kModelSampling) {
        throw std::logic_error(
            "vectorizedDecoder does not support model sampling");
      } else if (samplingStrategy_ == fl::app::asr::kRandSampling) {
        auto mask = Variable(
            (af::randu(y.dims()) * 100 <= pctTeacherForcing_).as(y.type()),
            false);
        auto samples =
            Variable((af::randu(y.dims()) * (nClass_ - 1)).as(y.type()), false);

        y = mask * y + (1 - mask) * samples;
      }
    }

    auto yEmbed = embedding()->forward(y);
    hy = concatenate({hy, yEmbed}, 1); // H x U x B
  }

  Variable alpha, summaries;
  for (int i = 0; i < nAttnRound_; i++) {
    hy = reorder(hy, 0, 2, 1); // H x U x B -> H x B x U
    hy = decodeRNN(i)->forward(hy);
    hy = reorder(hy, 0, 2, 1); // H x B x U ->  H x U x B

    Variable windowWeight;
    if (window_ && (!train_ || trainWithWindow_)) {
      windowWeight =
          window_->computeVectorizedWindow(U, T, B, inputSizes, targetSizes);
    }

    std::tie(alpha, summaries) = attention(i)->forward(
        hy,
        input,
        Variable(), // vectorizedDecoder does not support prev_attn input
        windowWeight,
        fl::noGrad(inputSizes));
    hy = hy + summaries;
  }

  auto out = linearOut()->forward(hy); // C x U x B
  return std::make_pair(out, alpha);
}

std::pair<Variable, Variable> Seq2SeqCriterion::decoder(
    const Variable& input,
    const Variable& target,
    const af::array& inputSizes,
    const af::array& targetSizes) {
  int U = target.dims(0);

  std::vector<Variable> outvec;
  std::vector<Variable> alphaVec;
  Seq2SeqState state(nAttnRound_);
  Variable y;
  for (int u = 0; u < U; u++) {
    Variable ox;
    std::tie(ox, state) =
        decodeStep(input, y, state, inputSizes, targetSizes, U);

    if (!train_) {
      y = target(u, af::span);
    } else if (samplingStrategy_ == fl::app::asr::kGumbelSampling) {
      double eps = 1e-7;
      auto gb = -log(-log((1 - 2 * eps) * af::randu(ox.dims()) + eps));
      ox = logSoftmax((ox + Variable(gb, false)) / gumbelTemperature_, 0);
      y = Variable(exp(ox).array(), false);
    } else if (af::allTrue<bool>(
                   af::randu(1) * 100 <= af::constant(pctTeacherForcing_, 1))) {
      y = target(u, af::span);
    } else if (samplingStrategy_ == fl::app::asr::kModelSampling) {
      af::array maxIdx, maxValues;
      max(maxValues, maxIdx, ox.array());
      y = Variable(maxIdx, false);
    } else if (samplingStrategy_ == fl::app::asr::kRandSampling) {
      y = Variable(
          (af::randu(af::dim4{1, target.dims(1)}) * (nClass_ - 1)).as(s32),
          false);
    } else {
      throw std::invalid_argument("Invalid sampling strategy");
    }

    outvec.push_back(ox);
    alphaVec.push_back(state.alpha);
  }

  auto out = concatenate(outvec, 1); // C x U x B
  auto alpha = concatenate(alphaVec, 0); // U x T x B

  return std::make_pair(out, alpha);
}

af::array Seq2SeqCriterion::viterbiPath(
    const af::array& input,
    const af::array& inputSizes /* = af::array() */) {
  return viterbiPathBase(input, inputSizes, false).first;
}

std::pair<af::array, Variable> Seq2SeqCriterion::viterbiPathBase(
    const af::array& input,
    const af::array& inputSizes,
    bool saveAttn) {
  // NB: xEncoded has to be with batchsize 1
  bool wasTrain = train_;
  eval();
  std::vector<int> maxPath;
  std::vector<Variable> alphaVec;
  Variable alpha;
  Seq2SeqState state(nAttnRound_);
  Variable y, ox;
  af::array maxIdx, maxValues;
  int pred;
  for (int u = 0; u < maxDecoderOutputLen_; u++) {
    std::tie(ox, state) = decodeStep(
        Variable(input, false),
        y,
        state,
        inputSizes,
        af::array(),
        input.dims(1));
    max(maxValues, maxIdx, ox.array());
    maxIdx.host(&pred);
    if (saveAttn) {
      alphaVec.push_back(state.alpha);
    }

    if (pred == eos_) {
      break;
    }
    y = constant(pred, 1, s32, false);
    maxPath.push_back(pred);
  }
  if (saveAttn) {
    alpha = concatenate(alphaVec, 0);
  }

  if (wasTrain) {
    train();
  }
  af::array vPath =
      maxPath.empty() ? af::array() : af::array(maxPath.size(), maxPath.data());
  return std::make_pair(vPath, alpha);
}

std::vector<int> Seq2SeqCriterion::beamPath(
    const af::array& input,
    const af::array& inputSizes,
    int beamSize /* = 10 */) {
  std::vector<Seq2SeqCriterion::CandidateHypo> beam;
  beam.emplace_back(CandidateHypo{});
  auto beamPaths =
      beamSearch(input, inputSizes, beam, beamSize, maxDecoderOutputLen_);
  return beamPaths[0].path;
}

// beam are candidates that need to be extended
std::vector<Seq2SeqCriterion::CandidateHypo> Seq2SeqCriterion::beamSearch(
    const af::array& input, // H x T x 1
    const af::array& inputSizes, // 1 x B
    std::vector<Seq2SeqCriterion::CandidateHypo> beam,
    int beamSize = 10,
    int maxLen = 200) {
  bool wasTrain = train_;
  eval();

  std::vector<Seq2SeqCriterion::CandidateHypo> complete;
  std::vector<Seq2SeqCriterion::CandidateHypo> newBeam;
  auto cmpfn = [](Seq2SeqCriterion::CandidateHypo& lhs,
                  Seq2SeqCriterion::CandidateHypo& rhs) {
    return lhs.score > rhs.score;
  };

  for (int l = 0; l < maxLen; l++) {
    newBeam.resize(0);

    std::vector<Variable> prevYVec;
    std::vector<Seq2SeqState> prevStateVec;
    std::vector<float> prevScoreVec;
    for (auto& hypo : beam) {
      Variable y;
      if (!hypo.path.empty()) {
        y = constant(hypo.path.back(), 1, s32, false);
      }
      prevYVec.push_back(y);
      prevStateVec.push_back(hypo.state);
      prevScoreVec.push_back(hypo.score);
    }
    auto prevY = concatenate(prevYVec, 1); // 1 x B
    auto prevState = detail::concatState(prevStateVec);

    Variable ox;
    Seq2SeqState state;
    // do proper cast of input size to batch size
    // because we have beam now for the input
    auto tiledInputSizes = af::tile(inputSizes, 1, prevY.dims(1));
    std::tie(ox, state) = decodeStep(
        Variable(input, false),
        prevY,
        prevState,
        tiledInputSizes,
        af::array(),
        input.dims(1));
    ox = logSoftmax(ox, 0); // C x 1 x B
    ox = fl::reorder(ox, 0, 2, 1);

    auto scoreArr =
        af::array(1, static_cast<int>(beam.size()), prevScoreVec.data());
    scoreArr = af::tile(scoreArr, ox.dims()[0]);

    scoreArr = scoreArr + ox.array(); // C x B
    scoreArr = af::flat(scoreArr); // column-first
    auto scoreVec = afToVector<float>(scoreArr);

    std::vector<size_t> indices(scoreVec.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::partial_sort(
        indices.begin(),
        indices.begin() +
            std::min(2 * beamSize, static_cast<int>(scoreVec.size())),
        indices.end(),
        [&scoreVec](size_t i1, size_t i2) {
          return scoreVec[i1] > scoreVec[i2];
        });

    int nClass = ox.dims()[0];
    for (int j = 0; j < indices.size(); j++) {
      int hypIdx = indices[j] / nClass;
      int clsIdx = indices[j] % nClass;
      std::vector<int> path_(beam[hypIdx].path);
      path_.push_back(clsIdx);
      if (j < beamSize && clsIdx == eos_) {
        path_.pop_back();
        complete.emplace_back(
            scoreVec[indices[j]], path_, detail::selectState(state, hypIdx));
      } else if (clsIdx != eos_) {
        newBeam.emplace_back(
            scoreVec[indices[j]], path_, detail::selectState(state, hypIdx));
      }
      if (newBeam.size() >= beamSize) {
        break;
      }
    }
    beam.resize(newBeam.size());
    beam = std::move(newBeam);

    if (complete.size() >= beamSize) {
      std::partial_sort(
          complete.begin(), complete.begin() + beamSize, complete.end(), cmpfn);
      complete.resize(beamSize);

      // if lowest score in complete is better than best future hypo
      // then its not possible for any future hypothesis to replace existing
      // hypothesises in complete.
      if (complete.back().score > beam[0].score) {
        break;
      }
    }
  }

  if (wasTrain) {
    train();
  }

  return complete.empty() ? beam : complete;
}

std::pair<Variable, Seq2SeqState> Seq2SeqCriterion::decodeStep(
    const Variable& xEncoded,
    const Variable& y,
    const Seq2SeqState& inState,
    const af::array& inputSizes,
    const af::array& targetSizes,
    const int maxDecoderSteps) const {
  Variable hy;
  if (y.isempty()) {
    hy = tile(startEmbedding(), {1, 1, static_cast<int>(xEncoded.dims(2))});
  } else if (train_ && samplingStrategy_ == fl::app::asr::kGumbelSampling) {
    hy = linear(y, embedding()->param(0));
  } else {
    hy = embedding()->forward(y);
  }

  if (inputFeeding_ && !y.isempty()) {
    hy = hy + moddims(inState.summary, hy.dims());
  }
  hy = moddims(hy, {hy.dims(0), -1}); // H x B

  Seq2SeqState outState(nAttnRound_);
  outState.step = inState.step + 1;

  Variable summaries;
  for (int i = 0; i < nAttnRound_; i++) {
    hy = moddims(hy, {hy.dims(0), -1}); // H x 1 x B -> H x B
    std::tie(hy, outState.hidden[i]) =
        decodeRNN(i)->forward(hy, inState.hidden[i]);
    hy = moddims(hy, {hy.dims(0), 1, hy.dims(1)}); // H x B -> H x 1 x B

    Variable windowWeight;
    // because of the beam search batchsize can be
    // different for xEncoded and y (xEncoded batch = 1 and y batch = beam
    // size)
    int batchsize = y.isempty() ? xEncoded.dims(2) : y.dims(1);
    if (window_ && (!train_ || trainWithWindow_)) {
      // TODO fix for softpretrain where target size is used
      // for now force to xEncoded.dims(1)
      windowWeight = window_->computeWindow(
          inState.alpha,
          inState.step,
          maxDecoderSteps,
          xEncoded.dims(1),
          batchsize,
          inputSizes,
          targetSizes);
    }
    std::tie(outState.alpha, summaries) = attention(i)->forward(
        hy, xEncoded, inState.alpha, windowWeight, fl::noGrad(inputSizes));
    hy = hy + summaries;
  }
  outState.summary = summaries;

  auto out = linearOut()->forward(hy); // C x 1 x B
  return std::make_pair(out, outState);
}

std::pair<std::vector<std::vector<float>>, std::vector<Seq2SeqStatePtr>>
Seq2SeqCriterion::decodeBatchStep(
    const fl::Variable& xEncoded,
    std::vector<fl::Variable>& ys,
    const std::vector<Seq2SeqState*>& inStates,
    const int attentionThreshold,
    const float smoothingTemperature) const {
  // NB: xEncoded has to be with batchsize 1
  int batchSize = ys.size();
  std::vector<Variable> statesVector(batchSize);

  // Batch Ys
  for (int i = 0; i < batchSize; i++) {
    if (ys[i].isempty()) {
      ys[i] = startEmbedding();
    } else {
      ys[i] = embedding()->forward(ys[i]);
      if (inputFeeding_) {
        ys[i] = ys[i] + moddims(inStates[i]->summary, ys[i].dims());
      }
    }
    ys[i] = moddims(ys[i], {ys[i].dims(0), -1});
  }
  Variable yBatched = concatenate(ys, 1); // H x B

  std::vector<Seq2SeqStatePtr> outstates(batchSize);
  for (int i = 0; i < batchSize; i++) {
    outstates[i] = std::make_shared<Seq2SeqState>(nAttnRound_);
    outstates[i]->step = inStates[i]->step + 1;
  }
  Variable outStateBatched;

  for (int n = 0; n < nAttnRound_; n++) {
    /* (1) RNN forward */
    if (inStates[0]->hidden[n].isempty()) {
      std::tie(yBatched, outStateBatched) =
          decodeRNN(n)->forward(yBatched, Variable());
    } else {
      for (int i = 0; i < batchSize; i++) {
        statesVector[i] = inStates[i]->hidden[n];
      }
      Variable inStateHiddenBatched = concatenate(statesVector, 1).linear();
      std::tie(yBatched, outStateBatched) =
          decodeRNN(n)->forward(yBatched, inStateHiddenBatched);
    }

    for (int i = 0; i < batchSize; i++) {
      outstates[i]->hidden[n] = outStateBatched.col(i);
    }

    /* (2) Attention forward */
    if (window_ && (!train_ || trainWithWindow_)) {
      throw std::runtime_error(
          "Batched decoding does not support models with window");
    }

    Variable summaries, alphaBatched;
    // NB:
    // - Third Variable is set to empty since no attention use it.
    // - Only ContentAttention is supported
    std::tie(alphaBatched, summaries) =
        attention(n)->forward(yBatched, xEncoded, Variable(), Variable());
    alphaBatched = reorder(alphaBatched, 1, 0); // B x T -> T x B
    yBatched = yBatched + summaries; // H x B

    af::array bestpath, maxvalues;
    af::max(maxvalues, bestpath, alphaBatched.array(), 0);
    std::vector<int> maxIdx = afToVector<int>(bestpath);
    for (int i = 0; i < batchSize; i++) {
      outstates[i]->peakAttnPos = maxIdx[i];
      // TODO: std::abs maybe unnecessary
      outstates[i]->isValid =
          std::abs(outstates[i]->peakAttnPos - inStates[i]->peakAttnPos) <=
          attentionThreshold;
      outstates[i]->alpha = alphaBatched.col(i);
      outstates[i]->summary = yBatched.col(i);
    }
  }

  /* (3) Linear forward */
  auto outBatched = linearOut()->forward(yBatched);
  outBatched = logSoftmax(outBatched / smoothingTemperature, 0);
  std::vector<std::vector<float>> out(batchSize);
  for (int i = 0; i < batchSize; i++) {
    out[i] = afToVector<float>(outBatched.col(i));
  }

  return std::make_pair(out, outstates);
}

void Seq2SeqCriterion::setUseSequentialDecoder() {
  useSequentialDecoder_ = false;
  if ((pctTeacherForcing_ < 100 &&
       samplingStrategy_ == fl::app::asr::kModelSampling) ||
      samplingStrategy_ == fl::app::asr::kGumbelSampling || inputFeeding_) {
    useSequentialDecoder_ = true;
  } else if (
      std::dynamic_pointer_cast<SimpleLocationAttention>(attention(0)) ||
      std::dynamic_pointer_cast<LocationAttention>(attention(0)) ||
      std::dynamic_pointer_cast<NeuralLocationAttention>(attention(0))) {
    useSequentialDecoder_ = true;
  } else if (
      window_ && trainWithWindow_ &&
      std::dynamic_pointer_cast<MedianWindow>(window_)) {
    useSequentialDecoder_ = true;
  }
}

std::string Seq2SeqCriterion::prettyString() const {
  return "Seq2SeqCriterion";
}

AMUpdateFunc buildSeq2SeqRnnAmUpdateFunction(
    std::shared_ptr<SequenceCriterion>& criterion,
    int attRound,
    int beamSize,
    float attThr,
    float smoothingTemp) {
  auto buf = std::make_shared<Seq2SeqDecoderBuffer>(
      attRound, beamSize, attThr, smoothingTemp);

  const Seq2SeqCriterion* s2sCriterion =
      static_cast<Seq2SeqCriterion*>(criterion.get());
  auto amUpdateFunc = [buf, s2sCriterion](
                          const float* emissions,
                          const int N,
                          const int T,
                          const std::vector<int>& rawY,
                          const std::vector<AMStatePtr>& rawPrevStates,
                          int& t) {
    if (t == 0) {
      buf->input = fl::Variable(af::array(N, T, emissions), false);
    }
    int batchSize = rawY.size();
    buf->prevStates.resize(0);
    buf->ys.resize(0);

    // Cast to seq2seq states
    for (int i = 0; i < batchSize; i++) {
      Seq2SeqState* prevState =
          static_cast<Seq2SeqState*>(rawPrevStates[i].get());
      fl::Variable y;
      if (t > 0) {
        y = fl::constant(rawY[i], 1, s32, false);
      } else {
        prevState = &buf->dummyState;
      }
      buf->ys.push_back(y);
      buf->prevStates.push_back(prevState);
    }

    // Run forward in batch
    std::vector<std::vector<float>> amScores;
    std::vector<Seq2SeqStatePtr> outStates;

    std::tie(amScores, outStates) = s2sCriterion->decodeBatchStep(
        buf->input,
        buf->ys,
        buf->prevStates,
        buf->attentionThreshold,
        buf->smoothingTemperature);

    // Cast back to void*
    std::vector<AMStatePtr> out;
    for (auto& os : outStates) {
      if (os->isValid) {
        out.push_back(os);
      } else {
        out.push_back(nullptr);
      }
    }
    return std::make_pair(amScores, out);
  };

  return amUpdateFunc;
}
} // namespace asr
} // namespace app
} // namespace fl
