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
#include "lj_state.h"
#include "defs.h"
#include "lj_proto_registry.h"

JUMPTABLE_DISPLAY

static int input_unsubscribe(lua_State *Lua);
static ERR key_event(const evKey *, APTR);

//********************************************************************************************************************

static void release_input_subscription(lua_State *Lua, struct finput *Input)
{
   if (Input->InputValue)  { luaL_unref(Lua, LUA_REGISTRYINDEX, Input->InputValue); Input->InputValue = 0; }
   release_tiri_function(Lua, &Input->Callback);
   if (Input->KeyEvent)    { gfx::UnsubscribeKeyboard(Input->KeyEvent); Input->KeyEvent = 0; }
   if (Input->InputHandle) { gfx::UnsubscribeInput(Input->InputHandle); Input->InputHandle = 0; }
}

//********************************************************************************************************************

[[nodiscard]] static ERR consume_input_events(const InputEvent *Events, int Handle)
{
   kt::Log log(__FUNCTION__);

   auto Self = (extTiri *)CurrentContext();
   LuaContextRootGuard context_guard(Self->Lua);

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

         LuaCallbackContextGuard callback_context(Self->Lua);
         if (push_tiri_function(Self->Lua, list->Callback, callback_context) != ERR::Okay) {
            log.warning("Input subscription callback is no longer valid.");
            return ERR::InvalidData;
         }
         lua_rawgeti(Self->Lua, LUA_REGISTRYINDEX, list->InputValue); // +1 Optional input value registered by the Tiri client
         if (!named_struct_to_table(Self->Lua, "InputEvent", Events)) { // +1 Input message
            if (lua_pcall(Self->Lua, 2, 0, 0)) {
               process_callback_error(Self, "Input DataFeed Callback");
            }
         }
         else process_callback_error(Self, "Failed to process InputEvent struct");

         // The remainder of the batch is discarded if the callback unsubscribed from the input feed.

         if (not list->InputHandle) break;

         Events = Events->Next;
      }

   SetResource(RES::LOG_DEPTH, branch);

   collect_garbage(Self->Lua);
   return ERR::Okay;
}

static int input_reserve(lua_State *Lua)
{
   auto input = (struct finput *)get_meta(Lua, lua_upvalueindex(1), "Tiri.input");
   if (not input or not input->KeyEvent) luaL_error(Lua, ERR::Args, "Active keyboard reservation required.");
   auto error = gfx::ReserveKey(input->KeyEvent, KQ(luaL_checkinteger(Lua, 1)),
      KQ(luaL_checkinteger(Lua, 2)), luaL_optinteger(Lua, 3, 0), luaL_optinteger(Lua, 4, 0));
   if (error != ERR::Okay) luaL_error(Lua, error, "Invalid or conflicting key reservation.");
   return 0;
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
         case strhash("reserve"):
            lua_pushvalue(Lua, 1);
            lua_pushcclosure(Lua, input_reserve, 1);
            return 1;

         case strhash("unsubscribe"):
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
   if (not modDisplay) {
      auto error = objModule::load("display", &modDisplay, &DisplayBase);
      if (error != ERR::Okay) luaL_error(Lua, error, "Failed to load Display for keyboard routing.");
   }

   OBJECTID object_id;
   GCobject *obj;
   if ((obj = lj_lib_optobject(Lua, 1))) object_id = obj->uid;
   else object_id = lua_tointeger(Lua, 1);

   OBJECTID surface_id = object_id, scope_id = 0;
   if (object_id and GetClassID(object_id) != CLASSID::SURFACE) {
      // Resolve vector scopes through their Scene field without loading or linking the Vector module.
      kt::ScopedObjectLock target(object_id);
      OBJECTPTR scene = nullptr;
      if (not target.granted() or target->get(strhash("scene"), scene) != ERR::Okay or not scene or
          scene->get(strhash("surface"), surface_id) != ERR::Okay) {
         luaL_argerror(Lua, 1, "Surface or vector object required.");
      }
      if (not surface_id) luaL_argerror(Lua, 1, "Vector scene is not attached to a surface.");
      scope_id = object_id;
   }
   if (lua_type(Lua, 2) != LUA_TFUNCTION) luaL_argerror(Lua, 2, "Function reference required.");
   KBF flags = KBF::NIL;
   auto option_bool = [&](CSTRING Name) {
      if (not lua_istable(Lua, 3)) return false;
      lua_getfield(Lua, 3, Name);
      bool value = lua_toboolean(Lua, -1);
      lua_pop(Lua, 1);
      return value;
   };
   if (option_bool("observer")) flags |= KBF::OBSERVER;
   if (option_bool("reserve")) flags |= KBF::RESERVE;
   if (option_bool("noRepeat")) flags |= KBF::NO_REPEAT;

   if (auto input = new (lua_newuserdata(Lua, sizeof(struct finput))) finput {}) {
      luaL_getmetatable(Lua, "Tiri.input");
      lua_setmetatable(Lua, -2);

      input->InputHandle = 0;
      input->Script      = Lua->script;
      input->SurfaceID   = surface_id;
      input->KeyEvent    = 0;
      input->InputValue  = 0;
      input->Mask        = JTYPE::NIL;
      input->Mode        = FIM_KEYBOARD;
      input->Next        = nullptr;
      if (capture_tiri_function(Lua, 2, input->Callback) != ERR::Okay) {
         luaL_argerror(Lua, 2, "Function reference required.");
      }

      lua_pushvalue(Lua, lua_gettop(Lua)); // Take a copy of the Tiri.input object
      input->InputValue = luaL_ref(Lua, LUA_REGISTRYINDEX);

      auto callback = C_FUNCTION(key_event, input);
      auto error = gfx::SubscribeKeyboard(surface_id, scope_id, flags, &callback, &input->KeyEvent);
      if (error IS ERR::Okay and lua_istable(Lua, 3) and (flags & KBF::RESERVE) != KBF::NIL) {
         auto option_int = [&](CSTRING Name) {
            lua_getfield(Lua, 3, Name);
            int value = lua_tointeger(Lua, -1);
            lua_pop(Lua, 1);
            return value;
         };
         auto required = KQ(option_int("required"));
         auto forbidden = KQ(option_int("forbidden"));
         auto code = option_int("code");
         auto unicode = option_int("unicode");
         if (code or unicode) error = gfx::ReserveKey(input->KeyEvent, required, forbidden, code, unicode);
      }
      if (error != ERR::Okay) {
         release_input_subscription(Lua, input);
         luaL_error(Lua, error, "Failed to subscribe to keyboard input.");
      }

      input->Next = tiri->InputList;
      tiri->InputList = input;
   }
   else luaL_error(Lua, ERR::Memory, "Failed to create Tiri.input object.");

   return 1;
}

//********************************************************************************************************************
// Releases requests that have not been answered within DATA_REQUEST_EXPIRY.

void expire_data_requests(extTiri *Self)
{
   auto now = PreciseTime();
   for (auto it = Self->Requests.begin(); it != Self->Requests.end(); ) {
      if (now - it->TimeCreated > DATA_REQUEST_EXPIRY) {
         kt::Log(__FUNCTION__).msg("Request for item %d from #%d has expired.", it->Item, it->SourceID);
         release_tiri_function(Self->Lua, &it->Callback);
         it = Self->Requests.erase(it);
      }
      else it++;
   }
}

// Removes a request if it is still pending.  A synchronous receipt may have consumed it already.

static void cancel_data_request(lua_State *Lua, extTiri *Self, OBJECTID SourceID, int Item, const FUNCTION &Callback)
{
   for (auto it = Self->Requests.begin(); it != Self->Requests.end(); it++) {
      if ((it->SourceID IS SourceID) and (it->Item IS Item) and it->Callback.identical(Callback)) {
         release_tiri_function(Lua, &it->Callback);
         Self->Requests.erase(it);
         return;
      }
   }
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

   FUNCTION callback;
   if (capture_tiri_function(Lua, 4, callback) != ERR::Okay) luaL_argerror(Lua, 4, "Function expected.");
   expire_data_requests(tiri);
   tiri->Requests.emplace_back(source_id, item, callback);

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

         auto error = acDataFeed(*src, Lua->script, DATA::REQUEST,
            std::span<const int8_t>((const int8_t *)&dcr, sizeof(dcr)));
         if (error != ERR::Okay) {
            cancel_data_request(Lua, tiri, source_id, item, callback);
            src.unlock();
            luaL_error(Lua, error, "Failed to request item %d from source #%d: %s", item, source_id,
               GetErrorMsg(error));
         }
      }
      else {
         cancel_data_request(Lua, tiri, source_id, item, callback);
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

   if (lua_type(Lua, 4) != LUA_TFUNCTION) luaL_argerror(Lua, 4, "Function reference required.");

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
   if ((input = new (lua_newuserdata(Lua, sizeof(struct finput))) finput {})) {
      luaL_getmetatable(Lua, "Tiri.input");
      lua_setmetatable(Lua, -2);

      input->SurfaceID = object_id;

      if (capture_tiri_function(Lua, 4, input->Callback) != ERR::Okay) {
         luaL_argerror(Lua, 4, "Function reference required.");
      }

      lua_pushvalue(Lua, lua_gettop(Lua)); // Take a copy of the Tiri.input object
      input->InputValue = luaL_ref(Lua, LUA_REGISTRYINDEX);
      input->Script      = Lua->script;
      input->KeyEvent    = 0;
      input->InputHandle = 0;
      input->Mask        = mask;
      input->Mode        = FIM_DEVICE;
      input->Next        = nullptr;

      auto callback = C_FUNCTION(consume_input_events);
      if ((error = gfx::SubscribeInput(&callback, input->SurfaceID, mask, device_id,
            &input->InputHandle)) != ERR::Okay) {
         if (input->InputHandle) { gfx::UnsubscribeInput(input->InputHandle); input->InputHandle = 0; }
         if (input->InputValue)  { luaL_unref(Lua, LUA_REGISTRYINDEX, input->InputValue); input->InputValue = 0; }
         release_tiri_function(Lua, &input->Callback);
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
      log.traceBranch("Surface: %d, CallbackRef: %u, KeyEvent: %d", input->SurfaceID,
         input->Callback.procedureID(), input->KeyEvent);

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

static ERR key_event(const evKey *Event, APTR Meta)
{
   auto input = (struct finput *)Meta;
   kt::Log log("input.key_event");

   auto tiri = input->Script;
   if (not tiri) {
      log.trace("input->Script undefined.");
      return ERR::Okay;
   }

   log.traceBranch("Incoming keyboard input");

   auto lua = tiri->Lua;
   LuaContextRootGuard context_guard(lua);
   int depth = GetResource(RES::LOG_DEPTH); // Required because thrown errors cause the debugger to lose its step position
   int top = lua_gettop(lua);
   LuaCallbackContextGuard callback_context(lua);
   if (push_tiri_function(lua, input->Callback, callback_context) != ERR::Okay) {
      log.warning("Keyboard input callback is no longer valid.");
      return ERR::Okay;
   }
   lua_rawgeti(lua, LUA_REGISTRYINDEX, input->InputValue); // Arg: Input value registered by the client
   lua_pushinteger(lua, input->SurfaceID);  // Arg: Surface (if applicable)
   lua_pushinteger(lua, uint32_t(Event->Qualifiers)); // Arg: Key Flags
   lua_pushinteger(lua, int(Event->Code));       // Arg: Key Value
   lua_pushinteger(lua, Event->Unicode);    // Arg: Unicode character

   ERR result = ERR::Okay;
   if (lua_pcall(lua, 5, 1, 0)) {
      if (lua->CaughtError IS ERR::Terminate) result = ERR::Terminate;
      process_callback_error(tiri, "Keyboard event callback");
   }
   else if (lua_type(lua, -1) IS LUA_TNUMBER) result = ERR(lua_tointeger(lua, -1));

   lua_settop(lua, top);
   SetResource(RES::LOG_DEPTH, depth);

   if (result IS ERR::Terminate) release_input_subscription(lua, input);
   collect_garbage(lua);
   return result;
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
   reg_iface_prototype("input", "keyboard", { TiriType::Any }, { TiriType::Any, TiriType::Func, TiriType::Table });
   reg_iface_prototype("input", "requestItem", {}, { TiriType::Any, TiriType::Num, TiriType::Any, TiriType::Func });
}
