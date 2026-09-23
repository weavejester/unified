#!/usr/bin/env bash

server_player_count() {
    /usr/bin/mysql -h "$NWNX_SQL_HOST" -D "$NWNX_SQL_DATABASE" \
                   -u "$NWNX_SQL_USERNAME" --password="$NWNX_SQL_PASSWORD" \
                   -e 'select count(*) from online_players' -N -s
}

stop_server_immediately() {
    kill -TERM $child
}

stop_server_nicely() {
    # Shutdown from within server
    echo 1 > /nwn/home/shutdown.txt

    # Wait 90 seconds before sending a SIGTERM
    (sleep 90; stop_server_immediately) &
}

stop_server() {
    echo Shutting down Risenholm server...

    if [ $(server_player_count) = "0" ]; then
        echo Zero players online, so shutting down immediately
        stop_server_immediately
    else
        echo Players online, so giving 60s grace period
        stop_server_nicely
    fi

    wait $child
}

trap stop_server SIGTERM

# Link NWNX directory to home
mkdir -p /nwn/home/nwnx
rmdir /nwn/run/nwnx
ln -s /nwn/home/nwnx /nwn/run/nwnx

echo Starting Risenholm server...

# Wait up to 5m for database to be ready before starting server
/nwn/wait-for-it.sh -h "$NWNX_SQL_HOST" -p 3306 -t 300 -s -- /nwn/run-server.sh &

child=$!
wait $child
