# Tiri LSP tooling

set(TIRI_LSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(TIRI_LSP_VSCODE_PLUGIN_DIR "${TIRI_LSP_DIR}/vscode_plugin")
set(TIRI_LSP_VSCODE_BUNDLE "${TIRI_LSP_VSCODE_PLUGIN_DIR}/dist/extension.js")
set(TIRI_LSP_VSCODE_VSIX "${TIRI_LSP_VSCODE_PLUGIN_DIR}/tiri-language-0.1.0.vsix")

find_program(NPM_EXECUTABLE NAMES npm.cmd npm)

if (NPM_EXECUTABLE)
   set(TIRI_LSP_NPM_INSTALL_COMMAND install)
   set(TIRI_LSP_NPM_INSTALL_DEPENDS)
   if (EXISTS "${TIRI_LSP_VSCODE_PLUGIN_DIR}/package-lock.json")
      set(TIRI_LSP_NPM_INSTALL_COMMAND ci)
      list(APPEND TIRI_LSP_NPM_INSTALL_DEPENDS "${TIRI_LSP_VSCODE_PLUGIN_DIR}/package-lock.json")
   endif ()

   add_custom_command(
      OUTPUT "${TIRI_LSP_VSCODE_BUNDLE}"
      COMMAND "${NPM_EXECUTABLE}" ${TIRI_LSP_NPM_INSTALL_COMMAND}
      COMMAND "${NPM_EXECUTABLE}" run bundle
      WORKING_DIRECTORY "${TIRI_LSP_VSCODE_PLUGIN_DIR}"
      DEPENDS
         "${TIRI_LSP_VSCODE_PLUGIN_DIR}/extension.js"
         "${TIRI_LSP_VSCODE_PLUGIN_DIR}/language-configuration.json"
         "${TIRI_LSP_VSCODE_PLUGIN_DIR}/package.json"
         ${TIRI_LSP_NPM_INSTALL_DEPENDS}
         "${TIRI_LSP_VSCODE_PLUGIN_DIR}/syntaxes/tiri.tmLanguage.json"
      COMMENT "Building the Tiri VS Code plugin bundle with npm"
      VERBATIM)

   add_custom_target(tiri_lsp_vscode_plugin
      DEPENDS "${TIRI_LSP_VSCODE_BUNDLE}")

   add_custom_command(
      OUTPUT "${TIRI_LSP_VSCODE_VSIX}"
      COMMAND "${NPM_EXECUTABLE}" exec --yes -- vsce package
      WORKING_DIRECTORY "${TIRI_LSP_VSCODE_PLUGIN_DIR}"
      DEPENDS
         tiri_lsp_vscode_plugin
         "${TIRI_LSP_VSCODE_PLUGIN_DIR}/.vscodeignore"
         "${TIRI_LSP_VSCODE_PLUGIN_DIR}/README.md"
         "${TIRI_LSP_VSCODE_PLUGIN_DIR}/images/tiri.png"
         "${TIRI_LSP_VSCODE_PLUGIN_DIR}/package.json"
      COMMENT "Packaging the Tiri VS Code plugin VSIX with npm"
      VERBATIM)

   add_custom_target(tiri_lsp_vscode_plugin_vsix
      DEPENDS "${TIRI_LSP_VSCODE_VSIX}")
else ()
   message(STATUS "npm was not found; Tiri VS Code plugin build targets will be unavailable.")
endif ()

if (KOTUKU_INSTALL)
   install (FILES "${TIRI_LSP_VSCODE_VSIX}"
      DESTINATION "${SHARE_TARGET}/tools/tiri_lsp"
      OPTIONAL)
endif ()

flute_test(tiri_lsp_definition "${TIRI_LSP_DIR}/tests/test_definition.tiri")
flute_test(tiri_lsp_document_symbols "${TIRI_LSP_DIR}/tests/test_document_symbols.tiri")
flute_test(tiri_lsp_hover "${TIRI_LSP_DIR}/tests/test_hover.tiri")
flute_test(tiri_lsp_semantic_tokens "${TIRI_LSP_DIR}/tests/test_semantic_tokens.tiri")
flute_test(tiri_lsp_signature_help "${TIRI_LSP_DIR}/tests/test_signature_help.tiri")
