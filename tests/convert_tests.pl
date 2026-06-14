#!/usr/bin/env perl
# -*-perl-*-
#
# One-time converter: converts Perl test scripts to .test format.
# This uses the EXISTING Perl infrastructure to bootstrap the conversion.
# After running this once, all Perl can be deleted.
#
# Usage: cd tests && perl convert_tests.pl

use strict;
use warnings;
use File::Path qw(make_path);
use File::Spec;

my $script_dir = 'scripts';
my $output_dir = 'scripts';
my $total = 0;
my $converted = 0;
my $failed = 0;

# Mock variables that test scripts need
our $makefile = 'Makefile';
our $pathsep = '/';
our $test_passed = 1;
our $port_type = 'UNIX';
our $perl_name = '#PERL#';
our $mkpath = '#MAKEPATH#';
our $make_name = '#MAKE#';
our $helptool = '#HELPER#';
our $CMD_rmfile = 'rm -f';
our $valgrind = 0;
our $origENV = { SHELL => '/bin/sh' };
our $num_tmpfiles = 0;
our $num_logfiles = 0;

# Mock test environment that scripts depend on
our %FEATURES = (
    'archives' => 1,
    'jobserver' => 1,
    'load' => 1,
    'grouped-target' => 1,
    'one-shell' => 1,
    'output-sync' => 1,
    'second-expansion' => 1,
    'shortest-stem' => 1,
    'order-only' => 1,
    'target-specific' => 1,
    'notintermediate' => 1,
    'check-symlink' => 1,
);
our %DEFVARS = (
    AR => 'ar',
    CC => 'cc',
);
our %CONFIG_FLAGS = (
    AR => 'ar',
    CC => 'cc',
);
our $osname = 'linux';
our $vos = 0;
our $parallel_jobs = 1;  # Assume parallel jobs supported

# Error strings that test scripts may reference
our $ERR_no_such_file = 'No such file or directory';
our $ERR_no_such_file_code = '2';
our $ERR_read_only_file = 'Permission denied';
our $ERR_unreadable_file = 'Permission denied';
our $ERR_nonexe_file = 'Permission denied';
our $ERR_exe_dir = 'Permission denied';
our $ERR_command_not_found = 'not found';

sub get_config {
    my $key = shift;
    return $CONFIG_FLAGS{$key} if exists $CONFIG_FLAGS{$key};
    return '';
}

# Capture array
our @cases = ();
our $description = '';
our $details = '';
our $_current_makefile_content = '';
our $_pending_old_style = 0;
our $_old_makefile = '';
our $_old_options = '';
our $_old_exit_code = 0;
our @_setup_files = ();   # Files to create before running cases
our @_setup_dirs = ();    # Directories to create before running cases

sub run_make_test {
    my ($mk, $opts, $ans, $code) = @_;
    my $case = {};

    if (defined($mk) && $mk ne '' && $mk ne 'undef') {
        $case->{makefile} = $mk;
    }

    if (ref($opts) eq 'ARRAY') {
        $case->{options} = join(' ', @$opts);
    } else {
        $case->{options} = defined($opts) ? $opts : '';
    }

    $case->{answer} = defined($ans) ? $ans : '';
    $case->{exit_code} = defined($code) ? int($code) : 0;

    push @cases, $case;
}

# Old-style functions - capture as best we can
sub run_make_with_options {
    # Store arguments for subsequent compare_output
    my ($filename, $options, $logname, $expected_code, $timeout, @call) = @_;
    $main::_pending_old_style = 1;
    $main::_old_makefile = $filename || $makefile;
    if (ref($options) eq 'ARRAY') {
        $main::_old_options = join(' ', @$options);
    } else {
        $main::_old_options = $options || '';
    }
    $main::_old_exit_code = defined($expected_code) ? $expected_code : 0;
    return 1;
}

sub compare_output {
    my ($answer, $logfile) = @_;
    if ($main::_pending_old_style) {
        my $case = {};
        # Get the current makefile content
        my $mk_content = $main::_current_makefile_content || '';
        # If not captured via create_file, try reading the makefile from disk
        if (!$mk_content && $main::_old_makefile && -f $main::_old_makefile) {
            open(my $f, '<', $main::_old_makefile);
            $mk_content = do { local $/; <$f> } if $f;
            close($f);
        }
        if ($mk_content && $mk_content =~ /\S/) {
            $case->{makefile} = $mk_content;
        }
        if (defined($main::_old_options) && $main::_old_options ne '') {
            $case->{options} = $main::_old_options;
        }
        $case->{answer} = defined($answer) ? $answer : '';
        $case->{exit_code} = $main::_old_exit_code;
        push @cases, $case;
        $main::_pending_old_style = 0;
        $main::_current_makefile_content = '';
    }
    return 1;
}
sub get_logfile { return 't001.log'; }
sub get_tmpfile { return 't001.mk'; }

# Track file/directory operations for setup directives
sub touch {
    push @_setup_files, @_;
    return @_;
}
sub utouch {
    my $off = shift;
    push @_setup_files, @_;
    return @_;
}
sub mkdir {
    push @_setup_dirs, $_[0];
    return 1;
}
sub unlink { return 1; }
sub rmfiles { return 1; }
sub create_file {
    my ($filename, @lines) = @_;
    # Capture as makefile content
    $main::_current_makefile_content = join('', @lines);
    return 1;
}
sub subst_make_string { return $_[0]; }

# Walk through test scripts
opendir(my $dh, $script_dir) or die "Cannot open $script_dir: $!";
my @dirs = sort grep { !/^\./ && -d "$script_dir/$_" } readdir($dh);
closedir($dh);

foreach my $dir (@dirs) {
    opendir(my $sdh, "$script_dir/$dir") or next;
    my @files = sort grep {
        !/^\./ && -f "$script_dir/$dir/$_" &&
        $_ !~ /\.test$/ && $_ !~ /~$/ &&
        $_ ne 'test_template'
    } readdir($sdh);
    closedir($sdh);

    foreach my $file (@files) {
        my $path = "$script_dir/$dir/$file";
        $total++;

        # Ensure output directory exists
        make_path("$output_dir/$dir");

        # Reset state
        @cases = ();
        $description = '';
        $details = '';
        $num_tmpfiles = 0;
        $num_logfiles = 0;
        $makefile = 'Makefile';
        $_current_makefile_content = '';
        $_pending_old_style = 0;
        $_old_makefile = '';
        $_old_options = '';
        $_old_exit_code = 0;
        @_setup_files = ();
        @_setup_dirs = ();

        # Evaluate the test script via 'do'
        {
            local $SIG{__WARN__} = sub {};  # Silence warnings

            eval {
                my $result = do "./$path";
                # return -1 means test skipped; still capture whatever was recorded
                # (some tests push cases then return -1 on unsupported features)
            };
            # Also capture from main:: namespace
            if (defined($main::description)) {
                $description = $main::description;
            }
            if (defined($main::details)) {
                $details = $main::details;
            }
        }

        # If no cases captured via run_make_test, try regex extraction
        if (@cases == 0) {
            extract_cases_regex($path);
        }

        # Write output
        my $outpath = "$output_dir/$dir/$file.test";
        open(my $out, '>', $outpath) or do {
            warn "Cannot write $outpath: $!\n";
            $failed++;
            next;
        };

        # Clean up description/details
        $description =~ s/^\s+|\s+$//g;
        $details =~ s/^\s+|\s+$//g;
        $description =~ s/\s+/ /g;
        $details =~ s/\s+/ /g;

        if ($description) {
            $description =~ s/"/\\"/g;
            print $out "# Converted from $file\n";
            print $out "description: \"$description\"\n";
        }
        if ($details) {
            $details =~ s/"/\\"/g;
            print $out "details: \"$details\"\n";
        }

        if (@cases == 0) {
            print $out "\n[case]\n# TODO: Manual conversion needed for $file\n";
            print $out "# Original: $path\n";
        }

        # Write setup directive if files/dirs need to be created
        if (@_setup_files || @_setup_dirs) {
            print $out "\n[setup]\n";
            if (@_setup_files) {
                my %seen;
                my @uniq = grep { !$seen{$_}++ } @_setup_files;
                print $out "touch: " . join(' ', @uniq) . "\n";
            }
            if (@_setup_dirs) {
                my %seen;
                my @uniq = grep { !$seen{$_}++ } @_setup_dirs;
                print $out "mkdir: " . join(' ', @uniq) . "\n";
            }
        }

        # Write cases
        foreach my $case (@cases) {
            print $out "\n[case]\n";

            if (exists $case->{makefile}) {
                my $mk = $case->{makefile};
                # Clean quoting artifacts
                $mk =~ s/\n$//;
                print $out "makefile:\n$mk\n" if $mk =~ /\S/;
            }

            if (exists $case->{options} && $case->{options} ne '') {
                my $opts = $case->{options};
                $opts =~ s/^\s+|\s+$//g;
                print $out "options: $opts\n" if $opts ne '';
            }

            if (exists $case->{answer}) {
                my $ans = $case->{answer};
                $ans =~ s/^\s+|\s+$//g;
                if ($ans ne '') {
                    $ans =~ s/"/\\"/g;
                    # Don't double-escape newlines from token strings
                    print $out "answer: \"$ans\"\n";
                }
            }

            if (exists $case->{exit_code} && $case->{exit_code} != 0) {
                print $out "exit_code: $case->{exit_code}\n";
            }
        }

        close($out);
        $converted++;
    }
}

print "Converted $converted / $total test scripts.\n";
if ($failed) {
    print "Failed: $failed\n";
}

# ---- Regex-based fallback extraction ----

sub extract_cases_regex {
    my ($path) = @_;
    open(my $src, '<', $path) or return;
    my $content = do { local $/; <$src> };
    close($src);

    # Extract description and details
    if ($content =~ /\$description\s*=\s*(.*?);/s) {
        $description = clean_perl_string($1);
    }
    if ($content =~ /\$details\s*=\s*(.*?);/s) {
        $details = clean_perl_string($1);
    }

    # Try run_make_test extraction first
    my $found = extract_run_make_test($content);
    
    # If nothing found, try old-style (run_make_with_options + compare_output)
    if (!$found) {
        extract_old_style($content);
    }
}

sub extract_run_make_test {
    my ($content) = @_;
    my $found = 0;
    my $pos = 0;
    
    while (1) {
        my $start = index($content, 'run_make_test', $pos);
        last if $start < 0;
        $start = index($content, '(', $start);
        last if $start < 0;

        my $end = find_matching_paren($content, $start);
        last unless defined($end);

        # Find the closing );
        my $after = substr($content, $end + 1);
        if ($after =~ /^\s*\)\s*;/) {
            $end = $end + 1 + length($&) - 1;
        }

        my $args_str = substr($content, $start + 1, $end - $start - 1);
        my @args = split_args($args_str);
        
        if (@args >= 3) {
            my $case = {};
            my $mk = $args[0];
            if (defined($mk) && $mk !~ /^\s*undef\s*$/ && $mk !~ /^\s*$/) {
                $mk = clean_perl_string($mk);
                $case->{makefile} = $mk if $mk =~ /\S/;
            }
            $case->{options} = clean_perl_string($args[1]);
            $case->{answer} = clean_perl_string($args[2]);
            $case->{exit_code} = $args[3] ? int(clean_perl_string($args[3])) : 0;
            push @cases, $case;
            $found++;
        }
        $pos = $end + 1;
    }
    return $found;
}

sub extract_old_style {
    my ($content) = @_;
    
    # Pattern: open(MAKEFILE,...) print MAKEFILE "..." close(MAKEFILE)
    # followed by: run_make_with_options(...) $answer = "..." compare_output(...)
    
    # Extract makefile content from print MAKEFILE statements
    my $mk_content = '';
    while ($content =~ /print\s+MAKEFILE\s+(.*?);/gs) {
        my $part = clean_perl_string($1);
        $mk_content .= $part;
    }
    
    # Extract run_make_with_options calls and following answer/compare_output
    my $pos = 0;
    while ($content =~ /run_make_with_options\s*\((.*?)\)\s*;/gs) {
        my $args_str = $1;
        my @args = split_args($args_str);
        
        # Get the position after this call
        my $call_end = pos($content);
        
        # Look for $answer = "..." after this call
        my $rest = substr($content, $call_end);
        my $answer = '';
        my $exit_code = 0;
        
        if ($rest =~ /\$answer\s*=\s*(.*?);/s) {
            $answer = clean_perl_string($1);
        }
        if ($args_str =~ /,\s*(\d+)\s*\)/) {
            $exit_code = $1;
        }
        
        my $case = {};
        if ($mk_content && $mk_content =~ /\S/) {
            $case->{makefile} = $mk_content;
        }
        if (@args >= 2) {
            $case->{options} = clean_perl_string($args[1]);
        }
        $case->{answer} = $answer;
        $case->{exit_code} = $exit_code;
        push @cases, $case;
    }
}

sub clean_perl_string {
    my ($s) = @_;
    return '' unless defined($s);
    $s =~ s/^\s+|\s+$//g;

    # Handle q!...! quoting
    if ($s =~ /^q!/) {
        $s =~ s/^q!//;
        $s =~ s/!\s*$//;
        return $s;
    }

    # Handle single quotes
    if ($s =~ /^'(.*)'$/s) {
        $s = $1;
        $s =~ s/\\'/'/g;
        return $s;
    }

    # Handle double quotes
    if ($s =~ /^"(.*)"$/s) {
        $s = $1;
        $s =~ s/\\"/"/g;
        $s =~ s/\\n/\n/g;
        $s =~ s/\\t/\t/g;
        return $s;
    }

    return $s;
}

sub find_matching_paren {
    my ($str, $start) = @_;
    my $depth = 0;
    for (my $i = $start; $i < length($str); $i++) {
        my $c = substr($str, $i, 1);
        if ($c eq '(') { $depth++; }
        elsif ($c eq ')') {
            $depth--;
            return $i if $depth == 0;
        }
    }
    return undef;
}

sub split_args {
    my ($str) = @_;
    my @args;
    my $depth = 0;
    my $current = '';
    my $in_single = 0;
    my $in_double = 0;
    my $in_q = 0;
    my $i = 0;

    while ($i < length($str)) {
        my $c = substr($str, $i, 1);

        if ($in_q) {
            if ($c eq '!') {
                $in_q = 0;
            } else {
                $current .= $c;
            }
            $i++;
            next;
        }

        if ($c eq 'q' && $i + 1 < length($str) && substr($str, $i+1, 1) eq '!') {
            $in_q = 1;
            $i += 2;
            next;
        }

        if ($in_single) {
            if ($c eq "'" && substr($str, $i-1, 1) ne '\\') {
                $in_single = 0;
            }
            $current .= $c;
            $i++;
            next;
        }

        if ($in_double) {
            if ($c eq '"' && substr($str, $i-1, 1) ne '\\') {
                $in_double = 0;
            }
            $current .= $c;
            if ($c eq '\\' && $i + 1 < length($str)) {
                $current .= substr($str, $i+1, 1);
                $i++;
            }
            $i++;
            next;
        }

        if ($c eq "'") {
            $in_single = 1;
            $current .= $c;
            $i++;
            next;
        }

        if ($c eq '"') {
            $in_double = 1;
            $current .= $c;
            $i++;
            next;
        }

        if ($c eq '(' || $c eq '[' || $c eq '{') {
            $depth++;
            $current .= $c;
        } elsif ($c eq ')' || $c eq ']' || $c eq '}') {
            $depth--;
            if ($depth < 0) {
                last;  # End of outer call
            }
            $current .= $c;
        } elsif ($c eq ',' && $depth == 0) {
            push @args, $current;
            $current = '';
        } else {
            $current .= $c;
        }
        $i++;
    }

    push @args, $current if $current =~ /\S/;
    return @args;
}
