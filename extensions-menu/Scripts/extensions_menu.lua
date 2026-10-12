-- Puts the UE4SS Lua extensions installed beside this folder on ModCore Settings' own page.
--
-- ModCore Settings' Modules tab reserves two slots after its Listing settings:
-- settings:extensions (how many extensions are installed) and settings:verifications (each
-- extension's startup verification switch). This folder publishes hidden pages whose rows fill
-- them: one for the count, attached to this folder so it has no entry of its own, and one per
-- installed extension, attached to the extension so its own entry is hidden too. Each switch
-- keeps the extension's own storage. A ModCore Settings without those slots shows the hidden
-- pages as ordinary pages instead, so nothing becomes unreachable.
--
-- Everything here is pure: the caller supplies the mods directory and a file test.

local M = {}

M.id = 'UE4SSLuaEXtensions'
M.name = 'ModCore Lua Extensions'
M.countSlot = 'settings:extensions'
M.verificationSlot = 'settings:verifications'

-- Order here is the order of the rows. contributor publishes the hidden page that holds the
-- extension's switch; it must differ from the extension's own id, which its mod_settings.ini
-- already puts in the menu.
M.extensions = {
    {id = 'UE4SSLuaEventBridge', folder = '0_ModCore_UE4SSLuaEventBridge', label = 'Events Extension',
        title = 'UE4SS Lua Event Bridge', setting = 'EventsLoadTimeVerifications',
        contributor = 'UE4SSLuaEXtensions.Events'},
    {id = 'UE4SSLuaFileBridge', folder = '0_ModCore_UE4SSLuaFileBridge', label = 'Files Extension',
        title = 'UE4SS Lua File Bridge', setting = 'FilesLoadTimeVerifications',
        contributor = 'UE4SSLuaEXtensions.Files'},
}

local DESCRIPTION = "Runs the extension's in-game checks when the game loads and writes PASS and FAIL "
    .. 'lines to UE4SS.log. Takes effect on the next launch.'

-- The extensions whose mod folder sits in modsDirectory, in row order.
-- exists(path) is true for a readable file.
function M.installed(modsDirectory, exists)
    local found = {}
    for _, extension in ipairs(M.extensions) do
        if exists(modsDirectory .. '/' .. extension.folder .. '/mod.json') then found[#found + 1] = extension end
    end
    return found
end

-- The submodule counter.
function M.count(found) return #found end

local function block(lines, name, fields)
    lines[#lines + 1] = '[' .. name .. ']'
    for _, pair in ipairs(fields) do lines[#lines + 1] = pair[1] .. '=' .. pair[2] end
    lines[#lines + 1] = ''
end

-- The count as a read-only row: DMM pickers need two choices, so the value is given twice.
function M.countManifest(found)
    local lines = {}
    local value = tostring(M.count(found))
    block(lines, 'Mod', {{'Id', M.id}, {'Name', M.name}})
    block(lines, 'Setting.LEX_Installed', {{'Id', 'LEX_Installed'}, {'Label', 'Installed extensions'},
        {'Group', 'Extensions'}, {'Type', 'picker'}, {'Default', '0'}, {'PresetValues', '0|1'},
        {'PresetLabels', value .. '|' .. value}, {'mcReadOnly', '1'}})
    return table.concat(lines, '\n')
end

-- An extension's own verification switch. Its storage is the extension's settings.ini, which
-- its Scripts read at startup. The setting id differs per extension: both land on one page.
function M.sourceManifest(extension)
    local lines = {}
    block(lines, 'Mod', {{'Id', extension.contributor}, {'Name', extension.title}})
    block(lines, 'Setting.' .. extension.setting, {{'Id', extension.setting}, {'Type', 'toggle'},
        {'Label', extension.label}, {'Description', DESCRIPTION}, {'Group', 'Diagnostics'},
        {'PresetValues', '0|1'}, {'PresetLabels', 'Off|On'}, {'Default', '0'}, {'ConfigFile', 'settings.ini'},
        {'ConfigSection', 'Verifications'}, {'ConfigKey', 'LoadTime'}})
    return table.concat(lines, '\n')
end

-- What to publish: a list of {id=, contribution=} in publishing order, which is also the order of
-- the rows in each slot. Each contributor needs its own directory: publishers name their files
-- by generation only. root: this mod's folder. modsDirectory: the folder holding all mods.
function M.contributions(found, root, modsDirectory, version)
    local out = {{id = M.id, contribution = {
        pages = {{id = M.id, name = M.name, version = version, manifest = M.countManifest(found),
            configDirectory = root, attach = root:match('([^/\\]+)$'), visible = false}},
        rows = {{page = M.id, slot = M.countSlot, settings = {'LEX_Installed'}}},
    }}}
    for _, extension in ipairs(found) do
        out[#out + 1] = {id = extension.contributor, contribution = {
            -- An extension with a mod_settings.ini is listed under its Mod Id, not its folder.
            pages = {{id = extension.contributor, name = extension.title, manifest = M.sourceManifest(extension),
                configDirectory = modsDirectory .. '/' .. extension.folder, attach = extension.id,
                visible = false}},
            rows = {{page = extension.contributor, slot = M.verificationSlot, settings = {extension.setting}}},
        }}
    end
    return out
end

-- The publishing directory of one contributor, inside this folder's cache.
function M.directory(cache, id) return cache .. '/' .. id end

return M
