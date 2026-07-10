pull() {
        adb shell run-as com.ludashi.benchmark "base64 files/imagefs/usr/lib/$1" | base64 -d > "shims/$1"
}

mkdir -p shims

pull libandroid-shmem.so
pull libzstd.so
pull libxcb.so
pull libX11-xcb.so
pull libxcb-dri3.so
pull libxcb-present.so
pull libxcb-sync.so
pull libxcb-randr.so
pull libxcb-shm.so
pull libdrm.so
pull libxshmfence.so
pull libxcb-xfixes.so
pull libXrandr.so
