xt.command('probe', function()
    local ok, err = pcall(function() xt.broadcast('SERVER PERMISSIONS MESSAGE') end)
    if ok then print('SERVER PERMISSIONS allowed')
    elseif tostring(err):find('permission denied', 1, true) then print('SERVER PERMISSIONS denied')
    else print('SERVER PERMISSIONS FAIL ' .. tostring(err)) end
end)
