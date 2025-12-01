#!/bin/bash
cd ..

exec > >(tee ./dev/logs/rel_out.txt) 2>&1

./build/cmd/examples/qserver -t -k server-key.pem -c server-cert.pem -t --bind_ip localhost
