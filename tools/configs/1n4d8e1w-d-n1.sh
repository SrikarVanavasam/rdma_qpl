sudo accel-config disable-device iax1
sudo accel-config disable-device iax3
sudo accel-config disable-device iax5
sudo accel-config disable-device iax7

sudo accel-config load-config -c /fast-lab-share/srikarv2/rdma_qpl/tools/configs/1n4d8e1w-d-n1.conf

sudo accel-config enable-device iax1
sudo accel-config enable-device iax3
sudo accel-config enable-device iax5
sudo accel-config enable-device iax7

sudo accel-config enable-wq iax1/wq1.0
sudo accel-config enable-wq iax3/wq3.0
sudo accel-config enable-wq iax5/wq5.0
sudo accel-config enable-wq iax7/wq7.0
