This folder contains the source code for the Network module.

Points to remember:

* Network sockets are non-blocking, and the code model is designed for this
* Windows uses the IOCP backend in @win32/iocp.cpp for asynchronous network activity
* For Linux, system network messsages are managed by listening to file descriptors, which is achieved with RegisterFD()
* Windows uses the native SSL library, other platforms use OpenSSL
* Windows socket system calls are managed in @win32/iocp.cpp
* Most debug messages in the Network module are trace only.  To see them in the log output, use a debug build with `KOTUKU_VLOG` enabled.
