#!/bin/sh
# Run in MacRelix and redirect to an LF transcript. MPW version resources
# identify installed binaries. Use native-process-check.c for actual classic
# process paths: MacRelix /proc contains only MacRelix tasks.
# Do not infer the executor from MPW_DIR, which controls discovery only.
echo '== MacRelix build date and platform =='
cat /etc/build_date /etc/platform
echo '== MrC banner =='
/Developer/Tools/tlsrvr -- MrC
echo '== PPCLink version =='
/Developer/Tools/tlsrvr -- DeRez '"{MPW}Tools:PPCLink"' -only vers
echo '== Rez version =='
/Developer/Tools/tlsrvr -- DeRez '"{MPW}Tools:Rez"' -only vers
echo '== ToolServer version =='
/Developer/Tools/tlsrvr -- DeRez '"{MPW}ToolServer"' -only vers
echo '== MPW paths =='
/Developer/Tools/tlsrvr -- Echo '"{MPW}"' '"{CIncludes}"' '"{RIncludes}"' '"{SharedLibraries}"' '"{PPCLibraries}"'
echo '== SDK release (installed baseline path) =='
perl -e 'local $/ = "\r"; while (<>) { print "$_\n" if /Release:|UNIVERSAL_INTERFACES_VERSION 0x/; }' '/Volumes/Macintosh HD/Applications (Mac OS 9)/MPW-GM/Interfaces&Libraries/Interfaces/CIncludes/ConditionalMacros.h'
echo '== complete =='
