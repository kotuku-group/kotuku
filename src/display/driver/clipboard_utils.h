#pragma once

#include <kotuku/modules/core.h>

#include <string>
#include <string_view>
#include <vector>

namespace display {

struct ClipboardFilePayload {
   std::string URIList;
   std::string GnomeFiles;
};

inline ERR encode_clipboard_files(const std::vector<std::string> &Paths, bool Cut, ClipboardFilePayload &Payload)
{
   constexpr char hex[] = "0123456789ABCDEF";
   Payload.URIList.clear();
   Payload.GnomeFiles = Cut ? "cut\n" : "copy\n";

   for (auto &item : Paths) {
      std::string path;
      if (ResolvePath(item, RSF::NIL, &path) != ERR::Okay) continue;
      if (path.empty() or (path[0] != '/')) continue;

      std::string uri("file://");
      for (unsigned char ch : path) {
         if (((ch >= 'A') and (ch <= 'Z')) or ((ch >= 'a') and (ch <= 'z')) or
               ((ch >= '0') and (ch <= '9')) or (ch IS '/') or (ch IS '-') or (ch IS '_') or
               (ch IS '.') or (ch IS '~')) uri.push_back(char(ch));
         else {
            uri.push_back('%');
            uri.push_back(hex[ch >> 4]);
            uri.push_back(hex[ch & 15]);
         }
      }
      Payload.URIList.append(uri).append("\r\n");
      Payload.GnomeFiles.append(uri).push_back('\n');
   }

   if (Payload.URIList.empty()) {
      Payload.GnomeFiles.clear();
      return ERR::InvalidPath;
   }
   return ERR::Okay;
}

}
