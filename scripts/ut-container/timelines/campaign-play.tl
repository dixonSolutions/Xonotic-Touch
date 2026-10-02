# Main menu -> campaign level 1 (Boil, 5 bots) -> join -> move, turn, fire, hop.
key 42
tap 0.99 0.01
sleep 3
tap 0.378 0.45              # Singleplayer panel
sleep 2
tap 0.601 0.924             # Play campaign!
sleep 25
tap 0.39 0.711              # Join!
sleep 3
shot
fingerdown 0 0.169 0.680    # move stick, pushed forward
fingermove 0 0.169 0.62
fingermove 0 0.169 0.56
sleep 1
fingerdown 1 0.62 0.45      # look: drag on the right half while moving
fingermove 1 0.66 0.45
fingermove 1 0.70 0.45
fingermove 1 0.74 0.45
fingerup 1
fingerdown 2 0.855 0.678    # FIRE
sleep 0.12
fingerup 2
sleep 0.4
fingerdown 3 0.855 0.883    # HOP
sleep 0.1
fingerup 3
sleep 1
fingerup 0
shot
