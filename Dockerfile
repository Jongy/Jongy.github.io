FROM ubuntu:20.04

# ruby-dev has "gem"
# various build tools required for later compilations done by bundler.
RUN apt-get update -y && apt-get install -y ruby ruby-dev gcc g++ make zlib1g-dev patch git

RUN gem install bundler

# bundler install has to be done after starting the docker - when the project
# directory is mounted.

EXPOSE 4000/tcp

CMD /bin/bash
