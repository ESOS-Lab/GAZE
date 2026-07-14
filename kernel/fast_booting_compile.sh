#!/bin/bash

#sudo make -j40  && \
make -j40 2> error_msg && \
cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_vanilla_profile.ko	# vanilla
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/pgc_profile.ko	# vanilla
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_copy_wait_copy_only_profile.ko	# vanilla
#cp fs/pgc/f3fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/pgc.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_vanilla_profile.ko	# vanilla
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_selective_blocking_happy.ko	# page pool only
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_selective_blocking_profile.ko	# page pool only
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_pgpool_profile.ko	# page pool only
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_4step_batchseg_512_profile.ko	# 4pipe varing batch
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_4step_profile.ko	# 4pipe`
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_4step_profile.ko	# GNS 4pipe`
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_pgpool_4step_profile.ko	# page pool + 4pipe

#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_4step_profile.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_2step_profile.ko

#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_vanilla_semiaggressive_discard_profile.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_deep_semiaggressive_discard_profile.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_deep_aggressive_discard_profile.ko

#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_deep_aggressive_discard_happy.ko

#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_deep_happy.ko

#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_deep_profile.ko

#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/fast_ver_happy.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/blk_gra_happy.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/blk_gra_profile.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_deep_happy.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_deep_is_alive_opti_happy.ko

#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_deep_profile.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/gns_blk_gra_profile.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_test.ko
#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_profile.ko

#cp fs/f2fs/f2fs.ko /home/juwon/test_fast_gc/mod/f2fs_copy.ko

#cp fs/f2fs/f2fs.ko /home/juwon/artifact_fastgc/experiment_script/general_resource/filesystem_module/f2fs_tmp.ko

#cp fs/f2fs/f2fs.ko /home/juwon/test_fast_gc/mod/f2fs_vanilla.ko
#sudo rm -rf /boot/*5.11.0*/ && rm -rf /lib/modules/*5.11.0* 
#sudo find /lib/modules/5.11.0.f2fs-zns/ -name *.ko -exec strip --strip-unneeded {} +  && \
