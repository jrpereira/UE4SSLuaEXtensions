-- Run from the repository root: lua5.4 extensions-menu/tests/ExtensionsTabTests.lua
-- End to end through ModCore Settings: the pages this folder publishes become ModCore
-- Settings' Extensions tab. ModCore Settings' page parses through DMM with all five tabs;
-- the extensions' rows fill the tab, keeping their sections, the Logs page's rows inside its
-- section; the mod list entry opens the tab; Apply there saves into the Log Bridge's
-- config.ini and reaches the Log Bridge's reload. Without the tab, the page is listed on its own.
-- Needs the real DMM parser and ModCore Settings' sources, which CI does not have; skip without
-- them. MCS_SCRIPTS_PATH defaults to ../ModCoreSettings/Scripts.
local choicesPath = os.getenv('DMM_CHOICES_PATH')
local settings = os.getenv('MCS_SCRIPTS_PATH') or '../ModCoreSettings/Scripts'
local probe = io.open(settings .. '/menu_pages.lua', 'rb')
if not choicesPath or not probe then
    print('SKIP Extensions tab: set DMM_CHOICES_PATH and MCS_SCRIPTS_PATH')
    return
end
probe:close()
-- ModCore Settings' files require their siblings; this folder's own modules have other names.
package.path = settings .. '/?.lua;' .. settings .. '/vendor/?.lua;extensions-menu/Scripts/?.lua;' .. package.path
local choices = dofile(choicesPath)
local Menu = require('extensions_menu')
-- This folder publishes through its own copy of the contribution format.
local Published = dofile('extensions-menu/Scripts/vendor/menu_contributions.lua')
local Contributions = require('menu_contributions')

-- The same wrappers, in the same order, as ModCore Settings' DMM extension.
local fieldTypes = dofile(settings .. '/field_types.lua').new()
fieldTypes:register('keybind', dofile(settings .. '/keybind_editor.lua'))
fieldTypes:register('range', dofile(settings .. '/range_editor.lua'))
fieldTypes:install({choices = choices, controls = {build = function() end}})
dofile(settings .. '/navigation.lua').install(choices)
dofile(settings .. '/presentation.lua').install(choices, {}, nil)
dofile(settings .. '/init_config.lua').install(choices)
local PageHooks = dofile(settings .. '/page_hooks.lua')
PageHooks.install(choices)
local MenuPages = dofile(settings .. '/menu_pages.lua')
local Slots = dofile(settings .. '/menu_slots.lua')
local Links = dofile(settings .. '/page_links.lua')
local MenuData = dofile(settings .. '/menu_data.lua')
local Groups = dofile(settings .. '/browser_groups.lua')

-- A Mods folder in TMPDIR holding the Log Bridge and the Event Bridge.
local function tmpname()
    local dir = os.getenv('TMPDIR')
    if not dir then return os.tmpname() end
    local path = dir:gsub('/+$', '') .. '/lex_' .. os.time() .. '_' .. math.random(1000000000)
    assert(io.open(path, 'wb')):close()
    return path
end
local function q(path) return string.format('%q', path) end
local mods = tmpname(); assert(os.remove(mods))
local LOGS, EVENTS = '0_ModCore_UE4SSLuaLogBridge', '0_ModCore_UE4SSLuaEventBridge'
local ROOT = mods .. '/0_ModCore_UE4SSLuaEXtensions'
for _, folder in ipairs({LOGS, EVENTS, '0_ModCore_UE4SSLuaEXtensions', '1_ModCore_Settings'}) do
    assert(os.execute('mkdir -p ' .. q(mods .. '/' .. folder)))
end
local function readFile(path)
    local file = io.open(path, 'rb')
    if not file then return nil end
    local text = file:read('a'); file:close(); return text
end
local function writeFile(path, text)
    local file = assert(io.open(path, 'wb')); file:write(text); file:close()
end
writeFile(mods .. '/' .. LOGS .. '/mod_settings.ini', assert(readFile('bridge-logs/packaging/mod_settings.ini')))
writeFile(mods .. '/' .. EVENTS .. '/mod_settings.ini', assert(readFile('bridge-events/packaging/mod_settings.ini')))
writeFile(mods .. '/' .. LOGS .. '/config.ini', '[UE4SSLuaEXtensions.Logs]\nGlobalLevel=4\n\n[Levels]\nModCoreSettings=0\n')
local found = {}
for _, extension in ipairs(Menu.extensions) do
    if extension.folder == LOGS or extension.folder == EVENTS then found[#found + 1] = extension end
end
local list = {{name = 'ModCoreSettings', categories = {{name = 'main'}, {name = 'slots'}}}}
local _, _, keys = Menu.normalizeLevels(readFile(mods .. '/' .. LOGS .. '/config.ini'), list)
local published = Menu.contributions(found, ROOT, mods, '1.2.0', readFile, {list = list, keys = keys})
local contributions = {}
for _, entry in ipairs(published) do
    assert(Published.validate(entry.id, entry.contribution), 'valid for ' .. entry.id)
    local copy = {id = entry.id, pages = entry.contribution.pages, rows = entry.contribution.rows}
    contributions[#contributions + 1] = copy
end

-- The Log Bridge's reload, as main.lua subscribes it: by each contributor's page id.
local subscribed, reloads = {}, 0
Menu.subscribeReloads(found, {subscribe = function(id, callback) subscribed[id] = callback end,
    onReload = function() end}, function(name)
        return name == 'UE4SSLuaLogBridge' and function()
            return {ReloadSettings = function() reloads = reloads + 1; return true end}
        end or nil
    end)

local function build(mcsMenu)
    local reports = {}
    local function report(event, detail) reports[#reports + 1] = event .. ' ' .. tostring(detail) end
    local controller = assert(Slots.install(choices, report, Contributions, function() return nil end)
        or choices.mcSlotsController)
    choices.mcSlotsController = controller
    local function compile(menu) return MenuData.manifest(menu) end
    local function hooked(page)
        local own = PageHooks.loader()(page.hooks)
        local context = PageHooks.context(page)
        local manifest, menu = PageHooks.manifest(own, context, compile)
        return own, manifest, context, menu
    end
    local function stored(page) return PageHooks.stored(page.menu), PageHooks.context(page) end
    local mcs = {id = 'ModCoreSettings', pages = {{id = 'ModCoreSettings', name = 'ModCore Settings',
        attach = '1_ModCore_Settings', menu = mcsMenu, configDirectory = mods .. '/1_ModCore_Settings'}}}
    local function placeholder(folder)
        return {id = 'detected:ue4ss:' .. folder:lower(), name = folder, detectedKind = 'ue4ss', noSettings = true,
            choices = {}}
    end
    -- DMM lists each extension by its own mod_settings.ini, and the extensions folder as a
    -- mod without settings.
    local function own(folder, id, name)
        local path = mods .. '/' .. folder .. '/mod_settings.ini'
        local items = choices.parse(readFile(path))
        return {id = id, name = name, choices = items, settingsCount = #items, choicesLoaded = true, path = path}
    end
    local providers = {placeholder('0_ModCore_UE4SSLuaEXtensions'), placeholder('1_ModCore_Settings'),
        own(EVENTS, 'UE4SSLuaEventBridge', 'UE4SS Lua Event Bridge'), own(LOGS, 'UE4SSLuaLogBridge', 'UE4SS Lua Log Bridge'),
        {id = 'Zeta', name = 'Zeta', choices = {{id = 'z'}}, path = '/z/mod_settings.ini'}}
    local all = {mcs}
    for _, contribution in ipairs(contributions) do all[#all + 1] = contribution end
    local state = {}
    MenuPages.apply(providers, all, function(content) return choices.parse(content) end, state, report,
        controller, hooked, compile, stored)
    assert(#reports == 0, table.concat(reports, ' / '))
    local byId, listed = {}, {}
    for index, provider in ipairs(providers) do
        byId[provider.id] = index
        if not provider.mcBrowserHidden then listed[#listed + 1] = provider.id end
    end
    return providers, byId, table.concat(listed, ','), state, controller, reports
end

local mcsMenu = dofile(settings .. '/mcs_page.lua')
local providers, byId, listed, state, controller, reports = build(mcsMenu)
-- The mod list: the extensions folder's entry, ModCore Settings, Zeta. The extensions' own
-- entries and this folder's page are kept, listed nowhere.
assert(listed == 'UE4SSLuaEXtensions.entry,ModCoreSettings,Zeta', listed)
local entry = providers[byId['UE4SSLuaEXtensions.entry']]
assert(entry.mcModuleEntry and entry.mcLinkSlot.host == 'ModCoreSettings' and entry.mcLinkSlot.slot == 'extensions',
    'the entry opens the Extensions tab')
for _, id in ipairs({'UE4SSLuaEXtensions', 'UE4SSLuaEXtensions.Logs', 'UE4SSLuaEXtensions.Events'}) do
    assert(byId[id] and providers[byId[id]].mcBrowserHidden, id .. ' is kept, unlisted')
end
assert(not byId.UE4SSLuaLogBridge and not byId.UE4SSLuaEventBridge, "the extensions' own entries are hidden")

-- ModCore Settings' page as the browser leaves it: Developer Tools appended, with no tools.
local host = providers[byId.ModCoreSettings]
host.mcManifest = host.mcManifest:gsub('%s*$', '') .. '\n\n' .. Groups.manifest({}, {}, {})
host.choices = choices.parse(host.mcManifest); host.settingsCount = #host.choices
controller.inserts = state.inserts
local applied = {}
controller.applied = function(provider, event)
    applied[#applied + 1] = provider.id
    if subscribed[provider.id] then subscribed[provider.id](event) end
end
assert(controller:load(host, function() return nil end), 'the Extensions tab fills')
assert(#reports == 0, table.concat(reports, ' / '))
local model = choices.open(host)
assert(not model.error, model.error)
assert(#model.items <= 256)
local index = {}
for i, item in ipairs(model.items) do index[item.id] = i end
local tabs = model.items[1]
assert(tabs.id == 'MCS_Page' and table.concat(tabs.labels, '|') == 'Settings|Controls|Templates|Extensions|Developer Tools'
    and table.concat(tabs.values, '|') == '0|1|2|3|4' and model.pending[1] == 0, 'five tabs, opening on Settings')
local function shown(page)
    model:set(1, page)
    local visible = model:visibility()
    local out = {}
    for i, item in ipairs(model.items) do if visible[i] then out[#out + 1] = item.id end end
    return table.concat(out, ',')
end
-- The Extensions tab: the count, then each extension's section with its settings, the
-- Logs page's level rows between its Global level and its verification switch.
local tab = shown(3)
assert(tab == 'MCS_Page,LEX_Installed,Events_EnableVerifications,Events_EnableHooks,Logs_GlobalLevel,Logs_Level_ModCoreSettings,'
    .. 'Logs_Level_ModCoreSettings_slots,Logs_EnableVerifications', tab)
assert(not index.MCS_Extensions and not index.LEX_Events and not index.LEX_Logs, 'placeholders give way')
assert(model.items[index.LEX_Installed].labels[1] == '2', 'two extensions installed')
assert(shown(4) == 'MCS_Page,MCS_Index_Empty_4', 'Developer Tools without tools shows None installed')
assert(not shown(0):find('LEX_', 1, true) and not shown(1):find('LEX_', 1, true), 'only the Extensions tab shows them')
model:set(1, 3)
assert(not model:dirty(), 'tabs are navigation')
-- Each section keeps its heading.
local function heading(id) return model.items[index[id]].mcGroup.label end
assert(heading('LEX_Installed') == 'Extensions' and heading('Events_EnableVerifications') == 'Events Extension'
    and heading('Logs_GlobalLevel') == 'Logs Extension' and heading('Logs_EnableVerifications') == 'Logs Extension',
    heading('LEX_Installed') .. '/' .. heading('Logs_GlobalLevel'))
assert(model.items[index.Logs_GlobalLevel].mcSlotSource == providers[byId['UE4SSLuaEXtensions.Logs']]
    and model.items[index.LEX_Installed].mcSlotSource == providers[byId.UE4SSLuaEXtensions], 'each row keeps its owner')

-- Apply on the tab saves into the Log Bridge's config.ini and reaches its reload.
model:set(index.Logs_GlobalLevel, 2)
model:set(index.Logs_Level_ModCoreSettings_slots, 5)
assert(model:dirty())
local ok, why = model:apply()
assert(ok, why)
local text = readFile(mods .. '/' .. LOGS .. '/config.ini')
assert(text:find('[UE4SSLuaEXtensions.Logs]\nGlobalLevel=2\n', 1, true) and text:find('ModCoreSettings.slots=5', 1, true),
    text)
assert(not readFile(ROOT .. '/config.ini'), 'the tab itself stores nothing')
local heard = {}
for _, id in ipairs(applied) do heard[id] = true end
assert(heard['UE4SSLuaEXtensions.Logs'] and reloads == 1, 'the Log Bridge reloads on the tab\'s Apply')

-- A DMM page over these providers: the entry opens ModCore Settings' page on the Extensions tab.
local page = {rows = {}, allRows = {}, controlStatus = {}, mode = 'browser'}
for at, provider in ipairs(providers) do
    local row = {providerIndex = at}
    page.allRows[#page.allRows + 1] = row
    if not provider.mcBrowserHidden then page.rows[#page.rows + 1] = row end
end
local trace = {}
page.controls = {tick = function() return false end, refresh = function() end,
    select = function(_, at) trace[#trace + 1] = 'select ' .. at end,
    show = function(self, at)
        controller:load(providers[at], function() return nil end)
        self.model = choices.open(providers[at]); self.provider = providers[at]
    end}
function page:showBrowser() self.mode = 'browser'; trace[#trace + 1] = 'browser' end
function page:showDetail(row)
    self.mode = 'detail'
    self.controls:show(self.rows[row].providerIndex)
    trace[#trace + 1] = self.controls.provider.id
end
Links.wrap(page, providers, {setText = function() end}, controller, function(event, detail)
    reports[#reports + 1] = event .. ' ' .. tostring(detail)
end)
local entryRow
for row, candidate in ipairs(page.rows) do
    if providers[candidate.providerIndex].id == 'UE4SSLuaEXtensions.entry' then entryRow = row end
end
page:showDetail(entryRow)
assert(page.controls.provider == host and page.controls.model.pending[1] == 3
    and trace[#trace] == 'select ' .. index.LEX_Installed, table.concat(trace, ','))
assert(#reports == 0, table.concat(reports, ' / '))

-- A ModCore Settings without the Extensions tab: this folder's page is listed on its own,
-- its sections filled, and the entry that would open the tab is gone.
local older = {}
for key, value in pairs(mcsMenu) do older[key] = value end
older.fields = {}
for _, field in ipairs(mcsMenu.fields) do
    if field.id ~= 'MCS_Extensions' then older.fields[#older.fields + 1] = field end
end
older.groups = {}
for _, group in ipairs(mcsMenu.groups) do
    if group.id ~= 'Extensions' then older.groups[#older.groups + 1] = group end
end
local oldProviders, oldIds, alone, oldState, oldController = build(older)
assert(alone == 'UE4SSLuaEXtensions,ModCoreSettings,Zeta', alone)
local lex = oldProviders[oldIds.UE4SSLuaEXtensions]
assert(lex.name == 'ModCore Lua Extensions' and not lex.mcBrowserHidden)
oldController.inserts = oldState.inserts
assert(oldController:load(lex, function() return nil end), 'its sections fill')
local own = {}
for _, item in ipairs(lex.choices) do own[#own + 1] = item.id end
assert(table.concat(own, ',') == 'LEX_Installed,Events_EnableVerifications,Events_EnableHooks,Logs_GlobalLevel,'
    .. 'Logs_Level_ModCoreSettings,Logs_Level_ModCoreSettings_slots,Logs_EnableVerifications', table.concat(own, ','))

assert(os.execute('rm -r ' .. q(mods)))
print('PASS Extensions tab: ModCore Settings\' page parses with five tabs, the extensions fill the Extensions '
    .. 'tab with their sections and the Logs rows, the entry opens it, Apply saves and reloads')
