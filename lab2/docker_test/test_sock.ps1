docker run -d --name wolf_sock --rm -p 5555:5555 `
  --mount type=bind,source="${PWD}\logs",target=/etc/logs `
  os-lab2 /app/run/host_sock --rounds 20 --timeout-ms 5000 --log-dir /etc/logs --nick wolf

$HOSTPID = (docker exec wolf_sock cat /app/run/host.pid).Trim()

1..7 | ForEach-Object {
  docker exec -d wolf_sock /app/run/client_sock --host-pid $HOSTPID  --nick "kid$_" --timeout-ms 5000 --log-dir /etc/logs
}
