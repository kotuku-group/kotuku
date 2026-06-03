/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

NOTE: The NetLookup class was created in order to support asynchronous name resolution in a way that would
be thread safe.  In essence the class is acting as a thread pool that is safely deallocated on termination.

-CLASS-
NetLookup: Resolve network IP addresses and names using Domain Name Servers.

Use the NetLookup class for resolving network names to IP addresses and vice versa.

-END-

*********************************************************************************************************************/

#define PRV_NETLOOKUP

struct resolve_buffer {
   OBJECTID NetLookupID = 0;
   ERR Error = ERR::Okay;
   IPAddress IP;
   std::string Address;

   resolve_buffer(OBJECTID pNLID, IPAddress pIP, std::string_view pAddress) :
      NetLookupID(pNLID), IP(pIP), Address(pAddress) { }

   resolve_buffer(OBJECTID pNLID, std::string_view pAddress) :
      NetLookupID(pNLID), Address(pAddress) { }

   std::vector<int8_t> serialise() {
      std::vector<int8_t> ser;
      ser.resize(sizeof(OBJECTID) + sizeof(ERR) + sizeof(IPAddress) + Address.size() + 1);
      auto ptr = ser.data();
      *(OBJECTID *)ptr = NetLookupID;
      ptr       += sizeof(OBJECTID);
      *(ERR*)ptr = Error;
      ptr       += sizeof(ERR);
      *(IPAddress *)ptr = IP;
      ptr       += sizeof(IPAddress);
      if (not Address.empty()) kt::copymem(Address.data(), ptr, Address.size() + 1);
      return ser;
   }

   resolve_buffer(int8_t *Data) {
      auto ptr = Data;
      NetLookupID = *(OBJECTID *)ptr;
      ptr  += sizeof(OBJECTID);
      Error = *(ERR *)ptr;
      ptr  += sizeof(ERR);
      IP    = *(IPAddress *)ptr;
      ptr  += sizeof(IPAddress);
      Address.assign((char *)ptr);
   }
};

static ERR resolve_address(CSTRING, const IPAddress *, DNSEntry &);
static ERR resolve_name(CSTRING, DNSEntry &);
static ERR cache_host(HOSTMAP &, CSTRING, const HostLookupResult &, DNSEntry &);

static std::vector<IPAddress> glNoAddresses;

static void resolve_callback(extNetLookup *, ERR, const std::string & = "", std::vector<IPAddress> & = glNoAddresses);

//********************************************************************************************************************
// Used for receiving asynchronous execution results (sent as a message).
// These routines execute in the main process.

static ERR resolve_name_receiver(APTR Custom, MSGID MsgID, int MsgType, APTR Message, int MsgSize)
{
   kt::Log log(__FUNCTION__);

   resolve_buffer r((int8_t *)Message);

   log.traceBranch("MsgID: %d, MsgType: %d, Host: %s", int(MsgID), int(MsgType), r.Address.c_str());

   if (kt::ScopedObjectLock<extNetLookup> nl(r.NetLookupID, 2000); nl.granted()) {
      bool found = false;
      DNSEntry cached;
      {
         std::shared_lock<std::shared_mutex> lock(glHostsMutex);
         auto it = glHosts.find(r.Address);
         if (it != glHosts.end()) {
            cached = it->second;
            found = true;
         }
      }

      if (found) {
         nl->Info = cached;
         resolve_callback(*nl, ERR::Okay, nl->Info.HostName, nl->Info.Addresses);
      }
      else resolve_callback(*nl, ERR::Failed);
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR resolve_addr_receiver(APTR Custom, MSGID MsgID, int MsgType, APTR Message, int MsgSize)
{
   kt::Log log(__FUNCTION__);

   resolve_buffer r((int8_t *)Message);

   log.traceBranch("MsgID: %d, MsgType: %d, Address: %s", int(MsgID), MsgType, r.Address.c_str());

   if (kt::ScopedObjectLock<extNetLookup> nl(r.NetLookupID, 2000); nl.granted()) {
      bool found = false;
      DNSEntry cached;
      {
         std::shared_lock<std::shared_mutex> lock(glAddressesMutex);
         auto it = glAddresses.find(r.Address);
         if (it != glAddresses.end()) {
            cached = it->second;
            found = true;
         }
      }

      if (found) {
         nl->Info = cached;
         resolve_callback(*nl, ERR::Okay, nl->Info.HostName, nl->Info.Addresses);
      }
      else resolve_callback(*nl, ERR::Failed);
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static void notify_free_callback(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extNetLookup *)CurrentContext())->Callback.clear();
}

/*********************************************************************************************************************

-METHOD-
BlockingResolveAddress: Resolves an IP address to a host name.

BlockingResolveAddress() performs an IP address resolution, converting an address to an official host name and list of
IP addresses.  The resolution process requires contact with a DNS server and this will cause the routine to block until
a response is received.

The results can be read from the #HostName field or received via the #Callback function.

-INPUT-
cstr Address: IP address to be resolved, e.g. 123.111.94.82.

-ERRORS-
Okay: The IP address was resolved successfully.
Args
NullArgs
Retry
Failed: The address could not be resolved.
Memory
BufferOverflow
SystemCall

-TAGS-
blocking, mutates-object, callback-inlines

*********************************************************************************************************************/

static ERR NETLOOKUP_BlockingResolveAddress(extNetLookup *Self, struct nl::BlockingResolveAddress *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Address)) return log.warning(ERR::NullArgs);

   log.branch("Address: %s", Args->Address);

   IPAddress ip;
   if (net::StrToAddress(std::string_view(Args->Address), &ip) IS ERR::Okay) {
      DNSEntry info;
      if (auto error = resolve_address(Args->Address, &ip, info); error IS ERR::Okay) {
         Self->Info = info;
         resolve_callback(Self, ERR::Okay, Self->Info.HostName, Self->Info.Addresses);
         return ERR::Okay;
      }
      else {
         resolve_callback(Self, error);
         return error;
      }
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-METHOD-
BlockingResolveName: Resolves a domain name to an official host name and a list of IP addresses.

BlockingResolveName() performs a domain name resolution, converting a domain name to its official host name and
IP addresses.  The resolution process requires contact with a DNS server and the function will block until a
response is received or a timeout occurs.

The results can be read from the #Addresses field or received via the #Callback function.

-INPUT-
cstr HostName: The host name to be resolved.

-ERRORS-
Okay
NullArgs
Retry
Failed
Memory
BufferOverflow
SystemCall

-TAGS-
blocking, mutates-object, callback-inlines

*********************************************************************************************************************/

static ERR NETLOOKUP_BlockingResolveName(extNetLookup *Self, struct nl::ResolveName *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->HostName)) return log.error(ERR::NullArgs);

   log.branch("Host: %s", Args->HostName);

   DNSEntry info;
   if (auto error = resolve_name(Args->HostName, info); error IS ERR::Okay) {
      Self->Info = info;
      resolve_callback(Self, ERR::Okay, Self->Info.HostName, Self->Info.Addresses);
      return ERR::Okay;
   }
   else {
      resolve_callback(Self, error, Args->HostName);
      return error;
   }
}

/*********************************************************************************************************************

-ACTION-
Free: Terminate the object.

This routine may block temporarily if there are unresolved requests awaiting completion in separate threads.

-TAGS-
blocking, mutates-object

*********************************************************************************************************************/

static ERR NETLOOKUP_Free(extNetLookup *Self)
{
   if (Self->Callback.isScript()) {
      UnsubscribeAction(Self->Callback.Context, AC::Free);
      Self->Callback.Type = CALL::NIL;
   }

   Self->~extNetLookup();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETLOOKUP_FreeWarning(extNetLookup *Self)
{
   // NOTE: If the NetLookup is terminated while threads are still running, it isn't an issue because the
   // threads always resolve and lock the NetLookup's ID before attempting to use it.
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR NETLOOKUP_NewPlacement(extNetLookup *Self)
{
   new (Self) extNetLookup;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ResolveAddress: Resolves an IP address to a host name.

ResolveAddress() performs a IP address resolution, converting an address to an official host name and list of
IP addresses.  The resolution process involves contacting a DNS server.  To prevent delays, asynchronous communication
is used so that the function can return immediately.  The #Callback function will be called on completion of the
process.

If synchronous (blocking) operation is desired then use the #BlockingResolveAddress() method.

-INPUT-
cstr Address: IP address to be resolved, e.g. "123.111.94.82".

-ERRORS-
Okay: The IP address was resolved successfully.
NullArgs
FieldNotSet
Failed: The address could not be resolved

-TAGS-
non-blocking, mutates-object, copies-input, callback-inlines

*********************************************************************************************************************/

static ERR NETLOOKUP_ResolveAddress(extNetLookup *Self, struct nl::ResolveAddress *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Address)) return log.warning(ERR::NullArgs);
   if (Self->Callback.Type IS CALL::NIL) return log.warning(ERR::FieldNotSet);

   log.branch("Address: %s", Args->Address);

   if ((Self->Flags & NLF::NO_CACHE) IS NLF::NIL) { // Use the cache if available.
      bool found = false;
      DNSEntry cached;
      {
         std::shared_lock<std::shared_mutex> lock(glAddressesMutex);
         auto it = glAddresses.find(Args->Address);
         if (it != glAddresses.end()) {
            cached = it->second;
            found = true;
         }
      }

      if (found) {
         Self->Info = cached;
         log.trace("Cache hit for address %s", Args->Address);
         resolve_callback(Self, ERR::Okay, Self->Info.HostName, Self->Info.Addresses);
         return ERR::Okay;
      }
   }

   IPAddress ip;
   if (net::StrToAddress(Args->Address, &ip) IS ERR::Okay) {
      resolve_buffer rb(Self->UID, ip, Args->Address);

      Self->Threads.emplace_back(std::make_unique<std::jthread>(std::jthread([](resolve_buffer rb) {
         DNSEntry dummy;
         rb.Error = resolve_address(rb.Address.c_str(), &rb.IP, dummy);
         auto ser = rb.serialise();
         if (auto error = SendMessage(glResolveAddrMsgID, MSF::NIL, ser.data(), ser.size()); error != ERR::Okay) {
            kt::Log(__FUNCTION__).warning("Failed to queue address resolution result: %s", GetErrorMsg(error));
         }
      }, std::move(rb))));

      return ERR::Okay;
   }
   else return log.warning(ERR::Failed);
}

/*********************************************************************************************************************

-METHOD-
ResolveName: Resolves a domain name to an official host name and a list of IP addresses.

ResolveName() performs a domain name resolution, converting a domain name to an official host name and IP addresses.
The resolution process involves contacting a DNS server.  To prevent delays, asynchronous communication is used so
that the function can return immediately.  The #Callback function will be called on completion of the process.

If synchronous (blocking) operation is desired then use the #BlockingResolveName() method.

-INPUT-
cstr HostName: The host name to be resolved.

-ERRORS-
Okay
NullArgs

-TAGS-
non-blocking, mutates-object, copies-input, callback-inlines

*********************************************************************************************************************/

static ERR NETLOOKUP_ResolveName(extNetLookup *Self, struct nl::ResolveName *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->HostName)) return log.error(ERR::NullArgs);

   log.branch("Host: %s", Args->HostName);

   if ((Self->Flags & NLF::NO_CACHE) IS NLF::NIL) { // Use the cache if available.
      bool found = false;
      DNSEntry cached;
      {
         std::shared_lock<std::shared_mutex> lock(glHostsMutex);
         auto it = glHosts.find(Args->HostName);
         if (it != glHosts.end()) {
            cached = it->second;
            found = true;
         }
      }

      if (found) {
         Self->Info = cached;
         log.trace("Cache hit for host %s", Self->Info.HostName);
         resolve_callback(Self, ERR::Okay, Self->Info.HostName, Self->Info.Addresses);
         return ERR::Okay;
      }
   }

   resolve_buffer rb(Self->UID, Args->HostName);
   Self->Threads.emplace_back(std::make_unique<std::jthread>(std::jthread([](resolve_buffer rb) {
      DNSEntry dummy;
      rb.Error = resolve_name(rb.Address.c_str(), dummy);
      auto ser = rb.serialise();
      if (auto error = SendMessage(glResolveNameMsgID, MSF::NIL, ser.data(), ser.size()); error != ERR::Okay) {
         kt::Log(__FUNCTION__).warning("Failed to queue name resolution result: %s", GetErrorMsg(error));
      }
   }, std::move(rb))));

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Addresses: List of resolved IP addresses.

A list of the most recently resolved IP addresses can be read from this field.

-TAGS-
object-owns-result, volatile-result

*********************************************************************************************************************/

static ERR GET_Addresses(extNetLookup *Self, int8_t **Value, int *Elements)
{
   if (not Self->Info.Addresses.empty()) {
      *Value = (int8_t *)Self->Info.Addresses.data();
      *Elements = Self->Info.Addresses.size();
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

/*********************************************************************************************************************

-FIELD-
Callback: This function will be called on the completion of any name or address resolution.

The function referenced here will receive the results of the most recently resolved name or address.  The C++
prototype is
`Function(*NetLookup, ERR Error, const std::string &amp;HostName, const std::vector&lt;IPAddress&gt; &amp;Addresses)`.

The Tiri prototype is as follows, with results readable from the #HostName and #Addresses fields:
`function(NetLookup, Error)`.

-TAGS-
object-owns-result, callback-held

*********************************************************************************************************************/

static ERR GET_Callback(extNetLookup *Self, FUNCTION * &Value)
{
   if (Self->Callback.defined()) {
      Value = &Self->Callback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_Callback(extNetLookup *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->Callback.isScript()) UnsubscribeAction(Self->Callback.Context, AC::Free);
      Self->Callback = *Value;
      if (Self->Callback.isScript()) {
         SubscribeAction(Self->Callback.Context, AC::Free, C_FUNCTION(notify_free_callback));
      }
   }
   else Self->Callback.clear();

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
HostName: Name of the most recently resolved host.

The name of the most recently resolved host is readable from this field.

-TAGS-
object-owns-result, volatile-result, null-terminated-result

*********************************************************************************************************************/

static ERR GET_HostName(extNetLookup *Self, std::string_view &Value)
{
   if (not Self->Info.HostName.empty()) {
      Value = Self->Info.HostName;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

//********************************************************************************************************************

static ERR cache_host(HOSTMAP &Store, CSTRING Key, const HostLookupResult &Result, DNSEntry &Cache)
{
   if (not Key) {
      if (Result.HostName.empty()) return ERR::Args;
      Key = Result.HostName.c_str();
   }

   DNSEntry cache;
   if (Result.HostName.empty()) cache.HostName = Key;
   else cache.HostName = Result.HostName;
   cache.Addresses = Result.Addresses;

   {
      std::shared_mutex &cache_mutex = (&Store IS &glHosts) ? glHostsMutex : glAddressesMutex;
      std::unique_lock<std::shared_mutex> lock(cache_mutex);
      auto &entry = Store[Key];
      entry = std::move(cache);
      Cache = entry;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR resolve_address(CSTRING Address, const IPAddress *IP, DNSEntry &Info)
{
   {
      std::shared_lock<std::shared_mutex> lock(glAddressesMutex);
      if (auto it = glAddresses.find(Address); it != glAddresses.end()) {
         Info = it->second;
         return ERR::Okay;
      }
   }

   HostLookupResult result;
   if (auto error = network_platform().resolve_address(Address, *IP, result); error != ERR::Okay) return error;
   return cache_host(glAddresses, Address, result, Info);
}

static ERR resolve_name(CSTRING HostName, DNSEntry &Info)
{
   // Use the cache if available.

   {
      std::shared_lock<std::shared_mutex> lock(glHostsMutex);
      auto it = glHosts.find(HostName);
      if (it != glHosts.end()) {
         Info = it->second;
         return ERR::Okay;
      }
   }

   kt::Log log(__FUNCTION__);
   log.detail("Resolving hostname: %s", HostName);

   HostLookupResult result;
   if (auto error = network_platform().resolve_name(HostName, result); error != ERR::Okay) return error;
   return cache_host(glHosts, HostName, result, Info);
}

//********************************************************************************************************************

static void resolve_callback(extNetLookup *Self, ERR Error, const std::string &HostName, std::vector<IPAddress> &Addresses)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("Host: %s", HostName.c_str());

   if (Self->Callback.isC()) {
      kt::SwitchContext context(Self->Callback.Context);
      auto routine = (ERR (*)(extNetLookup *, ERR, const std::string &, const std::vector<IPAddress> &, APTR))(Self->Callback.Routine);
      routine(Self, Error, HostName, Addresses, Self->Callback.Meta);
   }
   else if (Self->Callback.isScript()) {
      // Tiri scripts can retrieve the host and addresses from the NetLookup object - it's a more optimal solution
      sc::Call(Self->Callback, std::to_array<ScriptArg>({ { "NetLookup", Self, FDF_OBJECT }, { "Error", int(Error) } }));
   }
}

//********************************************************************************************************************

static const FieldArray clNetLookupFields[] = {
   { "ClientData", FDF_INT64|FDF_RW },
   { "Flags",      FDF_INT|FDF_FLAGS|FDF_RW },
   // Virtual fields
   { "HostName",   FDF_VIRTUAL|FDF_CPPSTRING|FDF_R|FDF_PURE, GET_HostName },
   { "Callback",   FDF_VIRTUAL|FDF_FUNCTION|FDF_RW|FDF_PURE, GET_Callback, SET_Callback },
   { "Addresses",  FDF_VIRTUAL|FDF_STRUCT|FDF_ARRAY|FDF_R|FDF_PURE, GET_Addresses, nullptr, "IPAddress" },
   END_FIELD
};

#include "class_netlookup_def.c"

//********************************************************************************************************************

ERR init_netlookup(void)
{
   clNetLookup = objMetaClass::create::global(
      fl::ClassVersion(VER_NETLOOKUP),
      fl::Name("NetLookup"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clNetLookupActions),
      fl::Methods(clNetLookupMethods),
      fl::Fields(clNetLookupFields),
      fl::Size(sizeof(extNetLookup)),
      fl::Path(MOD_PATH));

   return clNetLookup ? ERR::Okay : ERR::AddClass;
}
