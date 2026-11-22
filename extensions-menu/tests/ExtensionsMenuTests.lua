-- Run from the repository root: lua5.4 extensions-menu/tests/ExtensionsMenuTests.lua
package.path = 'extensions-menu/Scripts/?.lua;extensions-menu/Scripts/vendor/?.lua;' .. package.path
local Menu = require('extensions_menu')
local Contributions = require('menu_contributions')

local function present(...)
    local set = {}
    for _, folder in ipairs({...}) do set['/mods/' .. folder .. '/mod.json'] = true end
    return function(path) return set[path] == true end
end
local EVENTS, FILES, SOUNDS = '0_ModCore_UE4SSLuaEventBridge', '0_ModCore_UE4SSLuaFileBridge', '0_ModCore_UE4SSLuaSoundBridge'
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
assert(ids(Menu.installed('/mods', present(SOUNDS, FILES, EVENTS)))
    == 'UE4SSLuaEventBridge,UE4SSLuaFileBridge,UE4SSLuaSoundBridge')
assert(ids(Menu.installed('/mods', present(SOUNDS))) == 'UE4SSLuaSoundBridge')
local all = Menu.installed('/mods', present(EVENTS, FILES, SOUNDS))
assert(Menu.count(all) == 3 and Menu.pageManifest(all):find('PresetLabels=3|3', 1, true))
assert(Menu.pageManifest(all):find('Category.Files', 1, true) < Menu.pageManifest(all):find('Category.Sounds', 1, true))
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
local sounds = Menu.extensions[3]
assert(events.label == 'Events Extension' and files.label == 'Files Extension' and sounds.label == 'Sounds Extension')
assert(Menu.slotAddress(sounds) == 'UE4SSLuaEXtensions:Sounds' and sounds.contributor == 'UE4SSLuaEXtensions.Sounds')

-- The source page is the extension's own mod_settings.ini under the contributor's Mod Id.
local ini = '[Mod]\r\nId=UE4SSLuaEventBridge\r\nName=UE4SS Lua Event Bridge\r\nConfigSection=UE4SSLuaEXtensions.Events\r\n\r\n'
    .. '[Setting.A]\r\nId=EnableVerifications\r\nType=toggle\r\n\r\n'
    .. '[Setting.B]\r\nId=EnableHooks\r\nType=toggle\r\nVisibleWhen=EnableVerifications\r\n'
    .. 'VisibleValues=1\r\n\r\n'
    .. '[Setting.C]\r\nId=Volume\r\nType=toggle\r\nConfigKey=Loudness\r\n'
local source = Menu.sourceManifest(events, ini)
assert(source:find('[Mod]\nId=UE4SSLuaEXtensions.Events\nName=UE4SS Lua Event Bridge\nConfigSection=UE4SSLuaEXtensions.Events\n', 1, true),
    'Mod Id replaced, the storage section kept')
assert(not source:find('Id=UE4SSLuaEventBridge', 1, true) and not source:find('\r', 1, true))
-- Ids are unique on the shared page: each takes the extension's prefix, rules naming them too.
assert(table.concat(Menu.settingIds(source), ',') == 'Events_EnableVerifications,Events_EnableHooks,Events_Volume',
    'every setting, in order')
-- Storage keeps the original key: stated by the extension, or its unprefixed id.
assert(source:find('[Setting.Events_A]\nId=Events_EnableVerifications\nConfigKey=EnableVerifications\nType=toggle\n', 1, true))
assert(source:find('Id=Events_EnableHooks\nConfigKey=EnableHooks\n', 1, true))
assert(source:find('Id=Events_Volume\nType=toggle\nConfigKey=Loudness', 1, true) and not source:find('ConfigKey=Volume', 1, true))
assert(source:find('VisibleWhen=Events_EnableVerifications\n', 1, true), 'rules follow the ids')
-- Without that file, the verification switch alone, with the extension's storage.
for _, extension in ipairs(Menu.extensions) do
    local fallback = Menu.sourceManifest(extension)
    assert(fallback:find('Id=' .. extension.contributor .. '\n', 1, true))
    assert(not fallback:find('ConfigFile=', 1, true) and fallback:find('ConfigKey=EnableVerifications', 1, true),
        'config.ini at [<contributor>] EnableVerifications')
    assert(fallback:find('Type=toggle', 1, true) and fallback:find('Default=0', 1, true), 'off by default')
    assert(table.concat(Menu.settingIds(fallback), ',') == extension.slot .. '_EnableVerifications')
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
assert(table.concat(list[2].contribution.rows[1].settings, ',') == 'Events_EnableVerifications,Events_EnableHooks,Events_Volume', 'read from the file')
assert(table.concat(list[3].contribution.rows[1].settings, ',') == 'Files_EnableVerifications', 'fallback without the file')
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
-- A missing cache directory (a fresh install) is created; only a file written into it counts.
local function disk(existing)
    local dirs, calls = {}, {}
    for _, path in ipairs(existing or {}) do dirs[path] = true end
    local ops = {probe = function(path) return dirs[path] == true end}
    local function way(name, creates, result)
        return function(path)
            calls[#calls + 1] = name
            if creates then dirs[path] = true end
            if result == 'raise' then error(name .. ' refused', 0) end
            return result
        end
    end
    return ops, way, calls, dirs
end
do -- already there: nothing is created
    local ops, way, calls = disk({'/c'})
    ops.makeDir, ops.shellMakeDir = way('bridge', true), way('shell', true)
    assert(Menu.ensureDirectory('/c', ops) == true and #calls == 0)
end
do -- the File Bridge creates it first
    local ops, way, calls, dirs = disk()
    ops.makeDir, ops.shellMakeDir = way('bridge', true), way('shell', true)
    assert(Menu.ensureDirectory('/c', ops) == true and table.concat(calls, ',') == 'bridge' and dirs['/c'])
end
do -- the bridge fails, the shell creates it but reports failure: the probe decides
    local ops, way, calls = disk()
    ops.makeDir, ops.shellMakeDir = way('bridge', false, 'raise'), way('shell', true, 'raise')
    assert(Menu.ensureDirectory('/c', ops) == true and table.concat(calls, ',') == 'bridge,shell')
end
do -- without the bridge, the shell alone
    local ops, way, calls = disk()
    ops.shellMakeDir = way('shell', true, false)
    assert(Menu.ensureDirectory('/c', ops) == true and table.concat(calls, ',') == 'shell')
end
do -- a call that reports success without creating it is a failure, with each way's reason
    local ops, way = disk()
    ops.makeDir, ops.shellMakeDir = way('bridge', false, true), way('shell', false, 'raise')
    local made, why = Menu.ensureDirectory('/c', ops)
    assert(made == nil and why:find('cannot create the cache directory /c', 1, true)
        and why:find('makeDir: no file can be written there', 1, true) and why:find('shell refused', 1, true), why)
end

-- Config reload: only the Log Bridge reloads at run time, on its page's Apply and on a reload.
do
    local LOGS = '0_ModCore_UE4SSLuaLogBridge'
    local applied, reloaded, reloads = {}, {}, 0
    local settings = {
        subscribe = function(id, callback) applied[id] = callback end,
        onReload = function(id, callback) reloaded[id] = callback end,
    }
    local bridge = function() return {ReloadSettings = function() reloads = reloads + 1 return true end} end
    local globals = function(name) return name == 'UE4SSLuaLogBridge' and bridge or nil end
    local found = Menu.installed('/mods', present(EVENTS, FILES, SOUNDS, LOGS))
    local subscribed = Menu.subscribeReloads(found, settings, globals)
    assert(#subscribed == 1 and subscribed[1].id == 'UE4SSLuaLogBridge')
    assert(next(applied) == 'UE4SSLuaEXtensions.Logs' and next(reloaded) == 'UE4SSLuaEXtensions.Logs')
    applied['UE4SSLuaEXtensions.Logs']({revision = 1})
    reloaded['UE4SSLuaEXtensions.Logs']({revision = 1, reason = 'focus', time = 0})
    assert(reloads == 2)
    -- Without the Log Bridge installed nothing is subscribed; without its global nothing runs.
    assert(#Menu.subscribeReloads(Menu.installed('/mods', present(EVENTS)), settings, globals) == 0)
    Menu.subscribeReloads(found, settings, function() return nil end)
    applied['UE4SSLuaEXtensions.Logs']({revision = 2})
    assert(reloads == 2)
    -- A refused reload raises, so settings_api reports it.
    local refusing = function() return function() return {ReloadSettings = function() return nil, 'invalid', 'no' end} end end
    Menu.subscribeReloads(found, settings, refusing)
    local ok, why = pcall(reloaded['UE4SSLuaEXtensions.Logs'], {revision = 3})
    assert(not ok and why:find('ReloadSettings: no', 1, true), why)
end

-- The Log Bridge's level rows.
do
    -- [Levels] values become ranks; a repeated key (any case) keeps its first place and spelling and
    -- its last value; other sections and lines stay as they are.
    local text = '[UE4SSLuaEXtensions.Logs]\r\nGlobalLevel=DEBUG\r\n\r\n[Levels]\r\n; note\r\nPreymonition = DEBUG\r\n'
        .. 'Fangdango.combat=trace\r\nOld=loud\r\npreymonition=5\r\nKept=3\r\nLater=inherit\r\n[Other]\r\nX=DEBUG'
    local normalized, changed, keys = Menu.normalizeLevels(text)
    assert(changed)
    assert(normalized == '[UE4SSLuaEXtensions.Logs]\r\nGlobalLevel=DEBUG\r\n\r\n[Levels]\r\n; note\r\nPreymonition=5\r\n'
        .. 'Fangdango.combat=1\r\nOld=0\r\nKept=3\r\nLater=0\r\n[Other]\r\nX=DEBUG', normalized)
    assert(keys.preymonition == 'Preymonition' and keys['fangdango.combat'] == 'Fangdango.combat' and keys.kept == 'Kept')
    local again, changedAgain = Menu.normalizeLevels(normalized)
    assert(again == normalized and not changedAgain, 'normalizing twice changes nothing')
    local same, untouched = Menu.normalizeLevels('[Levels]\nA=1\n')
    assert(same == '[Levels]\nA=1\n' and not untouched)
    assert(select(2, Menu.normalizeLevels('')) == false and select(2, Menu.normalizeLevels(nil)) == false)
    assert(Menu.rankOf(' Warning ') == 4 and Menu.rankOf('0') == 0 and Menu.rankOf('8') == nil)

    -- A row per module, sorted, then its categories beyond main indented under it; keys keep
    -- config.ini's spelling. The module row is main's: main never has a row of its own.
    local list = {
        {name = 'Zeta', loaded = true, categories = {{name = 'main', loaded = true}}},
        {name = 'Preymonition', loaded = true, categories = {{name = 'main', loaded = true},
            {name = 'combat', loaded = true}, {name = 'net', loaded = false}, {name = 'old', loaded = false}}},
        {name = 'Gone Mod', loaded = false, categories = {}},
        {name = 'Idle Mod', loaded = false, categories = {}},
        {name = 'Parent', loaded = false, categories = {{name = 'main', loaded = false}, {name = 'ai', loaded = false}}},
    }
    local stored = {preymonition = 'preymonition', ['preymonition.combat'] = 'PREYMONITION.Combat',
        ['preymonition.net'] = 'Preymonition.net', ['gone mod'] = 'Gone Mod', ['parent.ai'] = 'Parent.ai'}
    local rows, count = Menu.levelRows(list, stored)
    local function labels(text)
        local order = {}
        for label in text:gmatch('\nLabel=([^\n]+)') do order[#order + 1] = label end
        return table.concat(order, ',')
    end
    local I = Menu.INDENT
    -- Not loaded shows only while [Levels] sets it (a module also for a category it holds).
    assert(labels(rows) == table.concat({'Gone Mod (not loaded)', 'Parent (not loaded)', I .. 'ai (not loaded)',
        'Preymonition', I .. 'combat', I .. 'net (not loaded)', 'Zeta'}, ','), labels(rows))
    assert(count == 7)
    assert(not rows:find('main', 1, true), 'no main row')
    assert(rows:find('Id=Level_Gone_Mod\n', 1, true) and rows:find('ConfigKey=Gone Mod\n', 1, true))
    assert(rows:find('ConfigKey=preymonition\n', 1, true) and rows:find('ConfigKey=PREYMONITION.Combat\n', 1, true))
    assert(rows:find('ConfigKey=Preymonition.net\n', 1, true) and rows:find('ConfigKey=Parent.ai\n', 1, true))
    assert(rows:find('Group=Levels\nPresetValues=0|1|2|3|4|5|6|7\nPresetLabels=Inherit|Trace|Debug|Info|Warn|Error|Critical|Off\n'
        .. 'Default=0\nConfigSection=Levels\n', 1, true))
    assert(select(2, rows:gsub('%[Setting%.', '')) == 7)
    -- A module that registered only main is one row.
    local single, one = Menu.levelRows({{name = 'Fangdango', loaded = true, categories = {{name = 'main', loaded = true}}}})
    assert(one == 1 and labels(single) == 'Fangdango' and single:find('ConfigKey=Fangdango\n', 1, true))
    assert(select(2, Menu.levelRows(nil)) == 0 and Menu.levelRows({}) == '')

    -- With the loggers, [Levels] is tidied: Inherit entries naming nothing registered go, set ones
    -- stay (shown as not loaded), and <Module>.main folds into <Module>.
    local fang = {{name = 'Fangdango', loaded = true, categories = {{name = 'main', loaded = true}}},
        {name = 'Preymonition', loaded = true, categories = {{name = 'main', loaded = true}, {name = 'combat', loaded = true}}}}
    local config = '[UE4SSLuaEXtensions.Logs]\r\nGlobalLevel=4\r\n\r\n[Levels]\r\nFangdango=3\r\nFangdango.main=0\r\n'
        .. 'Fangdango.config=0\r\nFangdango.ui=0\r\nFangdango.input=2\r\nPreymonition=0\r\nPreymonition.combat=0\r\n'
        .. 'Preymonition.main=5\r\nGhost=0\r\nGhost.main=6\r\nShade.main=0\r\nStay=1\r\nBoth=2\r\nboth.MAIN=6\r\n'
    local tidy, tidied, left = Menu.normalizeLevels(config, fang)
    assert(tidied)
    assert(tidy == '[UE4SSLuaEXtensions.Logs]\r\nGlobalLevel=4\r\n\r\n[Levels]\r\nFangdango=3\r\nFangdango.input=2\r\n'
        .. 'Preymonition=5\r\nPreymonition.combat=0\r\nGhost=6\r\nStay=1\r\nBoth=2\r\n', tidy)
    assert(left.fangdango == 'Fangdango' and left['fangdango.input'] == 'Fangdango.input' and left.ghost == 'Ghost'
        and left['fangdango.config'] == nil and left['fangdango.main'] == nil and left['preymonition.main'] == nil)
    assert(select(2, Menu.normalizeLevels(tidy, fang)) == false, 'tidying twice changes nothing')
    -- A module set only through <Module>.main keeps that spelling for <Module>.
    local moved = Menu.normalizeLevels('[Levels]\nMyMod.Main=4\n', {})
    assert(moved == '[Levels]\nMyMod=4\n', moved)
    -- Fangdango today: its module row, and the stale category it still sets.
    local fangList = {{name = 'Fangdango', loaded = true, categories = {{name = 'main', loaded = true},
        {name = 'config', loaded = false}, {name = 'ui', loaded = false}, {name = 'input', loaded = false}}}}
    assert(labels((Menu.levelRows(fangList, left))) == 'Fangdango,' .. I .. 'input (not loaded)')
    local plainFang = Menu.normalizeLevels('[Levels]\nFangdango=3\nFangdango.config=0\nFangdango.ui=0\nFangdango.input=0\n', fangList)
    local _, _, plainKeys = Menu.normalizeLevels(plainFang)
    assert(labels((Menu.levelRows(fangList, plainKeys))) == 'Fangdango', 'a main-only module is one row')

    -- Ids stay unique when names differ only in characters an id can't hold.
    local twins = Menu.levelRows({{name = 'A B', categories = {}}, {name = 'A-B', categories = {}}})
    assert(twins:find('Id=Level_A_B\n', 1, true) and twins:find('Id=Level_A_B_2\n', 1, true))
    -- At most MAX_LEVEL_ROWS rows.
    local many = {}
    for n = 1, 150 do
        many[n] = {name = string.format('Mod%03d', n), categories = {{name = 'main'}, {name = 'ui'}}}
    end
    assert(select(2, Menu.levelRows(many)) == Menu.MAX_LEVEL_ROWS)

    -- The rows follow the Global level row, in its group, before the next section.
    local ini = '[Mod]\r\nId=UE4SSLuaLogBridge\r\nConfigSection=UE4SSLuaEXtensions.Logs\r\n\r\n[Setting.GlobalLevel]\r\n'
        .. 'Id=GlobalLevel\r\nType=picker\r\nGroup=Levels\r\nPresetValues=1|2\r\nPresetLabels=A|B\r\nDefault=1\r\n\r\n'
        .. '[Setting.EnableVerifications]\r\nId=EnableVerifications\r\nType=toggle\r\nGroup=Diagnostics\r\n'
    local LOGS = Menu.extensions[4]
    assert(LOGS.levels and LOGS.slot == 'Logs')
    local placed = Menu.withLevelRows(ini, rows)
    assert(placed:find('Default=1\n\n[Setting.Level_', 1, true) and placed:find('ConfigKey=Zeta\n\n[Setting.EnableVerifications]', 1, true))
    assert(Menu.withLevelRows(ini, '') == ini)
    assert(Menu.withLevelRows('[Mod]\nId=X\n', '[Setting.Y]\nId=Y\n'):find('Id=X\n\n[Setting.Y]', 1, true))

    -- The contribution: prefixed ids, the stored keys kept, rows in entries of at most 32.
    local entry = Menu.extensionContribution(LOGS, '/mods/' .. LOGS.folder, ini, {list = many})
    local ids = Menu.settingIds(entry.contribution.pages[1].manifest)
    assert(#ids == 2 + Menu.MAX_LEVEL_ROWS and ids[1] == 'Logs_GlobalLevel' and ids[2] == 'Logs_Level_Mod001'
        and ids[#ids] == 'Logs_EnableVerifications', ids[2])
    local manifest = entry.contribution.pages[1].manifest
    assert(manifest:find('[Setting.Logs_Level_Mod001]\nId=Logs_Level_Mod001\nType=picker', 1, true))
    assert(manifest:find('ConfigSection=Levels\nConfigKey=Mod001\n', 1, true) and not manifest:find('ConfigKey=Level_', 1, true))
    local entries = entry.contribution.rows
    assert(#entries == math.ceil(#ids / 32))
    local joined = {}
    for _, row in ipairs(entries) do
        assert(#row.settings <= 32 and row.slot == 'UE4SSLuaEXtensions:Logs' and row.page == LOGS.contributor)
        for _, id in ipairs(row.settings) do joined[#joined + 1] = id end
    end
    assert(table.concat(joined, ',') == table.concat(ids, ','), 'every setting once, in order')
    assert(Contributions.validate(entry.id, entry.contribution))
    -- Without the bridge's list (an older bridge, or none) the page is its file alone.
    local plain = Menu.extensionContribution(LOGS, '/mods/' .. LOGS.folder, ini, nil)
    assert(table.concat(Menu.settingIds(plain.contribution.pages[1].manifest), ',') == 'Logs_GlobalLevel,Logs_EnableVerifications')
    -- Other extensions never get level rows.
    local other = Menu.extensionContribution(Menu.extensions[1], '/mods/x', ini, {list = many})
    assert(#Menu.settingIds(other.contribution.pages[1].manifest) == 2)
    -- contributions passes the list on to the Log Bridge's page.
    local withLogs = Menu.contributions({LOGS}, '/mods/0_ModCore_UE4SSLuaEXtensions', '/mods', '1.1.0',
        function(path) return path == '/mods/' .. LOGS.folder .. '/mod_settings.ini' and ini or nil end,
        {list = list, keys = stored})
    assert(#Menu.settingIds(withLogs[2].contribution.pages[1].manifest) == 2 + 7)
end

print('PASS extensions menu: detection, count, sections, settings files, storage and publishing as a ModCore Settings module')
