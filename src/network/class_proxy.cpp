/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Proxy: Manages user settings for proxy servers.

The proxy server class provides a global management service for a user's proxy servers.  You can alter proxy settings
manually or present the user with a dialog box to edit and create new proxies.  Scanning functions are also provided
with filtering, allowing you to scan for proxies that should be used with the user's network connection.

Proxy objects are designed to work similarly to database recordsets. Creating a new proxy object will allow you to
create a new proxy record if all required fields are set and the object is saved.

Searching through the records with the #Find() and #FindNext() methods will move the recordset through
each entry the proxy database.  You may change existing values of any proxy and then save the changes by calling the
#SaveSettings() action.
-END-

*********************************************************************************************************************/

#define PRV_PROXY

#include <memory>
#include <string_view>
#include <unordered_map>

//********************************************************************************************************************
// Global proxy configuration access.  Acquire glProxyMutex before using.

static std::recursive_mutex glProxyMutex;
static bool glProxyFileChecked = false;
static objConfig *glProxyConfig = nullptr;

static objConfig * get_proxy_config(void)
{
   std::lock_guard lock(glProxyMutex);
   if (!glProxyFileChecked) {
      kt::SwitchContext ctx(glNetworkModule);
      glProxyConfig = objConfig::create::global({ fl::Path("user:config/network/proxies.cfg") });
      glProxyFileChecked = true;
   }

   return glProxyConfig;
}

// Cleanup on module expunge

static void cleanup_proxy_config(void)
{
   std::lock_guard lock(glProxyMutex);
   if (glProxyConfig) { FreeResource(glProxyConfig); glProxyConfig = nullptr; }
   glProxyFileChecked = false;
}

//********************************************************************************************************************

class extProxy : public objProxy {
public:
   std::string GroupName;
   std::string FindPort;
   int FindEnabled = -1;
   bool Find = false;

   void setString(STRING& field, std::string_view value) {
      if (field) { FreeResource(field); field = nullptr; }
      if (!value.empty()) { field = kt::strclone(value); }
   }
};

static ERR find_proxy(extProxy *);

static bool parse_record_id(const std::string &GroupName, int &Record)
{
   if (GroupName.empty()) return false;

   int record = 0;
   for (auto ch : GroupName) {
      if ((ch < '0') or (ch > '9')) return false;

      int digit = ch - '0';
      if (record > ((0x7fffffff - digit) / 10)) return false;

      record = (record * 10) + digit;
   }

   Record = record;
   return true;
}

//********************************************************************************************************************

static void clear_values(extProxy *);
static ERR get_record(extProxy *);

/*********************************************************************************************************************

-METHOD-
DeleteRecord: Removes a proxy from the database.

Call the DeleteRecord() method to remove a proxy from the system.  The proxy will be permanently removed from the proxy
database on the success of this function.

-ERRORS-
Okay: Proxy deleted.
-END-

*********************************************************************************************************************/

static ERR PROXY_DeleteRecord(extProxy *Self)
{
   kt::Log log;

   if ((Self->GroupName.empty()) or (!Self->Record)) return log.error(ERR::Failed);

   log.branch();

   auto cfg = objConfig::create {fl::Path("user:config/network/proxies.cfg") };
   if (!cfg.ok()) return log.error(ERR::CreateObject);

   if (auto error = cfg->deleteGroup(Self->GroupName.c_str()); error != ERR::Okay) return log.warning(error);
   if (auto error = cfg->saveSettings(); error != ERR::Okay) return log.warning(error);
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Disable: Marks a proxy as disabled.

Calling the Disable() action will mark the proxy as disabled.  Disabled proxies remain in the system but are ignored by
programs that scan the database for active proxies.

The change will not come into effect until the proxy record is saved.

*********************************************************************************************************************/

static ERR PROXY_Disable(extProxy *Self)
{
   Self->Enabled = false;
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Enable: Enables a proxy.

Calling the Enable() action will mark the proxy as enabled.  The change will not come into effect until the proxy record
is saved.

*********************************************************************************************************************/

static ERR PROXY_Enable(extProxy *Self)
{
   Self->Enabled = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Find: Search for a proxy that matches a set of filters.

The following example searches for all proxies available for use on port 80 (HTTP).

<pre>
objProxy::create proxy;
if (proxy.ok()) {
   if (prxFind(*proxy, 80) IS ERR::Okay) {
      do {
         ...
      } while (prxFindNext(*proxy) IS ERR::Okay);
   }
}
</pre>

-INPUT-
int Port: The port number  to access.  If zero, all proxies will be returned if you perform a looped search.
int Enabled: Set to `true` to return only enabled proxies, `false` for disabled proxies or `-1` for all proxies.

-ERRORS-
Okay: A proxy was discovered.
NoSearchResult: No matching proxy was discovered.
-END-

*********************************************************************************************************************/

static ERR PROXY_Find(extProxy *Self, struct prx::Find *Args)
{
   kt::Log log;

   log.traceBranch("Port: %d, Enabled: %d", (Args) ? Args->Port : 0, (Args) ? Args->Enabled : -1);

   const std::lock_guard<std::recursive_mutex> lock(glProxyMutex);

   if (auto config [[maybe_unused]] = get_proxy_config()) {
      if (auto error = network_platform().sync_host_proxies(config); error != ERR::Okay) return log.warning(error);

      if (Args) {
         Self->FindPort = (Args->Port > 0) ? std::to_string(Args->Port) : "";
         Self->FindEnabled = Args->Enabled;
      }
      else {
         Self->FindPort.clear();
         Self->FindEnabled = -1;
      }

      Self->GroupName.clear();

      return find_proxy(Self);
   }
   else return ERR::CreateObject;
}

/*********************************************************************************************************************

-METHOD-
FindNext: Continues an initiated search.

This method continues searches that have been initiated by the #Find() method. If a proxy is found that matches
the filter, `ERR::Okay` is returned and the details of the proxy object will reflect the data of the discovered record.
`ERR::NoSearchResult` is returned if there are no more matching proxies.

-ERRORS-
Okay: A proxy was discovered.
NoSearchResult: No matching proxy was discovered.
-END-

*********************************************************************************************************************/

static ERR PROXY_FindNext(extProxy *Self)
{
   if (!Self->Find) return ERR::NoSearchResult; // Ensure that Find() was used to initiate a search

   return find_proxy(Self);
}

//********************************************************************************************************************

// Check if proxy matches port filter

template<typename KeysType>
static bool matchesPortFilter(const KeysType& keys, const std::string& findPort) {
   if (findPort.empty()) return true;

   if (keys.contains("Port")) {
      const auto& port = keys.at("Port");
      return (port IS "0") or kt::wildcmp(port, findPort);
   }
   return false;
}

// Check if proxy matches enabled filter

template<typename KeysType>
static bool matchesEnabledFilter(const KeysType& keys, int findEnabled) {
   if (findEnabled IS -1) return true;

   if (keys.contains("Enabled")) {
      int enabled;
      return parse_record_id(keys.at("Enabled"), enabled) and (enabled IS findEnabled);
   }
   return false;
}

static ERR find_proxy(extProxy *Self)
{
   kt::Log log(__FUNCTION__);

   clear_values(Self);

   const std::lock_guard<std::recursive_mutex> lock(glProxyMutex);

   if (auto config = get_proxy_config()) {
      if (!Self->Find) Self->Find = true; // Start of search

      ConfigGroups *groups;
      if (config->get(FID_Data, groups) != ERR::Okay) return ERR::NoData;

      auto group = groups->begin();

      // If continuing search, find next record
      if (!Self->GroupName.empty()) {
         group = std::find_if(groups->begin(), groups->end(),
            [&](const auto& g) { return g.first IS Self->GroupName; });
         if (group != groups->end()) ++group;
      }

      log.trace("Finding next proxy. Port: '%d', Enabled: %d", Self->FindPort, Self->FindEnabled);

      // Search for matching proxy
      for (; group != groups->end(); ++group) {
         log.trace("Checking group: %s", group->first.c_str());

         const auto& keys = group->second;
         int record;
         if (!parse_record_id(group->first, record)) continue;

         // Apply filters
         if (!matchesPortFilter(keys, Self->FindPort)) continue;
         if (!matchesEnabledFilter(keys, Self->FindEnabled)) continue;

         // TODO: Implement network and gateway filters
         if (keys.contains("NetworkFilter")) {
            log.error("Network filters not supported yet.");
         }

         if (keys.contains("GatewayFilter")) {
            log.error("Gateway filters not supported yet.");
         }

         log.trace("Found matching proxy.");
         Self->GroupName = group->first;
         return get_record(Self);
      }

      log.trace("No proxy matched.");
      Self->Find = false;
      return ERR::NoSearchResult;
   }
   else return ERR::CreateObject;
}

//********************************************************************************************************************

static ERR PROXY_Free(extProxy *Self)
{
   clear_values(Self);
   Self->~extProxy();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR PROXY_Init(extProxy *Self)
{
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR PROXY_NewPlacement(extProxy *Self)
{
   new (Self) extProxy;
   Self->Enabled = true;
   Self->Port = 80;
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
SaveSettings: Permanently saves user configurable settings for a proxy.

This action saves a user's settings for a proxy. Saving the proxy settings will make them available to the user on
subsequent logins.

Settings are saved to the user's local account under `user:config/network/proxies.cfg`.  It is possible for the
administrator to define proxy settings as the default for all users by copying the `proxies.cfg` file to the
`system:users/default/config/network/` folder.
-END-

*********************************************************************************************************************/

static ERR PROXY_SaveSettings(extProxy *Self)
{
   kt::Log log;

   if ((!Self->Server) or (!Self->ServerPort)) return log.error(ERR::FieldNotSet);

   log.branch("Host: %d", Self->Host);

   if (Self->Host) {
      return network_platform().save_host_proxy(Self->Server, Self->ServerPort, Self->Port, Self->Enabled);
   }

   const std::lock_guard<std::recursive_mutex> lock(glProxyMutex);

   if (auto config = get_proxy_config()) {
      if (!Self->GroupName.empty()) config->deleteGroup(Self->GroupName.c_str());
      else { // This is a new proxy
         int id = 0;
         config->read("ID", "Value", id);
         id = id + 1;
         config->write("ID", "Value", std::to_string(id));

         Self->GroupName = std::to_string(id);
         Self->Record = id;
      }

      config->write(Self->GroupName, "Port",          std::to_string(Self->Port));
      config->write(Self->GroupName, "NetworkFilter", Self->NetworkFilter);
      config->write(Self->GroupName, "GatewayFilter", Self->GatewayFilter);
      config->write(Self->GroupName, "Username",      Self->Username);
      config->write(Self->GroupName, "Password",      Self->Password);
      config->write(Self->GroupName, "Name",          Self->ProxyName);
      config->write(Self->GroupName, "Server",        Self->Server);
      config->write(Self->GroupName, "ServerPort",    std::to_string(Self->ServerPort));
      config->write(Self->GroupName, "Enabled",       std::to_string(Self->Enabled));

      objFile::create file = {
         fl::Path("user:config/network/proxies.cfg"),
         fl::Permissions(PERMIT::USER_READ|PERMIT::USER_WRITE),
         fl::Flags(FL::NEW|FL::WRITE)
      };

      if (file.ok()) return config->saveToObject(*file);
      else return ERR::CreateObject;
   }
   else return ERR::CreateObject;
}

/*********************************************************************************************************************

-FIELD-
GatewayFilter: The IP address of the gateway that the proxy is limited to.

The GatewayFilter defines the IP address of the gateway that this proxy is limited to. It is intended to limit the
results of searches performed by the #Find() method.

*********************************************************************************************************************/

static ERR SET_GatewayFilter(extProxy *Self, CSTRING Value)
{
   Self->setString(Self->GatewayFilter, Value ? Value : "");
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Host: If `true`, the proxy settings are derived from the host operating system's default settings.

If Host is set to `true`, the proxy settings are derived from the host operating system's default settings.  Hosted
proxies are treated differently to user proxies - they have priority, and any changes are applied directly to the host
system rather than the user's configuration.

-FIELD-
Port: Defines the ports supported by this proxy.

The Port defines the port that the proxy server is supporting, e.g. port 80 for HTTP.

*********************************************************************************************************************/

static ERR SET_Port(extProxy *Self, int Value)
{
   if (Value >= 0) {
      Self->Port = Value;
      return ERR::Okay;
   }
   return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
NetworkFilter: The name of the network that the proxy is limited to.

The NetworkFilter defines the name of the network that this proxy is limited to. It is intended to limit the results of
searches performed by the #Find() method.

This filter must not be set if the proxy needs to work on an unnamed network.

*********************************************************************************************************************/

static ERR SET_NetworkFilter(extProxy *Self, CSTRING Value)
{
   Self->setString(Self->NetworkFilter, Value ? Value : "");
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Username: The username to use when authenticating against the proxy server.

If the proxy requires authentication, the user name may be set here to enable an automated authentication process. If
the username is not set, a dialog will be required to prompt the user for the user name before communicating with the
proxy server.

*********************************************************************************************************************/

static ERR SET_Username(extProxy *Self, CSTRING Value)
{
   Self->setString(Self->Username, Value ? Value : "");
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Password: The password to use when authenticating against the proxy server.

If the proxy requires authentication, the user password may be set here to enable an automated authentication process.
If the password is not set, a dialog will need to be used to prompt the user for the password before communicating with
the proxy.

*********************************************************************************************************************/

static ERR SET_Password(extProxy *Self, CSTRING Value)
{
   Self->setString(Self->Password, Value ? Value : "");
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ProxyName: A human readable name for the proxy server entry.

A proxy can be given a human readable name by setting this field.

*********************************************************************************************************************/

static ERR SET_ProxyName(extProxy *Self, CSTRING Value)
{
   Self->setString(Self->ProxyName, Value ? Value : "");
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Server: The destination address of the proxy server - may be an IP address or resolvable domain name.

The domain name or IP address of the proxy server must be defined here.

*********************************************************************************************************************/

static ERR SET_Server(extProxy *Self, CSTRING Value)
{
   Self->setString(Self->Server, Value ? Value : "");
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ServerPort: The port that is used for proxy server communication.

The port used to communicate with the proxy server must be defined here.

*********************************************************************************************************************/

static ERR SET_ServerPort(extProxy *Self, int Value)
{
   if (Value > 0 and Value <= 0xffff) {
      Self->ServerPort = Value;
      return ERR::Okay;
   }
   kt::Log log;
   return log.error(ERR::OutOfRange);
}

/*********************************************************************************************************************

-FIELD-
Enabled: All proxies are enabled by default until this field is set to `false`.

To disable a proxy, set this field to `false` or call the #Disable() action.  This prevents the proxy from being
discovered in searches.

*********************************************************************************************************************/

static ERR SET_Enabled(extProxy *Self, int Value)
{
   Self->Enabled = (Value != 0);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Record: The unique ID of the current proxy record.

The Record is set to the unique ID of the current proxy record.  If no record is indexed then the Record is set to
zero.

If Record is set manually, the proxy object will attempt to lookup that record.  `ERR::Okay` will be returned if the
record is found and all record fields will be updated to reflect the data of that proxy.
-END-

*********************************************************************************************************************/

static ERR SET_Record(extProxy *Self, int Value)
{
   clear_values(Self);
   Self->GroupName = std::to_string(Value);
   return get_record(Self);
}

//********************************************************************************************************************
// The group field must be set to the record that you want before you call this function.
//
// Also not that you must have called clear_values() at some point before this function.

static ERR get_record(extProxy *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Group: %s", Self->GroupName.c_str());

   if (!parse_record_id(Self->GroupName, Self->Record)) return ERR::Search;

   const std::lock_guard<std::recursive_mutex> lock(glProxyMutex);

   if (auto config = get_proxy_config()) {
      std::string str;
      if (config->read(Self->GroupName, "Server", str) IS ERR::Okay) {
         Self->setString(Self->Server, str);

         if (config->read(Self->GroupName, "NetworkFilter", str) IS ERR::Okay) {
            Self->setString(Self->NetworkFilter, str);
         }
         if (config->read(Self->GroupName, "GatewayFilter", str) IS ERR::Okay) {
            Self->setString(Self->GatewayFilter, str);
         }
         if (config->read(Self->GroupName, "Username", str) IS ERR::Okay) {
            Self->setString(Self->Username, str);
         }
         if (config->read(Self->GroupName, "Password", str) IS ERR::Okay) {
            Self->setString(Self->Password, str);
         }
         if (config->read(Self->GroupName, "Name", str) IS ERR::Okay) {
            Self->setString(Self->ProxyName, str);
         }

         config->read(Self->GroupName, "Port", Self->Port);
         config->read(Self->GroupName, "ServerPort", Self->ServerPort);
         config->read(Self->GroupName, "Enabled", Self->Enabled);
         config->read(Self->GroupName, "Host", Self->Host);
         return ERR::Okay;
      }
      else return log.error(ERR::NotFound);
   }
   else return log.error(ERR::CreateObject);
}

//********************************************************************************************************************

static void clear_values(extProxy *Self)
{
   kt::Log log(__FUNCTION__);

   log.trace("");

   Self->Record     = 0;
   Self->Port       = 0;
   Self->Enabled    = 0;
   Self->ServerPort = 0;
   Self->Host       = 0;
   Self->setString(Self->NetworkFilter, "");
   Self->setString(Self->GatewayFilter, "");
   Self->setString(Self->Username, "");
   Self->setString(Self->Password, "");
   Self->setString(Self->ProxyName, "");
   Self->setString(Self->Server, "");
}

//********************************************************************************************************************

static const FieldDef clPorts[] = {
   { "FTP-Data",  20 },
   { "FTP",       21 },
   { "SSH",       22 },
   { "Telnet",    23 },
   { "SMTP",      25 },
   { "RSFTP",     26 },
   { "HTTP",      80 },
   { "SFTP",      115 },
   { "SQL",       118 },
   { "IRC",       194 },
   { "LDAP",      389 },
   { "HTTPS",     443 },
   { "FTPS",      990 },
   { "TelnetSSL", 992 },
   { "All",       0 },   // All ports
   { nullptr, 0 }
};

static const FieldArray clProxyFields[] = {
   { "NetworkFilter", FDF_STRING|FDF_RW, nullptr, SET_NetworkFilter },
   { "GatewayFilter", FDF_STRING|FDF_RW, nullptr, SET_GatewayFilter },
   { "Username",      FDF_STRING|FDF_RW, nullptr, SET_Username },
   { "Password",      FDF_STRING|FDF_RW, nullptr, SET_Password },
   { "ProxyName",     FDF_STRING|FDF_RW, nullptr, SET_ProxyName },
   { "Server",        FDF_STRING|FDF_RW, nullptr, SET_Server },
   { "Port",          FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_Port, &clPorts },
   { "ServerPort",    FDF_INT|FDF_RW, nullptr, SET_ServerPort },
   { "Enabled",       FDF_INT|FDF_RW, nullptr, SET_Enabled },
   { "Record",        FDF_INT|FDF_RW, nullptr, SET_Record },
   END_FIELD
};

#include "class_proxy_def.c"

//********************************************************************************************************************

ERR init_proxy(void)
{
   clProxy = objMetaClass::create::global(
      fl::ClassVersion(VER_PROXY),
      fl::Name("Proxy"),
      fl::Category(CCF::NETWORK),
      fl::Actions(clProxyActions),
      fl::Methods(clProxyMethods),
      fl::Fields(clProxyFields),
      fl::Size(sizeof(extProxy)),
      fl::Path(MOD_PATH));

   return clProxy ? ERR::Okay : ERR::AddClass;
}
