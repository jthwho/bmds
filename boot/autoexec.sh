# /boot/autoexec.sh: run by the first shell at startup.
# Run the SD card's autoexec.sh too, if there is a card.
if waitmount /sd0/p0 3000; then
        [ -f /sd0/p0/autoexec.sh ] && . /sd0/p0/autoexec.sh
fi
