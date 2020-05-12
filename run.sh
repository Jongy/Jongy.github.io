#!/bin/bash
set -e

docker build -t github-pages .

docker run -it --name=github-pages -v $(pwd):/site -p 4000:4000 github-pages

# inside, run serve.sh
