#!/bin/sh -e
# One-machine NDI SpeedHQ codec bench via a network namespace (needs sudo).
#
# Same-host NDI uses uncompressed shared memory, so the SpeedHQ codec cost
# never shows up in local runs. This creates netns "ndibench" joined to the
# host by a veth pair: the sender inside the namespace has a different IP,
# NDI treats it as remote and engages SpeedHQ + the real network stack.
# veth throughput far exceeds 10 GbE, so the codec (not the wire) is the
# bottleneck being measured.
#
#   sudo scripts/ndi-netns-bench.sh up        # create namespace + veth
#   scripts/ndi-netns-bench.sh sender 7680x4320   # run testgen in the ns (no sudo)
#   # ...on the host: receiver with discovery pointed at the namespace:
#   #   NDI_CONFIG_DIR=$(pwd)/scratch-ndi-host ./build/moo-headless ... --input MooNetBench
#   #   (the 'up' step writes that config: networks.ips = 10.99.77.2)
#   sudo scripts/ndi-netns-bench.sh down      # tear down
#
# Measure CPU with scripts/cpu_sample.py <pids> <seconds>.

NS=ndibench
HOST_IP=10.99.77.1
NS_IP=10.99.77.2
HOSTCFG=scratch-ndi-host

case "${1:-}" in
up)
    ip netns add $NS
    ip link add veth-nb0 type veth peer name veth-nb1
    ip link set veth-nb1 netns $NS
    ip addr add $HOST_IP/30 dev veth-nb0
    ip link set veth-nb0 up
    ip netns exec $NS ip addr add $NS_IP/30 dev veth-nb1
    ip netns exec $NS ip link set veth-nb1 up
    ip netns exec $NS ip link set lo up
    mkdir -p $HOSTCFG
    printf '{ "ndi": { "networks": { "ips": "%s" } } }\n' $NS_IP \
        > $HOSTCFG/ndi-config.v1.json
    chmod 666 $HOSTCFG/ndi-config.v1.json 2>/dev/null || true
    echo "namespace up; host receivers: NDI_CONFIG_DIR=\$(pwd)/$HOSTCFG"
    ;;
sender)
    SIZE=${2:-7680x4320}
    # sudo only for entering the namespace; testgen itself runs as you if
    # invoked via: sudo scripts/ndi-netns-bench.sh sender ...
    exec ip netns exec $NS ./build/moo-testgen --name MooNetBench \
        --size "$SIZE" --precompute 24
    ;;
down)
    ip netns del $NS 2>/dev/null || true
    ip link del veth-nb0 2>/dev/null || true
    echo "namespace removed"
    ;;
*)
    echo "usage: $0 up|sender [WxH]|down (up/down need sudo)" >&2
    exit 2
    ;;
esac
