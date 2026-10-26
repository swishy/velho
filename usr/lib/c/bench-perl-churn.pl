use strict; use warnings;
use Time::HiRes qw(time);
srand(77);
# allocation-churn-heavy: simulates SAX-pipeline-ish patterns
# (many small PVs, hash interning, string building, temp lists)
my $count = shift // 300_000;
my $t0 = time;
my %big;
for my $i (1..$count) {
    my $key = "elem:" . $i . ":" . ("x" x (8 + $i % 40));
    $big{$key} = [ $i, "val" . ($i * 7), { id => $i, tag => "t$i" }];
}
my $t1 = time;
my $n = 0;
for my $k (keys %big) { $n += length $k }
my $t2 = time;
my @strs;
for my $i (1..int($count * 2 / 3)) {
    push @strs, join(":", "a", $i, substr($big{"elem:$i:" . ("x" x (8 + $i % 40))}[1] // "", 0, 4) // "");
}
my @sorted = sort @strs;
my $t3 = time;
printf "hash-build %0.0f ms, scan %0.0f ms, str+sort %0.0f ms (n=%d, s=%d)\n",
  ($t1-$t0)*1000, ($t2-$t1)*1000, ($t3-$t2)*1000, $n, scalar(@sorted);
open my $st, "<", "/proc/self/status" or exit;
while (<$st>) { print "RSS: $1 kB\n" if /^VmRSS:\s+(\d+)/ }
