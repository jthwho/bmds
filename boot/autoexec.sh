# /boot/autoexec.sh: run by the system at startup; it starts things.

# First try for the SD card. If it is there, the system uses it.
if waitmount /sd0/p0 3000; then
        # With a card at startup, its home is /home (until reboot: not saved).
        mkdir -p /sd0/p0/home
        sysconf set home /sd0/p0/home

        # Then the card's autoexec.sh starts the rest.
        [ -f /sd0/p0/autoexec.sh ] && . /sd0/p0/autoexec.sh
else
        # No card: the bare minimum.
        display start 720 double
        win start
        terminal &
fi
