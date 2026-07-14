set terminal postscript eps enhanced color dashed dashlength 3 linewidth 2 rounded font "Helvetica, 35"
set term postscript size 7.5, 0.8
set output "../real_plot/legend_only_hwanseok_ver3.eps"

unset border; unset xtics; unset ytics; unset xlabel; unset ylabel
set xrange [0:1]; set yrange [0:1]

set key center at screen 0.5, screen 0.5 horizontal
set key samplen 1.5 spacing 1.2 width -1 Left reverse maxrows 2
# set key box opaque   # 필요하면 박스 표시

set style fill solid 1.0 border -1
set boxwidth 1.0

$ONE << EOD
0 0
EOD

plot \
  $ONE using 1:2 with boxes lc rgb "black"         title "Vanilla", \
  $ONE using 1:2 with boxes lc rgb "#f0e442"         title "Sel", \
  $ONE using 1:2 with boxes lc rgb "#0072b2"         title "Page", \
  $ONE using 1:2 with boxes lc rgb "#e69f00"       title "Pipe+Page",    \
  $ONE using 1:2 with boxes lc rgb "forest-green"         title "Pipe", \
  $ONE using 1:2 with boxes lc rgb "red"           title "Page+Pipe+Sel"

#plot \
#  $ONE using 1:2 with boxes lc rgb "black"         title "F2FS", \
#  $ONE using 1:2 with boxes lc rgb "#f0e442"         title "F2FS-Sel", \
#  $ONE using 1:2 with boxes lc rgb "#0072b2"         title "F2FS-Page", \
#  $ONE using 1:2 with boxes lc rgb "#e69f00"       title "F2FS-Pipe+Page",    \
#  $ONE using 1:2 with boxes lc rgb "forest-green"         title "F2FS-Pipe", \
#  $ONE using 1:2 with boxes lc rgb "red"           title "GAZE"


#plot \
#  $ONE using 1:2 with boxes lc rgb "blue"         title "GNS", \
#  $ONE using 1:2 with boxes lc rgb "dark-violet"       title "IPLFS",      \
#  $ONE using 1:2 with boxes lc rgb "black"  title "zF2FS",         \



#plot \
#  $ONE using 1:2 with lines lw 2.5 lc rgb "red"         title "Read", \
#  $ONE using 1:2 with lines lw 2.5 lc rgb "blue"       title "Write",      \
