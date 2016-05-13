#!/usr/bin/env python

from yt.common import set_pdeathsig
import yt.logger as logger
import yt.wrapper as yt

import os
import sys
import time
import signal
import argparse
import subprocess

def main():
    parser = argparse.ArgumentParser(description='Run command under lock')
    parser.add_argument('path')
    parser.add_argument('command')
    parser.add_argument('--step', type=float, default=1.0)
    parser.add_argument('--conflict-exit-code', type=int, default=1)
    args = parser.parse_args()

    with yt.Transaction(attributes={"title": "yt_lock transaction"}) as tx:
        try:
            yt.lock(args.path)
        except yt.YtResponseError as error:
            if error.is_concurrent_transaction_lock_conflict():
                logger.info("Lock conflict (path %s)", args.path)
                sys.exit(args.conflict_exit_code)
            raise

        def handler():
            tx.__exit__(None, None, None)
            sys.exit(1)

        signal.signal(signal.SIGTERM, lambda signum, frame: handler())

        logger.info("Running command %s", args.command)
        proc = subprocess.Popen(args.command, stdout=sys.stdout, stderr=sys.stderr, shell=True, env=os.environ.copy(),
                                preexec_fn=set_pdeathsig)

        while True:
            if not tx._ping_thread.is_alive():
                logger.error("Pinging thread failed. Terminating command.")
                proc.send_signal(2)
                time.sleep(args.step)
                proc.terminate()
                time.sleep(args.step)
                proc.kill()
                break

            if proc.poll() is not None:
                break

            time.sleep(args.step)

    sys.exit(proc.returncode)

if __name__ == "__main__":
    main()

