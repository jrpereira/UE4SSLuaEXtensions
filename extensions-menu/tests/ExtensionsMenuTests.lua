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

-- The count is a read-only row; each switch keeps the extension's own storage and label.
local count = Menu.countManifest(both)
assert(count:find('Label=Installed extensions\n', 1, true) and count:find('PresetLabels=2|2', 1, true)
    and count:find('mcReadOnly=1', 1, true) and not count:find('ConfigFile', 1, true))
assert(Menu.countManifest({}):find('PresetLabels=0|0', 1, true))
local events, files = Menu.extensions[1], Menu.extensions[2]
assert(events.setting ~= files.setting)
for _, extension in ipairs(Menu.extensions) do
    local source = Menu.sourceManifest(extension)
    assert(source:find('Label=' .. extension.label .. '\n', 1, true), 'the switch is named for its extension')
    assert(source:find('ConfigFile=settings.ini\nConfigSection=Verifications\nConfigKey=LoadTime', 1, true), 'storage')
    assert(source:find('Type=toggle', 1, true) and source:find('Default=0', 1, true), 'off by default')
end
assert(events.label == 'Events Extension' and files.label == 'Files Extension')

-- Publishing: every page is hidden; rows fill ModCore Settings' Modules slots, the count first.
local list = Menu.contributions(both, '/mods/0_ModCore_UE4SSLuaEXtensions', '/mods', '1.1.0')
assert(#list == 3 and list[1].id == 'UE4SSLuaEXtensions' and list[2].id == 'UE4SSLuaEXtensions.Events'
    and list[3].id == 'UE4SSLuaEXtensions.Files')
local countPage = list[1].contribution.pages[1]
assert(countPage.visible == false and countPage.attach == '0_ModCore_UE4SSLuaEXtensions'
    and countPage.version == '1.1.0' and countPage.name == 'ModCore Lua Extensions',
    'the folder has no entry of its own while the slot shows its count')
assert(list[1].contribution.rows[1].slot == 'settings:extensions' and list[1].contribution.rows[1].settings[1] == 'LEX_Installed')
for n, extension in ipairs(both) do
    local contribution = list[n + 1].contribution
    local page = contribution.pages[1]
    -- The extension's own mod_settings.ini already puts its id in the menu: the hidden page must not reuse it.
    assert(page.visible == false and page.attach == extension.id and page.id == extension.contributor
        and page.id ~= extension.id)
    assert(page.configDirectory == '/mods/' .. extension.folder, "the switch is stored in the extension's folder")
    local row = contribution.rows[1]
    assert(row.page == extension.contributor and row.slot == 'settings:verifications' and row.settings[1] == extension.setting)
end
for _, entry in ipairs(list) do
    assert(Contributions.validate(entry.id, entry.contribution), 'valid for ' .. entry.id)
end

-- With nothing installed only the count is published; with one, only its switch.
assert(#Menu.contributions({}, '/mods/x', '/mods', '1.1.0') == 1)
local single = Menu.contributions(Menu.installed('/mods', present(EVENTS)), '/mods/x', '/mods', nil)
assert(#single == 2 and single[2].id == 'UE4SSLuaEXtensions.Events' and single[1].contribution.pages[1].version == nil)

-- Each contributor publishes into a directory of its own: generation files carry no contributor name.
local seenDirectory = {}
for _, entry in ipairs(list) do
    local directory = Menu.directory('/mods/x/cache', entry.id)
    assert(not seenDirectory[directory]); seenDirectory[directory] = true
end
print('PASS extensions menu: detection, count, switches, storage and publishing to ModCore Settings')
