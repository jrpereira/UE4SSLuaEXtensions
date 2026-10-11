-- The menu entry that groups the UE4SS Lua extensions installed beside this folder.
--
-- ModCore Settings lists a mod with settings of its own as a separate entry. This module
-- publishes one page, "UE4SS Lua Extensions", that mirrors each installed extension's
-- verification toggle into a slot, and one hidden page per extension that holds the toggle.
-- Hidden pages are attached to the extension's folder, so ModCore Settings hides that
-- extension's own entry while this page is shown. An extension that is not installed gets
-- no slot, and without ModCore Settings support for hiding, each extension is simply
-- listed on its own.
--
-- Everything here is pure: the caller supplies the mods directory and a file test.

local M = {}

M.id = 'UE4SSLuaEXtensions'
M.name = 'UE4SS Lua Extensions'

-- Order here is the order of the rows on the page.
M.extensions = {
    {id = 'UE4SSLuaEventBridge', folder = '0_ModCore_UE4SSLuaEventBridge', label = 'Events Extension',
        title = 'UE4SS Lua Event Bridge', slot = 'events', setting = 'EventsLoadTimeVerifications'},
    {id = 'UE4SSLuaFileBridge', folder = '0_ModCore_UE4SSLuaFileBridge', label = 'Files Extension',
        title = 'UE4SS Lua File Bridge', slot = 'files', setting = 'FilesLoadTimeVerifications'},
}

local HEADING = 'Enable startup library verifications'
local DESCRIPTION = "Runs the extension's in-game checks when the game loads and writes PASS and FAIL "
    .. 'lines to UE4SS.log. Takes effect on the next launch.'

-- The extensions whose mod folder sits in modsDirectory, in page order.
-- exists(path) is true for a readable file.
function M.installed(modsDirectory, exists)
    local found = {}
    for _, extension in ipairs(M.extensions) do
        if exists(modsDirectory .. '/' .. extension.folder .. '/mod.json') then found[#found + 1] = extension end
    end
    return found
end

-- The submodule counter shown on the page.
function M.count(found) return #found end

local function block(lines, name, fields)
    lines[#lines + 1] = '[' .. name .. ']'
    for _, pair in ipairs(fields) do lines[#lines + 1] = pair[1] .. '=' .. pair[2] end
    lines[#lines + 1] = ''
end

-- A read-only row: DMM pickers need two choices, so the value is given twice.
local function display(lines, id, group, label, value, extra)
    local fields = {{'Id', id}, {'Label', label}, {'Group', group}, {'Type', 'picker'}, {'Default', '0'},
        {'PresetValues', '0|1'}, {'PresetLabels', value .. '|' .. value}, {'mcReadOnly', '1'}}
    for _, pair in ipairs(extra or {}) do fields[#fields + 1] = pair end
    block(lines, 'Setting.' .. id, fields)
end

-- The page's own manifest: the counter, then a slot for each installed extension.
function M.hostManifest(found)
    local lines = {}
    block(lines, 'Mod', {{'Id', M.id}, {'Name', M.name}})
    block(lines, 'Category.Summary', {{'mcHeading', '0'}})
    display(lines, 'LEX_Installed', 'Summary', 'Installed extensions', tostring(M.count(found)))
    if #found == 0 then
        display(lines, 'LEX_None', 'Summary', 'Extensions', 'None installed')
        return table.concat(lines, '\n')
    end
    block(lines, 'Category.Verifications', {{'mcLabel', HEADING}})
    for _, extension in ipairs(found) do
        display(lines, 'LEX_Pending_' .. extension.slot, 'Verifications', extension.label, 'Not available',
            {{'mcSlot', extension.slot}, {'mcSlotLabel', '1'}})
    end
    return table.concat(lines, '\n')
end

-- An extension's own verification toggle. Its storage is the extension's settings.ini, which its
-- Scripts read at startup. The setting id differs per extension: both land on one page.
function M.sourceManifest(extension)
    local lines = {}
    block(lines, 'Mod', {{'Id', extension.id}, {'Name', extension.title}})
    block(lines, 'Setting.' .. extension.setting, {{'Id', extension.setting}, {'Type', 'toggle'},
        {'Label', 'Enable load-time verifications'}, {'Description', DESCRIPTION}, {'Group', 'Diagnostics'},
        {'PresetValues', '0|1'}, {'PresetLabels', 'Off|On'}, {'Default', '0'}, {'ConfigFile', 'settings.ini'},
        {'ConfigSection', 'Verifications'}, {'ConfigKey', 'LoadTime'}})
    return table.concat(lines, '\n')
end

-- What to publish: a list of {id=, contribution=} in publishing order. Slots must belong to a
-- different contributor than the pages that fill them, so each extension publishes as its own
-- contributor and this folder's page as another.
-- root: this mod's folder. modsDirectory: the folder holding all mods.
function M.contributions(found, root, modsDirectory, version)
    local out = {{id = M.id, contribution = {pages = {{
        id = M.id, name = M.name, version = version, manifest = M.hostManifest(found),
        configDirectory = root, attach = root:match('([^/\\]+)$'),
    }}}}}
    for _, extension in ipairs(found) do
        out[#out + 1] = {id = extension.id, contribution = {
            pages = {{id = extension.id, name = extension.title, manifest = M.sourceManifest(extension),
                configDirectory = modsDirectory .. '/' .. extension.folder, attach = extension.folder,
                visible = false}},
            rows = {{page = extension.id, slot = M.id .. ':' .. extension.slot, settings = {extension.setting}}},
        }}
    end
    return out
end

return M
