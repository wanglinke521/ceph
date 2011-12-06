#!/bin/bash -ex

CEPH_SECRET_FILE=${CEPH_SECRET_FILE:-}
CEPH_ID=${CEPH_ID:-admin}
SECRET_ARGS=''
if [ -z $CEPH_SECRET_FILE ]; then
	SECRET_ARGS="--secret $CEPH_SECRET_FILE"
fi

TMP_FILES="/tmp/img1 /tmp/img1.small /tmp/img1.snap1 /tmp/img1.export /tmp/img1.trunc"

function get_device_dir {
	local POOL=$1
	local IMAGE=$2
	local SNAP=$3
	rbd showmapped | tail -n +2 | egrep "\s+$POOL\s+$IMAGE\s+$SNAP\s+" | cut -f 1
}

function clean_up {
	rbd unmap /dev/rbd/rbd/testimg1 || true
	rbd unmap /dev/rbd/rbd/testimg1@snap1 || true
	sudo chown root /sys/bus/rbd/add /sys/bus/rbd/remove
	rbd rm testimg1 || true
	rm -f $TMP_FILES
}

clean_up

trap clean_up INT TERM EXIT

# allow ubuntu user to map/unmap rbd devices
sudo chown ubuntu /sys/bus/rbd/add /sys/bus/rbd/remove

# create an image
dd if=/bin/sh of=/tmp/img1 bs=1k count=1 seek=10
dd if=/bin/dd of=/tmp/img1 bs=1k count=10 seek=100
dd if=/bin/rm of=/tmp/img1 bs=1k count=100 seek=1000
dd if=/bin/ls of=/tmp/img1 bs=1k seek=10000
dd if=/bin/ln of=/tmp/img1 bs=1k seek=100000
dd if=/dev/zero of=/tmp/img1 count=0 seek=150000

# import
rbd import /tmp/img1 testimg1
rbd map testimg1 --user $CEPH_ID $SECRET_ARGS
DEV_ID1=$(get_device_dir rbd testimg1 -)
echo "dev_id1 = $DEV_ID1"
cat /sys/bus/rbd/devices/$DEV_ID1/size
cat /sys/bus/rbd/devices/$DEV_ID1/size | grep 76800000

# wait for udev to catch up
while test ! -e /dev/rbd/rbd/testimg1
do
	sleep 1
done
sudo dd if=/dev/rbd/rbd/testimg1 of=/tmp/img1.export
cmp /tmp/img1 /tmp/img1.export

# snapshot
rbd snap create testimg1 --snap=snap1
cat /sys/bus/rbd/devices/$DEV_ID1/snap_snap1/snap_size | grep 76800000
rbd map --snap=snap1 testimg1 --user $CEPH_ID $SECRET_ARGS
DEV_ID2=$(get_device_dir rbd testimg1 snap1)
cat /sys/bus/rbd/devices/$DEV_ID2/size | grep 76800000

# wait for udev to catch up
while test ! -e /dev/rbd/rbd/testimg1@snap1
do
	sleep 1
done
sudo dd if=/dev/rbd/rbd/testimg1@snap1 of=/tmp/img1.snap1
cmp /tmp/img1 /tmp/img1.snap1

# resize
rbd resize testimg1 --size=40
echo 1 | sudo tee /sys/bus/rbd/devices/$DEV_ID1/refresh
cat /sys/bus/rbd/devices/$DEV_ID1/size | grep 41943040
echo 1 | sudo tee /sys/bus/rbd/devices/$DEV_ID2/refresh
cat /sys/bus/rbd/devices/$DEV_ID2/size | grep 76800000

sudo dd if=/dev/rbd/rbd/testimg1 of=/tmp/img1.small
cp /tmp/img1 /tmp/img1.trunc
truncate -s 41943040 /tmp/img1.trunc
cmp /tmp/img1.trunc /tmp/img1.small

# rollback and check data again
rbd snap rollback --snap=snap1 testimg1
echo 1 | sudo tee /sys/bus/rbd/devices/$DEV_ID1/refresh
cat /sys/bus/rbd/devices/$DEV_ID1/snap_snap1/snap_size | grep 76800000
cat /sys/bus/rbd/devices/$DEV_ID1/size | grep 76800000
rm -f /tmp/img1.snap1 /tmp/img1.export

sudo dd if=/dev/rbd/rbd/testimg1@snap1 of=/tmp/img1.snap1
cmp /tmp/img1 /tmp/img1.snap1
sudo dd if=/dev/rbd/rbd/testimg1 of=/tmp/img1.export
cmp /tmp/img1 /tmp/img1.export

# remove snapshot and detect error from mapped snapshot
rbd snap rm --snap=snap1 testimg1
sudo dd if=/dev/rbd/rbd/testimg1@snap1 of=/tmp/img1.snap1 2>&1 | grep 'Input/output error'

echo OK
