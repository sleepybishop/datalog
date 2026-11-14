use strict;
use warnings;
use Test::More;
use File::Temp qw(tempdir);
use IPC::Open3;
use Symbol qw(gensym);

sub run_repl {
    my ($statement, $database) = @_;
    my $errors = gensym;
    my @args = defined $database ? ($database) : ();
    my $pid = open3(my $input, my $output, $errors, './sparql_repl', @args);
    print {$input} $statement;
    close $input;
    my $stdout = do { local $/; <$output> } // '';
    my $stderr = do { local $/; <$errors> } // '';
    waitpid($pid, 0);
    return ($stdout, $stderr, $?);
}

my $dir = tempdir(CLEANUP => 1);
my $database = "$dir/facts.db";
my ($out, $err, $status) = run_repl(
    'PREFIX ex: <http://example/> INSERT DATA { ex:s ex:p "value:-literal" . }',
    $database);
is $status, 0, 'prefixed insertion exits successfully';
is $err, '', 'prefixed insertion is not routed to the rule or query parser';
like $out, qr/Successfully added 1 facts/, 'prefixed insertion adds its tuple';
like $out, qr/Persisted database changes/, 'successful persistence is reported';

($out, $err, $status) = run_repl(
    'PREFIX ex: <http://example/> ASK WHERE { ex:s ex:p "value:-literal" . }',
    $database);
is $status, 0, 'prefixed query exits successfully';
is $err, '', 'a rule delimiter in a literal does not select the rule parser';
like $out, qr/^true$/m, 'prefixed query finds the persisted literal';

($out, $err, $status) = run_repl('INSERT DATA { <s> <p> "another:-literal" . }');
is $status, 0, 'plain insertion exits successfully';
is $err, '', 'plain insertion with a rule delimiter uses the INSERT parser';
like $out, qr/Successfully added 1 facts/, 'plain insertion adds its tuple';

($out, $err, $status) = run_repl('?X <derived> ?Y :- ?X <base> ?Y .');
is $status, 0, 'rule input exits successfully';
is $err, '', 'real rules still use the rule parser';
like $out, qr/Successfully added rule/, 'real rule is accepted';

for my $head ('SELECT', 'ASK', 'INSERT', '"INSERT"', '<INSERT>') {
    ($out, $err, $status) = run_repl("$head <derived> <yes> :- <a> <source> <yes> .");
    is $status, 0, "rule with $head subject exits successfully";
    is $err, '', "rule with $head subject uses the rule parser";
    like $out, qr/Successfully added rule/, "rule with $head subject is accepted";
}

($out, $err, $status) = run_repl('ASK WHERE { <x:-y> <p> <o> }');
is $status, 0, 'query with a rule delimiter in an IRI exits successfully';
is $err, '', 'a rule delimiter in an IRI does not select the rule parser';
like $out, qr/^false$/m, 'IRI query executes against the empty database';

($out, $err, $status) = run_repl('PREFIX ex: <http://example/> INSERT DATA { ex:-subject ex:p ex:o }');
is $status, 0, 'insertion with a colon-hyphen in a prefixed name exits successfully';
is $err, '', 'a colon-hyphen inside a prefixed name does not select the rule parser';
like $out, qr/Successfully added 1 facts/, 'prefixed-name insertion adds its tuple';

($out, $err, $status) = run_repl(
    'PREFIX : <http://example/> INSERT DATA { :-subject :p :o . :- :p :o }', $database);
is $status, 0, 'insertion with default-prefixed colon-hyphen terms exits successfully';
is $err, '', 'default-prefixed colon-hyphen terms do not select the rule parser';
like $out, qr/Successfully added 2 facts/, 'both default-prefixed terms are inserted';

($out, $err, $status) = run_repl(
    'PREFIX : <http://example/> ASK WHERE { :-subject :p :o . :- :p :o }', $database);
is $status, 0, 'query with default-prefixed colon-hyphen terms exits successfully';
is $err, '', 'default-prefixed colon-hyphen query uses the query parser';
like $out, qr/^true$/m, 'default-prefixed terms round-trip through insertion and query';

done_testing();
