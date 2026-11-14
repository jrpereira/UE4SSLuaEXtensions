-- Run from the repository root: lua5.4 extensions-menu/tests/ExtensionsMenuTests.lua
package.path = 'extensions-menu/Scripts/?.lua;extensions-menu/Scripts/vendor/?.lua;' .. package.path
local Menu = require('extensions_menu')
local Contributions = require('menu_contributions')

local function present(...)
    local set = {}
    for _, folder in ipairs({...}) do set['/mods/' .. folder .. '/mod.json'] = true end
    return function(path) return set[path] == true end
end
local EVENTS, FILES = '0_ModCore_UE4SSLuaEventBridge', '0_ModCore_UE4SSLuaFileBridge'
local function ids(found)
    local out = {}
    for n, extension in ipairs(found) do out[n] = extension.id end
    return table.concat(out, ',')
end

-- Detection follows the mod folders, in row order, whatever order they were found.
assert(ids(Menu.installed('/mods', present(EVENTS, FILES))) == 'UE4SSLuaEventBridge,UE4SSLuaFileBridge')
assert(ids(Menu.installed('/mods', present(FILES, EVENTS))) == 'UE4SSLuaEventBridge,UE4SSLuaFileBridge')
assert(ids(Menu.installed('/mods', present(FILES))) == 'UE4SSLuaFileBridge')
assert(ids(Menu.installed('/mods', present())) == '')
local both = Menu.installed('/mods', present(EVENTS, FILES))
assert(Menu.count(both) == 2 and Menu.count(Menu.installed('/mods', present(EVENTS))) == 1)

-- The page shows the count as a read-only row, then a section per installed extension holding
-- the slot its settings fill; it stores nothing.
local page = Menu.pageManifest(both)
assert(page:find('Label=Installed extensions\n', 1, true) and page:find('PresetLabels=2|2', 1, true)
    and page:find('mcReadOnly=1', 1, true) and not page:find('ConfigFile', 1, true))
assert(Menu.pageManifest({}):find('PresetLabels=0|0', 1, true) and not Menu.pageManifest({}):find('mcSlot', 1, true))
for _, extension in ipairs(both) do
    assert(page:find('[Category.' .. extension.slot .. ']\nmcLabel=' .. extension.label .. '\n', 1, true), 'section heading')
    assert(page:find('Group=' .. extension.slot .. '\nType=picker\nDefault=0\nPresetValues=0|1\n'
        .. 'PresetLabels=Unavailable|Unavailable\nmcReadOnly=1\nmcSlot=' .. extension.slot .. '\n', 1, true), 'slot row')
end
assert(page:find('LEX_Installed', 1, true) < page:find('Category.Events', 1, true)
    and page:find('Category.Events', 1, true) < page:find('Category.Files', 1, true), 'count, then sections in order')
assert(Menu.slotAddress(Menu.extensions[1]) == 'UE4SSLuaEXtensions:Events')
assert(not Menu.pageManifest({Menu.extensions[2]}):find('Category.Events', 1, true), 'only installed extensions')
local events, files = Menu.extensions[1], Menu.extensions[2]
assert(events.label == 'Events Extension' and files.label == 'Files Extension')

-- The source page is the extension's own mod_settings.ini under the contributor's Mod Id.
local ini = '[Mod]\r\nId=UE4SSLuaEventBridge\r\nName=UE4SS Lua Event Bridge\r\n\r\n'
    .. '[Setting.A]\r\nId=LoadTimeVerifications\r\nType=toggle\r\nConfigFile=settings.ini\r\n\r\n'
    .. '[Setting.B]\r\nId=FunctionHooks\r\nType=toggle\r\nVisibleWhen=LoadTimeVerifications\r\n'
    .. 'VisibleValues=1\r\n'
local source = Menu.sourceManifest(events, ini)
assert(source:find('[Mod]\nId=UE4SSLuaEXtensions.Events\nName=UE4SS Lua Event Bridge\n', 1, true), 'Mod Id replaced')
assert(not source:find('Id=UE4SSLuaEventBridge', 1, true) and not source:find('\r', 1, true))
-- Ids are unique on the shared page: each takes the extension's prefix, rules naming them too.
assert(table.concat(Menu.settingIds(source), ',') == 'Events_LoadTimeVerifications,Events_FunctionHooks', 'every setting, in order')
assert(source:find('[Setting.Events_A]\nId=Events_LoadTimeVerifications\nType=toggle\nConfigFile=settings.ini\n', 1, true))
assert(source:find('VisibleWhen=Events_LoadTimeVerifications\n', 1, true), 'rules follow the ids')
-- Without that file, the verification switch alone, with the extension's storage.
for _, extension in ipairs(Menu.extensions) do
    local fallback = Menu.sourceManifest(extension)
    assert(fallback:find('Id=' .. extension.contributor .. '\n', 1, true))
    assert(fallback:find('ConfigFile=settings.ini\nConfigSection=Verifications\nConfigKey=LoadTime', 1, true), 'storage')
    assert(fallback:find('Type=toggle', 1, true) and fallback:find('Default=0', 1, true), 'off by default')
    assert(table.concat(Menu.settingIds(fallback), ',') == extension.slot .. '_LoadTimeVerifications')
end

-- Publishing: this folder's page comes first and is listed as its mod; each extension's hidden
-- page fills its section, so its own entry is hidden.
local files_read = {['/mods/' .. EVENTS .. '/mod_settings.ini'] = ini}
local function read(path) return files_read[path] end
local list = Menu.contributions(both, '/mods/0_ModCore_UE4SSLuaEXtensions', '/mods', '1.1.0', read)
assert(#list == 3 and list[1].id == 'UE4SSLuaEXtensions' and list[2].id == 'UE4SSLuaEXtensions.Events'
    and list[3].id == 'UE4SSLuaEXtensions.Files')
local countPage = list[1].contribution.pages[1]
assert(countPage.visible == nil and countPage.attach == '0_ModCore_UE4SSLuaEXtensions'
    and countPage.version == '1.1.0' and countPage.name == 'ModCore Lua Extensions',
    "the folder's entry is this page")
assert(list[1].contribution.rows == nil, 'the host page fills no slot')
for n, extension in ipairs(both) do
    local contribution = list[n + 1].contribution
    local page = contribution.pages[1]
    -- The extension's own mod_settings.ini already puts its id in the menu: the hidden page must not reuse it.
    assert(page.visible == false and page.attach == extension.id and page.id == extension.contributor
        and page.id ~= extension.id)
    assert(page.configDirectory == '/mods/' .. extension.folder, "settings are stored in the extension's folder")
    local row = contribution.rows[1]
    assert(row.page == extension.contributor and row.slot == Menu.slotAddress(extension))
end
assert(table.concat(list[2].contribution.rows[1].settings, ',') == 'Events_LoadTimeVerifications,Events_FunctionHooks', 'read from the file')
assert(table.concat(list[3].contribution.rows[1].settings, ',') == 'Files_LoadTimeVerifications', 'fallback without the file')
for _, entry in ipairs(list) do
    assert(Contributions.validate(entry.id, entry.contribution), 'valid for ' .. entry.id)
end

-- With nothing installed only the count is published; with one, only its page.
assert(#Menu.contributions({}, '/mods/x', '/mods', '1.1.0') == 1)
local single = Menu.contributions(Menu.installed('/mods', present(EVENTS)), '/mods/x', '/mods', nil)
assert(#single == 2 and single[2].id == 'UE4SSLuaEXtensions.Events' and single[1].contribution.pages[1].version == nil)

-- Each contributor publishes into a directory of its own: generation files carry no contributor name.
local seenDirectory = {}
for _, entry in ipairs(list) do
    local directory = Menu.directory('/mods/x/cache', entry.id)
    assert(not seenDirectory[directory]); seenDirectory[directory] = true
end
print('PASS extensions menu: detection, count, sections, settings files, storage and publishing as a ModCore Settings module')
