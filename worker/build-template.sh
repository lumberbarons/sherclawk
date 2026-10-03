#!/bin/sh
# Run only a materialized template snapshot inside a newly claimed job.
# The recipe reserves its own fresh output folder; failures leave no success.
set -e
set -u
sh build-native.sh native
