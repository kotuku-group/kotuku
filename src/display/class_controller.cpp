/*********************************************************************************************************************

-CLASS-
Controller: Provides support for reading state-based game controllers.

Use the Controller class to read the state of game controllers that are recognised by the operating system.

Unlike analog devices that stream input commands (e.g. mice), gamepad controllers maintain a state that can be read
at any time.  The controller state is normally read at least once per frame, which can be achieved in a program's
inner loop, or in a separate timer.

On Linux, controllers are read through the `/dev/input/js*` joystick API.  The user running the application must have
read access to these device nodes.

Controller input management is governed by the @Display class.  The `GRAB_CONTROLLERS` flag should be defined in the
active @Display.Flags field in order to ensure that controller input can be received from the host.  Failure to do
so may mean that the Controller object works inconsistently across different systems.

-END-

*********************************************************************************************************************/

#include "defs.h"

#ifdef _WIN32
using namespace display;
#endif

#ifdef __linux__
extern ERR linuxReadController(int Port, double *Values, CON &Buttons);
extern ERR linuxGetControllerPorts(int &Value);
#endif

/*********************************************************************************************************************
-ACTION-
Query: Get the current controller state.

Query will update the controller field values with the state of the controller connected to the specified port.
The most likely failure is a disconnection - if this occurs the port number will be set to `-1` and
`ERR::Disconnected` is returned.  Repeated calls to Query() will return the same error until a controller is
connected to any port.

-ERRORS-
Okay:
NoSupport: The host does not support controller input.
Disconnected: No controller is connected to the specified port.
OutOfRange: The port number is outside of acceptable range.
SystemCall: A call to the host system failed.
-END-
*********************************************************************************************************************/

static ERR CONTROLLER_Query(objController *Self)
{
#ifdef _WIN32
   return winReadController(Self->Port, (double *)&Self->LeftTrigger, Self->Buttons);
#elif defined(__linux__)
   return linuxReadController(Self->Port, (double *)&Self->LeftTrigger, Self->Buttons);
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-FIELD-
LeftTrigger: Left trigger value between 0.0 and 1.0.

-FIELD-
RightTrigger: Right trigger value between 0.0 and 1.0.

-FIELD-
LeftStickX: Left analog stick value for X axis, between -1.0 and 1.0.

-FIELD-
LeftStickY: Left analog stick value for Y axis, between -1.0 and 1.0.

-FIELD-
RightStickX: Right analog stick value for X axis, between -1.0 and 1.0.

-FIELD-
RightStickY: Right analog stick value for Y axis, between -1.0 and 1.0.

-FIELD-
Buttons: Button values expressed as bit-fields.

-FIELD-
Port: The port number assigned to the controller.

Set the port number to choose the controller that will be queried for state changes.  The default of -1 is used
to indicate the primary (first available) controller.  Fixed port numbers start from zero.  There is no guarantee
that the existence of a port means that a controller is connected to it.

It is acceptable to set the port number post-initialisation, so multiple controllers can be queried through one
interface at the cost of overwriting the previous state.  Enumeration for the discovery of controllers can be
achieved by calling #Query() for each port and checking for `ERR::Okay`.

Read #TotalPorts to get the maximum number of controller ports.

-FIELD-
TotalPorts: Reports the number of controller ports that should be scanned.

Port values range from zero to `TotalPorts - 1`.  Some platforms, including Linux, may expose sparse controller
indices, so an individual port in that range can fail to query if its device is not currently connected.

*********************************************************************************************************************/

static ERR CONTROLLER_GET_TotalPorts(extSurface *Self, int &Value)
{
#ifdef _WIN32
   if (glLastPort.load() >= 0) Value = glLastPort.load() + 1;
   else Value = 0;
   return ERR::Okay;
#elif defined(__linux__)
   return linuxGetControllerPorts(Value);
#endif

   return ERR::NoSupport;
}

//********************************************************************************************************************

#include "class_controller_def.c"

static const FieldArray clFields[] = {
   { "LeftTrigger",  FDF_DOUBLE|FDF_R },
   { "RightTrigger", FDF_DOUBLE|FDF_R },
   { "LeftStickX",   FDF_DOUBLE|FDF_R },
   { "LeftStickY",   FDF_DOUBLE|FDF_R },
   { "RightStickX",  FDF_DOUBLE|FDF_R },
   { "RightStickY",  FDF_DOUBLE|FDF_R },
   { "Buttons",      FDF_INT|FDF_R, nullptr, nullptr, &clControllerButtons },
   { "Port",         FDF_INT|FDF_RW },
   { "TotalPorts",   FDF_VIRTUAL|FDF_INT|FDF_R, CONTROLLER_GET_TotalPorts },
   END_FIELD
};

//********************************************************************************************************************

ERR create_controller_class(void)
{
   clController = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::CONTROLLER),
      fl::ClassVersion(VER_CONTROLLER),
      fl::Name("Controller"),
      fl::Category(CCF::IO),
      fl::Actions(clControllerActions),
      fl::Fields(clFields),
      fl::Size(sizeof(objController)),
      fl::Path(MOD_PATH));

   return clController ? ERR::Okay : ERR::AddClass;
}
