#!/system/bin/sh

###### KNOWN ISSUES ######
# - Must run as root.  Fails when called from dumpstate due to being radio
#   user.  It creates LOTS of adb zombies.

###### FUNCTIONS ######

setupDirs() {
    case $parentExists in
    1)
        outDirRoot=$1
        outDirBranch=$2
        ;;
    0)
        outDirRoot="/data/misc/ril"
        outDirBranch="bp-dump/${timestamp}/lcm"
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
        ;;
    esac
    outDir="${outDirRoot}/${outDirBranch}"
}

processLogs() {
    # Initialize communication with the BP.  Don't do adb kill-server, because
    # it might interfere with other processes that use ADB to talk to the BP.
    ADBHOST=192.168.20.2
    adb devices

    # The host-side iptables are important for logging functionality, and
    # they are not currently part of the AP bugreport.
    echo "file:begin:txt:${outDirBranch}/host-iptables.txt"
    iptables -L
    iptables -t nat -L
    echo "file:end:txt:${outDirBranch}/host-iptables.txt"

    # The host-side IPv6 routes are important, and they are not currently
    # part of the AP bugreport.
    echo "file:begin:txt:${outDirBranch}/host-ipv6_route.txt"
    cat /proc/net/ipv6_route
    echo "file:end:txt:${outDirBranch}/host-ipv6_route.txt"

    echo "file:begin:txt:${outDirBranch}/logcat-main.txt"
    adb -e logcat -d -v threadtime -b main
    echo "file:end:txt:${outDirBranch}/logcat-main.txt"

    echo "file:begin:txt:${outDirBranch}/logcat-radio.txt"
    adb -e logcat -d -v threadtime -b radio
    echo "file:end:txt:${outDirBranch}/logcat-radio.txt"

    echo "file:begin:txt:${outDirBranch}/dmesg.txt"
    adb -e shell dmesg
    echo "file:end:txt:${outDirBranch}/dmesg.txt"

    echo "file:begin:txt:${outDirBranch}/busybox-ifconfig.txt"
    adb -e shell busybox ifconfig
    echo "file:end:txt:${outDirBranch}/busybox-ifconfig.txt"

    echo "file:begin:txt:${outDirBranch}/busybox-route.txt"
    adb -e shell busybox route
    echo "file:end:txt:${outDirBranch}/busybox-route.txt"

    echo "file:begin:txt:${outDirBranch}/df.txt"
    adb -e shell df
    echo "file:end:txt:${outDirBranch}/df.txt"

    echo "file:begin:txt:${outDirBranch}/ps.txt"
    adb -e shell ps
    echo "file:end:txt:${outDirBranch}/ps.txt"

    txtFileList="\
        /data/panic_report.txt\
        /dev/mtd/mtd6ro\
        /proc/lte_dd\
        /proc/net/route\
        /proc/net/ipv6_route\
        "

    # 1024 chars is the largest command adb can currently accept, so be careful
    # not to exceed it.
    binFileList="$(adb -e shell '\
        files="$files $(busybox find\
            /logdata/app_dump\
            /logdata/tombstones\
            /logdata/panic_data\
            /logdata/dontpanic\
            /data/panic\
            /data/logger\
            /data/gki\
            /data/moid\
            /data/opprof\
            /data/comm_drv\
            /data/ratc\
            /data/scim\
            /pds/comm_drv\
            /pds/scim\
            -type f 2>/dev/null)";\
        echo -n $files')" # Use -n to avoid carriage return nastiness

    echo "file:begin:txt:${outDirBranch}/file-list.txt"
    for filePath in $txtFileList $binFileList; do
        echo "$filePath"
    done
    echo "file:end:txt:${outDirBranch}/file-list.txt"

    for txtFilePath in $txtFileList; do
        echo "file:begin:txt:${outDirBranch}${txtFilePath}"
        adb -e shell cat "${txtFilePath}" 2>&1 # this is failing
        echo ""
        echo "file:end:txt:${outDirBranch}${txtFilePath}"
    done

    for binFilePath in $binFileList; do
        binFileDir=${binFilePath%/*}
        binFileName=${binFilePath##*/}
        adb -e pull $binFilePath $outDir
        case $? in 0) ;; *) continue;; esac
        base64 -e "${outDir}/${binFileName}" "${outDir}/${binFileName}-base64" 2>&1
        case $? in
        0)
            ;;
        *)
            echo "base64 failed for ${outDir}/${binFileName}"
            rm "${outDir}/${binFileName}"
            continue
            ;;
        esac
        echo "file:begin:bin:${outDirBranch}${binFilePath}"
        # Do NOT redirect stderr to stdout for bin files.  It may confuse the base64 decoder.
        cat "${outDir}/${binFileName}-base64"
        echo ""
        echo "file:end:bin:${outDirBranch}${binFilePath}"
        rm "${outDir}/${binFileName}" "${outDir}/${binFileName}-base64" 2>&1
    done
}

cleanupDirs() {
    case $parentExists in
    0)
        # Cleanup output dirs
        cd $outDir
        case $? in 0) ;; *) echo "Couldn\'t cd to $outDir"; exit 1;; esac
        for dir in $outDirBranchUnwind; do
            cd ..
            rmdir $dir
        done
        ;;
    esac
}

###### MAIN ROUTINE ######

# Generate a timestamp if none passed in from parent script
case $1 in
    "") timestamp=$(date +'%Y-%m-%d_%H-%M-%S'); parentExists=0;;
    *) timestamp=$1; parentExists=1;;
esac

echo "Dumping LCM logs @ $timestamp"
echo "I am running as $(id)"
case $(id) in
    *root*) ;;
    *) echo "Aborting due to lack of root"; exit 1;;
esac
setupDirs $2 $3
processLogs
cleanupDirs
