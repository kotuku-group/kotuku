#pragma once

// For extremely verbose debug logs, run cmake with -DKOTUKU_VLOG=ON

namespace pf {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-zero-length"

class Log { // C++ wrapper for Kotuku's log functionality
   private:
      int branches;
      CSTRING header;

   public:
      Log() : branches(0), header(nullptr) { }
      Log(CSTRING Header) : branches(0), header(Header) { }

      ~Log() {
         while (branches > 0) { branches--; LogReturn(); }
      }

      void branch(CSTRING Message = "", ...) __attribute__((format(printf, 2, 3))) {
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::API|VLF::BRANCH, header, Message, arg);
         va_end(arg);
         branches++;
      }

      // Cancels de-branching on close

      void resetBranch() { branches = 0; }

      #ifndef NDEBUG
      void traceBranch(CSTRING Message = "", ...) __attribute__((format(printf, 2, 3))) {
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::TRACE|VLF::BRANCH, header, Message, arg);
         va_end(arg);
         branches++;
      }
      #else
      void traceBranch(CSTRING Message = "", ...) __attribute__((format(printf, 2, 3))) { }
      #endif

      inline void debranch() {
         branches--;
         LogReturn();
      }

      void app(CSTRING Message, ...) { // Info level, recommended for applications only
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::INFO, header, Message, arg);
         va_end(arg);
      }

      void msg(CSTRING Message, ...) __attribute__((format(printf, 2, 3))) { // Defaults to API level, recommended for modules
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::API, header, Message, arg);
         va_end(arg);
      }

      void msg(VLF Flags, CSTRING Message, ...) __attribute__((format(printf, 3, 4))) { // Defaults to API level, recommended for modules
         va_list arg;
         va_start(arg, Message);
         VLogF(Flags, header, Message, arg);
         va_end(arg);
         if ((Flags & VLF::BRANCH) != VLF::NIL) branches++;
      }

      void detail(CSTRING Message, ...) __attribute__((format(printf, 2, 3))) { // Detailed API message, '--log-xapi' to view
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::DETAIL, header, Message, arg);
         va_end(arg);
      }

      void pmsg(CSTRING Message, ...) __attribute__((format(printf, 2, 3))) { // "Parent message", uses the scope of the caller
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::API, nullptr, Message, arg);
         va_end(arg);
      }

      void warning(CSTRING Message, ...) __attribute__((format(printf, 2, 3))) {
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::WARNING, header, Message, arg);
         va_end(arg);
      }

      void error(CSTRING Message, ...) __attribute__((format(printf, 2, 3))) { // NB: Use for messages intended for the user, not the developer
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::ERROR, header, Message, arg);
         va_end(arg);
      }

      void function(CSTRING Message = "", ...) __attribute__((format(printf, 2, 3))) { // Equivalent to branch() but without a new branch being created
         va_list arg;
         va_start(arg, Message);
         VLogF(VLF::API|VLF::FUNCTION, header, Message, arg);
         va_end(arg);
      }

      inline ERR error(ERR Code) { // Technically a warning
         FuncError(header, Code);
         return Code;
      }

      inline ERR warning(ERR Code) {
         FuncError(header, Code);
         return Code;
      }

      void trace(CSTRING Message, ...) {
         #ifndef NDEBUG
            va_list arg;
            va_start(arg, Message);
            VLogF(VLF::TRACE, header, Message, arg);
            va_end(arg);
         #endif
      }

      void traceWarning(CSTRING Message, ...) {
         #ifndef NDEBUG
            va_list arg;
            va_start(arg, Message);
            VLogF(VLF::WARNING, header, Message, arg);
            va_end(arg);
         #endif
      }

      inline ERR traceWarning(ERR Code) {
         #ifndef NDEBUG
            FuncError(header, Code);
         #endif
         return Code;
      }
};

#pragma GCC diagnostic pop

class LogLevel {
   private:
      int level;
   public:
      LogLevel(int Level) : level(Level) {
         AdjustLogLevel(Level);
      }

      ~LogLevel() {
         AdjustLogLevel(-level);
      }
};

} // namespace
