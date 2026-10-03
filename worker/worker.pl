#!/usr/bin/env perl
# MacRelix Perl core only: no installed modules, shell loops or exit traps.
# A claimed job is never replayed; a crash leaves its claim and worker lock.
# Even strict.pm and warnings.pm are absent in the installed guest. Use
# lexical variables explicitly; perl -w enables built-in warnings there.

@ARGV == 1 || @ARGV == 2 or die "usage: perl worker.pl QUEUE [--once]\n";
my ($queue, $mode) = @ARGV;
!defined($mode) || $mode eq '--once' or die "unknown mode\n";
-d $queue && !-l $queue or die "queue must be a real directory\n";
chdir $queue or die "chdir queue: $!\n";
mkdir 'worker-lock', 0700 or die "worker locked; inspect prior worker before recovery: $!\n";

sub record {
    my ($path, $text) = @_;
    !-e $path && !-l $path && !-e "$path.tmp" && !-l "$path.tmp"
        or die "record already exists: $path\n";
    open(my $out, '>', "$path.tmp") or die "open $path.tmp: $!\n";
    print $out $text or die "write $path: $!\n";
    close $out or die "close $path: $!\n";
    rename "$path.tmp", $path or die "publish $path: $!\n";
}

sub execute_job {
    my ($id) = @_;
    chdir $id or die "chdir $id: $!\n";
    # The singleton lock serializes claims. Producer must never change READY.
    rename 'ready', 'claimed' or die "claim $id: $!\n";
    record('started', "protocol=1\nid=$id\nstate=claimed\n");
    my $valid = -f 'claimed' && !-l 'claimed' && -s 'claimed' == 11;
    if ($valid) {
        open(my $in, '<', 'claimed') or die "read claim: $!\n";
        local $/;
        $valid = <$in> eq "protocol=1\n";
        close $in or die "close claim: $!\n";
    }
    $valid &&= -f 'script' && !-l 'script' && -s 'script' <= 65536;
    if ($valid) {
        open(my $in, '<', 'script') or die "read script: $!\n";
        local $/;
        my $script = <$in>;
        $valid = defined($script) && $script !~ /[\r\0]/;
        close $in or die "close script: $!\n";
    }
    # Preexisting worker outputs imply a damaged or partially executed job.
    !-e 'result' && !-l 'result' && !-e 'stdout' && !-l 'stdout'
        && !-e 'stderr' && !-l 'stderr' or die "conflicting outputs: $id\n";
    if (!$valid) {
        record('result', "protocol=1\nid=$id\noutcome=rejected\n");
        chdir '..' or die "chdir queue: $!\n";
        return;
    }
    # Guest Perl cannot fork() or duplicate descriptors with three-arg open.
    # Let sh redirect and exec; this literal command contains no job input.
    # system() returns only after the foreground shell closes its logs.
    my $status = system('/bin/sh', '-c', 'exec /bin/sh script < /dev/null > stdout 2> stderr');
    $status != -1 or die "spawn failed: $!\n";
    -f 'stdout' && !-l 'stdout' && -f 'stderr' && !-l 'stderr'
        or die "missing output capture: $id\n";
    my $signal = $status & 127;
    my $exit = $status >> 8;
    my $outcome = $signal ? 'signaled' : $exit ? 'failed' : 'succeeded';
    record('result', "protocol=1\nid=$id\noutcome=$outcome\nexit=$exit\nsignal=$signal\nwait_status=$status\n");
    chdir '..' or die "chdir queue: $!\n";
}

while (!-e 'STOP') {
    opendir(my $dir, '.') or die "opendir: $!\n";
    my @ids = sort grep { /\A[a-z0-9_-]{1,24}\z/ && $_ ne 'worker-lock' } readdir $dir;
    closedir $dir or die "closedir: $!\n";
    foreach my $id (@ids) {
        last if -e 'STOP';
        next unless -d $id && !-l $id && (-e "$id/ready" || -l "$id/ready");
        next if -e "$id/claimed" || -l "$id/claimed";
        execute_job($id);
    }
    last if defined $mode;
    sleep 1;
}
rmdir 'worker-lock' or die "unlock: $!\n";
