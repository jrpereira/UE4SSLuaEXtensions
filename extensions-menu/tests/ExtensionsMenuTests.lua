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

-- Detection follows the mod folders, in page order, whatever order they were found.
assert(ids(Menu.installed('/mods', present(EVENTS, FILES))) == 'UE4SSLuaEventBridge,UE4SSLuaFileBridge')
assert(ids(Menu.installed('/mods', present(FILES, EVENTS))) == 'UE4SSLuaEventBridge,UE4SSLuaFileBridge')
assert(ids(Menu.installed('/mods', present(FILES))) == 'UE4SSLuaFileBridge')
assert(ids(Menu.installed('/mods', present())) == '')
assert(Menu.count(Menu.installed('/mods', present(EVENTS, FILES))) == 2)
assert(Menu.count(Menu.installed('/mods', present(EVENTS))) == 1)

-- The page shows the counter, one heading, and a labelled slot per installed extension.
local both = Menu.installed('/mods', present(EVENTS, FILES))
local host = Menu.hostManifest(both)
assert(host:find('Label=Installed extensions\n', 1, true) and host:find('PresetLabels=2|2', 1, true), 'counter row')
assert(host:find('[Category.Verifications]\nmcLabel=Enable startup library verifications', 1, true), 'heading')
assert(host:find('Label=Events Extension', 1, true) and host:find('mcSlot=events', 1, true))
assert(host:find('Label=Files Extension', 1, true) and host:find('mcSlot=files', 1, true))
assert(select(2, host:gsub('mcSlotLabel=1', '')) == 2, 'each slot is named by the host')
assert(host:find('Events Extension', 1, true) < host:find('Files Extension', 1, true), 'row order')

local one = Menu.hostManifest(Menu.installed('/mods', present(FILES)))
assert(one:find('PresetLabels=1|1', 1, true) and not one:find('mcSlot=events', 1, true) and one:find('mcSlot=files', 1, true))
local none = Menu.hostManifest({})
assert(none:find('PresetLabels=0|0', 1, true) and none:find('None installed', 1, true) and not none:find('mcSlot=', 1, true))

-- Each extension's toggle keeps the extension's own storage and has a distinct setting id.
local events, files = Menu.extensions[1], Menu.extensions[2]
assert(events.setting ~= files.setting)
for _, extension in ipairs(Menu.extensions) do
    local source = Menu.sourceManifest(extension)
    assert(source:find('[Setting.' .. extension.setting .. ']', 1, true) and source:find('Id=' .. extension.setting .. '\n', 1, true))
    assert(source:find('ConfigFile=settings.ini\nConfigSection=Verifications\nConfigKey=LoadTime', 1, true), 'storage')
    assert(source:find('Type=toggle', 1, true) and source:find('Default=0', 1, true), 'off by default')
end

-- Publishing: a host contributor, then one hidden, attached, row-publishing contributor each.
local list = Menu.contributions(both, '/mods/0_ModCore_UE4SSLuaEXtensions', '/mods', '1.1.0')
assert(#list == 3 and list[1].id == 'UE4SSLuaEXtensions' and list[2].id == 'UE4SSLuaEXtensions.Events'
    and list[3].id == 'UE4SSLuaEXtensions.Files')
local hostPage = list[1].contribution.pages[1]
assert(hostPage.attach == '0_ModCore_UE4SSLuaEXtensions' and hostPage.version == '1.1.0' and hostPage.visible == nil)
assert(hostPage.configDirectory == '/mods/0_ModCore_UE4SSLuaEXtensions')
for n, extension in ipairs(both) do
    local contribution = list[n + 1].contribution
    local page = contribution.pages[1]
    -- The extension's own mod_settings.ini already puts its id in the menu: the hidden page must not reuse it.
    assert(page.visible == false and page.attach == extension.id and page.id == extension.contributor
        and page.id ~= extension.id)
    assert(page.configDirectory == '/mods/' .. extension.folder, "the toggle is stored in the extension's folder")
    local row = contribution.rows[1]
    assert(row.page == extension.contributor and row.slot == 'UE4SSLuaEXtensions:' .. extension.slot and row.settings[1] == extension.setting)
end
for _, entry in ipairs(list) do
    assert(Contributions.validate(entry.id, entry.contribution), 'valid for ' .. entry.id)
end

-- With nothing installed only the page is published; with one, only its slot.
assert(#Menu.contributions({}, '/mods/x', '/mods', '1.1.0') == 1)
local single = Menu.contributions(Menu.installed('/mods', present(EVENTS)), '/mods/x', '/mods', nil)
assert(#single == 2 and single[2].id == 'UE4SSLuaEXtensions.Events' and single[1].contribution.pages[1].version == nil)

-- The absorb option comes first on the page, is on by default, and lives in this folder's settings.ini.
assert(host:find('[Setting.LEX_Absorb]', 1, true) < host:find('[Setting.LEX_Installed]', 1, true), 'absorb is first')
assert(host:find('Label=Absorb ModCore Native Extensions into this', 1, true))
assert(host:find('Default=1\nConfigFile=settings.ini\nConfigSection=Extensions\nConfigKey=Absorb', 1, true))
assert(Menu.absorbing(nil) == true and Menu.absorbing('') == true and Menu.absorbing('[Other]\nAbsorb=0') == true)
assert(Menu.absorbing('[Extensions]\nAbsorb=0\n') == false and Menu.absorbing('[extensions]\r\n absorb = 1 \r\n') == true)
assert(Menu.absorbing('[Extensions]\nAbsorb=maybe') == true, 'an unreadable value keeps the default')

-- Not absorbing: the page keeps its option and counter, no slots, and nothing hides the extensions.
local separate = Menu.contributions(both, '/mods/x', '/mods', '1.1.0', false)
local plain = separate[1].contribution.pages[1].manifest
assert(#separate == 1 and plain:find('LEX_Absorb', 1, true) and plain:find('PresetLabels=2|2', 1, true)
    and not plain:find('mcSlot=', 1, true) and not plain:find('Verifications', 1, true))

-- Each contributor publishes into a directory of its own: generation files carry no contributor name.
local seenDirectory = {}
for _, entry in ipairs(list) do
    local directory = Menu.directory('/mods/x/cache', entry.id)
    assert(not seenDirectory[directory]); seenDirectory[directory] = true
end
print('PASS extensions menu: detection, counter, absorb option, slots, storage and publishing')
