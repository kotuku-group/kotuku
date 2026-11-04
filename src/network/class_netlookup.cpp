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
   IPAddress IP = {};
   std::string Address;
   DNSEntry Info;
   bool Valid = true;

   resolve_buffer(OBJECTID pNLID, IPAddress pIP, std::string_view pAddress) :
      NetLookupID(pNLID), IP(pIP), Address(pAddress) { }

   resolve_buffer(OBJECTID pNLID, std::string_view pAddress) :
      NetLookupID(pNLID), Address(pAddress) { }

   std::vector<int8_t> serialise() {
      std::vector<int8_t> ser;
      ser.resize(sizeof(OBJECTID) + sizeof(ERR) + sizeof(IPAddress) + Address.size() + 1 +
         Info.HostName.size() + 1 + sizeof(uint32_t) + Info.Addresses.size() * sizeof(IPAddress));
      auto ptr = ser.data();
      std::memcpy(ptr, &NetLookupID, sizeof(NetLookupID));
      ptr       += sizeof(OBJECTID);
      std::memcpy(ptr, &Error, sizeof(Error));
      ptr       += sizeof(ERR);
      std::memcpy(ptr, &IP, sizeof(IP));
      ptr       += sizeof(IPAddress);
      std::memcpy(ptr, Address.c_str(), Address.size() + 1);
      ptr += Address.size() + 1;
      std::memcpy(ptr, Info.HostName.c_str(), Info.HostName.size() + 1);
      ptr += Info.HostName.size() + 1;
      auto count = uint32_t(Info.Addresses.size());
      std::memcpy(ptr, &count, sizeof(count));
      ptr += sizeof(count);
      if (count) std::memcpy(ptr, Info.Addresses.data(), count * sizeof(IPAddress));
      return ser;
   }

   resolve_buffer(std::span<std::byte> Message) {
      auto ptr = (const int8_t *)Message.data();
      auto end = ptr + Message.size();
      if (end - ptr < ptrdiff_t(sizeof(OBJECTID) + sizeof(ERR) + sizeof(IPAddress))) {
         Valid = false;
         return;
      }
      std::memcpy(&NetLookupID, ptr, sizeof(NetLookupID));
      ptr  += sizeof(OBJECTID);
      std::memcpy(&Error, ptr, sizeof(Error));
      ptr  += sizeof(ERR);
      std::memcpy(&IP, ptr, sizeof(IP));
      ptr  += sizeof(IPAddress);
      auto read_string = [&](std::string &Result) {
         auto terminator = (const int8_t *)std::memchr(ptr, 0, size_t(end - ptr));
         if (not terminator) return false;
         Result.assign((const char *)ptr, size_t(terminator - ptr));
         ptr = terminator + 1;
         return true;
      };
      if (not read_string(Address) or not read_string(Info.HostName) or end - ptr < ptrdiff_t(sizeof(uint32_t))) {
         Valid = false;
         return;
      }
      uint32_t count;
      std::memcpy(&count, ptr, sizeof(count));
      ptr += sizeof(count);
      if (count > size_t(end - ptr) / sizeof(IPAddress) or
          size_t(end - ptr) != size_t(count) * sizeof(IPAddress)) {
         Valid = false;
         return;
      }
      Info.Addresses.resize(count);
      if (count) std::memcpy(Info.Addresses.data(), ptr, count * sizeof(IPAddress));
   }
};

static ERR resolve_address(std::string_view, const IPAddress *, DNSEntry &, bool UseCache);
static ERR resolve_name(std::string_view, DNSEntry &, bool UseCache);
static ERR cache_host(HOSTMAP &, std::string_view, const HostLookupResult &, DNSEntry &);

static std::vector<IPAddress> glNoAddresses;

static void resolve_callback(extNetLookup *, ERR, std::string_view, std::vector<IPAddress> & = glNoAddresses);

//********************************************************************************************************************
// Used for receiving asynchronous execution results (sent as a message).
// These routines execute in the main process.

static ERR resolve_name_receiver(APTR Custom, int MsgID, MSGID MsgType, std::span<std::byte> Message)
{
   kt::Log log(__FUNCTION__);

   if (Message.empty()) return ERR::Okay;
   resolve_buffer r(Message);
   if (not r.Valid) return ERR::Okay;

   log.traceBranch("MsgID: %d, MsgType: %d, Host: %s", MsgID, int(MsgType), r.Address.c_str());

   if (kt::ScopedObjectLock<extNetLookup> nl(r.NetLookupID, 2000); nl.granted()) {
      if (r.Error IS ERR::Okay) {
         nl->Info = std::move(r.Info);
         resolve_callback(*nl, ERR::Okay, nl->Info.HostName, nl->Info.Addresses);
      }
      else resolve_callback(*nl, r.Error, "");
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR resolve_addr_receiver(APTR Custom, int MsgID, MSGID MsgType, std::span<std::byte> Message)
{
   kt::Log log(__FUNCTION__);

   if (Message.empty()) return ERR::Okay;
   resolve_buffer r(Message);
   if (not r.Valid) return ERR::Okay;

   log.traceBranch("MsgID: %d, MsgType: %d, Address: %s", MsgID, int(MsgType), r.Address.c_str());

   if (kt::ScopedObjectLock<extNetLookup> nl(r.NetLookupID, 2000); nl.granted()) {
      if (r.Error IS ERR::Okay) {
         nl->Info = std::move(r.Info);
         resolve_callback(*nl, ERR::Okay, nl->Info.HostName, nl->Info.Addresses);
      }
      else resolve_callback(*nl, r.Error, "");
   }
   return ERR::Okay;
}

//********************************************************************************************************************

/*********************************************************************************************************************

-METHOD-
BlockingResolveAddress: Resolves an IP address to a host name.

BlockingResolveAddress() performs an IP address resolution, converting an address to an official host name and list of
IP addresses.  The resolution process requires contact with a DNS server and this will cause the routine to block until
a response is received.

The results can be read from the #HostName field or received via the #Callback function.

-INPUT-
strview Address: IP address to be resolved, e.g. 123.111.94.82.

-ERRORS-
Okay: The IP address was resolved successfully.
Args
NullArgs
Retry
HostNotFound: The address could not be resolved.
Memory
BufferOverflow
SystemCall

-TAGS-
blocking, mutates-object, callback-inlines

*********************************************************************************************************************/

static ERR NETLOOKUP_BlockingResolveAddress(extNetLookup *Self, struct nl::BlockingResolveAddress *Args)
{
   kt::Log log;

   if ((not Args) or Args->Address.empty()) return log.warning(ERR::NullArgs);

   log.branch("Address: %.*s", int(Args->Address.size()), Args->Address.data());

   IPAddress ip;
   if (!net::StrToAddress(Args->Address, &ip)) {
      DNSEntry info;
      if (auto error = resolve_address(Args->Address, &ip, info,
          (Self->Flags & NLF::NO_CACHE) IS NLF::NIL); !error) {
         Self->Info = info;
         resolve_callback(Self, ERR::Okay, Self->Info.HostName, Self->Info.Addresses);
         return ERR::Okay;
      }
      else {
         resolve_callback(Self, error, "");
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
strview HostName: The host name to be resolved.

-ERRORS-
Okay
NullArgs
Retry
HostNotFound: The host name could not be resolved.
Memory
BufferOverflow
SystemCall

-TAGS-
blocking, mutates-object, callback-inlines

*********************************************************************************************************************/

static ERR NETLOOKUP_BlockingResolveName(extNetLookup *Self, struct nl::BlockingResolveName *Args)
{
   kt::Log log;

   if ((not Args) or Args->HostName.empty()) return log.error(ERR::NullArgs);

   log.branch("Host: %.*s", int(Args->HostName.size()), Args->HostName.data());

   DNSEntry info;
   if (auto error = resolve_name(Args->HostName, info, (Self->Flags & NLF::NO_CACHE) IS NLF::NIL); !error) {
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

extNetLookup::~extNetLookup()
{
   clear_callback_function(Callback);
}

//********************************************************************************************************************

static ERR NETLOOKUP_FreeWarning(extNetLookup *Self)
{
   // NOTE: If the NetLookup is terminated while threads are still running, it isn't an issue because the
   // threads always resolve and lock the NetLookup's ID before attempting to use it.
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR start_lookup(extNetLookup *Self, resolve_buffer Buffer, bool Reverse, bool UseCache)
{
   std::erase_if(Self->Threads, [](const auto &Worker) { return Worker.Finished->load(); });
   if (Self->Threads.size() >= 32) return ERR::BufferOverflow;

   auto finished = std::make_shared<std::atomic<bool>>(false);
   std::jthread thread([finished, Reverse, UseCache](resolve_buffer Buffer) {
      Buffer.Error = Reverse ? resolve_address(Buffer.Address, &Buffer.IP, Buffer.Info, UseCache) :
         resolve_name(Buffer.Address, Buffer.Info, UseCache);
      auto serialised = Buffer.serialise();
      auto id = Reverse ? glResolveAddrMsgID : glResolveNameMsgID;
      if (auto error = SendMessage(id, MSF::NIL, serialised); error != ERR::Okay) {
         kt::Log(__FUNCTION__).warning("Failed to queue resolution result: %s", GetErrorMsg(error));
      }
      finished->store(true);
   }, std::move(Buffer));
   Self->Threads.push_back({std::move(finished), std::move(thread)});
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

One object may have up to 32 outstanding asynchronous resolutions.  Completed threads are reclaimed when another
resolution starts.

-INPUT-
strview Address: IP address to be resolved, e.g. "123.111.94.82".

-ERRORS-
Okay: The IP address was resolved successfully.
NullArgs
FieldNotSet
Syntax: The address is not a valid IP address literal.
BufferOverflow: The object already has 32 outstanding resolution requests.

-TAGS-
non-blocking, mutates-object, copies-input, callback-inlines

*********************************************************************************************************************/

static ERR NETLOOKUP_ResolveAddress(extNetLookup *Self, struct nl::ResolveAddress *Args)
{
   kt::Log log;

   if ((not Args) or Args->Address.empty()) return log.warning(ERR::NullArgs);
   if (Self->Callback.Type IS CALL::NIL) return log.warning(ERR::FieldNotSet);

   log.branch("Address: %.*s", int(Args->Address.size()), Args->Address.data());

   const bool use_cache = (Self->Flags & NLF::NO_CACHE) IS NLF::NIL;
   if (use_cache) { // Use the cache if available.
      bool found = false;
      DNSEntry cached;
      std::string address(Args->Address);
      {
         std::shared_lock<std::shared_mutex> lock(glAddressesMutex);
         auto it = glAddresses.find(address);
         if (it != glAddresses.end()) {
            cached = it->second;
            found = true;
         }
      }

      if (found) {
         Self->Info = cached;
         log.trace("Cache hit for address %s", address.c_str());
         resolve_callback(Self, ERR::Okay, Self->Info.HostName, Self->Info.Addresses);
         return ERR::Okay;
      }
   }

   IPAddress ip;
   if (!net::StrToAddress(Args->Address, &ip)) {
      resolve_buffer rb(Self->UID, ip, Args->Address);
      return start_lookup(Self, std::move(rb), true, use_cache);
   }
   else return log.warning(ERR::Syntax);
}

/*********************************************************************************************************************

-METHOD-
ResolveName: Resolves a domain name to an official host name and a list of IP addresses.

ResolveName() performs a domain name resolution, converting a domain name to an official host name and IP addresses.
The resolution process involves contacting a DNS server.  To prevent delays, asynchronous communication is used so
that the function can return immediately.  The #Callback function will be called on completion of the process.

If synchronous (blocking) operation is desired then use the #BlockingResolveName() method.

One object may have up to 32 outstanding asynchronous resolutions.  Completed threads are reclaimed when another
resolution starts.

-INPUT-
strview HostName: The host name to be resolved.

-ERRORS-
Okay
NullArgs
BufferOverflow: The object already has 32 outstanding resolution requests.

-TAGS-
non-blocking, mutates-object, copies-input, callback-inlines

*********************************************************************************************************************/

static ERR NETLOOKUP_ResolveName(extNetLookup *Self, struct nl::ResolveName *Args)
{
   kt::Log log;

   if ((not Args) or Args->HostName.empty()) return log.error(ERR::NullArgs);

   log.branch("Host: %.*s", int(Args->HostName.size()), Args->HostName.data());

   const bool use_cache = (Self->Flags & NLF::NO_CACHE) IS NLF::NIL;
   if (use_cache) { // Use the cache if available.
      bool found = false;
      DNSEntry cached;
      std::string host_name(Args->HostName);
      {
         std::shared_lock<std::shared_mutex> lock(glHostsMutex);
         auto it = glHosts.find(host_name);
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
   return start_lookup(Self, std::move(rb), false, use_cache);
}

/*********************************************************************************************************************

-FIELD-
Addresses: List of resolved IP addresses.

A list of the most recently resolved IP addresses can be read from this field.

-TAGS-
object-owns-result, volatile-result

*********************************************************************************************************************/

static ERR GET_Addresses(extNetLookup *Self, std::span<IPAddress> &Value)
{
   if (not Self->Info.Addresses.empty()) {
      Value = std::span<IPAddress>(Self->Info.Addresses.data(), Self->Info.Addresses.size());
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

/*********************************************************************************************************************

-FIELD-
Callback: This function will be called on the completion of any name or address resolution.

The function referenced here will receive the results of the most recently resolved name or address.  The C++
prototype is `Function(*NetLookup, ERR Error, std::string_view HostName, const std::vector&lt;IPAddress&gt; &amp;Addresses)`.

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
   clear_callback_function(Self->Callback);
   if (Value) {
      Self->Callback = *Value;
      if (Self->Callback.defined()) Self->Callback.pin();
   }

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

static ERR cache_host(HOSTMAP &Store, std::string_view Key, const HostLookupResult &Result, DNSEntry &Cache)
{
   if (Key.empty()) {
      if (Result.HostName.empty()) return ERR::Args;
      Key = Result.HostName;
   }

   DNSEntry cache;
   if (Result.HostName.empty()) cache.HostName = Key;
   else cache.HostName = Result.HostName;
   cache.Addresses = Result.Addresses;

   {
      std::shared_mutex &cache_mutex = (&Store IS &glHosts) ? glHostsMutex : glAddressesMutex;
      std::unique_lock<std::shared_mutex> lock(cache_mutex);
      auto &entry = Store[std::string(Key)];
      entry = std::move(cache);
      Cache = entry;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR resolve_address(std::string_view Address, const IPAddress *IP, DNSEntry &Info, bool UseCache)
{
   std::string address(Address);
   if (UseCache) {
      std::shared_lock<std::shared_mutex> lock(glAddressesMutex);
      if (auto it = glAddresses.find(address); it != glAddresses.end()) {
         Info = it->second;
         return ERR::Okay;
      }
   }

   HostLookupResult result;
   if (auto error = network_platform().resolve_address(address.c_str(), *IP, result); error != ERR::Okay) return error;
   if (not UseCache) {
      Info.HostName = result.HostName.empty() ? address : std::move(result.HostName);
      Info.Addresses = std::move(result.Addresses);
      return ERR::Okay;
   }
   return cache_host(glAddresses, address, result, Info);
}

static ERR resolve_name(std::string_view HostName, DNSEntry &Info, bool UseCache)
{
   std::string host_name(HostName);
   if (UseCache) {
      std::shared_lock<std::shared_mutex> lock(glHostsMutex);
      auto it = glHosts.find(host_name);
      if (it != glHosts.end()) {
         Info = it->second;
         return ERR::Okay;
      }
   }

   kt::Log log(__FUNCTION__);
   log.detail("Resolving hostname: %s", host_name.c_str());

   HostLookupResult result;
   if (auto error = network_platform().resolve_name(host_name.c_str(), result); error != ERR::Okay) return error;
   if (not UseCache) {
      Info.HostName = result.HostName.empty() ? host_name : std::move(result.HostName);
      Info.Addresses = std::move(result.Addresses);
      return ERR::Okay;
   }
   return cache_host(glHosts, host_name, result, Info);
}

//********************************************************************************************************************

static void resolve_callback(extNetLookup *Self, ERR Error, std::string_view HostName,
   std::vector<IPAddress> &Addresses)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("Host: %.*s", int(HostName.size()), HostName.data());

   if (Self->Callback.stale()) {
      clear_callback_function(Self->Callback);
      return;
   }

   if (Self->Callback.isC()) {
      kt::SwitchContext context(Self->Callback.Context);
      auto routine = (ERR (*)(extNetLookup *, ERR, std::string_view, const std::vector<IPAddress> &, APTR))(
         Self->Callback.Routine);
      routine(Self, Error, HostName, Addresses, Self->Callback.Meta);
   }
   else if (Self->Callback.isScript()) {
      // Tiri scripts can retrieve the host and addresses from NetLookup.Addresses, it's a more optimal solution
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
