#pragma once

#include <mapreduce/yt/interface/node.h>

namespace google {
namespace protobuf {

class Message;
class Descriptor;

}
}

class IInputStream;

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TVector<const ::google::protobuf::Descriptor*> GetJobInputDescriptors();
TVector<const ::google::protobuf::Descriptor*> GetJobOutputDescriptors();

void ValidateProtoDescriptor(
    const ::google::protobuf::Message& row,
    size_t tableIndex,
    const TVector<const ::google::protobuf::Descriptor*>& descriptors,
    bool isRead);

void ParseFromStream(
    IInputStream* stream,
    ::google::protobuf::Message& row,
    ui32 size);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
