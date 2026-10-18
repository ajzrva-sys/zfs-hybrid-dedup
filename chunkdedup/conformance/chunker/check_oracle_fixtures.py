#!/usr/bin/env python3
"""Compare all delivered fixtures to the unmodified Microsoft C# oracle."""
import os,pathlib,subprocess
root=pathlib.Path(__file__).resolve().parent
ref=root.parent/'reference'
env=os.environ.copy()
env.update(DOTNET_CLI_TELEMETRY_OPTOUT='1',DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1',
 DOTNET_GENERATE_ASPNET_CERTIFICATE='false',DOTNET_CLI_HOME=str(ref/'.dotnet-cli-home'),
 DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true')
oracle=[os.environ.get('DOTNET_COMMAND','dotnet'),str(ref/'oracle/bin/Oracle.dll')]
paths=sorted((root/'fixtures').glob('*.bin'))
assert len(paths)==35
for path in paths:
 result=subprocess.check_output(oracle+[str(path),'262144'],env=env)
 assert result==path.with_suffix('.tsv').read_bytes(),path.name
print('35 original-C# oracle fixture rechecks PASS')
