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

-- A missing or older ModCore Settings only means no grouped entry: the extensions are listed
-- on their own and keep working.
local ok, why = pcall(function()
    local menu = require('extensions_menu')
    local found = menu.installed(modsDirectory, exists)
    local cache = root .. '/cache'
    ensureCache(menu, cache)
    local contributions = require('menu_contributions')
    for _, entry in ipairs(menu.contributions(found, root, modsDirectory, readVersion(), read)) do
        local directory = menu.directory(cache, entry.id)
        ensureCache(menu, directory)
        contributions.publisher(assert(rawget(_G, 'ModRef'), 'ModRef unavailable'),
            {id = entry.id, directory = directory}):publish(entry.contribution)
    end
    print(string.format('[UE4SSLuaEXtensions] menu published for %d extension(s)\n', menu.count(found)))
end)
if not ok then print('[UE4SSLuaEXtensions] menu unavailable: ' .. tostring(why) .. '\n') end
