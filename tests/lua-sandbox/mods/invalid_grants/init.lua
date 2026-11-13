-- a malformed grant file: the standard tier still works, nothing more is granted
local ok = pcall(function() return xt.cvar('fov') end)
print('PERMISSIONS ' .. (ok and 'PASS' or 'FAIL') .. ' malformed grant file: standard tier only')
