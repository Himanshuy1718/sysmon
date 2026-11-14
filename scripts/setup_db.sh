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

# Ubuntu picks 5433 (or higher) if port 5432 was already taken when PostgreSQL
# was installed, e.g. by a PostgreSQL on Windows that WSL can see. Ask the
# local server which port it really uses.
DB_PORT="$(sudo -u postgres psql -tAc "SHOW port" | tr -d '[:space:]')"
if [ "${DB_PORT}" != "5432" ]; then
  echo "Note: this PostgreSQL listens on port ${DB_PORT} (5432 is used by something else)."
fi

sudo -u postgres psql -v ON_ERROR_STOP=1 -q <<SQL
DO \$\$
BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '${DB_USER}') THEN
    CREATE ROLE ${DB_USER} LOGIN PASSWORD '${DB_PASS}';
  ELSE
    -- User already exists (e.g. from an earlier attempt): make sure the
    -- password and login permission are what we expect.
    ALTER ROLE ${DB_USER} WITH LOGIN PASSWORD '${DB_PASS}';
  END IF;
END
\$\$;
SQL

if ! sudo -u postgres psql -tAc "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}'" | grep -q 1; then
  sudo -u postgres createdb -O "${DB_USER}" "${DB_NAME}"
fi

# Verify that logging in with the password actually works.
if PGPASSWORD="${DB_PASS}" psql -h localhost -p "${DB_PORT}" -U "${DB_USER}" -d "${DB_NAME}" -tAc "SELECT 1" >/dev/null 2>&1; then
  echo "Login check: OK"
else
  echo "Login check FAILED: could not log in as ${DB_USER} with the password." >&2
  exit 1
fi

echo "Ready. Connection string:"
echo "  postgresql://${DB_USER}:${DB_PASS}@localhost:${DB_PORT}/${DB_NAME}"
