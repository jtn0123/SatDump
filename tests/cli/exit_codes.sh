#!/bin/bash
# Exit-code contract: 0 = success, 1 = usage error, 2 = the command failed.
. "$(dirname "$0")/common.sh"

run 0 help --help
run 1 unknown-option --no-such-option
run 1 no-pipeline-name pipeline
run 1 missing-arguments pipeline metop_ahrpt
run 1 unknown-pipeline pipeline no_such_pipeline baseband "$WORK/in.bin" "$WORK/out"
run 1 missing-input pipeline metop_ahrpt cadu "$WORK/no-such-file.cadu" "$WORK/out"

exit $FAILED
