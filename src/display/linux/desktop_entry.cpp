// See desktop_entry.h.  References are to the freedesktop.org Desktop Entry Specification.

#include "desktop_entry.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace desktop_entry {

//********************************************************************************************************************

static char lower_ascii(char Value)
{
   return ((Value >= 'A') and (Value <= 'Z')) ? char(Value - 'A' + 'a') : Value;
}

static std::string hex32(uint32_t Value)
{
   char buffer[9];
   std::snprintf(buffer, sizeof(buffer), "%08x", Value);
   return buffer;
}

//********************************************************************************************************************
// XDG_CURRENT_DESKTOP is a colon-separated list, e.g. "ubuntu:GNOME" or "GNOME-Classic:GNOME".

bool is_gnome_desktop(std::string_view CurrentDesktop)
{
   while (not CurrentDesktop.empty()) {
      auto sep = CurrentDesktop.find(':');
      auto name = CurrentDesktop.substr(0, sep);

      if (name.size() IS 5) {
         std::string lower;
         for (auto ch : name) lower += lower_ascii(ch);
         if (lower IS "gnome") return true;
      }

      if (sep IS std::string_view::npos) break;
      CurrentDesktop.remove_prefix(sep + 1);
   }
   return false;
}

//********************************************************************************************************************

uint32_t fnv1a(std::string_view Data)
{
   uint32_t hash = 2166136261u;
   for (auto ch : Data) {
      hash ^= uint8_t(ch);
      hash *= 16777619u;
   }
   return hash;
}

std::string app_id(std::string_view IconSource)
{
   return "org.kotuku.Origo.Icon-" + hex32(fnv1a(IconSource));
}

//********************************************************************************************************************
// Arguments containing reserved characters must be quoted, and within quotes the characters `"`, `` ` ``, `$` and
// `\` are escaped with a backslash.  A literal `%` is always written as `%%` because it introduces field codes.

std::string quote_exec_arg(std::string_view Value)
{
   static constexpr std::string_view reserved = " \t\n\"'\\><~|&;$*?#()`";

   bool quote = Value.empty();
   for (auto ch : Value) {
      if (reserved.find(ch) != std::string_view::npos) { quote = true; break; }
   }

   std::string result;
   if (quote) result += '"';
   for (auto ch : Value) {
      if (ch IS '%') result += "%%";
      else if (quote and ((ch IS '"') or (ch IS '`') or (ch IS '$') or (ch IS '\\'))) {
         result += '\\';
         result += ch;
      }
      else result += ch;
   }
   if (quote) result += '"';
   return result;
}

//********************************************************************************************************************
// String values support the escape sequences \s, \n, \t, \r and \\.  Other control characters are dropped, as
// they cannot be represented and would otherwise corrupt the line structure of the file.

std::string escape_value(std::string_view Value)
{
   std::string result;
   result.reserve(Value.size());
   bool leading = true;
   for (auto ch : Value) {
      if ((ch IS ' ') and leading) { result += "\\s"; continue; }
      leading = false;

      switch (ch) {
         case '\\': result += "\\\\"; break;
         case '\n': result += "\\n"; break;
         case '\t': result += "\\t"; break;
         case '\r': result += "\\r"; break;
         default:
            if ((unsigned char)ch >= 0x20) result += ch;
            break;
      }
   }
   return result;
}

//********************************************************************************************************************

std::string unescape_value(std::string_view Value)
{
   std::string result;
   result.reserve(Value.size());
   for (size_t i=0; i < Value.size(); i++) {
      if ((Value[i] IS '\\') and (i + 1 < Value.size())) {
         switch (Value[++i]) {
            case 's':  result += ' '; break;
            case 'n':  result += '\n'; break;
            case 't':  result += '\t'; break;
            case 'r':  result += '\r'; break;
            case '\\': result += '\\'; break;
            default:   result += '\\'; result += Value[i]; break;
         }
      }
      else result += Value[i];
   }
   return result;
}

//********************************************************************************************************************

std::string read_key(const std::filesystem::path &Path, std::string_view Key)
{
   std::ifstream file(Path, std::ios::binary);
   if (not file) return { };

   bool in_group = false;
   std::string line;
   while (std::getline(file, line)) {
      if (line.starts_with('[')) {
         in_group = (line IS "[Desktop Entry]");
         continue;
      }

      if (in_group and line.starts_with(Key) and (line.size() > Key.size()) and (line[Key.size()] IS '=')) {
         return unescape_value(std::string_view(line).substr(Key.size() + 1));
      }
   }
   return { };
}

//********************************************************************************************************************
// NoDisplay hides the entry from application menus while still allowing the desktop to match windows against it.

std::string format_entry(const Entry &Entry)
{
   std::string text = "[Desktop Entry]\n"
      "Type=Application\n"
      "Name=" + escape_value(Entry.Name.empty() ? std::string_view("Origo") : std::string_view(Entry.Name)) + "\n"
      "Exec=" + escape_value(quote_exec_arg(Entry.Exec)) + "\n"
      "Icon=" + escape_value(Entry.Icon) + "\n"
      "Terminal=false\n"
      "NoDisplay=true\n"
      "StartupWMClass=" + escape_value(Entry.AppID) + "\n"
      "X-Kotuku-Generated=true\n"
      "X-Kotuku-Source=" + escape_value(Entry.Source) + "\n"
      "X-Kotuku-IconHash=" + hex32(Entry.IconHash) + "\n";
   return text;
}

//********************************************************************************************************************
// The specification requires XDG_DATA_HOME to be an absolute path; relative values are ignored.

std::filesystem::path data_home()
{
   if (auto xdg = std::getenv("XDG_DATA_HOME"); xdg and (xdg[0] IS '/')) return xdg;
   if (auto home = std::getenv("HOME"); home and (home[0] IS '/')) {
      return std::filesystem::path(home) / ".local" / "share";
   }
   return { };
}

//********************************************************************************************************************

bool file_matches(const std::filesystem::path &Path, std::string_view Content)
{
   std::ifstream file(Path, std::ios::binary);
   if (not file) return false;
   std::string existing((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
   return existing IS Content;
}

//********************************************************************************************************************
// Folders that already exist are left unchanged.  A mode passed to mkdir() is still reduced by any umask that the
// process has set, so the result is never more permissive than 0755.

bool create_folders(const std::filesystem::path &Path)
{
   std::filesystem::path current;
   for (auto &part : Path) {
      current /= part;
      if ((mkdir(current.c_str(), 0755) != 0) and (errno != EEXIST)) return false;
   }

   std::error_code error;
   return std::filesystem::is_directory(Path, error);
}

//********************************************************************************************************************
// The temporary name includes the process ID so that concurrent processes writing the same entry cannot interfere
// with each other.  The rename is atomic, so the last writer wins with a complete file.

bool write_atomic(const std::filesystem::path &Path, std::string_view Content)
{
   if (not create_folders(Path.parent_path())) return false;

   auto temp = Path.parent_path() / ("." + Path.filename().string() + ".tmp-" + std::to_string(getpid()));

   auto fd = open(temp.c_str(), O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644);
   if (fd IS -1) return false;

   bool ok = true;
   size_t written = 0;
   while (written < Content.size()) {
      auto result = write(fd, Content.data() + written, Content.size() - written);
      if (result < 0) {
         if (errno IS EINTR) continue;
         ok = false;
         break;
      }
      written += size_t(result);
   }
   if (close(fd) != 0) ok = false;

   std::error_code error;
   if (ok) {
      std::filesystem::rename(temp, Path, error);
      if (not error) return true;
   }
   std::filesystem::remove(temp, error);
   return false;
}

} // namespace desktop_entry
