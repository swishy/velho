use strict;
use warnings;
use threads;
use threads::shared;
use Time::HiRes qw(time);

# Count is per worker. Keep interleaved objects live through partial free and
# refill: this measures holes in occupied allocation pools, not just teardown.
my ($count, $workers) = @ARGV;
$count //= 75_000;
$workers //= 4;
die "usage: $0 positive-count positive-workers\n"
    unless $count =~ /^\d+$/ && $count > 0
        && $workers =~ /^\d+$/ && $workers > 0;

my $gate :shared;
my $phase :shared = 0;
my @arrived :shared = (0, 0, 0, 0);
my $failure :shared = '';

sub checkpoint {
    my ($stage) = @_;
    lock($gate);
    ++$arrived[$stage];
    cond_broadcast($gate);
    cond_wait($gate) while $phase <= $stage && !$failure;
    die $failure if $failure;
}

sub key_for {
    my ($i) = @_;
    return "elem:$i:" . ('x' x (8 + $i % 40));
}

sub verify_graph {
    my ($big) = @_;
    my $sum = 0;
    for my $key (keys %$big) {
        my $value = $big->{$key};
        die "payload corrupt\n" unless $value->[0] == $value->[2]{id}
            && $value->[1] eq 'val' . ($value->[0] * 7);
        $sum += $value->[0];
    }
    die "payload checksum\n" unless keys(%$big) == $count
        && $sum == $count * ($count + 1) / 2;
}

sub verify_strings {
    my ($strs) = @_;
    for my $i (0 .. $#$strs) {
        my $id = $i + 1;
        my $expected = join(':', 'a', $id, substr('val' . ($id * 7), 0, 4));
        die "string corrupt\n" unless $strs->[$i] eq $expected;
    }
}

sub workload {
    my ($hash_ms, $scan_ms, $strings_ms);
    my ($partial_ms, $refill_ms, $cleanup_start);
    {
        my $t0 = time;
        my %big;
        for my $i (1 .. $count) {
            $big{key_for($i)} = [$i, 'val' . ($i * 7), {id => $i, tag => "t$i"}];
        }
        my $t1 = time;
        verify_graph(\%big);
        my $t2 = time;
        my @strs;
        for my $i (1 .. int($count * 2 / 3)) {
            push @strs, join(':', 'a', $i, substr($big{key_for($i)}[1], 0, 4));
        }
        my @sorted = sort @strs;
        verify_strings(\@strs);
        my $t3 = time;
        ($hash_ms, $scan_ms, $strings_ms) =
            (1000 * ($t1 - $t0), 1000 * ($t2 - $t1), 1000 * ($t3 - $t2));
        checkpoint(0);

        my $partial_start = time;
        # Leave odd hash entries and every other string alive. Preserve both
        # array capacities; release sorted aliases so dropped PVs can die.
        for (my $i = 2; $i <= $count; $i += 2) { delete $big{key_for($i)} }
        for (@sorted) { undef $_ }
        for (my $i = 1; $i < @strs; $i += 2) { undef $strs[$i] }
        die "partial payload count\n" unless keys(%big) == int(($count + 1) / 2);
        $partial_ms = 1000 * (time - $partial_start);
        checkpoint(1);

        my $refill_start = time;
        for (my $i = 2; $i <= $count; $i += 2) {
            $big{key_for($i)} = [$i, 'val' . ($i * 7), {id => $i, tag => "t$i"}];
        }
        for (my $i = 1; $i < @strs; $i += 2) {
            my $id = $i + 1;
            $strs[$i] = join(':', 'a', $id, substr($big{key_for($id)}[1], 0, 4));
        }
        @sorted = sort @strs;
        verify_graph(\%big);
        verify_strings(\@strs);
        $refill_ms = 1000 * (time - $refill_start);
        checkpoint(2);
        $cleanup_start = time;
    }
    my $cleanup_ms = 1000 * (time - $cleanup_start);
    checkpoint(3);                 # Payload gone, interpreter/worker still live.
    return [$hash_ms, $scan_ms, $strings_ms, $partial_ms, $refill_ms, $cleanup_ms];
}

sub rss {
    open my $status, '<', '/proc/self/status' or die "status: $!\n";
    while (<$status>) { return $1 if /^VmRSS:\s+(\d+)/ }
    die "VmRSS missing\n";
}

my @threads = map {
    threads->create(sub {
        my $result = eval { workload() };
        if ($@) {
            lock($gate);
            $failure = $@;
            cond_broadcast($gate);
        }
        return $result;
    })
} 1 .. $workers;

print "THREADED workers=$workers count-per-worker=$count\n";
my @labels = ('RSS', 'RSS_PARTIAL', 'RSS_REFILL', 'RSS_FREED');
for my $stage (0 .. 3) {
    lock($gate);
    cond_wait($gate) while $arrived[$stage] != $workers && !$failure;
    die $failure if $failure;
    printf "%s: %d kB\n", $labels[$stage], rss();
    ++$phase;
    cond_broadcast($gate);
}
my @times = (0) x 6;
for my $thread (@threads) {
    my $result = $thread->join;
    die "worker failed\n" unless $result;
    $times[$_] += $result->[$_] / $workers for 0 .. 5;
}
printf "RSS_JOINED: %d kB\n", rss();
printf "mean-worker hash-build %.0f ms, scan %.0f ms, str+sort %.0f ms\n", @times[0 .. 2];
printf "mean-worker partial-free %.0f ms, refill %.0f ms, cleanup %.0f ms\n", @times[3 .. 5];
print "partial free, refill and worker cleanup: PASS\n";
