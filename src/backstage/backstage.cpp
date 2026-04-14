/*********************************************************************************************************************

This source code and its accompanying files are in the public domain and therefore may be distributed without
restriction.  The source is based in part on libpng, authored by Glenn Randers-Pehrson, Andreas Eric Dilger and
Guy Eric Schalnat.

**********************************************************************************************************************

-MODULE-
Backstage: Provides a REST backend for interacting with the process over the network.

Backstage provides a REST backend for users and applications to interact with a Kōtuku program while it is running.
The module does not expose any API functionality, and is instead enabled by the user by specifying
`--backstage [port]` on the commandline.  If the command is omitted then backstage will do nothing.

The REST API and documentation on how to use Backstage is documented in the Kotuku Wiki.

-END-

*********************************************************************************************************************/

#define PRV_BACKSTAGE

#include <kotuku/main.h>
#include <kotuku/modules/backstage.h>
#include <kotuku/modules/network.h>
#include <kotuku/strings.hpp>

using namespace pf;

static OBJECTPTR modNetwork = nullptr;

JUMPTABLE_CORE
JUMPTABLE_NETWORK

static ERR init_backstage(int);

class objNetSocket *glServer = nullptr;

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   pf::Log log("Backstage");

   CoreBase = argCoreBase;

   if (objModule::load("network", &modNetwork, &NetworkBase) != ERR::Okay) return ERR::InitModule;

   // Parse commandline arguments to confirm if the user wants to enable Backstage.

   if (auto info = (OpenInfo *)GetResourcePtr(RES::OPEN_INFO)) {
      for (int i=0; i < info->ArgCount; i++) {
         if (pf::iequals(info->Args[i], "--backstage")) {
      	   if (i + 1 < info->ArgCount) {
      		   int port = atoi(info->Args[i + 1]);
      		   if (port > 0) {
      		      init_backstage(port);
      		      break;
      	      }
               else {
                  log.warning("Invalid port number %d specified for --backstage.", port);
                  return ERR::InvalidValue;
               }
            }
            else {
               log.warning("No port specified for --backstage.");
               return ERR::Failed;
            }
         }
      }
   }

   return(ERR::Okay);
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   if (modNetwork) { FreeResource(modNetwork); modNetwork = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

void server_feedback(objNetSocket *Socket, class objClientSocket *Client, NTC State)
{
   pf::Log log(__FUNCTION__);
   if (State IS NTC::CONNECTED) {
      log.msg("Client connected: %d.%d.%d.%d", Client->Client->IP[0], Client->Client->IP[1], Client->Client->IP[2], Client->Client->IP[3]);
   }
   else if (State IS NTC::DISCONNECTED) {
      log.msg("Client disconnected: %d.%d.%d.%d", Client->Client->IP[0], Client->Client->IP[1], Client->Client->IP[2], Client->Client->IP[3]);
   }
}

//********************************************************************************************************************

ERR server_incoming(objNetSocket *Socket, OBJECTPTR Context)
{
   return ERR::Okay;
}

//********************************************************************************************************************

ERR init_backstage(int Port)
{
   pf::Log log(__FUNCTION__);

   glServer = objNetSocket::create::global({
      fl::Port(Port),
      fl::Flags(NSF::SERVER | NSF::MULTI_CONNECT),
      fl::Feedback((CPTR)server_feedback),
      fl::Incoming((CPTR)server_incoming),
   });

   if (!glServer) {
      log.msg("Failed to initialise backstage server on port %d", Port);
      return ERR::CreateObject;
   }
   else {
      log.msg("Backstage is enabled at http://localhost:%d/", Port);
      return ERR::Okay;
   }
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_backstage_module() { return &ModHeader; }

