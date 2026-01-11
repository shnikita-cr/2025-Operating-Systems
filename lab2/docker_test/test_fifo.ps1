docker run -d --name wolf_fifo --rm `
  --mount type=bind,source="${PWD}\logs",target=/etc/logs `
  os-lab2 /app/run/host_fifo  --rounds 20 --timeout-ms 5000 --log-dir /etc/logs --nick wolf

$HOSTPID = (docker exec wolf_fifo cat /app/run/host.pid).Trim()

1..7 | ForEach-Object {
  docker exec -d wolf_fifo /app/run/client_fifo --host-pid $HOSTPID --nick "kid$_" --timeout-ms 5000 --log-dir /etc/logs
}
