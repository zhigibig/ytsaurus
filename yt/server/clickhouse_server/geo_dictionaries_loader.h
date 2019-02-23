#pragma once

#include "public.h"

#include <Dictionaries/Embedded/IGeoDictionariesLoader.h>

#include <memory>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IGeoDictionariesLoader> CreateGeoDictionariesLoader(const std::string& geodataPath);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
