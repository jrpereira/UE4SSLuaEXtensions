-- Lists the UE4SS Lua extensions installed beside this folder as one ModCore Settings module.
--
-- This folder publishes a visible page attached to itself, so it becomes the "ModCore Lua
-- Extensions" entry in the mod list. The page shows how many extensions are installed, then a
-- section per installed extension, each holding one slot. Each extension publishes a hidden page
-- built from its own mod_settings.ini whose rows fill its slot with all of its settings; the page
-- is attached to the extension, so the extension's own entry is hidden and its settings show
-- here instead, each with the extension's own storage. Without the slots (a ModCore Settings that
-- cannot host rows) the hidden pages show as ordinary pages, so nothing becomes unreachable.
--
-- Everything here is pure: the caller supplies the mods directory, a file test and a reader.

local M = {}

M.id = 'UE4SSLuaEXtensions'
M.name = 'ModCore Lua Extensions'

-- Order here is the order of the sections. contributor publishes the hidden page that holds the
-- extension's settings; it must differ from the extension's own id, which its mod_settings.ini
-- already puts in the menu. slot names the extension's section and slot on this folder's page.
M.extensions = {
    {id = 'UE4SSLuaEventBridge', folder = '0_ModCore_UE4SSLuaEventBridge', label = 'Events Extension',
        title = 'UE4SS Lua Event Bridge', slot = 'Events', contributor = 'UE4SSLuaEXtensions.Events'},
    {id = 'UE4SSLuaFileBridge', folder = '0_ModCore_UE4SSLuaFileBridge', label = 'Files Extension',
        title = 'UE4SS Lua File Bridge', slot = 'Files', contributor = 'UE4SSLuaEXtensions.Files'},
}

-- The address of an extension's slot on this folder's page.
function M.slotAddress(extension) return M.id .. ':' .. extension.slot end

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

-- This folder's page: the count as a read-only row (DMM pickers need two choices, so the value
-- is given twice), then one section per installed extension, headed by its label and holding
-- the slot its settings fill.
function M.pageManifest(found)
    local lines = {}
    local value = tostring(M.count(found))
    block(lines, 'Mod', {{'Id', M.id}, {'Name', M.name}})
    block(lines, 'Setting.LEX_Installed', {{'Id', 'LEX_Installed'}, {'Label', 'Installed extensions'},
        {'Group', 'Extensions'}, {'Type', 'picker'}, {'Default', '0'}, {'PresetValues', '0|1'},
        {'PresetLabels', value .. '|' .. value}, {'mcReadOnly', '1'}})
    for _, extension in ipairs(found) do
        block(lines, 'Category.' .. extension.slot, {{'mcLabel', extension.label}})
        block(lines, 'Setting.LEX_' .. extension.slot, {{'Id', 'LEX_' .. extension.slot}, {'Label', 'Settings'},
            {'Group', extension.slot}, {'Type', 'picker'}, {'Default', '0'}, {'PresetValues', '0|1'},
            {'PresetLabels', 'Unavailable|Unavailable'}, {'mcReadOnly', '1'}, {'mcSlot', extension.slot}})
    end
    return table.concat(lines, '\n')
end

-- The hidden page holding an extension's settings: its own mod_settings.ini (text) under the
-- contributor's Mod Id, so storage stays the extension's. Every extension's rows land on one
-- page, where setting ids must be unique, so each id (and any rule naming it) takes the
-- extension's slot as a prefix; storage is keyed by ConfigSection and ConfigKey, not by id.
-- Without that file, the startup verification switch its Scripts read from settings.ini.
local RULES = {VisibleWhen = true, mcLabelWhen = true}
function M.sourceManifest(extension, text)
    local prefix = extension.slot .. '_'
    if text then
        text = text:gsub('\r\n', '\n')
        local ids, section = {}, nil
        for line in (text .. '\n'):gmatch('([^\n]*)\n') do
            section = line:match('^%s*%[([^%]]+)%]%s*$') or section
            local id = section and section:match('^Setting%.') and line:match('^%s*Id%s*=%s*(.-)%s*$')
            if id then ids[id] = true end
        end
        local out = {}
        section = nil
        for line in (text .. '\n'):gmatch('([^\n]*)\n') do
            local name = line:match('^%s*%[([^%]]+)%]%s*$')
            if name then
                section = name
                local id = name:match('^Setting%.(.+)$')
                if id then line = '[Setting.' .. prefix .. id .. ']' end
            else
                local key, value = line:match('^%s*([%w_]+)%s*=%s*(.-)%s*$')
                if section == 'Mod' and key == 'Id' then line = 'Id=' .. extension.contributor
                elseif key == 'Id' and section and section:match('^Setting%.') then line = 'Id=' .. prefix .. value
                elseif key and RULES[key] and ids[value] then line = key .. '=' .. prefix .. value end
            end
            out[#out + 1] = line
        end
        return table.concat(out, '\n')
    end
    local lines = {}
    block(lines, 'Mod', {{'Id', extension.contributor}, {'Name', extension.title}})
    block(lines, 'Setting.' .. prefix .. 'LoadTimeVerifications', {{'Id', prefix .. 'LoadTimeVerifications'},
        {'Type', 'toggle'}, {'Label', 'Enable load-time verifications'}, {'Description', DESCRIPTION},
        {'Group', 'Diagnostics'}, {'PresetValues', '0|1'}, {'PresetLabels', 'Off|On'}, {'Default', '0'},
        {'ConfigFile', 'settings.ini'}, {'ConfigSection', 'Verifications'}, {'ConfigKey', 'LoadTime'}})
    return table.concat(lines, '\n')
end

-- The setting ids of a manifest, in order.
function M.settingIds(manifest)
    local ids, section = {}, nil
    for line in (manifest .. '\n'):gmatch('([^\n]*)\n') do
        local name = line:match('^%s*%[([^%]]+)%]%s*$')
        if name then section = name
        elseif section and section:match('^Setting%.') then
            local id = line:match('^%s*Id%s*=%s*(.-)%s*$')
            if id then ids[#ids + 1] = id end
        end
    end
    return ids
end

-- What to publish: a list of {id=, contribution=} in publishing order. This folder's page comes
-- first: it hosts the slots. Each contributor needs its own directory: publishers name their
-- files by generation only. root: this mod's folder. modsDirectory: the folder holding all mods.
-- read(path) returns a file's text, or nil.
function M.contributions(found, root, modsDirectory, version, read)
    local out = {{id = M.id, contribution = {
        pages = {{id = M.id, name = M.name, version = version, manifest = M.pageManifest(found),
            configDirectory = root, attach = root:match('([^/\\]+)$')}},
    }}}
    for _, extension in ipairs(found) do
        local directory = modsDirectory .. '/' .. extension.folder
        local manifest = M.sourceManifest(extension, read and read(directory .. '/mod_settings.ini'))
        out[#out + 1] = {id = extension.contributor, contribution = {
            -- An extension with a mod_settings.ini is listed under its Mod Id, not its folder.
            pages = {{id = extension.contributor, name = extension.title, manifest = manifest,
                configDirectory = directory, attach = extension.id, visible = false}},
            rows = {{page = extension.contributor, slot = M.slotAddress(extension), settings = M.settingIds(manifest)}},
        }}
    end
    return out
end

-- The publishing directory of one contributor, inside this folder's cache.
function M.directory(cache, id) return cache .. '/' .. id end

return M
