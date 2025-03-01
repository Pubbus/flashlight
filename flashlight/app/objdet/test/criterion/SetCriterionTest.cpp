/*
 * Copyright (c) Facebook, Inc. and its affiliates.  * All rights reserved.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "flashlight/app/objdet/criterion/SetCriterion.h"
#include "flashlight/app/objdet/nn/Transformer.h"

#include <gtest/gtest.h>
#include <unordered_map>

using namespace fl;
using namespace fl::app::objdet;

std::unordered_map<std::string, float> getLossWeights() {
  const std::unordered_map<std::string, float> lossWeightsBase = {
      {"lossCe", 1.f}, {"lossGiou", 1.f}, {"lossBbox", 1.f}};

  std::unordered_map<std::string, float> lossWeights;
  for (int i = 0; i < 6; i++) {
    for (auto l : lossWeightsBase) {
      std::string key = l.first + "_" + std::to_string(i);
      lossWeights[key] = l.second;
    }
  }
  return lossWeights;
}

TEST(SetCriterion, PytorchRepro) {
  const int NUM_CLASSES = 80;
  const int NUM_TARGETS = 1;
  const int NUM_PREDS = 1;
  const int NUM_BATCHES = 1;
  std::vector<float> predBoxesVec = {2, 2, 3, 3};

  std::vector<float> targetBoxesVec = {2, 2, 3, 3};

  // std::vector<float> predLogitsVec((NUM_CLASSES + 1) * NUM_PREDS * NUM_PREDS,
  // 0.0);

  auto predLogitsArray =
      af::constant(1, {NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES});

  std::vector<float> targetClassVec = {1};
  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {fl::Variable(
      af::array(4, NUM_TARGETS, NUM_BATCHES, targetBoxesVec.data()), false)};

  std::vector<fl::Variable> targetClasses = {fl::Variable(
      af::array(NUM_TARGETS, NUM_BATCHES, targetClassVec.data()), false)};
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_EQ(loss["lossGiou_0"].scalar<float>(), 0.0);
}

TEST(SetCriterion, PytorchReproMultiplePreds) {
  const int NUM_CLASSES = 80;
  const int NUM_TARGETS = 1;
  const int NUM_PREDS = 2;
  const int NUM_BATCHES = 1;
  std::vector<float> predBoxesVec = {2, 2, 3, 3, 1, 1, 2, 2};

  std::vector<float> targetBoxesVec = {2, 2, 3, 3};

  // std::vector<float> predLogitsVec((NUM_CLASSES + 1) * NUM_PREDS * NUM_PREDS,
  // 0.0);

  auto predLogitsArray =
      af::constant(1, NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES);

  std::vector<float> targetClassVec = {1};
  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {fl::Variable(
      af::array(4, NUM_TARGETS, NUM_BATCHES, targetBoxesVec.data()), false)};

  std::vector<fl::Variable> targetClasses = {fl::Variable(
      af::array(1, NUM_TARGETS, NUM_BATCHES, targetClassVec.data()), false)};
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_EQ(loss["lossGiou_0"].scalar<float>(), 0.0);
}

TEST(SetCriterion, PytorchReproMultipleTargets) {
  const int NUM_CLASSES = 80;
  const int NUM_TARGETS = 2;
  const int NUM_PREDS = 2;
  const int NUM_BATCHES = 1;
  std::vector<float> predBoxesVec = {2, 2, 3, 3, 1, 1, 2, 2};

  std::vector<float> targetBoxesVec = {
      1,
      1,
      2,
      2,
      2,
      2,
      3,
      3,
  };

  // std::vector<float> predLogitsVec((NUM_CLASSES + 1) * NUM_PREDS * NUM_PREDS,
  // 0.0);

  auto predLogitsArray =
      af::constant(1, {NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES});

  std::vector<float> targetClassVec = {1};
  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {fl::Variable(
      af::array(4, NUM_TARGETS, NUM_BATCHES, targetBoxesVec.data()), false)};

  std::vector<fl::Variable> targetClasses = {fl::Variable(
      af::array(NUM_TARGETS, NUM_BATCHES, targetClassVec.data()), false)};
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_FLOAT_EQ(loss["lossGiou_0"].scalar<float>(), 0.0);
}

TEST(SetCriterion, PytorchReproNoPerfectMatch) {
  const int NUM_CLASSES = 80;
  const int NUM_TARGETS = 2;
  const int NUM_PREDS = 2;
  const int NUM_BATCHES = 1;
  std::vector<float> predBoxesVec = {2, 2, 3, 3, 1, 1, 2, 2};

  std::vector<float> targetBoxesVec = {
      0.9, 0.8, 1.9, 1.95, 1.9, 1.95, 2.9, 2.95};

  // std::vector<float> predLogitsVec((NUM_CLASSES + 1) * NUM_PREDS * NUM_PREDS,
  // 0.0);

  auto predLogitsArray =
      af::constant(1, NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES);

  std::vector<float> targetClassVec = {1, 1};

  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {fl::Variable(
      af::array(4, NUM_TARGETS, NUM_BATCHES, targetBoxesVec.data()), false)};

  std::vector<fl::Variable> targetClasses = {fl::Variable(
      af::array(NUM_TARGETS, NUM_BATCHES, targetClassVec.data()), false)};
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_FLOAT_EQ(loss["lossGiou_0"].scalar<float>(), 0.18111613);
  EXPECT_FLOAT_EQ(loss["lossBbox_0"].scalar<float>(), 0.3750);
}

TEST(SetCriterion, PytorchMismatch1) {
  const int NUM_CLASSES = 80;
  const int NUM_TARGETS = 1;
  const int NUM_PREDS = 1;
  const int NUM_BATCHES = 1;
  std::vector<float> predBoxesVec = {
      2,
      2,
      3,
      3,
  };

  std::vector<float> targetBoxesVec1 = {
      1,
      1,
      2,
      2,
  };

  // std::vector<float> predLogitsVec((NUM_CLASSES + 1) * NUM_PREDS * NUM_PREDS,
  // 0.0);

  std::vector<float> targetClassVec = {1, 1};

  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogitsArray =
      af::constant(1, NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES);
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {
      fl::Variable(
          af::array(4, NUM_TARGETS, NUM_PREDS, targetBoxesVec1.data()), false),
  };

  std::vector<fl::Variable> targetClasses = {
      fl::Variable(
          af::array(1, NUM_TARGETS, NUM_PREDS, 1, targetClassVec.data()),
          false),
  };
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_FLOAT_EQ(loss["lossGiou_0"].scalar<float>(), 0.91314667f);
  EXPECT_FLOAT_EQ(loss["lossBbox_0"].scalar<float>(), 4.f);
}

TEST(SetCriterion, PytorchMismatch2) {
  const int NUM_CLASSES = 80;
  const int NUM_TARGETS = 1;
  const int NUM_PREDS = 1;
  const int NUM_BATCHES = 1;
  std::vector<float> predBoxesVec = {
      1,
      1,
      2,
      2,
  };

  std::vector<float> targetBoxesVec1 = {
      2,
      2,
      3,
      3,
  };

  // std::vector<float> predLogitsVec((NUM_CLASSES + 1) * NUM_PREDS * NUM_PREDS,
  // 0.0);

  std::vector<float> targetClassVec = {1, 1};

  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogitsArray =
      af::constant(1, NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES);
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {
      fl::Variable(
          af::array(4, NUM_TARGETS, NUM_PREDS, targetBoxesVec1.data()), false),
  };

  std::vector<fl::Variable> targetClasses = {
      fl::Variable(
          af::array(1, NUM_TARGETS, NUM_PREDS, 1, targetClassVec.data()),
          false),
  };
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_FLOAT_EQ(loss["lossGiou_0"].scalar<float>(), 0.91314667f);
  EXPECT_FLOAT_EQ(loss["lossBbox_0"].scalar<float>(), 4.0f);
}

TEST(SetCriterion, PytorchReproBatching) {
  const int NUM_CLASSES = 80;
  const int NUM_TARGETS = 1;
  const int NUM_PREDS = 1;
  const int NUM_BATCHES = 2;
  std::vector<float> predBoxesVec = {2, 2, 3, 3, 1, 1, 2, 2};

  std::vector<float> targetBoxesVec1 = {
      1,
      1,
      2,
      2,
  };

  std::vector<float> targetBoxesVec2 = {
      2,
      2,
      3,
      3,
  };

  // std::vector<float> predLogitsVec((NUM_CLASSES + 1) * NUM_PREDS * NUM_PREDS,
  // 0.0);

  std::vector<float> targetClassVec = {1, 1};

  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogitsArray =
      af::constant(1, {NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES});
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {
      fl::Variable(
          af::array(4, NUM_TARGETS, NUM_PREDS, targetBoxesVec1.data()), false),
      fl::Variable(
          af::array(4, NUM_TARGETS, NUM_PREDS, targetBoxesVec2.data()), false)};

  std::vector<fl::Variable> targetClasses = {
      fl::Variable(
          af::array(NUM_TARGETS, NUM_PREDS, 1, targetClassVec.data()), false),
      fl::Variable(
          af::array(NUM_TARGETS, NUM_PREDS, 1, targetClassVec.data()), false)};
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_FLOAT_EQ(loss["lossGiou_0"].scalar<float>(), 0.91314667f);
  EXPECT_FLOAT_EQ(loss["lossBbox_0"].scalar<float>(), 4.f);
}

TEST(SetCriterion, DifferentNumberOfLabels) {
  const int NUM_CLASSES = 80;
  const int NUM_PREDS = 2;
  const int NUM_BATCHES = 2;
  std::vector<float> predBoxesVec = {
      2, 2, 3, 3, 1, 1, 2, 2, 2, 2, 3, 3, 1, 1, 2, 2};

  std::vector<float> targetBoxesVec1 = {
      1,
      1,
      2,
      2,
      2,
      2,
      3,
      3,
  };

  std::vector<float> targetBoxesVec2 = {
      2,
      2,
      3,
      3,
  };

  // std::vector<float> predLogitsVec((NUM_CLASSES + 1) * NUM_PREDS * NUM_PREDS,
  // 0.0);

  std::vector<float> targetClassVec = {1, 1};

  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogitsArray =
      af::constant(1, {NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES});
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {
      fl::Variable(af::array(4, 2, 1, targetBoxesVec1.data()), false),
      fl::Variable(af::array(4, 1, 1, targetBoxesVec2.data()), false)};

  std::vector<fl::Variable> targetClasses = {
      fl::Variable(af::constant(1, {2, 1, 1}), false),
      fl::Variable(af::constant(1, {1, 1, 1}), false)};
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_FLOAT_EQ(loss["lossGiou_0"].scalar<float>(), 0.f);
  EXPECT_FLOAT_EQ(loss["lossBbox_0"].scalar<float>(), 0.f);
}
// Test to make sure class labels are properly handles across batches
TEST(SetCriterion, DifferentNumberOfLabelsClass) {
  const int NUM_CLASSES = 80;
  const int NUM_PREDS = 3;
  const int NUM_BATCHES = 2;
  std::vector<float> predBoxesVec = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

  std::vector<float> targetBoxesVec1 = {1, 1, 1, 1, 1, 1, 1, 1};

  std::vector<float> targetBoxesVec2 = {
      1,
      1,
      1,
      1,
  };

  auto predBoxes = fl::Variable(
      af::array(4, NUM_PREDS, NUM_BATCHES, predBoxesVec.data()), true);
  auto predLogitsArray =
      af::constant(1, {NUM_CLASSES + 1, NUM_PREDS, NUM_BATCHES});
  predLogitsArray(1, 1, 0) = 10; // These should get matched
  predLogitsArray(2, 2, 0) = 10;
  predLogitsArray(9, 1, 1) = 10;
  auto predLogits = fl::Variable(predLogitsArray, true);

  std::vector<fl::Variable> targetBoxes = {
      fl::Variable(af::array(4, 2, 1, targetBoxesVec1.data()), false),
      fl::Variable(af::array(4, 1, 1, targetBoxesVec2.data()), false)};

  std::vector<fl::Variable> targetClasses = {
      fl::Variable(af::iota({2}), false),
      fl::Variable(af::constant(9, {1, 1, 1}), false)};
  auto matcher = HungarianMatcher(1, 1, 1);
  auto crit = SetCriterion(80, matcher, getLossWeights(), 0.0);
  auto loss = crit.forward(predBoxes, predLogits, targetBoxes, targetClasses);
  EXPECT_FLOAT_EQ(loss["lossGiou_0"].scalar<float>(), 0.f);
  EXPECT_FLOAT_EQ(loss["lossBbox_0"].scalar<float>(), 0.f);
  EXPECT_FLOAT_EQ(loss["lossCe_0"].scalar<float>(), 1.4713663f);
}
