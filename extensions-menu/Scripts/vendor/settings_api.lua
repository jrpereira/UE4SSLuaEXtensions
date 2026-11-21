-- Vendored from ModCoreSettings (owner). Do not edit copies; change the source and re-vendor.
-- Public, game-agnostic client for durable Apply notifications. Consumers copy this file
-- unchanged into their Scripts/vendor folder; ModCoreSettings publishes the transport.
-- Version 2: a value is a number, or text (a keyed picker's key, a range's 'low|high', a
-- keybind) exactly as the page stores it. Version 3 adds onReload.
local M={version=3}
local subscriptions={}
local owner=tostring({}):gsub('%W','')
local prefix='MC_SettingsApplied_v2_'

local function hex(value)
    assert(type(value)=='string' and #value>0 and #value<=128,'invalid provider Id')
    return (value:gsub('.',function(c) return string.format('%02x',c:byte()) end))
end
local function unhex(value)
    assert(#value>0 and #value<=256 and #value%2==0 and not value:find('[^%da-f]'),'invalid encoded Id')
    return (value:gsub('..',function(c) return string.char(tonumber(c,16)) end))
end
local function finite(value)
    return type(value)=='number' and value==value and math.abs(value)<=1000000000
end
-- 'n<number>' or 's<hex text>'.
local function decode(value)
    local kind,body=value:sub(1,1),value:sub(2)
    if kind=='n' then
        local number=tonumber(body)
        assert(finite(number),'invalid notification value')
        return number
    end
    assert(kind=='s' and #body<=2048 and #body%2==0 and not body:find('[^%da-f]'),'invalid notification value')
    return (body:gsub('..',function(c) return string.char(tonumber(c,16)) end))
end
local function revision(payload)
    return type(payload)=='string' and tonumber(payload:match('^(%d+)\n')) or 0
end
local function read(providerId,payload)
    assert(type(payload)=='string' and #payload<=98304,'invalid settings notification')
    local rev=revision(payload)
    assert(rev and rev>=1 and rev<=9007199254740991,'invalid notification revision')
    local values,changes,count={},{},0
    for line in payload:sub(assert(payload:find('\n',1,true))+1):gmatch('[^\n]+') do
        local key,old,new=line:match('^(%x+) ([^ ]+) ([^ ]+)$')
        assert(key,'invalid settings notification row')
        key=unhex(key);old,new=decode(old),decode(new)
        assert(values[key]==nil,'invalid notification value')
        count=count+1;assert(count<=256,'too many notification values')
        values[key]=new
        if old~=new then changes[key]={old=old,new=new} end
    end
    assert(count>0,'empty settings notification')
    return {providerId=providerId,revision=rev,values=values,changes=changes}
end
local function setCallback(record,callback)
    local ticket={}
    record.callback,record.ticket=callback,ticket
    return function() if record.ticket==ticket then record.callback=nil end end
end

function M.subscribe(providerId,callback)
    assert(type(callback)=='function','settings callback must be a function')
    local command=prefix..hex(providerId)
    local existing=subscriptions[command]
    if existing then return setCallback(existing,callback) end
    assert(ModRef and type(RegisterConsoleCommandHandler)=='function','UE4SS notification API unavailable')
    local claim=command..'.owner'
    assert(ModRef:GetSharedVariable(claim)==nil,'this provider Id already has a subscriber; fully restart after script reloads')
    local record={callback=callback,last=revision(ModRef:GetSharedVariable(command..'.data')) or 0}
    ModRef:SetSharedVariable(claim,owner)
    local ok,result=pcall(RegisterConsoleCommandHandler,command,function()
        local success,message=pcall(function()
            local event=read(providerId,ModRef:GetSharedVariable(command..'.data'))
            if event.revision<=record.last then return end
            record.last=event.revision
            if record.callback then record.callback(event) end
        end)
        if not success then print('[ModCoreSettings API] callback failed for '..providerId..': '..tostring(message)..'\n') end
        return true
    end)
    if not ok or result==false then
        if ModRef:GetSharedVariable(claim)==owner then ModRef:SetSharedVariable(claim,nil) end
        error(tostring(ok and 'console command registration rejected' or result),0)
    end
    subscriptions[command]=record
    return setCallback(record,callback)
end
-- Config reload: callback({revision=,reason=,time=}) runs each time ModCoreSettings fans out
-- a reload (the Mod Index's Reload Config button, the game window regaining focus); it
-- reloads this module's own files. id is any text of 1 to 128 bytes, dots allowed. Calling it
-- again replaces the callback; the returned function unsubscribes. M.reloadReady() tells
-- whether ModCoreSettings handles reloads (a module without it reloads only itself).
local RELOAD='MC_ConfigReload_v1'
local reloads={}
local function index(change)
    local words,seen={},{}
    for word in tostring(ModRef:GetSharedVariable(RELOAD..'.index') or ''):gmatch('%S+') do
        if not seen[word] then seen[word]=true;words[#words+1]=word end
    end
    words=change(words)
    ModRef:SetSharedVariable(RELOAD..'.index',table.concat(words,' '))
end
function M.onReload(id,callback)
    assert(type(callback)=='function','reload callback must be a function')
    local encoded=hex(id)
    local command=RELOAD..'_'..encoded
    local record=reloads[command]
    if not record then
        assert(ModRef and type(RegisterConsoleCommandHandler)=='function','UE4SS notification API unavailable')
        record={last=0}
        local ok,result=pcall(RegisterConsoleCommandHandler,command,function()
            local success,message=pcall(function()
                local data=ModRef:GetSharedVariable(command..'.data')
                local rev,reason,time=tostring(data or ''):match('^(%d+)\n([%w_.:-]+)\n(%d+)$')
                rev=tonumber(rev)
                if not rev or rev<=record.last then return end
                record.last=rev
                if record.callback then record.callback({revision=rev,reason=reason,time=tonumber(time)}) end
            end)
            if not success then print('[ModCoreSettings API] reload failed for '..id..': '..tostring(message)..'\n') end
            return true
        end)
        if not ok or result==false then error(tostring(ok and 'console command registration rejected' or result),0) end
        reloads[command]=record
    end
    record.callback=callback
    ModRef:SetSharedVariable(command..'.owner',owner)
    index(function(words)
        for _,word in ipairs(words) do if word==encoded then return words end end
        words[#words+1]=encoded
        return words
    end)
    local ticket={}
    record.ticket=ticket
    return function()
        if record.ticket~=ticket then return end
        record.callback=nil
        ModRef:SetSharedVariable(command..'.owner',nil)
        index(function(words)
            local kept={}
            for _,word in ipairs(words) do if word~=encoded then kept[#kept+1]=word end end
            return kept
        end)
    end
end
function M.reloadReady()
    return rawget(_G,'ModRef')~=nil and ModRef:GetSharedVariable(RELOAD..'.ready')=='1'
end

return M
