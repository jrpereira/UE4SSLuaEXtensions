-- Lists the UE4SS Lua extensions installed beside this folder as one ModCore Settings module.
--
-- This folder's page shows how many extensions are installed, then a section per installed
-- extension, each holding one slot. It is ModCore Settings' Extensions tab: the page is hidden
-- and its rows fill the slot settings:extensions, keeping their sections, and the "ModCore Lua
-- Extensions" entry in the mod list, attached to this folder, opens that tab. Each extension
-- publishes a hidden page built from its own mod_settings.ini whose rows fill its slot with all
-- of its settings; the page is attached to the extension, so the extension's own entry is hidden
-- and its settings show in its section instead, each with the extension's own storage. Without
-- the tab (an older ModCore Settings) this folder's page is listed on its own, and without the
-- slots (a ModCore Settings that cannot host rows) the hidden pages show as ordinary pages, so
-- nothing becomes unreachable.
--
-- Everything here is pure: the caller supplies the mods directory, a file test and a reader.

local M = {}

M.id = 'UE4SSLuaEXtensions'
M.name = 'ModCore Lua Extensions'

-- Order here is the order of the sections. contributor publishes the hidden page that holds the
-- extension's settings; it must differ from the extension's own id, which its mod_settings.ini
-- already puts in the menu. slot names the extension's section and slot on this folder's page.
-- reload names the global of an extension whose settings apply while the game runs: its
-- environment's ReloadSettings() re-reads them (see M.subscribeReloads). levels marks the Log
-- Bridge, whose page also gets a row per module and category (see M.levelRows).
M.extensions = {
    {id = 'UE4SSLuaEventBridge', folder = '0_ModCore_UE4SSLuaEventBridge', label = 'Events Extension',
        title = 'UE4SS Lua Event Bridge', slot = 'Events', contributor = 'UE4SSLuaEXtensions.Events'},
    {id = 'UE4SSLuaFileBridge', folder = '0_ModCore_UE4SSLuaFileBridge', label = 'Files Extension',
        title = 'UE4SS Lua File Bridge', slot = 'Files', contributor = 'UE4SSLuaEXtensions.Files'},
    {id = 'UE4SSLuaSoundBridge', folder = '0_ModCore_UE4SSLuaSoundBridge', label = 'Sounds Extension',
        title = 'UE4SS Lua Sound Bridge', slot = 'Sounds', contributor = 'UE4SSLuaEXtensions.Sounds'},
    {id = 'UE4SSLuaLogBridge', folder = '0_ModCore_UE4SSLuaLogBridge', label = 'Logs Extension',
        title = 'UE4SS Lua Log Bridge', slot = 'Logs', contributor = 'UE4SSLuaEXtensions.Logs',
        reload = 'UE4SSLuaLogBridge', levels = true},
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
-- extension's slot as a prefix. Storage must not follow: a row without its own ConfigKey
-- gets its original id as ConfigKey, and [Mod] ConfigSection is kept, so values stay in the
-- extension's config.ini at [UE4SSLuaEXtensions.<Slot>] <id>.
-- Without that file, the startup verification switch its Scripts read: config.ini
-- [<contributor>] EnableVerifications, the contributor being this page's [Mod] Id.
local RULES = {VisibleWhen = true, mcLabelWhen = true}
function M.sourceManifest(extension, text)
    local prefix = extension.slot .. '_'
    if text then
        text = text:gsub('\r\n', '\n')
        local ids, keyed, section = {}, {}, nil
        for line in (text .. '\n'):gmatch('([^\n]*)\n') do
            section = line:match('^%s*%[([^%]]+)%]%s*$') or section
            local id = section and section:match('^Setting%.') and line:match('^%s*Id%s*=%s*(.-)%s*$')
            if id then ids[id] = true end
            if section and section:match('^Setting%.') and line:match('^%s*ConfigKey%s*=') then keyed[section] = true end
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
                elseif key == 'Id' and section and section:match('^Setting%.') then
                    line = 'Id=' .. prefix .. value
                    if not keyed[section] then line = line .. '\nConfigKey=' .. value end
                elseif key and RULES[key] and ids[value] then line = key .. '=' .. prefix .. value end
            end
            out[#out + 1] = line
        end
        return table.concat(out, '\n')
    end
    local lines = {}
    block(lines, 'Mod', {{'Id', extension.contributor}, {'Name', extension.title}})
    block(lines, 'Setting.' .. prefix .. 'EnableVerifications', {{'Id', prefix .. 'EnableVerifications'},
        {'Type', 'toggle'}, {'Label', 'Enable load-time verifications'}, {'Description', DESCRIPTION},
        {'Group', 'Diagnostics'}, {'PresetValues', '0|1'}, {'PresetLabels', 'Off|On'}, {'Default', '0'},
        {'ConfigKey', 'EnableVerifications'}})
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

-- The Log Bridge's level rows ----------------------------------------------------------------
--
-- Under the Global level row, one picker per module the bridge lists (ListLoggers), then one
-- per category it registered beyond main, indented. Each stores its rank in the bridge's
-- config.ini, [Levels] <Module>=<rank> or <Module>.<category>=<rank>; 0 is Inherit, which the bridge reads as no
-- level of its own. ModCore Settings writes every row's key when the page opens, so Inherit
-- cannot be a missing key.

local RANKS = {trace = 1, debug = 2, info = 3, warn = 4, warning = 4, error = 5, critical = 6, off = 7, none = 7,
    inherit = 0}
for rank = 0, 7 do RANKS[tostring(rank)] = rank end
local LEVEL_VALUES = '0|1|2|3|4|5|6|7'
local LEVEL_LABELS = 'Inherit|Trace|Debug|Info|Warn|Error|Critical|Off'
-- Category rows sit under their module: non-breaking spaces, which the menu does not trim.
M.INDENT = string.rep('\u{A0}', 4)
-- DMM reads at most 256 settings per page; the bridge's own rows take a few.
M.MAX_LEVEL_ROWS = 240
-- A slot rows entry names at most 32 settings.
local ROWS_PER_ENTRY = 32

local function trim(text) return (text:match('^%s*(.-)%s*$')) end

-- The rank a [Levels] value gives (0 for Inherit), or nil.
function M.rankOf(value) return RANKS[trim(value):lower()] end

local function lines(text)
    local out = {}
    for line, ending in (text):gmatch('([^\n]*)(\n?)') do
        if line == '' and ending == '' then break end
        out[#out + 1] = {text = line, ending = ending}
    end
    return out
end

-- The [Levels] keys a list (ListLoggers' modules) registered this session, folded: each loaded
-- module and each loaded category other than main.
function M.registeredKeys(list)
    local registered = {}
    for _, module in ipairs(list or {}) do
        local name = module.name:lower()
        if module.loaded ~= false then registered[name] = true end
        for _, category in ipairs(module.categories or {}) do
            if category.loaded ~= false and category.name:lower() ~= 'main' then
                registered[name .. '.' .. category.name:lower()] = true
            end
        end
    end
    return registered
end

-- config.ini text with its [Levels] entries made readable by the menu, which stores ranks: a
-- level name becomes its rank, an unknown value Inherit (0, as the bridge already reads it),
-- and repeated keys (any case) one entry with the last value, where the first was. With list
-- (ListLoggers' modules) the entries are also tidied: <Module>.main folds into <Module>, the
-- module row's key (the module's own level wins when both are set), and an Inherit entry
-- that names nothing registered this session goes, since it sets nothing. Other lines are
-- kept as they are. Returns the text and whether it changed, and the keys left, folded to
-- their spelling in the file.
function M.normalizeLevels(text, list)
    local out, keys, at, ranks, section = {}, {}, {}, {}, nil
    for _, entry in ipairs(lines(text or '')) do
        local line = entry.text:gsub('\r$', '')
        local cr = entry.text:sub(-1) == '\r' and '\r' or ''
        local header = line:match('^%s*%[([^%]]+)%]%s*$')
        if header then section = trim(header):lower() end
        local key, value = line:match('^%s*([^=;#]-)%s*=%s*(.-)%s*$')
        if not header and section == 'levels' and key and key ~= '' then
            local folded = key:lower()
            local rank = M.rankOf(value) or 0
            ranks[folded] = rank
            if at[folded] then
                local first = out[at[folded]]
                first.text = keys[folded] .. '=' .. rank .. first.cr
                out[#out + 1] = false
            else
                keys[folded] = key
                out[#out + 1] = {text = key .. '=' .. rank .. cr, cr = cr, ending = entry.ending}
                at[folded] = #out
            end
        else
            out[#out + 1] = {text = entry.text, cr = cr, ending = entry.ending}
        end
    end
    local function set(folded, rank)
        ranks[folded] = rank
        out[at[folded]].text = keys[folded] .. '=' .. rank .. out[at[folded]].cr
    end
    local function drop(folded)
        out[at[folded]] = false
        at[folded], keys[folded], ranks[folded] = nil, nil, nil
    end
    if list then
        local mains = {}
        for folded in pairs(at) do
            if folded:match('^.+%.main$') then mains[#mains + 1] = folded end
        end
        table.sort(mains)
        for _, folded in ipairs(mains) do
            local module = folded:sub(1, -6)
            local rank = ranks[folded]
            if at[module] then
                if ranks[module] == 0 and rank ~= 0 then set(module, rank) end
                drop(folded)
            elseif rank ~= 0 then
                at[module], keys[module] = at[folded], keys[folded]:sub(1, #module)
                at[folded], keys[folded], ranks[folded] = nil, nil, nil
                set(module, rank)
            else
                drop(folded)
            end
        end
        local registered = M.registeredKeys(list)
        local idle = {}
        for folded in pairs(at) do
            if ranks[folded] == 0 and not registered[folded] then idle[#idle + 1] = folded end
        end
        for _, folded in ipairs(idle) do drop(folded) end
    end
    local parts = {}
    for _, entry in ipairs(out) do
        if entry then parts[#parts + 1] = entry.text .. entry.ending end
    end
    local result = table.concat(parts)
    return result, result ~= (text or ''), keys
end

local function rowId(key, used)
    local base = 'Level_' .. key:gsub('[^%w]', '_')
    local id, n = base, 1
    while used[id] do n = n + 1; id = base .. '_' .. n end
    used[id] = true
    return id
end

-- The level rows (manifest text) for list (ListLoggers' modules, nil or empty for none) and
-- keys (normalizeLevels' keys: the spelling of each [Levels] key in config.ini). Modules are
-- sorted by name. A module's row is its main category's, so main has no row of its own; a
-- category row follows for each other category it registered. A module or category that has
-- not registered this session shows, marked so, only while [Levels] sets it (a module also
-- when one of its categories shows), so it can be cleared. Returns the text and the number of
-- rows.
function M.levelRows(list, keys)
    keys = keys or {}
    local modules = {}
    for _, module in ipairs(list or {}) do modules[#modules + 1] = module end
    table.sort(modules, function(a, b)
        if a.name:lower() ~= b.name:lower() then return a.name:lower() < b.name:lower() end
        return a.name < b.name
    end)
    local out, used, count = {}, {}, 0
    local function row(key, label, description)
        if count >= M.MAX_LEVEL_ROWS then return false end
        count = count + 1
        local id = rowId(key, used)
        block(out, 'Setting.' .. id, {{'Id', id}, {'Type', 'picker'}, {'Label', label}, {'Description', description},
            {'Group', 'Levels'}, {'PresetValues', LEVEL_VALUES}, {'PresetLabels', LEVEL_LABELS}, {'Default', '0'},
            {'ConfigSection', 'Levels'}, {'ConfigKey', key}})
        return true
    end
    for _, module in ipairs(modules) do
        local folded = module.name:lower()
        local moduleKey = keys[folded] or module.name
        local categories = {}
        for _, category in ipairs(module.categories or {}) do
            local categoryFolded = folded .. '.' .. category.name:lower()
            if category.name:lower() ~= 'main' and (category.loaded ~= false or keys[categoryFolded]) then
                categories[#categories + 1] = {name = category.name, loaded = category.loaded,
                    key = keys[categoryFolded] or (moduleKey .. '.' .. category.name)}
            end
        end
        if module.loaded ~= false or keys[folded] or #categories > 0 then
            local absent = module.loaded == false and ' (not loaded)' or ''
            if not row(moduleKey, module.name .. absent, 'The level of ' .. module.name .. "'s lines. Inherit: the "
                .. "module's own default level when it sets one, else the global level. Takes effect at once.") then
                break
            end
            for _, category in ipairs(categories) do
                local missing = category.loaded == false and ' (not loaded)' or ''
                if not row(category.key, M.INDENT .. category.name .. missing, 'The level of ' .. module.name .. "'s "
                    .. category.name .. ' lines. Inherit: the level of ' .. module.name .. '. Takes effect at once.') then
                    break
                end
            end
        end
    end
    return table.concat(out, '\n'), count
end

-- A settings manifest with rows inserted after the Global level row's section (the end
-- when it has none), so they share its group.
function M.withLevelRows(text, rows)
    if not rows or rows == '' then return text end
    text = text:gsub('\r\n', '\n')
    local out, inserted, inGlobal = {}, false, false
    for line in (text .. '\n'):gmatch('([^\n]*)\n') do
        local header = line:match('^%s*%[([^%]]+)%]%s*$')
        if header and inGlobal and not inserted then
            out[#out + 1] = rows
            inserted = true
        end
        if header then inGlobal = header == 'Setting.GlobalLevel' end
        out[#out + 1] = line
    end
    while out[#out] == '' do out[#out] = nil end
    if not inserted then out[#out + 1] = '\n' .. rows end
    return table.concat(out, '\n') .. '\n'
end

-- One contributor's entry: the hidden page holding an extension's settings and the rows
-- that fill its slot. levels (the Log Bridge's only): {list=, keys=} as for M.levelRows.
function M.extensionContribution(extension, directory, text, levels)
    if extension.levels and levels and text then
        text = M.withLevelRows(text:gsub('\r\n', '\n'), (M.levelRows(levels.list, levels.keys)))
    end
    local manifest = M.sourceManifest(extension, text)
    local ids, rows = M.settingIds(manifest), {}
    for first = 1, #ids, ROWS_PER_ENTRY do
        rows[#rows + 1] = {page = extension.contributor, slot = M.slotAddress(extension),
            settings = table.move(ids, first, math.min(first + ROWS_PER_ENTRY - 1, #ids), 1, {})}
    end
    return {id = extension.contributor, contribution = {
        -- An extension with a mod_settings.ini is listed under its Mod Id, not its folder.
        pages = {{id = extension.contributor, name = extension.title, manifest = manifest,
            configDirectory = directory, attach = extension.id, visible = false}},
        rows = rows,
    }}
end

-- ModCore Settings' tab this folder's page fills.
M.TAB = 'settings:extensions'

-- What to publish: a list of {id=, contribution=} in publishing order. This folder's page comes
-- first: it hosts the slots. It is hidden, its rows fill ModCore Settings' Extensions tab, and
-- its mod list entry (<id>.entry) opens that tab. Each contributor needs its own directory:
-- publishers name their files by generation only. root: this mod's folder. modsDirectory: the
-- folder holding all mods. read(path) returns a file's text, or nil. levels: as for
-- M.extensionContribution.
function M.contributions(found, root, modsDirectory, version, read, levels)
    local manifest = M.pageManifest(found)
    local ids, rows = M.settingIds(manifest), {}
    for first = 1, #ids, ROWS_PER_ENTRY do
        rows[#rows + 1] = {page = M.id, slot = M.TAB,
            settings = table.move(ids, first, math.min(first + ROWS_PER_ENTRY - 1, #ids), 1, {})}
    end
    local out = {{id = M.id, contribution = {
        pages = {{id = M.id, name = M.name, version = version, manifest = manifest,
            configDirectory = root, visible = false},
            {id = M.id .. '.entry', name = M.name, version = version, attach = root:match('([^/\\]+)$'),
                link = M.TAB}},
        rows = rows,
    }}}
    for _, extension in ipairs(found) do
        local directory = modsDirectory .. '/' .. extension.folder
        out[#out + 1] = M.extensionContribution(extension, directory,
            read and read(directory .. '/mod_settings.ini'), levels)
    end
    return out
end

-- The publishing directory of one contributor, inside this folder's cache.
function M.directory(cache, id) return cache .. '/' .. id end

-- For each installed extension with a reload global: when ModCore Settings applies its page
-- (subscribe, keyed by the page's contributor id) or fans out a config reload (onReload),
-- call <global>().ReloadSettings(). settings is the vendored settings_api; globals looks a
-- global up (rawget on _G in the game). Returns the extensions subscribed.
function M.subscribeReloads(found, settings, globals)
    local subscribed = {}
    for _, extension in ipairs(found) do
        if extension.reload then
            local function reload()
                local bridge = globals(extension.reload)
                if bridge == nil then return end
                local env = bridge()
                local ok, code, message = env.ReloadSettings()
                if not ok then error(extension.reload .. '.ReloadSettings: ' .. tostring(message or code), 0) end
            end
            settings.subscribe(extension.contributor, reload)
            settings.onReload(extension.contributor, reload)
            subscribed[#subscribed + 1] = extension
        end
    end
    return subscribed
end

-- Makes sure a directory exists and takes files. Only a file written into it counts: a mkdir's
-- own result is not trusted, since early in UE4SS's startup a shell mkdir may report failure for
-- a folder it did create. Each way of creating it is tried in turn, and the directory re-probed:
--   ops.probe(path)       true when a file can be written into path
--   ops.makeDir(path)     optional, UE4SSLuaFileBridge's MakeDir; may raise
--   ops.shellMakeDir(path) optional, os.execute mkdir; may raise
-- Returns true, or nil and the reasons each way failed.
function M.ensureDirectory(path, ops)
    if ops.probe(path) then return true end
    local reasons = {}
    for _, way in ipairs({'makeDir', 'shellMakeDir'}) do
        local make = ops[way]
        if make then
            local ok, why = pcall(make, path)
            if ops.probe(path) then return true end
            reasons[#reasons + 1] = way .. ': ' .. (ok and 'no file can be written there' or tostring(why))
        end
    end
    return nil, 'cannot create the cache directory ' .. path
        .. (#reasons > 0 and ' (' .. table.concat(reasons, '; ') .. ')' or '')
end

return M
