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

local function readVersion()
    local file = io.open(root .. '/mod.json', 'rb')
    if not file then return nil end
    local text = file:read('a')
    file:close()
    return text:match('"version"%s*:%s*"([^"]+)"')
end

local function ensureCache(cache)
    local probe = io.open(cache .. '/.lex-probe', 'wb')
    if probe then probe:close() os.remove(cache .. '/.lex-probe') return end
    local command
    if package.config:sub(1, 1) == '\\' then
        assert(not cache:find('["%%!\r\n]'), 'unsupported cache path')
        command = 'if not exist "' .. cache:gsub('/', '\\') .. '" mkdir "' .. cache:gsub('/', '\\') .. '"'
    else
        command = "mkdir -p -- '" .. cache:gsub("'", "'\\''") .. "'"
    end
    local made = os.execute(command)
    assert(made == true or made == 0, 'cannot create the cache directory')
end

-- A missing or older ModCore Settings only means no grouped entry: the extensions are listed
-- on their own and keep working.
local ok, why = pcall(function()
    local menu = require('extensions_menu')
    local found = menu.installed(modsDirectory, exists)
    local cache = root .. '/cache'
    ensureCache(cache)
    local contributions = require('menu_contributions')
    for _, entry in ipairs(menu.contributions(found, root, modsDirectory, readVersion())) do
        contributions.publisher(assert(rawget(_G, 'ModRef'), 'ModRef unavailable'),
            {id = entry.id, directory = cache}):publish(entry.contribution)
    end
    print(string.format('[UE4SSLuaEXtensions] menu published for %d extension(s)\n', menu.count(found)))
end)
if not ok then print('[UE4SSLuaEXtensions] menu unavailable: ' .. tostring(why) .. '\n') end
