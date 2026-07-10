export ANDROID_NDK_LATEST_HOME=~/Android/Sdk/ndk/27.0.12077973
export GITHUB_WORKSPACE=.

# $ANDROID_NDK_LATEST_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang \
#   -shared -fPIC drm_shim.c -o dummy_lib/libdrm.so
$ANDROID_NDK_LATEST_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar cr shims/librt.a

export PKG_CONFIG_LIBDIR=$GITHUB_WORKSPACE/shims

cat <<EOF > shims/xrandr.pc
Name: xrandr
Description: xrandr
Version: 1.15
Libs: -lXrandr
Cflags:
EOF

cat <<EOF > shims/libdrm.pc
Name: libdrm
Description: libdrm
Version: 2.5
Libs: -ldrm
Cflags:
EOF

cat <<EOF > shims/libx11-xcb.pc
Name: libx11-xcb
Description: libx11-xcb
Version: 2.5
Libs: -lX11-xcb
Cflags:
EOF

for lib in xcb xcb-randr xcb-dri3 xcb-present xcb-shm xcb-sync xshmfence xcb-xfixes;
do
cat <<EOF > shims/$lib.pc
Name: $lib
Description: $lib
Version: 1.15
Libs: -l$lib
Cflags:
EOF
done

meson setup build --reconfigure \
    --cross-file android.toml \
    -Dbuildtype=debugoptimized \
    -Dplatforms=android,x11 \
    -Dandroid-stub=true \
    -Dandroid-libbacktrace=disabled \
    -Dplatform-sdk-version=30 \
    -Dglx=disabled \
    -Dgbm=disabled \
    -Degl=disabled \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dglvnd=disabled \
    -Dllvm=disabled \
    -Dvalgrind=disabled \
    -Dgallium-drivers= \
    -Dshared-glapi=disabled \
    -Dzstd=disabled \
    -Dvulkan-drivers=wrapper
