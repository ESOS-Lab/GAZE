#!/bin/bash

cat dmesg | grep "cnt: 1" | grep util:\ 40 | grep pipe_OSDI |\
sed -e 's/\$/cache/g' \
    -e 's/(/( /g' \
        -e 's/\[/[ /g' \
	    > osdi_gc_lat_breakdown



awk '{
    c = 0
    for (i = 1; i <= NF; i++) {
        if ($i ~ /preproc:/) {
            c++
            if (c == 1) {
                sub(/preproc:/, "preproc_wrt:", $i)
            } else if (c == 2) {
                sub(/preproc:/, "preproc_wrt:", $i)
            }
        }
    }
    print
}'  osdi_gc_lat_breakdown > preprocessed_osdi_gc_lat





KEYWORDS=("$@")

awk -v keys="${KEYWORDS[*]}" '
BEGIN {
    n = split(keys, K, " ")
    # initialize sums
    for (i = 1; i <= n; i++)
        sum[K[i]] = 0
}

{
    # find values
    for (i = 1; i <= n; i++)
        val[K[i]] = ""

    for (i = 1; i <= NF; i++) {
        for (k = 1; k <= n; k++) {
            if ($i == K[k]) {
                val[K[k]] = $(i+1)
            }
        }
    }

    # accumulate
    for (k = 1; k <= n; k++)
        sum[K[k]] += val[K[k]]

    cnt++
}

END {
    # line 1: keywords
    for (k = 1; k <= n; k++)
        printf "%s%s", K[k], (k < n ? " " : "\n")

    # line 2: avg values
    for (k = 1; k <= n; k++)
        printf "%f%s", sum[K[k]] / cnt, (k < n ? " " : "\n")
}
' preprocessed_osdi_gc_lat







#
#cat osdi | awk '{
#    a=""; b="";
#    for(i=1;i<=NF;i++){
#        if($i=="A:") a=$(i+1)
#        if($i=="B:") b=$(i+1)
#    }
#    sumA += a
#    sumB += b
#    cnt++
#}
#END {
#    print "Avg A:", sumA/cnt
#    print "Avg B:", sumB/cnt
#}' 
# 
