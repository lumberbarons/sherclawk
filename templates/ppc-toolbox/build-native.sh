#!/bin/sh
# MacRelix has /bin/sh, not bash. MPW source must be CR/TEXT, and quotes
# around ToolServer variables must survive the shell into MPW command text.
# Each invocation reserves a new directory; failed builds are never reused.
set -e
set -u
test "$#" = 1 || exit 2
# Avoid compound shell syntax. Perl core needs no missing library modules.
perl -e 'exit($ARGV[0] !~ /\A[A-Za-z0-9_-]{1,24}\z/)' "$1"
mkdir -p build
mkdir "build/$1"
cd "build/$1"
stage=prepare
echo "stage=$stage started"
cp ../../main.c main.c
cp ../../app.r app.r
/Developer/Tools/SetFile -t TEXT -c ttxt main.c app.r
stage=compile
echo "stage=$stage started"
/Developer/Tools/tlsrvr -- MrC main.c -o main.o -i '"{CIncludes}"' -w off
stage=link
echo "stage=$stage started"
/Developer/Tools/tlsrvr -- PPCLink -o Template main.o '"{SharedLibraries}"InterfaceLib' '"{SharedLibraries}"StdCLib' '"{PPCLibraries}"StdCRuntime.o' '"{PPCLibraries}"PPCCRuntime.o' -t APPL
stage=resources
echo "stage=$stage started"
/Developer/Tools/tlsrvr -- Rez app.r -o Template -append -i '"{RIncludes}"'
stage=metadata
/Developer/Tools/SetFile -t APPL -c SHTP Template
test -s Template
echo 'artifact=Template' > success.txt
stage=complete
echo "stage=$stage status=0"
