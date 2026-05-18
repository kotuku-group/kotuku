/*********************************************************************************************************************

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

#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

using namespace kt;

static OBJECTPTR modNetwork = nullptr;

JUMPTABLE_CORE
JUMPTABLE_NETWORK

static ERR init_backstage(int = 8765);

class objNetSocket *glServer = nullptr;
static std::mutex glRequestLock;
static std::unordered_map<OBJECTID, std::string> glRequestBuffers;

static constexpr size_t MAX_REQUEST_HEADER = 16 * 1024;
static constexpr CSTRING PING_BODY = "{\"status\":\"pong\"}";

struct BackstageRoute {
   std::string_view method;  // HTTP method
   std::string_view path;    // Human readable path (decorative)
   APTR handler;             // Function handling this route
   Regex *regex = nullptr;

   BackstageRoute() {}

   BackstageRoute(std::string_view pMethod, std::string_view pPath, Regex *pRegex, APTR pHandler) :
      method(pMethod), path(pPath), handler(pHandler), regex(pRegex) {}

   BackstageRoute(std::string_view pMethod, std::string_view pPath, std::string_view pRegex, APTR pHandler) :
      method(pMethod), path(pPath), handler(pHandler) {
      if (rx::Compile(pRegex, REGEX::NIL, nullptr, &regex) != ERR::Okay) {
         kt::Log().warning("Failed to compile regex: %.*s", pRegex.data(), int(pRegex.size()));
      }
   }

   ~BackstageRoute() {
      if (regex) FreeResource(regex);
   }
};

struct BackstageRequest {
   BackstageRoute   *route;
   objNetServer     *server;
   objClientSocket  *client;
   std::string_view path;
   std::string_view rawPath;
   std::string_view queryString;
   std::span<const uint8_t> body;
   std::unordered_map<std::string_view, std::string_view> pathParams;
   std::unordered_map<std::string_view, std::string_view> queryParams;
};

struct BackstageResponse {
   int status;
   std::string contentType;
   std::string body;
};

//********************************************************************************************************************

struct RequestLine {
   std::string_view method;
   std::string_view path;
   std::string_view version;
};

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   kt::Log log("Backstage");

   CoreBase = argCoreBase;

   if (objModule::load("network", &modNetwork, &NetworkBase) != ERR::Okay) return ERR::InitModule;

   // Parse commandline arguments to confirm if the user wants to enable Backstage.

   if (auto state = GetSystemState()) {
      for (int i=0; i < state->OpenInfo->ArgCount; i++) {
         if (kt::iequals(state->OpenInfo->Args[i], "--backstage")) {
            if (i + 1 < state->OpenInfo->ArgCount) {
               int port = atoi(state->OpenInfo->Args[i + 1]);
               if (port > 0) {
                  init_backstage(port);
                  break;
               }
               else {
                  init_backstage();
                  break;
               }
            }
            else {
               init_backstage();
               break;
            }
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
   if (modNetwork) { FreeResource(modNetwork); modNetwork = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

#include "server.cpp"
#include "routes.cpp"

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_backstage_module() { return &ModHeader; }
