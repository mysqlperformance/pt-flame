#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

PERF_BIN=
SYSTEM_PERF=$(which perf)
PARALLEL_PERF=$(rpm -ql pt_func_perf | grep -P "/perf\$")

PERF_DATA=perf.data
PT_BIN=$SCRIPT_DIR/pt_flame
FLAME_BIN=$SCRIPT_DIR/flamegraph.pl
DL_FILTER_SO=$SCRIPT_DIR/../lib/pt_dlfilter.so

OUTPUT="flame"
LOG_FILE=
SKIP=
PARALLEL=0
RECORD_OPTIONS=" -e intel_pt/cyc/u "
CUSTOM_RECORD=
CUSTOM_FLAME=
FILTER=

DRY=

while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--data)
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
        -s|--skip)
            SKIP=1
            ;;
        -r|--record)
            CUSTOM_RECORD="$2"
            shift
            ;;
        -f|--flame)
            CUSTOM_FLAME="$2"
            shift
            ;;
        -j)
            PARALLEL="$2"
            shift
            ;;
        --dlfitler)
            FILTER=" --dlfilter $DL_FILTER_SO "
            ;;
        --dry-run)
            DRY=1
            ;;
        -h|--help)
            echo "Usage: pt_drawflame.sh [options]"
            echo "  Make sure existing perf script output is removed or they will be reused or overwritten"
            echo "  -o/--output <file>  output file name, will append .svg to flamegraph, "
            echo "                      default [flame]"
            echo "  -l/--log <file>     log file, default [stderr]"
            echo "  -j <num>            use parallel, default [0]"
            echo "  -r/--record <str>   perf record options, perf will be invoked like this:"
            echo "                      \"perf record -e intel_pt/cyc/u \$string\" "
            echo "                      multiple options will be expanded, e.g."
            echo "                      -r \"-p <pid> -- sleep 0.1\" expands to"
            echo "                      perf record -e intel_pt/cyc/u -p <pid> -- sleep 0.1"
            echo "  -d/--data <file>    perf record output, default [perf.data]"
            echo "                      or use existing perf data if -r not provided"
            echo "  -s/--skip           skip perf script and perf record, use existing data"
            echo "  -f/--flame <str>    additional options passed to pt_flame, similar to -r"
            echo "  -p/--perf <bin>     perf binary, defaults to [$SYSTEM_PERF] if -j 0,"
            echo "                      [$PARALLEL_PERF] if -j > 0"
            echo "  -t/--pt_flame <bin> pt_flame binary, use bundled by default"
            echo "  --dlfilter          use bundled pt_dlfilter.so with perf script"
            echo "  --dry-run           preview perf commands only"
            echo "  -h/--help           print this message"
            exit 1;;
        *)
            echo "Unknown option '$1'"
            exit 1;;
    esac
    shift
done

parallel_prefix=script_out_
perf_param=" --itrace=b --ns -F-event,-period,+addr,-comm,+flags,-dso $FILTER "

if [[ -z $SKIP ]]; then
    if [[ $PARALLEL == 0 ]]; then
        if [[ -z $PERF_BIN ]]; then
            PERF_BIN=$SYSTEM_PERF
        fi
    else
        if [[ -z $PERF_BIN ]]; then
            if [[ -z $PARALLEL_PERF ]]; then
                echo "parallel perf not found, did you install pt_func_perf?"
                exit 255
            fi
            PERF_BIN=$PARALLEL_PERF
        fi
    fi


    if [[ -n $CUSTOM_RECORD ]]; then
        record_cmd="$PERF_BIN record $RECORD_OPTIONS -o $PERF_DATA $CUSTOM_RECORD"
        echo $record_cmd
        if [[ -z $DRY ]]; then
            eval $record_cmd
        fi
    fi

    if ! [[ -f $PERF_DATA ]] && [[ -z $DRY ]]; then
        echo "no perf data [$PERF_DATA] found"
        exit 255
    fi

    script_cmd="$PERF_BIN script $perf_param -i $PERF_DATA"
    if [[ $PARALLEL == 0 ]]; then
        script_cmd="$script_cmd > ${parallel_prefix}_00000"
    else
        script_cmd="$script_cmd --parallel $PARALLEL --parallel-prefix ${parallel_prefix}"
    fi

    echo $script_cmd
    if [[ -z $DRY ]]; then
        eval $script_cmd
    fi
fi

if [[ $PARALLEL == 0 ]]; then
    pt_cmd="$PT_BIN $CUSTOM_FLAME ${parallel_prefix}*"
else
    pt_cmd="$PT_BIN -j $PARALLEL $CUSTOM_FLAME ${parallel_prefix}*"
fi

if [[ -z $LOG_FILE ]]; then
    pt_cmd="$pt_cmd | tee \"$OUTPUT\" | $FLAME_BIN > \"${OUTPUT}.svg\""
else
    pt_cmd="$pt_cmd 2> \"$LOG_FILE\" | tee \"$OUTPUT\" | $FLAME_BIN > \"${OUTPUT}.svg\" 2> \"$LOG_FILE\""
fi

echo $pt_cmd

JEMALLOC=$(which jemalloc-config)
if [[ -n $JEMALLOC ]]; then
    export LD_PRELOAD=`jemalloc-config --libdir`/libjemalloc.so.`jemalloc-config --revision`
    echo "use jemalloc"
fi

if [[ -z $DRY ]]; then
    eval $pt_cmd
fi
