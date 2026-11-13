-- touch_weapon: a touch button and a key bind.
--
-- xt.touch.button places a button like a HUD panel (clear of the stock
-- controls, movable), calls press when a finger lands on it, hold every frame
-- while it stays, release when it lifts. xt.bind takes a key from its binding
-- while the mod runs (keys that open the console or the menu stay theirs).

local held = 0
xt.touch.button{
  id = "next", label = "NEXT", w = 84, h = 84, side = "right",
  press = function()
    held = 0
    xt.exec("weapnext")
    print("touch_weapon: button pressed")
  end,
  hold = function()
    held = held + 1
    if held == 30 then -- about half a second
      xt.exec("weapbest")
      print("touch_weapon: long press, best weapon")
    end
  end,
  release = function()
    print("touch_weapon: button released")
  end,
}

local function bindkey()
  xt.bind(xt.setting("key"), function(key)
    xt.exec("weapnext")
    print("touch_weapon: key " .. key)
  end)
end
bindkey()
