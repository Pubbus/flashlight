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
#include "flashlight/app/asr/criterion/attention/attention.h"
#include "flashlight/app/asr/criterion/attention/window.h"

namespace fl {
namespace app {
namespace asr {

/*
 * Utility function to create an attention for s2s in encoder-decoder.
 * From gflags it uses FLAGS_attention, FLAGS_encoderdim, FLAGS_attnconvkernel,
 * FLAGS_attnconvchannel, FLAGS_attndim, FLAGS_encoderdim, FLAGS_numattnhead
 */
std::shared_ptr<AttentionBase> createAttention();

/*
 * Utility function to create an force attention (attention window)
 * for s2s in encoder-decoder.
 * From gflags it uses FLAGS_minsil, FLAGS_maxsil, FLAGS_minrate, FLAGS_maxrate,
 * FLAGS_leftWindowSize, FLAGS_rightWindowSize FLAGS_softwstd, FLAGS_softwrate,
 * FLAGS_softwoffset
 */
std::shared_ptr<WindowBase> createAttentionWindow();

} // namespace asr
} // namespace app
} // namespace fl
