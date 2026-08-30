#include "../../defs.h"

namespace display {

const DriverCallbacks *glWin32Callbacks = nullptr;

void MsgKeyPress(int Flags, int Value, int Printable)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->KeyPressed)) {
      glWin32Callbacks->KeyPressed(KQ(Flags), KEY(Value), Printable);
   }
}

void MsgKeyRelease(int Flags, int Value)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->KeyReleased)) {
      glWin32Callbacks->KeyReleased(KQ(Flags), KEY(Value));
   }
}

void MsgMovement(OBJECTID SurfaceID, double AbsX, double AbsY, int WinX, int WinY, bool NonClient)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->Movement)) {
      glWin32Callbacks->Movement(SurfaceID, AbsX, AbsY, NonClient);
   }
}

void MsgWheelMovement(OBJECTID SurfaceID, float Wheel)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->WheelMovement)) {
      glWin32Callbacks->WheelMovement(SurfaceID, Wheel);
   }
}

void MsgButtonPress(int Buttons, int State)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->ButtonInput)) {
      glWin32Callbacks->ButtonInput(Buttons, bool(State));
   }
}

void MsgFocusState(OBJECTID SurfaceID, int State)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->FocusState)) {
      glWin32Callbacks->FocusState(SurfaceID, bool(State));
   }
}

void MsgControllerPorts(int Port, bool Connected, int Total)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->ControllerPorts)) {
      glWin32Callbacks->ControllerPorts(Port, Connected, Total);
   }
}

void MsgControllerLog(bool Warning, const char *Message)
{
   kt::Log log("ControllerManager");
   if (Warning) log.warning("%s", Message);
   else log.msg("%s", Message);
}

void MsgClipboardUpdated()
{
   if ((glWin32Callbacks) and (glWin32Callbacks->ClipboardUpdated)) glWin32Callbacks->ClipboardUpdated();
}

void MsgEnableDragDrop(HWND Window)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->EnableDragDrop)) glWin32Callbacks->EnableDragDrop(Window);
}

void MsgDisableDragDrop(HWND Window)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->DisableDragDrop)) glWin32Callbacks->DisableDragDrop(Window);
}

void MsgResizedWindow(OBJECTID SurfaceID, int WinX, int WinY, int WinWidth, int WinHeight,
   int ClientX, int ClientY, int ClientWidth, int ClientHeight)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->WindowResized)) {
      glWin32Callbacks->WindowResized(SurfaceID, WinX, WinY, WinWidth, WinHeight,
         ClientX, ClientY, ClientWidth, ClientHeight);
   }
}

void RepaintWindow(OBJECTID SurfaceID, int X, int Y, int Width, int Height)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->ExposeRegion)) {
      glWin32Callbacks->ExposeRegion(SurfaceID, X, Y, Width, Height);
   }
}

void MsgWindowClose(OBJECTID SurfaceID)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->WindowClose)) glWin32Callbacks->WindowClose(SurfaceID);
}

void MsgWindowDestroyed(OBJECTID SurfaceID)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->WindowDestroyed)) glWin32Callbacks->WindowDestroyed(SurfaceID);
}

void MsgDPIChanged(OBJECTID SurfaceID)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->DPIChanged)) glWin32Callbacks->DPIChanged(SurfaceID);
}

void MsgSetFocus(OBJECTID SurfaceID)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->SetFocus)) glWin32Callbacks->SetFocus(SurfaceID);
}

void CheckWindowSize(OBJECTID SurfaceID, int &Width, int &Height, int CurrentWidth, int CurrentHeight, int Axis)
{
   if ((glWin32Callbacks) and (glWin32Callbacks->ConstrainWindowSize)) {
      glWin32Callbacks->ConstrainWindowSize(SurfaceID, Width, Height, CurrentWidth, CurrentHeight, Axis);
   }
}

void MsgTimer()
{
   if ((glWin32Callbacks) and (glWin32Callbacks->ProcessMessages)) glWin32Callbacks->ProcessMessages();
}

} // namespace display
