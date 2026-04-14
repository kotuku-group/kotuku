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
#include <kotuku/strings.hpp>
#include <inttypes.h>
#include <string_view>

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

[[nodiscard]] static ERR consume_input_events(const InputEvent *Events, int Handle)
{
   pf::Log log(__FUNCTION__);

   auto Self = (objScript *)CurrentContext();
   auto prv = (prvTiri *)Self->ChildPrivate;

   auto list = prv->InputList;
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

         lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, list->Callback); // +1 Reference to callback
         lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, list->InputValue); // +1 Optional input value registered by the Tiri client
         named_struct_to_table(prv->Lua, "InputEvent", Events); // +1 Input message

         if (lua_pcall(prv->Lua, 2, 0, 0)) {
            process_error(Self, "Input DataFeed Callback");
         }

         Events = Events->Next;
      }

   SetResource(RES::LOG_DEPTH, branch);

   if (lua_gc(prv->Lua, LUA_GCISRUNNING, 0)) {
      log.traceBranch("Collecting garbage.");
      lua_gc(prv->Lua, LUA_GCCOLLECT, 0);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Any Read accesses to the object will pass through here.

[[nodiscard]] static int input_index(lua_State *Lua)
{
   pf::Log log;

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
   pf::Log log("input.keyboard");
   auto prv = (prvTiri *)Lua->script->ChildPrivate;

   OBJECTID object_id;
   GCobject *obj;
   if ((obj = lj_lib_optobject(Lua, 1))) object_id = obj->uid;
   else object_id = lua_tointeger(Lua, 1);

   if ((object_id) and (GetClassID(object_id) != CLASSID::SURFACE)) luaL_argerror(Lua, 1, "Surface object required.");

   int function_type = lua_type(Lua, 2);
   if ((function_type IS LUA_TFUNCTION) or (function_type IS LUA_TSTRING));
   else {
      luaL_argerror(Lua, 2, "Function reference required.");
      return 0;
   }

   log.traceBranch("Surface: %d", object_id);

   bool sub_keyevent = false;
   if (object_id) {
      if (not prv->FocusEventHandle) { // Monitor the focus state of the target surface with a global function.
         SubscribeEvent(EVID_GUI_SURFACE_FOCUS, C_FUNCTION(focus_event, Lua), &prv->FocusEventHandle);
      }

      if (ScopedObjectLock<objSurface> surface(object_id, 5000); surface.granted()) {
         if (surface->hasFocus()) sub_keyevent = true;
      }
      else {
         luaL_error(Lua, ERR::AccessObject, "Failed to access surface #%d.", object_id);
         return 0;
      }
   }
   else sub_keyevent = true; // Global subscription independent of any surface.


   if (auto input = (struct finput *)lua_newuserdata(Lua, sizeof(struct finput))) {
      luaL_getmetatable(Lua, "Tiri.input");
      lua_setmetatable(Lua, -2);

      APTR event = nullptr;
      if (sub_keyevent) SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(key_event, input), &event);

      input->InputHandle = 0;
      input->Script      = Lua->script;
      input->SurfaceID   = object_id;
      input->KeyEvent    = event;
      if (function_type IS LUA_TFUNCTION) {
         lua_pushvalue(Lua, 2);
         input->Callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
      }
      else {
         lua_getglobal(Lua, (STRING)lua_tostring(Lua, 2));
         input->Callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
      }

      lua_pushvalue(Lua, lua_gettop(Lua)); // Take a copy of the Tiri.input object
      input->InputValue = luaL_ref(Lua, LUA_REGISTRYINDEX);
      input->Mode = FIM_KEYBOARD;
      input->Next = prv->InputList;
      prv->InputList = input;
      return 1;
   }
   else {
      luaL_error(Lua, ERR::Memory, "Failed to create Tiri.input object.");
      return 0;
   }
}

//********************************************************************************************************************
// Usage: req = input.requestItem(Source, Item, DataType, ReceiptFunction)
//
// Request an item of data from an existing object that can provision data.  Used to support drag and drop operations.

[[nodiscard]] static int input_request_item(lua_State *Lua)
{
   auto prv = (prvTiri *)Lua->script->ChildPrivate;

   if (not lua_isfunction(Lua, 4)) {
      luaL_argerror(Lua, 4, "Function expected.");
      return 0;
   }

   auto obj = lj_lib_optobject(Lua, 1);
   OBJECTID source_id;

   if (obj) source_id = obj->uid;
   else if (not (source_id = lua_tointeger(Lua, 1))) {
      luaL_argerror(Lua, 1, "Invalid object reference");
      return 0;
   }

   int item = lua_tointeger(Lua, 2);

   DATA datatype;
   if (lua_isstring(Lua, 3)) {
      auto dt = lua_tostringview(Lua, 3);
      if (pf::iequals("text", dt))              datatype = DATA::TEXT;
      else if (pf::iequals("raw", dt))          datatype = DATA::RAW;
      else if (pf::iequals("device_input", dt)) datatype = DATA::DEVICE_INPUT;
      else if (pf::iequals("xml", dt))          datatype = DATA::XML;
      else if (pf::iequals("audio", dt))        datatype = DATA::AUDIO;
      else if (pf::iequals("record", dt))       datatype = DATA::RECORD;
      else if (pf::iequals("image", dt))        datatype = DATA::IMAGE;
      else if (pf::iequals("request", dt))      datatype = DATA::REQUEST;
      else if (pf::iequals("receipt", dt))      datatype = DATA::RECEIPT;
      else if (pf::iequals("file", dt))         datatype = DATA::FILE;
      else if (pf::iequals("content", dt))      datatype = DATA::CONTENT;
      else {
         luaL_argerror(Lua, 3, "Unrecognised datatype");
         return 0;
      }
   }
   else {
      datatype = DATA(lua_tointeger(Lua, 3));
      if (int(datatype) <= 0) {
         luaL_argerror(Lua, 3, "Datatype invalid");
         return 0;
      }
   }

   auto function_type = lua_type(Lua, 4);
   if (function_type IS LUA_TFUNCTION) {
      lua_pushvalue(Lua, 4);
      prv->Requests.emplace_back(source_id, luaL_ref(Lua, LUA_REGISTRYINDEX));
   }
   else if (function_type IS LUA_TSTRING) {
      lua_getglobal(Lua, (STRING)lua_tostring(Lua, 4));
      prv->Requests.emplace_back(source_id, luaL_ref(Lua, LUA_REGISTRYINDEX));
   }

   {
      // The source will return a DATA::RECEIPT for the items that we've asked for (see the DataFeed action).
      pf::Log log("input.request_item");
      log.branch();
      pf::ScopedObjectLock src(source_id);
      if (src.granted()) {
         struct dcRequest dcr {
            .Item = item,
            .Preference = { char(datatype), 0 }
         };

         auto error = acDataFeed(*src, Lua->script, DATA::REQUEST, &dcr, sizeof(dcr));
         if (error != ERR::Okay) luaL_error(Lua, ERR::Failed, "Failed to request item %d from source #%d: %s", item, source_id, GetErrorMsg(error));
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
   pf::Log log("input.subscribe");
   auto prv = (prvTiri *)Lua->script->ChildPrivate;

   auto mask = JTYPE(lua_tointeger(Lua, 1)); // Optional

   OBJECTID object_id;
   GCobject *object_ref;
   if ((object_ref = lj_lib_optobject(Lua, 2))) object_id = object_ref->uid;
   else object_id = lua_tointeger(Lua, 2);

   int device_id = lua_tointeger(Lua, 3); // Optional

   int function_type = lua_type(Lua, 4);
   if ((function_type IS LUA_TFUNCTION) or (function_type IS LUA_TSTRING));
   else {
      luaL_argerror(Lua, 4, "Function reference required.");
      return 0;
   }

   ERR error;
   if (not modDisplay) {
      pf::SwitchContext context(modTiri);
      if ((error = objModule::load("display", &modDisplay, &DisplayBase)) != ERR::Okay) {
         luaL_error(Lua, ERR::LoadModule);
         return 0;
      }
   }

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
         lua_getglobal(Lua, (STRING)lua_tostring(Lua, 1));
         input->Callback = luaL_ref(Lua, LUA_REGISTRYINDEX);
      }

      lua_pushvalue(Lua, lua_gettop(Lua)); // Take a copy of the Tiri.input object
      input->InputValue = luaL_ref(Lua, LUA_REGISTRYINDEX);
      input->KeyEvent    = nullptr;
      input->InputHandle = 0;
      input->Mask        = mask;
      input->Mode        = FIM_DEVICE;
      input->Next        = prv->InputList;

      prv->InputList = input;

      auto callback = C_FUNCTION(consume_input_events);
      if ((error = gfx::SubscribeInput(&callback, input->SurfaceID, mask, device_id, &input->InputHandle)) != ERR::Okay) {
         luaL_error(Lua, error);
      }

      return 1;
   }
   else luaL_error(Lua, ERR::Memory, "Failed to initialise input subscription.");
   return 0;
}

//********************************************************************************************************************
// Usage: error = input.unsubscribe()

[[nodiscard]] static int input_unsubscribe(lua_State *Lua)
{
   auto input = (struct finput *)get_meta(Lua, lua_upvalueindex(1), "Tiri.input");
   if (not input) {
      luaL_argerror(Lua, 1, "Expected input interface.");
      return 0;
   }

   pf::Log log("input.unsubscribe");
   log.traceBranch();

   if (input->InputValue)  { luaL_unref(Lua, LUA_REGISTRYINDEX, input->InputValue); input->InputValue = 0; }
   if (input->Callback)    { luaL_unref(Lua, LUA_REGISTRYINDEX, input->Callback); input->Callback = 0; }
   if (input->KeyEvent)    { UnsubscribeEvent(input->KeyEvent); input->KeyEvent = nullptr; }
   if (input->InputHandle) { gfx::UnsubscribeInput(input->InputHandle); input->InputHandle = 0; }

   input->Script = nullptr;
   input->Mode   = 0;
   return 0;
}

//********************************************************************************************************************
// Input garbage collecter.

[[nodiscard]] static int input_destruct(lua_State *Lua)
{
   pf::Log log("input.destroy");

   auto input = (struct finput *)lua_touserdata(Lua, 1);
   if (input) {
      log.traceBranch("Surface: %d, CallbackRef: %d, KeyEvent: %p", input->SurfaceID, input->Callback, input->KeyEvent);

      if (input->SurfaceID)   input->SurfaceID = 0;
      if (input->InputHandle) { gfx::UnsubscribeInput(input->InputHandle); input->InputHandle = 0; }
      if (input->InputValue)  { luaL_unref(Lua, LUA_REGISTRYINDEX, input->InputValue); input->InputValue = 0; }
      if (input->Callback)    { luaL_unref(Lua, LUA_REGISTRYINDEX, input->Callback); input->Callback = 0; }
      if (input->KeyEvent)    { UnsubscribeEvent(input->KeyEvent); input->KeyEvent = nullptr; }

      if (Lua->script) { // Remove from the chain.
         auto prv = (prvTiri *)Lua->script->ChildPrivate;
         if (prv->InputList IS input) prv->InputList = input->Next;
         else {
            auto list = prv->InputList;
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
   pf::Log log("input.key_event");
   objScript *script = Input->Script;
   auto prv = (prvTiri *)script->ChildPrivate;

   if ((not script) or (not prv)) {
      log.trace("Input->Script undefined.");
      return;
   }

   log.traceBranch("Incoming keyboard input");

   int depth = GetResource(RES::LOG_DEPTH); // Required because thrown errors cause the debugger to lose its step position
   int top = lua_gettop(prv->Lua);
   lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, Input->Callback); // Get the function reference in Lua and place it on the stack
   lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, Input->InputValue); // Arg: Input value registered by the client
   lua_pushinteger(prv->Lua, Input->SurfaceID);  // Arg: Surface (if applicable)
   lua_pushinteger(prv->Lua, uint32_t(Event->Qualifiers)); // Arg: Key Flags
   lua_pushinteger(prv->Lua, int(Event->Code));       // Arg: Key Value
   lua_pushinteger(prv->Lua, Event->Unicode);    // Arg: Unicode character

   if (lua_pcall(prv->Lua, 5, 0, 0)) {
      process_error(script, "Keyboard event callback");
   }

   lua_settop(prv->Lua, top);
   SetResource(RES::LOG_DEPTH, depth);

   if (lua_gc(prv->Lua, LUA_GCISRUNNING, 0)) {
      log.traceBranch("Collecting garbage.");
      lua_gc(prv->Lua, LUA_GCCOLLECT, 0);
   }
}

//********************************************************************************************************************
// This is a global function for monitoring the focus of surfaces that we want to filter on for keyboard input.

static void focus_event(evFocus *Event, int Size, lua_State *Lua)
{
   pf::Log log(__FUNCTION__);
   auto prv = (prvTiri *)Lua->script->ChildPrivate;
   objScript *script = Lua->script;

   if ((not script) or (not prv)) {
      log.trace("Script undefined.");
      return;
   }

   log.traceBranch("Incoming focus event targeting #%d, focus lost from #%d.", Event->FocusList[0], Event->FocusList[Event->TotalWithFocus]);

   for (auto input=prv->InputList; input; input=input->Next) {
      if (input->Mode != FIM_KEYBOARD) continue;
      if (input->KeyEvent) continue;

      auto callback = C_FUNCTION(key_event, input);
      for (int i=0; i < Event->TotalWithFocus; i++) {
         if (input->SurfaceID IS Event->FocusList[i]) {
            log.trace("Focus notification received for key events on surface #%d.", input->SurfaceID);
            SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, callback, &input->KeyEvent);
            break;
         }
      }
   }

   for (auto input=prv->InputList; input; input=input->Next) {
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

   pf::Log log(__FUNCTION__);
   log.trace("Registering input interface.");

   luaL_newmetatable(Lua, "Tiri.input");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // pushes the metatable created earlier
   lua_settable(Lua, -3);   // metatable.__index = metatable

   luaL_openlib(Lua, nullptr, inputlib_methods, 0);
   luaL_openlib(Lua, "input", inputlib_functions, 0);

   // Register input interface prototypes for compile-time type inference
   reg_iface_prototype("input", "subscribe", { TiriType::Any }, { TiriType::Num, TiriType::Any, TiriType::Num, TiriType::Func });
   reg_iface_prototype("input", "keyboard", { TiriType::Any }, { TiriType::Any, TiriType::Func });
   reg_iface_prototype("input", "requestItem", {}, { TiriType::Any, TiriType::Num, TiriType::Any, TiriType::Func });
}
