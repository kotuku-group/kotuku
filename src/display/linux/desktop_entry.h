#pragma once

// Helpers for integrating hosted windows with freedesktop.org desktop entries.  GNOME resolves a window's name and
// icon exclusively from the .desktop entry that matches its X11 class or Wayland app ID, so apps that define their
// own icon need a generated entry.  These functions are independent of the Kotuku runtime so that they can be unit
// tested in isolation.

#include <kotuku/config.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace desktop_entry {

struct Entry {
   std::string AppID;      // Also the X11 class, Wayland app ID and StartupWMClass
   std::string Name;       // Application name displayed by the desktop
   std::string Exec;       // Absolute path of the executable
   std::string Icon;       // Absolute path of the icon file
   std::string Source;     // The icon source string, recorded for diagnostics
   uint32_t IconHash = 0;  // Hash of the rendered icon pixels, used to detect changes to the source image
};

// Returns true if an XDG_CURRENT_DESKTOP value identifies a GNOME session.

bool is_gnome_desktop(std::string_view CurrentDesktop);

// Returns a stable app ID derived from the icon source, e.g. "org.kotuku.Origo.Icon-1a2b3c4d".

std::string app_id(std::string_view IconSource);

// Returns a 32-bit FNV-1a hash of Data.

uint32_t fnv1a(std::string_view Data);

// Quotes a value for use as an argument in an Exec key, as defined by the Desktop Entry specification.

std::string quote_exec_arg(std::string_view Value);

// Escapes a value for use in a string key.  Line breaks and other control characters are neutralised.

std::string escape_value(std::string_view Value);

// Reverses escape_value().

std::string unescape_value(std::string_view Value);

// Returns the unescaped value of Key from the [Desktop Entry] group of the file at Path, or an empty string if the
// file or key is not present.

std::string read_key(const std::filesystem::path &Path, std::string_view Key);

// Returns the text of a generated .desktop entry.

std::string format_entry(const Entry &Entry);

// Returns $XDG_DATA_HOME, or $HOME/.local/share if it is undefined.  Returns an empty path if neither is available.

std::filesystem::path data_home();

// Creates Path and any missing parent folders with mode 0755.  Explicit modes are required because the Kotuku core
// clears the process umask at startup, which would otherwise leave new folders world-writable.

bool create_folders(const std::filesystem::path &Path);

// Returns true if the file at Path already contains Content.

bool file_matches(const std::filesystem::path &Path, std::string_view Content);

// Writes Content to Path by writing a temporary file in the same folder and renaming it into place, so that
// directory monitors never observe a partial file.  The file is created with mode 0644 and missing parent folders
// are created with create_folders().

bool write_atomic(const std::filesystem::path &Path, std::string_view Content);

} // namespace desktop_entry
