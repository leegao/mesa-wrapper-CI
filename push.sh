push() {
	adb push $1 /data/local/tmp/
	adb shell run-as com.ludashi.benchmark "cp /data/local/tmp/$1 files/imagefs/usr/lib/$1"
	adb shell run-as com.ludashi.benchmark "ls -lh files/imagefs/usr/lib/$1"
}

push_vvl() {
	adb push $1 /data/local/tmp/
	adb shell run-as com.ludashi.benchmark "cp /data/local/tmp/$1 files/imagefs/usr/share/vulkan/explicit_layer.d/$1"
	adb shell run-as com.ludashi.benchmark "ls -lh files/imagefs/usr/share/vulkan/explicit_layer.d/$1"
}

sudo docker run --rm -v "$(pwd):/workspace" wrapper-compiler

cd build/src/vulkan/wrapper/
push libvulkan_wrapper.so
cd -

cd build/subprojects/libadrenotools/src/hook/
push libhook_impl.so
cd -
