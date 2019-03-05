#include "codec.h"

namespace NYT::NErasure {

////////////////////////////////////////////////////////////////////////////////

ICodec* GetCodec(ECodec id) {
    return ::NErasure::GetCodec<TCodecTraits>(id);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NErasure
