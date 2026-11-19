#!/system/bin/sh
# Counts SurfaceFlinger crashes while Android draws with the Adreno GPU
# (/sbin/init resets the count on every start). After the third one it leaves
# /data/ds-gpu-failed and restarts Android; /sbin/init then chooses CPU drawing
# until that file is deleted or gpu.mode=on is set.
count_file=/data/ds-gpu-crashes
count=$(cat "$count_file" 2>/dev/null)
count=$(( ${count:-0} + 1 ))
echo "$count" > "$count_file"
log -p w -t ds-gpu-guard "SurfaceFlinger crashed with the GPU driver ($count of 3)"
if [ "$count" -ge 3 ]; then
    echo "SurfaceFlinger crashed $count times in a row with the GPU driver ($(date))" > /data/ds-gpu-failed
    rm -f "$count_file"
    log -p e -t ds-gpu-guard "giving up on the GPU; Android restarts and draws with the CPU"
    setprop sys.powerctl reboot,ds-gpu-failed
fi
