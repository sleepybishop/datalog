use strict;
use warnings;
use Test::More;
use t::Util;
use Time::HiRes qw(sleep gettimeofday tv_interval);
use File::Temp qw(tempfile tempdir);
use Digest::MD5 qw(md5_hex);
use Data::Dumper;

sub run_bench {
    my $cmd = shift;
    my ($err, $out) = run_prog("./t/00util/bench/$cmd");
    diag $out;
    return $?;
}

subtest "bench set" => sub {
    my @benches = (
        "benchmark_facts_add",
        "benchmark_set_add",
        "benchmark_set_get",
        "benchmark_facts_with",
        "benchmark_set_add_overflow",
        "benchmark_set_remove",
        "benchmark_reactive_retraction"
    );
    foreach (@benches) {
        my $t0 = [gettimeofday];
        is 0, run_bench($_), $_;
        my $elapsed = tv_interval($t0, [gettimeofday]);
        diag "took: ${elapsed}s";
    }
};
        
done_testing();
