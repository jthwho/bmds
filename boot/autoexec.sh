# /boot/autoexec.sh: run once by the first shell at startup.

# Home: the card's when there is one, else /boot/home (also when a home saved with
# sysconf save is somewhere that isn't there). Neither is saved.
if waitmount /sd0/p0 3000; then
        mkdir -p /sd0/p0/home
        sysconf set home /sd0/p0/home
else
        mkdir -p /boot/home
        sysconf set home /boot/home
fi

# The card's own startup if it has one; else a desktop from what is built in.
if [ -f /sd0/p0/autoexec.sh ]; then
        . /sd0/p0/autoexec.sh
else
        display start 720 double
        win start
        panel &
fi

# Your own startup, in your home (the card's, or /boot/home).
[ -f /home/autoexec.sh ] && . /home/autoexec.sh
