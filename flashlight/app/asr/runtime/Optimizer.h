/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "flashlight/fl/flashlight.h"

#include "flashlight/app/asr/common/Defines.h"
#include "flashlight/app/asr/common/Flags.h"

namespace fl {
namespace app {
namespace asr {

/*
 * Utility function to create an optimizer.
 * Supported optimizers: `sgd`, `adam`, `rmsprop`, `adadelta`, `adagrad`,
 * `amsgrad`, `novograd`. From gflags it uses FLAGS_optimrho, FLAGS_adambeta1,
 * FLAGS_adambeta2, FLAGS_optimepsilon,
 */
std::shared_ptr<fl::FirstOrderOptimizer> initOptimizer(
    const std::vector<std::shared_ptr<fl::Module>>& nets,
    const std::string& optimizer,
    double lr,
    double momentum,
    double weightdecay);
} // namespace asr
} // namespace app
} // namespace fl
