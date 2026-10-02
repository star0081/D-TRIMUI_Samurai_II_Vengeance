#!/bin/sh
# Samurai II — TrimUI Smart Pro stock and Knulli (POSIX sh).
# Knulli: roms/ports/Samurai II.sh + roms/ports/samurai2/
# TrimUI stock 1.1.1: Roms/PORTS/Samurai II.sh + Data/ports/samurai2/
# Do not change framebuffer.
# 32-bit Unity 4.6.3p2 Mono GLES2. A 64-bit presenter owns the PowerVR window.

if [ -f /mnt/SDCARD/System/etc/ex_config ]; then
  . /mnt/SDCARD/System/etc/ex_config
fi

export PATH="/mnt/SDCARD/System/bin:/usr/bin:/usr/sbin:/bin:/sbin:${PATH:-}"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)

if [ -f "$HERE/samurai2/samurai2_runtime" ]; then
  GAMEDIR="$HERE/samurai2"
elif [ -f /mnt/SDCARD/Data/ports/samurai2/samurai2_runtime ]; then
  GAMEDIR="/mnt/SDCARD/Data/ports/samurai2"
else
  GAMEDIR="$HERE/samurai2"
fi

SYS="$GAMEDIR/armhf"
LD="$SYS/lib/ld-linux-armhf.so.3"
LIB="$GAMEDIR/glbridge:$GAMEDIR/host-libs:$SYS/lib/arm-linux-gnueabihf:$SYS/lib"
LOGDIR="$GAMEDIR/logs"
LOG="$LOGDIR/samurai2.log"
PRES=0
PKG="com.madfingergames.SamuraiIIAll"
APK="$GAMEDIR/gamedata/samurai2-1.1.4.apk"
LIBS="$GAMEDIR/gamefiles/android-libs"

if [ -d /mnt/SDCARD/Apps/PortMaster/PortMaster ]; then
  PM="/mnt/SDCARD/Apps/PortMaster/PortMaster"
elif [ -n "${controlfolder:-}" ]; then
  PM="$controlfolder"
else
  PM=""
fi

GCDB=""
if [ -n "$PM" ] && [ -f "$PM/gamecontrollerdb.txt" ]; then
  GCDB="$PM/gamecontrollerdb.txt"
else
  for f in \
    /userdata/system/.local/share/PortMaster/gamecontrollerdb.txt \
    /usr/share/sdl/gamecontrollerdb.txt \
    /usr/share/gamecontrollerdb.txt
  do
    if [ -f "$f" ]; then
      GCDB="$f"
      break
    fi
  done
fi

if [ -d /usr/trimui/lib ]; then
  PRES_LIBS="/usr/trimui/lib:/usr/lib"
else
  PRES_LIBS="/usr/lib:/lib"
fi

mkdir -p "$LOGDIR" "$GAMEDIR/files" "$GAMEDIR/cache" \
  "$GAMEDIR/Android/data/$PKG" /tmp || exit 1

if [ -f "$LOG" ]; then
  mv -f "$LOG" "$LOG.1" 2>/dev/null || true
fi

echo "===== samurai2 start =====" > "$LOG"
date >> "$LOG" 2>/dev/null
echo "gamedir=$GAMEDIR" >> "$LOG"
echo "uname=$(uname -a)" >> "$LOG"
echo "id=$(id)" >> "$LOG"

cd "$GAMEDIR" || {
  echo "cannot cd $GAMEDIR" >> "$LOG"
  exit 1
}

if [ ! -f "$APK" ] || [ ! -f "$LIBS/libunity.so" ] || [ ! -f "$LIBS/libmono.so" ]; then
  echo "missing Samurai II 1.1.4 data" >> "$LOG"
  echo "need: $APK" >> "$LOG"
  echo "need: $LIBS/libunity.so and libmono.so (run setup.ps1 on a PC)" >> "$LOG"
  echo "===== samurai2 end =====" >> "$LOG"
  sync
  exit 1
fi

rm -f /tmp/samurai2.present.ready /tmp/s2-glbridge.sock /tmp/samurai2.frame
chmod a+x "$GAMEDIR/samurai2_present" "$GAMEDIR/samurai2_runtime" "$LD" 2>/dev/null || true
chmod a+rw /dev/dri/card0 /dev/dri/renderD128 /dev/fb0 2>/dev/null || true

echo performance >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo 1800000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null
echo 1800000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null

if [ ! -f "$GAMEDIR/samurai2_present" ] || [ ! -f "$GAMEDIR/glbridge/libGLESv2.so.2" ]; then
  echo "missing GLES2 glbridge" >> "$LOG"
  echo "===== samurai2 end =====" >> "$LOG"
  sync
  exit 1
fi

if [ ! -f "$LD" ] || [ ! -f "$GAMEDIR/samurai2_runtime" ]; then
  echo "missing armhf ld.so or samurai2_runtime" >> "$LOG"
  echo "===== samurai2 end =====" >> "$LOG"
  sync
  exit 1
fi

echo "----- s2-glbridge server -----" >> "$LOG"
(
  unset LD_PRELOAD
  unset SDL_VIDEODRIVER
  unset LIBGL_ALWAYS_SOFTWARE
  unset GALLIUM_DRIVER
  unset MESA_LOADER_DRIVER_OVERRIDE
  unset LIBGL_DRIVERS_PATH
  unset __EGL_VENDOR_LIBRARY_FILENAMES
  unset SDL_VIDEO_EGL_DRIVER
  export LD_LIBRARY_PATH="$PRES_LIBS"
  export SDL_VIDEO_GL_DRIVER=libGLESv2.so
  export SDL_OPENGL_ES_DRIVER=1
  if [ -n "$GCDB" ]; then
    export SDL_GAMECONTROLLERCONFIG_FILE="$GCDB"
  fi
  export XDG_RUNTIME_DIR=/tmp
  export TMPDIR=/tmp
  export TSPGL_WIDTH="${TSPGL_WIDTH:-1280}"
  export TSPGL_HEIGHT="${TSPGL_HEIGHT:-720}"
  export TSPGL_PRESENT="${TSPGL_PRESENT:-stretch}"
  if command -v setsid >/dev/null 2>&1; then
    exec setsid "$GAMEDIR/samurai2_present"
  else
    exec "$GAMEDIR/samurai2_present"
  fi
) >> "$LOG" 2>&1 &
PRES=$!
n=0
while [ "$n" -lt 15 ]; do
  if [ -f /tmp/samurai2.present.ready ]; then
    break
  fi
  n=$((n + 1))
  sleep 1
done
echo "present_ready=$n pid=$PRES" >> "$LOG"

export PORT_32BIT=Y
export XDG_RUNTIME_DIR=/tmp
export TMPDIR=/tmp
export SDL_VIDEODRIVER=offscreen
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export SDL_VIDEO_GL_DRIVER=libGLESv2.so.2
export SDL_VIDEO_EGL_DRIVER=libEGL.so.1
export SDL_OPENGL_ES_DRIVER=1
if [ -n "$GCDB" ]; then
  export SDL_GAMECONTROLLERCONFIG_FILE="$GCDB"
fi
export SDL_NO_SIGNAL_HANDLERS=1
export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
export MALLOC_ARENA_MAX=2
export NFSMW_WIDTH="${TSPGL_WIDTH:-1280}"
export NFSMW_HEIGHT="${TSPGL_HEIGHT:-720}"
export TSPGL_PRESENT="${TSPGL_PRESENT:-stretch}"
export SG_ROOT="$GAMEDIR"
export SG_PACKAGE="$PKG"
export SG_APK="$APK"
export SG_VERSION_CODE="101040"
export SG_VERSION_NAME="1.1.4"
export SG_FACE_LAYOUT="${SG_FACE_LAYOUT:-nintendo}"
# Knulli holds the sound card in PipeWire, so plughw comes back "device busy".
# 32-bit libpulse in host-libs talks to pipewire-pulse. TrimUI stays on ALSA.
if [ ! -d /usr/trimui/lib ]; then
  PULSE_SOCK=""
  for s in /run/user/0/pulse/native /var/run/pulse/native /run/pulse/native; do
    if [ -S "$s" ]; then
      PULSE_SOCK=$s
      break
    fi
  done
  if [ -z "$PULSE_SOCK" ]; then
    for d in /run/user/*; do
      if [ -S "$d/pulse/native" ]; then
        PULSE_SOCK=$d/pulse/native
        break
      fi
    done
  fi
  if [ -n "$PULSE_SOCK" ]; then
    export SDL_AUDIODRIVER=pulse
    export PULSE_SERVER="unix:$PULSE_SOCK"
    export PULSE_LATENCY_MSEC="${PULSE_LATENCY_MSEC:-80}"
    unset AUDIODEV
    for c in /var/run/pulse/.config/pulse/cookie /root/.config/pulse/cookie; do
      if [ -f "$c" ]; then
        export PULSE_COOKIE="$c"
        break
      fi
    done
  else
    export AUDIODEV="${AUDIODEV:-plughw:0,0}"
  fi
fi
unset SG_OBB
unset LIBGL_ALWAYS_SOFTWARE
unset GALLIUM_DRIVER
unset MESA_LOADER_DRIVER_OVERRIDE
unset LIBGL_DRIVERS_PATH
unset __EGL_VENDOR_LIBRARY_FILENAMES
unset EGL_PLATFORM
unset LD_PRELOAD

echo "ld=$LD" >> "$LOG"
echo "lib=$LIB" >> "$LOG"
echo "audio=${SDL_AUDIODRIVER:-?} dev=${AUDIODEV:-default} pulse=${PULSE_SERVER:-}" >> "$LOG"
echo "----- samurai2_runtime -----" >> "$LOG"
"$LD" --library-path "$LIB" "$GAMEDIR/samurai2_runtime" "$LIBS" >> "$LOG" 2>&1
RC=$?

if [ "$PRES" -ne 0 ]; then
  kill "$PRES" 2>/dev/null || true
  wait "$PRES" 2>/dev/null || true
fi
rm -f /tmp/samurai2.present.ready /tmp/s2-glbridge.sock /tmp/samurai2.frame
echo "exit_code=$RC" >> "$LOG"
echo "===== samurai2 end =====" >> "$LOG"
sync
exit $RC
