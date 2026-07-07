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
bench)
    # Start the fixed bench fleet inside the namespace (backgrounded, logs
    # in ./scratch-logs/): an 8K and a 1080p sender, plus a receiver that
    # pulls the host's "MooSwitcher PGM" whenever it exists (the encode-side
    # probe). Network netns alone is NOT enough: the NDI SDK rendezvouses
    # same-machine peers through the filesystem (observed: a unix-socket
    # marker at /tmp/NTK_NDI_QUIC_SERVER_V5_<port>, plus /dev/shm) and then
    # moves pixels over that local channel regardless of IPs. The fleet
    # therefore gets private /tmp and /dev/shm (mount ns) and its own
    # hostname (UTS ns); logs go to the repo, which stays shared on purpose.
    # Teardown from the host: pkill -x moo-testgen / moo-latmeter.
    NSCFG="$PWD/scratch-ndi-ns"
    LOGS="$PWD/scratch-logs"
    mkdir -p "$NSCFG" "$LOGS"
    # Running under sudo: the fleet writes here as the invoking user.
    chown "${SUDO_USER:-$USER}" "$NSCFG" "$LOGS" 2>/dev/null || true
    printf '{ "ndi": { "networks": { "ips": "%s" } } }\n' $HOST_IP \
        > "$NSCFG/ndi-config.v1.json"
    chmod 644 "$NSCFG/ndi-config.v1.json"
    ip netns exec $NS unshare --uts --mount sh -c "
        hostname moo-netbench
        mount -t tmpfs -o size=2g tmpfs /dev/shm
        mount -t tmpfs -o size=1g tmpfs /tmp
        exec sudo -u '${SUDO_USER:-$USER}' sh -c \"
            cd '$PWD'
            nohup ./build/moo-testgen --name MooNet8K --size 7680x4320 \
                --precompute 24 --noise --quiet > '$LOGS/nb-tg8k.log' 2>&1 &
            nohup ./build/moo-testgen --name MooNet1080 --noise --quiet \
                > '$LOGS/nb-tg1080.log' 2>&1 &
            NDI_CONFIG_DIR='$NSCFG' nohup ./build/moo-latmeter \
                --source 'MooSwitcher PGM' --find-timeout 3600 \
                --duration 3600 --quiet --csv '$LOGS/nb-recv.csv' \
                > '$LOGS/nb-recv.log' 2>&1 &
            echo 'bench fleet started (isolated /tmp, /dev/shm, hostname)'\""
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
