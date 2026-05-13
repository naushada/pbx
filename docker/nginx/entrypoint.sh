#!/bin/sh
#
# Renders /etc/nginx/templates/default.conf.template into
# /etc/nginx/conf.d/default.conf, substituting $PORT and $BACKEND_ORIGIN
# at container start. Runs from /docker-entrypoint.d so nginx:alpine's
# stock entrypoint picks it up before `nginx -g 'daemon off;'`.
#
# Why we ship this instead of relying on the image's built-in
# NGINX_ENVSUBST_TEMPLATE_*: those vars require nginx ≥ 1.19 *and* the
# variables you reference must be in NGINX_ENVSUBST_OUTPUT_DIR's
# allow-list. Shipping our own rendering step avoids that contract
# entirely and keeps the substitution surface explicit.

set -e

: "${PORT:?PORT must be set}"
: "${BACKEND_ORIGIN:?BACKEND_ORIGIN must be set}"

envsubst '${PORT} ${BACKEND_ORIGIN}' \
    < /etc/nginx/templates/default.conf.template \
    > /etc/nginx/conf.d/default.conf

# Drop the default 80-listening server that ships with nginx:alpine
# so it doesn't fight our $PORT listener on Heroku.
rm -f /etc/nginx/conf.d/default-original.conf 2>/dev/null || true
