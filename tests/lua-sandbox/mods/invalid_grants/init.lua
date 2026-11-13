local ok, err = pcall(function() return xt.cvar('fov') end)
if not ok and tostring(err):find('permission denied', 1, true) then
    print('PERMISSIONS PASS malformed grant file denied')
else
    print('PERMISSIONS FAIL malformed grant file allowed')
end
