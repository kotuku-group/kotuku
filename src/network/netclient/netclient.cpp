/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
NetClient: Represents a single client IP address.

When a connection is opened between a client IP and a @NetSocket object, a new NetClient object will be created for
the client's IP address if one does not already exist.  All @ClientSocket connections to that IP address are then
tracked under the single NetClient object.

NetClient objects are intended to be created from the network interfacing code exclusively.

-END-

*********************************************************************************************************************/

static ERR NETCLIENT_Free(objNetClient *Self)
{
   Self->~objNetClient();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETCLIENT_Init(objNetClient *Self)
{
   if (Self->Owner->classID() != CLASSID::NETSOCKET) {
      return pf::Log().warning(ERR::UnsupportedOwner);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETCLIENT_NewPlacement(objNetClient *Self)
{
   new (Self) objNetClient;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
IP: The IP address of the client.

-FIELD-
Next: The next client IP with connections to the server socket.

-FIELD-
Prev: The previous client IP with connections to the server socket.

-FIELD-
Connections: Pointer to the first established socket connection for the client IP.

-FIELD-
ClientData: A custom pointer available for userspace.

-FIELD-
TotalConnections: The total number of current socket connections for the IP address.

*********************************************************************************************************************/

#include "netclient_def.c"

static const FieldArray clNetClientFields[] = {
   { "IP",          FDF_ARRAY|FDF_BYTE|FDF_R, nullptr, nullptr, 8 },
   { "Next",        FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::NETCLIENT },
   { "Prev",        FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::NETCLIENT },
   { "Connections", FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::CLIENTSOCKET },
   { "ClientData",  FDF_POINTER|FDF_RW },
   { "TotalConnections", FDF_INT|FDF_R },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_netclient(void)
{
   clNetClient = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::NETCLIENT),
      fl::ClassVersion(1.0),
      fl::Name("NetClient"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clNetClientActions),
      fl::Fields(clNetClientFields),
      fl::Size(sizeof(objNetClient)),
      fl::Path(MOD_PATH));

   return clNetClient ? ERR::Okay : ERR::AddClass;
}
