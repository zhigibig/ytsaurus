#pragma once

#include "stream.h"

namespace NYT::NCompression::NDetail {

////////////////////////////////////////////////////////////////////////////////

void LzmaCompress(int level, TSource* source, TBlob* output);
void LzmaDecompress(TSource* source, TBlob* output);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCompression::NDetail


