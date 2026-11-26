#pragma once

#include <string_view>

// Filter resource references are resolved relative to the SVG document folder.

inline bool safe_filter_path(std::string_view Path)
{
   if (Path.empty() or Path.starts_with('/') or Path.starts_with('\\') or
       (Path.find(':') != std::string_view::npos) or (Path.find('\0') != std::string_view::npos)) return false;

   while (not Path.empty()) {
      const auto separator = Path.find_first_of("/\\");
      if (Path.substr(0, separator) IS "..") return false;
      if (separator IS std::string_view::npos) break;
      Path.remove_prefix(separator + 1);
   }
   return true;
}
