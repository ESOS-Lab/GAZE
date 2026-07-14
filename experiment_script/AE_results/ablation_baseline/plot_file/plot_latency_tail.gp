set terminal postscript eps enhanced color dashed dashlength 3 \
	                linewidth 2 rounded font "Helvetica, 40"
set term postscript size 4,2.5
set output "real_plot/hs_latency_tail.eps"
set style data histogram
set style fill solid 1.0 border 0
set style histogram clustered gap 1
set boxwidth 1 absolute
set xrange[-0.6:0.6]

set border 11
#set grid
set yrange [0:*]
set ytics 1
set ylabel "Latency (sec)" offset 1., -0.1
set bmargin 1
set lmargin 6.5
set tmargin 1
set rmargin 1
set key samplen 0.5
#set key outside top horizontal center
unset key
unset xtics

plot "../result_latency_tail.data" using ($9/1000000):xtic(1) linecolor rgb "black"        title "Vanilla   ",\
     "../result_latency_tail.data" using ($8/1000000):xtic(1) linecolor rgb "forest-green"        title "Pg        ",\
     "../result_latency_tail.data" using ($5/1000000):xtic(1) linecolor rgb "#e69f00"        title "NB        ",\
     "../result_latency_tail.data" using ($7/1000000):xtic(1) linecolor rgb "#0072b2"        title "Pipe      ",\
     "../result_latency_tail.data" using ($4/1000000):xtic(1) linecolor rgb "#56b4e9"        title "NB+Pg     ",\
     "../result_latency_tail.data" using ($6/1000000):xtic(1) linecolor rgb "#f0e442"        title "Pipe+Pg   ",\
     "../result_latency_tail.data" using ($3/1000000):xtic(1) linecolor rgb "dark-blue"        title "NB+Pipe   ",\
     "../result_latency_tail.data" using ($2/1000000):xtic(1) linecolor rgb "red" title "NB+Pipe+Pg"
