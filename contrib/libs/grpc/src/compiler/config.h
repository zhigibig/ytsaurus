/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef SRC_COMPILER_CONFIG_H
#define SRC_COMPILER_CONFIG_H

#include <util/generic/string.h>
#include <util/string/cast.h>

#include "src/compiler/config_protobuf.h"

#ifdef GRPC_CUSTOM_STRING
#warning GRPC_CUSTOM_STRING is no longer supported. Please use TString.
#endif

namespace grpc {

// Using grpc::string and grpc::to_string is discouraged in favor of
// TString and ::ToString. This is only for legacy code using
// them explictly.
typedef TString string;

namespace protobuf {

namespace compiler {
typedef GRPC_CUSTOM_CODEGENERATOR CodeGenerator;
typedef GRPC_CUSTOM_GENERATORCONTEXT GeneratorContext;
static inline int PluginMain(int argc, char* argv[],
                             const CodeGenerator* generator) {
  return GRPC_CUSTOM_PLUGINMAIN(argc, argv, generator);
}
static inline void ParseGeneratorParameter(
    const string& parameter, std::vector<std::pair<string, string> >* options) {
  GRPC_CUSTOM_PARSEGENERATORPARAMETER(parameter, options);
}

}  // namespace compiler
namespace io {
typedef GRPC_CUSTOM_PRINTER Printer;
typedef GRPC_CUSTOM_CODEDOUTPUTSTREAM CodedOutputStream;
typedef GRPC_CUSTOM_STRINGOUTPUTSTREAM StringOutputStream;
}  // namespace io
}  // namespace protobuf
}  // namespace grpc

namespace grpc_cpp_generator {

static const char* const kCppGeneratorMessageHeaderExt = ".pb.h";
static const char* const kCppGeneratorServiceHeaderExt = ".grpc.pb.h";

}  // namespace grpc_cpp_generator

#endif  // SRC_COMPILER_CONFIG_H
