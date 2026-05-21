/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-MODULE-
Backstage: Provides a REST service for interacting with running processes.

Backstage provides an embedded REST service that allows a Kōtuku program to be interrogated and modified while it is
running.  Backstage does not export its own API functions, but is instead enabled by the user by specifying
`--backstage [port]` on the commandline.  If the parameter is omitted then Backstage will not be loaded.  The default
port for accessing Backstage is 8765.

The REST API and documentation on how to use Backstage is documented in the Kōtuku Wiki.

-END-

See interface.tiri for the REST interface.

*********************************************************************************************************************/

#define PRV_BACKSTAGE

#include <kotuku/main.h>
#include <kotuku/modules/backstage.h>
#include <kotuku/modules/network.h>
#include <kotuku/modules/regex.h>
#include <kotuku/strings.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../link/base64.h"

using namespace kt;

static OBJECTPTR modNetwork = nullptr;
static OBJECTPTR modRegex = nullptr;

JUMPTABLE_CORE
JUMPTABLE_NETWORK
JUMPTABLE_REGEX

static ERR init_backstage(int = 8765);
static void release_backstage_logs();
static void release_backstage_routes();
static void release_backstage_websockets();

objNetServer *glServer = nullptr;
static std::mutex glRequestLock;
static std::unordered_map<OBJECTID, std::string> glRequestBuffers;

static constexpr size_t MAX_REQUEST_HEADER = 16 * 1024;
static constexpr size_t MAX_REQUEST_BODY = 1024 * 1024;

//********************************************************************************************************************
// For declaring HTTP routes in the glRoutes array.

struct BackstageRequest;
struct BackstageResponse;
using BackstageHandler = ERR (*)(const BackstageRequest &, BackstageResponse &);

struct BackstageParam {
   std::string_view name;
   std::string_view type;
   std::string_view description;
   std::string_view default_value;
   bool required = false;

   constexpr BackstageParam() {}

   constexpr BackstageParam(std::string_view Name, std::string_view Type, std::string_view Description,
      std::string_view DefaultValue = {}, bool Required = false) :
      name(Name), type(Type), description(Description), default_value(DefaultValue), required(Required) {}
};

struct BackstageRouteMetadata {
   std::string_view description;
   std::string_view body;
   std::string_view returns;
   std::string_view transport;
   std::span<const BackstageParam> path_params;
   std::span<const BackstageParam> query_params;

   constexpr BackstageRouteMetadata() = default;

   constexpr BackstageRouteMetadata(std::string_view Description, std::string_view Body, std::string_view Returns,
      std::string_view Transport = {}, std::span<const BackstageParam> PathParams = {},
      std::span<const BackstageParam> QueryParams = {}) :
      description(Description), body(Body), returns(Returns), transport(Transport), path_params(PathParams),
      query_params(QueryParams) {}
};

struct BackstageRoute {
   std::string_view method;  // HTTP method
   std::string_view path;    // Human readable path (decorative)
   std::string_view pattern; // Regex pattern for matching requests
   BackstageHandler handler = nullptr; // Function handling this route
   std::span<const std::string_view> path_param_names;
   BackstageRouteMetadata metadata;
   Regex *regex = nullptr;

   BackstageRoute() = default;
   BackstageRoute(const BackstageRoute &) = delete;
   BackstageRoute &operator=(const BackstageRoute &) = delete;

   BackstageRoute(std::string_view Method, std::string_view Path, std::string_view Regex, BackstageHandler Handler,
      std::span<const std::string_view> PathParamNames = {}, const BackstageRouteMetadata &Metadata = {}) :
      method(Method), path(Path), pattern(Regex), handler(Handler), path_param_names(PathParamNames),
      metadata(Metadata) {}

   ~BackstageRoute() {
      if (regex) FreeResource(regex);
   }
};

//********************************************************************************************************************

struct BackstageRequest {
   BackstageRoute   *route;
   objNetServer     *server;  // NB: Lock the server when you need to interact with it
   objClientSocket  *client;
   std::string_view method;
   std::string_view path;
   std::string_view rawPath;
   std::string_view queryString;
   std::string_view version;
   std::span<const uint8_t> body;
   std::unordered_map<std::string, std::string> pathParams;
   std::unordered_map<std::string, std::string> queryParams;
};

struct BackstageResponse {
   int status = 200;
   std::string contentType = "application/json";
   std::string body;
};

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   kt::Log log("Backstage");

   CoreBase = argCoreBase;

   // Parse commandline arguments to confirm if the user wants to enable Backstage.

   if (auto state = GetSystemState()) {
      for (int i=0; i < state->OpenInfo->ArgCount; i++) {
         if (kt::iequals(state->OpenInfo->Args[i], "--backstage")) {
            if (i + 1 < state->OpenInfo->ArgCount) {
               int port = atoi(state->OpenInfo->Args[i + 1]);
               if (port > 0) return init_backstage(port);
               else return init_backstage();
            }
            else return init_backstage();
         }
      }
   }

   return(ERR::Okay);
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   {
      std::lock_guard<std::mutex> lock(glRequestLock);
      glRequestBuffers.clear();
   }
   release_backstage_websockets();
   release_backstage_logs();
   release_backstage_routes();
   if (glServer)   { FreeResource(glServer);   glServer = nullptr; }
   if (modRegex)   { FreeResource(modRegex);   modRegex = nullptr; }
   if (modNetwork) { FreeResource(modNetwork); modNetwork = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

#include "utility.cpp"
#include "routes.cpp"
#include "http.cpp"
#include "websocket.cpp"
#include "router.cpp"
#include "server.cpp"

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_backstage_module() { return &ModHeader; }
