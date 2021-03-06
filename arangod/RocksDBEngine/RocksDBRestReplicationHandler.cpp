////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBRestReplicationHandler.h"
#include "Basics/VPackStringBufferAdapter.h"
#include "Basics/VelocyPackHelper.h"
#include "Logger/Logger.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBReplicationContext.h"
#include "RocksDBEngine/RocksDBReplicationManager.h"
#include "RocksDBEngine/RocksDBReplicationTailing.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/StandaloneContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;
using namespace arangodb::rocksutils;

RocksDBRestReplicationHandler::RocksDBRestReplicationHandler(
    GeneralRequest* request, GeneralResponse* response)
    : RestReplicationHandler(request, response),
      _manager(globalRocksEngine()->replicationManager()) {}

void RocksDBRestReplicationHandler::handleCommandBatch() {
  // extract the request type
  auto const type = _request->requestType();
  auto const& suffixes = _request->suffixes();
  size_t const len = suffixes.size();

  TRI_ASSERT(len >= 1);

  if (type == rest::RequestType::POST) {
    // create a new blocker
    std::shared_ptr<VPackBuilder> input = _request->toVelocyPackBuilderPtr();

    if (input == nullptr || !input->slice().isObject()) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "invalid JSON");
      return;
    }

    double ttl = VelocyPackHelper::getNumericValue<double>(input->slice(), "ttl",
                                                           RocksDBReplicationContext::DefaultTTL);
    // create transaction+snapshot
    RocksDBReplicationContext* ctx = _manager->createContext(ttl);
    RocksDBReplicationContextGuard guard(_manager, ctx);
    ctx->bind(_vocbase);

    VPackBuilder b;
    b.add(VPackValue(VPackValueType::Object));
    b.add("id", VPackValue(std::to_string(ctx->id())));  // id always string
    b.add("lastTick", VPackValue(std::to_string(ctx->lastTick())));
    b.close();

    // add client
    bool found;
    std::string const& value = _request->value("serverId", found);
    if (!found) {
      LOG_TOPIC(DEBUG, Logger::FIXME) << "no serverId parameter found in request to " << _request->fullUrl();
    }
     
    if (!found || (!value.empty() && value != "none")) {
      TRI_server_id_t serverId = 0;

      if (found) {
        serverId = static_cast<TRI_server_id_t>(StringUtils::uint64(value));
      } else {
        serverId = ctx->id();
      }

      _vocbase->updateReplicationClient(serverId, ctx->lastTick());
    }
    generateResult(rest::ResponseCode::OK, b.slice());
    return;
  }

  if (type == rest::RequestType::PUT && len >= 2) {
    // extend an existing blocker
    TRI_voc_tick_t id =
        static_cast<TRI_voc_tick_t>(StringUtils::uint64(suffixes[1]));

    auto input = _request->toVelocyPackBuilderPtr();

    if (input == nullptr || !input->slice().isObject()) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "invalid JSON");
      return;
    }

    // extract ttl
    double expires = VelocyPackHelper::getNumericValue<double>(input->slice(), "ttl", RocksDBReplicationContext::DefaultTTL);

    int res = TRI_ERROR_NO_ERROR;
    bool busy;
    RocksDBReplicationContext* ctx = _manager->find(id, busy, expires);
    RocksDBReplicationContextGuard guard(_manager, ctx);
    if (busy) {
      res = TRI_ERROR_CURSOR_BUSY;
      generateError(GeneralResponse::responseCode(res), res);
      return;
    } else if (ctx == nullptr) {
      res = TRI_ERROR_CURSOR_NOT_FOUND;
      generateError(GeneralResponse::responseCode(res), res);
      return;
    }

    // add client
    bool found;
    std::string const& value = _request->value("serverId", found);
    if (!found) {
      LOG_TOPIC(DEBUG, Logger::FIXME) << "no serverId parameter found in request to " << _request->fullUrl();
    }
     
    if (!found || (!value.empty() && value != "none")) {
      TRI_server_id_t serverId = 0;

      if (found) {
        serverId = static_cast<TRI_server_id_t>(StringUtils::uint64(value));
      } else {
        serverId = ctx->id();
      }

      _vocbase->updateReplicationClient(serverId, ctx->lastTick());
    }
    resetResponse(rest::ResponseCode::NO_CONTENT);
    return;
  }

  if (type == rest::RequestType::DELETE_REQ && len >= 2) {
    // delete an existing blocker
    TRI_voc_tick_t id =
        static_cast<TRI_voc_tick_t>(StringUtils::uint64(suffixes[1]));

    bool found = _manager->remove(id);
    if (found) {
      resetResponse(rest::ResponseCode::NO_CONTENT);
    } else {
      int res = TRI_ERROR_CURSOR_NOT_FOUND;
      generateError(GeneralResponse::responseCode(res), res);
    }
    return;
  }

  // we get here if anything above is invalid
  generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
}
  
void RocksDBRestReplicationHandler::handleCommandBarrier() {
  auto const type = _request->requestType();
  if (type == rest::RequestType::POST) {
    VPackBuilder b;
    b.add(VPackValue(VPackValueType::Object));
    // always return a non-0 barrier id
    // it will be ignored by the client anyway for the RocksDB engine
    std::string const idString = std::to_string(TRI_NewTickServer());
    b.add("id", VPackValue(idString));
    b.close();
    generateResult(rest::ResponseCode::OK, b.slice());
  } else if (type == rest::RequestType::PUT ||
             type == rest::RequestType::DELETE_REQ) {
    resetResponse(rest::ResponseCode::NO_CONTENT);
  } else if (type == rest::RequestType::GET) {
    generateResult(rest::ResponseCode::OK, VPackSlice::emptyArraySlice());
  }
}

void RocksDBRestReplicationHandler::handleCommandLoggerFollow() {
  bool useVst = false;
  if (_request->transportType() == Endpoint::TransportType::VST) {
    useVst = true;
  }

  // determine start and end tick
  TRI_voc_tick_t tickStart = 0;
  TRI_voc_tick_t tickEnd = UINT64_MAX;

  bool found;
  std::string const& value1 = _request->value("from", found);

  if (found) {
    tickStart = static_cast<TRI_voc_tick_t>(StringUtils::uint64(value1));
  }

  // determine end tick for dump
  std::string const& value2 = _request->value("to", found);

  if (found) {
    tickEnd = static_cast<TRI_voc_tick_t>(StringUtils::uint64(value2));
  }

  if (found && (tickStart > tickEnd || tickEnd == 0)) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid from/to values");
    return;
  }

  bool includeSystem = true;
  std::string const& value4 = _request->value("includeSystem", found);

  if (found) {
    includeSystem = StringUtils::boolean(value4);
  }

  size_t chunkSize = 1024 * 1024;  // TODO: determine good default value?
  std::string const& value5 = _request->value("chunkSize", found);
  if (found) {
    chunkSize = static_cast<size_t>(StringUtils::uint64(value5));
  }

  // extract collection
  TRI_voc_cid_t cid = 0;
  std::string const& value6 = _request->value("collection", found);
  if (found) {
    arangodb::LogicalCollection* c = _vocbase->lookupCollection(value6);

    if (c == nullptr) {
      generateError(rest::ResponseCode::NOT_FOUND,
                    TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND);
      return;
    }

    cid = c->cid();
  }

  auto trxContext = transaction::StandaloneContext::Create(_vocbase);
  VPackBuilder builder(trxContext->getVPackOptions());
  builder.openArray();
  auto result = tailWal(_vocbase, tickStart, tickEnd, chunkSize, includeSystem,
                        cid, builder);
  builder.close();
  auto data = builder.slice();

  uint64_t const latest = latestSequenceNumber();

  if (result.fail()) {
    generateError(GeneralResponse::responseCode(result.errorNumber()),
                  result.errorNumber(), result.errorMessage());
    return;
  }

  bool const checkMore = (result.maxTick() > 0 && result.maxTick() < latest);

  // generate the result
  size_t length = data.length();
  TRI_ASSERT(length == 0 || result.maxTick() > 0);

  if (length == 0) {
    resetResponse(rest::ResponseCode::NO_CONTENT);
  } else {
    resetResponse(rest::ResponseCode::OK);
  }

  // transfer ownership of the buffer contents
  _response->setContentType(rest::ContentType::DUMP);

  // set headers
  _response->setHeaderNC(TRI_REPLICATION_HEADER_CHECKMORE,
                         checkMore ? "true" : "false");
  _response->setHeaderNC(
      TRI_REPLICATION_HEADER_LASTINCLUDED,
      StringUtils::itoa((length == 0) ? 0 : result.maxTick()));
  _response->setHeaderNC(TRI_REPLICATION_HEADER_LASTTICK, StringUtils::itoa(latest));
  _response->setHeaderNC(TRI_REPLICATION_HEADER_ACTIVE, "true");
  _response->setHeaderNC(TRI_REPLICATION_HEADER_FROMPRESENT,
                         result.minTickIncluded() ? "true" : "false");

  if (length > 0) {
    if (useVst) {
      for (auto message : arangodb::velocypack::ArrayIterator(data)) {
        _response->addPayload(VPackSlice(message),
                              trxContext->getVPackOptions(), true);
      }
    } else {
      HttpResponse* httpResponse = dynamic_cast<HttpResponse*>(_response.get());

      if (httpResponse == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "invalid response type");
      }

      basics::StringBuffer& buffer = httpResponse->body();
      arangodb::basics::VPackStringBufferAdapter adapter(buffer.stringBuffer());
      // note: we need the CustomTypeHandler here
      VPackDumper dumper(&adapter, trxContext->getVPackOptions());
      for (auto marker : arangodb::velocypack::ArrayIterator(data)) {
        dumper.dump(marker);
        httpResponse->body().appendChar('\n');
        //LOG_TOPIC(INFO, Logger::FIXME) << marker.toJson(trxContext->getVPackOptions());
      }
    }
    // add client
    bool found;
    std::string const& value = _request->value("serverId", found);

    if (!found || (!value.empty() && value != "none")) {
      TRI_server_id_t serverId = static_cast<TRI_server_id_t>(StringUtils::uint64(value));
      _vocbase->updateReplicationClient(serverId, result.maxTick());
    }
  }
}

/// @brief run the command that determines which transactions were open at
/// a given tick value
/// this is an internal method use by ArangoDB's replication that should not
/// be called by client drivers directly
void RocksDBRestReplicationHandler::handleCommandDetermineOpenTransactions() {
  generateResult(rest::ResponseCode::OK, VPackSlice::emptyArraySlice());
  // rocksdb only includes finished transactions in the WAL.
  _response->setContentType(rest::ContentType::DUMP);
  _response->setHeaderNC(TRI_REPLICATION_HEADER_LASTTICK, "0");
  // always true to satisfy continuous syncer
  _response->setHeaderNC(TRI_REPLICATION_HEADER_FROMPRESENT, "true");
}

void RocksDBRestReplicationHandler::handleCommandInventory() {
  RocksDBReplicationContext* ctx = nullptr;
  bool found, busy;
  std::string batchId = _request->value("batchId", found);
  if (found) {
    ctx = _manager->find(StringUtils::uint64(batchId), busy);
  }
  RocksDBReplicationContextGuard guard(_manager, ctx);
  if (!found) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batchId not specified");
    return;
  }
  if (busy || ctx == nullptr) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "context is busy or nullptr");
    return;
  }

  TRI_voc_tick_t tick = TRI_CurrentTickServer();

  // include system collections?
  bool includeSystem = true;
  {
    std::string const& value = _request->value("includeSystem", found);
    if (found) {
      includeSystem = StringUtils::boolean(value);
    }
  }

  // produce inventory for all databases?
  bool isGlobal = false;
  getApplier(isGlobal);
  
  std::pair<RocksDBReplicationResult, std::shared_ptr<VPackBuilder>> result =
      ctx->getInventory(this->_vocbase, includeSystem, isGlobal);
  if (!result.first.ok()) {
    generateError(rest::ResponseCode::BAD, result.first.errorNumber(),
                  "inventory could not be created");
    return;
  }

  VPackBuilder builder;
  builder.openObject();

  VPackSlice const inventory = result.second->slice();
  if (isGlobal) {
    TRI_ASSERT(inventory.isObject());
    builder.add("databases", inventory);
  } else {
    // add collections data
    TRI_ASSERT(inventory.isArray());
    builder.add("collections", inventory);
  }

  // "state"
  builder.add("state", VPackValue(VPackValueType::Object));

  builder.add("running", VPackValue(true));
  builder.add("lastLogTick", VPackValue(std::to_string(ctx->lastTick())));
  builder.add(
      "lastUncommittedLogTick",
      VPackValue(std::to_string(ctx->lastTick())));  // s.lastAssignedTick
  builder.add("totalEvents",
              VPackValue(ctx->lastTick()));  // s.numEvents + s.numEventsSync
  builder.add("time", VPackValue(utilities::timeString()));
  builder.close();  // state

  std::string const tickString(std::to_string(tick));
  builder.add("tick", VPackValue(tickString));
  builder.close();  // Toplevel

  generateResult(rest::ResponseCode::OK, builder.slice());
}

/// @brief produce list of keys for a specific collection
void RocksDBRestReplicationHandler::handleCommandCreateKeys() {
  std::string const& collection = _request->value("collection");
  if (collection.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid collection parameter");
    return;
  }
  // to is ignored because the snapshot time is the latest point in time
  
  RocksDBReplicationContext* ctx = nullptr;
  //get batchId from url parameters
  bool found, busy;
  std::string batchId = _request->value("batchId", found);

  // find context
  if (found) {
    ctx = _manager->find(StringUtils::uint64(batchId), busy);
  }
  RocksDBReplicationContextGuard guard(_manager, ctx);
  if (!found || busy || ctx == nullptr) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batchId not specified");
    return;
  }
 
  // TRI_voc_tick_t tickEnd = UINT64_MAX;
  // determine end tick for keys
  // std::string const& value = _request->value("to", found);
  // if (found) {
  //  tickEnd = static_cast<TRI_voc_tick_t>(StringUtils::uint64(value));
  //}

  // bind collection to context - will initialize iterator
  int res = ctx->bindCollection(_vocbase, collection);
  if (res != TRI_ERROR_NO_ERROR) {
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND);
    return;
  }

  VPackBuilder result;
  result.add(VPackValue(VPackValueType::Object));
  result.add("id", VPackValue(StringUtils::itoa(ctx->id())));
  result.add("count", VPackValue(ctx->count()));
  result.close();
  generateResult(rest::ResponseCode::OK, result.slice());
}

/// @brief returns all key ranges
void RocksDBRestReplicationHandler::handleCommandGetKeys() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting GET /_api/replication/keys/<keys-id>");
    return;
  }

  static uint64_t const DefaultChunkSize = 5000;
  uint64_t chunkSize = DefaultChunkSize;

  // determine chunk size
  bool found;
  std::string const& value = _request->value("chunkSize", found);

  if (found) {
    chunkSize = StringUtils::uint64(value);
    if (chunkSize < 100) {
      chunkSize = DefaultChunkSize;
    } else if (chunkSize > 20000) {
      chunkSize = 20000;
    }
  }

  //first suffix needs to be the batch id
  std::string const& id = suffixes[1];
  uint64_t batchId = arangodb::basics::StringUtils::uint64(id);

  // get context
  bool busy;
  RocksDBReplicationContext* ctx = _manager->find(batchId, busy);
  //lock context
  RocksDBReplicationContextGuard guard(_manager, ctx);

  if (ctx == nullptr) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batchId not specified, expired or invalid in another way");
    return;
  }
  if (busy) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "replication context is busy");
    return;
  }

  VPackBuffer<uint8_t> buffer;
  VPackBuilder builder(buffer);
  ctx->dumpKeyChunks(builder, chunkSize);
  generateResult(rest::ResponseCode::OK, std::move(buffer));
}

/// @brief returns date for a key range
void RocksDBRestReplicationHandler::handleCommandFetchKeys() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting PUT /_api/replication/keys/<keys-id>");
    return;
  }

  static uint64_t const DefaultChunkSize = 5000;
  uint64_t chunkSize = DefaultChunkSize;

  // determine chunk size
  bool found;
  std::string const& value1 = _request->value("chunkSize", found);

  if (found) {
    chunkSize = StringUtils::uint64(value1);
    if (chunkSize < 100) {
      chunkSize = DefaultChunkSize;
    } else if (chunkSize > 20000) {
      chunkSize = 20000;
    }
  }

  // chunk is supplied by old clients, low is an optimization
  // for rocksdb, because seeking should be cheaper
  std::string const& value2 = _request->value("chunk", found);
  size_t chunk = 0;
  if (found) {
    chunk = static_cast<size_t>(StringUtils::uint64(value2));
  }
  std::string const& lowKey = _request->value("low", found);

  std::string const& value3 = _request->value("type", found);

  bool keys = true;
  if (value3 == "keys") {
    keys = true;
  } else if (value3 == "docs") {
    keys = false;
  } else {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid 'type' value");
    return;
  }
  
  size_t offsetInChunk = 0;
  size_t maxChunkSize = SIZE_MAX;
  std::string const& value4 = _request->value("offset", found);
  if (found) {
    offsetInChunk = static_cast<size_t>(StringUtils::uint64(value4));
    // "offset" was introduced with ArangoDB 3.3. if the client sends it,
    // it means we can adapt the result size dynamically and the client
    // may refetch data for the same chunk
    maxChunkSize = 8 * 1024 * 1024; 
    // if a client does not send an "offset" parameter at all, we are
    // not sure if it supports this protocol (3.2 and before) or not
  } 

  std::string const& id = suffixes[1];

  uint64_t batchId = arangodb::basics::StringUtils::uint64(id);
  bool busy;
  RocksDBReplicationContext* ctx = _manager->find(batchId, busy);
  RocksDBReplicationContextGuard guard(_manager, ctx);
  if (ctx == nullptr) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batchId not specified or not found");
    return;
  }

  if (busy) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_CURSOR_NOT_FOUND,
                  "batch is busy");
    return;
  }

  std::shared_ptr<transaction::Context> transactionContext =
      transaction::StandaloneContext::Create(_vocbase);

  VPackBuffer<uint8_t> buffer;
  VPackBuilder builder(buffer, transactionContext->getVPackOptions());
  
  if (keys) {
    Result rv = ctx->dumpKeys(builder, chunk, static_cast<size_t>(chunkSize), lowKey);
    if (rv.fail()) {
      generateError(rv);
      return;
    }
  } else {
    bool success;
    std::shared_ptr<VPackBuilder> parsedIds = parseVelocyPackBody(success);
    if (!success) {
      generateResult(rest::ResponseCode::BAD, VPackSlice());
      return;
    }
    
    Result rv = ctx->dumpDocuments(builder, chunk, static_cast<size_t>(chunkSize), offsetInChunk, maxChunkSize, lowKey, parsedIds->slice());
    if (rv.fail()) {
      generateError(rv);
      return;
    }
  }

  generateResult(rest::ResponseCode::OK, std::move(buffer),
                 transactionContext);
}

void RocksDBRestReplicationHandler::handleCommandRemoveKeys() {
  std::vector<std::string> const& suffixes = _request->suffixes();

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting DELETE /_api/replication/keys/<keys-id>");
    return;
  }

  std::string const& id = suffixes[1];
  VPackBuilder resultBuilder;
  resultBuilder.openObject();
  resultBuilder.add("id", VPackValue(id));  // id as a string
  resultBuilder.add("error", VPackValue(false));
  resultBuilder.add("code",
                    VPackValue(static_cast<int>(rest::ResponseCode::ACCEPTED)));
  resultBuilder.close();

  generateResult(rest::ResponseCode::ACCEPTED, resultBuilder.slice());
}

void RocksDBRestReplicationHandler::handleCommandDump() {
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "enter handleCommandDump";

  bool found = false;
  uint64_t contextId = 0;

  // contains dump options that might need to be inspected
  // VPackSlice options = _request->payload();

  // get collection Name
  std::string const& collection = _request->value("collection");
  if (collection.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid collection parameter");
    return;
  }

  // get contextId
  std::string const& contextIdString = _request->value("batchId", found);
  if (found) {
    contextId = StringUtils::uint64(contextIdString);
  } else {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "replication dump - request misses batchId");
    return;
  }

  // acquire context
  bool isBusy = false;
  RocksDBReplicationContext* context = _manager->find(contextId, isBusy);
  RocksDBReplicationContextGuard guard(_manager, context);
  
  if (context == nullptr) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "replication dump - unable to find context (it could be expired)");
    return;
  }

  if (isBusy) {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "replication dump - context is busy");
    return;
  }

  // print request
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME)
      << "requested collection dump for collection '" << collection
      << "' using contextId '" << context->id() << "'";


  // TODO needs to generalized || velocypacks needs to support multiple slices
  // per response!
  auto response = dynamic_cast<HttpResponse*>(_response.get());
  StringBuffer dump(8192, false);

  if (response == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "invalid response type");
  }

  // do the work!
  auto result = context->dump(_vocbase, collection, dump, determineChunkSize());

  // generate the result
  if (dump.length() == 0) {
    resetResponse(rest::ResponseCode::NO_CONTENT);
  } else {
    resetResponse(rest::ResponseCode::OK);
  }

  response->setContentType(rest::ContentType::DUMP);
  // set headers
  _response->setHeaderNC(TRI_REPLICATION_HEADER_CHECKMORE,
                         (context->more() ? "true" : "false"));

  _response->setHeaderNC(
      TRI_REPLICATION_HEADER_LASTINCLUDED,
      StringUtils::itoa((dump.length() == 0) ? 0 : result.maxTick()));

  // transfer ownership of the buffer contents
  response->body().set(dump.stringBuffer());

  // avoid double freeing
  TRI_StealStringBuffer(dump.stringBuffer());
}
