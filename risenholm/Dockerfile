# Risenholm's server image: the unified image plus our startup scripts. Built
# and pushed as weavejester/nwnxee by the nwnxee job in .github/workflows/docker.yml.
# UNIFIED_TAG is the commit SHA of the unified image that job has just pushed.
ARG UNIFIED_TAG=latest
FROM weavejester/unified:${UNIFIED_TAG}
LABEL maintainer="James Reeves <jreeves@weavejester.com>"

COPY wait-for-it.sh /nwn
COPY run-pw-server.sh /nwn

ENTRYPOINT ["/nwn/run-pw-server.sh"]
