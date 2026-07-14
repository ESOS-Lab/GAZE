set terminal postscript eps enhanced color dashed dashlength 3 \
	                linewidth 2 rounded font "Helvetica, 40"
set term postscript size 4,2.5
set output "../real_plot/hs_latency_avg.eps"
set style data histogram
set style fill solid 1.0 border 0
set style histogram clustered gap 1
set boxwidth 1 absolute
set xrange[-0.6:0.6]

set border 11
#set grid
set yrange [0:*]
set ytics 5
set ylabel "Latency (us)" offset 1., -0.1
set bmargin 1
set lmargin 6.5
set tmargin 1
set rmargin 1
set key samplen 0.5
#set key outside top horizontal center
unset key
unset xtics
plot "../result_latency_avg.data" using 9:xtic(1) linecolor rgb "black"        title "Vanilla   ",\
     "../result_latency_avg.data" using 8:xtic(1) linecolor rgb "forest-green"        title "Pg        ",\
     "../result_latency_avg.data" using 5:xtic(1) linecolor rgb "#e69f00"        title "NB        ",\
     "../result_latency_avg.data" using 7:xtic(1) linecolor rgb "#0072b2"        title "Pipe      ",\
     "../result_latency_avg.data" using 4:xtic(1) linecolor rgb "#56b4e9"        title "NB+Pg     ",\
     "../result_latency_avg.data" using 6:xtic(1) linecolor rgb "#f0e442"        title "Pipe+Pg   ",\
     "../result_latency_avg.data" using 3:xtic(1) linecolor rgb "dark-blue"        title "NB+Pipe   ",\
     "../result_latency_avg.data" using 2:xtic(1) linecolor rgb "red" title "NB+Pipe+Pg"

