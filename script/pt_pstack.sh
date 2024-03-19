#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

PERF_BIN=$(which perf)
PERF_DATA=perf.data
PID=
PT_BIN=$SCRIPT_DIR/pt_flame
OUTPUT=pt_stack
LOG_FILE=

WARMUP=2000000
INTERVAL=1000000
COUNT=1

while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--pid)
            PID="$2"
            shift
            ;;
        -d|--data)
            PERF_DATA="$2"
            shift
            ;;
        -o|--output)
            OUTPUT="$2"
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
        -w|--warmup)
            WARMUP="$2"
            shift
            ;;
        -i|--interval)
            INTERVAL="$2"
            shift
            ;;
        -c|--count)
            COUNT="$2"
            shift
            ;;
        -h|--help)
            echo "Usage: pt_pstack.sh [options]"
            echo "  -p/--pid collect PT data from pid, use existing data if unset"
            echo "  -d/--data perf data name, default [perf.data]"
            echo "  -o/--output output file name, will append .svg to flamegraph, default [flame]"
            echo "  -t/--pt stack replay binary, required"
            echo "  -c/--count number of stack to print, default [1]"
            echo "  -w/--warmup time before printing first stack, default [2000000] ns"
            echo "     increase warmup time if too few stack is printed"
            echo "  -i/--interval time between each stack, default [1000000] ns"
            echo "  -l/--log log file, default [stderr]"
            exit 1;;
        *)
            echo "Unknown option '$1'"
            exit 1;;
    esac
    shift
done

# two warmup time just to be safe
SLEEP_TIME_NS=$(($WARMUP + $COUNT * $INTERVAL + $WARMUP))
SLEEP_TIME=$(bc -l <<< "$SLEEP_TIME_NS / 1000000000")

if [[ -n "$PID" ]]; then
   $PERF_BIN record -m,4M -e intel_pt/cyc/u -p $PID -o $PERF_DATA -- sleep $SLEEP_TIME
fi

perf_param="--itrace=b --ns -F-event,-period,+addr,-comm,+flags,-dso -f "

if [[ -z "$LOG_FILE" ]]; then
   $PERF_BIN script $perf_param | $PT_BIN -S $OUTPUT -W $WARMUP -I $INTERVAL -C $COUNT -O
else
   $PERF_BIN script $perf_param | $PT_BIN -S $OUTPUT -W $WARMUP -I $INTERVAL -C $COUNT -O 2> $LOG_FILE
fi
