#!/bin/bash

bench_bin=~/libzbc/bench_fwu

result_file="bw_fix_20_20_200.csv"
#result_file="bw_fix_60_2_80.csv"
#result_file="bw_fix_70_1_80.csv"
#result_file="bw_fix_1_1_20.csv"
echo "" > $result_file
for i in 20 40 60 80 100 120 140 160 180 200
#for i in 60 62 64 66 68 70 72 74 76 78 80
#for i in 70 71 72 73 74 75 76 77 78 79 80
#for i in `seq 1 20`
do
    echo  $bench_bin/random_script_gen.sh $i -3 1024 
    $bench_bin/random_script_gen.sh $i -3 1024  > fix_$i.job
    echo $bench_bin/zbc_write_zone2 -p fix_$i.job -k $((1000/$i)) /dev/sdb
    $bench_bin/zbc_write_zone2 -p fix_$i.job -k $((1000/$i)) /dev/sdb > fix_$i.log
    $bench_bin/reset_write_ptr_all.sh /dev/sdb > /dev/null
    $bench_bin/extract_bw.sh fix_$i.log > bw_fix_$i.csv
    paste $result_file bw_fix_$i.csv > temp.csv
    mv temp.csv $result_file
done

head -n 900 $result_file > temp.csv
mv temp.csv $result_file
echo cp $result_file ~
cp $result_file ~
#tar cf ~/bw_fix_large.tar *.csv
#rm *.csv *.log *.job
