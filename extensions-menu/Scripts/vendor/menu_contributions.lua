-- Vendored from ModCoreSettings (owner). Do not edit copies; change the source and re-vendor.
-- Public client for contributing menu pages through ModCoreSettings. Consumers copy this
-- file unchanged into their Scripts/vendor folder. It only writes data files and one shared
-- variable per contributor; ModCoreSettings reads them while building the menu.
-- Descriptors use contract 1, contract 2 when they carry slot rows or slot links,
-- contract 3 when a page names a hooks file, or contract 4 when a page carries menu data.
local M={version=6,contract=1,rowsContract=2,hooksContract=3,menuContract=4}
local PREFIX='MCS_MenuContrib_v1_'
M.prefix,M.index=PREFIX,PREFIX..'index'
local MAX_PAGES,MAX_ROWS,MAX_ROW_SETTINGS,MAX_MANIFEST=256,64,32,262144
local PAGE_KEYS={id=true,name=true,author=true,version=true,description=true,manifest=true,menu=true,
    configDirectory=true,visible=true,under=true,attach=true,group=true,link=true,hooks=true,icon=true}
local DESCRIPTOR_KEYS={id=true,name=true,author=true,version=true,description=true,manifestFile=true,
    menuFile=true,configDirectory=true,visible=true,under=true,attach=true,group=true,link=true,hooks=true,
    icon=true}
local ROW_KEYS={page=true,slot=true,settings=true}

-- A slot address is '<provider>:<slot>'. A ModCore<Name> provider may be written as its
-- lowercase <name> (controls:visuals); both forms address the same slot.
function M.provider(id)
    local short=type(id)=='string' and id:match('^ModCore(%w+)$')
    return short and short:lower() or id
end
function M.address(value)
    if type(value)~='string' then return nil end
    local provider,slot=value:match('^([^:]+):([%w_]+)$')
    if not provider or #provider>128 or provider:find('%c') or #slot>64 then return nil end
    return M.provider(provider),slot
end

function M.hex(value)
    return (value:gsub('.',function(c) return string.format('%02x',c:byte()) end))
end

local function line(value,limit,name)
    assert(type(value)=='string' and value~='' and #value<=limit and not value:find('%c'),
        'invalid '..name)
end
local function optional(value,limit,name)
    if value~=nil then line(value,limit,name) end
end
local function absolute(value)
    return value:match('^/') or value:match('^%a:[/\\]') or value:match('^[/\\][/\\]')
end

-- Structural validation shared by publishers and ModCoreSettings. Settings manifests are
-- checked by ModCoreSettings when it builds the menu.
local function check(contributor,contribution)
    line(contributor,128,'contributor id')
    assert(type(contribution)=='table' and type(contribution.pages)=='table','contribution needs pages')
    for key in pairs(contribution) do
        assert(key=='pages' or key=='rows','unknown contribution field '..tostring(key))
    end
    local pages=contribution.pages
    assert(#pages<=MAX_PAGES,'too many pages')
    for key in pairs(pages) do
        assert(math.type(key)=='integer' and key>=1 and key<=#pages,'pages must be a list')
    end
    local seen={}
    for n,page in ipairs(pages) do
        local where='page '..n
        assert(type(page)=='table',where..' must be a table')
        for key in pairs(page) do assert(PAGE_KEYS[key],where..': unknown field '..tostring(key)) end
        line(page.id,128,where..' id')
        assert(page.id==contributor or page.id:sub(1,#contributor+1)==contributor..'.',
            where..': id '..page.id..' is not owned by '..contributor)
        assert(not seen[page.id],'duplicate page id '..page.id)
        line(page.name,200,where..' name')
        optional(page.author,120,where..' author')
        optional(page.version,64,where..' version')
        -- icon: a short glyph shown before the page's name in the ModCore browser group.
        optional(page.icon,16,where..' icon')
        assert(page.description==nil or (type(page.description)=='string' and #page.description<=4096
            and not page.description:find('%z')),'invalid '..where..' description')
        assert(page.visible==nil or type(page.visible)=='boolean','invalid '..where..' visible')
        -- module: listed in the ModCore browser group. tool: listed only under the ModCore
        -- page's Developer Tools tab.
        assert(page.group==nil or page.group=='module' or page.group=='tool','invalid '..where..' group')
        -- A hooks page generates its manifest in ModCoreSettings each time the menu is built.
        if page.hooks~=nil then
            line(page.hooks,1024,where..' hooks')
            assert(absolute(page.hooks) and page.hooks:match('%.lua$'),where..': hooks must be an absolute .lua path')
            assert(page.manifest==nil and page.link==nil,where..': a hooks page cannot have a manifest or link')
        end
        -- Menu data describes the page's settings; ModCoreSettings turns it into the page.
        -- Its contents are checked when the menu is built.
        if page.menu~=nil then
            assert(type(page.menu)=='table',where..': menu must be a table')
            assert(page.manifest==nil and page.hooks==nil and page.link==nil,
                where..': a menu page cannot have a manifest, hooks or link')
        end
        if page.manifest~=nil or page.hooks~=nil or page.menu~=nil then
            assert(page.manifest==nil or (type(page.manifest)=='string' and #page.manifest<=MAX_MANIFEST
                and not page.manifest:find('%z')),'invalid '..where..' manifest')
            line(page.configDirectory,1024,where..' configDirectory')
            assert(absolute(page.configDirectory),where..': configDirectory must be absolute')
        else
            assert(page.configDirectory==nil,where..': configDirectory needs a manifest')
        end
        assert(page.under==nil or page.attach==nil,where..': under and attach are exclusive')
        if page.under~=nil then
            line(page.under,128,where..' under')
            assert(seen[page.under],where..': under must name an earlier page')
            assert(not seen[page.under].link,where..': a link page cannot have children')
        end
        -- A link page opens another provider's page at a slot; it has no settings of its own.
        if page.link~=nil then
            local target=M.address(page.link)
            assert(target,'invalid '..where..' link address')
            assert(page.manifest==nil,where..': a link page cannot have a manifest')
            assert(target~=M.provider(contributor) and target~=M.provider(page.id),
                where..': link must open another provider')
        end
        if page.attach~=nil then
            line(page.attach,200,where..' attach')
            assert(not page.attach:find('[/\\]'),where..': attach must be a folder name')
        end
        seen[page.id]=page
    end
    -- Rows publish settings of one of these pages into a slot that another page declares
    -- with mcSlot=<name>. They stay owned, stored and applied by their source page.
    local rows=contribution.rows
    if rows==nil then return true end
    assert(type(rows)=='table' and #rows<=MAX_ROWS,'rows must be a list of at most '..MAX_ROWS)
    for key in pairs(rows) do
        assert(math.type(key)=='integer' and key>=1 and key<=#rows,'rows must be a list')
    end
    for n,row in ipairs(rows) do
        local where='row '..n
        assert(type(row)=='table',where..' must be a table')
        for key in pairs(row) do assert(ROW_KEYS[key],where..': unknown field '..tostring(key)) end
        line(row.page,128,where..' page')
        assert(seen[row.page] and (seen[row.page].manifest or seen[row.page].hooks or seen[row.page].menu),
            where..': page must name a page with settings')
        local target=M.address(row.slot)
        assert(target,'invalid '..where..' slot address')
        for id in pairs(seen) do
            assert(M.provider(id)~=target,where..': slot must belong to another provider')
        end
        local settings=row.settings
        assert(type(settings)=='table' and #settings>=1 and #settings<=MAX_ROW_SETTINGS,
            where..': settings must list 1 to '..MAX_ROW_SETTINGS..' ids')
        local ids={}
        for key,id in pairs(settings) do
            assert(math.type(key)=='integer' and key>=1 and key<=#settings,where..': settings must be a list')
            line(id,128,where..' setting id')
            assert(not id:find('|',1,true),where..': setting id cannot contain |')
            assert(not ids[id],where..': duplicate setting '..id)
            ids[id]=true
        end
    end
    return true
end
M.check=check

function M.validate(contributor,contribution)
    local ok,err=pcall(check,contributor,contribution)
    return ok,not ok and tostring(err) or nil
end

-- An mcTable row's entries, decoded as ModCoreSettings decodes them, so a contributor can
-- check its manifest values offline. value is '<code>:<text>|<code>:<text>|...': the code
-- ends at the first ':', and neither part may contain '|'. Both are trimmed, non-empty and
-- at most 8 characters. columns is mcColumns, 1 through 6 (1 when nil). Returns the
-- entries as {code=,text=} and the column count; raises on malformed input.
M.MAX_TABLE_ENTRIES,M.MAX_TABLE_COLUMNS=256,6
function M.textTable(value,columns)
    assert(type(value)=='string' and #value<=8192 and utf8.len(value) and not value:find('%c'),
        'invalid table text')
    local entries={}
    for entry in (value..'|'):gmatch('(.-)|') do
        local n=#entries+1
        assert(n<=M.MAX_TABLE_ENTRIES,'table exceeds '..M.MAX_TABLE_ENTRIES..' entries')
        local code,text=entry:match('^%s*([^:]-)%s*:%s*(.-)%s*$')
        assert(code and code~='' and text~='','table entry '..n..' needs <code>:<text>')
        assert(utf8.len(code)<=8 and utf8.len(text)<=8,'table entry '..n..' exceeds 8 characters')
        entries[n]={code=code,text=text}
    end
    local count=columns==nil and 1 or math.tointeger(tonumber(columns))
    assert(count and count>=1 and count<=M.MAX_TABLE_COLUMNS
        and (type(columns)~='string' or columns:match('^%d$')),
        'mcColumns must be an integer from 1 through '..M.MAX_TABLE_COLUMNS)
    return entries,count
end

local function escape(value)
    return (value:gsub('\\','\\\\'):gsub('\n','\\n'):gsub('\r','\\r'))
end
local function unescape(value)
    return (value:gsub('\\(.)',function(c)
        return assert(({n='\n',r='\r',['\\']='\\'})[c],'invalid escape')
    end))
end

-- Menu data needs a ModCoreSettings build that compiles it; hooks one that loads them;
-- slot rows and links one that understands slots.
function M.contractFor(contribution)
    for _,page in ipairs(contribution.pages) do if page.menu then return M.menuContract end end
    for _,page in ipairs(contribution.pages) do if page.hooks then return M.hooksContract end end
    return M.needsRows(contribution) and M.rowsContract or M.contract
end

-- Menu data travels as a Lua table constructor holding only strings, finite numbers,
-- booleans and tables; ModCoreSettings loads it without access to any globals.
local function serialize(value,depth)
    assert(depth<=16,'menu nests too deeply')
    local kind=type(value)
    if kind=='string' then return string.format('%q',value) end
    if kind=='boolean' then return tostring(value) end
    if kind=='number' then
        assert(value==value and value~=math.huge and value~=-math.huge,'menu numbers must be finite')
        return math.type(value)=='integer' and tostring(value) or string.format('%.17g',value)
    end
    assert(kind=='table','menu holds only strings, numbers, booleans and tables')
    local parts,keys={}, {}
    for n=1,#value do parts[n]=serialize(value[n],depth+1) end
    for key in pairs(value) do
        if not (math.type(key)=='integer' and key>=1 and key<=#value) then
            assert(type(key)=='string' or math.type(key)=='integer','menu keys are strings or integers')
            keys[#keys+1]=key
        end
    end
    table.sort(keys,function(a,b)
        if type(a)~=type(b) then return type(a)=='number' end
        return a<b
    end)
    for _,key in ipairs(keys) do
        local name=type(key)=='string' and key:match('^[%a_][%w_]*$') and key or '['..serialize(key,depth+1)..']'
        parts[#parts+1]=name..'='..serialize(value[key],depth+1)
    end
    return '{'..table.concat(parts,',')..'}'
end
function M.serialize(menu)
    local text=serialize(menu,0)
    assert(#text<=MAX_MANIFEST,'menu exceeds '..MAX_MANIFEST..' bytes')
    return text
end
function M.deserialize(text)
    assert(type(text)=='string' and #text<=MAX_MANIFEST and text:sub(1,1)=='{','invalid menu data')
    local chunk=assert(load('return '..text,'=menu','t',{}))
    local menu=chunk()
    assert(type(menu)=='table','invalid menu data')
    return menu
end
function M.needsRows(contribution)
    if contribution.rows and #contribution.rows>0 then return true end
    for _,page in ipairs(contribution.pages) do if page.link then return true end end
    return false
end

function M.descriptorName(generation) return 'mcs_menu.'..generation..'.ini' end
function M.manifestName(generation,n) return 'mcs_menu.'..generation..'.'..n..'.ini' end
function M.menuName(generation,n) return 'mcs_menu.'..generation..'.'..n..'.lua' end

-- Returns descriptor text and the manifest files it names.
function M.encode(contributor,generation,contribution)
    check(contributor,contribution)
    local rows=contribution.rows or {}
    local contract=M.contractFor(contribution)
    local out={'[Contribution]','contract='..contract,'id='..contributor,'generation='..generation}
    local files={}
    for n,page in ipairs(contribution.pages) do
        out[#out+1]='[Page.'..n..']'
        for _,key in ipairs({'id','name','author','version','description','configDirectory','under','attach','group','link','hooks','icon'}) do
            if page[key]~=nil then out[#out+1]=key..'='..escape(page[key]) end
        end
        if page.visible~=nil then out[#out+1]='visible='..(page.visible and '1' or '0') end
        if page.manifest then
            local name=M.manifestName(generation,#files+1)
            files[#files+1]={name=name,content=page.manifest}
            out[#out+1]='manifestFile='..name
        elseif page.menu then
            local name=M.menuName(generation,#files+1)
            files[#files+1]={name=name,content=M.serialize(page.menu)}
            out[#out+1]='menuFile='..name
        end
    end
    for n,row in ipairs(rows) do
        out[#out+1]='[Row.'..n..']'
        out[#out+1]='page='..escape(row.page)
        out[#out+1]='slot='..escape(row.slot)
        out[#out+1]='settings='..escape(table.concat(row.settings,'|'))
    end
    return table.concat(out,'\n')..'\n',files
end

-- Parses descriptor text. Manifest contents are filled in by the caller via read(name).
function M.decode(text,read)
    assert(type(text)=='string' and #text<=MAX_MANIFEST,'invalid descriptor')
    local header,pages,rows,current={}, {}, {}, nil
    for raw in (text..'\n'):gmatch('([^\n]*)\n') do
        local entry=raw:gsub('\r$','')
        if entry~='' then
            local section=entry:match('^%[([^%]]+)%]$')
            if section=='Contribution' then
                assert(current==nil and next(header)==nil,'misplaced Contribution section')
                current=header
            elseif section and section:match('^Row%.') then
                local n=tonumber(section:match('^Row%.(%d+)$'))
                assert(n==#rows+1,'rows must be numbered in order')
                current={};rows[n]=current
            elseif section then
                local n=tonumber(section:match('^Page%.(%d+)$'))
                assert(n==#pages+1 and #rows==0,'pages must be numbered in order before rows')
                current={};pages[n]=current
            else
                local key,value=entry:match('^([%w]+)=(.*)$')
                assert(key and current,'invalid descriptor line')
                assert(current~=header or ({contract=1,id=1,generation=1})[key],'unknown header key '..key)
                assert(current==header or (rows[#rows]==current and ROW_KEYS[key])
                    or (rows[#rows]~=current and DESCRIPTOR_KEYS[key]),'unknown page key '..key)
                assert(current[key]==nil,'duplicate key '..key)
                current[key]=unescape(value)
            end
        end
    end
    local generation=tonumber(header.generation)
    assert(math.type(generation)=='integer' and generation>=1,'invalid generation')
    for _,page in ipairs(pages) do
        if page.menuFile then
            assert(page.manifestFile==nil and page.menuFile==M.menuName(generation,page.menuFile:match('%.(%d+)%.lua$') or 0),
                'invalid menu file')
            page.menu=M.deserialize(read(page.menuFile))
            page.menuFile=nil
        end
    end
    assert(tonumber(header.contract)==M.contractFor({pages=pages,rows=rows}),
        'unsupported contract '..tostring(header.contract))
    for _,page in ipairs(pages) do
        if page.visible~=nil then
            assert(page.visible=='0' or page.visible=='1','invalid visible')
            page.visible=page.visible=='1'
        end
        if page.manifestFile then
            assert(page.manifestFile==M.manifestName(generation,page.manifestFile:match('%.(%d+)%.ini$') or 0),
                'invalid manifest file')
            page.manifest=read(page.manifestFile)
            page.manifestFile=nil
        end
    end
    for _,row in ipairs(rows) do
        local settings={}
        for id in ((row.settings or '')..'|'):gmatch('([^|]*)|') do settings[#settings+1]=id end
        row.settings=settings
    end
    local contribution={pages=pages,rows=#rows>0 and rows or nil}
    check(header.id,contribution)
    return {id=header.id,generation=generation,pages=pages,rows=contribution.rows}
end

-- Parses a contributor variable: "<generation>\n<descriptor path>"; an empty path means withdrawn.
function M.slot(value)
    if type(value)~='string' then return nil end
    local generation,path=value:match('^(%d+)\n(.*)$')
    generation=tonumber(generation)
    if not generation then return nil end
    return generation,path
end

local function defaultRead(path)
    local file=io.open(path,'rb')
    if not file then return nil end
    local content=file:read('a')
    file:close()
    return content
end

local function defaultWrite(path,content)
    local file=assert(io.open(path,'wb'))
    local ok,err=file:write(content)
    file:close()
    assert(ok,err)
end

function M.publisher(shared,options)
    assert(shared and type(shared.GetSharedVariable)=='function'
        and type(shared.SetSharedVariable)=='function','shared variables unavailable')
    assert(type(options)=='table','publisher options required')
    local id,directory=options.id,options.directory
    line(id,128,'contributor id')
    line(directory,1024,'directory')
    assert(absolute(directory),'directory must be absolute')
    directory=directory:gsub('[/\\]+$','')
    local write,remove=options.write or defaultWrite,options.remove or os.remove
    local read=options.read or defaultRead
    local hexId=M.hex(id)
    local key=PREFIX..hexId
    local function join(name) return directory..'/'..name end
    -- Shared variables reset each game launch; the counter file keeps generations increasing
    -- so g-2 cleanup also reaches files left by an earlier launch.
    local counter=join('mcs_menu.generation')
    local last=math.tointeger(tonumber(read(counter) or '')) or 0
    if last<0 then last=0 end
    local function current() return (M.slot(shared:GetSharedVariable(key))) or 0 end
    local function cleanup(generation)
        if generation<1 then return end
        remove(join(M.descriptorName(generation)))
        -- Manifest and menu files share one numbering; the first gap ends it.
        local n=1
        while remove(join(M.manifestName(generation,n))) or remove(join(M.menuName(generation,n))) do n=n+1 end
    end
    local function register()
        local index=shared:GetSharedVariable(M.index)
        index=type(index)=='string' and index or ''
        for word in index:gmatch('%S+') do if word==hexId then return end end
        local updated=index=='' and hexId or index..' '..hexId
        shared:SetSharedVariable(M.index,updated)
        assert(shared:GetSharedVariable(M.index)==updated,'menu contribution index changed concurrently')
    end
    local self={}
    function self:publish(contribution)
        local generation=math.max(current(),last)+1
        local descriptor,files=M.encode(id,generation,contribution)
        for _,file in ipairs(files) do write(join(file.name),file.content) end
        local path=join(M.descriptorName(generation))
        write(path,descriptor)
        register()
        shared:SetSharedVariable(key,generation..'\n'..path)
        last=generation
        write(counter,tostring(generation))
        cleanup(generation-2)
        return generation
    end
    function self:withdraw()
        local generation=math.max(current(),last)
        if generation==0 then return end
        shared:SetSharedVariable(key,generation..'\n')
        cleanup(generation);cleanup(generation-1)
        last=generation
    end
    return self
end

return M
