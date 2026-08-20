# The container tier of E7.
#
# Runtime dependencies only. The binary is bind-mounted from the host at run
# time rather than built here, so the container measures exactly the build the
# host measured; compiling inside the image would add a second compiler
# invocation as a confounder in an experiment whose whole subject is a
# difference of a few hundred cycles.
FROM ubuntu:24.04

RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends \
      libssl3t64 liburing2 libgomp1 ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Recorded so a process inside can tell it is containerised even where the
# cgroup heuristics are ambiguous.
ENV LR_ISOLATION_TIER=container

WORKDIR /work
ENTRYPOINT []
