xt.command('probe', function()
    local ok, err = pcall(xt.match.setcvar, 'g_balance_health_start', '100')
    if ok then print('SERVER PERMISSIONS allowed')
    elseif tostring(err):find('permission denied', 1, true) then print('SERVER PERMISSIONS denied')
    else print('SERVER PERMISSIONS FAIL ' .. tostring(err)) end
end)
