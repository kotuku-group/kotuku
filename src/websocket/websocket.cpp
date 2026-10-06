/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
WebSocket: Provides RFC 6455 WebSocket client and server connections.

The WebSocket module implements the WebSocket protocol (RFC 6455) over HTTP/1.1.  Client connections are opened
through the HTTP module's connection handover facility and server connections are accepted through the Network
module's NetServer class.

The @WebSocket class represents a single connection in either role, and the @WebSocketServer class accepts
connections and represents each of them as a @WebSocket.

-END-

*********************************************************************************************************************/

#include <kotuku/main.h>
#include <kotuku/modules/network.h>
#include <kotuku/modules/http.h>
#include <kotuku/modules/crypto.h>
#include <kotuku/modules/websocket.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/script.h>
#include <kotuku/strings.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ws_frame.h"
#include "ws_handshake.h"
#include "ws_send_queue.h"
#include "ws_transport.h"

JUMPTABLE_CORE
JUMPTABLE_NETWORK
JUMPTABLE_CRYPTO

static OBJECTPTR modNetwork = nullptr;
static OBJECTPTR modHTTP = nullptr;
static OBJECTPTR modCrypto = nullptr;
static OBJECTPTR clWebSocket = nullptr;
static OBJECTPTR clWebSocketServer = nullptr;
static MSGID glDeferredMsgID = MSGID::NIL;
static MsgHandler *glDeferredHandle = nullptr;

static ERR add_websocket_class(void);
static ERR add_websocket_server_class(void);
static ERR deferred_msg_handler(APTR, int, MSGID, std::span<std::byte>);

#include "class_websocket.cpp"
#include "class_websocket_server.cpp"

//********************************************************************************************************************

#ifdef UNIT_TESTS
extern void protocol_unit_tests(int &, int &);
#endif

static void MODTest(std::string_view Options, int *Passed, int *Total)
{
#ifdef UNIT_TESTS
   kt::Log log("WebSocketTests");
   log.branch("Running protocol unit tests...");
   protocol_unit_tests(*Passed, *Total);
#endif
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("network", &modNetwork, &NetworkBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("http", &modHTTP) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("crypto", &modCrypto, &CryptoBase) != ERR::Okay) return ERR::InitModule;

   glDeferredMsgID = MSGID(AllocateID(IDTYPE::MESSAGE));
   auto handler = C_FUNCTION(deferred_msg_handler);
   if (AddMsgHandler(glDeferredMsgID, &handler, &glDeferredHandle) != ERR::Okay) return ERR::InitModule;

   if (auto error = add_websocket_class(); error != ERR::Okay) return error;
   return add_websocket_server_class();
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   if (clWebSocketServer) { FreeResource(clWebSocketServer); clWebSocketServer = nullptr; }
   if (clWebSocket)      { FreeResource(clWebSocket);      clWebSocket      = nullptr; }
   if (glDeferredHandle) { FreeResource(glDeferredHandle); glDeferredHandle = nullptr; }
   if (modCrypto)  { FreeResource(modCrypto);  modCrypto  = nullptr; }
   if (modHTTP)    { FreeResource(modHTTP);    modHTTP    = nullptr; }
   if (modNetwork) { FreeResource(modNetwork); modNetwork = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, MODTest, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_websocket_module() { return &ModHeader; }
