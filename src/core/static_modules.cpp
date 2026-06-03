
#ifdef KOTUKU_STATIC

extern "C" ModHeader * register_audio_module();
extern "C" ModHeader * register_backstage_module();
extern "C" ModHeader * register_display_module();
extern "C" ModHeader * register_document_module();
extern "C" ModHeader * register_tiri_module();
extern "C" ModHeader * register_font_module();
extern "C" ModHeader * register_http_module();
extern "C" ModHeader * register_jpeg_module();
extern "C" ModHeader * register_json_module();
extern "C" ModHeader * register_mp3_module();
extern "C" ModHeader * register_network_module();
extern "C" ModHeader * register_image_module();
extern "C" ModHeader * register_regex_module();
extern "C" ModHeader * register_scintilla_module();
extern "C" ModHeader * register_svg_module();
extern "C" ModHeader * register_vector_module();
extern "C" ModHeader * register_xml_module();
extern "C" ModHeader * register_xquery_module();

//********************************************************************************************************************
// Register all static modules that were compiled into this build.

static void register_static_modules(void)
{
   #ifdef INC_MOD_AUDIO
   glStaticModules["audio"] = register_audio_module();
   #endif

   #ifdef INC_MOD_BACKSTAGE
   glStaticModules["backstage"] = register_backstage_module();
   #endif

   #ifdef INC_MOD_DISPLAY
   glStaticModules["display"] = register_display_module();
   #endif

   #ifdef INC_MOD_DOCUMENT
   glStaticModules["document"] = register_document_module();
   #endif

   #ifdef INC_MOD_TIRI
   glStaticModules["tiri"] = register_tiri_module();
   #endif

   #ifdef INC_MOD_FONT
   glStaticModules["font"] = register_font_module();
   #endif

   #ifdef INC_MOD_HTTP
   glStaticModules["http"] = register_http_module();
   #endif

   #ifdef INC_MOD_JPEG
   glStaticModules["jpeg"] = register_jpeg_module();
   #endif

   #ifdef INC_MOD_JSON
   glStaticModules["json"] = register_json_module();
   #endif

   #ifdef INC_MOD_MP3
   glStaticModules["mp3"] = register_mp3_module();
   #endif

   #ifdef INC_MOD_NETWORK
   glStaticModules["network"] = register_network_module();
   #endif

   #ifdef INC_MOD_IMAGE
   glStaticModules["image"] = register_image_module();
   #endif

   #ifdef INC_MOD_REGEX
   glStaticModules["regex"] = register_regex_module();
   #endif

   #ifdef INC_MOD_SCINTILLA
   glStaticModules["scintilla"] = register_scintilla_module();
   #endif

   #ifdef INC_MOD_SVG
   glStaticModules["svg"] = register_svg_module();
   #endif

   #ifdef INC_MOD_VECTOR
   glStaticModules["vector"] = register_vector_module();
   #endif

   #ifdef INC_MOD_XML
   glStaticModules["xml"] = register_xml_module();
   #endif

   #ifdef INC_MOD_XQUERY
   glStaticModules["xquery"] = register_xquery_module();
   #endif
}

#endif
