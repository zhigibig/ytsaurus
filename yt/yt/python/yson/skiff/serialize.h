#pragma once

#include "public.h"

#include <Extensions.hxx> // pycxx
#include <Objects.hxx> // pycxx

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

Py::Object DumpSkiff(Py::Tuple& args, Py::Dict& kwargs);
Py::Object DumpSkiffStructured(Py::Tuple& args, Py::Dict& kwargs);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython
