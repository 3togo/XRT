files=("$(find . -maxdepth 2 -type d -name ERT-BSP)")
[ ${#files[@]} -eq "0" ] && echo "cannot find directory ERT-BSP" && exit 1
mdir="$(realpath ${files[0]})"
cmd="ln -sf \"$mdir\" src/runtime_src/ert/scheduler"
echo $cmd
eval $cmd
mdir="src/runtime_src/ert/scheduler/ERT-BSP"
[ ! -d "$mdir" ] && echo "cannot file $mdir" && exit 1
export XILINX_VITIS=/opt/Xilinx/Vitis/2021.2
cd build
reset
./build.sh
[ $? -ne "" ] && echo "build error" && exit $?
cd Release
make package
