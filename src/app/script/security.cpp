// Aseprite
// Copyright (C) 2019-2025  Igara Studio S.A.
// Copyright (C) 2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/script/security.h"

#include "app/app.h"
#include "app/context.h"
#include "app/i18n/strings.h"
#include "app/ini_file.h"
#include "app/launcher.h"
#include "app/script/engine.h"
#include "app/script/luacpp.h"
#include "base/convert_to.h"
#include "base/fs.h"
#include "base/sha1.h"
#include "fmt/format.h"
#include "ui/widget_type.h"

#include "script_access.xml.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace app { namespace script {

namespace {

int secure_io_open(lua_State* L);
int secure_io_popen(lua_State* L);
int secure_io_lines(lua_State* L);
int secure_io_input(lua_State* L);
int secure_io_output(lua_State* L);
int secure_os_execute(lua_State* L);
int secure_os_remove(lua_State* L);
int secure_os_rename(lua_State* L);
int secure_package_loadlib(lua_State* L);

enum {
  io_open,
  io_popen,
  io_lines,
  io_input,
  io_output,
  os_execute,
  os_remove,
  os_rename,
  package_loadlib,
};

static struct {
  const char* package;
  const char* funcname;
  lua_CFunction newfunc;
  lua_CFunction origfunc = nullptr;
} replaced_functions[] = {
  { "io",      "open",    secure_io_open         },
  { "io",      "popen",   secure_io_popen        },
  { "io",      "lines",   secure_io_lines        },
  { "io",      "input",   secure_io_input        },
  { "io",      "output",  secure_io_output       },
  { "os",      "execute", secure_os_execute      },
  { "os",      "remove",  secure_os_remove       },
  { "os",      "rename",  secure_os_rename       },
  { "package", "loadlib", secure_package_loadlib },
};

// Map from .lua file name -> sha1
std::unordered_map<std::string, std::string> g_keys;

std::string get_key(const std::string& source)
{
  auto it = g_keys.find(source);
  if (it != g_keys.end())
    return it->second;
  else
    return g_keys[source] = base::convert_to<std::string>(base::Sha1::calculateFromString(source));
}

std::string get_script_filename(lua_State* L, int stackLevel)
{
  // Get script name
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "getinfo");
  lua_remove(L, -2);
  lua_pushinteger(L, stackLevel);
  lua_pushstring(L, "S");
  lua_call(L, 2, 1);
  lua_getfield(L, -1, "source");
  const char* source = lua_tostring(L, -1);
  std::string script;
  if (source && *source) {
    script = source;

    if (!script.empty() && script[0] == '@')
      script = source + 1;
  }
  lua_pop(L, 2);
  return script;
}

int unsupported(lua_State* L)
{
  // debug.getinfo(1, "n").name
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "getinfo");
  lua_remove(L, -2);
  lua_pushinteger(L, 1);
  lua_pushstring(L, "n");
  lua_call(L, 2, 1);
  lua_getfield(L, -1, "name");
  return luaL_error(L, "unsupported function '%s'", lua_tostring(L, -1));
}

int secure_io_open(lua_State* L)
{
  std::string absFilename = base::get_absolute_path(luaL_checkstring(L, 1));

  FileAccessMode mode = FileAccessMode::Read; // Read is the default access
  if (lua_tostring(L, 2) && std::strchr(lua_tostring(L, 2), 'w') != nullptr) {
    mode = FileAccessMode::Write;
  }

  if (!ask_access(L, absFilename.c_str(), mode, ResourceType::File)) {
    return luaL_error(L, "the script doesn't have access to file '%s'", absFilename.c_str());
  }
  return replaced_functions[io_open].origfunc(L);
}

int secure_io_popen(lua_State* L)
{
  const char* cmd = luaL_checkstring(L, 1);
  if (!ask_access(L, cmd, FileAccessMode::Execute, ResourceType::Command)) {
    // Stop script
    return luaL_error(L, "the script doesn't have access to execute the command: '%s'", cmd);
  }
  return replaced_functions[io_popen].origfunc(L);
}

int secure_io_lines(lua_State* L)
{
  if (auto fn = lua_tostring(L, 1)) {
    std::string absFilename = base::get_absolute_path(fn);

    if (!ask_access(L, absFilename.c_str(), FileAccessMode::Read, ResourceType::File)) {
      return luaL_error(L, "the script doesn't have access to file '%s'", absFilename.c_str());
    }
  }
  return replaced_functions[io_lines].origfunc(L);
}

int secure_io_input(lua_State* L)
{
  if (auto fn = lua_tostring(L, 1)) {
    std::string absFilename = base::get_absolute_path(fn);

    if (!ask_access(L, absFilename.c_str(), FileAccessMode::Read, ResourceType::File)) {
      return luaL_error(L, "the script doesn't have access to file '%s'", absFilename.c_str());
    }
  }
  return replaced_functions[io_input].origfunc(L);
}

int secure_io_output(lua_State* L)
{
  if (auto fn = lua_tostring(L, 1)) {
    std::string absFilename = base::get_absolute_path(fn);

    if (!ask_access(L, absFilename.c_str(), FileAccessMode::Write, ResourceType::File)) {
      return luaL_error(L, "the script doesn't have access to file '%s'", absFilename.c_str());
    }
  }
  return replaced_functions[io_output].origfunc(L);
}

int secure_os_execute(lua_State* L)
{
  const char* cmd = luaL_checkstring(L, 1);
  if (!ask_access(L, cmd, FileAccessMode::Execute, ResourceType::Command)) {
    // Stop script
    return luaL_error(L, "the script doesn't have access to execute the command: '%s'", cmd);
  }
  return replaced_functions[os_execute].origfunc(L);
}

int file_result(lua_State* L, bool result, int errorNo = 0, const std::string& fileName = "")
{
  if (result) {
    lua_pushboolean(L, 1);
    return 1;
  }

  luaL_pushfail(L);
  if (fileName.empty())
    lua_pushstring(L, strerror(errorNo));
  else
    lua_pushfstring(L, "%s: %s", fileName.c_str(), strerror(errorNo));
  lua_pushinteger(L, errorNo);
  return 3;
}

int secure_os_remove(lua_State* L)
{
  const std::string absFilename = base::get_canonical_path(luaL_checkstring(L, 1));
  if (absFilename.empty())
    return file_result(L, false, ENOENT, absFilename);

  if (!ask_access(L, absFilename.data(), FileAccessMode::Write, ResourceType::File))
    return file_result(L, false, EACCES, absFilename);

  if (base::is_directory(absFilename)) {
    try {
      base::remove_directory(absFilename);
      return file_result(L, true);
    }
    catch (const std::exception&) {
      return file_result(L, false, EIO, absFilename);
    }
  }

  try {
    base::delete_file(absFilename);
  }
  catch (const std::exception&) {
    return file_result(L, false, EIO, absFilename);
  }

  return file_result(L, true);
}

int secure_os_rename(lua_State* L)
{
  const std::string absSourceFilename = base::get_canonical_path(luaL_checkstring(L, 1));
  const std::string absDestFilename = base::get_absolute_path(luaL_checkstring(L, 2));
  lua_pop(L, 2);

  if (absSourceFilename.empty())
    return file_result(L, false, ENOENT, absSourceFilename);

  if (absDestFilename.empty())
    return file_result(L, false, EINVAL, absDestFilename);

  if (!ask_access(L, absSourceFilename.data(), FileAccessMode::Write, ResourceType::File))
    return file_result(L, false, EACCES, absSourceFilename);

  try {
    // If the destination file already exists, we should ask for permission to overwrite it.
    if (!base::get_canonical_path(absDestFilename).empty() &&
        !ask_access(L, absDestFilename.data(), FileAccessMode::Write, ResourceType::File)) {
      return file_result(L, false, EACCES, absDestFilename);
    }

    base::move_file(absSourceFilename, absDestFilename);
    return file_result(L, true);
  }
  catch (const std::exception&) {
    return file_result(L, false, EIO, absSourceFilename);
  }
}

int secure_package_loadlib(lua_State* L)
{
  const char* cmd = luaL_checkstring(L, 1);
  if (!ask_access(L, cmd, FileAccessMode::LoadLib, ResourceType::File)) {
    // Stop script
    return luaL_error(L, "the script doesn't have access to execute the command: '%s'", cmd);
  }
  return replaced_functions[package_loadlib].origfunc(L);
}

} // anonymous namespace

void overwrite_unsecure_functions(lua_State* L)
{
  // Remove unsupported functions
  lua_getglobal(L, "os");
  for (const char* name : { "exit", "tmpname" }) {
    lua_pushcfunction(L, unsupported);
    lua_setfield(L, -2, name);
  }
  lua_pop(L, 1);

  // Replace functions with our own implementations (that ask for
  // permissions first).
  for (auto& item : replaced_functions) {
    lua_getglobal(L, item.package);

    // Get old function
    if (!item.origfunc) {
      lua_getfield(L, -1, item.funcname);
      item.origfunc = lua_tocfunction(L, -1);
      lua_pop(L, 1);
    }

    // Push and set the new function
    lua_pushcfunction(L, item.newfunc);
    lua_setfield(L, -2, item.funcname);

    lua_pop(L, 1);
  }
}

bool ask_access(lua_State* L,
                const char* filename,
                const FileAccessMode mode,
                const ResourceType resourceType,
                const int stackLevel)
{
  // Ask for permission to open the file
  if (App::instance()->context()->isUIAvailable()) {
    const std::string script = get_script_filename(L, stackLevel);
    if (script.empty()) {
      // No script
      luaL_error(L, "no debug information (script filename) to secure io.open() call");
      return false;
    }

    const char* section = "script_access";
    std::string key = get_key(script);

    int access = get_config_int(section, key.c_str(), 0);

    // Has the correct access
    if ((access & int(mode)) == int(mode))
      return true;

    std::string allowButtonText;
    switch (mode) {
      case FileAccessMode::LoadLib:
        allowButtonText = Strings::script_access_allow_load_lib_access();
        break;
      case FileAccessMode::OpenSocket:
        allowButtonText = Strings::script_access_allow_open_conn_access();
        break;
      case FileAccessMode::Execute:
        allowButtonText = Strings::script_access_allow_execute_access();
        break;
      case FileAccessMode::Write:
        allowButtonText = Strings::script_access_allow_write_access();
        break;
      case FileAccessMode::Read:
        allowButtonText = Strings::script_access_allow_read_access();
        break;
      default: {
        luaL_error(L, "invalid access request");
        return false;
      }
    }

    app::gen::ScriptAccess dlg;
    dlg.script()->setText(script);

    {
      std::string label;
      switch (resourceType) {
        case ResourceType::File: {
          if (mode == FileAccessMode::Write) {
            label = Strings::script_access_file_write_label();
          }
          else {
            label = Strings::script_access_file_label();
          }
          break;
        }
        case ResourceType::Command:   label = Strings::script_access_command_label(); break;
        case ResourceType::WebSocket: label = Strings::script_access_websocket_label(); break;
        case ResourceType::Clipboard: label = Strings::script_access_clipboard_label(); break;
      }
      dlg.fileLabel()->setText(label);
    }

    if (filename && strlen(filename) > 0)
      dlg.file()->setText(filename);
    else
      dlg.fileContainer()->setVisible(false);

    dlg.allow()->setText(allowButtonText);
    dlg.allow()->processMnemonicFromText();

    if (script == "internal") {
      // This should not happen, we should have a proper script
      // filename here, probably the given "stackLevel" is wrong.
      ASSERT(false);

      // Make it look like a normal label
      dlg.script()->setType(ui::WidgetType::kLabelWidget);
      dlg.script()->initTheme();
    }
    else
      dlg.script()->Click.connect([&dlg] { app::launcher::open_folder(dlg.script()->text()); });

    dlg.full()->Click.connect([&dlg, &allowButtonText]() {
      if (dlg.full()->isSelected()) {
        dlg.dontShow()->setSelected(true);
        dlg.dontShow()->setEnabled(false);
        dlg.allow()->setText(Strings::script_access_give_full_access());
        dlg.allow()->processMnemonicFromText();
        dlg.layout();
      }
      else {
        dlg.dontShow()->setEnabled(true);
        dlg.allow()->setText(allowButtonText);
        dlg.allow()->processMnemonicFromText();
        dlg.layout();
      }
    });

    if (resourceType == ResourceType::File) {
      dlg.file()->Click.connect([&dlg] {
        std::string fn = dlg.file()->text();
        if (base::is_file(fn))
          app::launcher::open_folder(fn);
        else
          app::launcher::open_folder(base::get_file_path(fn));
      });
    }

    dlg.openWindowInForeground();
    const bool allow = (dlg.closer() == dlg.allow());

    // Save selected option
    if (allow && dlg.dontShow()->isSelected()) {
      if (dlg.full()->isSelected())
        set_config_int(section, key.c_str(), access | int(FileAccessMode::Full));
      else
        set_config_int(section, key.c_str(), access | int(mode));
      flush_config_file();
    }

    if (!allow)
      return false;
  }
  return true;
}

}} // namespace app::script
