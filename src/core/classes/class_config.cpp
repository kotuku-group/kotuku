/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Config: Manages the reading and writing of configuration files.

The Config class is provided for reading text based key-values in a simple structured format.  Although basic and
lacking support for trees and types, they are reliable, easy to support and use minimal resources.

The following segment of a config file illustrates:

<pre>
[Action]
ClassID  = 5800
Path = modules:action

[Animation]
ClassID  = 1000
Path = modules:animation

[Arrow]
ClassID  = 3200
Path = modules:arrow
</pre>

Notice the text enclosed in square brackets, such as `[Action]`. These are referred to as 'groups', which are
responsible for holding groups of key values expressed as strings.  In the above example, keys are defined by the
`ClassID` and `Path` identifiers.

The following source code illustrates how to open the classes.cfg file and read a key from it:

<pre>
cfg = obj.new('config', { path='config:classes.cfg' })
err, str = cfg.mtReadValue('Action', 'Path')
print('The Action class is located at ' .. str)
</pre>

Please note that internal string comparisons of group and key names are case sensitive by default.  Use of camel-case
is recommended as the default naming format.

-END-

*********************************************************************************************************************/

#include "../defs.h"
#include <kotuku/main.h>

class extConfig;
static uint32_t calc_crc(extConfig *Self);

class extConfig : public objConfig {
   public:
   using create = kt::Create<extConfig>;
   uint32_t CRC;   // CRC32, for determining if config data has been altered

   extConfig(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : objConfig(ClassPtr, ObjectID), CRC(0) { }

   ~extConfig() {
      if ((Flags & CNF::AUTO_SAVE) != CNF::NIL) {
         if (not Path.empty()) {
            auto crc = calc_crc(this);

            if ((not crc) or (crc != CRC)) {
               kt::Log().msg("Auto-saving changes to \"%s\" (CRC: %d : %d)", Path.c_str(), CRC, crc);

               objFile::create file = { fl::Path(Path), fl::Flags(FL::WRITE|FL::NEW), fl::Permissions(PERMIT::NIL) };
               saveToObject(*file);
            }
         }
      }
   }
};

class FilterConfig {
   public:
   bool reverse = false;
   bool valid = false;
   std::string name;
   std::vector<std::string> values;
};

static ERR CONFIG_SaveSettings(extConfig *);

//********************************************************************************************************************

static bool check_for_key(std::string_view);
static ERR parse_config(extConfig *, std::string_view);
static ConfigGroup * find_group_wild(extConfig *Self, std::string_view Group);
static void merge_groups(ConfigGroups &, ConfigGroups &);
static std::string_view sort_key_value(const ConfigGroup &, std::string_view);
static void apply_filters(extConfig *);
static void apply_key_filter(extConfig *, std::string_view);
static void apply_group_filter(extConfig *, std::string_view);
static class FilterConfig parse_filter(std::string_view, bool);

//********************************************************************************************************************

static std::string_view next_line(std::string_view Data)
{
   constexpr std::string_view WHITESPACE = " \t\n\r";

   // Find the end of the current line
   if (auto newline = Data.find('\n'); newline != std::string_view::npos) {
      Data.remove_prefix(newline + 1);
   }
   else return {};

   // Skip empty lines and leading whitespace
   if (auto pos = Data.find_first_not_of(WHITESPACE); pos != std::string_view::npos) {
      Data.remove_prefix(pos);
      return Data;
   }
   else return {}; // Only whitespace remaining
}

//********************************************************************************************************************
// Searches for the next group in a text buffer, returns its name and the start of the first key value.

static std::string_view next_group(std::string_view Data, std::string &GroupName)
{
   while (!Data.empty()) {
      if (Data.front() IS '[') {
         auto close = Data.find(']');
         auto newline = Data.find('\n');

         // Check for invalid '[' before ']'
         auto next_open = Data.find('[', 1);
         if ((next_open != std::string_view::npos) and (next_open < close)) {
            // Invalid nested '[', skip past it
            Data.remove_prefix(next_open);
            continue;
         }

         if ((close != std::string_view::npos) and ((newline IS std::string_view::npos) or (close < newline))) {
            GroupName = Data.substr(1, close - 1);
            Data.remove_prefix(close + 1);
            return next_line(Data);
         }
      }
      Data = next_line(Data);
   }
   return Data;
}

//********************************************************************************************************************

static std::pair<std::string, KEYVALUE> * find_group(extConfig *Self, std::string_view GroupName)
{
   for (auto &group : Self->Groups) {
      if (group.first IS GroupName) return &group;
   }
   return nullptr;
}

//********************************************************************************************************************

static uint32_t calc_crc(extConfig *Self)
{
   uint32_t crc = 0;
   for (auto & [group, keys] : Self->Groups) {
      crc = GenCRC32(crc, (APTR)group.c_str(), group.size());
      for (auto & [k, v] : keys) {
         crc = GenCRC32(crc, (APTR)k.c_str(), k.size());
         crc = GenCRC32(crc, (APTR)v.c_str(), v.size());
      }
   }
   return crc;
}

//********************************************************************************************************************
// Parse source file(s) into the Config object.
//
// Note that multiple files can be specified by separating each file path with a pipe.  This allows you to merge
// many configuration files into one object.

static ERR parse_file(extConfig *Self, std::string_view Path)
{
   ERR error = ERR::Okay;
   std::string_view paths(Path);

   while ((not paths.empty()) and (!error)) {
      auto sep = paths.find_first_of(";|"); // Find the next separator
      auto current_path = (sep != std::string_view::npos) ? paths.substr(0, sep) : paths;

      objFile::create file = { fl::Path(current_path), fl::Flags(FL::READ|FL::APPROXIMATE) };

      if (file.ok()) {
         auto filesize = file->get<int>(FID_Size);

         if (filesize > 0) {
            std::string data(filesize + 1, '\0');
            file->read(data.data(), filesize); // Read the entire file
            data[filesize] = '\n';
            error = parse_config(Self, data);
         }
      }
      else if ((Self->Flags & CNF::OPTIONAL_FILES) != CNF::NIL) error = ERR::Okay;

      // Move past this path and the separator
      if (sep != std::string_view::npos) paths.remove_prefix(sep + 1);
      else paths = {};
   }

   return error;
}

/*********************************************************************************************************************
-ACTION-
Clear: Clears all configuration data.
-END-
*********************************************************************************************************************/

static ERR CONFIG_Clear(extConfig *Self)
{
   Self->Groups.clear();
   Self->KeyFilter.clear();
   Self->GroupFilter.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
DataFeed: Data can be added to a Config object through this action.

This action will accept configuration data in `TEXT` format.  Any existing data that matches to the new group keys will
be overwritten with new values.
-END-
*********************************************************************************************************************/

static ERR CONFIG_DataFeed(extConfig *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if (Args->Datatype IS DATA::TEXT) {
      auto buf = (Args->Size > 0) ? std::string_view((CSTRING)Args->Buffer, Args->Size) : std::string_view((CSTRING)Args->Buffer);
      if (auto error = parse_config(Self, buf); !error) {
         apply_filters(Self);
      }
      else return error;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DeleteKey: Deletes single key entries.

This method deletes a single key from the Config object.

-INPUT-
strview Group: The name of the targeted group.
strview Key: The name of the targeted key.

-ERRORS-
Okay
NullArgs
Search

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR CONFIG_DeleteKey(extConfig *Self, struct cfg::DeleteKey *Args)
{
   kt::Log log;

   if ((not Args) or Args->Group.empty() or Args->Key.empty()) return ERR::NullArgs;

   log.msg("Group: %.*s, Key: %.*s", int(Args->Group.size()), Args->Group.data(), int(Args->Key.size()),
      Args->Key.data());

   for (auto & [group, keys] : Self->Groups) {
      if (group IS Args->Group) {
         if (auto it = keys.find(Args->Key); it != keys.end()) keys.erase(it);
         return ERR::Okay;
      }
   }

   return ERR::Search;
}

/*********************************************************************************************************************

-METHOD-
DeleteGroup: Deletes entire groups of configuration data.

This method will delete an entire group of key-values from a config object if a matching group name is provided.

-INPUT-
strview Group: The name of the group that will be deleted.

-ERRORS-
Okay: The group was deleted or does not exist.
NullArgs

-TAGS-
mutates-object, idempotent
-END-

*********************************************************************************************************************/

static ERR CONFIG_DeleteGroup(extConfig *Self, struct cfg::DeleteGroup *Args)
{
   if ((not Args) or Args->Group.empty()) return ERR::NullArgs;

   for (auto it = Self->Groups.begin(); it != Self->Groups.end(); it++) {
      if (it->first IS Args->Group) {
         Self->Groups.erase(it);
         return ERR::Okay;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Flush: Diverts to #SaveSettings().
-END-
*********************************************************************************************************************/

static ERR CONFIG_Flush(extConfig *Self)
{
   return CONFIG_SaveSettings(Self);
}

/*********************************************************************************************************************

-METHOD-
GetGroupFromIndex: Converts an index number into its matching group string.

Use GetGroupFromIndex() to convert a group index number to its matching name.

-INPUT-
int Index: The group index that you want to identify.
&strview Group: Refers to the group string that matches the index number.

-ERRORS-
Okay
Args
OutOfRange: The index number is out of range of the available groups.
NoData: There is no data loaded into the config object.

-TAGS-
pure-query, object-owns-result, null-terminated-result

*********************************************************************************************************************/

static ERR CONFIG_GetGroupFromIndex(extConfig *Self, struct cfg::GetGroupFromIndex *Args)
{
   kt::Log log;

   if ((not Args) or (Args->Index < 0)) return log.warning(ERR::Args);

   if ((Args->Index >= 0) and (Args->Index < (int)Self->Groups.size())) {
      Args->Group = std::string_view(Self->Groups[Args->Index].first);
      return ERR::Okay;
   }
   else return log.warning(ERR::OutOfRange);
}

//********************************************************************************************************************

static ERR CONFIG_Init(extConfig *Self)
{
   kt::Log log;

   if ((Self->Flags & CNF::NEW) != CNF::NIL) return ERR::Okay; // Do not load any data even if the path is defined.

   ERR error = ERR::Okay;
   if (not Self->Path.empty()) {
      error = parse_file(Self, Self->Path);
      if (!error) {
         apply_filters(Self);
      }
   }

   if ((Self->Flags & CNF::AUTO_SAVE) != CNF::NIL) Self->CRC = calc_crc(Self); // Store the CRC in advance of any changes
   return error;
}

/*********************************************************************************************************************

-METHOD-
Merge: Merges two config objects together.

The Merge() method is used to merge configuration data from one config object provided as a source, into the target
object.  Existing data in the target will be overwritten by the source in cases where there matching set of group
keys.

-INPUT-
obj Source: The Config object to be merged.

-ERRORS-
Okay
NullArgs
AccessObject: The source configuration object could not be accessed.
Args

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR CONFIG_Merge(extConfig *Self, struct cfg::Merge *Args)
{
   if ((not Args) or (not Args->Source)) return ERR::NullArgs;
   if (Args->Source->classID() != CLASSID::CONFIG) return ERR::Args;

   auto src = (extConfig *)Args->Source;
   merge_groups(Self->Groups, src->Groups);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
MergeFile: Merges a configuration file into existing configuration data.

The MergeFile() method is used to pull configuration data from a file and merge it into the target config object.
The path to the configuration file is all that is required.  Existing data in the target will be overwritten by the
source in cases where there matching set of group keys.

-INPUT-
strview Path: The location of the configuration file that you want to merge.

-ERRORS-
Okay
NullArgs
File: Failed to load the source file.

-TAGS-
blocking, mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR CONFIG_MergeFile(extConfig *Self, struct cfg::MergeFile *Args)
{
   kt::Log log;

   if ((not Args) or Args->Path.empty()) return log.warning(ERR::NullArgs);

   log.branch("%.*s", int(Args->Path.size()), Args->Path.data());

   extConfig::create src = { fl::Path(Args->Path) };

   if (src.ok()) {
      merge_groups(Self->Groups, src->Groups);
      return ERR::Okay;
   }
   else return ERR::File;
}

/*********************************************************************************************************************

-METHOD-
ReadValue: Reads a key-value string.

This function retrieves key values in their original string format.  On success, the resulting string remains valid
only for as long as the client has exclusive access to the config object.  The pointer can also be invalidated
if more information is written to the config object.  For this reason, consider copying the result if it will be
used extensively.

If the `Group` parameter is empty, the scan routine will treat all of the config data as a one dimensional array.
If the `Key` parameter is empty then the first key in the requested group is returned.  If both parameters are empty
then the first known key value will be returned.

-INPUT-
strview Group: The name of a group to examine for a key.  If empty, all groups are scanned.
strview Key: The name of a key to retrieve (case sensitive).  If empty, the first key in the group is returned.
&strview Data: The key value will be stored in this parameter on return.

-ERRORS-
Okay
NullArgs
Search: The requested configuration entry does not exist.

-TAGS-
pure-query, object-owns-result, null-terminated-result
-END-

*********************************************************************************************************************/

static ERR CONFIG_ReadValue(extConfig *Self, struct cfg::ReadValue *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   for (auto & [group, keys] : Self->Groups) {
      if ((not Args->Group.empty()) and (group != Args->Group)) continue;

      if (Args->Key.empty()) {
         if (keys.empty()) Args->Data = "";
         else Args->Data = keys.cbegin()->second;
         return ERR::Okay;
      }
      else if (auto it = keys.find(Args->Key); it != keys.end()) {
         Args->Data = it->second;
         return ERR::Okay;
      }
   }

   log.trace("Could not find key %.*s : %.*s.", int(Args->Group.size()), Args->Group.data(), int(Args->Key.size()), Args->Key.data());
   Args->Data = std::string_view{};
   return ERR::Search;
}

/*********************************************************************************************************************

-ACTION-
SaveSettings: Saves data to the file that the configuration data was loaded from.

This action will save the configuration data back to its original file source (assuming the #Path remains unchanged).

*********************************************************************************************************************/

static ERR CONFIG_SaveSettings(extConfig *Self)
{
   kt::Log log;
   log.branch();

   auto crc = calc_crc(Self);
   if (((Self->Flags & CNF::AUTO_SAVE) != CNF::NIL) and (crc IS Self->CRC)) return ERR::Okay;

   if (not Self->Path.empty()) {
      objFile::create file = {
         fl::Path(Self->Path), fl::Flags(FL::WRITE|FL::NEW), fl::Permissions(PERMIT::NIL)
      };

      if (file.ok()) {
         if (!Self->saveToObject(*file)) Self->CRC = crc;
         return ERR::Okay;
      }
      else return ERR::File;
   }
   else return ERR::MissingPath;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves configuration data to an object, using standard config text format.
-END-
*********************************************************************************************************************/

static ERR CONFIG_SaveToObject(extConfig *Self, struct acSaveToObject *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Dest)) return log.warning(ERR::NullArgs);

   log.branch("Saving %d groups to object #%d.", (int)Self->Groups.size(), Args->Dest->UID);

   std::string buffer;
   buffer.reserve(256);

   for (auto & [group, keys] : Self->Groups) {
      buffer.clear();
      buffer += "\n[" + group + "]\n";
      acWrite(Args->Dest, buffer.c_str(), buffer.size(), nullptr);

      for (auto & [k, v] : keys) {
         buffer.clear();
         buffer += k + " = " + v + '\n';
         acWrite(Args->Dest, buffer.c_str(), buffer.size(), nullptr);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Set: Sets keys in existing config groups (aborts if the group does not exist).

This method is identical to #WriteValue() except it will abort if the name of the referred group does not exist in the
config object.  The error code `ERR::Search` is returned if this is the case.  Please refer to #WriteValue() for further
information on the behaviour of this function.

-INPUT-
strview Group: The name of the group.  Wildcards are supported.
strview Key:  The name of the key.
strview Data: The data that will be added to the given group/key.

-ERRORS-
Okay
NullArgs
Search: The referred group does not exist.

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR CONFIG_Set(extConfig *Self, struct cfg::Set *Args)
{
   if (not Args) return ERR::NullArgs;
   if (Args->Group.empty()) return ERR::NullArgs;
   if (Args->Key.empty()) return ERR::NullArgs;

   auto group = find_group_wild(Self, Args->Group);
   if (group) {
      if (auto it = group->second.find(Args->Key); it != group->second.end()) it->second.assign(Args->Data);
      else group->second.emplace(Args->Key, Args->Data);
      return ERR::Okay;
   }
   else return ERR::Search;
}

/*********************************************************************************************************************

-METHOD-
SortByKey: Sorts config data using a sequence of sort instructions.

The SortByKey() method sorts the groups of a config object by key values (the named key value should be present in
every group).

-INPUT-
strview Key: The name of the key to sort on.
int Descending: Set to true if a descending sort is required.

-ERRORS-
Okay
NoData

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR CONFIG_SortByKey(extConfig *Self, struct cfg::SortByKey *Args)
{
   if ((not Args) or Args->Key.empty()) { // Sort by group name if no args or key provided.
      std::sort(Self->Groups.begin(), Self->Groups.end(),
         [](const ConfigGroup &a, const ConfigGroup &b ) {
         return a.first < b.first;
      });

      return ERR::Okay;
   }

   kt::Log log;

   log.branch("Key: %.*s, Descending: %d", int(Args->Key.size()), Args->Key.data(), Args->Descending);

   auto descending = Args->Descending;
   std::sort(Self->Groups.begin(), Self->Groups.end(),
      [Args, descending](const ConfigGroup &A, const ConfigGroup &B) {
      auto a_value = sort_key_value(A, Args->Key);
      auto b_value = sort_key_value(B, Args->Key);
      return descending ? (a_value > b_value) : (a_value < b_value);
   });

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
WriteValue: Adds new entries to config objects.

Use the WriteValue() method to add or update information in a config object.  A `Group` name, `Key` name, and `Data`
value are required.  If the `Group` and `Key` arguments match an existing entry in the config object, the data of
that entry will be replaced with the new Data value.

-INPUT-
strview Group: The name of the group.
strview Key:   The name of the key.
strview Data:  The data that will be added to the given group/key.

-ERRORS-
Okay
NullArgs
Args
AllocMemory: The additional memory required for the new entry could not be allocated.

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR CONFIG_WriteValue(extConfig *Self, struct cfg::WriteValue *Args)
{
   kt::Log log;

   if ((not Args) or Args->Group.empty() or Args->Key.empty()) return log.warning(ERR::NullArgs);

   log.trace("%.*s.%.*s = %.*s", int(Args->Group.size()), Args->Group.data(), int(Args->Key.size()),
      Args->Key.data(), int(Args->Data.size()), Args->Data.data());

   return Self->write(Args->Group, Args->Key, Args->Data);
}

/*********************************************************************************************************************

-FIELD-
Data: Reference to the raw data values.

This field points to a C++ type that contains all key-values for the config object.  It is intended to be used only
by system code that is included with the standard framework.

*********************************************************************************************************************/

static ERR GET_Data(extConfig *Self, ConfigGroups **Value)
{
   *Value = &Self->Groups;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags may be set here.

-FIELD-
KeyFilter: Set this field to enable key filtering.

When dealing with large configuration files it may be useful to filter out groups of key-values that are not needed.
The KeyFilter field allows simple filters to be defined that will perform this task for you.  It is recommended that it
is set prior to parsing new data for best performance, but can be set or changed at any time to apply a new filter.

Key filters are created in the format `[Key] = [Data1], [Data2], ...`.  For example:

<pre>
Group = Sun, Light
Path = documents:
Name = Kōtuku
</pre>

Filters can be inversed by prefixing the key with the `!` character.

To create a filter based on group names, refer to the #GroupFilter field.

*********************************************************************************************************************/

static ERR GET_KeyFilter(extConfig *Self, std::string_view &Value)
{
   if (not Self->KeyFilter.empty()) {
      Value = Self->KeyFilter;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_KeyFilter(extConfig *Self, std::string_view &Value)
{
   Self->KeyFilter = Value;
   if (Self->initialised()) apply_key_filter(Self, Self->KeyFilter);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
GroupFilter: Set this field to enable group filtering.

When dealing with large configuration files, filtering out unrelated data may be useful.  By setting the GroupFilter
field, it is possible to filter out entire groups that don't match the criteria.

Group filters are created in CSV format, i.e. `GroupA, GroupB, GroupC, ...`.

The filter can be reversed so that only the groups matching your criteria are filtered out.  To do this, prefix the
CSV list with the `!` character.

To create a filter based on key names, refer to the #KeyFilter field.

*********************************************************************************************************************/

static ERR GET_GroupFilter(extConfig *Self, std::string_view &Value)
{
   if (not Self->GroupFilter.empty()) {
      Value = Self->GroupFilter;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_GroupFilter(extConfig *Self, std::string_view &Value)
{
   Self->GroupFilter.assign(Value);
   if (Self->initialised()) apply_group_filter(Self, Self->GroupFilter);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Path: Set this field to the location of the source configuration file.

*********************************************************************************************************************/

static ERR SET_Path(extConfig *Self, std::string_view &Value)
{
   Self->Path.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
TotalGroups: Returns the total number of groups in a config object.

*********************************************************************************************************************/

static ERR GET_TotalGroups(extConfig *Self, int *Value)
{
   *Value = Self->Groups.size();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TotalKeys: The total number of key values loaded into the config object.
-END-

*********************************************************************************************************************/

static ERR GET_TotalKeys(extConfig *Self, int *Value)
{
   int total = 0;
   for (const auto & [group, keys] : Self->Groups) {
      total += keys.size();
   }
   *Value = total;
   return ERR::Okay;
}

//********************************************************************************************************************
// Checks the next line in a buffer to see if it is a valid key.

static bool check_for_key(std::string_view Data)
{
   if (Data.empty()) return false;

   char first = Data.front();
   if ((first IS '\n') or (first IS '\r') or (first IS '[') or (first IS '#')) return false;

   // Only search for '=' within the current line
   auto line_end = Data.find_first_of("\n\r");
   auto eq_pos = Data.find('=');

   return (eq_pos != std::string_view::npos) and ((line_end IS std::string_view::npos) or (eq_pos < line_end));
}

//********************************************************************************************************************

static void merge_groups(ConfigGroups &Dest, ConfigGroups &Source)
{
   for (auto & [src_group, src_keys] : Source) {
      bool processed = false;

      // Check if the group already exists and merge the keys

      for (auto & [dest_group, dest_keys] : Dest) {
         if (dest_group IS src_group) {
            processed = true;
            for (auto & [k, v] : src_keys) {
               dest_keys[k] = v;
            }
         }
      }

      if (not processed) { // New group to be added
         auto &new_group = Dest.emplace_back();
         new_group.first  = src_group;
         new_group.second = src_keys;
      }
   }
}

//********************************************************************************************************************

static std::string_view sort_key_value(const ConfigGroup &Group, std::string_view Key)
{
   if (auto it = Group.second.find(Key); it != Group.second.end()) return it->second;
   else return {};
}

//********************************************************************************************************************

static void apply_filters(extConfig *Self)
{
   if (not Self->GroupFilter.empty()) apply_group_filter(Self, Self->GroupFilter);
   if (not Self->KeyFilter.empty()) apply_key_filter(Self, Self->KeyFilter);
}

//********************************************************************************************************************

static FilterConfig parse_filter(std::string_view Filter, bool KeyValue = false)
{
   constexpr std::string_view WHITESPACE = " \t\n\r";
   FilterConfig f;

   if (Filter.starts_with('!')) {
      f.reverse = true;
      Filter.remove_prefix(1);
   }

   // Trim leading whitespace

   if (auto pos = Filter.find_first_not_of(WHITESPACE); pos != std::string_view::npos) {
      Filter.remove_prefix(pos);
   }
   else Filter = {};

   if (KeyValue) {
      auto eq_pos = Filter.find('=');
      if (eq_pos IS std::string_view::npos) return f;

      // Extract and trim the key name
      auto name_part = Filter.substr(0, eq_pos);
      if (auto end = name_part.find_last_not_of(WHITESPACE); end != std::string_view::npos) {
         f.name = name_part.substr(0, end + 1);
      }

      // Move past '=' and trim whitespace
      Filter.remove_prefix(eq_pos + 1);
      if (auto pos = Filter.find_first_not_of(WHITESPACE); pos != std::string_view::npos) {
         Filter.remove_prefix(pos);
      }
      else Filter = {};
   }

   // Parse comma-separated values

   while (not Filter.empty()) {
      auto comma_pos = Filter.find(',');
      auto value = (comma_pos IS std::string_view::npos) ? Filter : Filter.substr(0, comma_pos);

      // Trim trailing whitespace from value
      if (auto end = value.find_last_not_of(WHITESPACE); end != std::string_view::npos) {
         f.values.emplace_back(value.substr(0, end + 1));
      }
      else if (not value.empty()) f.values.emplace_back(value);

      if (comma_pos IS std::string_view::npos) break;

      Filter.remove_prefix(comma_pos + 1);

      // Trim leading whitespace for next value

      if (auto pos = Filter.find_first_not_of(WHITESPACE); pos != std::string_view::npos) {
         Filter.remove_prefix(pos);
      }
      else break;
   }

   f.valid = true;
   return f;
}

//********************************************************************************************************************

static ERR parse_config(extConfig *Self, std::string_view Buffer)
{
   constexpr std::string_view WHITESPACE = " \t\n\r";
   kt::Log log(__FUNCTION__);

   if (Buffer.empty()) return ERR::NoData;

   log.traceBranch("%.*s", int(std::min(Buffer.size(), size_t(20))), Buffer.data());

   std::string group_name;
   auto data = next_group(Buffer, group_name); // Find the first group

   while (!data.empty()) {
      // Skip leading whitespace
      if (auto pos = data.find_first_not_of(WHITESPACE); pos != std::string_view::npos) {
         data.remove_prefix(pos);
      }
      else break;

      if (data.front() IS '#') { // Commented
         data = next_line(data);
         continue;
      }

      std::pair<std::string, KEYVALUE> *current_group = nullptr;
      while ((not data.empty()) and (data.front() != '[')) { // Keep processing keys until either a new group or EOF is reached
         if (check_for_key(data)) {
            // Find the '=' separator
            auto eq_pos = data.find('=');
            if (eq_pos IS std::string_view::npos) break;

            // Extract and trim the key
            auto key_part = data.substr(0, eq_pos);
            if (auto end = key_part.find_last_not_of(WHITESPACE); end != std::string_view::npos) {
               key_part = key_part.substr(0, end + 1);
            }
            std::string key(key_part);

            // Move past '=' and skip whitespace (including newlines for multiline support)
            data.remove_prefix(eq_pos + 1);
            if (auto pos = data.find_first_not_of(WHITESPACE); pos != std::string_view::npos) {
               data.remove_prefix(pos);
            }

            std::string value;
            if (((Self->Flags & CNF::STRIP_QUOTES) != CNF::NIL) and (!data.empty()) and (data.front() IS '"')) {
               data.remove_prefix(1);
               auto end_quote = data.find('"');
               if (end_quote != std::string_view::npos) {
                  value = data.substr(0, end_quote);
                  data.remove_prefix(end_quote);
               }
            }
            else {
               auto line_end = data.find_first_of("\n\r");
               if (line_end IS std::string_view::npos) line_end = data.size();
               value = data.substr(0, line_end);
               data.remove_prefix(line_end);
            }
            data = next_line(data);

            if (not current_group) { // Check if a matching group already exists before creating a new one
               current_group = find_group(Self, group_name);
               if (not current_group) {
                  current_group = &Self->Groups.emplace_back();
                  current_group->first = group_name;
               }
            }
            current_group->second[std::move(key)] = std::move(value);
         }
         else data = next_line(data);
      }
      data = next_group(data, group_name);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void apply_key_filter(extConfig *Self, std::string_view Filter)
{
   kt::Log log(__FUNCTION__);

   if (Filter.empty()) return;

   log.branch("Filter: %.*s", int(Filter.size()), Filter.data());

   FilterConfig f = parse_filter(Filter, true);
   if (not f.valid) return;

   for (auto group = Self->Groups.begin(); group != Self->Groups.end(); ) {
      bool matched = (f.reverse) ? true : false;
      for (auto & [k, v] : group->second) {
         if (iequals(f.name, k)) {
            for (auto const &cmp : f.values) {
               if (iequals(cmp, v)) {
                  matched = f.reverse ? false : true;
                  break;
               }
            }
            break;
         }
      }

      if (not matched) group = Self->Groups.erase(group);
      else group++;
   }
}

//********************************************************************************************************************

static void apply_group_filter(extConfig *Self, std::string_view Filter)
{
   kt::Log log(__FUNCTION__);

   if (Filter.empty()) return;

   log.branch("Filter: %.*s", int(Filter.size()), Filter.data());

   FilterConfig f = parse_filter(Filter, false);
   if (not f.valid) return;

   for (auto group = Self->Groups.begin(); group != Self->Groups.end(); ) {
      bool matched = f.reverse ? true : false;
      for (auto const &cmp : f.values) {
         if (cmp IS group->first) {
            matched = f.reverse ? false : true;
            break;
         }
      }

      if (not matched) group = Self->Groups.erase(group);
      else group++;
   }
}

//********************************************************************************************************************
// Returns the key-values for a group, given a group name.  Supports wild-cards.

static ConfigGroup * find_group_wild(extConfig *Self, std::string_view Group)
{
   if (Group.empty()) return nullptr;

   for (auto &group : Self->Groups) {
      if (wildcmp(Group, group.first)) return &group;
   }

   return nullptr;
}

//********************************************************************************************************************

#include "class_config_def.c"

static const FieldArray clFields[] = {
   { "Path",        FDF_CPPSTRING|FDF_RW, nullptr, SET_Path },
   { "KeyFilter",   FDF_CPPSTRING|FDF_RW|FDF_PURE, GET_KeyFilter, SET_KeyFilter },
   { "GroupFilter", FDF_CPPSTRING|FDF_RW|FDF_PURE, GET_GroupFilter, SET_GroupFilter },
   { "Flags",       FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clConfigFlags },
   // Virtual fields
   { "Data",        FDF_POINTER|FDF_R|FDF_PURE, GET_Data },
   { "TotalGroups", FDF_INT|FDF_R|FDF_PURE, GET_TotalGroups },
   { "TotalKeys",   FDF_INT|FDF_R|FDF_PURE, GET_TotalKeys },
   END_FIELD
};

//********************************************************************************************************************

extern ERR add_config_class(void)
{
   glConfigClass = extMetaClass::create::global(
      fl::BaseClassID(CLASSID::CONFIG),
      fl::ClassVersion(VER_CONFIG),
      fl::Name("Config"),
      fl::Category(CCF::DATA),
      fl::FileExtension("cfg|cnf|config"),
      fl::FileDescription("Config File"),
      fl::Icon("filetypes/text"),
      fl::Actions(clConfigActions),
      fl::Methods(clConfigMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extConfig)),
      fl::Path("modules:core"));

   return glConfigClass ? ERR::Okay : ERR::AddClass;
}
