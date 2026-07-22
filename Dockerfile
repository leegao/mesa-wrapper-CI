FROM ghcr.io/termux/package-builder:latest

USER root

RUN apt-get update && \
    apt-get install -y --no-install-recommends ninja-build && \
    pip3 install --break-system-packages --ignore-installed --no-cache-dir meson ninja mako pyyaml packaging

RUN mkdir -p /tmp/sysroot && \
    cd /tmp/sysroot && \
    TERMUX_REPO="https://packages-cf.termux.dev/apt/termux-main" && \
    curl -s "${TERMUX_REPO}/dists/stable/main/binary-aarch64/Packages" > Packages && \
    PACKAGES="libdrm libandroid-shmem libxcb libx11 libxshmfence libxext libxrandr libxrender xorgproto libxau libxdmcp" && \
    for pkg in $PACKAGES; do \
        pkg_path=$(awk -v p="Package: $pkg" '$0==p{flag=1} flag && /^Filename:/{print $2; exit}' Packages) && \
        curl -L -O "${TERMUX_REPO}/${pkg_path}"; \
    done && \
    mkdir -p /data/data/com.termux/files/usr && \
    for f in *.deb; do dpkg-deb -x "$f" /; done && \
    rm -rf /tmp/sysroot

RUN NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1) && \
    NDK_BIN="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin" && \
    mkdir -p /root/build-config && \
    cat << EOF > /root/build-config/cross_file.txt
[binaries]
c = '${NDK_BIN}/aarch64-linux-android30-clang'
cpp = '${NDK_BIN}/aarch64-linux-android30-clang++'
ar = '${NDK_BIN}/llvm-ar'
strip = '${NDK_BIN}/llvm-strip'
pkg-config = 'pkg-config'

[constants]
termux_dir = '/data/data/com.termux/files/usr'

[properties]
pkg_config_libdir = termux_dir + '/lib/pkgconfig:' + termux_dir + '/share/pkgconfig'

[built-in options]
c_args = ['-D__TERMUX__', '-D__USE_GNU', '-U__ANDROID__', '-I' + termux_dir + '/include', '-include', 'fcntl.h', '-include', 'unistd.h']
cpp_args = ['-D__TERMUX__', '-D__USE_GNU', '-U__ANDROID__', '-I' + termux_dir + '/include', '-include', 'fcntl.h', '-include', 'unistd.h']
c_link_args = ['-L' + termux_dir + '/lib', '-landroid-shmem']
cpp_link_args = ['-L' + termux_dir + '/lib', '-landroid-shmem']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF

RUN cat << 'EOF' > /root/build.sh
#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

if [ ! -d "${BUILD_DIR}" ]; then
  meson setup "${BUILD_DIR}" --cross-file /root/build-config/cross_file.txt \
      -Dcpp_rtti=false \
      -Dgbm=disabled \
      -Dopengl=false \
      -Dllvm=disabled \
      -Dshared-llvm=disabled \
      -Dplatforms=x11 \
      -Dgallium-drivers= \
      -Dxmlconfig=disabled \
      -Dvulkan-drivers=wrapper
fi

ninja -C "${BUILD_DIR}" src/vulkan/wrapper/libvulkan_wrapper.so

cp "${BUILD_DIR}/src/vulkan/wrapper/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
STRIP="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
$STRIP --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

echo "Build successful:"
echo " - libvulkan_wrapper.so"
echo " - libvulkan_wrapper.so.unstripped"
EOF
RUN chmod +x /root/build.sh

WORKDIR /workspace
ENTRYPOINT ["/root/build.sh"]
