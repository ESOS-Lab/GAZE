set terminal postscript eps enhanced color dashed dashlength 3 \
	                linewidth 2 rounded font "Helvetica, 40"
set term postscript size 4,2.7
set output "../real_plot/overall_bar_avg_ver3.eps"
set style data histogram
set style fill solid 1.0 border 0
set style histogram clustered gap 1
set boxwidth 0.5 absolute
set xrange[-0.45:0.45]

set border 11
#set grid
set yrange [0:*]
set ytics 40
set ylabel "KIOPS" offset 1.0, -0.1
set bmargin 1
set lmargin 6.5
set tmargin 1.5
set rmargin 1
set key samplen 0.5
#set key outside top horizontal center
unset key
unset xtics
plot "../result_4k_avg.dat" using 9:xtic(1) linecolor rgb "black"        title "Vanilla   ",\
     "../result_4k_avg.dat" using 8:xtic(1) linecolor rgb "#0072b2"        title "Pg        ",\
     "../result_4k_avg.dat" using 7:xtic(1) linecolor rgb "forest-green"        title "Pipe        ",\
     "../result_4k_avg.dat" using 5:xtic(1) linecolor rgb "#f0e442"        title "NB        ",\
     "../result_4k_avg.dat" using 6:xtic(1) linecolor rgb "#e69f00"        title "Pipe+Pg   ",\
     "../result_4k_avg.dat" using 2:xtic(1) linecolor rgb "red" title "NB+Pipe+Pg",\
     '../result_4k_avg.dat' using ($0 - 0.35):($9):(sprintf("%.1f", $9)) \
     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\
     '../result_4k_avg.dat' using ($0 - 0.21):($8):(sprintf("%.1f", $8)) \
     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\
     '../result_4k_avg.dat' using ($0 - 0.07):($7):(sprintf("%.1f", $7)) \
     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\
     '../result_4k_avg.dat' using ($0 + 0.07):($5):(sprintf("%.1f", $5)) \
     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\
     '../result_4k_avg.dat' using ($0 + 0.21):($6):(sprintf("%.1f", $6)) \
     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\
     '../result_4k_avg.dat' using ($0 + 0.35):($2):(sprintf("%.1f", $2)) \
     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\

#     '../result_4k_avg.dat' using ($0 - 0.3):($9):(sprintf("%.1f", $9)) \
#     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\
#     '../result_4k_avg.dat' using ($0 - 0.1):($5):(sprintf("%.1f", $5)) \
#     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\
#     '../result_4k_avg.dat' using ($0 + 0.1):($6):(sprintf("%.1f", $6)) \
#     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\
#     '../result_4k_avg.dat' using ($0 + 0.3):($2):(sprintf("%.1f", $2)) \
#     with labels font "Helvetica, 44" left offset 0, 0.1 rotate by 90 title ""  ,\

# plot "../result_4k_avg.dat" using 9:xtic(1) linecolor rgb "black"        title "Vanilla   ",\
#      "../result_4k_avg.dat" using 8:xtic(1) linecolor rgb "forest-green"        title "Pg        ",\
#      "../result_4k_avg.dat" using 5:xtic(1) linecolor rgb "#e69f00"        title "NB        ",\
#      "../result_4k_avg.dat" using 7:xtic(1) linecolor rgb "#0072b2"        title "Pipe      ",\
#      "../result_4k_avg.dat" using 4:xtic(1) linecolor rgb "#56b4e9"        title "NB+Pg     ",\
#      "../result_4k_avg.dat" using 6:xtic(1) linecolor rgb "#f0e442"        title "Pipe+Pg   ",\
#      "../result_4k_avg.dat" using 3:xtic(1) linecolor rgb "dark-blue"        title "NB+Pipe   ",\
#      "../result_4k_avg.dat" using 2:xtic(1) linecolor rgb "red" title "NB+Pipe+Pg"
