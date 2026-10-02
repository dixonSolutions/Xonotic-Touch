# From the Terms of Service to the main menu, by touch, at 2340x1080 with the
# Standard preset. Run once the game shows the Terms (after the first download).
key 42                      # warm-up: Mutter adds each virtual device on its first event
tap 0.99 0.01
sleep 3
tap 0.39 0.781              # Terms of Service: Accept
sleep 2
tap 0.4568 0.6111           # Welcome: name from the character map, U ...
sleep 0.6
tap 0.4444 0.6111           # ... T
sleep 0.6
tap 0.471 0.709             # player statistics: No
sleep 1
tap 0.5 0.781               # Save settings
sleep 2
tap 0.344 0.443             # Xonotic Touch setup: Standard
sleep 1
tap 0.5 0.748               # Save & continue
sleep 2
shot
