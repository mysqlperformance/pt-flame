#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

PERF_BIN=$(which perf)
PERF_DATA=./perf.data
PT_BIN=$SCRIPT_DIR/pt_flame
FLAME_BIN=$SCRIPT_DIR/flamegraph.pl
OUTPUT="flame"
USE_FIFO=
OVERWRITE=
SKIP=
PARALLEL=0

JEMALLOC=$(which jemalloc-config)
if [[ -n $JEMALLOC ]]; then
   export LD_PRELOAD=`jemalloc-config --libdir`/libjemalloc.so.`jemalloc-config --revision`
fi

LOG_FILE=/dev/null

while [[ $# -gt 0 ]]; do
    case $1 in
        -i|--input)
            PERF_DATA="$2"
            shift
            ;;
        -o|--output)
            OUTPUT="$2"
            shift
            ;;
        -p|--perf)
            PERF_BIN="$2"
            shift
            ;;
        -t|--pt)
            PT_BIN="$2"
            shift
            ;;
        -l|--log)
            LOG_FILE="$2"
            shift
            ;;
        --fifo)
            USE_FIFO=1
            ;;
        --overwrite)
            OVERWRITE=1
            ;;
        --skip)
            SKIP=1
            ;;
        -j)
            PARALLEL="$2"
            shift
            ;;
        -h|--help)
            echo "Usage: draw.sh [options]"
            echo "  -i/--input input perf data, default [perf.data]"
            echo "  -o/--output output file name, will append .svg to flamegraph, default [flame]"
            echo "  -p/--perf modified perf binary, default [$(which perf)]"
            echo "  -t/--pt stack replay binary, use bundled by default"
            echo "  -l/--log log file, default [/dev/null]"
            echo "  -j use parallel, default [disable], DO NOT use parallel in production"
            echo "  --overwrite remove perf script intermediate file if exist"
            echo "  --skip skip perf script"
            echo "  --fifo make fifos for perf script, default [disabled]"
            exit 1;;
        *)
            echo "Unknown option '$1'"
            exit 1;;
    esac
    shift
done

parallel_prefix=parallel_out_
perf_param="--itrace=b --ns -F-event,-period,+addr,-comm,+flags,-dso -f "

if [[ $PARALLEL == 0 ]]; then
    # use vanilla perf
    $PERF_BIN script $perf_param -i $PERF_DATA $perf_param | \
        $PT_BIN -j 0 2> "$LOG_FILE" | tee "$OUTPUT" | \
        $FLAME_BIN > "${OUTPUT}.svg" 2> "$LOG_FILE"
else
    # use special parallel perf
    perf_param="${perf_param} --parallel-prefix ${parallel_prefix} --parallel-filter"

    echo "parallel preflight"
    if ! [[ -f "$PERF_DATA" ]]; then
        echo "perf data ${PERF_DATA} not found"
        exit 255;
    fi

    preflight=$($PERF_BIN script --parallel-preflight -i $PERF_DATA $perf_param \
                          2> "$LOG_FILE")
    if [[ $? != 0 ]]; then
        echo "perf binary ${PERF_BIN} does not support parallel"
        exit 255;
    fi

    preflight=$(echo "$preflight" | grep -E "^Preflight:")
    cpu_list=$(echo "$preflight" | grep -Po '(?<=cpu\[)\d*(?=\])' | sort | uniq)
    echo "found $(echo "$preflight" | wc -l) perf events collected from cpus: "
    echo $cpu_list

    # prepare pt parameters
    cores=$(nproc --all)
    echo "this machine has ${cores} cores"
    pt_parameters="-j ${cores} "
    IFS=$'\n'
    for cpu in $cpu_list; do
        cpu_files=$(echo "${preflight}" | grep "cpu\[${cpu}\]" | \
                        awk -c '{print $4}' | sort | tr '\n' ',' | sed 's/,$//g')
        pt_parameters="${pt_parameters} -c ${cpu} -t ${cpu_files}"
    done

    # check for perf script output files
    file_list=$(echo "$preflight" | awk -c '{print $4}')
    if [[ -z "$SKIP" ]]; then
        for f in $file_list; do
            if [ -e "${f}" ]; then
                echo -n "${f} exists, "
                if [[ -z "$OVERWRITE" ]]; then
                    echo "use --overwrite to overwrite."
                    exit 255;
                else
                    echo "delete."
                    rm "$f"
                fi
            fi
        done
    else
        for f in $file_list; do
            if ! [ -f "${f}" ]; then
                echo "${f} don't exist, don't set --skip"
                exit 255
            fi
        done
    fi
    IFS=$' '
    echo "pt parameters: "
    echo $pt_parameters

    if [[ -z "$USE_FIFO" ]]; then
        if [[ -z "$SKIP" ]]; then
            echo "do perf script"
            $PERF_BIN script --parallel-pt -i $PERF_DATA $perf_param 2> "$LOG_FILE"
        else
            echo "skip perf script"
        fi
        echo "make flame graph"
        $PT_BIN $pt_parameters 2> "$LOG_FILE" | tee "$OUTPUT" | \
            $FLAME_BIN > "${OUTPUT}.svg" 2> "$LOG_FILE"
    else
        echo "make fifos"
        IFS=$'\n'
        for f in $file_list; do
            mkfifo "$f";
        done
        IFS=$' '

        echo "start perf script"
        $PERF_BIN script --parallel-pt -i $PERF_DATA $perf_param 2> "$LOG_FILE" &

        echo "start make flame graph"
        $PT_BIN $pt_parameters 2> "$LOG_FILE" | tee "$OUTPUT" | \
            $FLAME_BIN > "${OUTPUT}.svg" 2> "$LOG_FILE" &
        fi
        echo "waiting..."
        wait
        echo "remove fifo"
        IFS=$'\n'
        for f in $file_list; do
            rm "$f";
        done
        IFS=$' '

    fi
fi
