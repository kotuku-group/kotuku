/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
NetClient: Represents a single client IP address.

When a connection is opened between a client IP and a @NetServer object, a new NetClient object will be created for
the client's IP address if one does not already exist.  All @ClientSocket connections to that IP address are then
tracked under the single NetClient object.

NetClient objects are intended to be created from the network interfacing code exclusively.

-END-

*********************************************************************************************************************/

static ERR NETCLIENT_Init(objNetClient *Self)
{
   if (Self->Owner->baseClassID() != CLASSID::NETSOCKET) {
      return kt::Log().warning(ERR::UnsupportedOwner);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
IP: The IP address of the client.

*********************************************************************************************************************/

static ERR GET_IP(objNetClient *Self, struct IPAddress **Value)
{
   *Value = &Self->IP;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Next: The next client IP with connections to the NetServer.

-FIELD-
Prev: The previous client IP with connections to the NetServer.

-FIELD-
Connections: Pointer to the first established socket connection for the client IP.

-FIELD-
ClientData: A custom pointer available for userspace.

-FIELD-
TotalConnections: The total number of current socket connections for the IP address.

*********************************************************************************************************************/

#include "netclient_def.c"

static const FieldArray clNetClientFields[] = {
   { "Next",        FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::NETCLIENT },
   { "Prev",        FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::NETCLIENT },
   { "Connections", FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::CLIENTSOCKET },
   { "ClientData",  FDF_POINTER|FDF_RW },
   { "TotalConnections", FDF_INT|FDF_R },
   // Virtual fields
   { "IP",          FDF_VIRTUAL|FDF_POINTER|FDF_STRUCT|FDF_R|FDF_PURE, GET_IP, nullptr, "IPAddress" },
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
