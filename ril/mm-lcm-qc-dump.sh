#!/system/bin/sh

timestamp=$(date +'%Y-%m-%d_%H-%M-%S')
echo "Dumping Motorola Multi-Mode BP Logs @ $timestamp"
echo "I am running as $(id)"

# Setup base output directories
outDirRoot="/data/misc/ril"
outDirBranch="bp-dump/${timestamp}"
cd $outDirRoot
case $? in 0) ;; *) echo "Couldn\'t cd to $outDirRoot"; exit 1;; esac
# Set delimiter to slash for separating directories
OIFS=$IFS; IFS='/'
for dir in $outDirBranch; do
    mkdir "$dir" 2>/dev/null
    cd "$dir" 2>&1
    case $? in 0) ;; *) echo "Couldn\'t cd to $dir"; exit 1;; esac
    outDirBranchUnwind="$dir $outDirBranchUnwind"
done
IFS=$OIFS
outDir="${outDirRoot}/${outDirBranch}"

echo "===== LCM Section ====="
mkdir "${outDir}/lcm"
case $? in
0)
    lcm-dump.sh $timestamp $outDirRoot "${outDirBranch}/lcm"
    rmdir "${outDir}/lcm"
    ;;
*)
    echo "Couldn\'t create ${outDir}/lcm"
    ;;
esac

echo "===== QCOM BP Section ====="
mkdir "${outDir}/qcom"
case $? in
0)
    qbp-dump.sh $timestamp $outDirRoot "${outDirBranch}/qcom"
    rmdir "${outDir}/qcom"
    ;;
*)
    echo "Couldn\'t create ${outDir}/qcom"
    ;;
esac

# Cleanup output dirs
cd $outDir
case $? in 0) ;; *) echo "Couldn\'t cd to $outDir"; exit 1;; esac
for dir in $outDirBranchUnwind; do
    cd ..
    rmdir $dir
done
