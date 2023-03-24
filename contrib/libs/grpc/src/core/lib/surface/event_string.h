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

#ifndef GRPC_CORE_LIB_SURFACE_EVENT_STRING_H
#define GRPC_CORE_LIB_SURFACE_EVENT_STRING_H

#include <grpc/support/port_platform.h>

#include <util/generic/string.h>
#include <util/string/cast.h>

#include <grpc/grpc.h>

/* Returns a string describing an event. Must be later freed with gpr_free() */
TString grpc_event_string(grpc_event* ev);

#endif /* GRPC_CORE_LIB_SURFACE_EVENT_STRING_H */
