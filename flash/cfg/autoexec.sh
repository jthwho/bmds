# /cfg/autoexec.sh: run once by the first shell at startup.

# Home: the card's when there is one, else /flash/home (also when a home saved with
# sysconf save is somewhere that isn't there). Neither is saved.
if waitmount /sd0/p0 3000; then
        mkdir -p /sd0/p0/home
        sysconf set home /sd0/p0/home
else
        mkdir -p /flash/home
        sysconf set home /flash/home
fi

# The card's own startup if it has one; else a desktop from what is built in.
if [ -f /sd0/p0/autoexec.sh ]; then
        . /sd0/p0/autoexec.sh
else
        display start
        win start
        panel &
fi

# A new card (nothing installed in pkg/root): the welcome window offers the standard set.
# It leaves at once otherwise, or with pkg/welcome off.
welcome --first-run &

# Your own startup, in your home (the card's, or /flash/home).
[ -f /home/autoexec.sh ] && . /home/autoexec.sh
