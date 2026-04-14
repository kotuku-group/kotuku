This folder contains the source code for the Network module.

Points to remember:

* Network sockets are non-blocking, and the code model is designed for this
* For Windows, system network messages are handled in @winew/winsockwrappers.cpp win_messages() and dispatched from there
* For Linux, system network messsages are managed by listening to file descriptors, which is achieved with RegisterFD()
* Windows uses the native SSL library, other platforms use OpenSSL
* System calls to Win32 functions are always wrapped and managed in @win32/winsockwrappers.cpp
* Most debug messages in the Network module are trace only.  To see them in the log output, use a debug build with `KOTUKU_VLOG` enabled.
