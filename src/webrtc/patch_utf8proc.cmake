# Local fix for pinned utf8proc a1b99daa: its sizing pass deliberately passes a null output buffer.
# Avoid pointer arithmetic on that buffer, and on exhausted output buffers during recursive decomposition.
# Run on every configure so existing FetchContent trees receive the fix too.  Repeated application is a no-op.
function (patch_utf8proc Source)
   file (READ "${Source}" CONTENT)
   set (ORIGINAL "${CONTENT}")
   foreach (PAIR IN ITEMS
      "entry_cp, dst+written,|entry_cp, (bufsize > written) ? dst + written : NULL,"
      "uc, buffer + wpos,|uc, (bufsize > wpos) ? buffer + wpos : NULL,")
      string (REPLACE "|" ";" PARTS "${PAIR}")
      list (GET PARTS 0 BEFORE)
      list (GET PARTS 1 AFTER)
      string (FIND "${CONTENT}" "${BEFORE}" POSITION)
      if (POSITION EQUAL -1)
         string (FIND "${CONTENT}" "${AFTER}" POSITION)
         if (POSITION EQUAL -1)
            message (FATAL_ERROR "utf8proc sizing patch no longer matches; review the dependency before updating it.")
         endif ()
      else ()
         string (REPLACE "${BEFORE}" "${AFTER}" CONTENT "${CONTENT}")
      endif ()
   endforeach ()
   if (NOT CONTENT STREQUAL ORIGINAL)
      file (WRITE "${Source}" "${CONTENT}")
   endif ()
endfunction ()
