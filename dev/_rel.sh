#!/bin/bash
cd ..
# stdout menjen csak a fájlba
exec 1> ./dev/logs/rel_stdout.txt

# stderr menjen egyszerre terminálra ÉS fájlba
exec 2> >(tee ./dev/logs/log_output_rel.txt >&2)

./build/cmd/examples/qserver -k dev/server-key.pem -c dev/server-cert.pem -t --bind_ip localhost
