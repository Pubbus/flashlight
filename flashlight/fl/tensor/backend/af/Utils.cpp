/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "flashlight/fl/tensor/backend/af/Utils.h"

#include <stdexcept>
#include <unordered_map>

#include "flashlight/fl/tensor/Index.h"
#include "flashlight/fl/tensor/backend/af/ArrayFireTensor.h"

namespace fl {
namespace detail {

const std::unordered_map<fl::dtype, af::dtype> kFlashlightTypeToArrayFire = {
    {fl::dtype::f16, af::dtype::f16},
    {fl::dtype::f32, af::dtype::f32},
    {fl::dtype::f64, af::dtype::f64},
    {fl::dtype::b8, af::dtype::b8},
    {fl::dtype::s16, af::dtype::s16},
    {fl::dtype::s32, af::dtype::s32},
    {fl::dtype::s64, af::dtype::s64},
    {fl::dtype::u8, af::dtype::u8},
    {fl::dtype::u16, af::dtype::u16},
    {fl::dtype::u32, af::dtype::u32},
    {fl::dtype::u64, af::dtype::u64}};

const std::unordered_map<af::dtype, fl::dtype> kArrayFireTypeToFlashlight = {
    {af::dtype::f16, fl::dtype::f16},
    {af::dtype::f32, fl::dtype::f32},
    {af::dtype::f64, fl::dtype::f64},
    {af::dtype::b8, fl::dtype::b8},
    {af::dtype::s16, fl::dtype::s16},
    {af::dtype::s32, fl::dtype::s32},
    {af::dtype::s64, fl::dtype::s64},
    {af::dtype::u8, fl::dtype::u8},
    {af::dtype::u16, fl::dtype::u16},
    {af::dtype::u32, fl::dtype::u32},
    {af::dtype::u64, fl::dtype::u64}};

af::dtype flToAfType(fl::dtype type) {
  return kFlashlightTypeToArrayFire.at(type);
}

fl::dtype afToFlType(af::dtype type) {
  return kArrayFireTypeToFlashlight.at(type);
}

af_mat_prop flToAfMatrixProperty(MatrixProperty property) {
  switch (property) {
    case MatrixProperty::None:
      return AF_MAT_NONE;
    case MatrixProperty::Transpose:
      return AF_MAT_TRANS;
    default:
      throw std::invalid_argument(
          "flToAfMatrixProperty: invalid property specified");
  }
}

af::dim4 flToAfDims(const Shape& shape) {
  if (shape.ndim() > 4) {
    throw std::invalid_argument(
        "flToAfDims: ArrayFire shapes can't be more than 4 dimensions");
  }
  if (shape.elements() == 0) {
    return af::dim4(0);
  }
  af::dim4 out(1, 1, 1, 1);
  for (size_t i = 0; i < shape.ndim(); ++i) {
    out.dims[i] = shape.dim(i);
  }
  return out;
}

void afToFlDims(const af::dim4& d, Shape& s) {
  auto& storage = s.get();
  if (d.elements() == 0) {
    storage.resize(0);
    return;
  }
  if (d.elements() == 1) {
    storage.resize(1);
    s[0] = 1;
    return;
  }

  // Number of non-trailing-1 dims
  unsigned idx = AF_MAX_DIMS - 1;
  while (d[idx] == 1) {
    --idx;
  }

  storage.resize(idx + 1);
  for (unsigned i = 0; i <= idx; ++i) {
    s[i] = d[i];
  }
}

Shape afToFlDims(const af::dim4& d) {
  Shape s;
  afToFlDims(d, s);
  return s;
}

af::seq flRangeToAfSeq(const fl::range& range) {
  const int end = range.end();
  return af::seq(range.start(), end == fl::end ? -1 : end, range.stride());
}

af::index flToAfIndex(const fl::Index& idx) {
  switch (idx.type()) {
    case IndexType::Tensor:
      return af::index(toArray(idx.get<Tensor>()));
    case IndexType::Range:
      if (idx.isSpan()) {
        return af::index(af::span);
      } else {
        return af::index(flRangeToAfSeq(idx.get<range>()));
      }
    case IndexType::Literal:
      return af::index(idx.get<int>());
    default:
      throw std::invalid_argument(
          "flToAfIndex: fl::Index has unknown or invalid type.");
  }
}

af::dim4 condenseDims(const af::dim4& dims) {
  if (dims.elements() == 0) {
    return af::dim4(0);
  }

  // Find the condensed shape
  af::dim4 newDims(1, 1, 1, 1);
  unsigned newDimIdx = 0;
  for (unsigned i = 0; i < AF_MAX_DIMS; ++i) {
    if (dims[i] != 1) {
      // found a non-1 dim size - populate newDims
      newDims[newDimIdx] = dims[i];
      newDimIdx++;
    }
  }
  return newDims;
}

af::array condenseIndices(const af::array& arr) {
  // Fast path - Array has zero elements or a dim of size zero
  if (arr.elements() == 0) {
    return arr;
  }

  // Only change dims if condensing is possible
  af::dim4 newDims = condenseDims(arr.dims());
  if (newDims != arr.dims()) {
    return af::moddims(arr, newDims);
  } else {
    return arr;
  }
}

af_source flToAfLocation(Location location) {
  switch (location) {
    case Location::Host:
      return afHost;
    case Location::Device:
      return afDevice;
    default:
      throw std::invalid_argument(
          "flToAfLocation: no valid ArrayFire location exists "
          " for given Flashlight location.");
  }
}

af::array fromFlData(
    const Shape& shape,
    void* ptr,
    fl::dtype type,
    fl::Location memoryLocation) {
  af::dim4 dims = detail::flToAfDims(shape);
  af::dtype afType = detail::flToAfType(type);
  af_source loc = detail::flToAfLocation(memoryLocation);

  // No or null buffer
  if (!ptr) {
    return af::array(dims, afType);
  }

  using af::dtype;
  switch (afType) {
    case f32:
      return af::array(dims, reinterpret_cast<float*>(ptr), loc);
    case f64:
      return af::array(dims, reinterpret_cast<double*>(ptr), loc);
    case s32:
      return af::array(dims, reinterpret_cast<int*>(ptr), loc);
    case u32:
      return af::array(dims, reinterpret_cast<unsigned*>(ptr), loc);
    case s64:
      return af::array(dims, reinterpret_cast<long long*>(ptr), loc);
    case u64:
      return af::array(dims, reinterpret_cast<unsigned long long*>(ptr), loc);
    case s16:
      return af::array(dims, reinterpret_cast<short*>(ptr), loc);
    case u16:
      return af::array(dims, reinterpret_cast<unsigned short*>(ptr), loc);
    case b8:
      return af::array(dims, reinterpret_cast<char*>(ptr), loc);
    case u8:
      return af::array(dims, reinterpret_cast<unsigned char*>(ptr), loc);
    default:
      throw std::invalid_argument(
          "fromFlData: can't construct ArrayFire array from given type.");
  }
}

af_border_type flToAfPadType(PadType type) {
  switch (type) {
    case PadType::Constant:
      return AF_PAD_ZERO; // constant padding --> zero padding in AF
    case PadType::Edge:
      return AF_PAD_CLAMP_TO_EDGE;
    case PadType::Symmetric:
      return AF_PAD_SYM;
    default:
      throw std::invalid_argument(
          "flToAfPadType: Flashlight padding "
          "type not supported by ArrayFire");
  }
}

} // namespace detail
} // namespace fl
