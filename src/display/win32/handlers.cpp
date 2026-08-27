namespace display {

void MsgKeyPress(int Flags, int Value, int Printable) { DriverKeyPress(KQ(Flags), KEY(Value), Printable); }
void MsgKeyRelease(int Flags, int Value) { DriverKeyRelease(KQ(Flags), KEY(Value)); }

void MsgMovement(OBJECTID SurfaceID, double AbsX, double AbsY, int WinX, int WinY, bool NonClient)
{
   DriverMovement(SurfaceID, AbsX, AbsY, NonClient);
}

void MsgWheelMovement(OBJECTID SurfaceID, float Wheel) { DriverWheelMovement(SurfaceID, Wheel); }
void MsgButtonPress(int Buttons, int State) { DriverButtonInput(Buttons, bool(State)); }
void MsgFocusState(OBJECTID SurfaceID, int State) { DriverFocusState(SurfaceID, bool(State)); }

void MsgResizedWindow(OBJECTID SurfaceID, int WinX, int WinY, int WinWidth, int WinHeight,
   int ClientX, int ClientY, int ClientWidth, int ClientHeight)
{
   DriverWindowResized(SurfaceID, WinX, WinY, WinWidth, WinHeight,
      ClientX, ClientY, ClientWidth, ClientHeight);
}

void RepaintWindow(OBJECTID SurfaceID, int X, int Y, int Width, int Height)
{
   DriverExposeRegion(SurfaceID, X, Y, Width, Height);
}

void MsgWindowClose(OBJECTID SurfaceID) { DriverWindowClose(SurfaceID); }
void MsgWindowDestroyed(OBJECTID SurfaceID) { DriverWindowDestroyed(SurfaceID); }
void MsgDPIChanged(OBJECTID SurfaceID) { DriverDPIChanged(SurfaceID); }
void MsgSetFocus(OBJECTID SurfaceID) { DriverSetFocus(SurfaceID); }

} // namespace
