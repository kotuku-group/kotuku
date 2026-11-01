/*********************************************************************************************************************

-CATEGORY-
Name: Input
-END-

*********************************************************************************************************************/

#include "defs.h"

static ankerl::unordered_dense::map<int, InputCallback> glInputCallbacks;
static std::vector<std::pair<int, InputCallback>> glNewSubscriptions;
std::vector<InputEvent> glInputEvents;

namespace gfx {

/*********************************************************************************************************************

-FUNCTION-
GetInputTypeName: Returns the string name for an input type.

This function converts `JET` integer constants to their string equivalent.

-INPUT-
int(JET) Type: JET type integer.

-RESULT-
cstr: A string describing the input `Type` is returned, or `NULL` if the `Type` is invalid.

-TAGS-
static-result, null-terminated-result, nullable-result, pure-query

*********************************************************************************************************************/

CSTRING GetInputTypeName(JET Type)
{
   if ((int(Type) < 1) or (int(Type) >= int(JET::END))) return nullptr;
   return glInputNames[int(Type)];
}

/*********************************************************************************************************************

-FUNCTION-
SubscribeInput: Subscribe to incoming input messages for any active surface object.

The SubscribeInput() function provides a systematic way of receiving input events as they occur.  Coverage is limited
to device events that are linked to the display (i.e. events from track pads, mouse pointers, graphics tablets and
touch screens).  Keyboard devices are not included.

The client is required to remove the subscription with ~UnsubscribeInput() once tracking is no longer required.

Input events can be filtered so that they are received in relation to surfaces and devices.  An input mask can also be
applied so that only certain types of events are received.

A callback is required for receiving the input events.  The following C++ code illustrates a method for processing
events in the callback:

<pre>
ERR consume_input_events(const InputEvent *Events, int Handle)
{
   for (auto e=Events; e; e=e->Next) {
      if (((e->Flags & JTYPE::BUTTON) != JTYPE::NIL) and (e->Value > 0)) {
         process_click(Self, e->RecipientID, e->X, e->Y);
      }
   }

   return ERR::Okay;
}
</pre>

All processable events are referenced in the !InputEvent structure in the `Events` parameter.

`JET` constants are as follows and take note of `CROSSED_IN` and `CROSSED_OUT` which are software generated and not
a device event:

<types lookup="JET"/>

The `JTYPE` values for the `Flags` field are as follows.  Note that these flags also serve as input masks for the
SubscribeInput() function, so to receive a message of the given type the appropriate `JTYPE` flag must have been set in the
original subscription call.

<types lookup="JTYPE"/>

-INPUT-
ptr(func) Callback: Reference to a callback function that will receive input messages.
oid SurfaceFilter: Optional.  Only the input messages that match the given @Surface ID will be received.
int(JTYPE) Mask: Combine #JTYPE flags to define the input messages required by the client.  Set to `0xffffffff` if all messages are required.
oid DeviceFilter: Optional.  Only the input messages that match the given device ID will be received.  NOTE - Support not yet implemented, set to zero.
&int Handle: A handle for the subscription is returned here.

-ERRORS-
Okay:
NullArgs:

-TAGS-
callback-held, does-not-take-ownership, blocking

*********************************************************************************************************************/

ERR SubscribeInput(FUNCTION *Callback, OBJECTID SurfaceFilter, JTYPE InputMask, OBJECTID DeviceFilter, int *Handle)
{
   static int counter = 1;
   kt::Log log(__FUNCTION__);
   bool retained_callback = false;

   auto consume_callback = kt::Defer([&]() {
      if ((Callback) and (not retained_callback)) Callback->consume();
   });

   if ((!Callback) or (!Handle)) return log.warning(ERR::NullArgs);

   log.branch("Surface Filter: #%d, Mask: $%.4x", SurfaceFilter, int(InputMask));

   const std::lock_guard<std::recursive_mutex> lock(glInputLock);

   *Handle = counter++;

   const InputCallback is = {
      .SurfaceFilter = SurfaceFilter,
      .InputMask     = (InputMask IS JTYPE::NIL) ? JTYPE(0xffff) : JTYPE(InputMask),
      .Callback      = *Callback
   };

   glInputCallbacks.emplace(*Handle, is);
   Callback->pin();
   retained_callback = true;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
UnsubscribeInput: Removes an input subscription.

This function removes an input subscription that has been created with ~SubscribeInput().

-INPUT-
int Handle: Reference to a handle returned by ~SubscribeInput().

-ERRORS-
Okay
NullArgs
NotFound

-TAGS-
closes-handle, blocking
-END-

*********************************************************************************************************************/

ERR UnsubscribeInput(int Handle)
{
   kt::Log log(__FUNCTION__);

   if (!Handle) return log.warning(ERR::NullArgs);

   log.branch("Handle: %d", Handle);

   const std::lock_guard<std::recursive_mutex> lock(glInputLock);

   auto it = glInputCallbacks.find(Handle);
   if (it IS glInputCallbacks.end()) return log.warning(ERR::NotFound);
   else {
      release_display_callback(it->second.Callback);
      glInputCallbacks.erase(it);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
SetPrimarySelection: Publishes UTF-8 text to the X11 primary selection.

The primary selection is the X11 mechanism that allows the most recently highlighted text to be pasted into another
application with a middle mouse click.  It operates independently of the @Clipboard, which remains the appropriate
mechanism for explicit cut, copy and paste operations.  Typically an application calls SetPrimarySelection() whenever
the user changes the highlighted text in an editable or selectable area.

On success the display claims ownership of the primary selection and retains a copy of `Text`, which is served to
other applications on request until ownership is lost.  Ownership is lost when another client claims the primary
selection, at which point the retained copy is discarded.  Calling this function with an empty `Text` value releases
ownership if the display currently holds it.

Only the X11 display driver implements the primary selection.  Other platforms return `ERR::NoSupport`, which clients
should treat as a benign result.

-INPUT-
strview Text: The UTF-8 text to publish.  An empty string releases ownership of the primary selection.

-ERRORS-
Okay: The text was published, or ownership was released.
NoSupport: The display driver does not support the primary selection, or no display connection is open.
Failed: The X server refused the ownership claim.

-TAGS-
copies-input, blocking
-END-

*********************************************************************************************************************/

ERR SetPrimarySelection(const std::string_view &Text)
{
   return glDriver ? glDriver->setPrimarySelection(Text) : ERR::NoSupport;
}

/*********************************************************************************************************************

-FUNCTION-
ReadPrimarySelection: Requests the X11 primary selection asynchronously.

ReadPrimarySelection() requests the current content of the X11 primary selection, which is the text most recently
highlighted in any application.  It is typically called in response to a middle mouse click in order to paste that
text.  The function returns immediately and the text is delivered later through the event loop.

When the text arrives it is sent to the `Recipient` object as a DataFeed of type `DATA::RECEIPT`, in the following
XML format:

<pre>
&lt;receipt id="Item"&gt;&lt;text&gt;Selected text&lt;/text&gt;&lt;/receipt&gt;
</pre>

The `id` attribute repeats the `Item` value so that the recipient can match the receipt to its request.  The text is
UTF-8, with ampersands, angle brackets and carriage returns escaped as XML entities.  Text that is provided in
Latin-1 by the selection owner is converted to UTF-8.

Only one request can be pending at a time.  Starting a new request cancels any request still in progress, and late
replies to a superseded request are discarded.  No receipt is sent if the owner cannot supply text, the transfer
exceeds the size limit, or the owner fails to respond within two seconds.  Clients must not wait on a receipt;
an `ERR::Okay` result only confirms that the request was started.

Only the X11 display driver implements the primary selection.  Other platforms return `ERR::NoSupport`.

-INPUT-
oid Recipient: The object that will receive the `DATA::RECEIPT` DataFeed.
int Item: A client-defined identifier that is returned in the `id` attribute of the receipt.

-ERRORS-
Okay: The request was started.  This does not imply that text will be delivered.
NoSupport: The display driver does not support the primary selection, or no display connection is open.
NoData: No application currently owns the primary selection.
NullArgs: The `Recipient` is not defined.

-TAGS-
non-blocking
-END-

*********************************************************************************************************************/

ERR ReadPrimarySelection(OBJECTID Recipient, int Item)
{
   if (not Recipient) return ERR::NullArgs;
   return glDriver ? glDriver->readPrimarySelection(Recipient, Item) : ERR::NoSupport;
}

} // namespace

//********************************************************************************************************************
// This routine is called on every cycle of ProcessMessages() so that we can check if there are input events
// that need to be processed.
//
// Input events are sent to each subscriber as a dynamically constructed linked-list of filtered input events.
//
// The copying of events isn't necessarily optimal in most cases, but it is the safest methodology and prevents
// issues from arising if the event queue is modified during the callback.

struct input_call {
   int handle;
   OBJECTID surface_filter;
   JTYPE input_mask;
   FUNCTION callback;
   std::vector<InputEvent> events;

   input_call(int Handle, const InputCallback &Input) :
      handle(Handle), surface_filter(Input.SurfaceFilter), input_mask(Input.InputMask), callback(Input.Callback) {
      if (callback.defined()) callback.pin();
   }

   ~input_call() {
      if (callback.defined()) callback.unpin();
   }

   input_call(input_call &&Other) noexcept:
      handle(Other.handle),
      surface_filter(Other.surface_filter),
      input_mask(Other.input_mask),
      callback(Other.callback),
      events(std::move(Other.events))
   {
      Other.callback.clear();
   }

   input_call(const input_call &) = delete;
   input_call & operator=(const input_call &) = delete;
   input_call & operator=(input_call &&) = delete;
};

static bool input_event_match(const InputEvent &Event, const input_call &Sub)
{
   return (((Event.RecipientID IS Sub.surface_filter) or (!Sub.surface_filter)) and
      ((Event.Flags & Sub.input_mask) != JTYPE::NIL));
}

void input_event_loop(HOSTHANDLE FD, APTR Data) // Data is undefined
{
   std::vector<InputEvent> input_events;
   std::vector<input_call> input_buffer;

   {
      const std::lock_guard<std::recursive_mutex> lock(glInputLock);

      if (glInputEvents.empty()) return;

      input_events.reserve(glInputEvents.size());
      input_events.assign(glInputEvents.begin(), glInputEvents.end());
      glInputEvents.clear();

      // Buffer the callbacks that we need to make so that no conflicts occur with the input event queue
      // or the callback queue.

      input_buffer.reserve(glInputCallbacks.size());

      for (auto it = glInputCallbacks.begin(); it != glInputCallbacks.end(); ) {
         if (it->second.Callback.stale()) {
            release_display_callback(it->second.Callback);
            it = glInputCallbacks.erase(it);
         }
         else {
            input_buffer.emplace_back(it->first, it->second);
            it++;
         }
      }
   }

   for (auto &sub : input_buffer) {
      for (auto &event : input_events) {
         if (input_event_match(event, sub)) sub.events.push_back(event);
      }

      for (size_t i=1; i < sub.events.size(); i++) {
         sub.events[i - 1].Next = &sub.events[i];
      }

      if (not sub.events.empty()) sub.events.back().Next = nullptr;
   }

   for (auto &sub : input_buffer) {
      if (sub.events.empty()) continue;

      {
         // An earlier callback in this cycle may have removed the subscription, e.g. by freeing its owner.
         const std::lock_guard<std::recursive_mutex> lock(glInputLock);
         if (not glInputCallbacks.contains(sub.handle)) continue;
      }

      auto &cb = sub.callback;
      if (sub.callback.isC()) {
         kt::ScopedObjectLock lock(OBJECTPTR(cb.Context), 2000); // Ensure that the object can't be removed until after input processing
         if (lock.granted()) {
            kt::SwitchContext ctx(cb.Context);
            auto func = (ERR (*)(InputEvent *, int, APTR))cb.Routine;
            func(sub.events.data(), sub.handle, cb.Meta);
         }
      }
      else if (sub.callback.isScript()) {
         sc::Call(sub.callback, std::to_array<ScriptArg>({
            { "InputEvent:Events", sub.events.data(), FD_PTR|FDF_STRUCT },
            { "Handle", sub.handle }
         }));
      }
   }
}
