/*********************************************************************************************************************
** Context menu
**
** Scintilla builds its context menu with ScintillaBase::ContextMenu(), which calls AddToPopUp() for each item and then
** Menu::Show().  The items are collected by the ScintillaKTK object and displayed with gui/menu, which runs in a
** private Tiri script that each editor creates on first use.  The script and the menu surface that it creates are
** owned by the editor.  The editor frees the script first, which frees the menu, so the menu cannot outlive the
** editor.  A menu is rebuilt each time that it opens, and is freed after it closes.
**
** The script reports back to the editor through its own SetKey action, which the editor subscribes to:
**
**   command: The ID of an activated item.  The command is validated again before it is executed.
**   closed:  The menu has been hidden.  The value is 'restore' if the menu still had the focus, i.e. it was closed by
**            activating an item or pressing Escape, in which case the focus is returned to the editor.  It is 'lost'
**            if the user moved the focus elsewhere.
**
** The menu opens at the scene position of the RMB release, which is unaffected by any transforms that apply to the
** editor.  The script converts it to display coordinates through the host surface.
*/

static const char glMenuBridge[] = R"TIRI(
   import 'gui', 'gui/menu'
   module display as mGfx

local script <const> = obj.find('self')
local current_menu

function closeMenu()
   local menu = current_menu
   current_menu = nil
   if menu then menu.surface?.free() end
end

-- Items are newline separated, with each line formatted as 'id|enabled|qualifier|key|label'.

global function showMenu(Host:obj!, X:num!, Y:num!, Items:str!)
   closeMenu()

   local scene = Host.scene
   local err, info = mGfx.GetSurfaceInfo(scene.surface)
   if err != ERR_Okay then raise err, 'Unable to analyse the host surface' end

   local config = array<table>
   for line in values(Items.split('\n')) do
      if line is '' then continue end
      local parts = line.split('|')
      config.push({
         id        = tonumber(parts[0]),
         disabled  = parts[1] is '0',
         qualifier = (parts[2] != '') ? parts[2] : nil,
         key       = (parts[3] != '') ? parts[3] : nil,
         label     = parts[4]
      })
   end

   local menu = gui.menu({
      popup = true, hostScene = scene, items = config, showIcons = false,
      popupX = info.absX + X, popupY = info.absY + Y,
      events = { activate = function(Item) script.setKey('command', tostring(Item.id)) end }
   })
   current_menu = menu

   local surface = menu.surface
   surface.subscribe('hide', function()
      if current_menu?.surface != surface then return end -- A menu that is being replaced or freed is not reported
      script.setKey('closed', (surface.flags has RNF_HAS_FOCUS) ? 'restore' : 'lost')

      -- The menu cannot be freed during its own notification, so it is released on the next cycle.

      processing.delayedCall(function()
         if current_menu is menu then closeMenu() end
      end)
   end)

   menu.show()
end

-- Hide the menu without reporting it, e.g. because the editor has been disabled.

global function hideMenu()
   closeMenu()
end
)TIRI";

//********************************************************************************************************************
// Receives the reports of the context menu's script (see above).

static void notify_menu_key(OBJECTPTR Script, ACTIONID ActionID, ERR Result, struct acSetKey *Args)
{
   auto Self = (extScintilla *)CurrentContext();
   if ((Result != ERR::Okay) or (not Args) or (not Self->API)) return;

   if (Args->Key IS "command") {
      int command = 0;
      std::from_chars(Args->Value.data(), Args->Value.data() + Args->Value.size(), command);
      Self->API->panPopupCommand(command);
   }
   else if (Args->Key IS "closed") {
      Self->MenuOpen = false;
      if ((Args->Value IS "restore") and (Self->Focus) and ((Self->Flags & SCIF::DISABLED) IS SCIF::NIL)) {
         // The menu took the surface focus, which the host surface must regain before the viewport can be focused.

         if ((Self->Focus->Scene) and (Self->Focus->Scene->SurfaceID)) {
            kt::ScopedObjectLock surface(Self->Focus->Scene->SurfaceID);
            if (surface.granted()) acFocus(*surface);
         }
         acFocus(Self->Focus);
      }
   }
}

//********************************************************************************************************************
// Create the script that hosts the context menu, if it does not already exist.

static ERR create_menu_script(extScintilla *Self)
{
   kt::Log log(__FUNCTION__);

   if (Self->MenuScript) return ERR::Okay;

   kt::SwitchContext ctx(Self); // The editor owns the script and receives its SetKey notifications

   objScript *script;
   if (NewObject(CLASSID::TIRI, &script) != ERR::Okay) return log.warning(ERR::NewObject);

   script->setStatement(std::string_view(glMenuBridge, sizeof(glMenuBridge) - 1));

   if (auto error = InitObject(script); error != ERR::Okay) {
      FreeResource(script);
      return log.warning(error);
   }

   if (auto error = acActivate(script); (error != ERR::Okay) or (script->Error != ERR::Okay)) {
      log.warning("Failed to initialise the context menu: %s", script->ErrorMessage.c_str());
      FreeResource(script);
      return ERR::Failed;
   }

   SubscribeAction(script, AC::SetKey, C_FUNCTION(notify_menu_key));
   Self->MenuScript = script;
   return ERR::Okay;
}

//********************************************************************************************************************
// Hide the context menu if it is open.  The focus is not restored.

static void hide_context_menu(extScintilla *Self)
{
   if ((not Self->MenuOpen) or (not Self->MenuScript)) return;
   Self->MenuOpen = false;
   Self->MenuScript->exec("hideMenu", nullptr, 0);
}

//********************************************************************************************************************
// The editor is being freed.  The notifications are removed first, because freeing the script hides the menu.

static void free_context_menu(extScintilla *Self)
{
   if (not Self->MenuScript) return;
   UnsubscribeAction(Self->MenuScript, AC::NIL);
   FreeResource(Self->MenuScript);
   Self->MenuScript = nullptr;
   Self->MenuOpen = false;
}

//********************************************************************************************************************
// Returns true if the clipboard holds text that can be pasted.

static bool clipboard_has_text()
{
   objClipboard::create clipboard = { };
   if (not clipboard.ok()) return false;

   kt::vector<std::string> files;
   return (clipboard->getFiles(CLIPTYPE::TEXT, 0, nullptr, files, nullptr) IS ERR::Okay) and (not files.empty());
}

/*********************************************************************************************************************
** Open the context menu for an RMB release at client position X,Y.  SceneX and SceneY are the position of the release
** in the scene, which determines the placement of the menu.
*/

void ScintillaKTK::panContextMenu(double X, double Y, double SceneX, double SceneY)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("%.0fx%.0f", X, Y);

   if ((scintilla->Flags & SCIF::DISABLED) != SCIF::NIL) return;

   scintilla->MenuX = SceneX;
   scintilla->MenuY = SceneY;
   popup_items.clear();
   ContextMenu(Scintilla::Point(int(std::floor(X)), int(std::floor(Y))));
}

/*********************************************************************************************************************
** Returns true if a context menu command can be executed in the current state of the editor.  This is checked when
** the menu is built and again when an item is activated, because the editor may have changed in the meantime.  Edits
** require SCIF::EDIT, and pasting requires text on the clipboard.
*/

bool ScintillaKTK::PopupCommandAllowed(int Command)
{
   if ((scintilla->Flags & SCIF::DISABLED) != SCIF::NIL) return false;

   const bool writable = ((scintilla->Flags & SCIF::EDIT) != SCIF::NIL) and (not pdoc->IsReadOnly());

   switch (Command) {
      case idcmdUndo:      return writable and pdoc->CanUndo();
      case idcmdRedo:      return writable and pdoc->CanRedo();
      case idcmdCut:       return writable and (not sel.Empty());
      case idcmdCopy:      return not sel.Empty();
      case idcmdPaste:     return writable and CanPaste() and clipboard_has_text();
      case idcmdDelete:    return writable and (not sel.Empty());
      case idcmdSelectAll: return true;
      default:             return false;
   }
}

/*********************************************************************************************************************
** Called by ScintillaBase::ContextMenu() for each item.  An empty label is a separator, which is omitted because
** gui/menu has no plain separator.  The shortcut of each item is shown as a hint, and also activates the item while
** the menu has the focus.
*/

void ScintillaKTK::AddToPopUp(const char *Label, int Command, bool Enabled)
{
   kt::Log(__FUNCTION__).trace("%s", Label);

   if ((not Label) or (not Label[0])) return;

   static const struct { int command; CSTRING qualifier; CSTRING key; } shortcuts[] = {
      { idcmdUndo, "CTRL", "Z" }, { idcmdRedo, "CTRL", "Y" }, { idcmdCut, "CTRL", "X" }, { idcmdCopy, "CTRL", "C" },
      { idcmdPaste, "CTRL", "V" }, { idcmdDelete, "", "DEL" }, { idcmdSelectAll, "CTRL", "A" }
   };

   popup_item item = { Label, Command, (Enabled) and (PopupCommandAllowed(Command)), "", "" };
   for (auto &shortcut : shortcuts) {
      if (shortcut.command IS Command) {
         item.qualifier = shortcut.qualifier;
         item.key = shortcut.key;
         break;
      }
   }

   popup_items.push_back(std::move(item));
}

/*********************************************************************************************************************
** Called by ScintillaBase::ContextMenu() once the items have been added.  The menu replaces any menu that is already
** open.
*/

void ScintillaKTK::panShowPopup()
{
   kt::Log log(__FUNCTION__);

   if ((popup_items.empty()) or (not scintilla->Viewport)) return;
   if (create_menu_script(scintilla) != ERR::Okay) return;

   std::string items;
   for (auto &item : popup_items) {
      items.append(std::format("{}|{}|{}|{}|{}\n", item.command, item.enabled ? 1 : 0, item.qualifier, item.key,
         item.label));
   }

   log.traceBranch("%d items", int(popup_items.size()));

   const ScriptArg args[] = {
      ScriptArg("Host", scintilla->Viewport),
      ScriptArg("X", scintilla->MenuX),
      ScriptArg("Y", scintilla->MenuY),
      ScriptArg("Items", items.c_str())
   };

   auto script = scintilla->MenuScript;
   if ((script->exec("showMenu", args, std::ssize(args)) != ERR::Okay) or (script->Error != ERR::Okay)) {
      log.warning("Failed to show the context menu: %s", script->ErrorMessage.c_str());
      return;
   }

   scintilla->MenuOpen = true;
}

//********************************************************************************************************************
// Execute a context menu command.  A command that is no longer allowed, e.g. because the selection has been cleared,
// is ignored.

void ScintillaKTK::panPopupCommand(int Command)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("Command: %d", Command);

   if (not PopupCommandAllowed(Command)) return;

   clip_error = ERR::Okay;
   this->Command(Command);
}

//********************************************************************************************************************
// The platform Menu class.  The items are held by the ScintillaKTK object, so the Menu has no state of its own.

Scintilla::Menu::Menu() : mid(nullptr) {}

void Scintilla::Menu::CreatePopUp()
{
   Destroy();
}

//********************************************************************************************************************

void Scintilla::Menu::Destroy()
{
   mid = nullptr;
}

//********************************************************************************************************************
// The Window identifies the Scintilla object (see pan_window.cxx).  The client point is ignored in favour of the scene
// position that was recorded by panContextMenu().

void Scintilla::Menu::Show(Scintilla::Point Pt, Window &Window)
{
   if (auto scintilla = get_scintilla(&Window); (scintilla) and (scintilla->API)) {
      scintilla->API->panShowPopup();
   }
}
