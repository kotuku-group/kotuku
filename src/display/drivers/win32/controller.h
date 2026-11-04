#pragma once

namespace display {

ERR winReadController(int Port, double *Values, CON &Buttons);
ERR winGetControllerPorts(int &Value);
void winControllerSetWindow(HWND Window, bool Enabled);
void winControllerRemoveWindow(HWND Window);
void winControllerActivateWindow(HWND Window);
void winControllerMarkDirty(HWND Window);
void winControllerShutdown();

} // namespace display
