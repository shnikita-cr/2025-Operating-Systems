docker run -d --name wolf_mq --rm `
  --mount type=bind,source="${PWD}\logs",target=/etc/logs `
  os-lab2 /app/run/host_mq --rounds 20 --timeout-ms 5000 --log-dir /etc/logs --nick wolf

$HOSTPID = (docker exec wolf_mq cat /app/run/host.pid).Trim()

1..7 | ForEach-Object {
  docker exec -d wolf_mq /app/run/client_mq --host-pid $HOSTPID --nick "kid$_" --timeout-ms 5000 --log-dir /etc/logs
}
