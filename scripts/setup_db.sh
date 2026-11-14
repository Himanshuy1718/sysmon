#!/usr/bin/env bash
# Creates a dedicated PostgreSQL user and database for SysMon.
# Safe to run more than once. Does not touch any other database.
#
# Usage: ./scripts/setup_db.sh            (user/db/password all "sysmon")
set -euo pipefail

DB_USER="${DB_USER:-sysmon}"
DB_NAME="${DB_NAME:-sysmon}"
DB_PASS="${DB_PASS:-sysmon}"

# Start the server if it is not running (WSL may not start services automatically).
if ! pg_isready -q; then
  echo "Starting PostgreSQL..."
  sudo service postgresql start
  sleep 2
fi

sudo -u postgres psql -v ON_ERROR_STOP=1 -q <<SQL
DO \$\$
BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '${DB_USER}') THEN
    CREATE ROLE ${DB_USER} LOGIN PASSWORD '${DB_PASS}';
  END IF;
END
\$\$;
SQL

if ! sudo -u postgres psql -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'" | grep -q 1; then
  sudo -u postgres createdb -O "${DB_USER}" "${DB_NAME}"
fi

echo "Ready. Connection string:"
echo "  postgresql://${DB_USER}:${DB_PASS}@localhost:5432/${DB_NAME}"
