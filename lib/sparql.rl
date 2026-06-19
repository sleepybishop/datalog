#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sparql.h"

%%{
    machine sparql_lexer;

    # Variables starting with '?'
    variable = '?' [a-zA-Z0-9_]+;

    # Constants (URIs in <...>, string literals in "...", or barewords)
    uri = '<' [^>]* '>';
    literal = '"' [^"]* '"';
    bareword = [a-zA-Z0-9_:\-]+;
    constant = uri | literal | bareword;

    # Keywords
    select_kw = [sS][eE][lL][eE][cC][tT];
    ask_kw = [aA][sS][kK];
    where_kw = [wW][hH][eE][rR][eE];
    not_kw = [nN][oO][tT];
    prefix_kw = [pP][rR][eE][fF][iI][xX];
    limit_kw = [lL][iI][mM][iI][tT];
    offset_kw = [oO][fF][fF][sS][eE][tT];
    order_kw = [oO][rR][dD][eE][rR];
    by_kw = [bB][yY];
    asc_kw = [aA][sS][cC];
    desc_kw = [dD][eE][sS][cC];

    # Symbols
    lbrace = '{';
    rbrace = '}';
    lparen = '(';
    rparen = ')';
    dot = '.';

    # Whitespace
    whitespace = [ \t\r\n]+;

    main := |*
        whitespace;
        select_kw => { /* Tokenize SELECT */ };
        ask_kw    => { /* Tokenize ASK */ };
        where_kw  => { /* Tokenize WHERE */ };
        not_kw    => { /* Tokenize NOT */ };
        prefix_kw => { /* Tokenize PREFIX */ };
        limit_kw  => { /* Tokenize LIMIT */ };
        offset_kw => { /* Tokenize OFFSET */ };
        order_kw  => { /* Tokenize ORDER */ };
        by_kw     => { /* Tokenize BY */ };
        asc_kw    => { /* Tokenize ASC */ };
        desc_kw   => { /* Tokenize DESC */ };
        variable  => { /* Tokenize variable */ };
        constant  => { /* Tokenize constant */ };
        lbrace    => { /* Tokenize lbrace */ };
        rbrace    => { /* Tokenize rbrace */ };
        lparen    => { /* Tokenize lparen */ };
        rparen    => { /* Tokenize rparen */ };
        dot       => { /* Tokenize dot */ };
    *|;
}%%

/*
 * The equivalent hand-compiled state machine and recursive descent parser
 * are implemented in sparql.c to ensure compilation works out of the box
 * without requiring the 'ragel' compiler binary at build time.
 */
