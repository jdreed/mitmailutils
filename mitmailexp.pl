#!/usr/bin/perl -w

# $Id: mitmailexp.pl,v 1.3 2004-07-29 19:11:53 rbasch Exp $

# Expunge an IMAP folder.

use strict;
use warnings FATAL => 'all';
use Cyrus::IMAP;
use Getopt::Long;

sub usage(;$);
sub send_command($);
sub fetch_callback(@);
sub number_callback(@);
sub make_msgspecs(@);
sub close_connection();
sub errorout($);

my $prog = $0;

sub usage(;$) {
    print STDERR "$prog: $_[0]\n" if ($_[0] && $_[0] ne "help");
    print STDERR <<EOF;
Usage: $prog [<options>]
  Options:
    --debug                turn on debugging
    --help                 print this usage information
    --host=<name>          query host <name> instead of default POBOX server
    --mailbox=<name>       examine mailbox <name> instead of INBOX
EOF
    exit 1;
}

# Parse the command line arguments.
use vars qw($opt_debug $opt_host $opt_mailbox);

GetOptions("debug",
	   "help" => \&usage,
	   "host=s",
	   "mailbox=s") || usage;

usage "Too many arguments" if @ARGV > 0;

$opt_mailbox = 'INBOX' unless $opt_mailbox;

my $username = $ENV{'ATHENA_USER'} || $ENV{'USER'} || getlogin || (getpwuid($<))[0] ||
    errorout "Cannot determine user name";

unless ($opt_host) {
    $opt_host = (split(" ", `hesinfo $username pobox`))[1] ||
        errorout "Cannot find Post Office server for $username";
}
errorout "Exchange accounts are not supported yet. Try http://owa.mit.edu/." if $opt_host =~ /EXCHANGE/;

# Connect to the IMAP server, and authenticate.
my $client = Cyrus::IMAP->new($opt_host) ||
    errorout "Cannot connect to IMAP server on $opt_host";
unless ($client->authenticate(-authz => $username)) {
    close_connection();
    errorout "Cannot authenticate to $opt_host";
}

# Access the mailbox.  Make sure we have read-write access.
send_command "SELECT \"$opt_mailbox\"";

# Close the mailbox, thereby expunging deleted messages.
send_command "CLOSE";

# We are done talking to the IMAP server, close down the connection.
close_connection();

# Subroutine to send a command to the IMAP server, and wait for the
# response; any defined callbacks for the response are invoked.
# If the server response indicates failure, we error out.
sub send_command($) {
    print "Sending: $_[0]\n" if $opt_debug;
    my ($status, $text) = $client->send('', '', $_[0]);
    print "Response: status $status, text $text\n" if $opt_debug;
    errorout "Premature end-of-file on IMAP connection to $opt_host"
	if $status eq 'EOF';
    if ($status ne 'OK') {
	close_connection();
	errorout "IMAP error for $opt_mailbox on $opt_host: $text" 
    }
}

# Logout from the IMAP server, and close the connection.
sub close_connection() {
    $client->send('', '', "LOGOUT");
    # Set the client reference to undef, so that perl invokes the
    # destructor, which closes the connection.  Note that if we invoke
    # the destructor explicitly here, then perl will still invoke it
    # again when the program exits, thus touching memory which has
    # already been freed.
    $client = undef;
}

sub errorout($) {
    print STDERR "$prog: $_[0]\n";
    exit 1;
}
