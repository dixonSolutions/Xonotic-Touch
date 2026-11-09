# This library is BROKEN.

Do not use it for any purpose whatsoever anymore.

It cannot be fixed without breaking Xonotic.

A new version may be made, although I recommend switching to DTLS instead.

Also, it does not make sense to keep building on RSA-1024, and even just
bumping that is not feasible as it will break DarkPlaces' single-threaded
networking code (as in, every user connecting will lag the game).
