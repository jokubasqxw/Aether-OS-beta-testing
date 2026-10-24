#!/bin/bash
# Notification ding for finished updates
pw-play /usr/share/sounds/freedesktop/stereo/bell.oga 2>/dev/null || \
paplay /usr/share/sounds/freedesktop/stereo/bell.oga 2>/dev/null || \
canberra-gtk-play -i bell 2>/dev/null || \
echo -e "\a"
