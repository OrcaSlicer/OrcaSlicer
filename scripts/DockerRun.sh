#!/bin/bash
set -x
# Just in case, here's some other things that might help:
#  Force the container's hostname to be the same as your workstation
#  -h $HOSTNAME \
#  If there's problems with the X display, try this
#  -v /tmp/.X11-unix:/tmp/.X11-unix \
#  If you get an error like "Authorization required, but no authorization protocol specified," run line 9 in your terminal before rerunning this program
#  xhost +local:docker
docker run \
  `# Use the hosts networking.  Printer wifi and also dbus communication` \
  --net=host \
  `# Some X installs will not have permissions to talk to sockets for shared memory` \
  --ipc host \
  `# Run as your workstations username to keep permissions the same` \
  -u "$USER" \
  `# Bind mount your home directory into the container for loading/saving files` \
  -v "$HOME:/home/$USER" \
  `# Pass the X display number to the container` \
  -e DISPLAY="$DISPLAY" \
  `# It seems that libGL and dbus things need privileged mode` \
  --privileged=true \
  `# Attach tty for running orca slicer with command line things` \
  -ti \
  `# Clean up after yourself` \
  --rm \
  `# Pass all parameters from this script to the orca slicer  ENTRYPOINT binary` \
  orcaslicer "$@"
