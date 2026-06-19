use strict;
use warnings;
use Test::More;
use t::Util;
use File::Temp qw(tempfile tempdir);
use Digest::MD5 qw(md5_hex);
use Data::Dumper;

chdir "./t/00util/test/";

sub run_suite {
    my $cmd = shift;
    my ($out, $err) = run_prog("./$cmd");
    diag($out);
    return $?;
}

subtest "check suites" => sub {
    my @suites = (
        "check_fact",
        "check_facts",
        "check_intern",
        "check_set",
        "check_spec",
        "check_movie_integration",
        "check_triejoin",
        "check_sparql"
    );
    foreach (@suites) {
        is 0, run_suite($_), $_;
    }
    my @test_files = (
        "test_write_facts_add_anon",
        "test_write_facts_empty",
        "test_write_facts_escapes",
        "test_write_facts_log_escapes",
        "test_write_facts_log_one",
        "test_write_facts_log_ten",
        "test_write_facts_log_two",
        "test_write_facts_one",
        "test_write_facts_ten",
        "test_write_facts_two"
    );
    foreach (@test_files) {
        my $munged = $_;
        $munged =~ s/_write//;
        my $expected = slurp_file($munged);
        my $actual = slurp_file("/tmp/$_");
        is $actual, $expected, "$munged";
    }
};
        
done_testing();
