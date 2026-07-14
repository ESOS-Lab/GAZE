#!/bin/bash

make -j40 && \
sudo make modules_install -j40 && \
sudo find /lib/modules/6.1.0.copy/ -name *.ko -exec strip --strip-unneeded {} +  && \
sudo make install -j40 && \


#cp fs/f2fs/f2fs.ko /home/juwon/test_fast_gc/mod/f2fs_vanilla.ko
#cp fs/f2fs/f2fs.ko /home/juwon/test_fast_gc/mod/f2fs_preread_batch_128.ko
cp fs/f2fs/f2fs.ko /home/juwon/test_fast_gc/mod/f2fs_tmp.ko
cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_tmp.ko

#cp fs/f2fs/f2fs.ko /home/juwon/test_fast_gc/mod/f2fs_vanilla.ko
#sudo rm -rf /boot/*5.11.0*/ && rm -rf /lib/modules/*5.11.0* 
#sudo find /lib/modules/5.11.0.f2fs-zns/ -name *.ko -exec strip --strip-unneeded {} +  && \
