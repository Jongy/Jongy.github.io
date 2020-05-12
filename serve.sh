#!/bin/bash
set -e

bundler install
bundler exec jekyll serve -H 0.0.0.0
# you have the site on http://127.0.0.1:4000 outside.
