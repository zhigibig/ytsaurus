/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef _THRIFT_SERVER_TSIMPLESERVER_H_
#define _THRIFT_SERVER_TSIMPLESERVER_H_ 1

#include <thrift/server/TServerFramework.h>

namespace apache {
namespace thrift {
namespace server {

/**
 * This is the most basic simple server. It is single-threaded and runs a
 * continuous loop of accepting a single connection, processing requests on
 * that connection until it closes, and then repeating.
 */
class TSimpleServer : public TServerFramework {
public:
  TSimpleServer(
      const stdcxx::shared_ptr<apache::thrift::TProcessorFactory>& processorFactory,
      const stdcxx::shared_ptr<apache::thrift::transport::TServerTransport>& serverTransport,
      const stdcxx::shared_ptr<apache::thrift::transport::TTransportFactory>& transportFactory,
      const stdcxx::shared_ptr<apache::thrift::protocol::TProtocolFactory>& protocolFactory);

  TSimpleServer(
      const stdcxx::shared_ptr<apache::thrift::TProcessor>& processor,
      const stdcxx::shared_ptr<apache::thrift::transport::TServerTransport>& serverTransport,
      const stdcxx::shared_ptr<apache::thrift::transport::TTransportFactory>& transportFactory,
      const stdcxx::shared_ptr<apache::thrift::protocol::TProtocolFactory>& protocolFactory);

  TSimpleServer(
      const stdcxx::shared_ptr<apache::thrift::TProcessorFactory>& processorFactory,
      const stdcxx::shared_ptr<apache::thrift::transport::TServerTransport>& serverTransport,
      const stdcxx::shared_ptr<apache::thrift::transport::TTransportFactory>& inputTransportFactory,
      const stdcxx::shared_ptr<apache::thrift::transport::TTransportFactory>& outputTransportFactory,
      const stdcxx::shared_ptr<apache::thrift::protocol::TProtocolFactory>& inputProtocolFactory,
      const stdcxx::shared_ptr<apache::thrift::protocol::TProtocolFactory>& outputProtocolFactory);

  TSimpleServer(
      const stdcxx::shared_ptr<apache::thrift::TProcessor>& processor,
      const stdcxx::shared_ptr<apache::thrift::transport::TServerTransport>& serverTransport,
      const stdcxx::shared_ptr<apache::thrift::transport::TTransportFactory>& inputTransportFactory,
      const stdcxx::shared_ptr<apache::thrift::transport::TTransportFactory>& outputTransportFactory,
      const stdcxx::shared_ptr<apache::thrift::protocol::TProtocolFactory>& inputProtocolFactory,
      const stdcxx::shared_ptr<apache::thrift::protocol::TProtocolFactory>& outputProtocolFactory);

  virtual ~TSimpleServer();

protected:
  virtual void onClientConnected(const stdcxx::shared_ptr<TConnectedClient>& pClient) /* override */;
  virtual void onClientDisconnected(TConnectedClient* pClient) /* override */;

private:
  void setConcurrentClientLimit(int64_t newLimit); // hide
};
}
}
} // apache::thrift::server

#endif // #ifndef _THRIFT_SERVER_TSIMPLESERVER_H_
