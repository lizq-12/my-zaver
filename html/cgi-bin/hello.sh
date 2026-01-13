#!/bin/sh

# Minimal CGI script for CI/functional tests.
# Emits CGI headers then body.

printf "Content-Type: text/plain\r\n\r\n"
printf "hello cgi\n"
