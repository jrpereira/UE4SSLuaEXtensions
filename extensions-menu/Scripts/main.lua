local source = debug.getinfo(1, 'S').source:gsub('^@', '')
local scripts = assert(source:match('^(.*)[/\\][^/\\]+$'), 'cannot locate Scripts')
local root = assert(scripts:match('^(.*)[/\\]Scripts$'), 'cannot locate the mod folder')
local modsDirectory = root:match('^(.*)[/\\][^/\\]+$') or '.'
package.path = scripts .. '/vendor/?.lua;' .. scripts .. '/?.lua;' .. package.path

local function exists(path)
    local file = io.open(path, 'rb')
    if file then file:close() return true end
    return false
end

local function read(path)
    local file = io.open(path, 'rb')
    if not file then return nil end
    local text = file:read('a')
    file:close()
    return text
end

local function readVersion()
    local text = read(root .. '/mod.json')
    return text and text:match('"version"%s*:%s*"([^"]+)"')
end

local function probe(path)
    local file = io.open(path .. '/.lex-probe', 'wb')
    if not file then return false end
    file:close()
    os.remove(path .. '/.lex-probe')
    return true
end

-- UE4SSLuaFileBridge, when it is loaded, creates folders through the Windows API; its 'mod'
-- location is this folder.
local function bridgeMakeDir()
    local bridge = rawget(_G, 'UE4SSLuaFileBridge')
    if bridge == nil then return nil end
    local ok, env = pcall(bridge)
    if not ok or type(env) ~= 'table' or type(env.MakeDir) ~= 'function' then return nil end
    return function(path)
        assert(path:sub(1, #root + 1) == root .. '/', 'not inside the mod folder')
        local made, code, message = env.MakeDir('mod/' .. path:sub(#root + 2))
        if not made then error(tostring(message or code), 0) end
    end
end

local function shellMakeDir(path)
    local command
    if package.config:sub(1, 1) == '\\' then
        assert(not path:find('["%%!\r\n]'), 'unsupported cache path')
        local native = path:gsub('/', '\\')
        command = 'if not exist "' .. native .. '" mkdir "' .. native .. '"'
    else
        command = "mkdir -p -- '" .. path:gsub("'", "'\\''") .. "'"
    end
    os.execute(command)
end

local cacheOps
local function ensureCache(menu, path)
    cacheOps = cacheOps or {probe = probe, makeDir = bridgeMakeDir(), shellMakeDir = shellMakeDir}
    assert(menu.ensureDirectory(path, cacheOps))
end

-- The Log Bridge's environment in this Lua state, when the bridge lists its loggers (API 2).
local function logBridge()
    local bridge = rawget(_G, 'UE4SSLuaLogBridge')
    if bridge == nil then return nil end
    local ok, env = pcall(bridge)
    if not ok or type(env) ~= 'table' or type(env.ListLoggers) ~= 'function' then return nil end
    return env
end

-- The Log Bridge's level rows: its loggers, and its config.ini's [Levels] made readable by the
-- menu (written back only when that changed something).
local function levelsFor(menu, extension)
    local env = logBridge()
    if not env then return nil end
    local list = env.ListLoggers()
    if type(list) ~= 'table' then return nil end
    local path = modsDirectory .. '/' .. extension.folder .. '/config.ini'
    local files = require('safe_file')
    local readable, text = pcall(files.read, path)
    local keys = {}
    if readable and text then
        local normalized, changed
        normalized, changed, keys = menu.normalizeLevels(text)
        if changed then
            local written, why = pcall(files.write, path, normalized)
            if written then print('[UE4SSLuaEXtensions] Log Bridge config.ini: [Levels] values stored as ranks\n')
            else print('[UE4SSLuaEXtensions] Log Bridge config.ini not rewritten: ' .. tostring(why) .. '\n') end
        end
    end
    return {list = list, keys = keys}
end

-- A missing or older ModCore Settings only means no grouped entry: the extensions are listed
-- on their own and keep working.
local ok, why = pcall(function()
    local menu = require('extensions_menu')
    local found = menu.installed(modsDirectory, exists)
    local cache = root .. '/cache'
    ensureCache(menu, cache)
    local contributions = require('menu_contributions')
    local shared = assert(rawget(_G, 'ModRef'), 'ModRef unavailable')
    local logs
    for _, extension in ipairs(found) do
        if extension.levels then logs = extension end
    end
    local publishers = {}
    for _, entry in ipairs(menu.contributions(found, root, modsDirectory, readVersion(), read,
        logs and levelsFor(menu, logs) or nil)) do
        local directory = menu.directory(cache, entry.id)
        ensureCache(menu, directory)
        local publisher = contributions.publisher(shared, {id = entry.id, directory = directory})
        publisher:publish(entry.contribution)
        publishers[entry.id] = {publisher = publisher, manifest = entry.contribution.pages[1].manifest}
    end
    print(string.format('[UE4SSLuaEXtensions] menu published for %d extension(s)\n', menu.count(found)))
    -- Settings that apply while the game runs (the Log Bridge's levels) are re-read when
    -- ModCore Settings applies their page or reloads config (its button, the window's focus).
    local reloading, reloadWhy = pcall(menu.subscribeReloads, found, require('settings_api'),
        function(name) return rawget(_G, name) end)
    if not reloading then print('[UE4SSLuaEXtensions] config reload unavailable: ' .. tostring(reloadWhy) .. '\n') end

    -- This folder loads before the mods that register loggers, so the Logs page is published
    -- again whenever the bridge's generation moves (a module or category registered, a level
    -- changed) and its rows came out different. ModCore Settings reads the new generation the
    -- next time it builds the menu.
    local env = logs and publishers[logs.contributor] and logBridge()
    if not env or type(rawget(_G, 'LoopAsync')) ~= 'function' then return end
    local function generation()
        local stats = env.GetStats()
        return type(stats) == 'table' and stats.generation or nil
    end
    local seen = generation()
    local directory = modsDirectory .. '/' .. logs.folder
    local function refresh()
        local refreshed, refreshWhy = pcall(function()
            local entry = menu.extensionContribution(logs, directory, read(directory .. '/mod_settings.ini'),
                levelsFor(menu, logs))
            local record = publishers[logs.contributor]
            if entry.contribution.pages[1].manifest == record.manifest then return end
            record.publisher:publish(entry.contribution)
            record.manifest = entry.contribution.pages[1].manifest
        end)
        if not refreshed then print('[UE4SSLuaEXtensions] Logs page not republished: ' .. tostring(refreshWhy) .. '\n') end
    end
    -- The bridge is asked from the game thread: UE4SS may refuse native calls from the async
    -- loop's thread.
    local onGameThread = rawget(_G, 'ExecuteInGameThread')
    local queued = false
    local function check()
        queued = false
        local now = generation()
        if now ~= nil and now ~= seen then
            seen = now
            refresh()
        end
    end
    LoopAsync(2000, function()
        if type(onGameThread) ~= 'function' then
            check()
        elseif not queued then
            queued = true
            onGameThread(check)
        end
        return false
    end)
end)
if not ok then print('[UE4SSLuaEXtensions] menu unavailable: ' .. tostring(why) .. '\n') end
