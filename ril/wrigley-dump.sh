#!/system/bin/sh

echo "Dumping LCM logs"
echo "I am running as $(id)"
hostName=$(getprop ro.product.device)

# The host-side iptables are important for logging functionality, and
# they are not currently part of the AP bugreport.
echo "file:begin:txt:${hostName}-iptables.txt"
iptables -L
iptables -t nat -L
echo "file:end:txt:${hostName}-iptables.txt"

# The host-side IPv6 routes are important, and they are not currently
# part of the AP bugreport.
echo "file:begin:txt:/${hostName}-ipv6_route.txt"
cat /proc/net/ipv6_route
echo "file:end:txt:/${hostName}-ipv6_route.txt"

# Get the rest of the info from dumpd
for cmd in "state" "logs" "files" "panic" "atvc"; do
    echo "-o wrigley $cmd" | nc 192.168.20.2 3002
done
