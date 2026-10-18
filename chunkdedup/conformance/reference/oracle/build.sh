#!/bin/sh
set -eu
cd "$(dirname "$0")"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_GENERATE_ASPNET_CERTIFICATE=false
export DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=true
export DOTNET_CLI_HOME="$PWD/../.dotnet-cli-home"
export DOTNET_NOLOGO=1
exec "${DOTNET_COMMAND:-dotnet}" build Oracle.csproj -c Release -o bin --ignore-failed-sources
