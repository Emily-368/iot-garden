#!/usr/bin/env bash
# Pull the latest code from GitHub and apply whatever changed.
#
#   ./update.sh         update only what changed since the last pull
#   ./update.sh --all   reinstall everything and reflash the Arduino
#
# - Pi code changed      -> restart the bridge service
# - Service file changed -> reinstall it into systemd
# - Arduino sketch changed -> compile and flash it over USB from the Pi
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH_DIR="$REPO_DIR/arduino/iot_garden"
FQBN="arduino:avr:uno"
SERVICE="garden-bridge"
ENV_FILE="/etc/default/garden-bridge"
export PATH="$HOME/.local/bin:$PATH"   # where arduino-cli is installed

cd "$REPO_DIR"

FORCE=0
[[ "${1:-}" == "--all" ]] && FORCE=1

# Use the same serial port as the bridge
PORT="/dev/ttyACM0"
if [[ -r "$ENV_FILE" ]]; then
  PORT="$(sed -n 's/^GARDEN_SERIAL_PORT=//p' "$ENV_FILE" | tail -n1)"
  PORT="${PORT:-/dev/ttyACM0}"
fi

OLD="$(git rev-parse HEAD)"
echo "Pulling from GitHub..."
git pull --ff-only
NEW="$(git rev-parse HEAD)"

if [[ $FORCE == 0 && "$OLD" == "$NEW" ]]; then
  echo "Already up to date."
  exit 0
fi

# True if any of the given paths changed in this pull (or --all was used)
changed() { [[ $FORCE == 1 ]] || ! git diff --quiet "$OLD" "$NEW" -- "$@"; }

# Python environment
if [[ ! -x venv/bin/python ]]; then
  echo "Creating Python environment..."
  python3 -m venv venv
  FORCE_REQS=1
fi
if [[ ${FORCE_REQS:-0} == 1 ]] || changed pi/requirements.txt; then
  echo "Installing Python packages..."
  ./venv/bin/pip install -q -r pi/requirements.txt
fi

# systemd service
if changed pi/garden-bridge.service; then
  echo "Installing $SERVICE service..."
  sed -e "s|CHANGE_ME_USER|$USER|g" -e "s|CHANGE_ME_DIR|$REPO_DIR|g" \
    pi/garden-bridge.service | sudo tee "/etc/systemd/system/$SERVICE.service" > /dev/null
  sudo systemctl daemon-reload
  sudo systemctl enable "$SERVICE"
fi

# Arduino sketch (the bridge must release the serial port while flashing)
if changed arduino/; then
  echo "Flashing Arduino on $PORT..."
  sudo systemctl stop "$SERVICE" || true
  if ! arduino-cli compile --upload --fqbn "$FQBN" -p "$PORT" "$SKETCH_DIR"; then
    echo "Arduino upload failed. Restarting the bridge with the old sketch still running."
    sudo systemctl start "$SERVICE"
    exit 1
  fi
fi

# Restart the bridge if anything it depends on changed
if changed pi/ arduino/; then
  echo "Restarting $SERVICE..."
  sudo systemctl restart "$SERVICE"
fi

echo "Done. Now running: $(git log -1 --format='%h %s')"
