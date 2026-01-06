sudo accel-config disable-device iax1
sudo accel-config disable-device iax3
sudo accel-config disable-device iax5
sudo accel-config disable-device iax7

sudo accel-config load-config -c /fast-lab-share/srikarv2/rdma_qpl/tools/configs/1n1d8e1w-s-n1.conf

sudo accel-config enable-device iax1

sudo accel-config enable-wq iax1/wq1.0
