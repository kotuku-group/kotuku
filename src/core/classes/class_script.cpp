/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Script: The Script class defines a common interface for script execution.

The Script class defines a common interface for the purpose of executing scripts, such as Tiri.  The base class does
not include a default parser or execution process of any kind.

To execute a script file, choose a derived class that matches the language and create the script object.  Set the #Path
field and then #Activate() the script.  Global input parameters for the script can be defined via the #SetKey()
action.

Note that client scripts may sometimes create objects that are unmanaged by the script object that created them.
Terminating the script will not remove objects that are outside its resource hierarchy.
-END-

*********************************************************************************************************************/

#define PRV_SCRIPT
#include "../defs.h"
#include <kotuku/main.h>
#include <kotuku/vector.hpp>

inline std::string_view check_bom(std::string_view Value)
{
   if ((Value.size() >= 3) and (uint8_t(Value[0]) IS 0xef) and (uint8_t(Value[1]) IS 0xbb) and
       (uint8_t(Value[2]) IS 0xbf)) return Value.substr(3); // UTF-8 BOM
   else if ((Value.size() >= 2) and (uint8_t(Value[0]) IS 0xfe) and (uint8_t(Value[1]) IS 0xff)) {
      return Value.substr(2); // UTF-16 BOM big endian
   }
   else if ((Value.size() >= 2) and (uint8_t(Value[0]) IS 0xff) and (uint8_t(Value[1]) IS 0xfe)) {
      return Value.substr(2); // UTF-16 BOM little endian
   }
   return Value;
}

/*********************************************************************************************************************
-ACTION-
Activate: Executes the script.
-END-
*********************************************************************************************************************/

static ERR SCRIPT_Activate(objScript *Self)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************
-ACTION-
DataFeed: Script source code can be passed to the object as XML or text via data feeds.
-END-
*********************************************************************************************************************/

static ERR SCRIPT_DataFeed(objScript *Self, struct acDataFeed *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Args->Datatype IS DATA::XML) {
      Self->setStatement((STRING)Args->Buffer);
   }
   else if (Args->Datatype IS DATA::TEXT) {
      Self->setStatement((STRING)Args->Buffer);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Callback: An internal method for managing callbacks.

Not for client use.

-INPUT-
large ProcedureID: An identifier for the target procedure.
cstruct(*ScriptArg) Args: Optional CSV string containing parameters to pass to the procedure.
int TotalArgs: The total number of parameters in the Args parameter.
&error Error: The error code returned from the script, if any.

-ERRORS-
Okay:
Args:

-TAGS-
mutates-object, callback-inlines, private

-END-

*********************************************************************************************************************/

static ERR SCRIPT_Callback(objScript *Self, struct sc::Callback *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if ((Args->TotalArgs < 0) or (Args->TotalArgs > 1024)) return log.warning(ERR::Args);

   auto save_id      = Self->ProcedureID;
   auto save_name    = std::move(Self->Procedure);
   Self->ProcedureID = Args->ProcedureID;
   Self->Procedure.clear();

   const ScriptArg *save_args = Self->ProcArgs;
   Self->ProcArgs  = Args->Args;

   auto save_total  = Self->TotalArgs;
   Self->TotalArgs  = Args->TotalArgs;
   auto saved_error = Self->Error;
   auto saved_error_msg = std::move(Self->ErrorMessage);
   Self->ErrorMessage.clear();
   Self->Error       = ERR::Okay;

   ERR error = acActivate(Self);

   Args->Error = Self->Error;
   Self->Error = saved_error;
   Self->ProcedureID = save_id;
   Self->Procedure   = std::move(save_name);
   Self->ProcArgs    = save_args;
   Self->TotalArgs   = save_total;
   Self->ErrorMessage = std::move(saved_error_msg);

   return error;
}

/*********************************************************************************************************************

-METHOD-
DebugLog: Acquire a debug log from a compiled Script.

Use the DebugLog() method to acquire debug information from a compiled script.  The exact nature of the log
will depend on the scripting language in use, but will typically dump readable bytecode for analysis.  The Options
parameter is a comma-separated list that may be used to pass language-specific options to the underlying
implementation.

The resulting log information is written to the caller-provided `Result` string.

-INPUT-
strview Options: Options to pass to the underlying language.
^&string Result: Resulting log information.

-ERRORS-
Okay:
NullArgs:

*********************************************************************************************************************/

static ERR SCRIPT_DebugLog(objScript *Self, struct sc::DebugLog *Args)
{
   // It is the responsibility of the derived class to override this method with something appropriate.
   if ((not Args) or (not Args->Result)) return ERR::NullArgs;
   Args->Result->clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DerefProcedure: Dereferences an acquired procedure.

This method will release a procedure reference that has been acquired through #GetProcedureID().  It is only necessary
to make this call if the scripting language is managing function references as a keyed resource.  Tiri is one such
language.  Languages that do not manage functions as a resource will ignore calls to this method.

Note that acquiring a procedure reference and then failing to release it can result in the reference remaining in
memory until the Script is terminated.  There may also be unforeseen consequences in the garbage collection process.

-INPUT-
ptr(func) Procedure: The procedure to be dereferenced.

-ERRORS-
Okay:
NullArgs:

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR SCRIPT_DerefProcedure(objScript *Self, struct sc::DerefProcedure *Args)
{
   // It is the responsibility of the derived class to override this method with something appropriate.
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Exec: Executes a procedure in the script.

Use the Exec() method to execute a named procedure in a script, optionally passing that procedure a series of
parameters.

The behaviour of this process matches that of the #Activate() action and will return the same error codes in the
event of failure.  If the `Procedure` returns results, they will be available from the #Results field after execution.

Parameter values must be specified as an array of ScriptArg structures.  The following example illustrates:

<pre>
struct ScriptArg args[] = {
   { "Object",       FD_OBJECTID, { .Int = Self->UID } },
   { "Output",       FD_PTR,      { .Address = output } },
   { "OutputLength", FD_INT,      { .Int = len } }
};
</>

The ScriptArg structure follows this arrangement:

<pre>
struct ScriptArg {
   STRING Name;
   int Type;
   union {
      APTR    Address;
      int     Int;
      int64_t Int64;
      double  Double;
   };
};
</>

The Field Descriptor `FD` specified in the `Type` must be a match to whatever value is defined in the union.  For instance
if the `Int` field is defined then an `FD_INT` `Type` must be used.  Supplementary field definition information, e.g.
`FD_OBJECT`, may be used to assist in clarifying the type of the value that is being passed.  Field Descriptors are
documented in detail in the Kotuku Wiki.

-INPUT-
strview Procedure: The name of the procedure to execute, or leave empty for the default entry point.
cstruct(*ScriptArg) Args: Optional parameters to pass to the procedure.
int TotalArgs: Total number of `Args` provided.

-ERRORS-
Okay: The procedure was executed.
NullArgs
Args: The `TotalArgs` value is invalid.

-TAGS-
mutates-object, callback-inlines
-END-

*********************************************************************************************************************/

static ERR SCRIPT_Exec(objScript *Self, struct sc::Exec *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if ((Args->TotalArgs < 0) or (Args->TotalArgs > 32)) return log.warning(ERR::Args);

   auto save_id = Self->ProcedureID;
   auto save_name = std::move(Self->Procedure);
   Self->ProcedureID = 0;
   Self->Procedure = Args->Procedure;

   const ScriptArg *save_args = Self->ProcArgs;
   Self->ProcArgs  = Args->Args;

   auto save_total = Self->TotalArgs;
   Self->TotalArgs = Args->TotalArgs;

   ERR error = acActivate(Self);

   Self->ProcedureID = save_id;
   Self->Procedure   = std::move(save_name);
   Self->ProcArgs    = save_args;
   Self->TotalArgs   = save_total;

   return error;
}

//********************************************************************************************************************

static ERR SCRIPT_Free(objScript *Self)
{
   Self->~objScript();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetProcedureID: Converts a procedure name to an ID.

This method will convert a procedure name to a unique reference within the script, if such a procedure exists.  The
ID can be used by the client to create new `FUNCTION` definitions, for example:

<pre>
FUNCTION callback;
SET_FUNCTION_SCRIPT(callback, script, procedure_id);
</pre>

Resolving a procedure will often result in the Script maintaining an ongoing reference for it.  To discard the
reference, call #DerefProcedure() once access to the procedure is no longer required.  Alternatively,
destroying the script will also dereference all procedures.

-INPUT-
strview Procedure:   The name of the procedure.
&large ProcedureID: The computed ID will be returned in this parameter.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object, creates-resource
-END-

*********************************************************************************************************************/

static ERR SCRIPT_GetProcedureID(objScript *Self, struct sc::GetProcedureID *Args)
{
   if ((not Args) or Args->Procedure.empty()) return kt::Log().warning(ERR::NullArgs);
   Args->ProcedureID = strihash(Args->Procedure);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
GetKey: Script parameters can be retrieved through this action.
-END-
*********************************************************************************************************************/

static ERR SCRIPT_GetKey(objScript *Self, struct acGetKey *Args)
{
   if ((not Args) or (not Args->Value)) return ERR::NullArgs;

   if (auto it = Self->Vars.find(Args->Key); it != Self->Vars.end()) {
      Args->Value->assign(it->second);
      return ERR::Okay;
   }
   else {
      Args->Value->clear();
      return ERR::UnsupportedField;
   }
}

//********************************************************************************************************************

static ERR SCRIPT_Init(objScript *Self)
{
   if (not Self->TargetID) { // Define the target if it has not been set already
      kt::Log().detail("Target not set, defaulting to owner #%d.", Self->ownerID());
      Self->TargetID = Self->ownerID();
   }

   if (Self->isDerived()) return ERR::Okay; // Break here to let the derived class continue initialisation

   return ERR::NoSupport;
}

//********************************************************************************************************************

static ERR SCRIPT_NewPlacement(objScript *Self)
{
   new (Self) objScript;

   Self->CurrentLine = -1;
   return ERR::Okay;
}

// If reset, the script will be reloaded from the original file location the next time an activation occurs.  All
// parameters are also reset.

static ERR SCRIPT_Reset(objScript *Self)
{
   Self->Vars.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SetKey: Script parameters can be set through this action.
-END-
*********************************************************************************************************************/

static ERR SCRIPT_SetKey(objScript *Self, struct acSetKey *Args)
{
   // It is acceptable to set zero-length string values (this has its uses in some scripts).

   if ((not Args) or (Args->Key.empty())) return ERR::NullArgs;

   kt::Log().trace("%.*s = %.*s", int(Args->Key.size()), Args->Key.data(), int(Args->Value.size()), Args->Value.data());

   auto key_it = Self->Vars.lower_bound(Args->Key);
   if ((key_it != Self->Vars.end()) and (not Self->Vars.key_comp()(Args->Key, key_it->first))) {
      key_it->second.assign(Args->Value);
   }
   else Self->Vars.emplace_hint(key_it, std::string(Args->Key), std::string(Args->Value));

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CacheFile: Compilable script languages can be compiled to a cache file.

Scripts that support compilation of the source code can be compiled to a target file when the script is initialised.
This file is then used as a cache, so that if the cache file exists on the next initialisation then the cache
file is used instead of the original source code.

If the cache file exists, a determination on whether the source code has been edited is usually made by comparing
date stamps on the original and cache files.

*********************************************************************************************************************/

static ERR GET_CacheFile(objScript *Self, std::string_view &Value)
{
   Value = Self->CacheFile;
   return Self->CacheFile.empty() ? ERR::FieldNotSet : ERR::Okay;
}

static ERR SET_CacheFile(objScript *Self, std::string_view &Value)
{
   Self->CacheFile.assign(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CurrentLine: Indicates the current line being executed when in debug mode.

In debug mode, the CurrentLine will indicate the current line of execution (according to the original source code for
the script).

It should be noted that not all script processors will support this feature, in which case the value for this field
will be set to -1.

-FIELD-
Error: If a script fails during execution, an error code may be readable here.

On execution of a script, the Error value is reset to ERR::Okay and will be updated if the script fails.  Be mindful
that if a script is likely to be executed recursively then the first thrown error will have priority and be
propagated through the call stack.

-FIELD-
ErrorMessage: A human readable error string may be declared here following a script execution failure.

-FIELD-
Flags: Optional flags.
Lookup: SCF

-FIELD-
LineOffset: For debugging purposes, this value is added to any message referencing a line number.

The LineOffset is a value that is added to all line numbers that are referenced in script debugging output.  It is
primarily intended for internal usage only.

-FIELD-
Path: The location of a script file to be loaded.

A script file can be loaded by setting the Path to its location.  The path must be defined prior to the initialisation
process, or alternatively the client can define the #Statement field.

Optional parameters can also be passed to the script via the Path string.  The name of a function is passed first,
surrounded by semicolons.  Arguments can be passed to the function by appending them as a CSV list.  The following
string illustrates the format used: `dir:location;procedure;arg1=val1,arg2,arg3=val2`

A target for the script may be specified by using the 'target' parameter in the parameter list (value must refer to a
valid existing object).
-END-

*********************************************************************************************************************/

static ERR GET_Path(objScript *Self, std::string_view &Value)
{
   Value = Self->Path;
   return ERR::Okay;
}

static ERR SET_Path(objScript *Self, std::string_view &Value)
{
   if (not Self->Path.empty()) {
      // If the location has already been set, throw the value to SetKey instead.

      if (not Value.empty()) return acSetKey(Self, "Path", std::string(Value).c_str());
   }
   else {
      Self->Path.clear();
      Self->String.clear();
      Self->WorkingPath.clear();

      int i;
      if (not Value.empty()) {
         auto len = Value.find(';');
         if (len IS std::string_view::npos) len = Value.size();

         if (Value.substr(0, len).starts_with("STRING:")) {
            Self->String.assign(check_bom(Value.substr(7)));
            return ERR::Okay;
         }

         Self->Path.assign(Value, 0, len);
         {
            auto value = Value.data();
            auto value_size = int(Value.size());
            i = len;

            // If a semi-colon has been used, this indicates that a procedure follows the filename.

            if ((i < value_size) and (value[i] IS ';')) {
               i++;
               while ((i < value_size) and (unsigned(value[i]) <= 0x20)) i++;
               auto start = i, end = i;
               while ((end < value_size) and (unsigned(value[end]) > 0x20) and (value[end] != ';')) end++;
               if (end > start) {
                  std::string buffer;
                  buffer.append(value, start, end - start);
                  std::string_view procedure(buffer);
                  Self->Procedure.assign(procedure);
               }

               // Process optional parameters

               if ((end < value_size) and (value[end] IS ';')) {
                  char arg[100];

                  i = end + 1;
                  while (i < value_size) {
                     while ((i < value_size) and (unsigned(value[i]) <= 0x20)) i++;
                     while ((i < value_size) and (value[i] IS ',')) {
                        i++;
                        while ((i < value_size) and (unsigned(value[i]) <= 0x20)) i++;
                     }

                     // Extract arg name

                     int j;
                     for (j=0; (i < value_size) and (value[i] != ',') and (value[i] != '=') and
                          (unsigned(value[i]) > 0x20) and (j < int(sizeof(arg)) - 1); j++) arg[j] = value[i++];
                     arg[j] = 0;

                     while ((i < value_size) and (value[i] <= 0x20)) i++;

                     // Extract arg value

                     std::string argval("1");
                     if ((i < value_size) and (value[i] IS '=')) {
                        i++;
                        while ((i < value_size) and (unsigned(value[i]) <= 0x20)) i++;
                        if ((i < value_size) and (value[i] IS '"')) {
                           i++;
                           for (j=0; (i+j < value_size) and (value[i+j] != '"'); j++);
                           argval.assign(value, i, j);
                           i += j;
                           if ((i < value_size) and (value[i] IS '"')) i++;
                        }
                        else {
                           for (j=0; (i+j < value_size) and (value[i+j] != ','); j++);
                           argval.assign(value, i, j);
                           i += j;
                        }
                     }

                     if (iequals("target", arg)) Self->setTarget(strtol(argval.c_str(), nullptr, 0));
                     else acSetKey(Self, arg, argval.c_str());
                  }
               }
            }
         }
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Procedure: Specifies a procedure name to be executed.

Sometimes scripts are split into several procedures or functions that can be executed independently from the 'main'
area of the script.  If a loaded script contains procedures, the client can set the Procedure field to execute a
specific routine whenever the script is activated with the #Activate() action.

If this field is not set, the first procedure in the script, or the 'main' procedure (as defined by the script type) is
executed by default.

-FIELD-
Results: Stores multiple string results for languages that support this feature.

If a scripting language supports the return of multiple results, this field may reflect those result values after the
execution of any procedure.

For maximum compatibility in type conversion, the results are stored as an array of strings.

*********************************************************************************************************************/

static ERR GET_Results(objScript *Self, kt::vector<std::string> **Value, int *Elements)
{
   *Value = &Self->Results;
   *Elements = Self->Results.size();
   return ERR::Okay;
}

static ERR SET_Results(objScript *Self, const kt::vector<std::string> *Value, int Elements)
{
   if (Value) Self->Results = *Value;
   else Self->Results.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Statement: Scripts can be executed from any string passed into this field.

Scripts may be compiled into a script object by setting the Statement field with a complete script string.  This is
often convenient for embedding a small script into another script file without having to make external file references.
It is also commonly used for executing scripts that have been embedded into program binaries.

*********************************************************************************************************************/

static ERR GET_String(objScript *Self, std::string_view &Value)
{
   Value = Self->String;
   return ERR::Okay;
}

static ERR SET_String(objScript *Self, std::string_view &Value)
{
   Self->Path.clear(); // Path removed when a statement string is being set
   Self->String.assign(check_bom(Value));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Target: Reference to the default container that new script objects will be initialised to.

This field can refer to the target object that new objects at the root of the script will be initialised to.  If this
field is not set, the root-level objects in the script will be initialised to the script's owner.

-FIELD-
TotalArgs: Reflects the total number of parameters used in a script object.

The total number of parameters that have been set in a script object through the unlisted field mechanism are reflected
in the value of this field.
-END-
*********************************************************************************************************************/

static ERR GET_TotalArgs(objScript *Self, int *Value)
{
   *Value = Self->Vars.size();
   return ERR::Okay;
}

/*********************************************************************************************************************
PRIVATE: Variables
*********************************************************************************************************************/

static ERR GET_Variables(objScript *Self, KEYVALUE **Value)
{
   *Value = &Self->Vars;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
WorkingPath: Defines the script's working path (folder).

The working path for a script is defined here.  By default this is defined as the location from which the script was
loaded, without the file name.  If this cannot be determined then the working path for the parent process is used
(this is usually set to the location of the program).

The working path is always fully qualified with a slash or colon at the end of the string.

A client can manually change the working path by setting this field with a custom string.
-END-

*********************************************************************************************************************/

static ERR GET_WorkingPath(objScript *Self, std::string_view &Value)
{
   kt::Log log;

   if (Self->WorkingPath.empty()) {
      if (Self->Path.empty()) {
         log.warning("Script has no defined Path.");
         return ERR::MissingPath;
      }

      // Determine if an absolute path has been indicated

      bool path = false;
      if (Self->Path[0] IS '/') path = true;
      else if (auto j = Self->Path.find_first_of(":/\\"); (j != std::string::npos) and (Self->Path[j] IS ':')) {
         path = true;
      }

      auto j = Self->Path.find_last_of(":/\\");
      if (j != std::string::npos) j++;

      if (path) { // Extract absolute path
         kt::SwitchContext ctx(Self);
         Self->WorkingPath.assign(Self->Path, 0, j);
      }
      else {
         std::string_view working_path;
         if ((CurrentTask()->get(FID_Path, working_path) IS ERR::Okay) and (not working_path.empty())) {
            // Using ResolvePath() can help to determine relative paths such as "../path/file"

            std::string buf(working_path);
            if (j != std::string::npos) buf.append(Self->Path, 0, j);

            kt::SwitchContext ctx(Self);
            std::string rpath;
            if (ResolvePath(buf, RSF::APPROXIMATE, &rpath) IS ERR::Okay) {
               Self->WorkingPath = rpath;
            }
            else Self->WorkingPath = working_path;
         }
         else log.warning("No working path.");
      }
   }

   Value = Self->WorkingPath;
   return Self->WorkingPath.empty() ? ERR::FieldNotSet : ERR::Okay;
}

static ERR SET_WorkingPath(objScript *Self, std::string_view &Value)
{
   Self->WorkingPath.assign(Value);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_script_def.c"

static const FieldArray clScriptFields[] = {
   { "Procedure",    FDF_CPPSTRING|FDF_RW|FDF_PURE },
   { "Path",         FDF_CPPSTRING|FDF_RI|FDF_PURE, nullptr, SET_Path },
   { "ErrorMessage", FDF_CPPSTRING|FDF_RW|FDF_PURE },
   { "Target",       FDF_OBJECTID|FDF_RW },
   { "Flags",        FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clScriptFlags },
   { "Error",        FDF_INT|FDF_R },
   { "CurrentLine",  FDF_INT|FDF_R },
   { "LineOffset",   FDF_INT|FDF_RW },
   // Virtual Fields
   { "CacheFile",    FDF_CPPSTRING|FDF_RW|FDF_PURE,             GET_CacheFile, SET_CacheFile },
   { "WorkingPath",  FDF_CPPSTRING|FDF_RW,                      GET_WorkingPath, SET_WorkingPath },
   { "Results",      FDF_ARRAY|FDF_CPPSTRING|FDF_RW|FDF_PURE,   GET_Results, SET_Results },
   { "Src",          FDF_SYNONYM|FDF_CPPSTRING|FDF_RI|FDF_PURE, GET_Path, SET_Path },
   { "Statement",    FDF_CPPSTRING|FDF_RW|FDF_PURE,             GET_String, SET_String },
   { "String",       FDF_SYNONYM|FDF_CPPSTRING|FDF_RW|FDF_PURE, GET_String, SET_String },
   { "TotalArgs",    FDF_INT|FDF_R|FDF_PURE,                    GET_TotalArgs },
   { "Variables",    FDF_POINTER|FDF_SYSTEM|FDF_R|FDF_PURE,     GET_Variables },
   END_FIELD
};

//********************************************************************************************************************

extern ERR add_script_class(void)
{
   glScriptClass = extMetaClass::create::global(
      fl::ClassVersion(VER_SCRIPT),
      fl::Name("Script"),
      fl::Category(CCF::DATA),
      fl::Actions(clScriptActions),
      fl::Methods(clScriptMethods),
      fl::Fields(clScriptFields),
      fl::Size(sizeof(objScript)),
      fl::Icon("filetypes/source"),
      fl::Path("modules:core"));

   return glScriptClass ? ERR::Okay : ERR::AddClass;
}
