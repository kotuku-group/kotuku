/*********************************************************************************************************************

The input interface provides support for processing input messages.  The InputEvent structure is passed for each incoming
message that is detected.

   local in = input.subscribe(JTYPE::MOVEMENT, SurfaceID, 0, function(SurfaceID, Event)

   end)

   in.unsubscribe()

To get keyboard feedback:

   local in = input.keyboard(SurfaceID, function(Input, SurfaceID, Flags, Value)

   end)

   in.unsubscribe()

For drag and drop operations, data can be requested from a source as follows:

   input.requestItem(SourceID, Item, DataType, function(Items)

   end)

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>
#include <inttypes.h>
#include <string_view>
#include <mutex>

#include "lib.h"
#include "lauxlib.h"
#include "lj_obj.h"

#include "hashes.h"
#include "defs.h"
#include "lj_proto_registry.h"

JUMPTABLE_DISPLAY

static int input_unsubscribe(lua_State *Lua);
static void focus_event(evFocus *, int, lua_State *);
static void key_event(evKey *, int, struct finput *);

//********************************************************************************************************************

static void release_input_subscription(lua_State *Lua, struct finput *Input)
{
   if (Input->InputValue)  { luaL_unref(Lua, LUA_REGISTRYINDEX, Input->InputValue); Input->InputValue = 0; }
   if (Input->Callback)    { luaL_unref(Lua, LUA_REGISTRYINDEX, Input->Callback); Input->Callback = 0; }
   if (Input->KeyEvent)    { UnsubscribeEvent(Input->KeyEvent); Input->KeyEvent = nullptr; }
   if (Input->InputHandle) { gfx::UnsubscribeInput(Input->InputHandle); Input->InputHandle = 0; }
}

//********************************************************************************************************************

[[nodiscard]] static ERR consume_input_events(const InputEvent *Events, int Handle)
{
   kt::Log log(__FUNCTION__);

   auto Self = (extTiri *)CurrentContext();

   auto list = Self->InputList;
   for (; (list) and (list->InputHandle != Handle); list=list->Next);

   if (not list) {
      log.warning("Dangling input feed subscription %d", Handle);
      gfx::UnsubscribeInput(Handle);
      return ERR::NotFound;
   }

   int branch = GetResource(RES::LOG_DEPTH); // Required as thrown errors cause the debugger to lose its branch position

      // For simplicity, a call to the handler is made for each individual input event.

      while (Events) {
         if ((Events->Flags & JTYPE::MOVEMENT) != JTYPE::NIL) {
            while ((Events->Next) and ((Events->Next->Flags & JTYPE::MOVEMENT) != JTYPE::NIL)) Events = Events->Next;
         }

         lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, list->Callback); // +1 Reference to callback
         lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, list->InputValue); // +1 Optional input value registered by the Tiri client
         if (!named_struct_to_table(Self->Lua, "InputEvent", Events)) { // +1 Input message
            if (lua_pcall(Self->Lua, 2, 0, 0)) {
               process_error(Self, "Input DataFeed Callback");
            }
         }
         else process_error(Self, "Failed to process InputEvent struct");

         Events = Events->Next;
      }

   SetResource(RES::LOG_DEPTH, branch);

   if (lua_gc(Self->Lua, LUA_GCISRUNNING, 0)) {
      log.traceBranch("Collecting garbage.");
      lua_gc(Self->Lua, LUA_GCCOLLECT, 0);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Any Read accesses to the object will pass through here.

[[nodiscard]] static int input_index(lua_State *Lua)
{
   kt::Log log;

   if (auto input = (struct finput *)luaL_checkudata(Lua, 1, "Tiri.input")) {
      auto field = lua_checkstringview(Lua, 2);
      if (field.empty()) return 0;

      log.trace("input.index(#%d, %.*s)", input->SurfaceID, int(field.size()), field.data());

      switch (strihash(field)) {
         case HASH_UNSUBSCRIBE:
            lua_pushvalue(Lua, 1); // Duplicate the interface reference
            lua_pushcclosure(Lua, input_unsubscribe, 1);
            return 1;

         default:
            luaL_error(Lua, ERR::UnknownProperty, "Unknown field reference '%s'", field);
      }
   }
   return 0;
}

//********************************************************************************************************************
// Usage: input = input.keyboard(SurfaceID, Function)

[[nodiscard]] static int input_keyboard(lua_State *Lua)
{
   kt::Log log("input.keyboard");
   auto tiri = Lua->script;

   OBJECTID object_id;
   GCobject *obj;
   if ((obj = lj_lib_optobject(Lua, 1))) object_id = obj->uid;
   else object_id = lua_tointeger(Lua, 1);

   if ((object_id) and (GetClassID(object_id) != CLASSID::SURFACE)) luaL_argerror(Lua, 1, "Surface object required.");

   int function_type = lua_type(Lua, 2);
   if ((function_type IS LUA_TFUNCTION) or (function_type IS LUA_TSTRING));
   else luaL_argerror(Lua, 2, "Function reference required.");

   log.traceBranch("Surface: %d", object_id);

   bool sub_keyevent = false;
   if (object_id) {
      if (not tiri->FocusEventHandle) { // Monitor the focus state of the target surface with a global function.
         if (auto error = SubscribeEvent(EVID_GUI_SURFACE_FOCUS, C_FUNCTION(focus_event, Lua),
               &tiri->FocusEventHandle); error != ERR::Okay) {
            luaL_error(Lua, error, "Failed to subscribe to surface focus events.");
         }
      }

      if (ScopedObjectLock<objSurface> surface(object_id, 5000); surface.granted()) {
         if (surface->hasFocus()) sub_keyevent = true;
      }
      else luaL_error(Lua, ERR::AccessObject, "Failed to access surface #%d.", object_id);
   }
   else sub_keyevent = true; // Global subscription independent of any surface.


   if (auto input = (struct finput *)lua_newuserdata(Lua, sizeof(struct finput))) {
      luaL_getmetatable(Lua, "Tiri.input");
      lua_setmetatable(Lua, -2);

      input->InputHandle = 0;
      input->Script      = Lua->script;
      input->SurfaceID   = object_id;
      input->KeyEvent    = nullptr;
      input->Callback    = 0;
      input->InputValue  = 0;
      input->Mask        = JTYPE::NIL;
      input->Mode        = FIM_KEYBOARD;
      input->Next        = nullptr;
      if (function_type IS LUA_TFUNCTION) {
         lua_pushvalue(Lua, 2);
         input->Callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
      }
      else {
         lua_getglobal(Lua, lua_tostringview(Lua, 2));
         input->Callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
      }

      lua_pushvalue(Lua, lua_gettop(Lua)); // Take a copy of the Tiri.input object
      input->InputValue = luaL_ref(Lua, LUA_REGISTRYINDEX);

      if (sub_keyevent) {
         if (auto error = SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(key_event, input),
               &input->KeyEvent); error != ERR::Okay) {
            if (input->InputValue) { luaL_unref(Lua, LUA_REGISTRYINDEX, input->InputValue); input->InputValue = 0; }
            if (input->Callback)   { luaL_unref(Lua, LUA_REGISTRYINDEX, input->Callback); input->Callback = 0; }
            luaL_error(Lua, error, "Failed to subscribe to keyboard input events.");
         }
      }

      input->Next = tiri->InputList;
      tiri->InputList = input;
   }
   else luaL_error(Lua, ERR::Memory, "Failed to create Tiri.input object.");

   return 1;
}

//********************************************************************************************************************
// Usage: req = input.requestItem(Source, Item, DataType, ReceiptFunction)
//
// Request an item of data from an existing object that can provision data.  Used to support drag and drop operations.

[[nodiscard]] static int input_request_item(lua_State *Lua)
{
   auto tiri = Lua->script;

   if (not lua_isfunction(Lua, 4)) luaL_argerror(Lua, 4, "Function expected.");

   auto obj = lj_lib_optobject(Lua, 1);
   OBJECTID source_id;

   if (obj) source_id = obj->uid;
   else if (not (source_id = lua_tointeger(Lua, 1))) luaL_argerror(Lua, 1, "Invalid object reference");

   int item = lua_tointeger(Lua, 2);

   DATA datatype;
   if (lua_isstring(Lua, 3)) {
      auto dt = lua_tostringview(Lua, 3);
      if (kt::iequals("text", dt))              datatype = DATA::TEXT;
      else if (kt::iequals("raw", dt))          datatype = DATA::RAW;
      else if (kt::iequals("device_input", dt)) datatype = DATA::DEVICE_INPUT;
      else if (kt::iequals("xml", dt))          datatype = DATA::XML;
      else if (kt::iequals("audio", dt))        datatype = DATA::AUDIO;
      else if (kt::iequals("record", dt))       datatype = DATA::RECORD;
      else if (kt::iequals("image", dt))        datatype = DATA::IMAGE;
      else if (kt::iequals("request", dt))      datatype = DATA::REQUEST;
      else if (kt::iequals("receipt", dt))      datatype = DATA::RECEIPT;
      else if (kt::iequals("file", dt))         datatype = DATA::FILE;
      else if (kt::iequals("content", dt))      datatype = DATA::CONTENT;
      else luaL_argerror(Lua, 3, "Unrecognised datatype");
   }
   else {
      datatype = DATA(lua_tointeger(Lua, 3));
      if (int(datatype) <= 0) luaL_argerror(Lua, 3, "Datatype invalid");
   }

   int callback_ref = 0;
   auto function_type = lua_type(Lua, 4);
   if (function_type IS LUA_TFUNCTION) {
      lua_pushvalue(Lua, 4);
      callback_ref = luaL_ref(Lua, LUA_REGISTRYINDEX);
      tiri->Requests.emplace_back(source_id, callback_ref);
   }
   else if (function_type IS LUA_TSTRING) {
      lua_getglobal(Lua, lua_tostringview(Lua, 4));
      callback_ref = luaL_ref(Lua, LUA_REGISTRYINDEX);
      tiri->Requests.emplace_back(source_id, callback_ref);
   }

   {
      // The source will return a DATA::RECEIPT for the items that we've asked for (see the DataFeed action).
      kt::Log log("input.request_item");
      log.branch();
      kt::ScopedObjectLock src(source_id);
      if (src.granted()) {
         struct dcRequest dcr {
            .Item = item,
            .Preference = { char(datatype), 0 }
         };

         auto error = acDataFeed(*src, Lua->script, DATA::REQUEST, &dcr, sizeof(dcr));
         if (error != ERR::Okay) {
            if (callback_ref) {
               bool request_found = false;
               for (auto it = tiri->Requests.begin(); it != tiri->Requests.end(); it++) {
                  if ((it->SourceID IS source_id) and (it->Callback IS callback_ref)) {
                     tiri->Requests.erase(it);
                     request_found = true;
                     break;
                  }
               }
               if (request_found) luaL_unref(Lua, LUA_REGISTRYINDEX, callback_ref);
            }

            src.unlock();
            luaL_error(Lua, ERR::Failed, "Failed to request item %d from source #%d: %s", item, source_id,
               GetErrorMsg(error));
         }
      }
      else {
         if (callback_ref) {
            bool request_found = false;
            for (auto it = tiri->Requests.begin(); it != tiri->Requests.end(); it++) {
               if ((it->SourceID IS source_id) and (it->Callback IS callback_ref)) {
                  tiri->Requests.erase(it);
                  request_found = true;
                  break;
               }
            }
            if (request_found) luaL_unref(Lua, LUA_REGISTRYINDEX, callback_ref);
         }
         luaL_error(Lua, ERR::AccessObject, "Failed to access data source #%d.", source_id);
      }
   }

   return 0;
}

//********************************************************************************************************************
// Usage: input = input.subscribe(MaskFlags (JTYPE), SurfaceFilter (Optional), DeviceFilter (Optional), Function)
//
// This functionality is a wrapper for the gfx::SubscribeInput() function.

[[nodiscard]] static int input_subscribe(lua_State *Lua)
{
   kt::Log log("input.subscribe");
   auto tiri = Lua->script;

   auto mask = JTYPE(lua_tointeger(Lua, 1)); // Optional

   OBJECTID object_id;
   GCobject *object_ref;
   if ((object_ref = lj_lib_optobject(Lua, 2))) object_id = object_ref->uid;
   else object_id = lua_tointeger(Lua, 2);

   int device_id = lua_tointeger(Lua, 3); // Optional

   int function_type = lua_type(Lua, 4);
   if ((function_type IS LUA_TFUNCTION) or (function_type IS LUA_TSTRING));
   else luaL_argerror(Lua, 4, "Function reference required.");

   ERR error = ERR::Okay;
   {
      static std::mutex display_load_lock;
      const std::lock_guard<std::mutex> lock(display_load_lock);

      if (not modDisplay) {
         kt::SwitchContext context(modTiri);
         error = objModule::load("display", &modDisplay, &DisplayBase);
      }
   }
   if (error != ERR::Okay) luaL_error(Lua, ERR::LoadModule);

   log.msg("Surface: %d, Mask: $%.8x, Device: %d", object_id, int(mask), device_id);

   struct finput *input;
   if ((input = (struct finput *)lua_newuserdata(Lua, sizeof(struct finput)))) {
      luaL_getmetatable(Lua, "Tiri.input");
      lua_setmetatable(Lua, -2);

      input->SurfaceID = object_id;

      if (function_type IS LUA_TFUNCTION) {
         lua_pushvalue(Lua, 4);
         input->Callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
      }
      else {
         lua_getglobal(Lua, lua_tostringview(Lua, 4));
         input->Callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
      }

      lua_pushvalue(Lua, lua_gettop(Lua)); // Take a copy of the Tiri.input object
      input->InputValue = luaL_ref(Lua, LUA_REGISTRYINDEX);
      input->Script      = Lua->script;
      input->KeyEvent    = nullptr;
      input->InputHandle = 0;
      input->Mask        = mask;
      input->Mode        = FIM_DEVICE;
      input->Next        = nullptr;

      auto callback = C_FUNCTION(consume_input_events);
      if ((error = gfx::SubscribeInput(&callback, input->SurfaceID, mask, device_id,
            &input->InputHandle)) != ERR::Okay) {
         if (input->InputHandle) { gfx::UnsubscribeInput(input->InputHandle); input->InputHandle = 0; }
         if (input->InputValue)  { luaL_unref(Lua, LUA_REGISTRYINDEX, input->InputValue); input->InputValue = 0; }
         if (input->Callback)    { luaL_unref(Lua, LUA_REGISTRYINDEX, input->Callback); input->Callback = 0; }
         luaL_error(Lua, error);
      }

      input->Next = tiri->InputList;
      tiri->InputList = input;
   }
   else luaL_error(Lua, ERR::Memory, "Failed to initialise input subscription.");

   return 1;
}

//********************************************************************************************************************
// Usage: error = input.unsubscribe()

[[nodiscard]] static int input_unsubscribe(lua_State *Lua)
{
   auto input = (struct finput *)get_meta(Lua, lua_upvalueindex(1), "Tiri.input");
   if (not input) luaL_argerror(Lua, 1, "Expected input interface.");

   kt::Log log("input.unsubscribe");
   log.traceBranch();

   release_input_subscription(Lua, input);
   input->Script = nullptr;
   input->Mode   = 0;
   return 0;
}

//********************************************************************************************************************
// Input garbage collecter.

[[nodiscard]] static int input_destruct(lua_State *Lua)
{
   kt::Log log("input.destroy");

   auto input = (struct finput *)lua_touserdata(Lua, 1);
   if (input) {
      log.traceBranch("Surface: %d, CallbackRef: %d, KeyEvent: %p", input->SurfaceID, input->Callback, input->KeyEvent);

      if (input->SurfaceID)   input->SurfaceID = 0;
      release_input_subscription(Lua, input);

      if (Lua->script) { // Remove from the chain.
         auto tiri = Lua->script;
         if (tiri->InputList IS input) tiri->InputList = input->Next;
         else {
            auto list = tiri->InputList;
            while (list) {
               if (list->Next IS input) {
                  list->Next = input->Next;
                  break;
               }
               list = list->Next;
            }
         }
      }
   }

   return 0;
}

//********************************************************************************************************************
// Key events should only be received when a monitored surface has the focus.

static void key_event(evKey *Event, int Size, struct finput *Input)
{
   kt::Log log("input.key_event");

   auto tiri = Input->Script;
   if (not tiri) {
      log.trace("Input->Script undefined.");
      return;
   }

   log.traceBranch("Incoming keyboard input");

   auto lua = tiri->Lua;
   int depth = GetResource(RES::LOG_DEPTH); // Required because thrown errors cause the debugger to lose its step position
   int top = lua_gettop(lua);
   lua_rawgeti(lua, LUA_REGISTRYINDEX, Input->Callback); // Get the function reference in Lua and place it on the stack
   lua_rawgeti(lua, LUA_REGISTRYINDEX, Input->InputValue); // Arg: Input value registered by the client
   lua_pushinteger(lua, Input->SurfaceID);  // Arg: Surface (if applicable)
   lua_pushinteger(lua, uint32_t(Event->Qualifiers)); // Arg: Key Flags
   lua_pushinteger(lua, int(Event->Code));       // Arg: Key Value
   lua_pushinteger(lua, Event->Unicode);    // Arg: Unicode character

   if (lua_pcall(lua, 5, 0, 0)) {
      process_error(tiri, "Keyboard event callback");
   }

   lua_settop(lua, top);
   SetResource(RES::LOG_DEPTH, depth);

   if (lua_gc(lua, LUA_GCISRUNNING, 0)) {
      log.traceBranch("Collecting garbage.");
      lua_gc(lua, LUA_GCCOLLECT, 0);
   }
}

//********************************************************************************************************************
// This is a global function for monitoring the focus of surfaces that we want to filter on for keyboard input.

static void focus_event(evFocus *Event, int Size, lua_State *Lua)
{
   kt::Log log(__FUNCTION__);

   auto tiri = Lua->script;
   if (not tiri) {
      log.trace("Script undefined.");
      return;
   }

   if ((Event->TotalWithFocus > 0) and (Event->TotalLostFocus > 0)) {
      log.traceBranch("Incoming focus event targeting #%d, focus lost from #%d.", Event->FocusList[0],
         Event->FocusList[Event->TotalWithFocus]);
   }
   else if (Event->TotalWithFocus > 0) {
      log.traceBranch("Incoming focus event targeting #%d.", Event->FocusList[0]);
   }
   else if (Event->TotalLostFocus > 0) {
      log.traceBranch("Incoming focus event lost from #%d.", Event->FocusList[Event->TotalWithFocus]);
   }
   else log.traceBranch("Incoming focus event with no focus changes.");

   for (auto input=tiri->InputList; input; input=input->Next) {
      if (input->Mode != FIM_KEYBOARD) continue;
      if (input->KeyEvent) continue;

      auto callback = C_FUNCTION(key_event, input);
      for (int i=0; i < Event->TotalWithFocus; i++) {
         if (input->SurfaceID IS Event->FocusList[i]) {
            log.trace("Focus notification received for key events on surface #%d.", input->SurfaceID);
            if (auto error = SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, callback, &input->KeyEvent);
                  error != ERR::Okay) {
               log.warning("Failed to subscribe to keyboard input events: %s", GetErrorMsg(error));
            }
            break;
         }
      }
   }

   for (auto input=tiri->InputList; input; input=input->Next) {
      if (input->Mode != FIM_KEYBOARD) continue;
      if (not input->KeyEvent) continue;

      for (int i=0; i < Event->TotalLostFocus; i++) {
         if (input->SurfaceID IS Event->FocusList[Event->TotalWithFocus+i]) {
            log.trace("Lost focus notification received for key events on surface #%d.", input->SurfaceID);
            UnsubscribeEvent(input->KeyEvent);
            input->KeyEvent = nullptr;
            break;
         }
      }
   }
}

//********************************************************************************************************************

[[nodiscard]] static int input_tostring(lua_State *Lua)
{
   auto input = (struct finput *)lua_touserdata(Lua, 1);
   if (input) lua_pushfstring(Lua, "Input handler for surface #%d", input->SurfaceID);
   else lua_pushstring(Lua, "?");
   return 1;
}

//********************************************************************************************************************

void register_input_class(lua_State *Lua)
{
   static constexpr struct luaL_Reg inputlib_functions[] = {
      { "subscribe",   input_subscribe },
      { "keyboard",    input_keyboard },
      { "requestItem", input_request_item },
      { nullptr, nullptr }
   };

   static constexpr struct luaL_Reg inputlib_methods[] = {
      { "__gc",       input_destruct },
      { "__tostring", input_tostring },
      { "__index",    input_index },
      { nullptr, nullptr }
   };

   kt::Log log(__FUNCTION__);
   log.trace("Registering input interface.");

   luaL_newmetatable(Lua, "Tiri.input");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable created earlier
   lua_settable(Lua, -3);   // metatable.__index = metatable

   luaL_openlib(Lua, nullptr, inputlib_methods, 0);
   luaL_openlib(Lua, "input", inputlib_functions, 0);

   lua_pop(Lua, 2); // Drop the Tiri.input metatable and the input library table

   // Register input interface prototypes for compile-time type inference
   reg_iface_prototype("input", "subscribe", { TiriType::Any }, { TiriType::Num, TiriType::Any, TiriType::Num, TiriType::Func });
   reg_iface_prototype("input", "keyboard", { TiriType::Any }, { TiriType::Any, TiriType::Func });
   reg_iface_prototype("input", "requestItem", {}, { TiriType::Any, TiriType::Num, TiriType::Any, TiriType::Func });
}
