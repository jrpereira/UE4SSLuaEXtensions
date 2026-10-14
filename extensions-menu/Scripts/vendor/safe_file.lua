-- Vendored from ModCoreSettings (owner). Do not edit copies; change the source and re-vendor.
-- Plain-io file reading, crash-safe replacement and folder creation for ModCore modules.
-- A replacement writes <path>.new, moves the current file to <path>.old, which is kept as a
-- backup, and then makes <path>.new the file. A crash leaves either the old or the new file
-- in place; the next read or write finishes or discards the interrupted replacement.
local M={LIMIT=1048576}

local function exists(path)
    local file=io.open(path,'rb')
    if not file then return false end
    file:close()
    return true
end

-- Finish a replacement interrupted after the current file was moved out, and discard a
-- replacement that was never published. A missing file with only a backup is left alone:
-- that file was removed on purpose.
function M.recover(path)
    local new,old=path..'.new',path..'.old'
    if not exists(new) then return end
    if not exists(path) and exists(old) then
        -- The current file is moved out only after the replacement is complete.
        assert(os.rename(new,path),'could not publish interrupted replacement: '..new)
        return
    end
    assert(os.remove(new),'could not clear unpublished replacement: '..new)
end

-- A file's contents, nil when it does not exist. Denied access, an I/O error or a file over
-- limit bytes (default M.LIMIT, 1 MiB) raises instead of looking like a missing file.
function M.read(path,limit)
    limit=limit or M.LIMIT
    M.recover(path)
    local file,err,code=io.open(path,'rb')
    if not file then
        if code==2 then return nil end
        error(err or ('cannot read '..path),0)
    end
    local text,why=file:read(limit+1)
    local closed,closeWhy=file:close()
    assert(closed,closeWhy or 'close failed')
    assert(text or not why,why)
    text=text or ''
    assert(#text<=limit,'file exceeds '..limit..' bytes: '..path)
    return text
end

-- Writes text to path in place, for callers that stage and rename under their own rules.
function M.put(path,text)
    local file=assert(io.open(path,'wb'))
    local ok,err=file:write(text)
    local closed,why=file:close()
    assert(ok and closed,err or why or 'write failed')
end

-- Replaces path with text crash-safely. Windows refuses to rename onto an existing file, so
-- the current file always moves to <path>.old before the new one takes its name.
function M.write(path,text)
    M.recover(path)
    local new,old=path..'.new',path..'.old'
    local staged,stageWhy=pcall(M.put,new,text)
    if not staged then os.remove(new);error(tostring(stageWhy),0) end
    local moved=false
    if exists(path) then
        if exists(old) then
            local removed,removeWhy=os.remove(old)
            if not removed then os.remove(new);error(tostring(removeWhy),0) end
        end
        local renamed,renameWhy=os.rename(path,old)
        if not renamed then os.remove(new);error(tostring(renameWhy),0) end
        moved=true
    end
    local published,publishWhy=os.rename(new,path)
    if not published then
        if moved then os.rename(old,path) end
        os.remove(new)
        error(tostring(publishWhy),0)
    end
    return true
end

-- Creates a folder when it is missing. A probe file shows an existing one without starting
-- a shell, which on Windows is a cmd.exe process on the game thread for every call.
function M.mkdir(path)
    local probe=path..'/.mc-probe'
    local file=io.open(probe,'wb')
    if file then file:close();os.remove(probe);return end
    local command
    if package.config:sub(1,1)=='\\' then
        -- Quoting alone does not prevent cmd.exe environment expansion.
        assert(not path:find('["%%!\r\n]'),'unsupported directory path')
        path=path:gsub('/','\\')
        command='if not exist "'..path..'" mkdir "'..path..'"'
    else
        command="mkdir -p -- '"..path:gsub("'","'\\''").."'"
    end
    local ok=os.execute(command)
    assert(ok==true or ok==0,'cannot create directory: '..path)
end

return M
