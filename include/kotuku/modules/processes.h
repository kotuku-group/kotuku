#pragma once

// Copyright: Paul Manias © 1996-2026
// Generator: idl-c

#include <kotuku/main.h>

class objTask;
class objThread;

// Thread flags

enum class THF : uint32_t {
   NIL = 0,
   AUTO_FREE = 0x00000001,
};

DEFINE_ENUM_FLAG_OPERATORS(THF)

// Task flags

enum class TSF : uint32_t {
   NIL = 0,
   WAIT = 0x00000001,
   RESET_PATH = 0x00000002,
   PRIVILEGED = 0x00000004,
   SHELL = 0x00000008,
   VERBOSE = 0x00000010,
   QUIET = 0x00000020,
   DETACHED = 0x00000040,
   ATTACHED = 0x00000080,
   PIPE = 0x00000100,
};

DEFINE_ENUM_FLAG_OPERATORS(TSF)

// Task class definition

#define VER_TASK (1.000000)

// Task methods

namespace task {
struct Expunge { static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct AddArgument { std::string_view Argument; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Quit { static const AC id = AC(-3); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct GetEnv { std::string_view Name; std::string *Value; static const AC id = AC(-4); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct SetEnv { std::string_view Name; std::string_view Value; static const AC id = AC(-5); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objTask : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::TASK;
   static constexpr CSTRING CLASS_NAME = "Task";

   using create = kt::Create<objTask>;
   objTask(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   std::string LaunchPath;                // Launched executables will start in the path specified here.
   std::string Name;                      // Name of the task.
   std::string Location;                  // Location of an executable file to launch.
   std::string Path;                      // The current working folder of the active process.
   std::string ProcessPath;               // The path of the executable that is associated with the task.
   double TimeOut;                        // Limits the amount of time to wait for a launched process to return.
   kt::vector<std::string> Parameters;    // Command line arguments (list format).
   TSF    Flags;                          // Optional flags.
   int    ReturnCode;                     // The task's return code can be retrieved following execution.
   int    ProcessID;                      // Reflects the process ID when an executable is launched.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR getKey(std::string_view Key, std::string &Value) noexcept {
      struct acGetKey args = { Key, &Value };
      auto error = Action(AC::GetKey, this, &args);
      if (error != ERR::Okay) Value.clear();
      return error;
   }
   inline ERR init() noexcept { return InitObject(this); }
   inline ERR query() noexcept { return Action(AC::Query, this, nullptr); }
   inline ERR acSetKey(std::string_view FieldName, std::string_view Value) noexcept {
      struct acSetKey args = { FieldName, Value };
      return Action(AC::SetKey, this, &args);
   }
   inline ERR write(std::span<const int8_t> Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { Buffer };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR write(std::string Buffer, int *Result = nullptr) noexcept {
      struct acWrite write = { std::span((const int8_t *)Buffer.data(), Buffer.size()) };
      if (auto error = Action(AC::Write, this, &write); error IS ERR::Okay) {
         if (Result) *Result = write.Result;
         return ERR::Okay;
      }
      else {
         if (Result) *Result = 0;
         return error;
      }
   }
   inline ERR expunge() noexcept {
      return Action(AC(-1), this, nullptr);
   }
   inline ERR addArgument(const std::string_view &Argument) noexcept {
      struct task::AddArgument args = { Argument };
      return Action(AC(-2), this, &args);
   }
   inline ERR quit() noexcept {
      return Action(AC(-3), this, nullptr);
   }
   inline ERR getEnv(const std::string_view &Name, std::string &Value) noexcept {
      struct task::GetEnv args = { Name, &Value };
      ERR error = Action(AC(-4), this, &args);
      return error;
   }
   inline ERR setEnv(const std::string_view &Name, const std::string_view &Value) noexcept {
      struct task::SetEnv args = { Name, Value };
      return Action(AC(-5), this, &args);
   }

   // Customised field getting

   inline ERR getLaunchPath(std::string_view &Value) noexcept {
      Value = this->LaunchPath;
      return ERR::Okay;
   }

   inline ERR getName(std::string_view &Value) noexcept {
      Value = this->Name;
      return ERR::Okay;
   }

   inline ERR getLocation(std::string_view &Value) noexcept {
      Value = this->Location;
      return ERR::Okay;
   }

   inline ERR getPath(std::string_view &Value) noexcept {
      Value = this->Path;
      return ERR::Okay;
   }

   inline ERR getProcessPath(std::string_view &Value) noexcept {
      Value = this->ProcessPath;
      return ERR::Okay;
   }

   inline ERR getTimeOut(double &Value) noexcept {
      Value = this->TimeOut;
      return ERR::Okay;
   }

   inline ERR getParameters(std::span<std::string> &Value) noexcept {
      auto ktv = (kt::vector<std::string> *)(((int8_t *)this) + CLASS_OFFSET + 168);
      Value = std::span<std::string>(ktv->data(), ktv->size());
      return ERR::Okay;
   }

   inline ERR getFlags(TSF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }

   inline ERR getReturnCode(int &Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getProcess(int &Value) noexcept {
      Value = this->ProcessID;
      return ERR::Okay;
   }

   inline ERR getActions(std::span<struct ActionEntry> &Value) noexcept {
      auto field = &this->Class->Dictionary[8];
      auto get_field = (ERR (*)(APTR, std::span<struct ActionEntry> &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getAffinityMask(int64_t &Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->GetValue(this, &Value);
   }

   inline ERR getKeys(std::span<std::string> &Value) noexcept {
      auto field = &this->Class->Dictionary[22];
      SetObjectContext(this, field, AC::NIL);
      auto get_field = (ERR (*)(APTR, std::span<std::string> &))field->GetValue;
      auto error = get_field(this, Value);
      RestoreObjectContext();
      return error;
   }

   inline ERR getErrorCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getExitCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getInputCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getOutputCallback(FUNCTION * &Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      auto get_field = (ERR (*)(APTR, FUNCTION * &))field->GetValue;
      return get_field(this, Value);
   }

   inline ERR getPriority(int &Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      SetObjectContext(this, field, AC::NIL);
      auto error = field->GetValue(this, &Value);
      RestoreObjectContext();
      return error;
   }


   // Customised field setting

   inline ERR setLaunchPath(const std::string_view &Value) noexcept {
      this->LaunchPath = Value;
      return ERR::Okay;
   }

   inline ERR setName(const std::string_view &Value) noexcept {
      this->Name = Value;
      return ERR::Okay;
   }

   inline ERR setLocation(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[17];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setPath(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, 0x00804300, &Value);
   }

   inline ERR setTimeOut(const double Value) noexcept {
      this->TimeOut = Value;
      return ERR::Okay;
   }

   inline ERR setParameters(const kt::vector<std::string> &Value) noexcept {
      this->Parameters = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const TSF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

   inline ERR setReturnCode(const int Value) noexcept {
      auto field = &this->Class->Dictionary[19];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

   inline ERR setProcess(const int Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->ProcessID = Value;
      return ERR::Okay;
   }

   inline ERR setAffinityMask(const int64_t Value) noexcept {
      auto field = &this->Class->Dictionary[5];
      return field->WriteValue(this, field, FD_INT64, &Value);
   }

   inline ERR setArgs(const std::string_view &Value) noexcept {
      auto field = &this->Class->Dictionary[13];
      return field->WriteValue(this, field, 0x00804208, &Value);
   }

   inline ERR setErrorCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[20];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setExitCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[7];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setInputCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[14];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setOutputCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[3];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setPriority(const int Value) noexcept {
      auto field = &this->Class->Dictionary[6];
      return field->WriteValue(this, field, FD_INT, &Value);
   }

};

// Thread class definition

#define VER_THREAD (1.000000)

class objThread : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::THREAD;
   static constexpr CSTRING CLASS_NAME = "Thread";

   using create = kt::Create<objThread>;
   objThread(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   FUNCTION Callback;          // This function will be called when the thread finishes.
   FUNCTION Routine;           // This function will be called when the thread starts.
   kt::vector<int8_t> Data;    // Storage for custom client data.
   ERR      Error;             // Reflects the error code returned by the thread routine.
   THF      Flags;             // Optional flags can be defined here.

   // Action stubs

   inline ERR activate() noexcept { return Action(AC::Activate, this, nullptr); }
   inline ERR deactivate() noexcept { return Action(AC::Deactivate, this, nullptr); }
   inline ERR init() noexcept { return InitObject(this); }

   // Customised field getting

   inline ERR getCallback(FUNCTION * &Value) noexcept {
      Value = &this->Callback;
      return ERR::Okay;
   }

   inline ERR getRoutine(FUNCTION * &Value) noexcept {
      Value = &this->Routine;
      return ERR::Okay;
   }

   inline ERR getData(std::span<int8_t> &Value) noexcept {
      auto ktv = (kt::vector<int8_t> *)(((int8_t *)this) + CLASS_OFFSET + 64);
      Value = std::span<int8_t>(ktv->data(), ktv->size());
      return ERR::Okay;
   }

   inline ERR getError(ERR &Value) noexcept {
      Value = this->Error;
      return ERR::Okay;
   }

   inline ERR getFlags(THF &Value) noexcept {
      Value = this->Flags;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setCallback(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[2];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setRoutine(const FUNCTION Value) noexcept {
      auto field = &this->Class->Dictionary[9];
      return field->WriteValue(this, field, FD_FUNCTION, &Value);
   }

   inline ERR setData(const kt::vector<int8_t> &Value) noexcept {
      this->Data = Value;
      return ERR::Okay;
   }

   inline ERR setFlags(const THF Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Flags = Value;
      return ERR::Okay;
   }

};