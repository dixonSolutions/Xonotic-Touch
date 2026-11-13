local function denied(label, fn)
    local ok, err = pcall(fn)
    if not ok and tostring(err):find('permission denied', 1, true) then
        print('PERMISSIONS PASS denied ' .. label)
    else
        print('PERMISSIONS FAIL denied ' .. label .. ': ' .. tostring(err))
    end
end

denied('exec', function() xt.exec('echo unexpectedly allowed') end)
denied('storage', function() xt.save('test', 'x') end)
denied('cvar_write', function() xt.setcvar('fov', '90') end)
local frames = 0
xt.on('frame', function() frames = frames + 1 end)
xt.command('probe', function()
    local ok, err = pcall(function() xt.exec('echo PERMISSIONS ALLOWED') end)
    print('PERMISSIONS exec ' .. (ok and 'allowed' or tostring(err)))
    local unsafe = pcall(function() xt.exec('quit') end)
    print('PERMISSIONS allowlist ' .. (unsafe and 'FAIL' or 'PASS'))
    print('PERMISSIONS frames ' .. frames)
end)
