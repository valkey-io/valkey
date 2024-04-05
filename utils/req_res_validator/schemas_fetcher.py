import json
import subprocess
import tempfile
import time
import typing

import redis


class SchemasFetcher:
    def __init__(self, cli_exec: str, server_exec: str, port: int):
        self._cli_exec = cli_exec
        self._server_exec = server_exec
        self._port = port

    def fetch_from_server(self, modules: typing.Sequence[str]) -> typing.Dict[str, typing.Any]:
        """Fetch schemas from a Valkey instance."""
        print('Starting Valkey server')
        redis_args = [self._server_exec, '--port', str(self._port)]
        for module in modules:
            redis_args += ['--loadmodule', f'tests/modules/{module}.so']

        return self._fetch_schemas(args=redis_args)

    def fetch_from_sentinel(self):
        """Fetch schemas from a sentinel."""
        print('Starting Valkey sentinel')
        # Sentinel needs a config file to start
        with tempfile.NamedTemporaryFile(prefix="tmpsentinel", suffix=".conf") as conf_file:
            sentinel_args = [self._server_exec, conf_file.name, '--port', str(self._port), "--sentinel"]
            return self._fetch_schemas(args=sentinel_args)

    def _fetch_schemas(self, args: typing.Sequence[str]) -> typing.Dict[str, typing.Any]:
        docs = {}

        with subprocess.Popen(args, stdout=subprocess.PIPE) as redis_proc:
            self._wait_for_server_up()
            docs_response = self._execute_command_docs()
            redis_proc.terminate()

        for name, doc in docs_response.items():
            if "subcommands" in doc:
                for sub_name, sub_doc in doc["subcommands"].items():
                    docs[sub_name] = sub_doc
            else:
                docs[name] = doc

        return docs

    def _wait_for_server_up(self) -> None:
        while True:
            try:
                print('Connecting to Valkey...')
                r = redis.Redis(port=self._port)
                r.ping()
                break
            except Exception as e:
                time.sleep(0.1)

        print('Connected')

    def _execute_command_docs(self) -> typing.Dict[str, typing.Any]:
        cli_proc = subprocess.Popen(
            args=[self._cli_exec, '-p', str(self._port), '--json', 'command', 'docs'],
            stdout=subprocess.PIPE,
        )
        stdout, stderr = cli_proc.communicate()
        return json.loads(stdout)
